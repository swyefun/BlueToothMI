//! headset-link-179 — Xiaomi Band 11 (4.100.179) headset link front-end.
//!
//! # Scope — read before enabling this module
//!
//! This module implements the part of "connect a headset" that the `.179`
//! target-private backend actually exposes as statically recovered ABI:
//!
//!   adapter readiness -> discovery -> create bond -> register the A2DP source
//!   and AVRCP controller SDP records -> queue AVDTP/AVCTP L2CAP connect
//!   requests.
//!
//! As of 0.10.15 `CMD_A2DP_CONNECT` drives a minimal A2DP **Source**
//! handshake (see `avdtp`): AVDTP DISCOVER / GET_CAPABILITIES /
//! SET_CONFIGURATION / OPEN over PSM 0x19. That is enough for the headset to
//! announce "connected"; it is **not** an audio link. Still missing, and
//! deliberately not faked: START / SBC encoding / media packetisation, an
//! audio data source, and AVRCP command handling. The firmware ships only an
//! A2DP sink + HFP-AG profile, so this initiator side is ours.
//!
//! Discovery results ARE consumed as of this revision. `bt_adapter_register`
//! (0xc46a302, IDA) is append-only: the registration at adapter+120 is an
//! observer list (`sub_C466740` appends cookie+table nodes), so the module
//! appends its own 17-word table without displacing the stock one, and
//! `bt_adapter_unregister` removes it on teardown. Slots 0-2 receive adapter
//! state / discovery state / discovery results into static buffers; every
//! other slot points at a stub returning 0 so no slot is null. Slot 5 (pair
//! request) stays a stub: the stock table still answers pairing. UNVERIFIED,
//! only statically argued: dispatch order (broadcast vs first-handler), the
//! stub return-value contract, the 17-slot ordering (from the .139 ABI note),
//! the 24-byte `discovery_name` read bound, and whether the register
//! handshake (708-byte IPC, command 49, `sub_C46A18C`) is safe from the
//! caller's task context. See `NOTES-ida-179.md` §7. CMD_SET_PEER remains
//! the manual override.
//!
//! Control-event payloads are not freed (the connect request may be delivered
//! twice); event-7 DATA StockBuffers ARE released by the callback, which IDA
//! shows owns them (sub_C869764). This was the log16 heap-corruption fix.
//! Do not treat a queued request as a connected channel.
//!
//! IDA cross-check on the patched `band11std` `vela_ap.bin` (base 0x0C0C0000)
//! found a stronger constraint: `bt_l2cap_connect` (0xc86d468) only submits a
//! 20-byte work node; the handler `sub_C86F9B0` (0xc86f9b0) reads request+12
//! as the event callback, installs it at channel+20 via `sub_C86BD54`
//! (0xc86bd54), writes the CID back to request+0 and invokes the callback as
//! `(2, request)` (CONFIRM). When request+12 is zero the firmware falls back
//! to `sub_C867C80(PSM, ...)`, i.e. it expects an already-registered PSM
//! handler for AVDTP (0x19) / AVCTP (0x17) only when request+12 is zero.
//! `queue_connect` now installs the callback at +12 (0.10.5); the firmware
//! stores it at channel+20 and invokes it for CONFIRM(2) / COMPLETE(3) /
//! DATA(7), so no separate PSM handler is required. Boot residency is
//! declared through `lifecycle = "resident-after-activation"`. The legacy
//! CMD_CONNECT (PSM 1 ACL-hold test) remains unverified on hardware; the
//! A2DP path is the one under test. See `NOTES-ida-179.md` for addresses.
//!
//! All firmware calls are exact-target full-trust absolute addresses. `.179`
//! symbols remain restricted/PENDING with `capabilities = []`
//! (`targets/xiaomi-band-11-4.100.179/target.toml`); `AGENTS.md` forbids
//! promoting them. Nothing here has device proof. Activation fails closed on
//! an identity mismatch and the module stays idle until an explicit control
//! call — loading it alone touches no Bluetooth state.

#![no_std]

#[cfg(feature = "device")]
mod audio_dev;
#[cfg(feature = "device")]
mod avdtp;
#[cfg(feature = "device")]
mod media;
/// Sliced MP3 -> PCM -> SBC streaming pipeline feeding the AVDTP media
/// channel. Verified on-device by 0.10.18's dependency probe; 0.10.19 makes
/// it the real audio source and removes the synchronous full-clip decode.
mod stream;

use canopus_abi::{ABI_MAJOR, ABI_MINOR, ContextV1, ModuleDescriptorV1, StatusWriterV1};
use canopus_runtime::{status_put_u32, status_writer_publish};
use core::ffi::c_void;
use core::sync::atomic::{AtomicI32, AtomicPtr, AtomicU8, AtomicU32, Ordering};

pub const MAGIC: u32 = 0x484C_4E4B; // "HLNK"

pub const ST_IDLE: u32 = 0;
pub const ST_SCANNING: u32 = 1;
pub const ST_PEER_SET: u32 = 2;
pub const ST_BONDING: u32 = 3;
pub const ST_SDP_READY: u32 = 4;
pub const ST_CHANNELS_QUEUED: u32 = 5;
pub const ST_FAILED: u32 = 6;

/// Control commands for `canopus_headset_control`. `arg` is a 6-byte BDADDR
/// for CMD_SET_PEER, a 256-byte output buffer for CMD_SCAN_RESULTS, and
/// unused otherwise.
pub const CMD_SCAN_START: u32 = 1;
pub const CMD_SCAN_STOP: u32 = 2;
pub const CMD_SET_PEER: u32 = 3;
pub const CMD_PAIR: u32 = 4;
pub const CMD_REGISTER_SDP: u32 = 5;
pub const CMD_CONNECT: u32 = 6;
pub const CMD_TEARDOWN: u32 = 7;
/// Copies discovered peers into `arg`: 32 bytes per entry, address[6], rssi,
/// reserved, class_of_device[4], name[20], up to 8 entries. Returns the count.
pub const CMD_SCAN_RESULTS: u32 = 8;
/// Queries the adapter for the current peer's bond state and stores it in
/// BOND_STATE. Needed because already-bonded devices never emit a bond-state
/// change event, so the slot-9 callback alone always reads NONE.
pub const CMD_QUERY_BOND: u32 = 9;
/// Removes the current peer's bond (bt_remove_bond / device_request 95) so a
/// fresh create_bond can run the pairing dialog again.
pub const CMD_REMOVE_BOND: u32 = 10;
/// ACL-hold test: L2CAP to PSM 1 (SDP). bluetoothd's own handler serves that
/// PSM, so the callback-0 fallback in `bt_l2cap_connect` resolves instead of
/// null-dereferencing; holding the channel keeps the ACL link up.
pub const CMD_CONNECT_ACL: u32 = 11;
/// Originates the AVDTP signalling channel and drives DISCOVER ->
/// GET_CAPABILITIES -> SET_CONFIGURATION -> OPEN (see `avdtp`).
pub const CMD_A2DP_CONNECT: u32 = 12;
/// Decodes ONE MP3-frame-sized slice (see `stream`). No firmware calls.
pub const CMD_MP3_PROBE: u32 = 13;

// Service error codes from sdk/c/canopus_services.h; E_IDENTITY is local.
pub const E_IDENTITY: i32 = -1;
pub const E_STATE: i32 = -16;
pub const E_ARGUMENT: i32 = -22;
pub const E_IO: i32 = -5;
pub const E_NOMEM: i32 = -12;
pub const E_UNSUPPORTED: i32 = -38;

static STATE: AtomicU32 = AtomicU32::new(ST_IDLE);
static LAST_ERROR: AtomicI32 = AtomicI32::new(0);
/// Raw return value of the last `bt_discovery_start` call. Kept separately
/// from LAST_ERROR because this firmware entry is not device-validated: a
/// non-negative value can mean "already running" rather than success/failure.
static LAST_FW_RC: AtomicI32 = AtomicI32::new(0);
/// Raw return values of the last stop / bond requests. Both firmware entries
/// are unvalidated on hardware; surfacing the numbers lets us pin down their
/// real semantics instead of guessing whether a non-zero value is an error.
static LAST_STOP_RC: AtomicI32 = AtomicI32::new(0);
static LAST_PAIR_RC: AtomicI32 = AtomicI32::new(0);
static BOND_STATE: AtomicU32 = AtomicU32::new(0);
/// Peer Class of Device. IDA (4.100.179): the cmd83 IPC response stores the
/// 24-bit COD at offset 12 - bt_get_bond_state's return value is the COD,
/// NOT a bond-state enum (NPU=0x240404 Audio/Video, PC=0x2E410C Computer).
static BOND_COD: AtomicU32 = AtomicU32::new(0);
/// Slot-9 diagnostics: total callback invocations and the first four address
/// bytes (b0 | b1<<8 | b2<<16 | b3<<24) of the last event, packed for the
/// status row. Verifies whether the .179 firmware ever fires this slot.
static CB_COUNT: AtomicU32 = AtomicU32::new(0);
static CB_ADDR: AtomicU32 = AtomicU32::new(0);
/// Bond-restore diagnostics: entries enumerated by cmd45/46 and the first
/// entry's leading bytes EXACTLY as the list carries them (no reorder), so a
/// single glance at the peer row settles the list byte order on device.
static RESTORE_COUNT: AtomicU32 = AtomicU32::new(0);
static RESTORE_RAW2: AtomicU32 = AtomicU32::new(0);
/// Build marker rendered on the peer row, so a stale install is visible.
const BUILD_TAG: &str = "v40";
/// Peer SBC capability IE, written by `avdtp::parse_capabilities` and shown
/// on the audio-pipeline row. Declared at crate root so the page module can
/// read it in host builds where the `device` feature is off.
pub static CAP_WORD: AtomicU32 = AtomicU32::new(0);
/// IDA: the firmware's own headset_connect bridge (sub_C719878) accepts an
/// explicit profile_id 0..3 (atoi + CMP #3), while 255 is the "all profiles"
/// default. On the server, cmd62 case 39 -> sub_C471404: the non-255 branch
/// ALSO creates/updates a profile-manager node (sub_C87D074 / sub_C870BDC)
/// before posting the connect work; the 255 branch skips that step. Device
/// log 15 shows cmd62(255) built the ACL, paired and read NPU's UUIDs, then
/// the band dropped the link locally (reason 0x16) with no audio profile.
/// Rotate through the firmware-supported ids: one install identifies which
/// profile actually brings the audio link up.
const PROFILE_SEQ: [u8; 5] = [1, 3, 0, 2, 255];
static PROFILE_IDX: AtomicU32 = AtomicU32::new(0);
static LAST_PROFILE: AtomicU32 = AtomicU32::new(0);
static ADAPTER: AtomicPtr<c_void> = AtomicPtr::new(core::ptr::null_mut());
static SDP_SOURCE: AtomicU32 = AtomicU32::new(0);
static SDP_AVRCP: AtomicU32 = AtomicU32::new(0);
static AVDTP_NODE: AtomicPtr<c_void> = AtomicPtr::new(core::ptr::null_mut());
/// Queue node for the SECOND PSM 0x19 channel (AVDTP media transport).
static MEDIA_NODE: AtomicPtr<c_void> = AtomicPtr::new(core::ptr::null_mut());
static AVCTP_NODE: AtomicPtr<c_void> = AtomicPtr::new(core::ptr::null_mut());
static PEER: [AtomicU8; 6] = [const { AtomicU8::new(0) }; 6];

