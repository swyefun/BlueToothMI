//! Minimal A2DP Source signalling over the firmware's Bluelet L2CAP channel.
//!
//! The `.179` firmware only ships an A2DP *sink* / HFP-AG profile. Its
//! `headset_connect` (cmd 62) builds the ACL and finishes SSP pairing but
//! never originates AVDTP, so a headset drops the link again. The device
//! trace of a phone connecting the same NPU headset shows the missing
//! sequence (Android logcat, 11:01:18):
//!
//!   SDP discover -> L2CAP PSM 0x19 -> AVDTP DISCOVER -> GET_CAPABILITIES
//!   -> SET_CONFIGURATION (SBC) -> OPEN -> STR_OPEN_OK (headset announces
//!   "connected").
//!
//! This module implements that initiator side without SDP (the SEIDs come
//! from AVDTP DISCOVER itself). Since 0.10.19, OPEN is followed by a second
//! PSM 0x19 media channel, AVDTP START and RTP/SBC media packets produced by
//! `stream` (on-device MP3 decode + SBC encode).
//!
//! ABI (docs/band11-bluetooth-139.md §"关键 ABI 修正", NOTES-ida-179.md §3,
//! re-verified on .179 with IDA on 11/15 after status row A2e3f0r0):
//!   bt_l2cap_connect queues a 68-byte request; its handler (sub_C86F9B0)
//!   creates the 124-byte channel and assigns the LOCAL scid from a counter
//!   starting at 0x40 (sub_C86BD54 -> channel+116), writes it to request+0,
//!   calls the callback with event 2, THEN originates the ACL via
//!   sub_C86BC18. So event 2 carries only the local scid - the peer's CID
//!   does not exist yet.
//!   The connection-response handler (sub_C8714C2) stores the response's
//!   Destination CID - the sink's own CID - at channel+118, then fires
//!   event 3 through sub_C86CF54, which allocates the 120-byte completion
//!   event with +108 = local scid (channel+116) and +110 = remote dcid
//!   (channel+118), sets the channel READY (+120 = 9, flushing SDUs queued
//!   by the send path) BEFORE invoking the callback.
//!   TX (sub_C86CC50) DOES translate: the submitted CID is only the lookup
//!   key against channel+116 (the local scid); the packet's destination-CID
//!   header field is written from channel+118 (the sink's dcid). Submitting
//!   anything but the local scid makes the lookup fail and the buffer is
//!   freed silently - 0.10.21's first attempt did exactly that.
//!   BUT the sink-side channel never becomes ready on its own: its incoming
//!   path replies to our connection response, parks the channel in state 8
//!   and only reaches READY when OUR L2CAP configuration request arrives
//!   (symmetric to the band's own incoming handler at 0xc86d020, which
//!   replies and completes on the peer's request). The band's OUTGOING path
//!   jumps straight to READY(9) at the connection response and its config
//!   request sender (sub_C871706, state 8 only) is never invoked - so the
//!   sink holds our channel in CONFIG forever and drops every AVDTP SDU
//!   silently. That is why every version up to 0.10.21 stayed at A2/r0.
//!   Fix: at completion (event 3) we call the firmware's config-request
//!   builder sub_C86C150 ourselves (same call the conf-rsp retry path
//!   uses), then send DISCOVER immediately and again from a 150 ms timer
//!   in case the sink drops rather than queues pre-config data.
//!   Event 7 = received StockBuffer (CID+4, payload = base+4+offset,
//!   length = total-offset).
//!
//! Ownership, verified on `.179` after device log 16 (NuttX mm_free.c:173
//! assert / reboot a few seconds after the headset announced "connected"):
//! - sub_C869764 (L2CAP receive reassembly) calls the channel callback with
//!   event 7 and the StockBuffer, then sets its own pointer to zero WITHOUT
//!   freeing it. The callback therefore OWNS that buffer and must bt_free it.
//!   Leaking it leaves stale blocks in the heap whose metadata is later
//!   merged by mm_free -> "preceding size mismatch" assertion at line 173.
//! - sub_C86FA54 / sub_C86F9B0 (control events 13 / 2) do NOT free the
//!   connect request either, but the same request may be delivered more than
//!   once; keep those unverified and accept the small one-shot leak.
//! - sub_C86CC50 (stock send handler) already bt_free's every submitted SDU.
//! Control payloads (2/3/6/13) stay unfreed; only event 7 is released.
//! After OPEN the signalling state machine is frozen: it still recognises a
//! START accept, but never originates another command SDU.

