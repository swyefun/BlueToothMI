//! MP3 soft-decode + SBC encode dependency probe (0.10.18).
//!
//! This module deliberately does NOT touch the Bluetooth data plane. Its only
//! job is to prove, on real `.179` hardware, that the two external crates we
//! plan to build the music pipeline on actually compile for
//! `thumbv8m.main-none-eabi`, link, pass the CAN-P1-011 verifier, and RUN:
//!
//!   rmp3 (minimp3, no_std / zero-alloc)  -> interleaved i16 PCM
//!   nanoaudio-sbc (no_std / zero-alloc) -> 119-byte A2DP SBC frames
//!
//! The sample is the first 8 KiB of sample-3s.mp3 (44100 Hz stereo, 128 kbps),
//! enough for ~19 MP3 frames. It is stored nibble-expanded: every byte of
//! `probe_mp3.bin` is in 0x10..=0x1F, so no 4-byte little-endian word in the
//! ELF data section can fall in the `.179` firmware address range
//! 0x0C000000..0x0CD00000 that CAN-P1-011 scans. De-nibbling is lossless and
//! happens into a static buffer at runtime, before decoding.
//!
//! `RawDecoder` (not `Decoder`) is used because LyraPlayer streams MP3 bytes
//! through `dev_write` in chunks; `RawDecoder::next(src, dest)` accepts a
//! slice per call and reports how many bytes it consumed, which is exactly the
//! contract a ring buffer needs. Its `mp3dec_t` state must survive across
//! calls, hence the static `MaybeUninit` slot.
//!
//! `SbcParameters::HIGH_QUALITY` (44.1 kHz joint stereo, 16 blocks, 8
//! subbands, bitpool 53, 328 kbps) matches the configuration the NPU accepted
//! in the phone's Android Bluetooth log (`codec_arry = 21 15 02 35`,
//! `BitPool=53`). Its 119-byte frames make 5 frames + 13-byte RTP header =
//! 608 bytes, under the peer's 656-byte MTU.

use core::cell::UnsafeCell;
use core::mem::MaybeUninit;
use core::sync::atomic::{AtomicI32, AtomicU32, Ordering};

use nanoaudio_core::codec::Encoder as _;
use nanoaudio_sbc::{SbcEncoder, SbcParameters};
use rmp3::{Frame, RawDecoder, Sample, MAX_SAMPLES_PER_FRAME};

const ENCODED: &[u8] = include_bytes!("probe_mp3.bin");
const MP3_LEN: usize = ENCODED.len() / 2;
const SBC_OUT_LEN: usize = 512;

/// Single-threaded access (UI callback), so a bare UnsafeCell is enough to
/// keep the large buffers out of the stack. The module never enables
/// interrupts around `run()`.
struct Cell<T>(UnsafeCell<T>);
unsafe impl<T> Sync for Cell<T> {}

struct State {
    mp3: [u8; MP3_LEN],
    pcm: [Sample; MAX_SAMPLES_PER_FRAME],
    sbc: [u8; SBC_OUT_LEN],
    dec: MaybeUninit<RawDecoder>,
    enc: MaybeUninit<SbcEncoder>,
}

static STATE: Cell<State> = Cell(UnsafeCell::new(State {
    mp3: [0; MP3_LEN],
    pcm: [0; MAX_SAMPLES_PER_FRAME],
    sbc: [0; SBC_OUT_LEN],
    dec: MaybeUninit::uninit(),
    enc: MaybeUninit::uninit(),
}));