fn ok(state: u32) -> i32 {
    LAST_ERROR.store(0, Ordering::Relaxed);
    STATE.store(state, Ordering::Relaxed);
    0
}

fn fail(code: i32, firmware_rc: i32) -> i32 {
    LAST_ERROR.store(firmware_rc, Ordering::Relaxed);
    STATE.store(ST_FAILED, Ordering::Relaxed);
    code
}

fn peer_bytes() -> [u8; 6] {
    let mut out = [0u8; 6];
    for (i, byte) in out.iter_mut().enumerate() {
        *byte = PEER[i].load(Ordering::Relaxed);
    }
    out
}

fn set_peer(arg: *const u8) -> bool {
    if arg.is_null() {
        return false;
    }
    let bytes = unsafe { core::slice::from_raw_parts(arg, 6) };
    for (i, byte) in bytes.iter().enumerate() {
        PEER[i].store(*byte, Ordering::Relaxed);
    }
    true
}

fn has_peer(addr: &[u8; 6]) -> bool {
    addr.iter().any(|&b| b != 0)
}

#[cfg(feature = "device")]
mod device {
    use super::*;
    use crate::page::{put_num, put_str};
    use canopus_target_private::{
        ADAPTER_STATE_ON, CLASSIC_TRANSPORT, CONNECT_ADDRESS_OFFSET, CONNECT_CALLBACK_OFFSET,
        CONNECT_CONFIG_OFFSET, CONNECT_OPTION_LOCAL_MTU, CONNECT_OPTIONS_OFFSET,
        CONNECT_PSM_OFFSET, CONNECT_REQUEST_SIZE, DISCOVERY_TIMEOUT_SECONDS, DiscoveryResult,
        SdpAvrcpControllerRecord, SdpSourceRecord, AVDTP_LOCAL_RX_MTU, bt_adapter_get_instance,
        bt_adapter_get_state, bt_adapter_register, bt_adapter_unregister, bt_alloc,
        bt_create_bond, bt_discovery_start, bt_discovery_stop, bt_get_bond_state,
        bt_get_pairing_state,
        bt_l2cap_connect, bt_remove_bond, configure_avctp_connect_request,
        configure_avdtp_connect_request, discovery_name, sdp_builder_create, sdp_commit,
        sdp_set_raw_attribute, sdp_unregister,
    };

    /// One discovered peer: 32 bytes — address[6], rssi, reserved,
    /// class_of_device[4], name[20]. Up to [`FOUND_MAX`] entries.
    pub const FOUND_STRIDE: usize = 32;
    pub const FOUND_MAX: usize = 8;
    /// Thumb callable bit, matching `canopus_thumb_callable` in the generated
    /// bindings; the firmware reads table words and dispatches via BLX.
    const THUMB: u32 = 1;

    pub static REGISTRATION: AtomicU32 = AtomicU32::new(0);
    /// 0xFFFFFFFF mirrors CANOPUS_BT_ADAPTER_UNKNOWN from canopus_services.h.
    pub static ADAPTER_STATE: AtomicU32 = AtomicU32::new(0xFFFF_FFFF);
    pub static DISCOVERY_STATE: AtomicU32 = AtomicU32::new(0xFFFF_FFFF);
    pub static FOUND_LEN: AtomicU32 = AtomicU32::new(0);
    static FOUND: [AtomicU8; FOUND_MAX * FOUND_STRIDE] =
        [const { AtomicU8::new(0) }; FOUND_MAX * FOUND_STRIDE];

    unsafe extern "C" fn on_adapter_state(_cookie: *mut c_void, state: i32) {
        ADAPTER_STATE.store(state as u32, Ordering::Relaxed);
    }

    unsafe extern "C" fn on_discovery_state(_cookie: *mut c_void, state: i32) {
        DISCOVERY_STATE.store(state as u32, Ordering::Relaxed);
    }

    /// Runs in the firmware's Bluetooth task context: writes to statics only,
    /// no firmware calls, no allocation, no locking.
    unsafe extern "C" fn on_discovery_result(_cookie: *mut c_void, result: *const DiscoveryResult) {
        if result.is_null() {
            return;
        }
        let r = unsafe { &*result };
        // capacity 24 = 12-byte header + up to 12 name bytes; the true size of
        // the firmware-allocated buffer is unknown, so this bound is a guess.
        let name = unsafe { discovery_name(result, 24) };
        let len = (FOUND_LEN.load(Ordering::Relaxed) as usize).min(FOUND_MAX);
        let mut slot = len;
        for i in 0..len {
            let base = i * FOUND_STRIDE;
            let same = (0..6).all(|k| FOUND[base + k].load(Ordering::Relaxed) == r.address[k]);
            if same {
                slot = i;
                break;
            }
        }
        if slot == len {
            if len == FOUND_MAX {
                return;
            }
            FOUND_LEN.store((len + 1) as u32, Ordering::Relaxed);
        }
        let base = slot * FOUND_STRIDE;
        for k in 0..6 {
            FOUND[base + k].store(r.address[k], Ordering::Relaxed);
        }
        FOUND[base + 6].store(r.rssi as u8, Ordering::Relaxed);
        FOUND[base + 7].store(0, Ordering::Relaxed);
        for k in 0..4 {
            FOUND[base + 8 + k].store(r.class_of_device.to_le_bytes()[k], Ordering::Relaxed);
        }
        let n = name.len().min(20);
        for k in 0..n {
            FOUND[base + 12 + k].store(name[k], Ordering::Relaxed);
        }
        for k in n..20 {
            FOUND[base + 12 + k].store(0, Ordering::Relaxed);
        }
    }

    /// Bond state changes arrive asynchronously on the firmware BT task.
    /// Slot 9 used to be a stub, so BOND_STATE was only ever the snapshot
    /// taken right after create_bond - always NONE at that point.
    unsafe extern "C" fn on_bond_state(
        _cookie: *mut c_void,
        address: *const u8,
        r2: i32,
        r3: i32,
    ) {
        if address.is_null() {
            return;
        }
        // Device-proven ABI (0.9.3 diagnostics): the firmware hands the
        // address in OTA REVERSED byte order (NPU 1B:01:73:5E:07:BB arrived
        // as BB:07:5E:73:01:1B), r3 carries the bond state (a BOND_NONE
        // removal surfaced as state 0). r2 follows the cmd83 layout (COD).
        let incoming = unsafe { core::slice::from_raw_parts(address, 6) };
        let packed = (incoming[0] as u32)
            | (incoming[1] as u32) << 8
            | (incoming[2] as u32) << 16
            | (incoming[3] as u32) << 24;
        CB_ADDR.store(packed, Ordering::Relaxed);
        CB_COUNT.fetch_add(1, Ordering::Relaxed);
        // Reversed-order peer match: only the selected peer drives the page,
        // so a stray bond event from the phone/laptop cannot fake "paired".
        // Iterator zip (no alloc): incoming[i] == peer[5-i] for all six bytes.
        let peer = peer_bytes();
        if has_peer(&peer)
            && incoming
                .iter()
                .zip(peer.iter().rev())
                .all(|(a, b)| a == b)
        {
            BOND_COD.store(r2 as u32, Ordering::Relaxed);
            BOND_STATE.store(r3 as u32, Ordering::Relaxed);
        }
    }

    /// Placeholder for slots this module does not act on. Declared with two
    /// parameters: extra AAPCS register arguments are ignored, r0 = 0.
    unsafe extern "C" fn stub(_cookie: *mut c_void, _a: i32) -> i32 {
        0
    }

    /// 17-word adapter table. Slot order per `CALLBACK_ADAPTER_STATE` /
    /// `CALLBACK_DISCOVERY_STATE` / `CALLBACK_DISCOVERY_RESULT` /
    /// `CALLBACK_PAIR_REQUEST` / `CALLBACK_PAIR_DISPLAY` (0,1,2,5,6).
    /// Fn-pointer-to-integer casts are rejected by const eval, so the words
    /// are filled at runtime by [`fill_callback_table`] right before
    /// `bt_adapter_register` is handed the buffer.
    static mut CALLBACK_TABLE: [u32; 17] = [0; 17];
    static TABLE_READY: AtomicU32 = AtomicU32::new(0);

    /// Fills the adapter callback table with Thumb-callable words. The
    /// supervisor serves module calls from a single task, so this write races
    /// with nothing.
    fn fill_callback_table() {
        let table = unsafe { &mut *core::ptr::addr_of_mut!(CALLBACK_TABLE) };
        table[0] = on_adapter_state as usize as u32 | THUMB;
        table[1] = on_discovery_state as usize as u32 | THUMB;
        table[2] = on_discovery_result as usize as u32 | THUMB;
        let stub_word = stub as usize as u32 | THUMB;
        for slot in table[3..17].iter_mut() {
            *slot = stub_word;
        }
        // CALLBACK_BOND_STATE = 9 in the .179 ABI; install the real handler
        // after the stub sweep so asynchronous bond events reach BOND_STATE.
        table[9] = on_bond_state as usize as u32 | THUMB;
        TABLE_READY.store(1, Ordering::Relaxed);
    }

