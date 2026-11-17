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
//!   Channel config truth (re-verified 0.10.27): sub_C86874C, which the
//!   connect handler runs for every outgoing channel, sets endpoint-0 MTU
//!   (channel+52) to the 672 stack default and channel+33 (mode) to 0 - so
//!   outgoing channels are born fully valid and the stock auto-reply
//!   (sub_C86CFD8) always emits a legal config request. The earlier MTU=0
//!   theory (0.10.23-0.10.26) was wrong: +52 is never zero.
//!   THE ACTUAL r0 ROOT CAUSE (0.10.15..0.10.26): bt_buffer_new headroom.
//!   sub_C86CC50 stamps a fixed preamble flag 0x81 at buffer+10 AFTER
//!   writing the 4-byte L2CAP header at [offset, offset+4). Stock buffers
//!   use headroom 12 (header at +16..20, flag lands in spare bytes); our
//!   headroom 4 put the header at +8..12, so the stamp overwrote the
//!   destination-CID low byte - every module-sent packet left the band
//!   addressed to CID 0x0081 and the sink dropped it silently. Stock-sent
//!   conn/conf responses (headroom 12) always reached the sink, which is
//!   why the channel came up but AVDTP never got an answer. Fix: allocate
//!   TX buffers with headroom 12 in avdtp::send and media::send_packet.
//!   DISCOVER is sent at completion and re-sent on a self-re-arming
//!   150 ms timer (up to 4 copies) until a response arrives.
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
//                callable 0x0C86BD29 = 0x0A86BD29 ^ 0x06000000. 0.10.22 shipped
//                a typo (0x0A6BD29) that transmuted to 0x06A6BD29 - unmapped
//                memory, bus fault, instant reboot (crash4/crash5, PC 06a6bd28).
//   sub_C86C150  builds and sends the L2CAP configuration request (code 4)
//                for a channel; used by the stack's own conf-rsp retry path.
const L2CAP_FIND_CH_LO: u32 = 0x0A86_BD29;
const L2CAP_FIND_CH_HI: u32 = 0x0600_0000;
const L2CAP_CONF_REQ_LO: u32 = 0x0A86_C151;
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
/// Number of DISCOVER commands issued: one at completion, then re-sends
/// from the self-re-arming 150 ms timer (max 4). Shown on the status row.
pub static DISCOVER_SENT: AtomicU32 = AtomicU32::new(0);
/// Guards double-arming the retry timer (one completion per channel).
static RETRY_ARMED: AtomicU32 = AtomicU32::new(0);
/// Set once the OPEN accept deferred the media-channel kick.
static MEDIA_KICK: AtomicU32 = AtomicU32::new(0);
/// Set by the UI button; consumed by the sig_waiter timer in the BT task.
/// bt_timer_add itself must NEVER run in the UI task - 0.10.32's button
/// called it there and the band crashed instantly.
static KICK_REQ: AtomicU32 = AtomicU32::new(0);
/// Guards double-arming the sig_waiter chain.
static WAITER_ARMED: AtomicU32 = AtomicU32::new(0);
/// Total sig_waiter firings (status row k).
pub static KICKS: AtomicU32 = AtomicU32::new(0);
/// Sends made within the CURRENT phase; reset on every phase advance. The
/// kick re-arms until this reaches 3, mirroring the DISCOVER retry that
/// proved necessary (v41: d4, the sink answered a later copy).
static PHASE_SENDS: AtomicU32 = AtomicU32::new(0);
/// Raw first header byte of the last received signalling packet
/// (label<<4 | packet_type<<2 | message_type). Status row h, 2 hex digits:
/// message_type 2 = accept, 1 = general reject, 3 = reject.
pub static LAST_HEADER: AtomicU32 = AtomicU32::new(0);
/// Config-request diagnostic for the status row:
/// 0 never ran | 1 sent | 2 skipped (+114 bit2 already set) |
/// 3 find_ch returned NULL | 4 link pointer zero.
pub static CONF_SENT: AtomicU32 = AtomicU32::new(0);
pub static SEID: AtomicU32 = AtomicU32::new(0);
/// DISCOVER-response forensics (v50): [raw byte 0] | [raw byte 1]<<8 |
/// [SEP count]<<16 | [chosen SEID]<<24. The pre-v50 parser scanned the
/// response BYTE-BY-BYTE instead of in SEID/TSEP pairs, so with the first
/// endpoint in use it read the media/TSEP byte as a SEID and SET_CFG then
/// targeted a nonexistent endpoint - prime suspect for the instant NPU
/// hard-lock on the 0.10.33 button test.
pub static SEP_INFO: AtomicU32 = AtomicU32::new(0);
/// v55 TX-path forensics: channel byte 33 (send-mode selector in
/// sub_C86CC50) | byte 71 (queue type) | byte 120 (channel state)
/// packed low-to-high. sub_C86CC50 only sends DIRECTLY when +33 == 0;
/// any other value queues the SDU on the link's flow-control list and, if
/// +71 == 3, waits for peer L2CAP credits that this NPU never grants -
/// exactly the "first command ok, everything after vanishes, NPU locks
/// ~20 s later" signature. Cleared at COMPLETE; keep the values visible.
pub static CHSTAT: AtomicU32 = AtomicU32::new(0);
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
    CONF_SENT.store(0, Ordering::Relaxed);
    MEDIA_KICK.store(0, Ordering::Relaxed);
    KICKS.store(0, Ordering::Relaxed);
    PHASE_SENDS.store(0, Ordering::Relaxed);
    KICK_REQ.store(0, Ordering::Relaxed);
    WAITER_ARMED.store(0, Ordering::Relaxed);
    LAST_HEADER.store(0, Ordering::Relaxed);
    SEID.store(0, Ordering::Relaxed);
    SEP_INFO.store(0, Ordering::Relaxed);
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
    // v53 event ring: 8 DISCOVER, 9 GET_CAP, A SET_CFG, B OPEN, F other
    crate::evlog(match signal {
        0x01 => 0x08,
        0x02 => 0x09,
        0x03 => 0x0A,
        0x06 => 0x0B,
        _ => 0x0F,
    });
    // Headroom MUST be >= 8 (stock uses 12): sub_C86CC50 writes the 4-byte
    // L2CAP header at [offset, offset+4) and then stamps its fixed preamble
    // flag at buffer+10 (0x81). With headroom 4 the header sits at +8..12 and
    // the stamp overwrote the destination-CID LOW BYTE, so every packet left
    // the band addressed to CID 0x0081 instead of the sink's dcid - the NPU
    // dropped them all silently. That was the A2e3f0r0 signature of
    // 0.10.15..0.10.26 (stock-sent conn/conf responses, allocated with
    // headroom 12, always reached the sink; module-sent data never did).
    let buffer = unsafe { bt_buffer_new(length as u16, 12) };
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

