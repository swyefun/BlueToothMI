//! Streaming MP3 -> PCM -> SBC pipeline (0.10.19).
//!
//! 0.10.18 proved on real `.179` hardware that rmp3 and nanoaudio-sbc both
//! compile, link, pass CAN-P1-011 and run: `d19s21888b119k1x156e0`. It also
//! showed a problem: decoding all 19 MP3 frames inside one UI callback made
//! the page visibly hang. Two causes:
//!   * `src/cstub/string.h` implemented memcpy/memmove/memset as byte loops;
//!     minimp3 moves large buffers constantly. It is now word-wise.
//!   * `probe::run()` decoded the entire 8 KiB sample synchronously.
//!
//! This module therefore does the work IN SLICES. One `pump(1)` decodes at
//! most one MP3 frame (1152 samples/channel -> 9 SBC frames of 128 samples)
//! and never blocks the caller for the whole clip. Slices are spread across
//! the AVDTP signalling round-trips and the media timer ticks, so the UI is
//! never the decode thread.
//!
//! Layout (all static, no heap, no stack blow-up):
//!   probe_mp3.bin (nibble-expanded) -> mp3[] -> RawDecoder -> pcm[2304]
//!   -> SbcEncoder(HIGH_QUALITY) -> sbc frame ring (32 x 119 bytes)
//!   -> media.rs RTP packets (5 frames + 13-byte header = 608 < MTU 656)
//!
//! When the clip ends, a pre-encoded all-zero SBC frame is substituted. That
//! is the keep-alive/silence frame: the firmware tears an idle A2DP link down
//! locally with reason 0x16 (device log 15), so the stream must never go
//! quiet while the media channel is open.
//!
//! HIGH_QUALITY is exactly the configuration the NPU accepted in the phone
//! logcat (`codec_arry = 21 15 02 35`, ChannelMode=3, bitpool 53, 328 kbps),
//! so the encoded frames and SET_CONFIGURATION agree by construction.
//!
//! A trivial non-blocking lock guards the ring: UI callbacks and Bluelet
//! callbacks run on different threads. A failed try_lock is harmless - the
//! caller just gets silence frames for that one packet.

use core::cell::UnsafeCell;
use core::mem::MaybeUninit;
use core::sync::atomic::{AtomicI32, AtomicU32, Ordering};

use nanoaudio_core::codec::Encoder as _;
use nanoaudio_sbc::{SbcEncoder, SbcParameters};
use rmp3::{Frame, RawDecoder, Sample, MAX_SAMPLES_PER_FRAME};

const ENCODED: &[u8] = include_bytes!("probe_mp3.bin");
const MP3_LEN: usize = ENCODED.len() / 2;
/// Measured by the 0.10.18 probe (`b119`); checked again at runtime.
pub const SBC_FRAME_LEN: usize = 119;
const RING_LEN: usize = 32;
const LOW_WATER: usize = 10;

/// Total RTP frames media.rs asks for per packet: 5 x 128 = 640 samples,
/// matching the phone logcat's AVDT_WriteReqOpt timestamp step of 640.
pub const FRAMES_PER_PACKET: usize = 5;

struct Cell<T>(UnsafeCell<T>);
unsafe impl<T> Sync for Cell<T> {}

struct State {
    mp3: [u8; MP3_LEN],
    pcm: [Sample; MAX_SAMPLES_PER_FRAME],
    frames: [[u8; SBC_FRAME_LEN]; RING_LEN],
    silence: [u8; SBC_FRAME_LEN],
    scratch: [u8; SBC_FRAME_LEN],
    dec: MaybeUninit<RawDecoder>,
    enc: MaybeUninit<SbcEncoder>,
    mp3_pos: usize,
    pcm_pos: usize,
    pcm_len: usize,
    head: usize,
    tail: usize,
    count: usize,
    frame_samples: usize,
    ready: bool,
}

static STATE: Cell<State> = Cell(UnsafeCell::new(State {
    mp3: [0; MP3_LEN],
    pcm: [0; MAX_SAMPLES_PER_FRAME],
    frames: [[0; SBC_FRAME_LEN]; RING_LEN],
    silence: [0; SBC_FRAME_LEN],
    scratch: [0; SBC_FRAME_LEN],
    dec: MaybeUninit::uninit(),
    enc: MaybeUninit::uninit(),
    mp3_pos: 0,
    pcm_pos: 0,
    pcm_len: 0,
    head: 0,
    tail: 0,
    count: 0,
    frame_samples: 0,
    ready: false,
}));

static LOCK: AtomicU32 = AtomicU32::new(0);