    pub fn guard_ok() -> bool {
        canopus_target_private::canopus_identity_guard() == 0
    }

    /// Registers the `/dev/canopus_audio` character device (LyraPlayer
    /// compatibility layer). Call once per activation.
    pub fn audio_init() -> i32 {
        audio_dev::init()
    }

    /// One-line audio-device diagnostic for the status page:
    /// `reg<rc> open<n> st<s> acc<b> mono<m> bad<c>`.
    pub fn audio_diag(buf: &mut [u8]) -> u32 {
        let mut n = put_str(buf, "reg") as usize;
        n += put_num(&mut buf[n..], audio_dev::reg_rc()) as usize;
        n += put_str(&mut buf[n..], " open") as usize;
        n += put_num(&mut buf[n..], audio_dev::open_count() as i32) as usize;
        n += put_str(&mut buf[n..], " st") as usize;
        n += put_num(&mut buf[n..], audio_dev::state() as i32) as usize;
        n += put_str(&mut buf[n..], " acc") as usize;
        n += put_num(&mut buf[n..], audio_dev::bytes_accepted() as i32) as usize;
        n += put_str(&mut buf[n..], " mono") as usize;
        n += put_num(&mut buf[n..], audio_dev::mono_ok() as i32) as usize;
        n += put_str(&mut buf[n..], " bad") as usize;
        n += put_num(&mut buf[n..], audio_dev::bad_ioctl() as i32) as usize;
        n as u32
    }

    pub fn control(cmd: u32, arg: *const u8) -> i32 {
        if !guard_ok() {
            return fail(E_IDENTITY, -1);
        }
        match cmd {
            CMD_SCAN_START => scan_start(),
            CMD_SCAN_STOP => scan_stop(),
            CMD_SET_PEER => {
                if !set_peer(arg) {
                    fail(E_ARGUMENT, 0)
                } else {
                    // New peer: stale COD/bond info belongs to the old one.
                    BOND_COD.store(0, Ordering::Relaxed);
                    BOND_STATE.store(0, Ordering::Relaxed);
                    ok(ST_PEER_SET)
                }
            }
            CMD_PAIR => pair(),
            CMD_QUERY_BOND => query_bond(),
            CMD_REMOVE_BOND => remove_bond(),
            CMD_REGISTER_SDP => register_sdp(),
            CMD_CONNECT => connect(),
            CMD_CONNECT_ACL => device::connect_profiles(),
            CMD_A2DP_CONNECT => device::a2dp_connect(),
            CMD_MP3_PROBE => {
                crate::stream::step_once();
                0
            }
            CMD_TEARDOWN => teardown(false),
            CMD_SCAN_RESULTS => scan_results(arg),
            _ => fail(E_ARGUMENT, 0),
        }
    }

    fn scan_start() -> i32 {
        // The UI host can deliver both ACTIVATE and VALUE_CHANGED for one tap.
        // Re-issuing discovery while it runs makes the firmware return a
        // non-zero "already in progress" status; stay idempotent instead.
        if STATE.load(Ordering::Relaxed) == ST_SCANNING {
            return ok(ST_SCANNING);
        }
        let adapter = unsafe { bt_adapter_get_instance() };
        if adapter.is_null() {
            return fail(E_STATE, 0);
        }
        let state = unsafe { bt_adapter_get_state(adapter) };
        if state != ADAPTER_STATE_ON {
            return fail(E_STATE, state);
        }
        ADAPTER.store(adapter, Ordering::Relaxed);
        // Append-only registration (IDA-verified observer list at adapter+120);
        // the stock table keeps receiving events. One registration per load.
        if REGISTRATION.load(Ordering::Relaxed) == 0 {
            if TABLE_READY.load(Ordering::Relaxed) == 0 {
                fill_callback_table();
            }
            let table = core::ptr::addr_of!(CALLBACK_TABLE);
            let reg = unsafe { bt_adapter_register(adapter, table.cast()) };
            if reg == 0 {
                return fail(E_IO, 0);
            }
            REGISTRATION.store(reg, Ordering::Relaxed);
        }
        FOUND_LEN.store(0, Ordering::Relaxed);
        let rc = unsafe { bt_discovery_start(adapter, DISCOVERY_TIMEOUT_SECONDS) };
        LAST_FW_RC.store(rc, Ordering::Relaxed);
        // This entry is STATIC_RECOVERED / device not_probed. Observed on the
        // band: a non-zero rc still starts discovery and the result callback
        // keeps delivering peers. Only a negative errno is treated as fatal;
        // the raw value is surfaced on the page for further diagnosis.
        if rc < 0 {
            return fail(E_IO, rc);
        }
        ok(ST_SCANNING)
    }

    /// Copies found peers into `arg` (needs 256 bytes); returns the entry count.
    fn scan_results(arg: *const u8) -> i32 {
        if arg.is_null() {
            return fail(E_ARGUMENT, 0);
        }
        let len = (FOUND_LEN.load(Ordering::Relaxed) as usize).min(FOUND_MAX);
        let out = unsafe { core::slice::from_raw_parts_mut(arg.cast_mut(), len * FOUND_STRIDE) };
        for i in 0..len {
            for k in 0..FOUND_STRIDE {
                out[i * FOUND_STRIDE + k] = FOUND[i * FOUND_STRIDE + k].load(Ordering::Relaxed);
            }
        }
        len as i32
    }

    fn scan_stop() -> i32 {
        // Discovery ends by itself after DISCOVERY_TIMEOUT_SECONDS, and the UI
        // host can deliver ACTIVATE + VALUE_CHANGED for a single tap. Both make
        // us call stop when nothing is running; the firmware then returns a
        // non-zero "no inquiry in progress" status. bt_discovery_stop is
        // STATIC_RECOVERED / device not_probed, so only a negative errno is
        // treated as fatal.
        if STATE.load(Ordering::Relaxed) == ST_IDLE {
            return ok(ST_IDLE);
        }
        let adapter = ADAPTER.load(Ordering::Relaxed);
        if adapter.is_null() {
            return ok(ST_IDLE);
        }
        // Exactly one IPC request. IDA: sub_C46A18C parks the request buffer
        // at adapter+72 until the response arrives, so back-to-back stop
        // calls (v0.7 retried four times) can overwrite an in-flight slot.
        // Restore the single-call behaviour of 0.3/0.4, the only build that
        // ever produced a pairing dialog on the band.
        let rc = unsafe { bt_discovery_stop(adapter) };
        LAST_STOP_RC.store(rc, Ordering::Relaxed);
        // Always drop back to idle, even on a firmware error: a stale
        // ST_FAILED would keep the page red and block the next scan.
        STATE.store(ST_IDLE, Ordering::Relaxed);
        if rc < 0 {
            LAST_ERROR.store(rc, Ordering::Relaxed);
            return E_IO;
        }
        ok(ST_IDLE)
    }

    /// Polls the pairing state via device_request(92) - `bt_get_pairing_state`.
    /// The slot-9 callback never fires for BONDING/BONDED (0.9.5 device log:
    /// a full successful pairing with cb count unchanged), and cmd83 returns
    /// the remote COD regardless of bond state (0/2360324 survived an
    /// unpair), so polling is the only working path on .179.
    fn query_bond() -> i32 {
        let addr = peer_bytes();
        if !has_peer(&addr) {
            BOND_STATE.store(0, Ordering::Relaxed);
            return ok(STATE.load(Ordering::Relaxed));
        }
        let st = unsafe { bt_get_pairing_state(addr.as_ptr(), CLASSIC_TRANSPORT) };
        BOND_STATE.store(st as u32, Ordering::Relaxed);
        ok(STATE.load(Ordering::Relaxed))
    }

    /// Clears the current peer's bond so create_bond can run the pairing
    /// dialog again (useful when the peer is already in the bonded table).
    fn remove_bond() -> i32 {
        let addr = peer_bytes();
        if !has_peer(&addr) {
            return fail(E_ARGUMENT, 0);
        }
        // Drop the peer's profiles/ACL first (IPC cmd 63): removing a bond
        // while the peer is still ACL-connected leaves a stale link whose
        // re-pairing stalls forever (log 11: laptop BONDING with no SSP).
        // The server ignores disconnect when the connected flag is clear.
        unsafe {
            adapter_ipc(63, &addr);
        }
        let rc = unsafe { bt_remove_bond(addr.as_ptr(), CLASSIC_TRANSPORT) };
        LAST_PAIR_RC.store(rc, Ordering::Relaxed);
        BOND_STATE.store(0, Ordering::Relaxed);
        BOND_COD.store(0, Ordering::Relaxed);
        ok(ST_IDLE)
    }

    fn pair() -> i32 {
        let addr = peer_bytes();
        if !has_peer(&addr) || ADAPTER.load(Ordering::Relaxed).is_null() {
            return fail(E_ARGUMENT, 0);
        }
        // Back to the only build that ever produced a pairing dialog (0.3/0.4):
        // ONE create_bond, no preceding discovery_stop, no retry, and no
        // get_bond_state immediately afterwards.
        //
        // IDA: sub_C46A18C parks the 708-byte request at adapter+72 until its
        // response arrives - a single in-flight slot. v0.5+ chained stop ->
        // create_bond -> get_bond_state (v0.7 retried each), which can clobber
        // that slot. Bond results now arrive asynchronously via on_bond_state
        // (callback slot 9), so no follow-up IPC is needed here.
        let rc = unsafe { bt_create_bond(addr.as_ptr(), CLASSIC_TRANSPORT) };
        LAST_PAIR_RC.store(rc, Ordering::Relaxed);
        // rc is a response status word, not an errno; never turn it into a
        // hard failure. The raw number stays on the page for diagnosis.
        ok(ST_BONDING)
    }