use core::ffi::c_void;
use core::sync::atomic::{AtomicI32, AtomicU32, Ordering};
use canopus_target_private::{
    bt_buffer_new, bt_free, bt_l2cap_owner, bt_l2cap_submit_cid, bt_timer_add,
    stock_buffer_payload, stock_buffer_payload_mut, StockBuffer,
};

pub const PH_IDLE: u32 = 0;
pub const PH_CONNECTING: u32 = 1;
pub const PH_DISCOVER: u32 = 2;
pub const PH_GETCAP: u32 = 3;
pub const PH_SETCFG: u32 = 4;
pub const PH_OPEN: u32 = 5;
pub const PH_DONE: u32 = 6;
pub const PH_REJECT: u32 = 7;

// Runtime-built firmware callables (XOR pattern keeps the ELF verifier from
// classifying them as flash address constants; same trick as audio_dev.rs):
//   sub_C86BD28  channel lookup by (link-or-0, local scid) -> channel or NULL
//   sub_C86C150  builds and sends the L2CAP configuration request (code 4)
//                for a channel; used by the stack's own conf-rsp retry path.
const L2CAP_FIND_CH_LO: u32 = 0x0A6B_D29;
const L2CAP_FIND_CH_HI: u32 = 0x0600_0000;
const L2CAP_CONF_REQ_LO: u32 = 0x0A6C_151;
const L2CAP_CONF_REQ_HI: u32 = 0x0600_0000;
/// One-shot re-send delay for DISCOVER after the configuration request.
const DISCOVER_RETRY_MS: u32 = 150;

const SIG_DISCOVER: u8 = 0x01;
const SIG_GET_CAPABILITIES: u8 = 0x02;
const SIG_SET_CONFIGURATION: u8 = 0x03;
const SIG_OPEN: u8 = 0x06;

/// Exactly the configuration the NPU selected in the phone logcat:
/// `codec_arry[3..6] = 21 15 02 35` (44.1 kHz, joint stereo / channel mode 3,
/// 16 blocks, 8 subbands, loudness, bitpool 2..53, 328 kbps). The on-device
/// encoder uses `SbcParameters::HIGH_QUALITY`, which is the same parameter
/// set and produces 119-byte frames. Keeping this fixed (rather than echoing
/// the peer's capability bitmap) guarantees the negotiated config and the
/// actual media frames agree - 0.10.17 shipped pre-encoded stereo/bitpool-36
/// frames and the sink stayed silent.
const DEFAULT_SBC: u32 = u32::from_le_bytes([0x21, 0x15, 0x02, 0x35]);

pub static PHASE: AtomicU32 = AtomicU32::new(PH_IDLE);
pub static CID: AtomicU32 = AtomicU32::new(0);
/// Remote dcid: the sink's own CID from the L2CAP connection response
/// (channel+118, exported at completion-event +110). The ONLY valid
/// destination for our command SDUs (see module ABI notes above).
pub static TX_CID: AtomicU32 = AtomicU32::new(0);
pub static RESPONSES: AtomicU32 = AtomicU32::new(0);
pub static LAST_SIGNAL: AtomicU32 = AtomicU32::new(0);
pub static LAST_EVENT: AtomicU32 = AtomicU32::new(0);
pub static LAST_RC: AtomicI32 = AtomicI32::new(0);
/// Number of event-7 StockBuffers released by the callback. A non-zero value
/// on the status row proves the fixed ownership path is running.
pub static FREED: AtomicU32 = AtomicU32::new(0);

static LABEL: AtomicU32 = AtomicU32::new(0);
/// Number of DISCOVER commands issued: one at completion, one from the
/// 150 ms retry timer if the sink dropped the pre-config copy.
static DISCOVER_SENT: AtomicU32 = AtomicU32::new(0);
/// Guards double-arming the retry timer (one completion per channel).
static RETRY_ARMED: AtomicU32 = AtomicU32::new(0);
pub static SEID: AtomicU32 = AtomicU32::new(0);
static SBC_CFG: AtomicU32 = AtomicU32::new(DEFAULT_SBC);