/// Pre-fills the channel's MTU fields BEFORE any config traffic can flow.
/// The stock paths that transmit OUR config request (sub_C86CFD8 after the
/// sink's own request, sub_C871706, and our event-3 backup) all take the MTU
/// parameter from channel+54 and the option VALUE from channel+52 - both 0
/// on a fresh channel. If the sink's request carries no MTU option, 0x6C
/// stays 0 and the emitted request carries an invalid MTU=0 option; the sink
/// rejects it, its channel stays in CONFIG, and every SDU we submit is
/// dropped - the v39-v41 A2e3f0r0 signature. Storing the stack default 672
/// in both fields makes C867D04 omit the option entirely (option-less =
/// accept defaults, valid) or carry whatever valid value the sink
/// announced. Call at CONFIRM (event 2), when the channel exists but the
/// connection response has not been processed yet.
pub unsafe fn prepare_channel_for_config(local_scid: u16) {
    if local_scid == 0 {
        return;
    }
    let find_ch: unsafe extern "C" fn(u32, u32) -> *mut u8 =
        core::mem::transmute(L2CAP_FIND_CH_LO ^ L2CAP_FIND_CH_HI);
    let channel = unsafe { find_ch(0, local_scid as u32) };
    if channel.is_null() {
        return;
    }
    if unsafe { *(channel.add(52).cast::<u16>()) } == 0 {
        unsafe { *(channel.add(52).cast::<u16>()) = 672 };
    }
    if unsafe { *(channel.add(54).cast::<u16>()) } == 0 {
        unsafe { *(channel.add(54).cast::<u16>()) = 672 };
    }
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
        CONF_SENT.store(3, Ordering::Relaxed);
        return;
    }
    // C86C150 sets +114 bit2 itself after sending; skip a re-run (event 3 can
    // fire more than once, e.g. again after our own conf response).
    if unsafe { *(channel.add(114).cast::<u16>()) } & 4 != 0 {
        CONF_SENT.store(2, Ordering::Relaxed);
        return;
    }
    let link = unsafe { *(channel.cast::<u32>()) };
    if link == 0 {
        CONF_SENT.store(4, Ordering::Relaxed);
        return;
    }
    // v52 PHONE-REPLICA CONFIG REQUEST. The 21:56 HCI capture (piliplus ->
    // NPU, all accepted) shows the working phone's CONF_REQ to the AVDTP
    // signalling channel ALWAYS carries an explicit MTU option:
    //   04 0d 08 00 43 00 00 00 01 02 a0 02  (type=01 MTU, value 672)
    // Our request has channel+52 == 672, which suppresses the option in
    // sub_C867D04, leaving an option-less CONF_REQ. That is valid L2CAP,
    // but it is the ONLY remaining L2CAP-level difference from the working
    // phone - and the NPU hard-locks ~20 s after DISCOVER regardless of
    // which AVDTP command follows (0.10.32 sent nothing, v51 sent GET_CAP;
    // k78 = 78 x 250 ms ~= 19.5 s in the v51 capture), pointing at the
    // config stage, not AVDTP. sub_C867DE8 copies the option VALUE from
    // channel+52, and the option is emitted iff +52 != 672 - so stage 668
    // (equally legal; the phone itself uses MTU 668 on its AVRCP channel)
    // for the request, then restore 672 so every accept/bookkeeping path
    // keeps its proven behaviour.
    if unsafe { *(channel.add(52).cast::<u16>()) } == 0 {
        unsafe { *(channel.add(52).cast::<u16>()) = 672 };
    }
    unsafe { *(channel.add(52).cast::<u16>()) = 668 };
    let mtu = unsafe { *(channel.add(52).cast::<u16>()) };
    let conf_req: unsafe extern "C" fn(*mut u8, *mut u8, u16) =
        core::mem::transmute(L2CAP_CONF_REQ_LO ^ L2CAP_CONF_REQ_HI);
    unsafe { conf_req(link as usize as *mut u8, channel, mtu) };
    unsafe { *(channel.add(52).cast::<u16>()) = 672 };
    crate::evlog(0x0C);
    CONF_SENT.store(1, Ordering::Relaxed);
}

