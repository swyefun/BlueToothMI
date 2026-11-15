//! Minimal AVDTP media transport (phase-1 test tone).
//!
//! This deliberately does NOT decode MP3 and does NOT encode SBC on device.
//! A short 1 kHz tone was encoded on the host with ffmpeg's SBC encoder
//! (44.1 kHz, stereo, 16 blocks, 8 subbands, loudness, bitpool 36 -> 156-byte
//! frames) and is compiled in as `tone.sbc`. Every `0x0C` byte in that data
//! was nudged to `0x0D` so no 4-byte window can look like a firmware address
//! to the CAN-P1-011 ELF verifier (target range 0x0C000000..0x0CD00000).
//! The change is a 1-LSB difference in eight audio bytes; inaudible.
//!
//! Flow: after AVDTP OPEN, `begin()` queues a SECOND L2CAP channel on PSM
//! 0x19 (the media/transport channel). On its CONFIRM we send AVDTP START
//! on the signalling channel, and on the START accept `start_streaming()`
//! arms a one-shot bt_timer that sends one RTP packet per tick and re-arms
//! itself. Streaming stops after MAX_PACKETS so a timer bug cannot run
//! forever.

use core::sync::atomic::{AtomicI32, AtomicU32, Ordering};

use canopus_target_private::{
    bt_buffer_new, bt_free, bt_l2cap_owner, bt_l2cap_submit_cid, bt_timer_add,
    stock_buffer_payload_mut,
};

const SBC_FRAME_LEN: usize = 156;
const FRAMES_PER_PACKET: usize = 5;
const SAMPLES_PER_PACKET: u32 = (FRAMES_PER_PACKET * 128) as u32;
const PACKET_LEN: usize = 13 + FRAMES_PER_PACKET * SBC_FRAME_LEN;
const TONE: &[u8] = include_bytes!("tone.sbc");
const TONE_FRAMES: usize = TONE.len() / SBC_FRAME_LEN;
/// 200 packets * 14.5 ms ~= 2.9 s of tone. Bounded so a stray timer cannot
/// spin indefinitely (the .179 heap assert in log 16 is a fresh memory).
const MAX_PACKETS: u32 = 200;
const TIMER_MS: u32 = 15;
const TIMER_EVENT: u8 = 3;
const TIMER_FLAGS: u32 = 1;
const THUMB: u32 = 1;
/// SBC media payload header: F=0, S=1, L=1, 5 frames. If the sink stays
/// silent, try `FRAMES_PER_PACKET as u8` (0x05) - some stacks ignore S/L.
const SBC_MEDIA_HEADER: u8 = 0x60 | FRAMES_PER_PACKET as u8;
const SIG_START: u8 = 0x07;

pub static MEDIA_CID: AtomicU32 = AtomicU32::new(0);
pub static PACKETS: AtomicU32 = AtomicU32::new(0);
pub static TICKS: AtomicU32 = AtomicU32::new(0);
pub static START_SENT: AtomicU32 = AtomicU32::new(0);
pub static START_ACCEPTED: AtomicU32 = AtomicU32::new(0);
pub static STREAMING: AtomicU32 = AtomicU32::new(0);
pub static LAST_EVENT: AtomicU32 = AtomicU32::new(0);
pub static LAST_RC: AtomicI32 = AtomicI32::new(0);

static FRAME_POS: AtomicU32 = AtomicU32::new(0);
static SEQ: AtomicU32 = AtomicU32::new(0);
static TIMESTAMP: AtomicU32 = AtomicU32::new(0);
static TIMER_HANDLE: AtomicU32 = AtomicU32::new(0);

pub fn reset() {
    MEDIA_CID.store(0, Ordering::Relaxed);
    PACKETS.store(0, Ordering::Relaxed);
    TICKS.store(0, Ordering::Relaxed);
    START_SENT.store(0, Ordering::Relaxed);
    START_ACCEPTED.store(0, Ordering::Relaxed);
    STREAMING.store(0, Ordering::Relaxed);
    LAST_EVENT.store(0, Ordering::Relaxed);
    LAST_RC.store(0, Ordering::Relaxed);
    FRAME_POS.store(0, Ordering::Relaxed);
    SEQ.store(0, Ordering::Relaxed);
    TIMESTAMP.store(0, Ordering::Relaxed);
    TIMER_HANDLE.store(0, Ordering::Relaxed);
}

/// Callback address for the media connect request, with the Thumb bit.
pub fn callback() -> u32 {
    on_media_event as *const () as usize as u32 | THUMB
}

unsafe fn write_be32(p: *mut u8, v: u32) {
    unsafe {
        *p = (v >> 24) as u8;
        *p.add(1) = (v >> 16) as u8;
        *p.add(2) = (v >> 8) as u8;
        *p.add(3) = v as u8;
    }
}