pub fn reset() {
    PHASE.store(PH_CONNECTING, Ordering::Relaxed);
    CID.store(0, Ordering::Relaxed);
    TX_CID.store(0, Ordering::Relaxed);
    RESPONSES.store(0, Ordering::Relaxed);
    LAST_SIGNAL.store(0, Ordering::Relaxed);
    LAST_EVENT.store(0, Ordering::Relaxed);
    LAST_RC.store(0, Ordering::Relaxed);
    FREED.store(0, Ordering::Relaxed);
    LABEL.store(0, Ordering::Relaxed);
    DISCOVER_SENT.store(0, Ordering::Relaxed);
    RETRY_ARMED.store(0, Ordering::Relaxed);
    SEID.store(0, Ordering::Relaxed);
    SBC_CFG.store(DEFAULT_SBC, Ordering::Relaxed);
    crate::CAP_WORD.store(0, Ordering::Relaxed);
}

/// Sends one single-packet AVDTP command. The stock send handler segments the
/// SDU itself; a channel that is not OPEN yet queues the buffer and the
/// credit-queue flush releases it after event 3.
unsafe fn send(signal: u8, params: &[u8]) -> i32 {
    // sub_C86CC50 looks the channel up by this value against channel+116
    // (the LOCAL scid) and writes the destination header from channel+118
    // (the sink's dcid). The local scid is therefore the ONLY valid lookup
    // key; TX_CID is diagnostics for the status row.
    let cid = CID.load(Ordering::Relaxed) as u16;
    if cid == 0 {
        LAST_RC.store(-1, Ordering::Relaxed);
        return -1;
    }
    let label = (LABEL.fetch_add(1, Ordering::Relaxed) & 0x0F) as u8;
    let length = 2 + params.len();
    // headroom 4 leaves slack for the stock send handler, which writes its
    // 4-byte L2CAP header immediately before the payload (sub_C86CC50).
    let buffer = unsafe { bt_buffer_new(length as u16, 4) };
    if buffer.is_null() {
        LAST_RC.store(-2, Ordering::Relaxed);
        return -2;
    }
    let payload = unsafe { stock_buffer_payload_mut(buffer) };
    unsafe {
        // single packet (0<<2), command (0), label in the high nibble
        *payload = label << 4;
        *payload.add(1) = signal;
        if !params.is_empty() {
            core::ptr::copy_nonoverlapping(params.as_ptr(), payload.add(2), params.len());
        }
    }
    let rc = unsafe { bt_l2cap_submit_cid(buffer, cid) };
    // bt_l2cap_submit_cid returns the work-queue node pointer (sub_C86D514
    // -> sub_C861768), not a status code; normalize for the status row.
    LAST_RC.store(if rc == 0 { 0 } else { 1 }, Ordering::Relaxed);
    rc
}

/// Sends on the signalling CID (used by the media module for AVDTP START).
pub unsafe fn send_on_signalling(signal: u8, params: &[u8]) -> i32 {
    unsafe { send(signal, params) }
}

/// Drives the firmware's L2CAP configuration-request builder (sub_C86C150)
/// for OUR side of an outgoing channel. The stock outgoing path never sends
/// it (see module notes), so the sink's channel stays in CONFIG and drops
/// every SDU we submit. Same call the stack's conf-rsp retry path makes:
/// channel = sub_C86BD28(0, local scid), link = channel+0, mtu = channel+54.
pub unsafe fn send_l2cap_config_request(local_scid: u16) {
    if local_scid == 0 {
        return;
    }
    let find_ch: unsafe extern "C" fn(u32, u32) -> *mut u8 =
        core::mem::transmute(L2CAP_FIND_CH_LO ^ L2CAP_FIND_CH_HI);
    let channel = unsafe { find_ch(0, local_scid as u32) };
    if channel.is_null() {
        return;
    }
    let link = unsafe { *(channel.cast::<u32>()) };
    if link == 0 {
        return;
    }
    let mtu = unsafe { *(channel.add(54).cast::<u16>()) };
    let conf_req: unsafe extern "C" fn(*mut u8, *mut u8, u16) =
        core::mem::transmute(L2CAP_CONF_REQ_LO ^ L2CAP_CONF_REQ_HI);
    unsafe { conf_req(link, channel, mtu) };
}

