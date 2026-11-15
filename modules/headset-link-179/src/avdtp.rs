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
//! This module implements exactly that initiator side, without SDP (the
//! SEIDs come from AVDTP DISCOVER itself), without an audio source and
//! without SBC encoding: OPEN is the terminal state.
//!
//! ABI (docs/band11-bluetooth-139.md §"关键 ABI 修正", NOTES-ida-179.md §3):
//!   bt_l2cap_connect queues a 68-byte request; its handler finds/creates the
//!   BR/EDR peer, allocates a 124-byte channel, installs request+12 as the
//!   channel callback at channel+20 and calls it with (2, request) where the
//!   CID is at request+0. `bt_l2cap_connect` itself originates the ACL via
//!   sub_C86BC18, so no cmd 62 pairing round-trip is needed.
//!   Event 3 = channel complete (CID+108), event 7 = received StockBuffer
//!   (CID+4, payload = base+4+offset, length = total-offset).
//!
//! Callback payloads are deliberately not freed: event 2 owns the module's
//! connect request, events 3/7 own firmware-allocated objects whose allocator
//! is not proven on `.179`. A few hundred leaked bytes per connection is the
//! safe side of an unverified free.

use core::sync::atomic::{AtomicI32, AtomicU32, Ordering};
use canopus_target_private::{
    bt_buffer_new, bt_l2cap_submit_cid, stock_buffer_payload, stock_buffer_payload_mut, StockBuffer,
};

pub const PH_IDLE: u32 = 0;
pub const PH_CONNECTING: u32 = 1;
pub const PH_DISCOVER: u32 = 2;
pub const PH_GETCAP: u32 = 3;
pub const PH_SETCFG: u32 = 4;
pub const PH_OPEN: u32 = 5;
pub const PH_DONE: u32 = 6;
pub const PH_REJECT: u32 = 7;

const SIG_DISCOVER: u8 = 0x01;
const SIG_GET_CAPABILITIES: u8 = 0x02;
const SIG_SET_CONFIGURATION: u8 = 0x03;
const SIG_OPEN: u8 = 0x06;

/// Generic, widely accepted SBC configuration: 44.1 kHz / joint stereo,
/// 16 blocks, 8 subbands, loudness allocation, bitpool 2..53. Replaced with
/// the peer's advertised capabilities after GET_CAPABILITIES succeeds.
const DEFAULT_SBC: u32 = u32::from_le_bytes([0x21, 0x15, 0x02, 0x35]);

pub static PHASE: AtomicU32 = AtomicU32::new(PH_IDLE);
pub static CID: AtomicU32 = AtomicU32::new(0);
pub static RESPONSES: AtomicU32 = AtomicU32::new(0);
pub static LAST_SIGNAL: AtomicU32 = AtomicU32::new(0);
pub static LAST_EVENT: AtomicU32 = AtomicU32::new(0);
pub static LAST_RC: AtomicI32 = AtomicI32::new(0);

static LABEL: AtomicU32 = AtomicU32::new(0);
static SEID: AtomicU32 = AtomicU32::new(0);
static SBC_CFG: AtomicU32 = AtomicU32::new(DEFAULT_SBC);

pub fn reset() {
    PHASE.store(PH_CONNECTING, Ordering::Relaxed);
    CID.store(0, Ordering::Relaxed);
    RESPONSES.store(0, Ordering::Relaxed);
    LAST_SIGNAL.store(0, Ordering::Relaxed);
    LAST_EVENT.store(0, Ordering::Relaxed);
    LAST_RC.store(0, Ordering::Relaxed);
    LABEL.store(0, Ordering::Relaxed);
    SEID.store(0, Ordering::Relaxed);
    SBC_CFG.store(DEFAULT_SBC, Ordering::Relaxed);
}

/// Sends one single-packet AVDTP command. The stock send handler segments the
/// SDU itself; a channel that is not OPEN yet queues the buffer and the
/// credit-queue flush releases it after event 3.
unsafe fn send(signal: u8, params: &[u8]) -> i32 {
    let cid = CID.load(Ordering::Relaxed) as u16;
    if cid == 0 {
        return -1;
    }
    let label = (LABEL.fetch_add(1, Ordering::Relaxed) & 0x0F) as u8;
    let length = 2 + params.len();
    let buffer = unsafe { bt_buffer_new(length as u16, 0) };
    if buffer.is_null() {
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
    LAST_RC.store(rc, Ordering::Relaxed);
    rc
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
            let info = unsafe { data.add(offset + 2) };
            // info[0] = media type, info[1] = codec type (0 = SBC),
            // info[2..6] = sampling/channel, block/subband/alloc, bitpools.
            if unsafe { *info.add(1) } == 0x00 {
                let bytes = [
                    unsafe { *info.add(2) },
                    unsafe { *info.add(3) },
                    unsafe { *info.add(4) },
                    unsafe { *info.add(5) },
                ];
                SBC_CFG.store(u32::from_le_bytes(bytes), Ordering::Relaxed);
            }
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

    // message type 2 = accept; 1 = general reject, 3 = reject.
    if (header & 0x03) != 2 {
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
        }
        _ => {}
    }
}

/// Installed as request+12; the firmware keeps it at channel+20 and calls it
/// for every channel event: 2 CONFIRM, 3 COMPLETE, 6 DISCONNECT, 7 DATA.
pub unsafe extern "C" fn handle_event(event: i32, argument: *mut u8) {
    LAST_EVENT.store(event as u32, Ordering::Relaxed);
    if argument.is_null() {
        return;
    }
    match event {
        2 => {
            let cid = unsafe { core::ptr::read_unaligned(argument.cast::<u16>()) } as u32;
            if cid != 0 {
                CID.store(cid, Ordering::Relaxed);
            }
            if PHASE.load(Ordering::Relaxed) == PH_CONNECTING {
                PHASE.store(PH_DISCOVER, Ordering::Relaxed);
                // The channel may still be CONFIG state; the stock send
                // handler queues this SDU and flushes it on event 3.
                send_discover();
            }
        }
        3 => {
            let cid = unsafe { core::ptr::read_unaligned(argument.add(108).cast::<u16>()) } as u32;
            let known = CID.load(Ordering::Relaxed);
            if cid != 0 && (known == 0 || cid == known) {
                CID.store(cid, Ordering::Relaxed);
            }
            // If event 2's SDU never got flushed, re-issue DISCOVER with a
            // fresh label. A duplicate accept is harmless: PHASE has moved on.
            if PHASE.load(Ordering::Relaxed) == PH_DISCOVER
                && RESPONSES.load(Ordering::Relaxed) == 0
            {
                send_discover();
            }
        }
        7 => unsafe { handle_data(argument) },
        6 => {
            PHASE.store(PH_REJECT, Ordering::Relaxed);
        }
        _ => {}
    }
}
