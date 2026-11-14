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
//! It does **not** implement an A2DP source. Missing, and deliberately not
//! faked: AVDTP signalling (DISCOVER / GET_CAPABILITIES / SET_CONFIGURATION /
//! OPEN / START), SBC encoding and media packetisation, an audio data source,
//! AVRCP command handling, and the L2CAP completion-event callback that owns
//! and frees the connect request and the 120-byte completion event (see
//! `docs/band11-bluetooth-139.md`). After CMD_CONNECT the state is
//! `ST_CHANNELS_QUEUED` — requests submitted, not a playing audio link.
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
//! Because the completion callback is absent, the two 68-byte connect requests
//! are not freed by this module. This is a known ownership gap, not an
//! oversight in the firmware ABI. Do not treat a queued request as a connected
//! channel.
//!
//! IDA cross-check on the patched `band11std` `vela_ap.bin` (base 0x0C0C0000)
//! found a stronger constraint: `bt_l2cap_connect` (0xc86d468) only submits a
//! 20-byte work node; the handler `sub_C86F9B0` (0xc86f9b0) reads request+12
//! as the event callback, installs it at channel+20 via `sub_C86BD54`
//! (0xc86bd54), writes the CID back to request+0 and invokes the callback as
//! `(2, request)` (CONFIRM). When request+12 is zero the firmware falls back
//! to `sub_C867C80(PSM, ...)`, i.e. it expects an already-registered PSM
//! handler for AVDTP (0x19) / AVCTP (0x17). `queue_connect` below zeroes +12
//! and registers no PSM handler, so CMD_CONNECT is an UNVERIFIED path that
//! may dereference a null callback. Published callbacks also require boot
//! residency while this module is declared `lifecycle = "removable"`.
//! Do not run CMD_CONNECT on hardware until both points are resolved. See
//! `NOTES-ida-179.md` for the address evidence and the build hand-off.
//!
//! All firmware calls are exact-target full-trust absolute addresses. `.179`
//! symbols remain restricted/PENDING with `capabilities = []`
//! (`targets/xiaomi-band-11-4.100.179/target.toml`); `AGENTS.md` forbids
//! promoting them. Nothing here has device proof. Activation fails closed on
//! an identity mismatch and the module stays idle until an explicit control
//! call — loading it alone touches no Bluetooth state.

#![no_std]

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

// Service error codes from sdk/c/canopus_services.h; E_IDENTITY is local.
pub const E_IDENTITY: i32 = -1;
pub const E_STATE: i32 = -16;
pub const E_ARGUMENT: i32 = -22;
pub const E_IO: i32 = -5;
pub const E_NOMEM: i32 = -12;
pub const E_UNSUPPORTED: i32 = -38;