    /// Builds one SDP record from the backend's already-audited attribute
    /// bytes. `prefix = 0` matches `scripts/tests/band11_private_backend.rs`
    /// (`test_sdp_attr`); `sdp_commit` transfers builder ownership to the
    /// firmware registry, so it is not freed here.
    unsafe fn commit_record(
        uuid: u16,
        version: u16,
        name: &[u8],
        attributes: &[(u16, &'static [u8])],
    ) -> u32 {
        let builder = unsafe { sdp_builder_create(0, uuid, version, 0, name.as_ptr()) };
        if builder.is_null() {
            return 0;
        }
        for &(id, value) in attributes {
            let written = unsafe {
                sdp_set_raw_attribute(
                    builder,
                    id,
                    0,
                    value.len() as u16,
                    value.as_ptr().cast::<c_void>(),
                )
            };
            // A failed attribute means the builder is incomplete. Ownership
            // on this path is not covered by the static evidence, so it is
            // explicitly left to the firmware rather than freed speculatively.
            if written.is_null() {
                return 0;
            }
        }
        unsafe { sdp_commit(builder) }
    }

    fn register_sdp() -> i32 {
        let source = unsafe {
            commit_record(
                SdpSourceRecord::SERVICE_UUID,
                SdpSourceRecord::PROFILE_VERSION,
                SdpSourceRecord::SERVICE_NAME,
                &SdpSourceRecord::ATTRIBUTES,
            )
        };
        if source == 0 {
            return fail(E_NOMEM, 0);
        }
        SDP_SOURCE.store(source, Ordering::Relaxed);

        let avrcp = unsafe {
            commit_record(
                SdpAvrcpControllerRecord::SERVICE_UUID,
                SdpAvrcpControllerRecord::PROFILE_VERSION,
                SdpAvrcpControllerRecord::SERVICE_NAME,
                &SdpAvrcpControllerRecord::ATTRIBUTES,
            )
        };
        if avrcp == 0 {
            LAST_ERROR.store(unsafe { sdp_unregister(source) }, Ordering::Relaxed);
            SDP_SOURCE.store(0, Ordering::Relaxed);
            return fail(E_NOMEM, 0);
        }
        SDP_AVRCP.store(avrcp, Ordering::Relaxed);
        ok(ST_SDP_READY)
    }

    /// L2CAP connect completion evidence (callback installed at request+12).
    pub static L2CAP_EVENTS: AtomicU32 = AtomicU32::new(0);
    pub static L2CAP_EVENT: AtomicI32 = AtomicI32::new(0);
    pub static L2CAP_CID: AtomicU32 = AtomicU32::new(0);

    /// Completion callback the firmware invokes as `cb(event, request)`.
    /// Control-event payloads (2/3/6/13) are intentionally not freed: the
    /// same connect request can be delivered from more than one dispatch site
    /// (sub_C86FA54 event 13, sub_C86F9B0 event 2), so freeing it twice
    /// corrupts the heap. Event 7 DATA payloads ARE owned by the callback
    /// (sub_C869764 clears its pointer without freeing) and are released in
    /// `avdtp::handle_event` - see the log16 mm_free.c:173 analysis.
    unsafe extern "C" fn on_l2cap_event(event: i32, request: *mut u8) -> i32 {
        if request.is_null() {
            return 0;
        }
        let cid = unsafe { core::ptr::read_unaligned(request.cast::<u16>()) };
        L2CAP_EVENT.store(event, Ordering::Relaxed);
        L2CAP_CID.store(cid as u32, Ordering::Relaxed);
        L2CAP_EVENTS.fetch_add(1, Ordering::Relaxed);
        unsafe { crate::avdtp::handle_event(event, request) }
    }

    fn signalling_cb() -> u32 {
        on_l2cap_event as *const () as usize as u32 | THUMB
    }

    /// Queues the AVDTP media (transport) channel after OPEN. It uses the
    /// same PSM 0x19 request layout but installs `media::on_media_event`, so
    /// media-channel CONFIRM/COMPLETE/DATA events never disturb the
    /// signalling state machine's CID.
    pub fn open_media_channel() -> i32 {
        let addr = peer_bytes();
        if !has_peer(&addr) {
            return fail(E_ARGUMENT, 0);
        }
        let rc = unsafe {
            queue_connect(
                configure_avdtp_connect_request,
                &addr,
                &MEDIA_NODE,
                crate::media::callback(),
            )
        };
        if rc != 0 {
            fail(rc, 0)
        } else {
            0
        }
    }

    /// Queues one 68-byte L2CAP connect request with the completion callback
    /// installed. `bt_l2cap_connect` returns the accepted queue node, 0 when
    /// rejected.
    unsafe fn queue_connect(
        configure: unsafe fn(*mut u8),
        addr: &[u8; 6],
        node: &AtomicPtr<c_void>,
        callback: u32,
    ) -> i32 {
        let raw = unsafe { bt_alloc(CONNECT_REQUEST_SIZE as u32) };
        if raw.is_null() {
            return E_NOMEM;
        }
        let request = raw.cast::<u8>();
        unsafe {
            core::ptr::write_bytes(request, 0, CONNECT_REQUEST_SIZE);
            core::ptr::copy_nonoverlapping(
                addr.as_ptr(),
                request.add(CONNECT_ADDRESS_OFFSET),
                addr.len(),
            );
            configure(request);
            // A zero callback word makes the firmware silently drop the work
            // node (0.10.4: no page ever went out). Install the completion
            // callback so the connect actually proceeds; the firmware invokes
            // it as cb(event=2 CONFIRM, request) with the CID at request+0.
            core::ptr::write_unaligned(
                request.add(CONNECT_CALLBACK_OFFSET).cast::<u32>(),
                callback,
            );
        }
        let queued = unsafe { bt_l2cap_connect(request.cast()) };
        node.store(queued as usize as *mut c_void, Ordering::Relaxed);
        if queued == 0 {
            E_IO
        } else {
            0
        }
    }

    fn connect() -> i32 {
        let addr = peer_bytes();
        if !has_peer(&addr) {
            return fail(E_ARGUMENT, 0);
        }
        // Bond completion is asynchronous and reported through the adapter
        // callback table, which this module does not replace. BOND_STATE is
        // only the snapshot taken at CMD_PAIR and is diagnostic output in
        // `query`, never a gate for queueing a connection.
        if SDP_SOURCE.load(Ordering::Relaxed) == 0 {
            return fail(E_STATE, 0);
        }
        let avdtp = unsafe { queue_connect(configure_avdtp_connect_request, &addr, &AVDTP_NODE, signalling_cb()) };
        if avdtp != 0 {
            return fail(avdtp, 0);
        }
        let avctp = unsafe { queue_connect(configure_avctp_connect_request, &addr, &AVCTP_NODE, signalling_cb()) };
        if avctp != 0 {
            return fail(avctp, 0);
        }
        ok(ST_CHANNELS_QUEUED)
    }

    /// Queues AVDTP (PSM 0x19) + AVCTP (PSM 0x17) signaling connects to the
    /// current peer, both with the completion callback installed. Profile-level
    /// signalling is what makes audio peers announce "connected"; the PSM-1
    /// SDP test (0.10.5) never produced one. Parks in ST_CHANNELS_QUEUED.
    /// Raw adapter IPC envelope - the same 708-byte sendrecv the SDK's
    /// private adapter_request uses, embedded here because the SDK helper and
    /// its call! macro are crate-internal. Sendrecv thumb callable 0xc46a18d,
    /// built from non-address operands for the ELF verifier.
    const SENDRECV_LO: u32 = 0x0A46_A18D;
    const SENDRECV_HI: u32 = 0x0600_0000; // xor -> 0x0C46A18D

    /// cmd 62 = connect: payload {addr[6], profile_id}, 255 = all profiles.
    /// cmd 63 = disconnect: payload {addr[6]}. Server dispatcher sub_C90D90C
    /// (IDA, case cmd-23=39/40): per-device node flags at +288 bit3 mean
    /// "connected" - connect requires it clear, disconnect requires it set.
    unsafe fn adapter_ipc(command: u32, payload: &[u8]) -> i32 {
        let mut none = [0u8; 0];
        unsafe { adapter_ipc_resp(command, payload, &mut none) }
    }

    /// Adapter IPC with a response-payload view. The dispatcher writes the
    /// status byte to envelope offset 12 and command results into the payload
    /// area (offset 60 onward), e.g. cmd 45/46 write the bonded-device count
    /// at payload[0..4] followed by 6-byte address records.
    unsafe fn adapter_ipc_resp(command: u32, payload: &[u8], resp: &mut [u8]) -> i32 {
        let adapter = unsafe { bt_adapter_get_instance() };
        if adapter.is_null() || payload.len() > 648 {
            return 7;
        }
        let mut request = [0u32; 177]; // 708-byte envelope
        let bytes = unsafe {
            core::slice::from_raw_parts_mut(request.as_mut_ptr().cast::<u8>(), 708)
        };
        bytes[60..60 + payload.len()].copy_from_slice(payload);
        let f: unsafe extern "C" fn(*mut c_void, *mut u32, u32) -> i32 =
            core::mem::transmute(SENDRECV_LO ^ SENDRECV_HI);
        let rc = unsafe { f(adapter, request.as_mut_ptr(), command) };
        if rc != 0 {
            return rc;
        }
        let n = resp.len().min(648);
        resp[..n].copy_from_slice(&bytes[60..60 + n]);
        (request[3] & 255) as i32 // status byte at response offset 12
    }

    /// Lists bonded devices via the adapter's own enumeration (dispatcher
    /// cases 22/23 = IPC cmd 45/46 -> sub_C8AA7B4). The list selector lives
    /// at envelope offset 196; the reply carries a u32 count at payload[0]
    /// followed by 6-byte addresses taken from each device node at +130.
    /// Tries cmd 46 selector 0/1 then cmd 45 selector 0/1.
    fn bonded_list(out: &mut [[u8; 6]; 16]) -> usize {
        let mut resp = [0u8; 200];
        for (cmd, sel) in [(46u32, 0u8), (46, 1), (45, 0), (45, 1)] {
            let mut payload = [0u8; 137];
            payload[136] = sel;
            resp.fill(0);
            let rc = unsafe { adapter_ipc_resp(cmd, &payload, &mut resp) };
            if rc != 0 {
                continue;
            }
            let count = u32::from_le_bytes([resp[0], resp[1], resp[2], resp[3]]) as usize;
            let count = count.min(out.len()).min((resp.len() - 4) / 6);
            if count > 0 {
                for i in 0..count {
                    out[i].copy_from_slice(&resp[4 + 6 * i..4 + 6 * i + 6]);
                }
                return count;
            }
        }
        0
    }

    /// Restores PEER from the firmware's bond table without scanning: prefer
    /// an audio/video device (COD major 4), else the first non-phone entry.
    /// The cmd45/46 list carries OTA-reversed addresses (same byte order as
    /// the slot-9 callback); PEER and every adapter IPC use display order, so
    /// each entry is reversed first. Returns true when PEER was set.
    /// Restores PEER from the firmware's bond table without scanning.
    /// IDA proof: the firmware's own cmd62 client (sub_C719878) converts a
    /// display string via sub_C465538, which writes hex pairs backwards -
    /// so every adapter IPC address is OTA order. The cmd45/46 list is the
    /// same order; keep bytes verbatim for PEER (and thus cmd62/63/83/92),
    /// and only flip for display in addr_text. Classify by COD to prefer
    /// an audio device. Returns true when PEER was set.
    pub fn restore_peer() -> bool {
        let mut addrs = [[0u8; 6]; 16];
        let count = bonded_list(&mut addrs);
        RESTORE_COUNT.store(count as u32, Ordering::Relaxed);
        if count > 0 {
            let raw = (addrs[0][0] as u32) | ((addrs[0][1] as u32) << 8);
            RESTORE_RAW2.store(raw, Ordering::Relaxed);
        }
        let mut fallback: Option<[u8; 6]> = None;
        for i in 0..count {
            let addr = addrs[i];
            let cod = unsafe { bt_get_bond_state(addr.as_ptr()) } as u32;
            let major = (cod >> 8) & 0x1F;
            if major == 4 {
                for (j, b) in addr.iter().enumerate() {
                    PEER[j].store(*b, Ordering::Relaxed);
                }
                return true;
            }
            if major != 2 && fallback.is_none() {
                fallback = Some(addr);
            }
        }
        if let Some(a) = fallback {
            for (j, b) in a.iter().enumerate() {
                PEER[j].store(*b, Ordering::Relaxed);
            }
            return true;
        }
        false
    }

    /// Establishes the AVDTP signalling channel and drives the A2DP Source
    /// handshake. IDA: `bt_l2cap_connect` -> sub_C86F9B0 -> sub_C86BC18
    /// originates the ACL itself, so cmd 62's pairing round-trip (which the
    /// firmware tears down after 2s with reason 0x16) is not needed here.
    pub fn a2dp_connect() -> i32 {
        let addr = peer_bytes();
        if !has_peer(&addr) && !restore_peer() {
            return fail(E_ARGUMENT, 0);
        }
        let addr = peer_bytes();
        if SDP_SOURCE.load(Ordering::Relaxed) == 0 {
            let rc = register_sdp();
            if SDP_SOURCE.load(Ordering::Relaxed) == 0 {
                return rc;
            }
        }
        crate::avdtp::reset();
        crate::media::reset();
        // Cheap: de-nibbles the sample and encodes one silence frame. No
        // bulk decoding in the UI callback - slices come later from the
        // signalling round trips and the media timer.
        crate::stream::reset();
        let rc = unsafe { queue_connect(configure_avdtp_connect_request, &addr, &AVDTP_NODE, signalling_cb()) };
        if rc != 0 {
            return fail(rc, 0);
        }
        ok(ST_CHANNELS_QUEUED)
    }

    /// Asks the adapter service to connect ALL of the peer's profiles
    /// (profile_id 255). This is the firmware's own connect path - the same
    /// one its headset service drives. When PEER is unset (fresh install),
    /// it is restored from the bond table first, so the flow works without
    /// a prior scan.
    pub fn connect_profiles() -> i32 {
        let addr = peer_bytes();
        if !has_peer(&addr) && !restore_peer() {
            return fail(E_ARGUMENT, 0);
        }
        let addr = peer_bytes();
        let idx = PROFILE_IDX.fetch_add(1, Ordering::Relaxed) % PROFILE_SEQ.len() as u32;
        let pid = PROFILE_SEQ[idx as usize];
        LAST_PROFILE.store(pid as u32, Ordering::Relaxed);
        let mut payload = [0u8; 7];
        payload[..6].copy_from_slice(&addr);
        payload[6] = pid;
        let rc = unsafe { adapter_ipc(62, &payload) };
        LAST_PAIR_RC.store(rc, Ordering::Relaxed);
        if rc != 0 {
            return fail(rc, 0);
        }
        ok(ST_CHANNELS_QUEUED)
    }

    /// `unregister_callbacks`: true on module deactivate/stop (the callback
    /// table would dangle once the code is unloaded), false on the UI "复位"
    /// button - unregistering there deafened the page for the rest of the
    /// session (0.9.3 log: bond events only reached the stock table until
    /// the next scan_start re-registration).
    pub fn teardown(unregister_callbacks: bool) -> i32 {
        let source = SDP_SOURCE.swap(0, Ordering::Relaxed);
        if source != 0 {
            LAST_ERROR.store(unsafe { sdp_unregister(source) }, Ordering::Relaxed);
        }
        let avrcp = SDP_AVRCP.swap(0, Ordering::Relaxed);
        if avrcp != 0 {
            LAST_ERROR.store(unsafe { sdp_unregister(avrcp) }, Ordering::Relaxed);
        }
        let adapter = ADAPTER.load(Ordering::Relaxed);
        if !adapter.is_null() {
            unsafe { bt_discovery_stop(adapter) };
        }
        if unregister_callbacks {
            let reg = REGISTRATION.swap(0, Ordering::Relaxed);
            if reg != 0 && !adapter.is_null() {
                LAST_ERROR.store(unsafe { bt_adapter_unregister(adapter, reg) }, Ordering::Relaxed);
            }
        }
        FOUND_LEN.store(0, Ordering::Relaxed);
        AVDTP_NODE.store(core::ptr::null_mut(), Ordering::Relaxed);
        AVCTP_NODE.store(core::ptr::null_mut(), Ordering::Relaxed);
        BOND_STATE.store(0, Ordering::Relaxed);
        BOND_COD.store(0, Ordering::Relaxed);
        STATE.store(ST_IDLE, Ordering::Relaxed);
        0
    }
}

#[cfg(feature = "device")]
fn identity_ok() -> bool {
    device::guard_ok()
}

#[cfg(not(feature = "device"))]
fn identity_ok() -> bool {
    true
}

/// Explicit control entry. Nothing runs on activation; every firmware call is
/// triggered from here by the caller (Manager / phone-side plugin / probe).
#[unsafe(no_mangle)]
pub extern "C" fn canopus_headset_control(cmd: u32, arg: *const u8) -> i32 {
    #[cfg(feature = "device")]
    {
        device::control(cmd, arg)
    }
    #[cfg(not(feature = "device"))]
    {
        let _ = (cmd, arg);
        E_UNSUPPORTED
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn canopus_mod_prepare(_ctx: *const ContextV1) -> i32 {
    STATE.store(ST_IDLE, Ordering::Relaxed);
    LAST_ERROR.store(0, Ordering::Relaxed);
    LAST_FW_RC.store(0, Ordering::Relaxed);
    LAST_STOP_RC.store(0, Ordering::Relaxed);
    LAST_PAIR_RC.store(0, Ordering::Relaxed);
    BOND_STATE.store(0, Ordering::Relaxed);
    BOND_COD.store(0, Ordering::Relaxed);
    CB_COUNT.store(0, Ordering::Relaxed);
    CB_ADDR.store(0, Ordering::Relaxed);
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn canopus_mod_activate(ctx: *const ContextV1) -> i32 {
    if !identity_ok() {
        return -1;
    }
    STATE.store(ST_IDLE, Ordering::Relaxed);
    let rc = page::register(ctx);
    #[cfg(feature = "device")]
    if rc == 0 {
        // LyraPlayer compatibility: expose /dev/canopus_audio (the paid
        // BluetoothAudio module's character device) so the player talks to
        // this module. Registration failure is non-fatal to the page.
        let _ = device::audio_init();
    }
    rc
}

#[unsafe(no_mangle)]
pub extern "C" fn canopus_mod_deactivate(ctx: *const ContextV1) -> i32 {
    page::unregister(ctx);
    // This module is `resident-after-activation`: the page going away is NOT
    // the end of the A2DP link. Device log 16 shows the NuttX mm_free.c:173
    // assert firing right after a page pop / module deactivate while the
    // AVDTP channel was still live - teardown(true) unregisters the SDP
    // records and adapter callbacks underneath an active channel. Only
    // canopus_mod_stop (explicit unload) and CMD_TEARDOWN release them.
    let _ = ctx;
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn canopus_mod_stop(_ctx: *const ContextV1) -> i32 {
    #[cfg(feature = "device")]
    {
        device::teardown(true)
    }
    #[cfg(not(feature = "device"))]
    {
        0
    }
}

/// status record: magic, state, last firmware rc, bond state, two SDP handles,
/// whether each L2CAP request was accepted by the queue, discovered-peer count,
/// adapter-table registration handle (0 = not registered), and the last
/// adapter / discovery states reported by the firmware (0xFFFFFFFF unknown).
#[unsafe(no_mangle)]
pub extern "C" fn canopus_mod_query(writer: *mut StatusWriterV1) -> i32 {
    let Some(writer) = (unsafe { writer.as_mut() }) else {
        return -1;
    };
    #[cfg(feature = "device")]
    let tail = [
        device::FOUND_LEN.load(Ordering::Relaxed),
        device::REGISTRATION.load(Ordering::Relaxed),
        device::ADAPTER_STATE.load(Ordering::Relaxed),
        device::DISCOVERY_STATE.load(Ordering::Relaxed),
    ];
    #[cfg(not(feature = "device"))]
    let tail = [0u32, 0, 0, 0];
    let record = [
        MAGIC,
        STATE.load(Ordering::Relaxed),
        LAST_ERROR.load(Ordering::Relaxed) as u32,
        BOND_STATE.load(Ordering::Relaxed),
        SDP_SOURCE.load(Ordering::Relaxed),
        SDP_AVRCP.load(Ordering::Relaxed),
        !AVDTP_NODE.load(Ordering::Relaxed).is_null() as u32,
        !AVCTP_NODE.load(Ordering::Relaxed).is_null() as u32,
        tail[0],
        tail[1],
        tail[2],
        tail[3],
    ];
    for value in record {
        if !unsafe { status_put_u32(writer, value) } {
            return -1;
        }
    }
    status_writer_publish(writer);
    0
}

const fn pack<const N: usize>(s: &[u8]) -> [u8; N] {
    let mut out = [0u8; N];
    let mut i = 0;
    while i < s.len() && i < N {
        out[i] = s[i];
        i += 1;
    }
    out
}

const TARGET_ID: &str = match option_env!("CANOPUS_TARGET_ID") {
    Some(id) => id,
    None => "xiaomi-band-11-4.100-179",
};

// ---------------------------------------------------------------------------
// Manager UI page (ADR-CAN-010). Registered on activation so the Manager
// detail page offers an entry; rows dispatch the same control commands as
// `canopus_headset_control`. Unverified firmware paths (CMD_CONNECT, and
// CMD_PAIR with a UI-chosen peer) stay out of the surface.
// ---------------------------------------------------------------------------
mod page {
    use super::*;
    use canopus_abi::{
        CanopusContext, CanopusUiObj, UiActionRowPropsV1, UiEntryV1, UiEventV1,
        UiNavigationPagePropsV1, UiSectionPropsV1, UiStatusRowPropsV1, UI_EVENT_ACTIVATE,
        UI_EVENT_VALUE_CHANGED, UI_OK,
    };
    use canopus_runtime::ui::{clear_entry, set_entry};

    const EV_SCAN_START: u32 = 1;
    const EV_SCAN_STOP: u32 = 2;
    const EV_SDP: u32 = 3;
    const EV_PAIR_FIRST: u32 = 4;
    const EV_TEARDOWN: u32 = 5;
    const EV_PAIR_SECOND: u32 = 6;
    const EV_PAIR_THIRD: u32 = 7;
    const EV_REFRESH: u32 = 8; // query the adapter's bond state + re-render
    const EV_REMOVE_BOND: u32 = 9;
    const EV_CONNECT_ACL: u32 = 10; // hold an ACL to the peer via PSM 1
    /// The UI host emits ACTIVATE and VALUE_CHANGED for one tap. Rotation
    /// must advance exactly once, so remember the last event type seen for
    /// this button and skip only the paired VALUE_CHANGED.
    static LAST_CONNECT_EV: AtomicU32 = AtomicU32::new(0xFFFF_FFFF);
    const EV_A2DP: u32 = 11;
    static LAST_A2DP_EV: AtomicU32 = AtomicU32::new(0xFFFF_FFFF);
    const EV_PROBE: u32 = 12;
    static LAST_PROBE_EV: AtomicU32 = AtomicU32::new(0xFFFF_FFFF);

    const DEV_LABELS: [&str; 3] = ["设备 1", "设备 2", "设备 3"];

    /// NUL-terminated entry label (`UI_ENTRY_LABEL_MAX` is 24 bytes).
    static LABEL: &[u8] = "耳机连接\0".as_bytes();

    /// Dispatches a control command; on the device build this is the same
    /// `device::control` path as `canopus_headset_control`.
    fn ctl(cmd: u32, arg: *const u8) -> i32 {
        #[cfg(feature = "device")]
        {
            device::control(cmd, arg)
        }
        #[cfg(not(feature = "device"))]
        {
            let _ = (cmd, arg);
            E_UNSUPPORTED
        }
    }

    pub(crate) fn put_str(buf: &mut [u8], text: &str) -> u32 {
        let bytes = text.as_bytes();
        let n = bytes.len().min(buf.len());
        buf[..n].copy_from_slice(&bytes[..n]);
        n as u32
    }

    pub(crate) fn put_num(buf: &mut [u8], value: i32) -> u32 {
        let mut tmp = [0u8; 12];
        let mut i = 0usize;
        let mut rest = value.unsigned_abs();
        loop {
            tmp[i] = b'0' + (rest % 10) as u8;
            i += 1;
            rest /= 10;
            if rest == 0 {
                break;
            }
        }
        let mut n = 0usize;
        if value < 0 && n < buf.len() {
            buf[n] = b'-';
            n += 1;
        }
        while i > 0 && n < buf.len() {
            i -= 1;
            buf[n] = tmp[i];
            n += 1;
        }
        n as u32
    }

    fn state_text(buf: &mut [u8]) -> u32 {
        if STATE.load(Ordering::Relaxed) == ST_CHANNELS_QUEUED {
            if crate::avdtp::PHASE.load(Ordering::Relaxed) == crate::avdtp::PH_DONE {
                // m<media CID>p<packets sent>t<timer ticks>s<START accept>
                let mut n = put_str(buf, "m") as usize;
                n += put_num(&mut buf[n..], crate::media::MEDIA_CID.load(Ordering::Relaxed) as i32) as usize;
                n += put_str(&mut buf[n..], "p") as usize;
                n += put_num(&mut buf[n..], crate::media::PACKETS.load(Ordering::Relaxed) as i32) as usize;
                n += put_str(&mut buf[n..], "t") as usize;
                n += put_num(&mut buf[n..], crate::media::TICKS.load(Ordering::Relaxed) as i32) as usize;
                n += put_str(&mut buf[n..], "s") as usize;
                n += put_num(&mut buf[n..], crate::media::START_ACCEPTED.load(Ordering::Relaxed) as i32) as usize;
                return n as u32;
            }
            let phase = crate::avdtp::PHASE.load(Ordering::Relaxed);
            if phase != crate::avdtp::PH_IDLE {
                // A<phase>e<last event>f<freed buffers>r<responses>;
                // phase 6 = OPEN accepted, f>0 proves the fixed ownership.
                let mut m = put_str(buf, "A") as usize;
                m += put_num(&mut buf[m..], phase as i32) as usize;
                m += put_str(&mut buf[m..], "e") as usize;
                m += put_num(&mut buf[m..], crate::avdtp::LAST_EVENT.load(Ordering::Relaxed) as i32) as usize;
                m += put_str(&mut buf[m..], "f") as usize;
                m += put_num(&mut buf[m..], crate::avdtp::FREED.load(Ordering::Relaxed) as i32) as usize;
                m += put_str(&mut buf[m..], "r") as usize;
                m += put_num(&mut buf[m..], crate::avdtp::RESPONSES.load(Ordering::Relaxed) as i32) as usize;
                // q<send rc: -2/-1/0/1>c<remote dcid>: c0 means the sink's
                // CID never arrived (connection response missing); c>0 is
                // the value every command SDU is submitted with.
                m += put_str(&mut buf[m..], "q") as usize;
                m += put_num(&mut buf[m..], crate::avdtp::LAST_RC.load(Ordering::Relaxed)) as usize;
                m += put_str(&mut buf[m..], "c") as usize;
                m += put_num(&mut buf[m..], crate::avdtp::TX_CID.load(Ordering::Relaxed) as i32) as usize;
                return m as u32;
            }
            // Show which firmware profile id was used and the IPC result, so
            // one tap reports both facts on the device.
            let mut n = put_str(buf, "P") as usize;
            n += put_num(&mut buf[n..], LAST_PROFILE.load(Ordering::Relaxed) as i32) as usize;
            if n < buf.len() {
                buf[n] = b' ';
                n += 1;
            }
            n += put_str(&mut buf[n..], "r") as usize;
            n += put_num(&mut buf[n..], LAST_PAIR_RC.load(Ordering::Relaxed)) as usize;
            return n as u32;
        }
        let name = match STATE.load(Ordering::Relaxed) {
            ST_IDLE => "待机",
            ST_SCANNING => "扫描中",
            ST_PEER_SET => "对端已设",
            ST_BONDING => {
                match BOND_STATE.load(Ordering::Relaxed) {
                    1 => "配对中",
                    2 => "已配对",
                    _ => "配对已提交",
                }
            }
            ST_SDP_READY => "SDP 就绪",
            ST_CHANNELS_QUEUED => "信道已排",
            ST_FAILED => "失败",
            _ => "未知",
        };
        put_str(buf, name)
    }

    fn err_text(buf: &mut [u8]) -> u32 {
        let code = LAST_ERROR.load(Ordering::Relaxed);
        if code == 0 {
            return put_str(buf, "无");
        }
        let name = match code {
            E_IDENTITY => "身份不符",
            E_STATE => "状态错误",
            E_ARGUMENT => "参数错误",
            E_IO => "固件IO",
            E_NOMEM => "内存不足",
            E_UNSUPPORTED => "不支持",
            _ => "固件",
        };
        let mut n = put_str(buf, name) as usize;
        if n < buf.len() {
            buf[n] = b' ';
            n += 1;
        }
        n += put_num(&mut buf[n..], code) as usize;
        n as u32
    }

    fn bond_text(buf: &mut [u8]) -> u32 {
        let v = BOND_STATE.load(Ordering::Relaxed);
        let mut n = put_num(buf, v as i32) as usize;
        let name = match v {
            1 => " 配对中",
            2 => " 已配对",
            0 => " 未配对",
            _ => "",
        };
        n += put_str(&mut buf[n..], name) as usize;
        // Slot-9 diagnostics stay for one more build: count + last address.
        if n < buf.len() {
            buf[n] = b' ';
            n += 1;
        }
        n += put_str(&mut buf[n..], "cb") as usize;
        n += put_num(&mut buf[n..], CB_COUNT.load(Ordering::Relaxed) as i32) as usize;
        let a = CB_ADDR.load(Ordering::Relaxed);
        if a != 0 && n + 6 <= buf.len() {
            const HEX: &[u8; 16] = b"0123456789ABCDEF";
            buf[n] = b'@';
            n += 1;
            for shift in [0, 8] {
                let byte = ((a >> shift) & 0xFF) as u8;
                buf[n] = HEX[((byte >> 4) & 0xF) as usize];
                buf[n + 1] = HEX[(byte & 0xF) as usize];
                n += 2;
            }
        }
        // L2CAP completion evidence: event code + negotiated CID. Kept short -
        // status-row values over ~32 bytes crashed the UI backend (0.10.1).
        if device::L2CAP_EVENTS.load(Ordering::Relaxed) != 0 && n + 8 <= buf.len() {
            const HEX: &[u8; 16] = b"0123456789ABCDEF";
            let cid = device::L2CAP_CID.load(Ordering::Relaxed);
            n += put_str(&mut buf[n..], " L") as usize;
            n += put_num(&mut buf[n..], device::L2CAP_EVENT.load(Ordering::Relaxed)) as usize;
            buf[n] = b':';
            n += 1;
            for shift in [12, 8, 4, 0] {
                let nib = ((cid >> shift) & 0xF) as usize;
                buf[n] = HEX[nib];
                n += 1;
            }
        }
        n as u32
    }

    /// Human-readable Class of Device. 0.9.2: NOT rendered - the extra
    /// status row added in 0.9.1 black-screens the Manager UI page, so this
    /// diagnostic path is disabled until the render regression is understood.
    #[allow(dead_code)]
    fn cod_text(buf: &mut [u8]) -> u32 {
        let cod = BOND_COD.load(Ordering::Relaxed);
        if cod == 0 {
            return put_str(buf, "未查询");
        }
        const MAJORS: [&str; 10] = [
            "杂项", "电脑", "手机", "网络", "音视频", "外设", "影像", "穿戴", "玩具", "医疗",
        ];
        let major = ((cod >> 8) & 0x1F) as usize;
        let mut n = put_str(
            buf,
            if major < MAJORS.len() { MAJORS[major] } else { "类别?" },
        ) as usize;
        let svc = cod >> 13;
        for (bit, tag) in [
            (3, "渲染"),
            (4, "传输"),
            (5, "音频"),
            (6, "电话"),
        ] {
            if svc & (1 << bit) != 0 && n < buf.len() - 7 {
                buf[n] = b' ';
                n += 1;
                n += put_str(&mut buf[n..], tag) as usize;
            }
        }
        n as u32
    }

    /// d<mp3 frames>s<pcm samples>b<sbc frame len>k<encode ok>x<syncword>e<err>
    /// d<mp3 frames>f<sbc frames>z<silence>b<frame len>k<encode ok>
    /// x<syncword>e<err>c<peer capability low 16 bits, hex>
    fn probe_text(buf: &mut [u8]) -> u32 {
        if crate::stream::PUMPS.load(Ordering::Relaxed) == 0 {
            return put_str(buf, "待运行");
        }
        let mut n = put_str(buf, "d") as usize;
        n += put_num(&mut buf[n..], crate::stream::DECODED.load(Ordering::Relaxed) as i32) as usize;
        n += put_str(&mut buf[n..], "f") as usize;
        n += put_num(&mut buf[n..], crate::stream::SBC_FRAMES.load(Ordering::Relaxed) as i32) as usize;
        n += put_str(&mut buf[n..], "z") as usize;
        n += put_num(&mut buf[n..], crate::stream::SILENCE.load(Ordering::Relaxed) as i32) as usize;
        n += put_str(&mut buf[n..], "b") as usize;
        n += put_num(&mut buf[n..], crate::stream::MEASURED_FRAME_LEN.load(Ordering::Relaxed) as i32) as usize;
        n += put_str(&mut buf[n..], "k") as usize;
        n += put_num(&mut buf[n..], crate::stream::SBC_OK.load(Ordering::Relaxed) as i32) as usize;
        n += put_str(&mut buf[n..], "x") as usize;
        n += put_num(&mut buf[n..], crate::stream::SBC_SYNC.load(Ordering::Relaxed) as i32) as usize;
        n += put_str(&mut buf[n..], "e") as usize;
        n += put_num(&mut buf[n..], crate::stream::LAST_ERR.load(Ordering::Relaxed)) as usize;
        let cap = crate::CAP_WORD.load(Ordering::Relaxed);
        n += put_str(&mut buf[n..], "c") as usize;
        const HEX: &[u8; 16] = b"0123456789ABCDEF";
        if n + 4 <= buf.len() {
            for shift in [12, 8, 4, 0] {
                buf[n] = HEX[((cap >> shift) & 0xF) as usize];
                n += 1;
            }
        }
        n as u32
    }

    fn peer_text(buf: &mut [u8]) -> u32 {
        let mut n = put_str(buf, BUILD_TAG) as usize;
        if n < buf.len() {
            buf[n] = b' ';
            n += 1;
        }
        let addr = peer_bytes();
        if has_peer(&addr) {
            n += addr_text(&addr, &mut buf[n..]) as usize;
            return n as u32;
        }
        let c = RESTORE_COUNT.load(Ordering::Relaxed);
        if c > 0 {
            // Enumeration ran but nothing matched: show the entry count and
            // the first entry's leading bytes exactly as the list carries
            // them (e.g. b3:BB07 => 3 entries, list order reversed).
            n += put_str(&mut buf[n..], "b") as usize;
            n += put_num(&mut buf[n..], c as i32) as usize;
            let raw = RESTORE_RAW2.load(Ordering::Relaxed);
            const HEX: &[u8; 16] = b"0123456789ABCDEF";
            if n + 5 <= buf.len() {
                buf[n] = b':';
                n += 1;
                for shift in [8, 0] {
                    let byte = ((raw >> shift) & 0xFF) as u8;
                    buf[n] = HEX[((byte >> 4) & 0xF) as usize];
                    buf[n + 1] = HEX[(byte & 0xF) as usize];
                    n += 2;
                }
            }
            return n as u32;
        }
        n += put_str(&mut buf[n..], "未设置") as usize;
        n as u32
    }

    fn addr_text(addr: &[u8], buf: &mut [u8]) -> u32 {
        const HEX: &[u8; 16] = b"0123456789ABCDEF";
        let mut n = 0usize;
        // Adapter IPC addresses are OTA byte order (IDA: the firmware's own
        // cmd62 client converts "1B:01:..." via sub_C465538 which writes the
        // first hex pair into out[5]). Render in reverse so the page shows
        // the familiar display order; never flip PEER itself.
        for (i, byte) in addr.iter().rev().enumerate() {
            if i > 0 && n < buf.len() {
                buf[n] = b':';
                n += 1;
            }
            for shift in [4, 0] {
                if n < buf.len() {
                    buf[n] = HEX[((byte >> shift) & 0xF) as usize];
                    n += 1;
                }
            }
        }
        n as u32
    }

    /// Name of one discovered peer (`FOUND_STRIDE` layout: address[6], rssi,
    /// reserved, class_of_device[4], name[20]); falls back to the address.
    fn peer_name(entry: &[u8; 32], buf: &mut [u8]) -> u32 {
        let name = &entry[12..32];
        let end = name.iter().position(|&b| b == 0).unwrap_or(name.len());
        if end > 0 {
            let n = end.min(buf.len());
            buf[..n].copy_from_slice(&name[..n]);
            return n as u32;
        }
        addr_text(&entry[..6], buf)
    }

    fn section(ui_obj: &CanopusUiObj, tree: *mut c_void, key: u32, title: &str) -> i32 {
        let props = UiSectionPropsV1 {
            struct_size: core::mem::size_of::<UiSectionPropsV1>() as u32,
            title: title.as_bytes().as_ptr().cast(),
            title_len: title.len() as u32,
        };
        (unsafe { (ui_obj.section)(tree, key, &props as *const UiSectionPropsV1 as *const c_void) })
    }

    fn status_row(
        ui_obj: &CanopusUiObj,
        tree: *mut c_void,
        key: u32,
        label: &str,
        value: &[u8],
    ) -> i32 {
        let props = UiStatusRowPropsV1 {
            struct_size: core::mem::size_of::<UiStatusRowPropsV1>() as u32,
            label: label.as_bytes().as_ptr().cast(),
            label_len: label.len() as u32,
            value: value.as_ptr().cast(),
            value_len: value.len() as u32,
        };
        unsafe { (ui_obj.status_row)(tree, key, &props as *const UiStatusRowPropsV1 as *const c_void) }
    }

    fn action_row(
        ui_obj: &CanopusUiObj,
        tree: *mut c_void,
        key: u32,
        label: &str,
        detail: &str,
        event_id: u32,
    ) -> i32 {
        let props = UiActionRowPropsV1 {
            struct_size: core::mem::size_of::<UiActionRowPropsV1>() as u32,
            label: label.as_bytes().as_ptr().cast(),
            label_len: label.len() as u32,
            detail: detail.as_bytes().as_ptr().cast(),
            detail_len: detail.len() as u32,
            event_id,
            enabled: 1,
        };
        unsafe { (ui_obj.action_row)(tree, key, &props as *const UiActionRowPropsV1 as *const c_void) }
    }

    /// Builds the single-page tree and commits it. Value buffers stay alive
    /// until `tree_commit`, matching the C demo's contract with the backend.
    fn render(ctx: *const CanopusContext, ui: *mut c_void) -> i32 {
        let object = unsafe { (*ctx).ui };
        if object.is_null() {
            return E_UNSUPPORTED;
        }
        let ui_obj = unsafe { &*object };
        unsafe {
            let mut tree: *mut c_void = core::ptr::null_mut();
            let mut rc = (ui_obj.tree_begin)(ui, &mut tree);
            if rc != UI_OK {
                return rc;
            }

            let title = "耳机连接";
            let page = UiNavigationPagePropsV1 {
                struct_size: core::mem::size_of::<UiNavigationPagePropsV1>() as u32,
                title: title.as_bytes().as_ptr().cast(),
                title_len: title.len() as u32,
            };
            rc = (ui_obj.navigation_page)(tree, 1, &page as *const UiNavigationPagePropsV1 as *const c_void);

            let mut b_state = [0u8; 24];
            let n_state = state_text(&mut b_state);
            let mut b_err = [0u8; 28];
            let n_err = err_text(&mut b_err);
            let mut b_bond = [0u8; 36];
            let n_bond = bond_text(&mut b_bond);
            let mut b_peer = [0u8; 28];
            let n_peer = peer_text(&mut b_peer);
            let mut b_probe = [0u8; 40];
            let n_probe = probe_text(&mut b_probe);
            // CMD_SCAN_RESULTS writes into this buffer; it must be mutable.
            let mut scan = [0u8; 256];
            let found = ctl(CMD_SCAN_RESULTS, scan.as_mut_ptr());
            let mut b_found = [0u8; 16];
            let n_found = match found {
                count if count < 0 => put_str(&mut b_found, "暂不可用"),
                0 => put_str(&mut b_found, "无"),
                count => {
                    let mut n = put_num(&mut b_found, count) as usize;
                    n += put_str(&mut b_found[n..], " 台") as usize;
                    n as u32
                }
            };
            let mut b_names = [[0u8; 24]; 3];
            let mut n_names = [0u32; 3];
            let shown = found.clamp(0, 3) as usize;
            for i in 0..shown {
                let mut entry = [0u8; 32];
                entry.copy_from_slice(&scan[i * 32..i * 32 + 32]);
                n_names[i] = peer_name(&entry, &mut b_names[i]);
            }
            if rc == UI_OK {
                rc = section(ui_obj, tree, 2, "状态");
            }
            if rc == UI_OK {
                rc = status_row(ui_obj, tree, 3, "模块", &b_state[..n_state as usize]);
            }
            if rc == UI_OK {
                rc = status_row(ui_obj, tree, 4, "最近错误", &b_err[..n_err as usize]);
            }
            if rc == UI_OK {
                rc = status_row(ui_obj, tree, 5, "配对状态", &b_bond[..n_bond as usize]);
            }
            if rc == UI_OK {
                rc = status_row(ui_obj, tree, 6, "对端地址", &b_peer[..n_peer as usize]);
            }
            if rc == UI_OK {
                rc = status_row(ui_obj, tree, 7, "扫描结果", &b_found[..n_found as usize]);
            }
            if rc == UI_OK {
                rc = status_row(ui_obj, tree, 26, "音频流水线", &b_probe[..n_probe as usize]);
            }
            for i in 0..shown {
                if rc != UI_OK {
                    break;
                }
                rc = status_row(
                    ui_obj,
                    tree,
                    14 + i as u32,
                    DEV_LABELS[i],
                    &b_names[i][..n_names[i] as usize],
                );
            }
            if rc == UI_OK {
                // Each container needs its own end(): the previous section and
                // the root navigation page are distinct open containers.
                rc = (ui_obj.end)(tree); // section "状态"
            }
            if rc == UI_OK {
                rc = section(ui_obj, tree, 8, "操作");
            }
            if rc == UI_OK {
                rc = action_row(ui_obj, tree, 9, "开始扫描", "搜索附近耳机", EV_SCAN_START);
            }
            if rc == UI_OK {
                rc = action_row(ui_obj, tree, 10, "停止扫描", "结束 discovery", EV_SCAN_STOP);
            }
            if rc == UI_OK {
                rc = action_row(ui_obj, tree, 11, "注册 SDP", "发布 A2DP/AVRCP 记录", EV_SDP);
            }
            if rc == UI_OK {
                rc = action_row(ui_obj, tree, 12, "配对第一台", "设备 1：仅交换配对密钥", EV_PAIR_FIRST);
            }
            if rc == UI_OK {
                rc = action_row(ui_obj, tree, 20, "配对第 2 台", "设备 2：仅交换配对密钥", EV_PAIR_SECOND);
            }
            if rc == UI_OK {
                rc = action_row(ui_obj, tree, 21, "配对第 3 台", "设备 3：仅交换配对密钥", EV_PAIR_THIRD);
            }
            if rc == UI_OK {
                rc = action_row(ui_obj, tree, 13, "复位", "释放适配器/SDP/L2CAP", EV_TEARDOWN);
            }
            if rc == UI_OK {
                rc = action_row(ui_obj, tree, 22, "刷新状态", "查询对端配对状态", EV_REFRESH);
            }
            if rc == UI_OK {
                rc = action_row(ui_obj, tree, 25, "A2DP 连接", "AVDTP DISCOVER→CAP→CFG→OPEN，状态行看 A<阶段>", EV_A2DP);
            }
            if rc == UI_OK {
                rc = action_row(ui_obj, tree, 24, "ACL 连接(旧)", "cmd62 profile 轮换诊断", EV_CONNECT_ACL);
            }
            if rc == UI_OK {
                rc = action_row(ui_obj, tree, 27, "解码一片", "只解一帧 MP3，验证分片不卡；看音频流水线行", EV_PROBE);
            }
            if rc == UI_OK {
                rc = action_row(ui_obj, tree, 23, "解除配对", "清除当前对端 bond，可重新配对", EV_REMOVE_BOND);
            }
            if rc == UI_OK {
                rc = (ui_obj.end)(tree); // section "操作"
            }
            if rc == UI_OK {
                rc = (ui_obj.end)(tree); // navigation page
            }
            if rc == UI_OK {
                (ui_obj.tree_commit)(tree)
            } else {
                (ui_obj.tree_abort)(tree);
                rc
            }
        }
    }

    unsafe extern "C" fn on_open(
        ctx: *const ContextV1,
        ui: *mut c_void,
        _user_data: *mut c_void,
    ) -> i32 {
        render(ctx.cast(), ui)
    }

    /// Pairs with the peer at `index` in the discovery list. The first entry
    /// is frequently a phone or laptop rather than the headset, so explicit
    /// selection is required. `CMD_CONNECT` stays unreachable from the UI
    /// (unverified null-callback firmware path).
    fn pair_at(index: usize) -> i32 {
        let mut scan = [0u8; 256];
        let found = ctl(CMD_SCAN_RESULTS, scan.as_mut_ptr());
        if found <= index as i32 {
            return fail(E_STATE, 0);
        }
        let base = index * 32; // FOUND_STRIDE: address[6] + rssi + CoD + name
        let mut addr = [0u8; 6];
        addr.copy_from_slice(&scan[base..base + 6]);
        let rc = ctl(CMD_SET_PEER, addr.as_ptr());
        if rc < 0 {
            return rc;
        }
        ctl(CMD_PAIR, core::ptr::null())
    }

    fn pair_first() -> i32 {
        pair_at(0)
    }

    unsafe extern "C" fn on_event(
        ctx: *const ContextV1,
        ui: *mut c_void,
        event: *const UiEventV1,
        _user_data: *mut c_void,
    ) -> i32 {
        let event = unsafe { &*event };
        if event.type_ != UI_EVENT_ACTIVATE && event.type_ != UI_EVENT_VALUE_CHANGED {
            return UI_OK;
        }
        let rc = match event.event_id {
            EV_SCAN_START => ctl(CMD_SCAN_START, core::ptr::null()),
            EV_SCAN_STOP => ctl(CMD_SCAN_STOP, core::ptr::null()),
            EV_SDP => ctl(CMD_REGISTER_SDP, core::ptr::null()),
            EV_PAIR_FIRST => pair_first(),
            EV_PAIR_SECOND => pair_at(1),
            EV_PAIR_THIRD => pair_at(2),
            EV_REFRESH => ctl(CMD_QUERY_BOND, core::ptr::null()),
            EV_REMOVE_BOND => ctl(CMD_REMOVE_BOND, core::ptr::null()),
            EV_CONNECT_ACL => {
                let prev = LAST_CONNECT_EV.swap(event.type_, Ordering::Relaxed);
                if event.type_ == UI_EVENT_VALUE_CHANGED && prev == UI_EVENT_ACTIVATE {
                    UI_OK
                } else {
                    ctl(CMD_CONNECT_ACL, core::ptr::null())
                }
            }
            EV_A2DP => {
                let prev = LAST_A2DP_EV.swap(event.type_, Ordering::Relaxed);
                if event.type_ == UI_EVENT_VALUE_CHANGED && prev == UI_EVENT_ACTIVATE {
                    UI_OK
                } else {
                    ctl(CMD_A2DP_CONNECT, core::ptr::null())
                }
            }
            EV_PROBE => {
                let prev = LAST_PROBE_EV.swap(event.type_, Ordering::Relaxed);
                if event.type_ == UI_EVENT_VALUE_CHANGED && prev == UI_EVENT_ACTIVATE {
                    UI_OK
                } else {
                    ctl(CMD_MP3_PROBE, core::ptr::null())
                }
            }
            EV_TEARDOWN => ctl(CMD_TEARDOWN, core::ptr::null()),
            _ => 0,
        };
        let _ = rc;
        render(ctx.cast(), ui)
    }

    static ENTRY: UiEntryV1 = UiEntryV1 {
        struct_size: core::mem::size_of::<UiEntryV1>() as u32,
        label: LABEL.as_ptr().cast(),
        on_open: Some(on_open),
        on_event: Some(on_event),
        on_back: None,
        on_close: None,
        user_data: core::ptr::null_mut(),
        page_count: 0,
        on_resume: None,
        on_pause: None,
    };

    /// Registers the Manager page entry and propagates the result: a failed
    /// registration surfaces as the module's activation error (visible in the
    /// Manager detail page) instead of a silently missing "打开界面" entry.
    pub fn register(ctx: *const ContextV1) -> i32 {
        let Some(context) = (unsafe { (ctx as *const CanopusContext).as_ref() }) else {
            return 0;
        };
        if context.ui.is_null() {
            return 0;
        }
        match set_entry(ctx.cast(), &ENTRY) {
            Ok(()) => 0,
            Err(rc) => rc,
        }
    }

    pub fn unregister(ctx: *const ContextV1) {
        let context = ctx.cast::<CanopusContext>();
        if context.is_null() {
            return;
        }
        let _ = clear_entry(context);
    }
}

#[unsafe(no_mangle)]
pub static canopus_module_descriptor: ModuleDescriptorV1 = ModuleDescriptorV1 {
    struct_size: core::mem::size_of::<ModuleDescriptorV1>() as u32,
    abi_major: ABI_MAJOR,
    abi_minor: ABI_MINOR,
    flags: 0,
    module_id: pack(b"headset_link_179"),
    module_version: pack(b"0.9.0"),
    build_id: pack(b"headset-link-179-0.9.0"),
    target_id: pack(TARGET_ID.as_bytes()),
    prepare: Some(canopus_mod_prepare),
    activate: Some(canopus_mod_activate),
    deactivate: Some(canopus_mod_deactivate),
    stop: Some(canopus_mod_stop),
    query: Some(canopus_mod_query),
    publish_native_app: None,
    publish_native_app_stage: None,
};

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