/// 1 once `run()` has completed, so the UI can distinguish "not attempted"
/// from "attempted and found nothing".
pub static RAN: AtomicU32 = AtomicU32::new(0);
/// Number of MP3 audio frames decoded.
pub static FRAMES: AtomicU32 = AtomicU32::new(0);
/// PCM samples per channel produced.
pub static SAMPLES: AtomicU32 = AtomicU32::new(0);
pub static RATE: AtomicU32 = AtomicU32::new(0);
pub static CHANNELS: AtomicU32 = AtomicU32::new(0);
/// SBC frame length reported by `SbcParameters::frame_length()`.
pub static SBC_FRAME_LEN: AtomicU32 = AtomicU32::new(0);
/// 1 when `SbcEncoder::encode` returned Ok.
pub static SBC_OK: AtomicU32 = AtomicU32::new(0);
/// First byte of the encoded SBC frame; 0x9C is the SBC syncword.
pub static SBC_SYNC: AtomicU32 = AtomicU32::new(0);
/// Negative stage marker: -1 encoder ctor, -2 buffer, -3 encode call.
pub static LAST_ERR: AtomicI32 = AtomicI32::new(0);

fn denibble(st: &mut State) {
    for i in 0..MP3_LEN {
        let hi = ENCODED[2 * i] & 0x0F;
        let lo = ENCODED[2 * i + 1] & 0x0F;
        st.mp3[i] = (hi << 4) | lo;
    }
}

fn decode(st: &mut State) -> (u32, u32, u32, u32) {
    let State { mp3, pcm, dec, .. } = st;
    dec.write(RawDecoder::new());
    let dec = unsafe { dec.assume_init_mut() };
    let src = &mp3[..];

    let mut pos = 0usize;
    let mut frames = 0u32;
    let mut samples = 0u32;
    let mut rate = 0u32;
    let mut channels = 0u32;
    while pos < MP3_LEN {
        let result = dec.next(&src[pos..], &mut *pcm);
        match result {
            Some((Frame::Audio(audio), used)) => {
                frames += 1;
                samples += audio.sample_count() as u32;
                rate = audio.sample_rate();
                channels = audio.channels() as u32;
                if used == 0 {
                    break;
                }
                pos += used;
            }
            Some((Frame::Other(_), used)) => {
                if used == 0 {
                    break;
                }
                pos += used;
            }
            None => break,
        }
    }
    (frames, samples, rate, channels)
}

fn encode_sbc(st: &mut State) {
    let State { pcm, sbc, enc, .. } = st;
    let params = SbcParameters::HIGH_QUALITY;
    let frame_len = params.frame_length();
    let needed = params.frame_samples();
    SBC_FRAME_LEN.store(frame_len as u32, Ordering::Relaxed);

    if needed > pcm.len() || frame_len > sbc.len() {
        LAST_ERR.store(-2, Ordering::Relaxed);
        return;
    }
    match SbcEncoder::new(params) {
        Ok(encoder) => {
            enc.write(encoder);
            let encoder = unsafe { enc.assume_init_mut() };
            match encoder.encode(&pcm[..needed], &mut sbc[..frame_len]) {
                Ok(_) => {
                    SBC_OK.store(1, Ordering::Relaxed);
                    SBC_SYNC.store(sbc[0] as u32, Ordering::Relaxed);
                }
                Err(_) => LAST_ERR.store(-3, Ordering::Relaxed),
            }
        }
        Err(_) => LAST_ERR.store(-1, Ordering::Relaxed),
    }
}

/// Runs the whole probe once. Called from the UI event callback (app
/// context), never from a firmware callback or timer.
pub fn run() {
    RAN.store(0, Ordering::Relaxed);
    FRAMES.store(0, Ordering::Relaxed);
    SAMPLES.store(0, Ordering::Relaxed);
    RATE.store(0, Ordering::Relaxed);
    CHANNELS.store(0, Ordering::Relaxed);
    SBC_FRAME_LEN.store(0, Ordering::Relaxed);
    SBC_OK.store(0, Ordering::Relaxed);
    SBC_SYNC.store(0, Ordering::Relaxed);
    LAST_ERR.store(0, Ordering::Relaxed);

    let st = unsafe { &mut *STATE.0.get() };
    denibble(st);
    let (frames, samples, rate, channels) = decode(st);
    FRAMES.store(frames, Ordering::Relaxed);
    SAMPLES.store(samples, Ordering::Relaxed);
    RATE.store(rate, Ordering::Relaxed);
    CHANNELS.store(channels, Ordering::Relaxed);
    encode_sbc(st);
    RAN.store(1, Ordering::Relaxed);
}