/// MP3 audio frames decoded.
pub static DECODED: AtomicU32 = AtomicU32::new(0);
/// PCM samples per channel produced.
pub static SAMPLES: AtomicU32 = AtomicU32::new(0);
/// SBC frames produced from real audio.
pub static SBC_FRAMES: AtomicU32 = AtomicU32::new(0);
/// Silence frames injected (underrun or clip end).
pub static SILENCE: AtomicU32 = AtomicU32::new(0);
pub static PUMPS: AtomicU32 = AtomicU32::new(0);
pub static SBC_OK: AtomicU32 = AtomicU32::new(0);
pub static SBC_SYNC: AtomicU32 = AtomicU32::new(0);
pub static MEASURED_FRAME_LEN: AtomicU32 = AtomicU32::new(0);
/// Negative stage marker.
pub static LAST_ERR: AtomicI32 = AtomicI32::new(0);

fn try_lock() -> bool {
    LOCK.compare_exchange(0, 1, Ordering::Acquire, Ordering::Relaxed)
        .is_ok()
}

fn unlock() {
    LOCK.store(0, Ordering::Release);
}

fn denibble(st: &mut State) {
    for i in 0..MP3_LEN {
        let hi = ENCODED[2 * i] & 0x0F;
        let lo = ENCODED[2 * i + 1] & 0x0F;
        st.mp3[i] = (hi << 4) | lo;
    }
}

/// Encodes one SBC frame from `pcm[..frame_samples]` into `dst`.
fn encode_frame(st: &mut State, dst: &mut [u8; SBC_FRAME_LEN]) -> bool {
    let enc = unsafe { st.enc.assume_init_mut() };
    let n = st.frame_samples;
    match enc.encode(&st.pcm[st.pcm_pos..st.pcm_pos + n], &mut st.scratch[..]) {
        Ok(_) => {
            dst.copy_from_slice(&st.scratch);
            st.pcm_pos += n;
            st.pcm_len -= n;
            SBC_OK.fetch_add(1, Ordering::Relaxed);
            true
        }
        Err(_) => {
            LAST_ERR.store(-3, Ordering::Relaxed);
            false
        }
    }
}

/// Decodes one MP3 frame into `pcm`, or fills it with zeros when the clip is
/// exhausted. Returns the number of interleaved i16 samples available.
fn fill_pcm(st: &mut State) -> usize {
    st.pcm_pos = 0;
    st.pcm_len = 0;
    // Skip non-audio frames (ID3 etc.) within this slice instead of turning
    // them into 130 ms of silence.
    for _ in 0..4 {
        if st.mp3_pos >= MP3_LEN {
            break;
        }
        let dec = unsafe { st.dec.assume_init_mut() };
        match dec.next(&st.mp3[st.mp3_pos..], &mut st.pcm) {
            Some((Frame::Audio(audio), used)) => {
                let n = audio.sample_count() as usize * audio.channels() as usize;
                let n = n.min(MAX_SAMPLES_PER_FRAME);
                DECODED.fetch_add(1, Ordering::Relaxed);
                SAMPLES.fetch_add(audio.sample_count() as u32, Ordering::Relaxed);
                st.pcm_len = n;
                if used == 0 {
                    st.mp3_pos = MP3_LEN;
                } else {
                    st.mp3_pos += used;
                }
                return n;
            }
            Some((Frame::Other(_), used)) => {
                if used == 0 {
                    st.mp3_pos = MP3_LEN;
                } else {
                    st.mp3_pos += used;
                }
                continue;
            }
            None => {
                st.mp3_pos = MP3_LEN;
                break;
            }
        }
    }
    // Silence / keep-alive: a full decoder frame of zeros, which yields 9
    // SBC frames from a single MP3-frame-sized slice.
    for x in st.pcm.iter_mut() {
        *x = 0;
    }
    st.pcm_len = MAX_SAMPLES_PER_FRAME;
    SILENCE.fetch_add(1, Ordering::Relaxed);
    MAX_SAMPLES_PER_FRAME
}

fn push_frame(st: &mut State, frame: &[u8; SBC_FRAME_LEN]) {
    st.frames[st.head].copy_from_slice(frame);
    st.head = (st.head + 1) % RING_LEN;
    if st.count < RING_LEN {
        st.count += 1;
    } else {
        st.tail = (st.tail + 1) % RING_LEN;
    }
}

fn pop_frame(st: &mut State, dst: &mut [u8]) -> bool {
    if st.count == 0 {
        dst.copy_from_slice(&st.silence);
        return false;
    }
    dst.copy_from_slice(&st.frames[st.tail]);
    st.tail = (st.tail + 1) % RING_LEN;
    st.count -= 1;
    true
}