/// 150 ms cadence: the sink's channel only OPENS after the config exchange
/// completes, so the copies sent at completion may be dropped. Re-arm until
/// an AVDTP response arrives or 4 copies are out; a one-shot timer that
/// re-arms itself follows the media timer pattern.
unsafe extern "C" fn discover_retry(_arg: *mut c_void) {
    if PHASE.load(Ordering::Relaxed) == PH_DISCOVER
        && RESPONSES.load(Ordering::Relaxed) == 0
        && TX_CID.load(Ordering::Relaxed) != 0
        && DISCOVER_SENT.load(Ordering::Relaxed) < 4
    {
        DISCOVER_SENT.fetch_add(1, Ordering::Relaxed);
        send_discover();
        unsafe { arm_discover_retry() };
    }
}

/// Self-re-arming 250 ms waiter, armed ONCE from the completion event (BT
/// task). When the UI button sets KICK_REQ, the send happens HERE, in timer
/// context: every firmware call the button path needs (bt_timer_add,
/// bt_buffer_new, bt_l2cap_submit_cid) is only proven safe in the BT task,
/// and 0.10.32 calling bt_timer_add from the UI task crashed the band.
unsafe extern "C" fn sig_waiter(_arg: *mut c_void) {
    KICKS.fetch_add(1, Ordering::Relaxed);
    let phase = PHASE.load(Ordering::Relaxed);
    match phase {
        PH_GETCAP | PH_SETCFG | PH_OPEN => {
            // v51: automatic retry while the phase is unanswered, at most 3
            // sends per phase (the manual button just forces the same path).
            // Each accept switches phase and resets PHASE_SENDS to 0, so the
            // next command goes out on the following tick.
            if PHASE_SENDS.load(Ordering::Relaxed) < 3 {
                let seid = SEID.load(Ordering::Relaxed);
                match phase {
                    PH_GETCAP => {
                        PHASE_SENDS.fetch_add(1, Ordering::Relaxed);
                        send_get_capabilities(seid);
                    }
                    PH_SETCFG => {
                        // Single-shot: the 0.10.33 test showed the NPU
                        // hard-locks instantly on SET_CFG; never re-send it
                        // automatically. The UI button (KICK_REQ) may force
                        // one explicit re-send for diagnostics.
                        if PHASE_SENDS.swap(1, Ordering::Relaxed) == 0
                            || KICK_REQ.swap(0, Ordering::Relaxed) == 1
                        {
                            send_set_configuration(seid);
                        }
                    }
                    _ => {
                        PHASE_SENDS.fetch_add(1, Ordering::Relaxed);
                        send_open(seid);
                    }
                }
            }
            unsafe { arm_sig_waiter() };
        }
        // DONE/REJECT/IDLE: the chain ends (media_open_kick owns DONE).
        _ => {}
    }
}