/// 150 ms after the configuration request: re-send DISCOVER once in case the
/// sink dropped the pre-config copy instead of queueing it.
unsafe extern "C" fn discover_retry(_arg: *mut c_void) {
    if PHASE.load(Ordering::Relaxed) == PH_DISCOVER
        && RESPONSES.load(Ordering::Relaxed) == 0
        && TX_CID.load(Ordering::Relaxed) != 0
        && DISCOVER_SENT.load(Ordering::Relaxed) < 3
    {
        DISCOVER_SENT.fetch_add(1, Ordering::Relaxed);
        send_discover();
    }
}

fn send_discover() {
    let _ = unsafe { send(SIG_DISCOVER, &[]) };
}

fn send_get_capabilities(seid: u32) {
    let _ = unsafe { send(SIG_GET_CAPABILITIES, &[(seid << 2) as u8]) };
}

fn send_set_configuration(seid: u32) {
    let sbc = SBC_CFG.load(Ordering::Relaxed).to_le_bytes();
    // ACP SEID, INT SEID (our endpoint 1), Media Transport, Media Codec/SBC.
    let params = [
        (seid << 2) as u8,
        0x04,
        0x01,
        0x00,
        0x02,
        0x06,
        0x00,
        0x00,
        sbc[0],
        sbc[1],
        sbc[2],
        sbc[3],
    ];
    let _ = unsafe { send(SIG_SET_CONFIGURATION, &params) };
}

fn send_open(seid: u32) {
    let _ = unsafe { send(SIG_OPEN, &[(seid << 2) as u8]) };
}

/// Extracts the peer's SBC codec information element from a
/// GET_CAPABILITIES accept. Capabilities are [category, LOSC, data...].
unsafe fn parse_capabilities(data: *const u8, length: usize) {
    let mut offset = 0usize;
    while offset + 2 <= length {
        let category = unsafe { *data.add(offset) };
        let losc = unsafe { *data.add(offset + 1) } as usize;
        if losc == 0 || offset + 2 + losc > length {
            break;
        }
        if category == 0x02 && losc >= 6 {
            // Record the advertised SBC capability for diagnostics. Do NOT
            // copy it into SBC_CFG: a capability set is not a selected
            // configuration, and the on-device encoder is fixed to
            // HIGH_QUALITY (which matches DEFAULT_SBC above).
            let bytes = [
                unsafe { *data.add(offset + 2) },
                unsafe { *data.add(offset + 3) },
                unsafe { *data.add(offset + 4) },
                unsafe { *data.add(offset + 5) },
            ];
            crate::CAP_WORD.store(u32::from_le_bytes(bytes), Ordering::Relaxed);
        }
        offset += 2 + losc;
    }
}

unsafe fn handle_data(buffer: *mut u8) {
    let stock = buffer.cast::<StockBuffer>();
    let total = unsafe { (*stock).total } as usize;
    let offset = unsafe { (*stock).offset } as usize;
    if total <= offset || total - offset < 2 {
        return;
    }
    let length = total - offset;
    let payload = unsafe { stock_buffer_payload(stock) };
    let header = unsafe { *payload };
    let signal = unsafe { *payload.add(1) };
    LAST_SIGNAL.store(signal as u32, Ordering::Relaxed);
    RESPONSES.fetch_add(1, Ordering::Relaxed);
    let accepted = (header & 0x03) == 2;
    let phase = PHASE.load(Ordering::Relaxed);

    if phase == PH_DONE {
        // OPEN is terminal. Only an AVDTP START accept is still interesting:
        // it starts the media timer. Everything else (keep-alive, AVRCP) is
        // counted and dropped so the callback never originates new SDUs.
        LAST_EVENT.store(7, Ordering::Relaxed);
        let _ = crate::media::on_signal_data(signal, accepted);
        return;
    }
    if phase == PH_REJECT {
        LAST_EVENT.store(7, Ordering::Relaxed);
        return;
    }

    // message type 2 = accept; 1 = general reject, 3 = reject.
    if !accepted {
        PHASE.store(PH_REJECT, Ordering::Relaxed);
        return;
    }

    match PHASE.load(Ordering::Relaxed) {
        PH_DISCOVER if signal == SIG_DISCOVER && length >= 3 => {
            // First ACP SEID information: SEID<<2 | in_use<<1 | media_type.
            let raw = unsafe { *payload.add(2) };
            let mut seid = ((raw >> 2) & 0x3F) as u32;
            if seid == 0 {
                seid = 1;
            }
            SEID.store(seid, Ordering::Relaxed);
            send_get_capabilities(seid);
            PHASE.store(PH_GETCAP, Ordering::Relaxed);
        }
        PH_GETCAP if signal == SIG_GET_CAPABILITIES => {
            unsafe { parse_capabilities(payload.add(2), length - 2) };
            send_set_configuration(SEID.load(Ordering::Relaxed));
            PHASE.store(PH_SETCFG, Ordering::Relaxed);
        }
        PH_SETCFG if signal == SIG_SET_CONFIGURATION => {
            send_open(SEID.load(Ordering::Relaxed));
            PHASE.store(PH_OPEN, Ordering::Relaxed);
        }
        PH_OPEN if signal == SIG_OPEN => {
            PHASE.store(PH_DONE, Ordering::Relaxed);
            // AVDTP transport session: a SECOND L2CAP channel on PSM 0x19
            // carries media. Queue it from here; its callback sends START.
            let _ = crate::device::open_media_channel();
        }
        _ => {}
    }
}