/// Produces up to `slices` MP3 frames worth of SBC frames. Called from the
/// AVDTP signalling callbacks and the media timer, never from UI rendering.
pub fn pump(slices: u32) {
    if !try_lock() {
        return;
    }
    PUMPS.fetch_add(1, Ordering::Relaxed);
    let st = unsafe { &mut *STATE.0.get() };
    if !st.ready {
        unlock();
        return;
    }
    for _ in 0..slices {
        if st.count >= LOW_WATER * 2 {
            break;
        }
        if st.pcm_len < st.frame_samples {
            fill_pcm(st);
        }
        while st.pcm_len >= st.frame_samples && st.count < RING_LEN {
            let mut frame = [0u8; SBC_FRAME_LEN];
            if !encode_frame(st, &mut frame) {
                break;
            }
            push_frame(st, &frame);
            SBC_FRAMES.fetch_add(1, Ordering::Relaxed);
        }
    }
    unlock();
}

/// Fills one RTP media payload with 5 SBC frames. Frames that are not ready
/// yet are replaced by the pre-encoded silence frame; the packet still goes
/// out, which keeps the A2DP link alive.
pub fn fill_packet(dst: &mut [u8]) {
    if !try_lock() {
        // Another context (a UI step or reset) owns the ring. Do not touch
        // State at all; emit one intentionally blank packet. The sink drops
        // or mutes it, and the next tick resumes with valid frames. This is
        // far rarer than losing the whole link to an idle disconnect.
        for b in dst.iter_mut() {
            *b = 0;
        }
        SILENCE.fetch_add(FRAMES_PER_PACKET as u32, Ordering::Relaxed);
        return;
    }
    let st = unsafe { &mut *STATE.0.get() };
    if st.ready && st.count < LOW_WATER {
        if st.pcm_len < st.frame_samples {
            fill_pcm(st);
        }
        while st.pcm_len >= st.frame_samples && st.count < RING_LEN {
            let mut frame = [0u8; SBC_FRAME_LEN];
            if !encode_frame(st, &mut frame) {
                break;
            }
            push_frame(st, &frame);
            SBC_FRAMES.fetch_add(1, Ordering::Relaxed);
        }
    }
    for k in 0..FRAMES_PER_PACKET {
        let off = k * SBC_FRAME_LEN;
        if !pop_frame(st, &mut dst[off..off + SBC_FRAME_LEN]) {
            SILENCE.fetch_add(1, Ordering::Relaxed);
        }
    }
    unlock();
}

/// Resets the pipeline for a fresh A2DP connection. Cheap: de-nibbles the
/// 8 KiB sample and encodes ONE silence frame; no bulk decoding here.
pub fn reset() {
    if !try_lock() {
        return;
    }
    DECODED.store(0, Ordering::Relaxed);
    SAMPLES.store(0, Ordering::Relaxed);
    SBC_FRAMES.store(0, Ordering::Relaxed);
    SILENCE.store(0, Ordering::Relaxed);
    PUMPS.store(0, Ordering::Relaxed);
    SBC_OK.store(0, Ordering::Relaxed);
    SBC_SYNC.store(0, Ordering::Relaxed);
    LAST_ERR.store(0, Ordering::Relaxed);

    let st = unsafe { &mut *STATE.0.get() };
    denibble(st);
    st.mp3_pos = 0;
    st.pcm_pos = 0;
    st.pcm_len = 0;
    st.head = 0;
    st.tail = 0;
    st.count = 0;
    st.ready = false;

    let params = SbcParameters::HIGH_QUALITY;
    st.frame_samples = params.frame_samples();
    MEASURED_FRAME_LEN.store(params.frame_length() as u32, Ordering::Relaxed);
    if params.frame_length() != SBC_FRAME_LEN
        || st.frame_samples == 0
        || st.frame_samples > MAX_SAMPLES_PER_FRAME
    {
        LAST_ERR.store(-2, Ordering::Relaxed);
        unlock();
        return;
    }
    st.dec.write(RawDecoder::new());
    match SbcEncoder::new(params) {
        Ok(encoder) => {
            st.enc.write(encoder);
        }
        Err(_) => {
            LAST_ERR.store(-1, Ordering::Relaxed);
            unlock();
            return;
        }
    }
    // Pre-encode one all-zero PCM frame for underrun / keep-alive.
    for x in st.pcm.iter_mut() {
        *x = 0;
    }
    st.pcm_pos = 0;
    st.pcm_len = st.frame_samples;
    let mut silence = [0u8; SBC_FRAME_LEN];
    if encode_frame(st, &mut silence) {
        st.silence.copy_from_slice(&silence);
        SBC_SYNC.store(silence[0] as u32, Ordering::Relaxed);
    }
    st.pcm_pos = 0;
    st.pcm_len = 0;
    st.ready = true;
    unlock();
}

/// UI helper: decode exactly one slice. Kept so the "one tap" latency can be
/// compared against 0.10.18, which decoded the whole clip in one tap.
pub fn step_once() {
    pump(1);
}