/// Arms the self-re-arming signalling waiter (250 ms cadence).
unsafe fn arm_sig_waiter() {
    let handle = unsafe {
        bt_timer_add(
            bt_l2cap_owner(),
            250,
            1,
            sig_waiter as *const () as usize as *mut c_void,
            core::ptr::null_mut(),
            core::ptr::null(),
            1,
        )
    };
    let _ = handle;
}

/// UI-safe trigger: sets a flag only. All firmware calls happen in
/// sig_waiter, inside the BT task.
pub fn kick_next() {
    KICK_REQ.store(1, Ordering::Relaxed);
}

/// 100 ms after the OPEN accept: queue the media L2CAP channel from the
/// timer context, never from inside the RX dispatch.
unsafe extern "C" fn media_open_kick(_arg: *mut c_void) {
    if PHASE.load(Ordering::Relaxed) == PH_DONE {
        let _ = crate::device::open_media_channel();
    }
}

/// Arms the one-shot media-open kick (100 ms).
unsafe fn arm_media_open_kick() {
    let handle = unsafe {
        bt_timer_add(
            bt_l2cap_owner(),
            100,
            1,
            media_open_kick as *const () as usize as *mut c_void,
            core::ptr::null_mut(),
            core::ptr::null(),
            1,
        )
    };
    let _ = handle;
}