static STATE: AtomicU32 = AtomicU32::new(ST_IDLE);
static LAST_ERROR: AtomicI32 = AtomicI32::new(0);
static BOND_STATE: AtomicU32 = AtomicU32::new(0);
static ADAPTER: AtomicPtr<c_void> = AtomicPtr::new(core::ptr::null_mut());
static SDP_SOURCE: AtomicU32 = AtomicU32::new(0);
static SDP_AVRCP: AtomicU32 = AtomicU32::new(0);
static AVDTP_NODE: AtomicPtr<c_void> = AtomicPtr::new(core::ptr::null_mut());
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
    use canopus_target_private::{
        ADAPTER_STATE_ON, CLASSIC_TRANSPORT, CONNECT_ADDRESS_OFFSET, CONNECT_REQUEST_SIZE,
        CREATE_BOND_ADAPTER_NOT_READY, DISCOVERY_TIMEOUT_SECONDS, DiscoveryResult,
        SdpAvrcpControllerRecord, SdpSourceRecord, bt_adapter_get_instance, bt_adapter_get_state,
        bt_adapter_register, bt_adapter_unregister, bt_alloc, bt_create_bond, bt_discovery_start,
        bt_discovery_stop, bt_get_bond_state, bt_l2cap_connect, configure_avctp_connect_request,
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
        TABLE_READY.store(1, Ordering::Relaxed);
    }

    pub fn guard_ok() -> bool {
        canopus_target_private::canopus_identity_guard() == 0
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
                    ok(ST_PEER_SET)
                }
            }
            CMD_PAIR => pair(),
            CMD_REGISTER_SDP => register_sdp(),
            CMD_CONNECT => connect(),
            CMD_TEARDOWN => teardown(),
            CMD_SCAN_RESULTS => scan_results(arg),
            _ => fail(E_ARGUMENT, 0),
        }
    }

    fn scan_start() -> i32 {
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
        if rc != 0 {
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
        let adapter = ADAPTER.load(Ordering::Relaxed);
        if adapter.is_null() {
            return fail(E_STATE, 0);
        }
        let rc = unsafe { bt_discovery_stop(adapter) };
        LAST_ERROR.store(rc, Ordering::Relaxed);
        if rc != 0 {
            return fail(E_IO, rc);
        }
        LAST_ERROR.store(0, Ordering::Relaxed);
        0
    }

    fn pair() -> i32 {
        let addr = peer_bytes();
        if !has_peer(&addr) || ADAPTER.load(Ordering::Relaxed).is_null() {
            return fail(E_ARGUMENT, 0);
        }
        let rc = unsafe { bt_create_bond(addr.as_ptr(), CLASSIC_TRANSPORT) };
        if rc == CREATE_BOND_ADAPTER_NOT_READY {
            return fail(E_STATE, rc);
        }
        if rc != 0 {
            return fail(E_IO, rc);
        }
        BOND_STATE.store(unsafe { bt_get_bond_state(addr.as_ptr()) }, Ordering::Relaxed);
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

    /// Queues one 68-byte L2CAP connect request. The callback word at +12 is
    /// left zero: no completion-event handler is installed (see module header).
    /// `bt_l2cap_connect` returns the accepted queue node, 0 when rejected.
    unsafe fn queue_connect(
        configure: unsafe fn(*mut u8),
        addr: &[u8; 6],
        node: &AtomicPtr<c_void>,
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
        let avdtp = unsafe { queue_connect(configure_avdtp_connect_request, &addr, &AVDTP_NODE) };
        if avdtp != 0 {
            return fail(avdtp, 0);
        }
        let avctp = unsafe { queue_connect(configure_avctp_connect_request, &addr, &AVCTP_NODE) };
        if avctp != 0 {
            return fail(avctp, 0);
        }
        ok(ST_CHANNELS_QUEUED)
    }

    pub fn teardown() -> i32 {
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
        // Remove this module's table from the adapter's observer list so the
        // firmware stops calling into it once the module is deactivated.
        let reg = REGISTRATION.swap(0, Ordering::Relaxed);
        if reg != 0 && !adapter.is_null() {
            LAST_ERROR.store(unsafe { bt_adapter_unregister(adapter, reg) }, Ordering::Relaxed);
        }
        FOUND_LEN.store(0, Ordering::Relaxed);
        AVDTP_NODE.store(core::ptr::null_mut(), Ordering::Relaxed);
        AVCTP_NODE.store(core::ptr::null_mut(), Ordering::Relaxed);
        BOND_STATE.store(0, Ordering::Relaxed);
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
    BOND_STATE.store(0, Ordering::Relaxed);
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn canopus_mod_activate(ctx: *const ContextV1) -> i32 {
    if !identity_ok() {
        return -1;
    }
    STATE.store(ST_IDLE, Ordering::Relaxed);
    page::register(ctx);
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn canopus_mod_deactivate(ctx: *const ContextV1) -> i32 {
    page::unregister(ctx);
    #[cfg(feature = "device")]
    {
        device::teardown()
    }
    #[cfg(not(feature = "device"))]
    {
        0
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn canopus_mod_stop(_ctx: *const ContextV1) -> i32 {
    #[cfg(feature = "device")]
    {
        device::teardown()
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
        CanopusContext, UiActionRowPropsV1, UiEntryV1, UiEventV1, UiNavigationPagePropsV1,
        UiSectionPropsV1, UiStatusRowPropsV1, UI_EVENT_ACTIVATE, UI_EVENT_VALUE_CHANGED, UI_OK,
    };
    use canopus_runtime::ui::{clear_entry, set_entry};

    const EV_SCAN_START: u32 = 1;
    const EV_SCAN_STOP: u32 = 2;
    const EV_SDP: u32 = 3;
    const EV_PAIR_FIRST: u32 = 4;
    const EV_TEARDOWN: u32 = 5;

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

    fn put_str(buf: &mut [u8], text: &str) -> u32 {
        let bytes = text.as_bytes();
        let n = bytes.len().min(buf.len());
        buf[..n].copy_from_slice(&bytes[..n]);
        n as u32
    }

    fn put_num(buf: &mut [u8], value: i32) -> u32 {
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
        let name = match STATE.load(Ordering::Relaxed) {
            ST_IDLE => "待机",
            ST_SCANNING => "扫描中",
            ST_PEER_SET => "对端已设",
            ST_BONDING => "配对中",
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

    fn peer_text(buf: &mut [u8]) -> u32 {
        let addr = peer_bytes();
        if !has_peer(&addr) {
            return put_str(buf, "未设置");
        }
        addr_text(&addr, buf)
    }

    fn addr_text(addr: &[u8], buf: &mut [u8]) -> u32 {
        const HEX: &[u8; 16] = b"0123456789ABCDEF";
        let mut n = 0usize;
        for (i, byte) in addr.iter().enumerate() {
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
        (ui_obj.section)(tree, key, &props as *const UiSectionPropsV1 as *const c_void)
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
        (ui_obj.status_row)(tree, key, &props as *const UiStatusRowPropsV1 as *const c_void)
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
        (ui_obj.action_row)(tree, key, &props as *const UiActionRowPropsV1 as *const c_void)
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
            rc = (ui_obj.navigation_page)(tree, 1, &page);

            let mut b_state = [0u8; 16];
            let n_state = state_text(&mut b_state);
            let mut b_err = [0u8; 28];
            let n_err = err_text(&mut b_err);
            let mut b_bond = [0u8; 12];
            let n_bond = put_num(&mut b_bond, BOND_STATE.load(Ordering::Relaxed) as i32);
            let mut b_peer = [0u8; 20];
            let n_peer = peer_text(&mut b_peer);
            let mut scan = [0u8; 256];
            let found = ctl(CMD_SCAN_RESULTS, scan.as_ptr());
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
                rc = action_row(ui_obj, tree, 12, "配对第一台", "与扫描结果第一台建 bond", EV_PAIR_FIRST);
            }
            if rc == UI_OK {
                rc = action_row(ui_obj, tree, 13, "复位", "释放适配器/SDP/L2CAP", EV_TEARDOWN);
            }
            if rc == UI_OK {
                rc = (ui_obj.end)(tree); // section
            }
            if rc == UI_OK {
                rc = (ui_obj.end)(tree); // page
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

    /// Pairs with the first discovered peer: re-queries the control plane,
    /// sets the peer address and bonds. `CMD_CONNECT` is deliberately not
    /// reachable from the UI (unverified null-callback firmware path).
    fn pair_first() -> i32 {
        let mut scan = [0u8; 256];
        let found = ctl(CMD_SCAN_RESULTS, scan.as_ptr());
        if found <= 0 {
            return fail(E_STATE, 0);
        }
        let rc = ctl(CMD_SET_PEER, scan.as_ptr());
        if rc < 0 {
            return rc;
        }
        ctl(CMD_PAIR, core::ptr::null())
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

    /// Registers the Manager page entry. Failure (a build without the Manager
    /// UI backend) is not an activation error; the control plane stays usable.
    pub fn register(ctx: *const ContextV1) {
        let context = ctx.cast::<CanopusContext>();
        if context.is_null() {
            return;
        }
        let _ = set_entry(context, &ENTRY);
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
    module_version: pack(b"0.2.0"),
    build_id: pack(b"headset-link-179-0.2.0"),
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
