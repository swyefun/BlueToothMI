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
#include "canopus_ui.h"

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
#define FW_OBJ_SIZE     0x0c387ba9u /* void(obj, w, h)                      */
#define ALIGN_CENTER    9u          /* lvgl v8 LV_ALIGN_CENTER              */
#define ALIGN_TOP_LEFT  1u          /* lvgl v8 LV_ALIGN_TOP_LEFT            */
#define FW_ERRNO_LOC    0x0c349539u /* int *errno_location(void)            */
/* UI v0.3: story display — label + long-mode + CJK font */
#define FW_LABEL_LONG   0x0c3b3f5du /* void lvx_label_set_long_mode(label, mode) */
#define LV_LABEL_LONG_WRAP 2u       /* lvgl v8: WRAP=2 (scroll=0,clip=1,wrap=2) */
/* CJK font: private-ABI calendar font port (approved, device-unproven) */
#define FW_FONT_CREATE  0x0c4950b1u /* void *calendar_font_create(family,size,style) */
#define FW_TEXT_FONT    0x0c387ebbu /* void calendar_text_font(obj, font, selector)  */

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

/* ui */
static void *s_label;
static void *s_timer;
static char  s_status[72];

/* ---- v0.5: dual path ----
 * .139 real device (Arstobox): native-app + firmware veneers, exactly the
 * v0.3.2 behavior that is verified on hardware. Other targets (.155/.179,
 * manager/emulator): ctx->ui semantic tree, ctx->fs log, ctx->time timer.
 * Chosen at activate by CANOPUS_TARGET_ID, never by service probing, so the
 * .139 path cannot be hijacked by a partially-implemented ui service. */
static uint8_t s_is_139;
static int32_t s_timer_id_ctx = -1;  /* ctx->time timer (non-.139 headless) */
enum { EV_NEXT = 1 };
static struct canopus_ui_context_v1 *s_ui_ctx; /* live semantic page */
/* manager fs maps a relative name into the module data dir
 * (/data/canopus/m.bsvm/), which is writable; absolute /data/canopus is not */
static const char LOG_NAME_CTX[] = "bsvm_log.txt";

