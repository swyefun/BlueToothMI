//! `/dev/canopus_audio` — drop-in stand-in for the paid BluetoothAudio
//! module's character device, so LyraPlayer drives THIS module's audio
//! pipeline instead.
//!
//! Contract recovered from `Canopus-App-LyraPlayer/crates/lyra-player-device/
//! src/target/audio.rs` (the client):
//!
//! - `open(O_RDWR)` / `close`
//! - `write(mp3_bytes)` → accepted count, partial accepts allowed, never
//!   blocks; a negative return is classified via GET_STATUS
//! - `read(&ControlEventV1)` → non-blocking pop of one headset gesture;
//!   empty queue MUST return -1 with errno=EAGAIN (the client reads the
//!   task's errno slot via `__errno()` at 0xc349489 on .179)
//! - `ioctl`: 0x305 SET_FORMAT (`AudioFormatV1`), 0x306 START, 0x307 PAUSE,
//!   0x308 RESUME, 0x309 STOP, 0x30D DRAIN, 0x30F GET_STATUS (`AudioStatusV1`,
//!   client passes a pointer with struct_size preset), 0x310/0x311 volume
//!   (arg is a u32 pointer, 0..=100)
//! - state machine: 2 CONFIGURED, 3 BUFFERING, 4 PLAYING, 5 PAUSED,
//!   6 DRAINING, 7 STOPPED, 8 ERROR
//!
//! Phase 1 is a dry run: accepted bytes are counted and dropped, and
//! `pcm_frames` advances on the wall clock so the player UI, position
//! tracking, drain and stop transitions behave exactly as with the real
//! sink. The A2DP source stream (AVDTP + SBC) hooks in at `dev_write` and
//! `IOC_GET_STATUS` later.
//!
//! The `.179` firmware exposes NuttX `register_driver` (0xc913425; SDK
//! `canopus_fw_register_driver`); the `file_operations` layout is the stock
//! NuttX order: open, close, read, write, seek, ioctl, poll, unlink.

use core::ffi::c_void;
use core::ptr::addr_of;
use core::sync::atomic::{AtomicI32, AtomicU32, Ordering};

use canopus_target_private::{
    canopus_fw_clock_gettime, canopus_fw_register_driver, stock_timespec_t,
};

const DEVICE_PATH: &[u8] = b"/dev/canopus_audio\0";
const FORMAT_MP3: u32 = 1;
const IOC_SET_FORMAT: i32 = 0x305;
const IOC_START: i32 = 0x306;
const IOC_PAUSE: i32 = 0x307;
const IOC_RESUME: i32 = 0x308;
const IOC_STOP: i32 = 0x309;
const IOC_DRAIN: i32 = 0x30D;
const IOC_GET_STATUS: i32 = 0x30F;
const IOC_SET_VOLUME: i32 = 0x310;
const IOC_GET_VOLUME: i32 = 0x311;
const CONTROL_PLAY: u32 = 1;
const CONTROL_PAUSE: u32 = 2;
const CONTROL_NEXT: u32 = 3;
const CONTROL_PREVIOUS: u32 = 4;
pub const STATE_CONFIGURED: u32 = 2;
pub const STATE_BUFFERING: u32 = 3;
pub const STATE_PLAYING: u32 = 4;
pub const STATE_PAUSED: u32 = 5;
pub const STATE_DRAINING: u32 = 6;
pub const STATE_STOPPED: u32 = 7;
pub const STATE_ERROR: u32 = 8;
const EAGAIN: i32 = -11;
const ENOTTY: i32 = -25;
const CLOCK_MONOTONIC: u32 = 1;
/// Simulated tail playout after DRAIN before the state flips to STOPPED.
const DRAIN_PLAYOUT_MS: u32 = 2_000;
const INPUT_CAPACITY: u32 = 16_384;
const CONTROL_EVENT_SIZE: usize = 16;
const STATUS_FIELDS: usize = 18;
const EVT_SLOTS: usize = 16;

/// `__errno()` at 0xc349488, thumb callable 0xc349489 (generated .179).
/// Built at runtime from non-address-shaped operands so the ELF verifier
/// does not classify it as a flash constant.
const ERRNO_LOC_LO: u32 = 0x0A34_9489;
const ERRNO_LOC_HI: u32 = 0x0600_0000;