/// Arms the one-shot DISCOVER retry timer (150 ms). Safe to call repeatedly:
/// each call creates a new one-shot; the DISCOVER_SENT/RESPONSES guards bound
/// the total to 4 copies.
unsafe fn arm_discover_retry() {
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

fn send_discover() {
    let _ = unsafe { send(SIG_DISCOVER, &[]) };
}

fn send_get_capabilities(seid: u32) {
    let _ = unsafe { send(SIG_GET_CAPABILITIES, &[(seid << 2) as u8]) };
}

fn send_set_configuration(seid: u32) {
    let sbc = SBC_CFG.load(Ordering::Relaxed).to_le_bytes();
    // ACP SEID, INT SEID (our endpoint 1), then the capabilities list.
    // Verified byte-for-byte against the phone logcat (codec_arry, the
    // Fluoride side's own print of the SBC codec capability it uses):
    //   [0x01 0x00]                              Media Transport, no params
    //   [0x07 0x06 0x00 0x00 0x21 0x15 0x02 0x35] Media Codec, LOSC 6:
    //     media_type 0 (audio), codec_type 0 (SBC), then the SBC IE
    //     21 15 02 35 = 44.1 kHz (b5), JOINT STEREO (b0), 16 blocks (b4),
    //     8 subbands (b2), Loudness (b0), bitpool 2..53.
    // 0.10.29 and earlier had category 0x02 (REPORTING!) here instead of
    // 0x07 (MEDIA_CODEC) - the sink parsed a 6-byte reporting capability
    // and the whole codec entry vanished from the configuration.
    let params = [
        (seid << 2) as u8,
        0x04,
        0x01,
        0x00,
        0x07,
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
    LAST_HEADER.store(header as u32, Ordering::Relaxed);
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
    // v53 event ring: D accept, E reject/other.
    if !accepted {
        crate::evlog(0x0E);
        PHASE.store(PH_REJECT, Ordering::Relaxed);
        return;
    }
    crate::evlog(0x0D);

    match PHASE.load(Ordering::Relaxed) {
        PH_DISCOVER if signal == SIG_DISCOVER && length >= 3 => {
            // ACP SEID information: the response is PAIRS of bytes -
            //   [SEID<<2 | in_use<<1 | 0] [media_type<<4 | TSEP<<2 | 0]
            // v50: parse pairwise (the old byte-by-byte scan could eat a
            // TSEP byte as a SEID), prefer a FREE audio SINK (TSEP=1).
            let raw0 = (unsafe { *payload.add(2) }) as u32;
            let raw1 = if length >= 4 { (unsafe { *payload.add(3) }) as u32 } else { 0 };
            let seps = (length - 2) / 2;
            let mut seid = 0u32;
            let mut k = 2usize;
            while k + 1 < length {
                let sbyte = unsafe { *payload.add(k) } as u32;
                let mbyte = unsafe { *payload.add(k + 1) } as u32;
                let candidate = (sbyte >> 2) & 0x3F;
                let media = (mbyte >> 4) & 0xF;
                let _tsep = (mbyte >> 2) & 0x3;
                // v51: the phone's HCI capture shows this NPU advertises
                // TSEP bytes 0x08 (tsep field = 2, not the spec 1) - do NOT
                // filter on tsep; just take the first free audio endpoint,
                // exactly what the working phone did (it used SEID 1).
                if candidate != 0 && (sbyte >> 1) & 1 == 0 && media == 0 {
                    seid = candidate;
                    break;
                }
                k += 2;
            }
            if seid == 0 {
                // Fallback: any free SEID on an even (pair-aligned) offset.
                k = 2usize;
                while k + 1 < length {
                    let sbyte = unsafe { *payload.add(k) } as u32;
                    let candidate = (sbyte >> 2) & 0x3F;
                    if candidate != 0 && (sbyte >> 1) & 1 == 0 {
                        seid = candidate;
                        break;
                    }
                    k += 2;
                }
            }
            if seid == 0 {
                seid = (raw0 >> 2) & 0x3F;
                if seid == 0 {
                    seid = 1;
                }
            }
            SEP_INFO.store(
                raw0 | (raw1 << 8) | ((seps as u32) << 16) | (seid << 24),
                Ordering::Relaxed,
            );
            SEID.store(seid, Ordering::Relaxed);
            // v51 PHONE-REPLICA FLOW (from the 21:56 HCI capture, piliplus
            // -> NPU, every step accepted): DISCOVER -> GET_CAP(SEID) ->
            // SET_CFG -> OPEN -> START. The phone ALWAYS sends GET_CAP
            // before SET_CFG (it even queries both SEPs); the NPU's stack
            // very likely needs that response to initialise SEP state, and
            // a bare SET_CFG hard-locks it (our crash since 0.10.31). The
            // old "NPU ignored GET_CAP" finding is from 0.10.28-30, before
            // the signalling L2CAP channel was fully proven - so retry it
            // in the flow again. Advance is automatic via KICK_REQ consumed
            // by sig_waiter in BT timer context.
            PHASE.store(PH_GETCAP, Ordering::Relaxed);
            PHASE_SENDS.store(0, Ordering::Relaxed);
            // sig_waiter sends GET_CAP on its next tick automatically.
            PHASE_SENDS.store(0, Ordering::Relaxed);
            // v51: advance automatically (phone replica). KICK_REQ is
            // consumed by sig_waiter in BT timer context.
        }
        PH_GETCAP if signal == SIG_GET_CAPABILITIES => {
            unsafe { parse_capabilities(payload.add(2), length - 2) };
            PHASE.store(PH_SETCFG, Ordering::Relaxed);
            PHASE_SENDS.store(0, Ordering::Relaxed);
            // SET_CFG follows on the next waiter tick (single-shot).
        }
        PH_SETCFG if signal == SIG_SET_CONFIGURATION => {
            PHASE.store(PH_OPEN, Ordering::Relaxed);
            PHASE_SENDS.store(0, Ordering::Relaxed);
            // OPEN follows on the next waiter tick.
        }
        PH_OPEN if signal == SIG_OPEN => {
            PHASE.store(PH_DONE, Ordering::Relaxed);
            // AVDTP transport session: a SECOND L2CAP channel on PSM 0x19
            // carries media. Do NOT queue it from inside this RX callback:
            // bt_l2cap_connect's work item can drain inline while the RX
            // dispatcher frame is still active (0.10.27's crash shape).
            // Kick it from a one-shot timer instead.
            if MEDIA_KICK.swap(1, Ordering::Relaxed) == 0 {
                unsafe { arm_media_open_kick() };
            }
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
    // v53 event ring: 1 ev2, 2 ev3, 3 ev6, 4 ev7, 5 ev8, 6 ev13
    crate::evlog(match event {
        2 => 1,
        3 => 2,
        6 => 3,
        7 => 4,
        8 => 5,
        13 => 6,
        _ => 0x0F,
    });
    if argument.is_null() {
        return 0;
    }
    match event {
        2 => {
            let cid = unsafe { core::ptr::read_unaligned(argument.cast::<u16>()) } as u32;
            if cid != 0 {
                CID.store(cid, Ordering::Relaxed);
                // Pre-fill MTU fields before the connection response can
                // trigger the config exchange (see prepare_channel_for_config).
                unsafe { prepare_channel_for_config(cid as u16) };
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
                // v55: dump the channel TX-mode bytes and clear +33 so every
                // later submit takes the DIRECT path in sub_C86CC50 instead
                // of the credit queue the NPU never drains. See CHSTAT.
                {
                    let find_ch: unsafe extern "C" fn(u32, u32) -> *mut u8 =
                        core::mem::transmute(L2CAP_FIND_CH_LO ^ L2CAP_FIND_CH_HI);
                    let ch = unsafe { find_ch(0, local as u32) };
                    if !ch.is_null() {
                        let f33 = unsafe { *ch.add(33) } as u32;
                        let f71 = unsafe { *ch.add(71) } as u32;
                        let f120 = unsafe { *ch.add(120) } as u32;
                        CHSTAT.store(f33 | (f71 << 8) | (f120 << 16), Ordering::Relaxed);
                        unsafe { *ch.add(33) = 0 };
                    }
                }
                // The stock outgoing path never sends the L2CAP config
                // request, so the sink's channel sits in CONFIG and drops
                // our SDUs. Send it ourselves first (harmless if the sink
                // already configures; +114 bit4 is normally already set by
                // the stock sender, which is why g2 reads 2 and no "C" ever
                // appears in the event ring).
                unsafe { send_l2cap_config_request(local as u16) };
                // v54 KILLER EXPERIMENT (from the 23:05 event ring): every
                // round was DISCOVER -> instant accept -> GET_CAP x3 ALL
                // IGNORED -> ~20 s disconnect. Make GET_CAP the FIRST
                // command instead (SEID 1 hardcoded; the HCI capture shows
                // this NPU always advertises SEIDs 1+2). If the NPU answers
                // the first command instantly regardless of which command
                // it is, its stack only processes ONE signalling command
                // per channel and the 20 s lock is its state machine
                // hanging after that. If GET_CAP as first command still
                // gets ignored, the poison sits below AVDTP.
                if PHASE.load(Ordering::Relaxed) == PH_DISCOVER
                    && RESPONSES.load(Ordering::Relaxed) == 0
                    && DISCOVER_SENT.load(Ordering::Relaxed) == 0
                {
                    DISCOVER_SENT.store(1, Ordering::Relaxed);
                    SEID.store(1, Ordering::Relaxed);
                    PHASE.store(PH_GETCAP, Ordering::Relaxed);
                    PHASE_SENDS.store(1, Ordering::Relaxed);
                    send_get_capabilities(1);
                }
                if WAITER_ARMED.swap(1, Ordering::Relaxed) == 0 {
                    unsafe { arm_sig_waiter() };
                }
            }
        }
        7 => {
            unsafe { handle_data(argument) };
            // sub_C869764 zeroes its reassembly pointer after this callback
            // and never frees the StockBuffer -> ownership is ours.
            unsafe { bt_free(argument.cast()) };
            FREED.fetch_add(1, Ordering::Relaxed);
            // NO heavy work and NO submits here. 0.10.27 crashed inside
            // the RX dispatch (UsageFault) with the RX dispatcher,
            // submit_cid and a work-queue drain all active at once; 0.10.28
            // still submitted GET_CAP from here and it never drained (f
            // stuck at 1, sink silent, e8, idle teardown). Everything the
            // response processing wants to SEND is deferred: signalling
            // commands via arm_signal_kick, the media channel via
            // media_open_kick, decoding happens in fill_packet only.
        }
        6 => {
            PHASE.store(PH_REJECT, Ordering::Relaxed);
        }
        _ => {}
    }
    0
}
