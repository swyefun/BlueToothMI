//! AVDTP media transport (0.10.19): on-device MP3 -> PCM -> SBC -> RTP.
//!
//! 0.10.17 shipped host-pre-encoded frames (stereo / bitpool 36, 156 bytes,
//! 793-byte packets) which did NOT match the config NPU selected in the phone
//! logcat (`21 15 02 35`, joint stereo / bitpool 53, 119-byte frames) and
//! exceeded its 656-byte stream MTU. Those frames are gone. `stream` now
//! encodes on device with `SbcParameters::HIGH_QUALITY`, which is exactly the
//! negotiated configuration, so 5 frames + 13-byte RTP header = 608 bytes.
//!
//! Flow: after AVDTP OPEN, `device::open_media_channel()` queues a SECOND
//! L2CAP channel on PSM 0x19. On its COMPLETE (not CONFIRM - the channel is
//! still in CONFIG at CONFIRM) we send AVDTP START on the signalling channel;
//! the START accept arms a one-shot bt_timer that emits one RTP packet per
//! tick and re-arms itself. The phone logcat confirms 640 samples per packet
//! (5 x 128) and RTP payload type 0x60.
//!
//! IDA: `sub_C869720` calls the firmware's bt_timer_add with event = 1
//! (owner, 10*flush_timeout, 1, callback, 0, 1). 0.10.17 used 3.
//! When MP3 data runs out, `stream` substitutes a pre-encoded silence SBC
//! frame; the link is never idle, which avoids the local reason-0x16
//! disconnect seen in device log 15.

use core::sync::atomic::{AtomicI32, AtomicU32, Ordering};

use canopus_target_private::{
    bt_buffer_new, bt_free, bt_l2cap_owner, bt_l2cap_submit_cid, bt_timer_add,
    stock_buffer_payload_mut,
};

const SBC_FRAME_LEN: usize = crate::stream::SBC_FRAME_LEN; // 119
const FRAMES_PER_PACKET: usize = crate::stream::FRAMES_PER_PACKET; // 5
const SAMPLES_PER_PACKET: u32 = (FRAMES_PER_PACKET * 128) as u32; // 640
/// 13 + 5*119 = 608 bytes, under the NPU's 656-byte stream MTU.
const PACKET_LEN: usize = 13 + FRAMES_PER_PACKET * SBC_FRAME_LEN;
/// 3 s of MP3 at 640 samples/packet needs ~207 packets. Bounded so a stray
/// timer cannot spin indefinitely.
const MAX_PACKETS: u32 = 240;
/// 640 samples / 44100 Hz = 14.5 ms of audio per packet.
const TIMER_MS: u32 = 15;
/// Firmware-native timer event (sub_C869720 passes 1), not 3.
const TIMER_EVENT: u8 = 1;
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
        let media = core::slice::from_raw_parts_mut(p.add(13), PACKET_LEN - 13);
        // Fills 5 SBC frames. Decoding is sliced: at most one MP3 frame per
        // call, and a failed lock yields silence frames rather than blocking.
        crate::stream::fill_packet(media);
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
            // CONFIRM only reports the CID. The channel is still in L2CAP
            // CONFIG here, so sending START now can race the credit queue.
            let cid = unsafe { core::ptr::read_unaligned(argument.cast::<u16>()) } as u32;
            if cid != 0 {
                MEDIA_CID.store(cid, Ordering::Relaxed);
            }
        }
        3 => {
            let cid =
                unsafe { core::ptr::read_unaligned(argument.add(108).cast::<u16>()) } as u32;
            if cid != 0 {
                MEDIA_CID.store(cid, Ordering::Relaxed);
            }
            if START_SENT.swap(1, Ordering::Relaxed) == 0 {
                let seid = crate::avdtp::SEID.load(Ordering::Relaxed);
                unsafe { crate::avdtp::send_on_signalling(SIG_START, &[(seid << 2) as u8]) };
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
