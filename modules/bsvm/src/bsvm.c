/*
 * bsvm.c — BlackSoul VM: RMVA events bytecode runtime for Band 11 (4.100.139).
 *
 * v0.1 HEADLESS: runs every event page (common events, all map pages, troop
 * pages) in sequence with auto-advance (TEXT logged, CHOICE picks 0, BATTLE
 * assumed won). Everything lands in /data/bsvm/play_log.txt. This validates
 * file IO, decoding, memory and the interpreter on real hardware BEFORE any
 * UI is attached.
 *
 * Verifier rules honored: no 64-bit division, no float math, firmware veneers
 * XOR 0x06000000 (VENA), zero imports, fixed pools only, timer-sliced work.
 */

#include "canopus_module_entry.h"
#include "canopus_memory.h"

#include <stdint.h>
#include <stddef.h>

#define BSVM_MODULE_ID "bsvm"

/* Canopus service context, captured in prepare/activate (declared early:
 * the FS service reads game data before the lifecycle section below) */
static const struct canopus_context_v1 *s_ctx;

/* ---------------- firmware veneers (139) ---------------- */
#define FW_POSIX_OPEN   0x0c342c55u
#define FW_POSIX_READ   0x0c33d785u
#define FW_POSIX_WRITE  0xc33dc4fu
#define FW_POSIX_CLOSE  0xc33818du
#define FW_LSEEK        0x0c33d511u
#define FW_LABEL_CREATE 0x0c3ab301u
#define FW_LABEL_TEXT   0x0c3b3949u
#define FW_TIMER_CREATE 0x0c3abd21u
#define FW_TIMER_DEL    0x0c3abe71u
#define FW_PAGE_GOTO    0x0c8ed781u
#define FW_OBJ_ALIGN    0x0c387789u /* void(obj, align, x, y)               */
#define ALIGN_CENTER    9u          /* lv_align_t LV_ALIGN_CENTER (lvgl v8) */
#define FW_ERRNO_LOC    0x0c349539u /* int *errno_location(void)            */

#define VENA(addr) ((uintptr_t)(addr) ^ 0x06000000u)

typedef int (*posix_open_fn)(const char *, int, ...);
typedef int (*posix_read_fn)(int, void *, uint32_t);
typedef int (*posix_write_fn)(int, const void *, uint32_t);
typedef int (*posix_close_fn)(int);
/* firmware lseek is int64_t(int, int64_t, int): the 64-bit offset travels in
 * the even-aligned r2:r3 pair per AAPCS — a 32-bit call site corrupts it */
typedef int64_t (*posix_lseek_fn)(int, int64_t, int);

#define O_RDONLY 0
#define O_RDWR   2
#define O_CREAT  4
#define SEEK_SET 0
#define SEEK_END 2

static const char BIN_PATH_A[] = "/data/bsvm.bin";                /* v0.1.6: outside /data/canopus (framework denies module reads there) */
static const char BIN_PATH_B[] = "/data/canopus/bsvm/bsvm.bin";   /* legacy probe only */
static const char LOG_PATH[] = "/data/canopus/bsvm_log.txt";      /* /data/bsvm/ does not exist on device */

/* ---------------- static pools ---------------- */
#define EVT_MAX 180000u
#define DB_MAX  116000u
#define DIR_MAX 160000u
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
static int32_t   s_fd = -1;

/* directory indices */
static uint32_t s_ce_off[256];
static uint16_t s_ce_len[256];
static uint32_t s_ce_n;
static uint32_t s_pg_off[7000];
static uint32_t s_pg_n;          /* all map pages */
static uint32_t s_troop_pg_off[1024];
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
static uint32_t s_text_n;         /* TEXT ops processed (not logged) */
static uint8_t  s_ok_init;
static uint32_t s_total_entries;

/* ui */
static void *s_label;
static void *s_timer;
static char  s_status[72];