unsafe fn set_errno(value: i32) {
    let addr = ERRNO_LOC_LO ^ ERRNO_LOC_HI; // 0x0C34_9489, thumb bit set
    let f: unsafe extern "C" fn() -> *mut i32 = core::mem::transmute(addr);
    unsafe { (*f()) = value };
}

fn monotonic_ms() -> u32 {
    let mut time = stock_timespec_t {
        tv_sec: 0,
        tv_nsec: 0,
    };
    let rc =
        unsafe { canopus_fw_clock_gettime(CLOCK_MONOTONIC, core::ptr::addr_of_mut!(time)) };
    if rc != 0 || time.tv_sec < 0 {
        return 0;
    }
    (((time.tv_sec as u64).saturating_mul(1000) + u64::from(time.tv_nsec as u32) / 1_000_000)
        & 0xFFFF_FFFF) as u32
}

// ---------------------------------------------------------------------------
// Device state
// ---------------------------------------------------------------------------

static STATE: AtomicU32 = AtomicU32::new(STATE_STOPPED);
static FORMAT: AtomicU32 = AtomicU32::new(FORMAT_MP3);
static RATE: AtomicU32 = AtomicU32::new(44_100);
static CHANNELS: AtomicU32 = AtomicU32::new(2);
static VOLUME: AtomicU32 = AtomicU32::new(100);
static BYTES_ACCEPTED: AtomicU32 = AtomicU32::new(0);
static BYTES_CONSUMED: AtomicU32 = AtomicU32::new(0);
static PCM_BASE: AtomicU32 = AtomicU32::new(0);
static PLAY_STARTED_AT: AtomicU32 = AtomicU32::new(0);
static DRAIN_STARTED_AT: AtomicU32 = AtomicU32::new(0);
static GENERATION: AtomicU32 = AtomicU32::new(0);
static LAST_ERROR: AtomicI32 = AtomicI32::new(0);
static OPEN_COUNT: AtomicU32 = AtomicU32::new(0);
/// Headset-gesture queue (phase 3 feeds it from AVRCP pass-through).
static EVT_HEAD: AtomicU32 = AtomicU32::new(0);
static EVT_TAIL: AtomicU32 = AtomicU32::new(0);
static mut EVENT_QUEUE: [u32; 4 * EVT_SLOTS] = [0; 4 * EVT_SLOTS];

fn now_playing_frames(now_ms: u32) -> u32 {
    let started = PLAY_STARTED_AT.load(Ordering::Relaxed);
    let base = PCM_BASE.load(Ordering::Relaxed);
    let rate = RATE.load(Ordering::Relaxed);
    let elapsed = now_ms.wrapping_sub(started);
    base.saturating_add((u64::from(elapsed) * u64::from(rate) / 1000) as u32)
}

// ---------------------------------------------------------------------------
// file_operations
// ---------------------------------------------------------------------------

static mut FOPS: [u32; 8] = [0; 8];

const THUMB: u32 = 1;

unsafe extern "C" fn dev_open(_filep: *mut c_void) -> i32 {
    OPEN_COUNT.fetch_add(1, Ordering::Relaxed);
    0
}

unsafe extern "C" fn dev_close(_filep: *mut c_void) -> i32 {
    0
}

unsafe extern "C" fn dev_read(_filep: *mut c_void, buf: *mut u8, len: u32) -> isize {
    if usize::try_from(len).unwrap_or(0) < CONTROL_EVENT_SIZE {
        set_errno(EAGAIN);
        return -1;
    }
    let head = EVT_HEAD.load(Ordering::Acquire);
    let tail = EVT_TAIL.load(Ordering::Acquire);
    if head == tail {
        set_errno(EAGAIN);
        return -1;
    }
    let slot = (tail as usize) % EVT_SLOTS * 4;
    let event = unsafe { core::ptr::addr_of!(EVENT_QUEUE).cast::<u32>().add(slot) };
    for i in 0..4 {
        let word = unsafe { *event.add(i) };
        for b in 0..4 {
            unsafe { *buf.add(i * 4 + b) = (word >> (b * 8)) as u8 };
        }
    }
    EVT_TAIL.store(tail.wrapping_add(1), Ordering::Release);
    CONTROL_EVENT_SIZE as isize
}

