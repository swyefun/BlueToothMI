/*
 * bsvm.c — BlackSoul VM: RMVA events bytecode runtime for Band 11.
 *
 * v0.4: all firmware access goes through the Supervisor's mapped ctx services
 * (ctx->fs / ctx->time / ctx->ui). No raw veneer addresses, so the same source
 * builds for .139 / .155 / .179 and behaves identically on device and Mirage.
 *
 * Two modes (chosen by /data/bsvm_headless.txt):
 *  - headless: a ctx->time timer walks every event page in slices (TEXT logged,
 *    CHOICE picks 0, BATTLE assumed won); the full trace is appended to
 *    /data/bsvm_log.txt. Used to validate decode/VM/memory end to end.
 *  - story UI: ctx->ui semantic tree in the Manager page; "next" steps one
 *    TEXT at a time through the selected story entry. Auto-wrapped CJK text is
 *    rendered by the target backend, not by hand-built LVGL objects.
 *
 * Verifier rules honored: no 64-bit division, no float math, zero imports,
 * fixed pools only, timer-sliced work.
 */

#include "canopus_module_entry.h"
#include "canopus_memory.h"
#include "canopus_ui.h"

#include <stdint.h>
#include <stddef.h>

#define BSVM_MODULE_ID "bsvm"

/* Canopus service context, captured in prepare/activate (declared early:
 * the FS service reads game data before the lifecycle section below) */
static const struct canopus_context_v1 *s_ctx;

/* ---------------- firmware access: ctx services only (v0.4) -------------
 * ALL firmware calls go through the Supervisor's mapped ctx services
 * (ctx->fs / ctx->time / ctx->ui). No raw veneer addresses: those are target
 * specific and were the cause of the "load -> instant crash, no log" on .179.
 * This source builds for every target that exposes the ABI2 service set, so
 * the same package target list covers .139 / .155 / .179.
 */

static const char BIN_PATH_A[] = "/data/bsvm.bin";                /* v0.1.6: outside /data/canopus (framework denies module reads there) */
static const char BIN_PATH_B[] = "/data/canopus/bsvm/bsvm.bin";   /* legacy probe only */
/* v0.4: written through ctx->fs (official service). /data/canopus is a
 * protected prefix for bare module reads, but /data itself is allowed, so the
 * log sits beside the game data where the emulator harness can fetch it. */
static const char LOG_PATH[] = "/data/bsvm_log.txt";

/* ---------------- static pools ---------------- */
#define EVT_MAX 178000u
#define DB_MAX  115200u
#define DIR_MAX 110000u
#define STRN_MAX 7200u
#define STEPS_PER_ENTRY 120000u

static uint8_t  s_evt[EVT_MAX];
static uint32_t s_evt_len;
static uint8_t  s_db[DB_MAX];
static uint32_t s_db_len;
static uint8_t  s_dir[DIR_MAX];
static uint32_t s_dir_len;
static uint32_t s_strtab[STRN_MAX];
static uint32_t s_strn;

static uint32_t s_str_off, s_str_len;

/* directory indices */
static uint32_t s_ce_off[256];
static uint16_t s_ce_len[256];
static uint32_t s_ce_n;
static uint32_t s_pg_off[6440];
static uint32_t s_pg_n;          /* all map pages */
static uint32_t s_troop_pg_off[720];
static uint32_t s_troop_pg_n;

/* ---------------- VM state ---------------- */
static uint8_t  s_switches[73];
static int32_t  s_vars[104];
static int32_t  s_gold;
static uint16_t s_items[512];
static uint8_t  s_party[8];
static uint32_t s_party_n;
static int32_t  s_battle_result;

static uint32_t s_pc;
static uint32_t s_ret[8];
static uint32_t s_sp;
static uint32_t s_steps;
static uint32_t s_entry_idx;      /* scheduler cursor */
static uint8_t  s_phase;          /* 0 running, 2 done */
static uint32_t s_runs_done, s_runs_bad;
static uint32_t s_opcnt[256];    /* opcode usage histogram for UI planning */
static uint32_t s_total_steps;   /* steps across all entries */
static uint32_t s_text_n;         /* TEXT ops processed (not logged) */
static uint8_t  s_ok_init;
static uint32_t s_total_entries;

/* ui: ctx services (no raw firmware pointers) */
static int32_t s_timer_id = -1;   /* ctx->time timer; -1 = none */
static char  s_status[72];

/* ---- story player state (rendered through ctx->ui semantic tree) ---- */
static uint8_t s_ui_mode;    /* 0 boot, 1 story, 3 done */
static char  s_dlg_buf[256];    /* current dialogue (body text node) */
static char  s_scene_buf[64];   /* scene / location (title text node) */
static uint32_t s_story_off; /* bytecode offset of the story entry being played */
static uint32_t s_story_end; /* end offset */

/* ui entry / render state */
enum { EV_NEXT = 1, EV_HEADLESS };
static struct canopus_ui_context_v1 *s_ui_ctx;   /* live page, valid while open */

/* ---------------- helpers ---------------- */
static void u32_to_dec(char *b, uint32_t v)
{
    char t[12]; int i = 0, j = 0;
    if (v == 0u) { b[0] = '0'; b[1] = '\0'; return; }
    while (v > 0u) { t[i++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (i > 0) { b[j++] = t[--i]; }
    b[j] = '\0';
}

static uint32_t udiv32(uint32_t n, uint32_t d)
{
    uint32_t q = 0u, r = 0u; int i;
    if (d == 0u) return 0xFFFFFFFFu;
    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) { r -= d; q |= (1u << i); }
    }
    return q;
}

static int i16(const uint8_t *p)
{
    return (int)(int16_t)((uint16_t)(p[0] | (p[1] << 8)));
}