/* ---------------- helpers ---------------- */
static void *vfn(uint32_t enc) { return (void *)(uintptr_t)VENA(enc); }

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
    int fd = ((posix_open_fn)vfn(FW_POSIX_OPEN))(LOG_PATH, O_RDWR | O_CREAT, 0666);
    if (fd >= 0) {
        ((posix_lseek_fn)vfn(FW_LSEEK))(fd, (int64_t)0, SEEK_END);
        ((posix_write_fn)vfn(FW_POSIX_WRITE))(fd, s_logbuf, (uint32_t)s_logn);
        ((posix_close_fn)vfn(FW_POSIX_CLOSE))(fd);
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

static void str_copy(char *d, const char *src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) { d[i] = src[i]; i++; }
    d[i] = '\0';
}

/* default label lands at (0,0) — fully hidden by the round bezel */
static void label_center(void *obj)
{
    if (obj != 0) {
        ((void (*)(void *, uint32_t, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_ALIGN))(
            obj, ALIGN_CENTER, 0, 0);
    }
}

/* ---------------- string access ---------------- */
static int str_get(uint32_t off, char *out, int max)
{
    uint32_t lo = 0, hi = s_strn;
    const struct canopus_fs_obj *fs = (s_ctx != 0) ? s_ctx->fs : 0;
    if (fs == 0 || fs->read == 0 || fs->seek == 0 || s_fd < 0 || s_strn == 0u) return -1;
    while (lo < hi) {
        uint32_t mid = udiv32(lo + hi, 2u);
        if (s_strtab[mid] < off) lo = mid + 1u;
        else hi = mid;
    }
    if (lo >= s_strn || s_strtab[lo] != off) return -1;
    uint32_t end = (lo + 1u < s_strn) ? s_strtab[lo + 1u] : s_str_len;
    uint32_t len = end - off;
    if (len > (uint32_t)(max - 1)) len = (uint32_t)(max - 1);
    fs->seek(s_ctx, s_fd, (int64_t)(s_str_off + off), CANOPUS_FS_SEEK_SET);
    int n = fs->read(s_ctx, s_fd, out, len);
    if (n < 0) return -1;
    out[n] = '\0';
    return n;
}

/* size probe for diagnostics: -1 missing, -2 lseek error, else size */
static int32_t probe_size(const char *path)
{
    int fd = ((posix_open_fn)vfn(FW_POSIX_OPEN))(path, O_RDONLY);
    if (fd < 0) return -1;
    int64_t sz = ((posix_lseek_fn)vfn(FW_LSEEK))(fd, (int64_t)0, SEEK_END);
    ((posix_close_fn)vfn(FW_POSIX_CLOSE))(fd);
    if (sz < 0 || sz > 0x7FFFFFF0ll) return -2;
    return (int32_t)sz;
}

static int last_errno(void)
{
    int *p = ((int *(*)(void))(uintptr_t)VENA(FW_ERRNO_LOC))();
    return (p != 0) ? *p : -1000;
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
static int      s_e1, s_e2;      /* errno of failed header reads */
static uint32_t s_hdr_r;         /* bytes returned by first header read */
static int32_t  s_open_e;        /* errno if even open failed */

static int bsvm_load(void)
{
    uint8_t hdr[38];
    const struct canopus_fs_obj *fs = (s_ctx != 0) ? s_ctx->fs : 0;
    if (fs == 0 || fs->open == 0 || fs->read == 0 || fs->seek == 0 || fs->size == 0 || fs->close == 0) return -9;
    int32_t fd = fs->open(s_ctx, BIN_PATH_A, CANOPUS_FS_READ);
    if (fd < 0) { s_open_e = fd; return -1; }
    int64_t fsize = fs->size(s_ctx, fd);
    s_probe_size = (fsize >= 0 && fsize <= 0x7FFFFFF0ll) ? (uint32_t)fsize : 0xFFFFFFFFu;
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
    if (dir_len > DIR_MAX || s_evt_len > EVT_MAX || s_db_len > DB_MAX) { fs->close(s_ctx, fd); return -4; }

    log_kv("ld1", s_evt_len);
    log_flush();
    if (read_full(fs, fd, s_evt, s_evt_len) != (int)s_evt_len) { fs->close(s_ctx, fd); return -5; }
    log_kv("ld2", s_db_len);
    log_flush();
    if (read_full(fs, fd, s_db, s_db_len) != (int)s_db_len) { fs->close(s_ctx, fd); return -6; }
    log_kv("ld3", dir_len);
    log_flush();
    if (read_full(fs, fd, s_dir, dir_len) != (int)dir_len) { fs->close(s_ctx, fd); return -7; }
    log_kv("ldok", dir_len);
    log_flush();
    /* keep fd open for string reads */
    s_fd = fd;

    /* parse directory */
    uint32_t p = 0;
    s_strn = rd32(s_dir + p); p += 4;
    if (s_strn > STRN_MAX) s_strn = STRN_MAX;
    for (uint32_t i = 0; i < s_strn; i++) { s_strtab[i] = rd32(s_dir + p); p += 4; }
    s_ce_n = rd32(s_dir + p); p += 4;
    if (s_ce_n > 256u) s_ce_n = 256u;
    for (uint32_t i = 0; i < s_ce_n; i++) {
        uint32_t id = rd32(s_dir + p); p += 4;
        s_ce_off[i] = rd32(s_dir + p); p += 4;
        s_ce_len[i] = rd16(s_dir + p); p += 2;
        (void)id;
    }
    uint32_t map_n = rd32(s_dir + p); p += 4;
    s_pg_n = 0;
    for (uint32_t m = 0; m < map_n; m++) {
        uint32_t mid = rd16(s_dir + p); p += 2;
        uint32_t evn = rd16(s_dir + p); p += 2;
        p += 4; /* reserved u32 */
        for (uint32_t e = 0; e < evn; e++) {
            p += 4; /* eid u16 + page u16 */
            uint32_t off = rd32(s_dir + p); p += 4;
            p += 4; /* size */
            if (s_pg_n < 7000u) s_pg_off[s_pg_n++] = off;
            (void)mid;
        }
    }
    /* db fields */
    uint32_t skill_off = rd32(s_dir + p); p += 4;
    uint32_t state_off = rd32(s_dir + p); p += 4;
    uint32_t enemy_off = rd32(s_dir + p); p += 4;
    uint32_t troop_off = rd32(s_dir + p); p += 4;
    p += 16; /* class/actor/items/sz */
    (void)skill_off; (void)state_off; (void)enemy_off;

    /* troop pages: walk troop records in db */
    s_troop_pg_n = 0;
    uint32_t tp = troop_off;
    uint32_t tn = rd16(s_db + tp); tp += 2;
    for (uint32_t i = 0; i < tn && tp + 8 < s_db_len; i++) {
        tp += 6;                                   /* id u16 + name u32 */
        uint32_t nm = s_db[tp]; tp += 1 + nm * 6;
        uint32_t np = rd16(s_db + tp); tp += 2;
        for (uint32_t j = 0; j < np; j++) {
            tp += 9;                               /* condition */
            uint32_t off = rd32(s_db + tp); tp += 4;
            if (s_troop_pg_n < 1024u) s_troop_pg_off[s_troop_pg_n++] = off;
        }
    }
    s_total_entries = s_ce_n + s_pg_n + s_troop_pg_n;
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
        if (cur != val) s_pc = (uint32_t)((int32_t)(a + 3) + eo + 4);
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
        if (!cond) s_pc = (uint32_t)((int32_t)(a + 5) + eo + 4);
        else s_pc += 10;
        return 0; }
    case 0x06: { /* JMP */
        int32_t rel = i32(s_evt + a);
        s_pc = (uint32_t)((int32_t)a + rel);
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
            s_pc = (uint32_t)((int32_t)(a + 1) + eo + 4);
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
    if (s_label != 0) {
        ((void (*)(void *, const char *))(uintptr_t)VENA(FW_LABEL_TEXT))(s_label, s_status);
    }
}

static void bsvm_tick(void *arg)
{
    (void)arg;
    run_slice(400u);
    if (s_phase == 2u && s_timer != 0) {
        ((void (*)(void *))(uintptr_t)VENA(FW_TIMER_DEL))(s_timer);
        s_timer = 0;
        log_line("ALL-DONE");
        log_flush();
        if (s_label != 0) {
            ((void (*)(void *, const char *))(uintptr_t)VENA(FW_LABEL_TEXT))(s_label, "BSVM OK");
        }
    }
}

/* ---------------- page lifecycle (native-app-demo pattern) ---------------- */
static int bsvm_on_signal(void *page, uint32_t event, void *payload)
{
    (void)page; (void)event; (void)payload;
    return 0;
}

static int bsvm_on_create(void *page, void *root, void *start_data)
{
    (void)page; (void)start_data;
    s_label = ((void *(*)(void *))(uintptr_t)VENA(FW_LABEL_CREATE))(root);
    label_center(s_label);
    if (s_label != 0) {
        ((void (*)(void *, const char *))(uintptr_t)VENA(FW_LABEL_TEXT))(s_label, "BSVM");
    }
    s_ok_init = 0;
    log_line("BSVM create");
    log_flush();
    int rc = bsvm_load();
    if (rc == 0) {
        s_ok_init = 1u;
        log_line("BSVM headless start");
        log_kv("entries", s_total_entries);
        log_kv("fsize", s_probe_size);
        log_kv("evt", s_evt_len);
        log_kv("db", s_db_len);
        log_kv("dir", s_dir_len);
        log_kv("strn", s_strn);
        log_flush();
        s_timer = ((void *(*)(void *, uint32_t, void *))(uintptr_t)VENA(FW_TIMER_CREATE))(
            bsvm_tick, 50u, 0);
    } else {
        /* short label: round-screen bezel clips long text */
        char msg[12];
        char vb[12];
        u32_to_dec(vb, (uint32_t)(-rc));
        msg[0] = 'L';
        str_copy(msg + 1, vb, sizeof(msg) - 1);
        if (s_label != 0) {
            ((void (*)(void *, const char *))(uintptr_t)VENA(FW_LABEL_TEXT))(s_label, msg);
        }
        log_line("LOAD-FAIL");
        log_kv("code", (uint32_t)(-rc));
        log_kv("e1", (uint32_t)s_e1);
        log_kv("e2", (uint32_t)s_e2);
        log_kv("hdrr", s_hdr_r);
        log_kv("bsize", s_probe_size);
        log_kv("alt", (uint32_t)probe_size(BIN_PATH_B));
        log_kv("koinbox", (uint32_t)probe_size("/data/canopus/inbox/bsvm.ko.next"));
        log_kv("cmi", (uint32_t)probe_size("/data/canopus/inbox/bsvm.cmi.next"));
        log_kv("opene", (uint32_t)s_open_e);
        log_flush();
    }
    return 0;
}

static int bsvm_on_resume(void *page)
{
    (void)page;
    if (s_ok_init && s_timer == 0 && s_phase != 2u) {
        s_timer = ((void *(*)(void *, uint32_t, void *))(uintptr_t)VENA(FW_TIMER_CREATE))(
            bsvm_tick, 50u, 0);
    }
    return 0;
}

static int bsvm_on_pause(void *page)
{
    (void)page;
    log_flush();
    if (s_timer != 0) {
        ((void (*)(void *))(uintptr_t)VENA(FW_TIMER_DEL))(s_timer);
        s_timer = 0;
    }
    return 0;
}

static int bsvm_on_ui_destroy(void *page)
{
    (void)page;
    log_flush();
    if (s_timer != 0) {
        ((void (*)(void *))(uintptr_t)VENA(FW_TIMER_DEL))(s_timer);
        s_timer = 0;
    }
    return 0;
}

/* ---------------- module lifecycle ---------------- */
static uint16_t s_app_id;
static const char s_display_name[] = "BSVM";

static int32_t bsvm_prepare(const struct canopus_context_v1 *ctx)
{
    s_ctx = ctx;
    return 0;
}

static int32_t bsvm_activate(const struct canopus_context_v1 *ctx)
{
    s_ctx = ctx;
    return 0;
}

static int32_t bsvm_deactivate(const struct canopus_context_v1 *ctx)
{
    (void)ctx;
    return 0;
}

static int32_t bsvm_stop(const struct canopus_context_v1 *ctx)
{
    (void)ctx;
    s_ctx = 0;
    return 0;
}

static int32_t bsvm_query(struct canopus_status_writer_v1 *writer)
{
    (void)writer;
    return 0;
}

static struct firmware_page_descriptor s_bsvm_page;

static int32_t bsvm_publish_native_app_stage(const struct canopus_context_v1 *ctx,
                                             uint32_t stage)
{
    if (stage == 1u) {
        void *pages[1];
        struct canopus_app_entry_v1 entry;
        int32_t app_id;
        if (!canopus_services_has(ctx, CANOPUS_SVC_LAUNCHER, 1) ||
            ctx->launcher == 0 || ctx->launcher->register_app == 0) {
            return CANOPUS_SVC_ERR_UNSUPPORTED;
        }
        if (s_app_id != 0u) return 0;

        canopus_memset(&s_bsvm_page, 0, sizeof(s_bsvm_page));
        s_bsvm_page.page_name = (void *)"bsvm_main";
        s_bsvm_page.page_id = 0u;
        s_bsvm_page.on_signal = (void *)bsvm_on_signal;
        s_bsvm_page.on_create = (void *)bsvm_on_create;
        s_bsvm_page.on_resume = (void *)bsvm_on_resume;
        s_bsvm_page.on_pause = (void *)bsvm_on_pause;
        s_bsvm_page.on_ui_destroy = (void *)bsvm_on_ui_destroy;

        pages[0] = &s_bsvm_page;
        canopus_memset(&entry, 0, sizeof(entry));
        entry.struct_size = sizeof(entry);
        entry.label = s_display_name;
        entry.icon = 0;
        entry.pages = pages;
        entry.page_count = 1u;
        app_id = ctx->launcher->register_app(ctx, &entry);
        if (app_id < 0) return app_id;
        s_app_id = (uint16_t)app_id;
        return 0;
    }
    if (stage == 2u) {
        if (s_app_id == 0u) return CANOPUS_SVC_ERR_STATE;
        (void)((void (*)(uint32_t, uint32_t, uint32_t, uint32_t))(
            uintptr_t)VENA(FW_PAGE_GOTO))((uint32_t)s_app_id << 16, 0u, 0u, 0u);
        return 0;
    }
    return CANOPUS_SVC_ERR_ARGUMENT;
}

static const struct canopus_module_descriptor_v1 s_descriptor = {
    sizeof(struct canopus_module_descriptor_v1),
    CANOPUS_ABI_MAJOR,
    CANOPUS_ABI_MINOR,
    CANOPUS_FLAG_HAS_NATIVE_APP | CANOPUS_FLAG_REGISTERS_LAUNCHER_ENTRY |
        CANOPUS_FLAG_APP_UNREGISTER_REBOOT_REQUIRED,
    BSVM_MODULE_ID,
    "0.1.8",
    "bsvm",
    CANOPUS_TARGET_ID,
    bsvm_prepare,
    bsvm_activate,
    bsvm_deactivate,
    bsvm_stop,
    bsvm_query,
    0,
    bsvm_publish_native_app_stage,
};

CANOPUS_MODULE_ENTRY(s_descriptor)