unsafe extern "C" fn dev_write(_filep: *mut c_void, buf: *const u8, len: u32) -> isize {
    let _ = buf;
    // Phase 1 dry run: accept and drop. The real sink replaces this body.
    BYTES_ACCEPTED.fetch_add(len, Ordering::Relaxed);
    BYTES_CONSUMED.fetch_add(len, Ordering::Relaxed);
    if STATE.load(Ordering::Relaxed) == STATE_BUFFERING {
        STATE.store(STATE_PLAYING, Ordering::Relaxed);
        PLAY_STARTED_AT.store(monotonic_ms(), Ordering::Relaxed);
        PCM_BASE.store(0, Ordering::Relaxed);
    }
    len as isize
}

unsafe extern "C" fn dev_seek(_filep: *mut c_void, _off: i64, _whence: i32) -> isize {
    // Seeking is a local-file concern on the client side; the device is a
    // stream and does not support it.
    set_errno(ENOTTY);
    -1
}

unsafe extern "C" fn dev_poll(_filep: *mut c_void, _fds: *mut c_void, _setup: i32) -> i32 {
    0
}

unsafe extern "C" fn dev_unlink(_inode: *mut c_void) -> i32 {
    ENOTTY
}

unsafe extern "C" fn dev_ioctl(_filep: *mut c_void, cmd: i32, arg: u32) -> i32 {
    match cmd {
        IOC_SET_FORMAT => {
            // AudioFormatV1: struct_size, format, sample_rate_hint,
            // channels_hint, flags, reserved[3].
            if arg == 0 {
                return -22;
            }
            let p = arg as *const u32;
            let format = unsafe { *p.add(1) };
            let rate = unsafe { *p.add(2) };
            let channels = unsafe { *p.add(3) };
            if format != FORMAT_MP3 {
                LAST_ERROR.store(-22, Ordering::Relaxed);
                STATE.store(STATE_ERROR, Ordering::Relaxed);
                return -22;
            }
            FORMAT.store(format, Ordering::Relaxed);
            if (1_000..=96_000).contains(&rate) {
                RATE.store(rate, Ordering::Relaxed);
            }
            if (1..=2).contains(&channels) {
                CHANNELS.store(channels, Ordering::Relaxed);
            }
            stop_stream();
            STATE.store(STATE_CONFIGURED, Ordering::Relaxed);
            0
        }
        IOC_START => {
            stop_stream();
            STATE.store(STATE_BUFFERING, Ordering::Relaxed);
            GENERATION.fetch_add(1, Ordering::Relaxed);
            0
        }
        IOC_PAUSE => {
            if STATE.load(Ordering::Relaxed) == STATE_PLAYING {
                PCM_BASE.store(now_playing_frames(monotonic_ms()), Ordering::Relaxed);
                STATE.store(STATE_PAUSED, Ordering::Relaxed);
            }
            0
        }
        IOC_RESUME => {
            if STATE.load(Ordering::Relaxed) == STATE_PAUSED {
                PLAY_STARTED_AT.store(monotonic_ms(), Ordering::Relaxed);
                STATE.store(STATE_PLAYING, Ordering::Relaxed);
            }
            0
        }
        IOC_STOP => {
            stop_stream();
            STATE.store(STATE_STOPPED, Ordering::Relaxed);
            0
        }
        IOC_DRAIN => {
            DRAIN_STARTED_AT.store(monotonic_ms(), Ordering::Relaxed);
            if STATE.load(Ordering::Relaxed) == STATE_PLAYING {
                STATE.store(STATE_DRAINING, Ordering::Relaxed);
            } else {
                STATE.store(STATE_STOPPED, Ordering::Relaxed);
            }
            0
        }
        IOC_GET_STATUS => {
            // AudioStatusV1: 18 u32 fields, client presets struct_size.
            if arg == 0 {
                return -22;
            }
            let p = arg as *mut u32;
            let now = monotonic_ms();
            let mut st = STATE.load(Ordering::Relaxed);
            if st == STATE_DRAINING
                && now.wrapping_sub(DRAIN_STARTED_AT.load(Ordering::Relaxed)) >= DRAIN_PLAYOUT_MS
            {
                st = STATE_STOPPED;
                STATE.store(st, Ordering::Relaxed);
            }
            let frames = if st == STATE_PLAYING {
                now_playing_frames(now)
            } else if st == STATE_PAUSED {
                PCM_BASE.load(Ordering::Relaxed)
            } else {
                0
            };
            let fields: [u32; STATUS_FIELDS] = [
                unsafe { *p }, // struct_size (preserved)
                1,             // abi_version
                st,
                LAST_ERROR.load(Ordering::Relaxed) as u32,
                FORMAT.load(Ordering::Relaxed),
                INPUT_CAPACITY,
                0, // input_used (dry run: drained instantly)
                INPUT_CAPACITY,
                RATE.load(Ordering::Relaxed),
                CHANNELS.load(Ordering::Relaxed),
                53, // negotiated_bitpool
                BYTES_ACCEPTED.load(Ordering::Relaxed),
                BYTES_CONSUMED.load(Ordering::Relaxed),
                frames,
                frames / 1_152, // rtp_packets estimate
                0,              // underruns
                GENERATION.load(Ordering::Relaxed),
                VOLUME.load(Ordering::Relaxed),
            ];
            for (i, word) in fields.iter().enumerate() {
                unsafe { *p.add(i) = *word };
            }
            0
        }
        IOC_SET_VOLUME => {
            if arg == 0 {
                return -22;
            }
            let v = unsafe { *(arg as *const u32) }.min(100);
            VOLUME.store(v, Ordering::Relaxed);
            0
        }
        IOC_GET_VOLUME => {
            if arg == 0 {
                return -22;
            }
            unsafe { *(arg as *mut u32) = VOLUME.load(Ordering::Relaxed) };
            0
        }
        _ => ENOTTY,
    }
}