static int32_t i32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

/* ---------------- logging ---------------- */
static char s_logbuf[2048];
static int  s_logn;

static void log_flush(void)
{
    if (s_logn <= 0) return;
    const struct canopus_fs_obj *fs = (s_ctx != 0) ? s_ctx->fs : 0;
    if (fs != 0 && fs->open != 0 && fs->write != 0 && fs->close != 0) {
        int32_t fd = fs->open(s_ctx, LOG_PATH, CANOPUS_FS_WRITE | CANOPUS_FS_APPEND);
        if (fd >= 0) {
            (void)fs->write(s_ctx, fd, s_logbuf, (uint32_t)s_logn);
            (void)fs->close(s_ctx, fd);
        }
    }
    s_logn = 0;
}

static void log_raw(const char *s, int n)
{
    if (n <= 0) { while (s && s[n]) n++; }
    if (n > 512) n = 512;
    if (s_logn + n > (int)sizeof(s_logbuf) - 2) log_flush();
    for (int i = 0; i < n; i++) s_logbuf[s_logn++] = s[i];
}

static void log_kv(const char *k, uint32_t v)
{
    char vb[12]; int n = 0;
    while (k[n]) n++;
    log_raw(k, n);
    u32_to_dec(vb, v);
    n = 0; while (vb[n]) n++;
    log_raw(vb, n);
    log_raw("\r\n", 2);
}

static void log_text(const char *tag, uint32_t a, const char *txt)
{
    int n = 0;
    while (tag[n]) n++;
    log_raw(tag, n);
    char vb[12];
    u32_to_dec(vb, a);
    n = 0; while (vb[n]) n++;
    log_raw(vb, n);
    log_raw(" ", 1);
    n = 0; while (txt && txt[n]) n++;
    log_raw(txt, n);
    log_raw("\r\n", 2);
}

static void log_line(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    log_raw(s, n);
    log_raw("\r\n", 2);
}