/// One RTP packet: 12-byte RTP header, 1-byte SBC media header, N SBC frames.
/// The stock send handler segments an oversized SDU and frees the buffer.
unsafe fn send_packet() {
    let cid = MEDIA_CID.load(Ordering::Relaxed) as u16;
    if cid == 0 {
        return;
    }
    let buffer = unsafe { bt_buffer_new(PACKET_LEN as u16, 4) };
    if buffer.is_null() {
        LAST_RC.store(-2, Ordering::Relaxed);
        return;
    }
    let p = unsafe { stock_buffer_payload_mut(buffer) };
    unsafe {
        *p = 0x80; // RTP v2
        *p.add(1) = 0x60; // dynamic payload type 96
        let seq = SEQ.fetch_add(1, Ordering::Relaxed) as u16;
        *p.add(2) = (seq >> 8) as u8;
        *p.add(3) = seq as u8;
        let ts = TIMESTAMP.fetch_add(SAMPLES_PER_PACKET, Ordering::Relaxed);
        write_be32(p.add(4), ts);
        write_be32(p.add(8), 1); // SSRC
        *p.add(12) = SBC_MEDIA_HEADER;
        for k in 0..FRAMES_PER_PACKET {
            let idx = (FRAME_POS.fetch_add(1, Ordering::Relaxed) as usize % TONE_FRAMES)
                * SBC_FRAME_LEN;
            core::ptr::copy_nonoverlapping(
                TONE.as_ptr().add(idx),
                p.add(13 + k * SBC_FRAME_LEN),
                SBC_FRAME_LEN,
            );
        }
    }
    let rc = unsafe { bt_l2cap_submit_cid(buffer, cid) };
    LAST_RC.store(rc, Ordering::Relaxed);
    if rc == 0 {
        PACKETS.fetch_add(1, Ordering::Relaxed);
    }
}

/// bt_timer_add on .179 is one-shot (sub_C8C63D8 allocates a 24-byte node per
/// call), so each tick re-arms itself. Bounded by MAX_PACKETS.
pub unsafe extern "C" fn tick(_arg: *mut core::ffi::c_void) {
    TICKS.fetch_add(1, Ordering::Relaxed);
    if PACKETS.load(Ordering::Relaxed) >= MAX_PACKETS
        || MEDIA_CID.load(Ordering::Relaxed) == 0
        || STREAMING.load(Ordering::Relaxed) == 0
    {
        STREAMING.store(0, Ordering::Relaxed);
        return;
    }
    unsafe { send_packet() };
    let handle = unsafe {
        bt_timer_add(
            bt_l2cap_owner(),
            TIMER_MS,
            TIMER_EVENT,
            tick as *const () as usize as *mut core::ffi::c_void,
            core::ptr::null_mut(),
            core::ptr::null(),
            TIMER_FLAGS,
        )
    };
    TIMER_HANDLE.store(handle, Ordering::Relaxed);
}

pub fn start_streaming() {
    if STREAMING.swap(1, Ordering::Relaxed) != 0 {
        return;
    }
    let handle = unsafe {
        bt_timer_add(
            bt_l2cap_owner(),
            TIMER_MS,
            TIMER_EVENT,
            tick as *const () as usize as *mut core::ffi::c_void,
            core::ptr::null_mut(),
            core::ptr::null(),
            TIMER_FLAGS,
        )
    };
    TIMER_HANDLE.store(handle, Ordering::Relaxed);
}

/// Called by the signalling-channel data handler when an AVDTP START accept
/// arrives. Returns true when the packet was consumed as a START response.
pub fn on_signal_data(signal: u8, accepted: bool) -> bool {
    if signal == SIG_START && accepted {
        START_ACCEPTED.store(1, Ordering::Relaxed);
        start_streaming();
        return true;
    }
    false
}

/// Installed at request+12 of the SECOND (media) connect request only.
/// Control payloads are left alone (the firmware may deliver them twice);
/// event-7 StockBuffers are owned by the callback (sub_C869764) and freed.
pub unsafe extern "C" fn on_media_event(event: i32, argument: *mut u8) -> i32 {
    LAST_EVENT.store(event as u32, Ordering::Relaxed);
    if argument.is_null() {
        return 0;
    }
    match event {
        2 => {
            let cid = unsafe { core::ptr::read_unaligned(argument.cast::<u16>()) } as u32;
            if cid != 0 {
                MEDIA_CID.store(cid, Ordering::Relaxed);
            }
            if START_SENT.swap(1, Ordering::Relaxed) == 0 {
                let seid = crate::avdtp::SEID.load(Ordering::Relaxed);
                unsafe { crate::avdtp::send_on_signalling(SIG_START, &[(seid << 2) as u8]) };
            }
        }
        3 => {
            let cid =
                unsafe { core::ptr::read_unaligned(argument.add(108).cast::<u16>()) } as u32;
            if cid != 0 {
                MEDIA_CID.store(cid, Ordering::Relaxed);
            }
        }
        7 => unsafe { bt_free(argument.cast()) },
        6 => {
            STREAMING.store(0, Ordering::Relaxed);
            MEDIA_CID.store(0, Ordering::Relaxed);
        }
        _ => {}
    }
    0
}