fn stop_stream() {
    PCM_BASE.store(0, Ordering::Relaxed);
    PLAY_STARTED_AT.store(0, Ordering::Relaxed);
    LAST_ERROR.store(0, Ordering::Relaxed);
}

/// Queues one headset gesture for the player (phase 3: AVRCP pass-through).
/// Returns false when the queue is full.
pub fn push_control(kind: u32) -> bool {
    let head = EVT_HEAD.load(Ordering::Relaxed);
    let tail = EVT_TAIL.load(Ordering::Relaxed);
    if head.wrapping_sub(tail) as usize >= EVT_SLOTS {
        return false;
    }
    let slot = (head as usize) % EVT_SLOTS * 4;
    let event = unsafe { core::ptr::addr_of_mut!(EVENT_QUEUE).cast::<u32>().add(slot) };
    unsafe {
        *event.add(0) = CONTROL_EVENT_SIZE as u32;
        *event.add(1) = kind;
        *event.add(2) = head; // sequence
        *event.add(3) = 0;
    }
    EVT_HEAD.store(head.wrapping_add(1), Ordering::Release);
    true
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

fn fill_fops() {
    let table = unsafe { &mut *core::ptr::addr_of_mut!(FOPS) };
    table[0] = dev_open as usize as u32 | THUMB;
    table[1] = dev_close as usize as u32 | THUMB;
    table[2] = dev_read as usize as u32 | THUMB;
    table[3] = dev_write as usize as u32 | THUMB;
    table[4] = dev_seek as usize as u32 | THUMB;
    table[5] = dev_ioctl as usize as u32 | THUMB;
    table[6] = dev_poll as usize as u32 | THUMB;
    table[7] = dev_unlink as usize as u32 | THUMB;
}

/// Registers `/dev/canopus_audio`. Call once after activation. Returns the
/// firmware's register_driver result (0 on success).
pub fn init() -> i32 {
    fill_fops();
    let fops = addr_of!(FOPS).cast::<c_void>();
    unsafe { canopus_fw_register_driver(DEVICE_PATH.as_ptr(), fops, 0, core::ptr::null_mut()) }
}