/* ---------------- string access ---------------- */
static int str_get(uint32_t off, char *out, int max)
{
    uint32_t lo = 0, hi = s_strn;
    const struct canopus_fs_obj *fs = (s_ctx != 0) ? s_ctx->fs : 0;
    if (fs == 0 || fs->open == 0 || fs->read == 0 || fs->seek == 0 ||
        fs->close == 0 || s_strn == 0u) return -1;
    while (lo < hi) {
        uint32_t mid = udiv32(lo + hi, 2u);
        if (s_strtab[mid] < off) lo = mid + 1u;
        else hi = mid;
    }
    if (lo >= s_strn || s_strtab[lo] != off) return -1;
    uint32_t end = (lo + 1u < s_strn) ? s_strtab[lo + 1u] : s_str_len;
    uint32_t len = end - off;
    if (len > (uint32_t)(max - 1)) len = (uint32_t)(max - 1);
    /* per-call open/close: no service fd stays open across the run, so
     * raw-VFS log writes never overlap a framework handle */
    int32_t fd = fs->open(s_ctx, BIN_PATH_A, CANOPUS_FS_READ);
    if (fd < 0) return -1;
    fs->seek(s_ctx, fd, (int64_t)(s_str_off + off), CANOPUS_FS_SEEK_SET);
    int n = fs->read(s_ctx, fd, out, len);
    fs->close(s_ctx, fd);
    if (n < 0) return -1;
    out[n] = '\0';
    return n;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* chunked read via the Canopus FS service. Raw firmware read() is ACCESS-
 * denied for module tasks inside /data/canopus; ctx->fs is the sanctioned
 * path and /data/bsvm.bin sits outside the guarded area. 4KB chunks: large
 * single transfers appear to block the service (watchdog reboot at 32KB). */
static int read_full(const struct canopus_fs_obj *fs, int32_t fd, uint8_t *buf, uint32_t len)
{
    uint32_t got = 0;
    while (got < len) {
        uint32_t want = len - got;
        if (want > 4096u) want = 4096u;
        int r = fs->read(s_ctx, fd, buf + got, want);
        if (r <= 0) return -1;
        got += (uint32_t)r;
    }
    return (int)got;
}

/* ---------------- load ---------------- */
static uint32_t s_probe_size;    /* size of the opened game-data file */
static uint32_t s_strn_raw, s_mapn_raw, s_dir_len_v;  /* raw parse values for diagnostics */
static int      s_e1, s_e2;      /* errno of failed header reads */
static uint32_t s_hdr_r;         /* bytes returned by first header read */
static int32_t  s_open_e;        /* errno if even open failed */

/* progress marker written through the FS service itself (no raw-VFS ops
 * while a service fd is open). After a crash the file holds the last step
 * attempted: 1 open 2 size 3 hdr 4 parse 5 evt 6 db 7 dir 8 loaded
 * 11 strtab 12 ce 13 maps 14 troop-walk 9 done */
static void ld_mark(const struct canopus_fs_obj *fs, int n)
{
    char buf[16];
    if (fs == 0 || fs->write_all == 0) return;
    buf[0] = 'M';
    u32_to_dec(buf + 1, (uint32_t)n);
    int len = 0;
    while (buf[len]) len++;
    fs->write_all(s_ctx, "/data/bsvm_ld.txt", buf, (uint32_t)len);
}

static int bsvm_load(void)
{
    uint8_t hdr[38];
    const struct canopus_fs_obj *fs = (s_ctx != 0) ? s_ctx->fs : 0;
    if (fs == 0 || fs->open == 0 || fs->read == 0 || fs->seek == 0 || fs->size == 0 || fs->close == 0) return -9;
    ld_mark(fs, 1);
    int32_t fd = fs->open(s_ctx, BIN_PATH_A, CANOPUS_FS_READ);
    if (fd < 0) { s_open_e = fd; return -1; }
    ld_mark(fs, 2);
    int64_t fsize = fs->size(s_ctx, fd);
    s_probe_size = (fsize >= 0 && fsize <= 0x7FFFFFF0ll) ? (uint32_t)fsize : 0xFFFFFFFFu;
    ld_mark(fs, 3);
    int r = fs->read(s_ctx, fd, hdr, 38);
    s_hdr_r = (uint32_t)r;
    if (r <= 0) {
        s_e1 = (r < 0) ? (int)(-r) : 0;
        /* retry with a static-pool buffer: distinguishes buffer-access
         * failures from fd-level read failure */
        int r2 = fs->read(s_ctx, fd, s_evt, 38);
        if (r2 > 0) {
            for (int i = 0; i < 38; i++) hdr[i] = s_evt[i];
            r = r2;
            s_e2 = 0;
        } else {
            s_e2 = (r2 < 0) ? (int)(-r2) : 0;
            fs->close(s_ctx, fd);
            return (r == 0 && r2 == 0) ? -2 : -8;
        }
    }
    if (hdr[0] != 'B' || hdr[1] != 'S' || hdr[2] != 'V' || hdr[3] != 'M') { fs->close(s_ctx, fd); return -3; }
    ld_mark(fs, 4);
    s_str_off = rd32(hdr + 6);
    s_str_len = rd32(hdr + 10) - s_str_off;
    uint32_t evt_off = rd32(hdr + 14);
    s_evt_len = rd32(hdr + 18) - evt_off;
    uint32_t db_off = rd32(hdr + 22);
    s_db_len = rd32(hdr + 26) - db_off;
    uint32_t dir_off = rd32(hdr + 30);
    uint32_t dir_end = rd32(hdr + 34);
    /* older compiler builds wrote dir_end as 0; fall back to file end */
    if (dir_end <= dir_off || dir_end > s_probe_size) dir_end = s_probe_size;
    uint32_t dir_len = dir_end - dir_off;
    s_dir_len_v = dir_len;
    if (dir_len > DIR_MAX || s_evt_len > EVT_MAX || s_db_len > DB_MAX) { fs->close(s_ctx, fd); return -4; }

    /* each section must be seeked to before reading: offsets in the header
     * are absolute file positions, and read_full starts at the fd's cursor */
    ld_mark(fs, 5);
    if (fs->seek(s_ctx, fd, (int64_t)evt_off, CANOPUS_FS_SEEK_SET) < 0) { fs->close(s_ctx, fd); return -11; }
    if (read_full(fs, fd, s_evt, s_evt_len) != (int)s_evt_len) { fs->close(s_ctx, fd); return -5; }
    ld_mark(fs, 6);
    if (fs->seek(s_ctx, fd, (int64_t)db_off, CANOPUS_FS_SEEK_SET) < 0) { fs->close(s_ctx, fd); return -12; }
    if (read_full(fs, fd, s_db, s_db_len) != (int)s_db_len) { fs->close(s_ctx, fd); return -6; }
    ld_mark(fs, 7);
    if (fs->seek(s_ctx, fd, (int64_t)dir_off, CANOPUS_FS_SEEK_SET) < 0) { fs->close(s_ctx, fd); return -13; }
    if (read_full(fs, fd, s_dir, dir_len) != (int)dir_len) { fs->close(s_ctx, fd); return -7; }
    ld_mark(fs, 8);
    /* all sections are in RAM now; close the service fd so raw-VFS log
     * writes during the run never overlap a framework handle */
    fs->close(s_ctx, fd);
    /* pool-aliasing probe: if the loader under-allocated bss, s_dir
     * overlaps s_evt and the first words match */
    log_kv("d0", rd32(s_dir));
    log_kv("e0", rd32(s_evt));
    log_kv("b0", rd32(s_db));
    log_flush();

    /* parse directory (bounds-checked; sub-markers pinpoint crashes) */
    uint32_t p = 0;
    s_strn = rd32(s_dir + p); p += 4;
    s_strn_raw = s_strn;
    if (s_strn > STRN_MAX) s_strn = STRN_MAX;
    if ((p + s_strn * 4u + 4u) > dir_len) { log_kv("f1", p); log_kv("sn", s_strn); return -10; }
    for (uint32_t i = 0; i < s_strn; i++) { s_strtab[i] = rd32(s_dir + p); p += 4; }
    ld_mark(fs, 11);
    s_ce_n = rd32(s_dir + p); p += 4;
    if (s_ce_n > 256u) s_ce_n = 256u;
    if ((p + s_ce_n * 10u) > dir_len) { log_kv("f2", p); log_kv("cn", s_ce_n); return -10; }
    for (uint32_t i = 0; i < s_ce_n; i++) {
        uint32_t id = rd32(s_dir + p); p += 4;
        s_ce_off[i] = rd32(s_dir + p); p += 4;
        s_ce_len[i] = rd16(s_dir + p); p += 2;
        (void)id;
    }
    ld_mark(fs, 12);
    uint32_t map_n = rd32(s_dir + p); p += 4;
    s_mapn_raw = map_n;
    if (map_n > 512u) map_n = 512u;
    s_pg_n = 0;
    for (uint32_t m = 0; m < map_n; m++) {
        if ((p + 8u) > dir_len) { log_kv("f3", p); return -10; }
        uint32_t mid = rd16(s_dir + p); p += 2;
        uint32_t evn = rd16(s_dir + p); p += 2;
        p += 4; /* reserved u32 */
        if ((p + evn * 12u) > dir_len) { log_kv("f4", p); log_kv("mn", map_n); log_kv("en", evn); return -10; }
        for (uint32_t e = 0; e < evn; e++) {
            p += 4; /* eid u16 + page u16 */
            uint32_t off = rd32(s_dir + p); p += 4;
            p += 4; /* size */
            if (s_pg_n < 6440u) s_pg_off[s_pg_n++] = off;
            (void)mid;
        }
    }
    ld_mark(fs, 13);
    /* db fields */
    if ((p + 32u) > dir_len) { log_kv("f5", p); return -10; }
    uint32_t skill_off = rd32(s_dir + p); p += 4;
    uint32_t state_off = rd32(s_dir + p); p += 4;
    uint32_t enemy_off = rd32(s_dir + p); p += 4;
    uint32_t troop_off = rd32(s_dir + p); p += 4;
    p += 16; /* class/actor/items/sz */
    (void)skill_off; (void)state_off; (void)enemy_off;

    /* troop pages: walk troop records in db (bounds-checked) */
    s_troop_pg_n = 0;
    uint32_t tp = troop_off;
    if (tp + 2u > s_db_len) tp = s_db_len;
    else {
        uint32_t tn = rd16(s_db + tp); tp += 2;
        if (tn > 720u) tn = 720u;
        for (uint32_t i = 0; i < tn && tp + 13u < s_db_len; i++) {
            tp += 6;                                   /* id u16 + name u32 */
            uint32_t nm = s_db[tp]; tp += 1 + nm * 6;
            if (tp + 2u > s_db_len) break;
            uint32_t np = rd16(s_db + tp); tp += 2;
            if (np > 64u) np = 64u;
            for (uint32_t j = 0; j < np; j++) {
                if (tp + 13u > s_db_len) { tp = s_db_len; break; }
                tp += 9;                               /* condition */
                uint32_t off = rd32(s_db + tp); tp += 4;
                if (s_troop_pg_n < 720u) s_troop_pg_off[s_troop_pg_n++] = off;
            }
        }
    }
    s_total_entries = s_ce_n + s_pg_n + s_troop_pg_n;
    ld_mark(fs, 14);
    ld_mark(fs, 9);
    return 0;
}

/* ---------------- interpreter ---------------- */
static void vm_reset(void)
{
    for (uint32_t i = 0; i < 73; i++) s_switches[i] = 0;
    for (uint32_t i = 0; i < 104; i++) s_vars[i] = 0;
    s_gold = 0;
    for (uint32_t i = 0; i < 512; i++) s_items[i] = 0;
    s_party_n = 0;
    s_sp = 0;
    s_steps = 0;
}

static int vm_step(void)
{
    if (s_pc >= s_evt_len) return -1;
    uint8_t op = s_evt[s_pc];
    uint32_t a = s_pc + 1;
    s_steps++;
    s_total_steps++;
    s_opcnt[op]++;

    switch (op) {
    case 0x00: /* END */
        if (s_sp == 0u) return 1;                    /* entry finished */
        s_sp--;
        s_pc = s_ret[s_sp];
        return 0;
    case 0x01: { /* TEXT: full bodies only for the first 50 entries (a full
                  * run would otherwise write megabytes to NAND) */
        char txt[96];
        uint32_t idx = rd32(s_evt + a);
        if (s_entry_idx <= 50u && str_get(idx, txt, (int)sizeof(txt)) >= 0) {
            log_text("T", 0u, txt);
        } else {
            s_text_n++;
        }
        s_pc += 5;
        return 0; }
    case 0x02: { /* CHOICE: headless picks option 0 */
        uint32_t n = s_evt[a];
        uint32_t size = 1u + n * 6u + 2u;
        s_pc += size;
        return 0; }
    case 0x03: { /* IF_SW: u16 sw, u8 val, s32 else_off */
        uint32_t sw = rd16(s_evt + a);
        uint32_t val = s_evt[a + 2];
        int32_t eo = i32(s_evt + a + 3);
        uint32_t bit = sw >> 3;
        uint8_t cur = (bit < 73u) ? ((s_switches[bit] >> (sw & 7u)) & 1u) : 0u;
        if (cur != val) s_pc = (uint32_t)((int32_t)(a + 3) + eo);
        else s_pc += 8;
        return 0; }
    case 0x04: { /* IF_VAR: u16 var, u8 op, u16 imm, s32 else_off */
        uint32_t v = rd16(s_evt + a);
        uint32_t o = s_evt[a + 2];
        uint32_t imm = rd16(s_evt + a + 3);
        int32_t eo = i32(s_evt + a + 5);
        int32_t lhs = (v < 104u) ? s_vars[v] : 0;
        int32_t rhs = (int32_t)imm;
        int cond;
        switch (o) {
        case 0: cond = (lhs >  rhs); break;
        case 1: cond = (lhs >= rhs); break;
        case 2: cond = (lhs <  rhs); break;
        case 3: cond = (lhs <= rhs); break;
        case 4: cond = (lhs == rhs); break;
        case 5: cond = (lhs != rhs); break;
        default: cond = 0; break;
        }
        if (!cond) s_pc = (uint32_t)((int32_t)(a + 5) + eo);
        else s_pc += 10;
        return 0; }
    case 0x06: { /* JMP: compiler writes an ABSOLUTE evt offset */
        int32_t target = i32(s_evt + a);
        if (target < 0 || (uint32_t)target >= s_evt_len) return -1;
        s_pc = (uint32_t)target;
        return 0; }
    case 0x07: { /* SET_SW */
        uint32_t sw = rd16(s_evt + a);
        uint32_t val = s_evt[a + 2];
        uint32_t bit = sw >> 3;
        if (bit < 73u) {
            if (val) s_switches[bit] |= (uint8_t)(1u << (sw & 7u));
            else     s_switches[bit] &= (uint8_t)~(1u << (sw & 7u));
        }
        s_pc += 4;
        return 0; }
    case 0x08: { /* SET_VAR: u16 var, u8 op, u32 imm */
        uint32_t v = rd16(s_evt + a);
        uint32_t o = s_evt[a + 2];
        int32_t imm = (int32_t)rd32(s_evt + a + 3);
        if (v < 104u) {
            switch (o) {
            case 0: s_vars[v] = imm; break;
            case 1: s_vars[v] += imm; break;
            case 2: s_vars[v] -= imm; break;
            case 3: s_vars[v] = (imm != 0) ? (int32_t)udiv32((uint32_t)s_vars[v], (uint32_t)imm) : 0; break;
            case 4: s_vars[v] = (imm != 0) ? (int32_t)((uint32_t)s_vars[v] % (uint32_t)imm) : 0; break;
            case 5: s_vars[v] = (int32_t)(udiv32(s_steps, 7u) & 0xFFFFu); break; /* rand placeholder */
            case 6: s_vars[v] = imm; break;
            default: s_vars[v] = imm; break;
            }
        }
        s_pc += 8;
        return 0; }
    case 0x09: { /* ADD_ITEM: u8 kind, u16 id, u16 n */
        uint32_t kind = s_evt[a];
        uint32_t id = rd16(s_evt + a + 1);
        int32_t n = i16(s_evt + a + 3);
        uint32_t key = kind * 128u + (id % 128u);
        if (key < 512u) {
            int32_t cur = (int32_t)s_items[key] + n;
            s_items[key] = (cur < 0) ? 0u : (cur > 999) ? 999u : (uint16_t)cur;
        }
        s_pc += 6;
        return 0; }
    case 0x0A: { /* CALL_CE */
        uint32_t cid = rd16(s_evt + a);
        if (cid < s_ce_n && s_sp < 7u) {
            s_ret[s_sp++] = s_pc + 3;
            s_pc = s_ce_off[cid];
        } else {
            s_pc += 3;
        }
        return 0; }
    case 0x0B: { /* BATTLE: headless = always win */
        s_battle_result = 0;
        s_pc += 5;
        return 0; }
    case 0x0C: { /* PICTURE */
        char name[96];
        uint32_t idx = rd32(s_evt + a + 1);
        str_get(idx, name, (int)sizeof(name));
        s_pc += 8;
        return 0; }
    case 0x0D: s_pc += 4; return 0;  /* PIC_MOVE */
    case 0x0E: s_pc += 2; return 0;  /* ERASE_PIC */
    case 0x0F: s_pc += 2; return 0;  /* FADE */
    case 0x10: s_pc += 3; return 0;  /* WAIT */
    case 0x11: s_pc += 5; return 0;  /* TRANSFER */
    case 0x12: s_pc += 3; return 0;  /* SELF_SW */
    case 0x13: s_pc += 5; return 0;  /* HP */
    case 0x14: s_pc += 5; return 0;  /* GOLD */
    case 0x15: s_pc += 5; return 0;  /* SCRIPT (skipped) */
    case 0x18: { /* IF_RES */
        uint32_t want = s_evt[a];
        int32_t eo = i32(s_evt + a + 1);
        if (s_battle_result != (int32_t)want)
            s_pc = (uint32_t)((int32_t)(a + 1) + eo);
        else s_pc += 6;
        return 0; }
    case 0x19: { /* PARTY */
        uint32_t o = s_evt[a];
        uint32_t id = rd16(s_evt + a + 1);
        if (o == 0u && s_party_n < 8u) s_party[s_party_n++] = (uint8_t)id;
        else if (o == 1u && s_party_n > 0u) s_party_n--;
        else if (o == 2u) { s_party[0] = (uint8_t)id; s_party_n = 1u; }
        s_pc += 4;
        return 0; }
    case 0x1B: s_pc += 3; return 0;  /* HEAL */
    case 0x1C: s_pc += 4; return 0;  /* PIC_MOVE alt */
    default:
        log_kv("BADOP", op);
        s_runs_bad++;
        return -1;
    }
}

/* ---------------- scheduler ---------------- */
static uint32_t s_batch_pc;      /* current entry pc */
static uint8_t  s_batch_active;

static void next_entry(void)
{
    if (s_entry_idx >= s_total_entries) {
        s_phase = 2u;
        log_flush();
        return;
    }
    uint32_t off;
    if (s_entry_idx < s_ce_n) {
        off = s_ce_off[s_entry_idx];
    } else if (s_entry_idx < s_ce_n + s_pg_n) {
        off = s_pg_off[s_entry_idx - s_ce_n];
    } else {
        off = s_troop_pg_off[s_entry_idx - s_ce_n - s_pg_n];
    }
    s_entry_idx++;
    s_total_steps += s_steps;   /* carry the just-finished entry's steps */
    if (off >= s_evt_len) { s_runs_bad++; return; }
    vm_reset();
    s_pc = off;
    s_steps = 0;
    s_batch_active = 1u;
    s_runs_done++;
}

static void run_slice(uint32_t budget)
{
    if (!s_ok_init || s_phase == 2u) return;
    if (!s_batch_active) next_entry();
    if (s_phase == 2u) return;
    for (uint32_t i = 0; i < budget; i++) {
        int r = vm_step();
        if (r != 0) {
            if (r < 0) log_kv("err-at", s_pc);
            s_batch_active = 0u;
            log_kv("END entry", s_entry_idx);
            if (s_steps > STEPS_PER_ENTRY) log_kv("LONG", s_steps);
            next_entry();
            if (s_phase == 2u) return;
            /* flush only when the buffer is near full or every 256 entries:
             * per-entry NAND appends starved the firmware watchdog */
            if (s_logn > 1600 || (s_entry_idx % 256u) == 0u) log_flush();
            continue;
        }
        if (s_steps > STEPS_PER_ENTRY) {
            s_batch_active = 0u;
            log_kv("LIMIT entry", s_entry_idx);
            next_entry();
            if (s_phase == 2u) return;
            if (s_logn > 1600 || (s_entry_idx % 256u) == 0u) log_flush();
            break;
        }
    }
    /* status line */
    char vb[12];
    s_status[0] = 'E';
    u32_to_dec(vb, s_entry_idx);
    int m = 2, n = 0;
    while (vb[n]) s_status[m++] = vb[n++];
    s_status[m++] = ' ';
    s_status[m++] = 'T';
    s_status[m++] = ' ';
    u32_to_dec(vb, s_text_n);
    n = 0; while (vb[n]) s_status[m++] = vb[n++];
    s_status[m++] = ' ';
    s_status[m++] = 'B';
    s_status[m++] = ' ';
    u32_to_dec(vb, s_runs_bad);
    n = 0; while (vb[n] && m < (int)sizeof(s_status) - 2) s_status[m++] = vb[n++];
    s_status[m] = '\0';
}

/* ---------------- UI v0.3: auto story player ----------------
 * Display-only first step: pick one story entry, walk its bytecode with the
 * fixed jump semantics, and show each TEXT in a wrapped Chinese label.
 * Timer-driven auto-advance; no unverified click APIs. Every step is logged.
 */
/* quick dry-run of one entry with choice-0 auto-pick; counts TEXTs and
 * rejects entries that jump backwards (menus/loops) or never finish */
static int ui_probe_entry(uint32_t off, uint32_t end, uint32_t *texts)
{
    uint32_t pc = off, texts_n = 0u;
    *texts = 0u;
    if (end > s_evt_len) end = s_evt_len;
    for (uint32_t guard = 0u; guard < 3000u; guard++) {
        if (pc >= end || pc >= s_evt_len) return (texts_n >= 3u) ? 1 : 0;
        uint8_t op = s_evt[pc];
        uint32_t a = pc + 1u;
        if (op == 0x00u) return (texts_n >= 3u) ? 1 : 0;   /* END */
        if (op == 0x01u) { texts_n++; pc += 5u; continue; } /* TEXT */
        if (op == 0x02u) {                                  /* CHOICE pick 0 */
            uint32_t n = s_evt[a];
            if (n > 0u) {
                uint32_t field = a + 1u + 4u * n;
                int32_t rel = (int32_t)rd16(s_evt + field);
                uint32_t tgt = (uint32_t)((int32_t)field + rel);
                if (tgt < s_evt_len && tgt > pc) { pc = tgt; continue; }
                if (tgt <= pc) return 0;                    /* backward: menu */
            }
            pc += 1u + n * 6u + 2u;
            continue;
        }
        if (op == 0x06u) {                                  /* JMP absolute */
            int32_t tgt = i32(s_evt + a);
            if (tgt < 0 || (uint32_t)tgt >= s_evt_len) return 0;
            if ((uint32_t)tgt <= pc) return 0;              /* backward: loop */
            pc = (uint32_t)tgt;
            continue;
        }
        if (op == 0x03u || op == 0x04u || op == 0x18u) {    /* branches: both sides must stay forward */
            uint32_t field = pc + ((op == 0x03u) ? 4u : (op == 0x04u) ? 6u : 2u);
            if (field + 4u > s_evt_len) return 0;
            int32_t rel = i32(s_evt + field);
            uint32_t tgt = (uint32_t)((int32_t)field + rel);
            if (tgt > pc && tgt < s_evt_len) {
                /* take the jump side (false branch) as the spine */
                pc = tgt;
                continue;
            }
            /* fall-through side */
            pc += (op == 0x03u) ? 8u : (op == 0x04u) ? 10u : 6u;
            continue;
        }
        pc += 12u; /* unknown opcode: pessimistic skip keeps us moving */
    }
    return 0;
}

static void ui_pick_story(void)
{
    uint32_t best_off = 0u, best_end = 0u, best_texts = 0u;
    /* CE entries */
    for (uint32_t i = 0; i < s_ce_n; i++) {
        uint32_t off = s_ce_off[i];
        uint32_t end = off + s_ce_len[i];
        uint32_t t;
        if (ui_probe_entry(off, end, &t) && t > best_texts) {
            best_texts = t; best_off = off; best_end = end;
        }
    }
    /* map pages */
    for (uint32_t i = 0; i < s_pg_n && best_texts < 8u; i++) {
        uint32_t off = s_pg_off[i];
        uint32_t end = off + 8192u;
        uint32_t t;
        if (ui_probe_entry(off, end, &t) && t > best_texts) {
            best_texts = t; best_off = off; best_end = end;
        }
    }
    if (best_off == 0u) {                    /* fallback: biggest CE */
        uint32_t best = 0, bestlen = 0;
        for (uint32_t i = 0; i < s_ce_n; i++) {
            if (s_ce_len[i] > bestlen) { bestlen = s_ce_len[i]; best = i; }
        }
        best_off = s_ce_off[best];
        best_end = best_off + s_ce_len[best];
    }
    s_story_off = best_off;
    s_story_end = best_end;
    if (s_story_end > s_evt_len) s_story_end = s_evt_len;
    s_pc = s_story_off;
    s_ui_mode = 1u;
    log_kv("story-off", s_story_off);
    log_kv("story-end", s_story_end);
    log_kv("story-texts", best_texts);
}

static void ui_set_scene(const char *txt)
{
    int i = 0;
    while (txt[i] && i < (int)sizeof(s_scene_buf) - 1) { s_scene_buf[i] = txt[i]; i++; }
    s_scene_buf[i] = '\0';
}

static void ui_set_dlg(const char *txt)
{
    int i = 0;
    while (txt[i] && i < (int)sizeof(s_dlg_buf) - 1) { s_dlg_buf[i] = txt[i]; i++; }
    s_dlg_buf[i] = '\0';
}

/* run bytecode until a TEXT is shown or the entry ends; returns 1 if a
 * dialogue is now on screen, 0 if entry finished */
static int ui_run_until_text(void)
{
    for (uint32_t guard = 0; guard < 20000u; guard++) {
        if (s_pc >= s_story_end || s_pc >= s_evt_len) return 0;
        uint8_t op = s_evt[s_pc];
        uint32_t a = s_pc + 1u;
        if (op == 0x00u) return 0;              /* END */
        if (op == 0x01u) {                      /* TEXT */
            uint32_t idx = rd32(s_evt + a);
            char txt[240];
            if (str_get(idx, txt, (int)sizeof(txt)) >= 0) {
                ui_set_dlg(txt);
                log_line("SHOW-TEXT");
                return 1;
            }
            s_pc += 5u;
            continue;
        }
        if (op == 0x02u) {                      /* CHOICE: pick option 0 */
            uint32_t n = s_evt[a];
            if (n > 0u) {
                uint32_t field = a + 1u + 4u * n;
                int32_t rel = (int32_t)rd16(s_evt + field);
                uint32_t tgt = (uint32_t)((int32_t)field + rel);
                if (tgt < s_evt_len) { s_pc = tgt; continue; }
            }
            s_pc += 1u + n * 6u + 2u;
            continue;
        }
        /* reuse the VM step for all logic opcodes; it updates s_pc */
        int r = vm_step();
        if (r != 0) return 0;
    }
    return 0;
}

/* headless full-run: one slice per timer fire, driven by ctx->time. */
static void bsvm_tick(const struct canopus_context_v1 *ctx, int32_t timer_id,
                      void *user_data)
{
    (void)ctx; (void)timer_id; (void)user_data;
    run_slice(400u);
    if (s_phase == 2u) {
        log_line("ALL-DONE");
        /* opcode histogram: only non-zero entries, format "opNN <count>" */
        for (uint32_t op = 0u; op < 256u; op++) {
            if (s_opcnt[op] == 0u) continue;
            char key[8];
            key[0] = 'o'; key[1] = 'p';
            uint32_t v = op;
            char hex[2];
            hex[0] = (v >> 4) < 10u ? (char)('0' + (v >> 4)) : (char)('A' + (v >> 4) - 10u);
            hex[1] = (v & 15u) < 10u ? (char)('0' + (v & 15u)) : (char)('A' + (v & 15u) - 10u);
            key[2] = hex[0]; key[3] = hex[1]; key[4] = '\0';
            log_kv(key, s_opcnt[op]);
        }
        log_kv("entries", s_total_entries);
        log_kv("done", s_runs_done);
        log_kv("bad", s_runs_bad);
        log_kv("steps", s_total_steps);
        log_flush();
        if (s_ctx != 0 && s_ctx->time != 0 && s_timer_id >= 0) {
            (void)s_ctx->time->timer_destroy(s_ctx, s_timer_id);
            s_timer_id = -1;
        }
    }
}

/* ---------------- story UI: ctx->ui semantic tree ---------------- */
static uint32_t str_len(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

/* Build the current story tree. One page: title, dialogue body (auto-wrapped
 * by the backend), a progress row and a "next" action. Rendering is a pure
 * function of the story buffers, so on_open / on_event share it. */
static int32_t story_render(const struct canopus_context_v1 *ctx,
                            struct canopus_ui_context_v1 *ui)
{
    struct canopus_ui_tree_v1 *tree = 0;
    char progress[32];
    int32_t rc;
    if (ctx->ui == 0 || ui == 0) return CANOPUS_SVC_ERR_UNSUPPORTED;
    rc = ctx->ui->tree_begin(ui, &tree);
    if (rc != CANOPUS_UI_OK) return rc;
    {
        struct canopus_ui_navigation_page_props_v1 page = {
            sizeof(page), "BSVM 剧情", str_len("BSVM 剧情") };
        rc = ctx->ui->navigation_page(tree, 1u, &page);
    }
    if (rc == CANOPUS_UI_OK) {
        struct canopus_ui_text_props_v1 title = {
            sizeof(title), s_scene_buf, str_len(s_scene_buf), CANOPUS_UI_TEXT_TITLE };
        rc = ctx->ui->text(tree, 2u, &title);
    }
    if (rc == CANOPUS_UI_OK) {
        struct canopus_ui_text_props_v1 body = {
            sizeof(body), s_dlg_buf, str_len(s_dlg_buf), CANOPUS_UI_TEXT_BODY };
        rc = ctx->ui->text(tree, 3u, &body);
    }
    if (rc == CANOPUS_UI_OK) {
        progress[0] = 'E';
        u32_to_dec(progress + 1, s_entry_idx);
        {
            struct canopus_ui_status_row_props_v1 row = {
                sizeof(row), "进度", str_len("进度"), progress, str_len(progress) };
            rc = ctx->ui->status_row(tree, 4u, &row);
        }
    }
    if (rc == CANOPUS_UI_OK) {
        struct canopus_ui_action_row_props_v1 next = {
            sizeof(next), "下一句", str_len("下一句"),
            s_ui_mode == 1u ? "继续剧情" : "已播放完毕",
            str_len(s_ui_mode == 1u ? "继续剧情" : "已播放完毕"),
            EV_NEXT, s_ui_mode == 1u ? 1u : 0u };
        rc = ctx->ui->action_row(tree, 5u, &next);
    }
    if (rc == CANOPUS_UI_OK) return ctx->ui->tree_commit(tree);
    ctx->ui->tree_abort(tree);
    return rc;
}

static void log_load_failure(int rc)
{
    log_line("LOAD-FAIL");
    log_kv("code", (uint32_t)(-rc));
    log_kv("d0", rd32(s_dir));
    log_kv("e0", rd32(s_evt));
    log_kv("b0", rd32(s_db));
    log_kv("snr", s_strn_raw);
    log_kv("mnr", s_mapn_raw);
    log_flush();
}

static int32_t story_on_open(const struct canopus_context_v1 *ctx,
                             struct canopus_ui_context_v1 *ui, void *user_data)
{
    (void)user_data;
    s_ui_ctx = ui;
    if (!s_ok_init) { ui_set_dlg("游戏数据未加载"); return story_render(ctx, ui); }
    if (s_ui_mode == 0u) {
        ui_set_scene("BSVM 剧情");
        ui_pick_story();
        if (!ui_run_until_text()) {
            s_ui_mode = 3u;
            ui_set_dlg("故事播放完毕");
        }
        log_flush();
    }
    return story_render(ctx, ui);
}

static int32_t story_on_event(const struct canopus_context_v1 *ctx,
                              struct canopus_ui_context_v1 *ui,
                              const struct canopus_ui_event_v1 *event, void *user_data)
{
    (void)user_data;
    if (event->type != CANOPUS_UI_EVENT_ACTIVATE) return CANOPUS_UI_OK;
    if (event->event_id != EV_NEXT) return CANOPUS_UI_ERR_ARGUMENT;
    if (s_ui_mode == 1u) {
        if (!ui_run_until_text()) {
            s_ui_mode = 3u;
            ui_set_dlg("故事播放完毕");
            log_line("STORY-DONE");
            log_flush();
        }
    }
    return story_render(ctx, ui);
}

static int32_t story_on_back(const struct canopus_context_v1 *ctx,
                             struct canopus_ui_context_v1 *ui, void *user_data)
{
    (void)ctx; (void)ui; (void)user_data;
    return 1; /* let the page close */
}

static void story_on_close(const struct canopus_context_v1 *ctx, void *user_data)
{
    (void)user_data;
    s_ui_ctx = 0;
    log_flush();
    (void)ctx;
}

static const struct canopus_ui_entry_v1 s_story_entry = {
    sizeof(struct canopus_ui_entry_v1), "BSVM 剧情",
    story_on_open, story_on_event, story_on_back, story_on_close,
    0, 0, 0, 0,
};

/* ---------------- module lifecycle ---------------- */
static int32_t bsvm_prepare(const struct canopus_context_v1 *ctx)
{
    s_ctx = ctx;
    return 0;
}

static int32_t bsvm_activate(const struct canopus_context_v1 *ctx)
{
    s_ctx = ctx;
    s_ok_init = 0;
    log_line("BSVM activate");
    log_flush();
    int rc = bsvm_load();
    if (rc != 0) { log_load_failure(rc); return 0; }
    s_ok_init = 1u;
    log_line("BSVM start");
    log_kv("entries", s_total_entries);
    log_kv("evt", s_evt_len);
    log_kv("db", s_db_len);
    log_kv("dir", s_dir_len_v);
    log_kv("strn", s_strn);
    log_kv("ce", s_ce_n);
    log_kv("pg", s_pg_n);
    log_kv("troop", s_troop_pg_n);
    log_flush();

    /* headless escape hatch: /data/bsvm_headless.txt present => full run */
    int headless = 0;
    if (ctx->fs != 0 && ctx->fs->exists != 0) {
        headless = ctx->fs->exists(ctx, "/data/bsvm_headless.txt") > 0;
    }
    if (headless) {
        log_line("MODE headless");
        if (ctx->time != 0 && ctx->time->timer_create != 0) {
            s_timer_id = ctx->time->timer_create(ctx, 50u, 1, bsvm_tick, 0);
            if (s_timer_id >= 0 && ctx->time->timer_start != 0)
                (void)ctx->time->timer_start(ctx, s_timer_id);
            else log_line("TIMER-FAIL");
        } else log_line("NO-TIME-SERVICE");
        log_flush();
    } else {
        log_line("MODE story-ui");
        if (ctx->ui != 0 && ctx->ui->set_entry != 0) {
            int32_t urc = ctx->ui->set_entry(ctx, &s_story_entry);
            log_kv("set-entry", (uint32_t)urc);
        } else log_line("NO-UI-SERVICE");
        log_flush();
    }
    return 0;
}

static int32_t bsvm_deactivate(const struct canopus_context_v1 *ctx)
{
    if (s_timer_id >= 0 && ctx->time != 0 && ctx->time->timer_destroy != 0) {
        (void)ctx->time->timer_destroy(ctx, s_timer_id);
        s_timer_id = -1;
    }
    if (ctx->ui != 0 && ctx->ui->set_entry != 0) (void)ctx->ui->set_entry(ctx, 0);
    s_ui_ctx = 0;
    log_flush();
    return 0;
}

static int32_t bsvm_stop(const struct canopus_context_v1 *ctx)
{
    (void)bsvm_deactivate(ctx);
    s_ctx = 0;
    return 0;
}

static int32_t bsvm_query(struct canopus_status_writer_v1 *writer)
{
    (void)writer;
    return 0;
}

static const struct canopus_module_descriptor_v1 s_descriptor = {
    sizeof(struct canopus_module_descriptor_v1),
    CANOPUS_ABI_MAJOR,
    CANOPUS_ABI_MINOR,
    0u,
    BSVM_MODULE_ID,
    "0.4.0",
    "bsvm",
    CANOPUS_TARGET_ID,
    bsvm_prepare,
    bsvm_activate,
    bsvm_deactivate,
    bsvm_stop,
    bsvm_query,
    0,
    0,
};

CANOPUS_MODULE_ENTRY(s_descriptor)