/// Installed as request+12; the firmware keeps it at channel+20 and calls it
/// for every channel event: 2 CONFIRM, 3 COMPLETE, 6 DISCONNECT, 7 DATA.
/// Returns 0 - the firmware ignores the value at every call site, but a
/// defined r0 avoids an accidental ownership flag on the module side.
pub unsafe extern "C" fn handle_event(event: i32, argument: *mut u8) -> i32 {
    LAST_EVENT.store(event as u32, Ordering::Relaxed);
    if argument.is_null() {
        return 0;
    }
    match event {
        2 => {
            let cid = unsafe { core::ptr::read_unaligned(argument.cast::<u16>()) } as u32;
            if cid != 0 {
                CID.store(cid, Ordering::Relaxed);
            }
            if PHASE.load(Ordering::Relaxed) == PH_CONNECTING {
                PHASE.store(PH_DISCOVER, Ordering::Relaxed);
                // request+0 is the LOCAL scid, assigned before the ACL was
                // even originated; the remote dcid does not exist yet, so
                // nothing can be sent from here. Event 3 below is the only
                // send point.
            }
        }
        3 => {
            // Completion event layout (sub_C86CF54): +108 = local scid,
            // +110 = remote dcid.
            let local = unsafe { core::ptr::read_unaligned(argument.add(108).cast::<u16>()) } as u32;
            let remote = unsafe { core::ptr::read_unaligned(argument.add(110).cast::<u16>()) } as u32;
            if local != 0 {
                CID.store(local, Ordering::Relaxed);
            }
            if remote != 0 {
                TX_CID.store(remote, Ordering::Relaxed);
                // The stock outgoing path never sends the L2CAP config
                // request, so the sink's channel sits in CONFIG and drops
                // our SDUs. Send it ourselves first (harmless if the sink
                // already configures), then DISCOVER immediately plus one
                // timer retry for sinks that drop pre-config data.
                unsafe { send_l2cap_config_request(local as u16) };
                if PHASE.load(Ordering::Relaxed) == PH_DISCOVER
                    && RESPONSES.load(Ordering::Relaxed) == 0
                    && DISCOVER_SENT.load(Ordering::Relaxed) == 0
                {
                    DISCOVER_SENT.store(1, Ordering::Relaxed);
                    send_discover();
                }
                if RETRY_ARMED.swap(1, Ordering::Relaxed) == 0 {
                    let handle = unsafe {
                        bt_timer_add(
                            bt_l2cap_owner(),
                            DISCOVER_RETRY_MS,
                            1,
                            discover_retry as *const () as usize as *mut c_void,
                            core::ptr::null_mut(),
                            core::ptr::null(),
                            1,
                        )
                    };
                    let _ = handle;
                }
            }
        }
        7 => {
            unsafe { handle_data(argument) };
            // sub_C869764 zeroes its reassembly pointer after this callback
            // and never frees the StockBuffer -> ownership is ours.
            unsafe { bt_free(argument.cast()) };
            FREED.fetch_add(1, Ordering::Relaxed);
            // Decode ONE MP3-frame-sized slice while we are between AVDTP
            // round trips. This pre-fills the SBC ring before START and keeps
            // any single firmware callback short (0.10.18's UI probe decoded
            // all 19 frames at once and visibly hung the page).
            crate::stream::pump(1);
        }
        6 => {
            PHASE.store(PH_REJECT, Ordering::Relaxed);
        }
        _ => {}
    }
    0
}