/* ---- UI v0.3: auto story player (display-only, timer-driven) ---- */
static void *s_root;
static void *s_dlg;          /* dialogue text label (long-mode wrap) */
static void *s_scene;        /* scene / location line */
static uint8_t s_ui_mode;    /* 0 boot, 1 story, 3 done */
static char  s_dlg_buf[256];
static char  s_scene_buf[64];
static uint32_t s_story_off; /* bytecode offset of the story entry being played */
static uint32_t s_story_end; /* end offset */
static uint32_t s_dwell;     /* timer ticks before auto-advance */
static void *s_cjk_font;     /* created once, must outlive all labels */

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
    if (!s_is_139) {
        /* manager/emulator: sanctioned fs service, relative name lands in
         * the module data dir; WRITE|APPEND keeps existing content */
        const struct canopus_fs_obj *fs = (s_ctx != 0) ? s_ctx->fs : 0;
        if (fs != 0 && fs->open != 0 && fs->write != 0 && fs->close != 0) {
            int32_t fd = fs->open(s_ctx, LOG_NAME_CTX,
                                  CANOPUS_FS_WRITE | CANOPUS_FS_APPEND);
            if (fd >= 0) {
                (void)fs->write(s_ctx, fd, s_logbuf, (uint32_t)s_logn);
                (void)fs->close(s_ctx, fd);
            }
        }
    } else {
        /* .139 device: raw posix veneers (verified on hardware); the
         * installer-face log viewer reads this exact path */
        int fd = ((posix_open_fn)vfn(FW_POSIX_OPEN))(LOG_PATH, O_RDWR | O_CREAT, 0666);
        if (fd >= 0) {
            ((posix_lseek_fn)vfn(FW_LSEEK))(fd, (int64_t)0, SEEK_END);
            ((posix_write_fn)vfn(FW_POSIX_WRITE))(fd, s_logbuf, (uint32_t)s_logn);
            ((posix_close_fn)vfn(FW_POSIX_CLOSE))(fd);
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

/* UI: label at absolute position, with optional long-text wrap */
static void *ui_label(void *root, int32_t x, int32_t y, int32_t w, int32_t h, int wrap)
{
    void *obj = ((void *(*)(void *))(uintptr_t)VENA(FW_LABEL_CREATE))(root);
    if (obj == 0) return 0;
    ((void (*)(void *, uint32_t, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_ALIGN))(
        obj, ALIGN_TOP_LEFT, x, y);
    if (w > 0) {
        ((void (*)(void *, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_SIZE))(obj, w, h);
    }
    if (wrap) {
        ((void (*)(void *, uint32_t))(uintptr_t)VENA(FW_LABEL_LONG))(
            obj, LV_LABEL_LONG_WRAP);
    }
    if (s_cjk_font != 0) {
        ((void (*)(void *, const void *, uint32_t))(uintptr_t)VENA(FW_TEXT_FONT))(
            obj, s_cjk_font, 0u);
    }
    return obj;
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
    if (s_label != 0) {
        ((void (*)(void *, const char *))(uintptr_t)VENA(FW_LABEL_TEXT))(s_label, s_status);
    }
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
    s_dwell = 0u;
    log_kv("story-off", s_story_off);
    log_kv("story-end", s_story_end);
    log_kv("story-texts", best_texts);
}

static void ui_set_scene(const char *txt)
{
    int i = 0;
    while (txt[i] && i < (int)sizeof(s_scene_buf) - 1) { s_scene_buf[i] = txt[i]; i++; }
    s_scene_buf[i] = '\0';
    if (s_scene != 0) {
        ((void (*)(void *, const char *))(uintptr_t)VENA(FW_LABEL_TEXT))(s_scene, s_scene_buf);
    }
}

static void ui_set_dlg(const char *txt)
{
    int i = 0;
    while (txt[i] && i < (int)sizeof(s_dlg_buf) - 1) { s_dlg_buf[i] = txt[i]; i++; }
    s_dlg_buf[i] = '\0';
    if (s_dlg != 0) {
        ((void (*)(void *, const char *))(uintptr_t)VENA(FW_LABEL_TEXT))(s_dlg, s_dlg_buf);
    }
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

static void ui_tick(void *arg)
{
    (void)arg;
    if (s_ui_mode != 1u) return;
    if (s_dwell > 0u) {                 /* dwell on the current dialogue */
        s_dwell--;
        return;
    }
    int showing = ui_run_until_text();
    if (showing) {
        s_dwell = 40u;                  /* ~2s at 50ms per tick */
    } else {
        s_ui_mode = 3u;
        ui_set_dlg("故事播放完毕");
        log_line("STORY-DONE");
        log_flush();
        if (s_timer != 0) {
            ((void (*)(void *))(uintptr_t)VENA(FW_TIMER_DEL))(s_timer);
            s_timer = 0;
        }
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
        s_root = root;
        log_line("BSVM start");
        log_kv("entries", s_total_entries);
        log_kv("fsize", s_probe_size);
        log_kv("evt", s_evt_len);
        log_kv("db", s_db_len);
        log_kv("dir", s_dir_len_v);
        log_kv("strn", s_strn);
        /* headless escape hatch: /data/bsvm_headless.txt present => old full run */
        {
            int hfd = ((posix_open_fn)vfn(FW_POSIX_OPEN))("/data/bsvm_headless.txt", O_RDONLY);
            if (hfd >= 0) {
                ((posix_close_fn)vfn(FW_POSIX_CLOSE))(hfd);
                log_line("MODE headless");
                log_flush();
                s_timer = ((void *(*)(void *, uint32_t, void *))(uintptr_t)VENA(FW_TIMER_CREATE))(
                    bsvm_tick, 50u, 0);
            } else {
                log_line("MODE story-ui");
                /* CJK font: watchfaces render Chinese with MiSans; the native
                 * default font has no CJK glyphs (boxes on device) */
                log_line("font-create");
                log_flush();
                s_cjk_font = ((void *(*)(const char *, uint16_t, uint16_t))(uintptr_t)VENA(FW_FONT_CREATE))(
                    "MiSans-Regular", 18u, 0u);
                log_kv("font", (uint32_t)(uintptr_t)s_cjk_font);
                /* build the UI: scene line + wrapped dialogue label; widths
                 * stay inside the ~192px screen so page gestures keep working */
                s_scene = ui_label(root, 14, 96, 164, 30, 0);
                s_dlg   = ui_label(root, 14, 150, 164, 260, 1);
                ui_set_scene("BSVM 剧情");
                ui_pick_story();
                log_flush();
                s_timer = ((void *(*)(void *, uint32_t, void *))(uintptr_t)VENA(FW_TIMER_CREATE))(
                    ui_tick, 50u, 0);
            }
        }
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
        /* pool-content probes: expected d0=7195 dm=147 de=0 e0=21 b0=65873.
         * d0==e0 => bss aliasing; d0 ok but dm wrong => chunked-read corruption */
        log_kv("d0", rd32(s_dir));
        log_kv("dm", rd32(s_dir + 31268u));
        log_kv("de", rd32(s_dir + ((s_dir_len_v > 4u) ? (s_dir_len_v - 4u) : 0u)));
        log_kv("e0", rd32(s_evt));
        log_kv("b0", rd32(s_db));
        log_kv("snr", s_strn_raw);
        log_kv("mnr", s_mapn_raw);
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

/* ---------------- v0.5: manager/emulator path (ctx->ui semantic tree) ---------------- */
static uint32_t str_len(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

static uint8_t target_is_139(void)
{
    const char *t = CANOPUS_TARGET_ID;
    const char *m = "xiaomi-band-11-4.100.139";
    uint32_t i = 0;
    while (m[i]) {
        if (t[i] != m[i]) return 0;
        i++;
    }
    return t[i] == '\0';
}

/* headless full-run on manager targets: one slice per ctx->time fire */
static void bsvm_tick_ctx(const struct canopus_context_v1 *ctx, int32_t timer_id,
                          void *user_data)
{
    (void)timer_id; (void)user_data;
    run_slice(400u);
    if (s_phase == 2u) {
        log_line("ALL-DONE");
        for (uint32_t op = 0u; op < 256u; op++) {
            if (s_opcnt[op] == 0u) continue;
            char key[8];
            key[0] = 'o'; key[1] = 'p';
            uint32_t v = op;
            key[2] = (v >> 4) < 10u ? (char)('0' + (v >> 4)) : (char)('A' + (v >> 4) - 10u);
            key[3] = (v & 15u) < 10u ? (char)('0' + (v & 15u)) : (char)('A' + (v & 15u) - 10u);
            key[4] = '\0';
            log_kv(key, s_opcnt[op]);
        }
        log_kv("entries", s_total_entries);
        log_kv("done", s_runs_done);
        log_kv("bad", s_runs_bad);
        log_kv("steps", s_total_steps);
        log_flush();
        if (s_timer_id_ctx >= 0 && ctx->time != 0 && ctx->time->timer_destroy != 0) {
            (void)ctx->time->timer_destroy(ctx, s_timer_id_ctx);
            s_timer_id_ctx = -1;
        }
    }
}

/* Build the current story tree. Pure function of the story buffers, so
 * on_open / on_event share it. Always renders something (even a load-error
 * notice) — a visible page is the anti-black-screen guarantee. */
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
static uint16_t s_app_id;
static const char s_display_name[] = "BSVM";

static int32_t bsvm_prepare(const struct canopus_context_v1 *ctx)
{
    s_ctx = ctx;
    s_is_139 = target_is_139();
    return 0;
}

static int32_t bsvm_activate(const struct canopus_context_v1 *ctx)
{
    s_ctx = ctx;
    s_is_139 = target_is_139();
    log_line("BSVM activate");
    log_kv("t139", s_is_139);
    log_flush();

    if (s_is_139) return 0;   /* .139: load happens in on_create (v0.3.2 flow) */

    /* manager/emulator: load here and ALWAYS publish the semantic page,
     * so a missing data file shows an error page instead of a black screen */
    s_ok_init = 0;
    int rc = bsvm_load();
    if (rc == 0) {
        s_ok_init = 1u;
        log_line("BSVM start");
        log_kv("entries", s_total_entries);
        log_kv("evt", s_evt_len);
        log_kv("db", s_db_len);
        log_kv("dir", s_dir_len_v);
        log_kv("strn", s_strn);
    } else {
        log_load_failure(rc);
    }

    /* headless escape hatch: /data/bsvm_headless.txt present => full run */
    int headless = 0;
    if (ctx->fs != 0 && ctx->fs->exists != 0) {
        headless = ctx->fs->exists(ctx, "/data/bsvm_headless.txt") > 0;
    }
    if (headless && s_ok_init) {
        log_line("MODE headless");
        if (ctx->time != 0 && ctx->time->timer_create != 0) {
            s_timer_id_ctx = ctx->time->timer_create(ctx, 50u, 1, bsvm_tick_ctx, 0);
            if (s_timer_id_ctx >= 0 && ctx->time->timer_start != 0)
                (void)ctx->time->timer_start(ctx, s_timer_id_ctx);
            else log_line("TIMER-FAIL");
        } else {
            log_line("NO-TIME-SERVICE");
        }
    } else if (headless) {
        log_line("HEADLESS-NODATA");
    } else {
        log_line("MODE story-ui");
    }
    if (ctx->ui != 0 && ctx->ui->set_entry != 0) {
        int32_t urc = ctx->ui->set_entry(ctx, &s_story_entry);
        log_kv("set-entry", (uint32_t)urc);
    } else {
        log_line("NO-UI-SERVICE");
    }
    log_flush();
    return 0;
}

static int32_t bsvm_deactivate(const struct canopus_context_v1 *ctx)
{
    if (!s_is_139) {
        if (s_timer_id_ctx >= 0 && ctx->time != 0 && ctx->time->timer_destroy != 0) {
            (void)ctx->time->timer_destroy(ctx, s_timer_id_ctx);
            s_timer_id_ctx = -1;
        }
        if (ctx->ui != 0 && ctx->ui->set_entry != 0) (void)ctx->ui->set_entry(ctx, 0);
        s_ui_ctx = 0;
    }
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

static struct firmware_page_descriptor s_bsvm_page;

static int32_t bsvm_publish_native_app_stage(const struct canopus_context_v1 *ctx,
                                             uint32_t stage)
{
    if (stage == 1u) {
        /* non-.139 (manager/emulator): no launcher app — the page callbacks
         * below drive .139 firmware veneers and would crash on other targets.
         * UI is published via ctx->ui->set_entry in activate instead. */
        if (!s_is_139) return CANOPUS_SVC_ERR_UNSUPPORTED;
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
    "0.3.2",
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
