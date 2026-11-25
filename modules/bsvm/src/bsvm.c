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
#include "bsvm_prologue.h"
#include "bsvm_atrects.h"

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
/* UI v0.7: title screen — addresses cross-checked against the manager's own
 * band11 mappings (target_config.rs) and the 139 symbol table */
#define FW_IMAGE_CREATE 0x0c3ae1d1u /* void *lv_image_create(parent)                 */
#define FW_IMAGE_SET_SRC 0x0c3b2c29u /* void lv_image_set_src(obj, const char *path) */
#define FW_OBJ_ADD_FLAG 0x0c38404du /* void lvx_object_add_flag(obj, flag)           */
#define FW_OBJ_CLEAR_FLAG 0x0c3840d9u /* void lv_obj_clear_flag(obj, flag)           */
#define FW_EVENT_ADD    0x0c37fc1du /* void *lv_obj_add_event_cb(obj,cb,code,ud)     */
#define FW_PAGE_FINISH  0x0c69b5adu /* int32 page_finish(struct page_descriptor *)   */
#define LV_FLAG_HIDDEN    1u        /* firmware family: HIDDEN=0x01, SCROLLABLE=0x10 */
#define LV_FLAG_CLICKABLE 2u        /* (manager host_set_hidden/host_object_flag)    */
#define LV_EVENT_CLICKED  7u        /* manager HOST_EVENT_CLICKED                    */
#define TITLE_PATH      "/data/bsvm_title.bin"

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

/* ---- UI v0.7: title screen (original Scene_Title layout) ----
 * mist.png stretched full-screen + "BLACK SOULS" (upper half, centered) +
 * command rows NEW GAME / CONTINUE / END below center, exactly the stock
 * scene: title1_name empty (black under-layer), title2_name "Mist",
 * opt_draw_title, Window_TitleCommand at (width-160)/2, y=(h*1.6-w)/2.
 * CONTINUE is shown but inert: no save system on the band yet (the stock
 * code also adds it disabled when no save exists). Tap NEW GAME or wait —
 * a 12s countdown auto-starts, so the game runs even if page clicks never
 * reach module objects on this firmware. */
static void *s_title_img;
static void *s_title_lbl;
static void *s_menu_ng;
static void *s_menu_cont;
static void *s_menu_end;
static void *s_title_font;
static uint8_t s_started;       /* 0 title, 1 story running */
static uint32_t s_countdown;    /* title auto-start seconds */
struct firmware_page_descriptor;
static struct firmware_page_descriptor s_bsvm_page;  /* fwd: defined below */

/* ---- UI v0.3: auto story player (display-only, timer-driven) ---- */
static void *s_root;
static void *s_dlg;          /* dialogue text label (long-mode wrap) */
static void *s_scene;        /* scene / location line */
static uint32_t s_dwell;     /* story picker dwell ticks (non-.139 path) */
static uint8_t s_ui_mode;    /* 0 boot, 1 story, 3 done */
static char  s_dlg_buf[256];
static char  s_scene_buf[64];
static uint32_t s_story_off; /* bytecode offset of the story entry being played */
static uint32_t s_story_end; /* end offset */
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
static void *ui_label(void *root, int32_t x, int32_t y, int32_t w, int32_t h,
                      int wrap, void *font)
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
    if (font != 0) {
        ((void (*)(void *, const void *, uint32_t))(uintptr_t)VENA(FW_TEXT_FONT))(
            obj, font, 0u);
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
/* dry-run one entry with the REAL VM semantics (choice 0, battle won,
 * default switches/vars) and count TEXT ops on the executed path.
 * No string fetches (zero fs churn). Budget caps runaway menus/loops.
 * Because this is the same code path playback uses, the count cannot
 * diverge from what will actually be shown — the v0.5 static probe did. */
static uint32_t story_dry_run(uint32_t off, uint32_t end, uint32_t budget)
{
    uint32_t texts = 0u, steps = 0u;
    if (end > s_evt_len) end = s_evt_len;
    vm_reset();
    s_pc = off;
    while (steps < budget) {
        if (s_pc >= end || s_pc >= s_evt_len) break;
        uint8_t op = s_evt[s_pc];
        uint32_t a = s_pc + 1u;
        if (op == 0x00u) {                       /* END */
            if (s_sp == 0u) break;
            s_sp--;
            s_pc = s_ret[s_sp];
            continue;
        }
        steps++;
        if (op == 0x01u) { texts++; s_pc += 5u; continue; }  /* TEXT */
        if (op == 0x02u) {                       /* CHOICE: pick option 0 */
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
        if (vm_step() != 0) break;
    }
    return texts;
}

static void ui_pick_story(void)
{
    uint32_t best_off = 0u, best_end = 0u, best_texts = 0u;
    int32_t best_ce = -1;
    /* CE entries: dry-run each, keep the one with the most real TEXTs */
    for (uint32_t i = 0; i < s_ce_n; i++) {
        uint32_t off = s_ce_off[i];
        uint32_t t = story_dry_run(off, off + s_ce_len[i], 2000u);
        if (t > best_texts) {
            best_texts = t; best_off = off;
            best_end = off + s_ce_len[i]; best_ce = (int32_t)i;
        }
    }
    /* map pages: only when no CE carries a real story */
    for (uint32_t i = 0; i < s_pg_n && best_texts < 8u; i++) {
        uint32_t off = s_pg_off[i];
        uint32_t t = story_dry_run(off, off + 8192u, 2000u);
        if (t > best_texts) {
            best_texts = t; best_off = off;
            best_end = off + 8192u; best_ce = -1;
        }
    }
    if (best_off == 0u) {                    /* fallback: biggest CE */
        uint32_t best = 0, bestlen = 0;
        for (uint32_t i = 0; i < s_ce_n; i++) {
            if (s_ce_len[i] > bestlen) { bestlen = s_ce_len[i]; best = i; }
        }
        best_off = s_ce_off[best];
        best_end = best_off + s_ce_len[best];
        best_ce = (int32_t)best;
    }
    s_story_off = best_off;
    s_story_end = best_end;
    if (s_story_end > s_evt_len) s_story_end = s_evt_len;
    vm_reset();                              /* dry runs mutated VM state */
    s_pc = s_story_off;
    s_ui_mode = 1u;
    s_dwell = 0u;
    log_kv("story-ce", (uint32_t)best_ce);
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
            s_pc += 5u;                         /* advance FIRST: the next call
                                                 * continues after this line */
            if (str_get(idx, txt, (int)sizeof(txt)) >= 0) {
                ui_set_dlg(txt);
                log_line("SHOW-TEXT");
                return 1;
            }
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

static void prologue_start(void);

static void obj_hide(void *obj)
{
    if (obj != 0) {
        ((void (*)(void *, uint32_t))(uintptr_t)VENA(FW_OBJ_ADD_FLAG))(
            obj, LV_FLAG_HIDDEN);
    }
}

static void obj_show(void *obj)
{
    if (obj != 0) {
        ((void (*)(void *, uint32_t))(uintptr_t)VENA(FW_OBJ_CLEAR_FLAG))(
            obj, LV_FLAG_HIDDEN);
    }
}

static void ui_show(void *obj, int show)
{
    if (show) obj_show(obj);
    else obj_hide(obj);
}

static void cb_continue(void *e)
{
    (void)e;
    /* stock scene: disabled when no save exists - band has no save system */
    log_line("TITLE-CONTINUE");
    log_flush();
}

static void cb_end(void *e)
{
    (void)e;
    log_line("TITLE-END");
    log_flush();
    (void)((int32_t (*)(void *))(uintptr_t)VENA(FW_PAGE_FINISH))(&s_bsvm_page);
}

static void title_tick(void *arg)
{
    (void)arg;
    if (s_started) return;
    if (s_countdown > 0u) { s_countdown--; return; }
    log_line("TITLE-AUTOSTART");
    log_flush();
    prologue_start();
}

static void cb_new_game(void *e)
{
    (void)e;
    if (s_started) return;
    log_line("TITLE-NEWGAME");
    log_flush();
    prologue_start();
}

/* ---------------- v0.8 prologue engine (Map097 character make) ----------------
 * Map view (pre-rendered /data/bsvm_map.bin, knight baked in) + RMVA-style
 * textbox (/data/bsvm_box.bin) + alice portrait (/data/bsvm_face.bin).
 * Steps come from bsvm_prologue.h (verbatim Map097 EV001 page0 texts).
 * Click map/box advances; TEXT steps auto-advance after 4s; choices are
 * tappable rows; name input uses an on-band letter keyboard (the firmware
 * IME page is not exposed to modules - no symbols in the 139 table).
 * Origin choice: every option yields the knight, per spec. */
static void *s_map_img;
static void *s_box_img;
static void *s_face_img;
static void *s_plate_lbl;
static void *s_ptext_lbl;
static void *s_card_lbl;
static void *s_name_lbl;
static void *s_choice_lbl[4];
static void *s_key_lbl[28];
static void *s_pro_font;
static uint8_t  s_pro_on;    /* prologue running */
static uint8_t  s_pro_kb;    /* keyboard visible */
static uint16_t s_pro_idx;   /* current step index */
static uint8_t  s_pro_wait;  /* auto-advance countdown, TEXT only */
static uint8_t  s_pname_n;
static char     s_pname[24];
static char     s_ptextbuf[512];

#define MAP_PATH   "/data/bsvm_map.bin"
#define BOX_PATH   "/data/bsvm_box.bin"
#define FACE_PATH  "/data/bsvm_face.bin"
#define BOX_Y      404
#define PNAME_DEFAULT "\xe6\xa0\xbc\xe6\x9e\x97"  /* 格林 */

static void prologue_show_step(void);
static int map_compose(void);
static void prologue_tick(void *arg);

static void prologue_key(int n);
static void prologue_pick(int n);

#define PKEY_FN(n) static void cb_key_##n(void *e) { (void)e; prologue_key(n); }
PKEY_FN(0)  PKEY_FN(1)  PKEY_FN(2)  PKEY_FN(3)
PKEY_FN(4)  PKEY_FN(5)  PKEY_FN(6)  PKEY_FN(7)
PKEY_FN(8)  PKEY_FN(9)  PKEY_FN(10) PKEY_FN(11)
PKEY_FN(12) PKEY_FN(13) PKEY_FN(14) PKEY_FN(15)
PKEY_FN(16) PKEY_FN(17) PKEY_FN(18) PKEY_FN(19)
PKEY_FN(20) PKEY_FN(21) PKEY_FN(22) PKEY_FN(23)
PKEY_FN(24) PKEY_FN(25) PKEY_FN(26) PKEY_FN(27)
#undef PKEY_FN
static void (*const s_key_cbs[28])(void *) = {
    cb_key_0,  cb_key_1,  cb_key_2,  cb_key_3,  cb_key_4,  cb_key_5,  cb_key_6,
    cb_key_7,  cb_key_8,  cb_key_9,  cb_key_10, cb_key_11, cb_key_12, cb_key_13,
    cb_key_14, cb_key_15, cb_key_16, cb_key_17, cb_key_18, cb_key_19, cb_key_20,
    cb_key_21, cb_key_22, cb_key_23, cb_key_24, cb_key_25, cb_key_26, cb_key_27,
};

#define PCHOICE_FN(n) static void cb_choice_##n(void *e) { (void)e; prologue_pick(n); }
PCHOICE_FN(0) PCHOICE_FN(1) PCHOICE_FN(2) PCHOICE_FN(3)
static void (*const s_choice_cbs[4])(void *) = {
    cb_choice_0, cb_choice_1, cb_choice_2, cb_choice_3,
};

static void cb_advance(void *e)
{
    (void)e;
    if (!s_pro_on || s_pro_kb) return;
    if (s_prologue[s_pro_idx].type != PSTEP_TEXT) return;
    log_line("PRO-ADVANCE");
    s_pro_wait = 0;
    s_pro_idx++;
    prologue_show_step();
}

static void prologue_hide_all(void)
{
    obj_hide(s_map_img);
    obj_hide(s_box_img);
    obj_hide(s_face_img);
    obj_hide(s_plate_lbl);
    obj_hide(s_ptext_lbl);
    obj_hide(s_card_lbl);
    obj_hide(s_name_lbl);
    for (int i = 0; i < 4; i++) obj_hide(s_choice_lbl[i]);
    for (int i = 0; i < 28; i++) obj_hide(s_key_lbl[i]);
}

/* <NAME> -> chosen name (default 格林) */
static const char *prologue_text(uint16_t id)
{
    const char *src = (id < sizeof(s_prologue_texts) / sizeof(s_prologue_texts[0]))
                          ? s_prologue_texts[id] : "";
    uint32_t o = 0;
    for (uint32_t i = 0; src[i] != '\0' && o + 12 < sizeof(s_ptextbuf); i++) {
        if (src[i] == '<' && src[i + 1] == 'N' && src[i + 2] == 'A') {
            const char *nm = (s_pname_n > 0) ? s_pname : PNAME_DEFAULT;
            for (uint32_t j = 0; nm[j] != '\0'; j++) s_ptextbuf[o++] = nm[j];
            i += 5;
            continue;
        }
        s_ptextbuf[o++] = src[i];
    }
    s_ptextbuf[o] = '\0';
    return s_ptextbuf;
}

static void prologue_stop(int back_to_title)
{
    prologue_hide_all();
    s_pro_on = 0;
    s_pro_kb = 0;
    if (s_timer != 0) {
        ((void (*)(void *))(uintptr_t)VENA(FW_TIMER_DEL))(s_timer);
        s_timer = 0;
    }
    if (back_to_title) {
        obj_show(s_title_img);
        obj_show(s_title_lbl);
        obj_show(s_menu_ng);
        obj_show(s_menu_cont);
        obj_show(s_menu_end);
        s_started = 0;
        s_countdown = 12u;
        s_timer = ((void *(*)(void *, uint32_t, void *))(uintptr_t)VENA(FW_TIMER_CREATE))(
            title_tick, 1000u, 0);
        log_line("PROLOGUE-DONE");
    }
    log_flush();
}

static void prologue_key(int n)
{
    if (!s_pro_kb) return;
    if (n < 26) {
        if (s_pname_n < 20) s_pname[s_pname_n++] = (char)('A' + n);
    } else if (n == 26) {              /* backspace */
        if (s_pname_n > 0) s_pname[--s_pname_n] = '\0';
    } else {                           /* done */
        if (s_pname_n == 0) {
            const char *def = PNAME_DEFAULT;
            uint32_t j = 0;
            for (; def[j] != '\0'; j++) s_pname[j] = def[j];
            s_pname_n = (uint8_t)j;
        }
        s_pname[s_pname_n] = '\0';
        s_pro_kb = 0;
        log_kv("pro-name", s_pname_n);
        obj_show(s_map_img);
        obj_show(s_box_img);
        for (int i = 0; i < 28; i++) obj_hide(s_key_lbl[i]);
        ui_show(s_name_lbl, 0);
        s_pro_idx++;
        prologue_show_step();
        return;
    }
    s_pname[s_pname_n] = '\0';
    {
        char disp[40];
        uint32_t o = 0;
        const char *pre = "\xe5\x90\x8d\xe5\xad\x97: ";  /* 名字: */
        for (uint32_t j = 0; pre[j] != '\0'; j++) disp[o++] = pre[j];
        for (uint8_t j = 0; j < s_pname_n && o < 36; j++) disp[o++] = s_pname[j];
        disp[o] = '\0';
        if (s_name_lbl != 0)
            ((void (*)(void *, const char *))(uintptr_t)VENA(FW_LABEL_TEXT))(
                s_name_lbl, disp);
    }
}

static void prologue_pick(int n)
{
    const PrologueStep *st = &s_prologue[s_pro_idx];
    if (!s_pro_on || n >= st->nchoice) return;
    log_kv("pro-choice", (uint32_t)n);
    log_kv("pro-choice-t", st->choice_text[n]);
    log_flush();
    s_pro_idx = st->choice_next[n];
    prologue_show_step();
}

static void prologue_show_step(void)
{
    const PrologueStep *st = &s_prologue[s_pro_idx];
    prologue_hide_all();
    log_kv("pro-step", s_pro_idx);
    log_kv("pro-type", st->type);

    if (st->type == PSTEP_END) {
        prologue_stop(1);
        return;
    }

    int has_face = 0;
    if (st->type == PSTEP_CARD) {
        /* black screen + big logo; box below for the prompt */
        ui_show(s_card_lbl, 1);
        if (s_card_lbl != 0)
            ((void (*)(void *, const char *))(uintptr_t)VENA(FW_LABEL_TEXT))(
                s_card_lbl, "BLACK SOULS");
    } else {
        ui_show(s_map_img, 1);
        ui_show(s_box_img, 1);
        has_face = (st->face != 0);
        if (has_face) ui_show(s_face_img, 1);
    }

    if (st->text != 0xFFFF) {
        const char *txt = prologue_text(st->text);
        if (s_ptext_lbl != 0) {
            ((void (*)(void *, const char *))(uintptr_t)VENA(FW_LABEL_TEXT))(
                s_ptext_lbl, txt);
            if (st->type == PSTEP_CARD) {
                /* title-card prompt: centered under the logo */
                ((void (*)(void *, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_SIZE))(
                    s_ptext_lbl, 194, 40);
                ((void (*)(void *, uint32_t, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_ALIGN))(
                    s_ptext_lbl, ALIGN_CENTER, 0, 30);
            } else {
                ((void (*)(void *, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_SIZE))(
                    s_ptext_lbl, has_face ? 126 : 194, 94);
                ((void (*)(void *, uint32_t, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_ALIGN))(
                    s_ptext_lbl, ALIGN_TOP_LEFT, has_face ? 78 : 8, BOX_Y + 6);
            }
            ui_show(s_ptext_lbl, 1);
        }
    }

    if (st->plate != 0xFFFF && st->type != PSTEP_CARD) {
        if (s_plate_lbl != 0) {
            ((void (*)(void *, const char *))(uintptr_t)VENA(FW_LABEL_TEXT))(
                s_plate_lbl, s_prologue_texts[st->plate]);
            ui_show(s_plate_lbl, 1);
        }
    }

    /* choices (above the box; card step shows them mid-screen) */
    if (st->nchoice > 0) {
        int base = (st->type == PSTEP_CARD) ? 300 : (BOX_Y - st->nchoice * 34 - 8);
        for (int i = 0; i < st->nchoice; i++) {
            void *lbl = s_choice_lbl[i];
            if (lbl == 0) continue;
            ((void (*)(void *, const char *))(uintptr_t)VENA(FW_LABEL_TEXT))(
                lbl, s_prologue_texts[st->choice_text[i]]);
            ((void (*)(void *, uint32_t, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_ALIGN))(
                lbl, ALIGN_CENTER, 0, base + i * 34 - 260);
            ((void (*)(void *, uint32_t))(uintptr_t)VENA(FW_OBJ_ADD_FLAG))(
                lbl, LV_FLAG_CLICKABLE);
            (void)((void *(*)(void *, void (*)(void *), uint32_t, void *))(
                uintptr_t)VENA(FW_EVENT_ADD))(lbl, s_choice_cbs[i],
                                              LV_EVENT_CLICKED, 0);
            ui_show(lbl, 1);
        }
    }

    if (st->type == PSTEP_NAME) {
        s_pro_kb = 1;
        ui_show(s_name_lbl, 1);
        for (int i = 0; i < 28; i++) ui_show(s_key_lbl[i], 1);
        s_pro_wait = 0;
    } else {
        s_pro_wait = (st->type == PSTEP_TEXT) ? 4u : 0u;
    }
    log_flush();
}

static void prologue_start(void)
{
    if (s_pro_on || !s_ok_init) return;
    s_pro_on = 1;
    s_pro_idx = 0;
    s_pname_n = 0;
    s_pname[0] = '\0';
    log_line("PROLOGUE-START");
    log_flush();

    /* hide the title layer */
    obj_hide(s_title_img);
    obj_hide(s_title_lbl);
    obj_hide(s_menu_ng);
    obj_hide(s_menu_cont);
    obj_hide(s_menu_end);

    if (s_map_img == 0) {
        s_map_img = ((void *(*)(void *))(uintptr_t)VENA(FW_IMAGE_CREATE))(s_root);
        if (s_map_img != 0) {
            ((void (*)(void *, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_SIZE))(
                s_map_img, 212, 384);
            ((void (*)(void *, uint32_t, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_ALIGN))(
                s_map_img, ALIGN_TOP_LEFT, 0, 0);
            /* compose the GPU frame at runtime from the compact tile lib */
            int mc = map_compose();
            log_kv("map-compose", (uint32_t)(mc == 0 ? 1u : 0u));
            log_flush();
            if (mc == 0) {
                ((void (*)(void *, const char *))(uintptr_t)VENA(FW_IMAGE_SET_SRC))(
                    s_map_img, MAP_PATH);
            }
            ((void (*)(void *, uint32_t))(uintptr_t)VENA(FW_OBJ_ADD_FLAG))(
                s_map_img, LV_FLAG_CLICKABLE);
            (void)((void *(*)(void *, void (*)(void *), uint32_t, void *))(
                uintptr_t)VENA(FW_EVENT_ADD))(s_map_img, cb_advance,
                                              LV_EVENT_CLICKED, 0);
        }
        s_box_img = ((void *(*)(void *))(uintptr_t)VENA(FW_IMAGE_CREATE))(s_root);
        if (s_box_img != 0) {
            ((void (*)(void *, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_SIZE))(
                s_box_img, 212, 110);
            ((void (*)(void *, uint32_t, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_ALIGN))(
                s_box_img, ALIGN_TOP_LEFT, 0, BOX_Y);
            ((void (*)(void *, const char *))(uintptr_t)VENA(FW_IMAGE_SET_SRC))(
                s_box_img, BOX_PATH);
            ((void (*)(void *, uint32_t))(uintptr_t)VENA(FW_OBJ_ADD_FLAG))(
                s_box_img, LV_FLAG_CLICKABLE);
            (void)((void *(*)(void *, void (*)(void *), uint32_t, void *))(
                uintptr_t)VENA(FW_EVENT_ADD))(s_box_img, cb_advance,
                                              LV_EVENT_CLICKED, 0);
        }
        s_face_img = ((void *(*)(void *))(uintptr_t)VENA(FW_IMAGE_CREATE))(s_root);
        if (s_face_img != 0) {
            ((void (*)(void *, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_SIZE))(
                s_face_img, 64, 64);
            ((void (*)(void *, uint32_t, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_ALIGN))(
                s_face_img, ALIGN_TOP_LEFT, 8, BOX_Y + 8);
            ((void (*)(void *, const char *))(uintptr_t)VENA(FW_IMAGE_SET_SRC))(
                s_face_img, FACE_PATH);
        }
        s_pro_font = ((void *(*)(const char *, uint16_t, uint16_t))(uintptr_t)VENA(FW_FONT_CREATE))(
            "MiSans-Regular", 16u, 0u);
        s_plate_lbl = ui_label(s_root, 8, 378, 120, 24, 0, s_cjk_font);
        s_ptext_lbl = ui_label(s_root, 8, BOX_Y + 6, 194, 94, 1, s_pro_font);
        s_card_lbl = ui_label(s_root, 0, 0, 0, 0, 0, s_title_font);
        if (s_card_lbl != 0)
            ((void (*)(void *, uint32_t, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_ALIGN))(
                s_card_lbl, ALIGN_CENTER, 0, -80);
        s_name_lbl = ui_label(s_root, 0, 0, 0, 0, 0, s_title_font);
        if (s_name_lbl != 0)
            ((void (*)(void *, uint32_t, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_ALIGN))(
                s_name_lbl, ALIGN_CENTER, 0, -140);
        for (int i = 0; i < 4; i++) {
            s_choice_lbl[i] = ui_label(s_root, 0, 0, 0, 0, 0, s_cjk_font);
        }
        /* keyboard: 26 letters + backspace + done, 4 rows x 7 cols */
        {
            char buf[8];
            for (int i = 0; i < 26; i++) {
                buf[0] = (char)('A' + i);
                buf[1] = '\0';
                s_key_lbl[i] = ui_label(s_root, 0, 0, 0, 0, 0, s_cjk_font);
                if (s_key_lbl[i] == 0) continue;
                ((void (*)(void *, const char *))(uintptr_t)VENA(FW_LABEL_TEXT))(
                    s_key_lbl[i], buf);
                ((void (*)(void *, uint32_t, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_ALIGN))(
                    s_key_lbl[i], ALIGN_TOP_LEFT, 10 + (i % 7) * 28, 190 + (i / 7) * 44);
                ((void (*)(void *, uint32_t))(uintptr_t)VENA(FW_OBJ_ADD_FLAG))(
                    s_key_lbl[i], LV_FLAG_CLICKABLE);
                (void)((void *(*)(void *, void (*)(void *), uint32_t, void *))(
                    uintptr_t)VENA(FW_EVENT_ADD))(s_key_lbl[i], s_key_cbs[i],
                                                  LV_EVENT_CLICKED, 0);
                obj_hide(s_key_lbl[i]);
            }
            {
                const char *bk = "\xe9\x80\x80\xe6\xa0\xbc";      /* 退格 */
                const char *dn = "\xe5\xae\x8c\xe6\x88\x90";      /* 完成 */
                s_key_lbl[26] = ui_label(s_root, 0, 0, 0, 0, 0, s_cjk_font);
                s_key_lbl[27] = ui_label(s_root, 0, 0, 0, 0, 0, s_cjk_font);
                if (s_key_lbl[26] != 0) {
                    ((void (*)(void *, const char *))(uintptr_t)VENA(FW_LABEL_TEXT))(
                        s_key_lbl[26], bk);
                    ((void (*)(void *, uint32_t, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_ALIGN))(
                        s_key_lbl[26], ALIGN_TOP_LEFT, 10 + 5 * 28, 190 + 3 * 44);
                    ((void (*)(void *, uint32_t))(uintptr_t)VENA(FW_OBJ_ADD_FLAG))(
                        s_key_lbl[26], LV_FLAG_CLICKABLE);
                    (void)((void *(*)(void *, void (*)(void *), uint32_t, void *))(
                        uintptr_t)VENA(FW_EVENT_ADD))(s_key_lbl[26], s_key_cbs[26],
                                                      LV_EVENT_CLICKED, 0);
                    obj_hide(s_key_lbl[26]);
                }
                if (s_key_lbl[27] != 0) {
                    ((void (*)(void *, const char *))(uintptr_t)VENA(FW_LABEL_TEXT))(
                        s_key_lbl[27], dn);
                    ((void (*)(void *, uint32_t, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_ALIGN))(
                        s_key_lbl[27], ALIGN_TOP_LEFT, 10 + 6 * 28, 190 + 3 * 44);
                    ((void (*)(void *, uint32_t))(uintptr_t)VENA(FW_OBJ_ADD_FLAG))(
                        s_key_lbl[27], LV_FLAG_CLICKABLE);
                    (void)((void *(*)(void *, void (*)(void *), uint32_t, void *))(
                        uintptr_t)VENA(FW_EVENT_ADD))(s_key_lbl[27], s_key_cbs[27],
                                                      LV_EVENT_CLICKED, 0);
                    obj_hide(s_key_lbl[27]);
                }
            }
        }
    }

    /* 1s prologue tick (auto-advance) */
    if (s_timer != 0) {
        ((void (*)(void *))(uintptr_t)VENA(FW_TIMER_DEL))(s_timer);
        s_timer = 0;
    }
    s_timer = ((void *(*)(void *, uint32_t, void *))(uintptr_t)VENA(FW_TIMER_CREATE))(
        prologue_tick, 1000u, 0);
    prologue_show_step();
}

static void prologue_tick(void *arg)
{
    (void)arg;
    if (!s_pro_on || s_pro_kb) return;
    const PrologueStep *st = &s_prologue[s_pro_idx];
    if (st->type != PSTEP_TEXT || s_pro_wait == 0u) return;
    s_pro_wait--;
    if (s_pro_wait == 0u) {
        log_line("PRO-AUTO");
        s_pro_idx++;
        prologue_show_step();
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

/* map-viewer / streaming composer paths (v2 self-describing mapdata) */
#define MAPDATA_PATH "/data/bsvm_mapdata.bin"
#define SEISEN_PATH  "/data/bsvm_seisen_mapdata.bin"
#define VIEW_A_PATH  "/data/bsvm_view_a.bin"
#define VIEW_B_PATH  "/data/bsvm_view_b.bin"

static int md_load(const char *path);
static void viewer_start(void *root);

static int bsvm_on_create(void *page, void *root, void *start_data)
{
    (void)page; (void)start_data;
    /* map-viewer test mode: triggered by the seisen mapdata file */
    {
        int tfd = ((posix_open_fn)vfn(FW_POSIX_OPEN))(SEISEN_PATH, O_RDONLY);
        if (tfd >= 0) {
            ((posix_close_fn)vfn(FW_POSIX_CLOSE))(tfd);
            log_line("MODE viewer");
            log_flush();
            if (md_load(SEISEN_PATH) == 0) {
                viewer_start(root);
                return 0;
            }
            log_line("MV-LOAD-FAIL");
            log_flush();
        }
    }
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
                log_line("MODE title");
                /* CJK font: watchfaces render Chinese with MiSans; the native
                 * default font has no CJK glyphs (boxes on device) */
                log_line("font-create");
                log_flush();
                s_cjk_font = ((void *(*)(const char *, uint16_t, uint16_t))(uintptr_t)VENA(FW_FONT_CREATE))(
                    "MiSans-Regular", 18u, 0u);
                s_title_font = ((void *(*)(const char *, uint16_t, uint16_t))(uintptr_t)VENA(FW_FONT_CREATE))(
                    "MiSans-Regular", 26u, 0u);
                log_kv("font", (uint32_t)(uintptr_t)s_cjk_font);
                log_kv("tfont", (uint32_t)(uintptr_t)s_title_font);
                /* background: /data/bsvm_title.bin (mist.png pre-stretched to
                 * 212x520 in the firmware GPU-decoder format; decoded by the
                 * firmware render task, zero module RAM) */
                s_title_img = ((void *(*)(void *))(uintptr_t)VENA(FW_IMAGE_CREATE))(root);
                if (s_title_img != 0) {
                    ((void (*)(void *, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_SIZE))(
                        s_title_img, 212, 520);
                    ((void (*)(void *, uint32_t, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_ALIGN))(
                        s_title_img, ALIGN_TOP_LEFT, 0, 0);
                    ((void (*)(void *, const char *))(uintptr_t)VENA(FW_IMAGE_SET_SRC))(
                        s_title_img, TITLE_PATH);
                }
                log_kv("img", (uint32_t)(uintptr_t)s_title_img);
                /* game title text, upper half, centered (stock draw_game_title) */
                s_title_lbl = ui_label(root, 0, 0, 0, 0, 0, s_title_font);
                if (s_title_lbl != 0) {
                    ((void (*)(void *, const char *))(uintptr_t)VENA(FW_LABEL_TEXT))(
                        s_title_lbl, "BLACK SOULS");
                    ((void (*)(void *, uint32_t, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_ALIGN))(
                        s_title_lbl, ALIGN_CENTER, 0, -140);
                }
                /* command rows (Window_TitleCommand: lower-middle, centered) */
                s_menu_ng = ui_label(root, 0, 0, 0, 0, 0, s_cjk_font);
                if (s_menu_ng != 0) {
                    ((void (*)(void *, const char *))(uintptr_t)VENA(FW_LABEL_TEXT))(
                        s_menu_ng, "NEW GAME");
                    ((void (*)(void *, uint32_t, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_ALIGN))(
                        s_menu_ng, ALIGN_CENTER, 0, 40);
                    ((void (*)(void *, uint32_t))(uintptr_t)VENA(FW_OBJ_ADD_FLAG))(
                        s_menu_ng, LV_FLAG_CLICKABLE);
                    (void)((void *(*)(void *, void (*)(void *), uint32_t, void *))(uintptr_t)VENA(FW_EVENT_ADD))(
                        s_menu_ng, cb_new_game, LV_EVENT_CLICKED, 0);
                }
                s_menu_cont = ui_label(root, 0, 0, 0, 0, 0, s_cjk_font);
                if (s_menu_cont != 0) {
                    ((void (*)(void *, const char *))(uintptr_t)VENA(FW_LABEL_TEXT))(
                        s_menu_cont, "CONTINUE");
                    ((void (*)(void *, uint32_t, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_ALIGN))(
                        s_menu_cont, ALIGN_CENTER, 0, 90);
                }
                s_menu_end = ui_label(root, 0, 0, 0, 0, 0, s_cjk_font);
                if (s_menu_end != 0) {
                    ((void (*)(void *, const char *))(uintptr_t)VENA(FW_LABEL_TEXT))(
                        s_menu_end, "END");
                    ((void (*)(void *, uint32_t, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_ALIGN))(
                        s_menu_end, ALIGN_CENTER, 0, 140);
                    ((void (*)(void *, uint32_t))(uintptr_t)VENA(FW_OBJ_ADD_FLAG))(
                        s_menu_end, LV_FLAG_CLICKABLE);
                    (void)((void *(*)(void *, void (*)(void *), uint32_t, void *))(uintptr_t)VENA(FW_EVENT_ADD))(
                        s_menu_end, cb_end, LV_EVENT_CLICKED, 0);
                }
                s_countdown = 12u;
                log_flush();
                s_timer = ((void *(*)(void *, uint32_t, void *))(uintptr_t)VENA(FW_TIMER_CREATE))(
                    title_tick, 1000u, 0);
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

/* ---- on-band map composition (v1.0, v2 self-describing mapdata) ----
 * Any map ships as one v2 bsvm_mapdata.bin (header carries dims; the
 * tail carries the id/at tables and player spawn), composed here at
 * runtime into a GPU-decoder ARGB8888 file:
 *   - tilemap rows are streamed from the file per strip (no RAM copy)
 *   - normal cells are streamed per blit (1KB read each)
 *   - autotile blocks live in a small LRU cache (8 x 6KB)
 * RAM: palette 1K + tables ~1.3K + at cache 48K + 212x32 band 27K. */
#define MD_VIEW_W    212u              /* band width (camera crop) */
#define MD_CAM_X     94u               /* prologue camera x0 */
#define MD_AT_MAX    24u               /* autotile blocks per map */
#define MD_AT_CACHE  8u                /* LRU slots */

static uint8_t  s_md_pal[256][4];            /* BGRA words, [0] unused */
static uint16_t s_md_id[256];                /* tile id per normal cell */
static uint16_t s_md_nfl[256];               /* flags: bit4 over-player */
static uint16_t s_md_normal_n;
static uint16_t s_md_at_n;
static uint16_t s_md_at_base[MD_AT_MAX];
static uint16_t s_md_at_idx[MD_AT_MAX];
static uint16_t s_md_at_h[MD_AT_MAX];        /* block height in tiles */
static uint16_t s_md_at_fl[MD_AT_MAX];       /* bit0 waterfall, bit7 table */
static int32_t  s_md_cells_off;              /* file offset: first cell */
static int32_t  s_md_atoff[MD_AT_MAX];       /* file offset per block */
static int32_t  s_md_tm_off;                 /* file offset: tilemap */
static uint16_t s_md_x, s_md_y;              /* map dims in tiles */
static int16_t  s_md_ptx, s_md_pty;          /* player spawn tile, -1 = none */
static int      s_md_fd = -1;                /* open mapdata fd */
static int      s_md_cx0;                    /* window x0 in view px */
/* LRU autotile-block cache */
static uint8_t  s_at_cache[MD_AT_CACHE][96u * 64u];
static int16_t  s_at_slot_blk[MD_AT_CACHE];
static uint32_t s_at_slot_tick[MD_AT_CACHE];
static uint32_t s_at_tick;
static uint32_t s_md_band[212u * 32u];        /* one 212x32 strip */
static int      s_md_band_y0;                 /* view-y of strip top */

typedef int (*md_rd_fn)(int, void *, uint32_t);

static const uint8_t *md_at_ptr(int blk)
{
    /* LRU cache: fetch the 64px-wide autotile block into RAM */
    int slot = -1;
    for (int i = 0; i < MD_AT_CACHE; i++) {
        if (s_at_slot_blk[i] == blk) {
            s_at_slot_tick[i] = ++s_at_tick;
            return s_at_cache[i];
        }
        if (s_at_slot_blk[i] < 0 || slot < 0 ||
            s_at_slot_tick[i] < s_at_slot_tick[slot]) slot = i;
    }
    if (slot < 0) slot = 0;
    uint32_t bytes = (uint32_t)s_md_at_h[blk] * 2048u;
    if (bytes > 96u * 64u) bytes = 96u * 64u;
    ((posix_lseek_fn)vfn(FW_LSEEK))(s_md_fd, (int64_t)s_md_atoff[blk], SEEK_SET);
    if (((md_rd_fn)vfn(FW_POSIX_READ))(s_md_fd, s_at_cache[slot], bytes) <
        (int)bytes) return 0;
    s_at_slot_blk[slot] = (int16_t)blk;
    s_at_slot_tick[slot] = ++s_at_tick;
    return s_at_cache[slot];
}

static void md_rect_blit_any(int tx, int ty, int vx0, int vy0, int vw, int vh,
                             int blk, uint16_t off, int sw, int sh)
{
    /* draw a view-space sub-rect (vx0/vy0/vw/vh in view px) sampling
     * sw x sh source pixels at byte offset off inside the 64px-wide
     * 8bpp autotile block, at half resolution. */
    const uint8_t *base = md_at_ptr(blk);
    if (base == 0) return;
    int sx = off % 64u, sy = off / 64u;
    int bh = s_md_at_h[blk] * 32u;
    int bx = (int)(tx * 16u) + vx0 - s_md_cx0;
    int by0 = ty * 16u + vy0 - s_md_band_y0;
    for (int yy = 0; yy < vh; yy++) {
        int r = by0 + yy;
        if (r < 0 || r >= 32) continue;
        int syy = sy + (yy * sh) / vh;
        if (syy >= bh) syy = bh - 1;
        uint32_t *row = s_md_band + (uint32_t)r * MD_VIEW_W;
        const uint8_t *src = base + (uint32_t)syy * 64u;
        for (int xx = 0; xx < vw; xx++) {
            int vx = bx + xx;
            if (vx < 0 || vx >= (int)MD_VIEW_W) continue;
            int sxx = sx + (xx * sw) / vw;
            if (sxx >= sx + sw) sxx = sx + sw - 1;
            uint8_t c = src[sxx];
            if (c != 0) row[vx] = *(const uint32_t *)s_md_pal[c];
        }
    }
}

static void md_rect_blit(int tx, int ty, int qx, int qy,
                         uint8_t blk, uint16_t off, int sw)
{
    /* one autotile quadrant: 16px source quadrant -> 8x8 view px */
    md_rect_blit_any(tx, ty, qx / 2, qy / 2, 8, 8, blk, off, sw, sw);
}

static void md_cell_blit(int fd, uint32_t cell_off, uint16_t idx,
                         int tx, int ty)
{
    /* 32x32 8bpp cell -> 16x16 view (2x2 box downscale, opaque wins) */
    uint8_t cell[1024];
    ((posix_lseek_fn)vfn(FW_LSEEK))(fd, (int64_t)cell_off + (uint32_t)idx * 1024u,
                                    SEEK_SET);
    if (((md_rd_fn)vfn(FW_POSIX_READ))(fd, cell, 1024u) < 1024) return;
    int bx = (int)(tx * 16u) - s_md_cx0;
    int by0 = ty * 16u - s_md_band_y0;
    for (int yy = 0; yy < 16; yy++) {
        int r = by0 + yy;
        if (r < 0 || r >= 32) continue;
        uint32_t *row = s_md_band + (uint32_t)r * MD_VIEW_W;
        const uint8_t *s0 = cell + (yy * 2) * 32u;
        const uint8_t *s1 = s0 + 32u;
        for (int xx = 0; xx < 16; xx++) {
            int vx = bx + xx;
            if (vx < 0 || vx >= (int)MD_VIEW_W) continue;
            uint8_t c = s0[xx * 2];
            if (c == 0) c = s0[xx * 2 + 1];
            if (c == 0) c = s1[xx * 2];
            if (c == 0) c = s1[xx * 2 + 1];
            if (c != 0) row[vx] = *(const uint32_t *)s_md_pal[c];
        }
    }
}

static int md_find_normal(uint16_t id)
{
    if (id == 0) return -1;
    for (uint16_t k = 0; k < s_md_normal_n; k++) {
        if (s_md_id[k] == id) return (int)k;
    }
    return -1;
}

static int md_find_at(uint16_t base, uint16_t at)
{
    for (uint16_t k = 0; k < s_md_at_n; k++) {
        if (s_md_at_base[k] == base && s_md_at_idx[k] == at) return (int)k;
    }
    return -1;
}

static void md_draw_at(uint32_t tx, uint32_t ty, uint16_t v)
{
    /* mkxp tileatlasvx.cpp onTile dispatch: A1(0x800)/A2(0xB00)/
     * A3(0x1100)/A4(0x1700). at fl: bit7 = A2 table (6-quad legs),
     * bit0 = A1 waterfall C-type (2 half-width 16x32 quads). */
    uint16_t base = (v < 0xB00u) ? 0x0800u :
                    (v < 0x1100u) ? 0x0B00u :
                    (v < 0x1700u) ? 0x1100u : 0x1700u;
    int k = md_find_at(base, (uint16_t)((v - base) / 48u));
    if (k < 0) return;
    int blk = k;
    uint16_t afl = s_md_at_fl[k];
    uint16_t pat = (uint16_t)((v - base) % 48u);
    static const int ax[4] = {0, 16, 0, 16};
    static const int ay[4] = {0, 0, 16, 16};
    if (base == 0x1100u) {
        /* A3 wall: B table; shapes >= 16 render nothing (mkxp) */
        if (pat < 16u) {
            for (int i = 0; i < 4; i++)
                md_rect_blit((int)tx, (int)ty, ax[i], ay[i],
                             blk, bsva_at_b[pat * 4u + i], 15);
        }
    } else if (base == 0x0800u && (afl & 0x01u)) {
        /* A1 waterfall C-type: 2 quads of 15x31 -> 16x32 halves */
        if (pat < 4u) {
            for (int i = 0; i < 2; i++)
                md_rect_blit_any((int)tx, (int)ty, i * 8, 0, 8, 16,
                                 blk, bsva_at_c[pat * 2u + i], 15, 31);
        }
    } else if (base == 0x0B00u && (afl & 0x80u)) {
        /* A2 table tile: 6-quad layout, legs spill into the tile below */
        uint8_t bits = bsva_a2_size[pat];
        for (int i = 0; i < 6; i++) {
            if (!((bits >> i) & 1u)) continue;
            int qx, qy;
            if (i < 4) { qx = ax[i]; qy = ay[i]; }
            else { qx = (i == 4) ? 0 : 16; qy = 32; }
            md_rect_blit((int)tx, (int)ty, qx, qy, blk,
                         bsva_at_a2[pat * 6u + i], 16);
        }
    } else {
        for (int i = 0; i < 4; i++)
            md_rect_blit((int)tx, (int)ty, ax[i], ay[i],
                         blk, bsva_at_a[pat * 4u + i], 15);
    }
}

static int md_load(const char *path)
{
    /* open a v2 self-describing mapdata file and parse the tables */
    if (s_md_fd >= 0) {
        ((posix_close_fn)vfn(FW_POSIX_CLOSE))(s_md_fd);
        s_md_fd = -1;
    }
    for (int i = 0; i < MD_AT_CACHE; i++) s_at_slot_blk[i] = -1;
    int fd = ((posix_open_fn)vfn(FW_POSIX_OPEN))(path, O_RDONLY);
    if (fd < 0) return -1;
    uint8_t hdr[16];
    if (((md_rd_fn)vfn(FW_POSIX_READ))(fd, hdr, 16u) < 16 ||
        hdr[0] != 'B' || hdr[1] != 'S' || hdr[2] != 'M' || hdr[3] != 'D') {
        ((posix_close_fn)vfn(FW_POSIX_CLOSE))(fd);
        return -2;
    }
    uint16_t pal_n = (uint16_t)(hdr[4] | (hdr[5] << 8));
    uint16_t normal_n = (uint16_t)(hdr[6] | (hdr[7] << 8));
    uint16_t at_n = (uint16_t)(hdr[8] | (hdr[9] << 8));
    uint16_t mx = (uint16_t)(hdr[10] | (hdr[11] << 8));
    uint16_t my = (uint16_t)(hdr[12] | (hdr[13] << 8));
    if (pal_n > 256u || normal_n == 0u || normal_n > 256u ||
        at_n == 0u || at_n > MD_AT_MAX ||
        mx == 0u || my == 0u || mx > 512u || my > 512u) {
        ((posix_close_fn)vfn(FW_POSIX_CLOSE))(fd);
        return -3;
    }
    uint8_t praw[768];
    if (((md_rd_fn)vfn(FW_POSIX_READ))(fd, praw, 768u) < 768) {
        ((posix_close_fn)vfn(FW_POSIX_CLOSE))(fd);
        return -4;
    }
    for (uint32_t k = 0; k < 256u; k++) {
        uint8_t r = (k < pal_n) ? praw[k * 3u] : 0;
        uint8_t g = (k < pal_n) ? praw[k * 3u + 1u] : 0;
        uint8_t b = (k < pal_n) ? praw[k * 3u + 2u] : 0;
        s_md_pal[k][0] = b;
        s_md_pal[k][1] = g;
        s_md_pal[k][2] = r;
        s_md_pal[k][3] = 0xFF;
    }
    s_md_fd = fd;
    s_md_normal_n = normal_n;
    s_md_at_n = at_n;
    s_md_x = mx;
    s_md_y = my;
    s_md_cells_off = 16 + 768;
    /* the tail (backwards): spawn i16x2, at table 8B each, id table 4B each */
    int64_t fsize = ((posix_lseek_fn)vfn(FW_LSEEK))(fd, 0, SEEK_END);
    int64_t need = 16 + 768 + (int64_t)normal_n * 1024 +
                   (int64_t)4 * mx * my * 2 + normal_n * 4 + at_n * 8 + 4;
    if (fsize < need) {
        ((posix_close_fn)vfn(FW_POSIX_CLOSE))(fd);
        s_md_fd = -1;
        return -5;
    }
    uint8_t tmp[MD_AT_MAX * 8u];
    int64_t pos = fsize - 4;
    ((posix_lseek_fn)vfn(FW_LSEEK))(fd, pos, SEEK_SET);
    uint8_t spawn[4];
    if (((md_rd_fn)vfn(FW_POSIX_READ))(fd, spawn, 4u) < 4) goto bad;
    s_md_ptx = (int16_t)(spawn[0] | ((uint16_t)spawn[1] << 8));
    s_md_pty = (int16_t)(spawn[2] | ((uint16_t)spawn[3] << 8));
    pos -= (int64_t)at_n * 8;
    ((posix_lseek_fn)vfn(FW_LSEEK))(fd, pos, SEEK_SET);
    if (((md_rd_fn)vfn(FW_POSIX_READ))(fd, tmp, at_n * 8u) < (int)(at_n * 8u))
        goto bad;
    for (uint16_t k = 0; k < at_n; k++) {
        const uint8_t *e = tmp + (uint32_t)k * 8u;
        s_md_at_base[k] = (uint16_t)(e[0] | ((uint16_t)e[1] << 8));
        s_md_at_idx[k] = (uint16_t)(e[2] | ((uint16_t)e[3] << 8));
        s_md_at_h[k] = (uint16_t)(e[4] | ((uint16_t)e[5] << 8));
        s_md_at_fl[k] = (uint16_t)(e[6] | ((uint16_t)e[7] << 8));
    }
    pos -= (int64_t)normal_n * 4;
    ((posix_lseek_fn)vfn(FW_LSEEK))(fd, pos, SEEK_SET);
    if (((md_rd_fn)vfn(FW_POSIX_READ))(fd, tmp, normal_n * 4u) <
        (int)(normal_n * 4u)) goto bad;
    for (uint16_t k = 0; k < normal_n; k++) {
        const uint8_t *e = tmp + (uint32_t)k * 4u;
        s_md_id[k] = (uint16_t)(e[0] | ((uint16_t)e[1] << 8));
        s_md_nfl[k] = (uint16_t)(e[2] | ((uint16_t)e[3] << 8));
    }
    /* tilemap sits before the id table; at blocks before the tilemap */
    int64_t ids_off = pos;
    s_md_tm_off = ids_off - (int64_t)4 * mx * my * 2;
    int64_t cur = s_md_tm_off;
    for (int k = (int)at_n - 1; k >= 0; k--) {
        cur -= (int64_t)s_md_at_h[k] * 2048;
        s_md_atoff[k] = cur;
    }
    return 0;
bad:
    ((posix_close_fn)vfn(FW_POSIX_CLOSE))(fd);
    s_md_fd = -1;
    return -6;
}

static int md_tm_row(uint16_t z, uint16_t ty, uint16_t *row)
{
    /* stream one tilemap row (X entries) from the file */
    if (ty >= s_md_y) return -1;
    int64_t off = s_md_tm_off +
        (int64_t)(z * s_md_y + ty) * s_md_x * 2;
    ((posix_lseek_fn)vfn(FW_LSEEK))(s_md_fd, off, SEEK_SET);
    return ((md_rd_fn)vfn(FW_POSIX_READ))(s_md_fd, row, s_md_x * 2u) <
                   (int)(s_md_x * 2u) ? -1 : 0;
}

static int md_render_window(const char *out_path, int cx0, int cy0,
                            int w, int h)
{
    /* stream-render the window in 32-view-row strips (GPU-decoder file) */
    if (s_md_fd < 0) return -1;
    int fo = ((posix_open_fn)vfn(FW_POSIX_OPEN))(out_path,
                                                 O_RDWR | O_CREAT, 0666);
    if (fo < 0) return -7;
    uint8_t out_hdr[12] = {0x19, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    out_hdr[4] = (uint8_t)(w & 0xFF);
    out_hdr[5] = (uint8_t)(w >> 8);
    out_hdr[6] = (uint8_t)(h & 0xFF);
    out_hdr[7] = (uint8_t)(h >> 8);
    out_hdr[8] = (uint8_t)((w * 4u) & 0xFF);
    out_hdr[9] = (uint8_t)(((w * 4u) >> 8) & 0xFF);
    ((posix_write_fn)vfn(FW_POSIX_WRITE))(fo, out_hdr, 12u);
    s_md_cx0 = cx0;
    uint16_t row[512];
    uint16_t over[32];
    int wy = 0;
    while (wy < h) {
        int strip_h = (h - wy < 32) ? (h - wy) : 32;
        for (uint32_t i = 0; i < MD_VIEW_W * 32u; i++) s_md_band[i] = 0xFF000000u;
        s_md_band_y0 = cy0 + wy;
        uint32_t t0 = (uint32_t)(s_md_band_y0 / 16);
        uint32_t t1 = (uint32_t)((s_md_band_y0 + strip_h + 15) / 16);
        if (t1 > s_md_y) t1 = s_md_y;
        int over_n = 0;
        for (int z = 0; z < 2; z++) {
            for (uint32_t ty = t0; ty < t1; ty++) {
                if (md_tm_row((uint16_t)z, (uint16_t)ty, row) != 0) continue;
                for (uint16_t tx = 0; tx < s_md_x; tx++) {
                    uint16_t v = row[tx];
                    if (v == 0) continue;
                    int ni = md_find_normal(v);
                    if (ni >= 0) {
                        if (s_md_nfl[ni] & 0x10u) {
                            if (over_n < 16) {
                                over[over_n * 2] = v;
                                over[over_n * 2 + 1] =
                                    (uint16_t)(tx | (ty << 8));
                                over_n++;
                            }
                            continue;
                        }
                        md_cell_blit(s_md_fd, (uint32_t)s_md_cells_off,
                                     (uint16_t)ni, (int)tx, (int)ty);
                        continue;
                    }
                    if (v < 0x0800u) continue;
                    md_draw_at(tx, ty, v);
                }
            }
        }
        /* table legs from the tile row above spill into the strip top */
        if (t0 > 0 && md_tm_row(1u, (uint16_t)(t0 - 1u), row) == 0) {
            for (uint16_t tx = 0; tx < s_md_x; tx++) {
                uint16_t v = row[tx];
                if (v == 0 || v < 0x0B00u || v >= 0x1100u) continue;
                int k = md_find_at(0xB00u, (uint16_t)((v - 0x0B00u) / 48u));
                if (k < 0 || !(s_md_at_fl[k] & 0x80u)) continue;
                uint16_t pat = (uint16_t)((v - 0x0B00u) % 48u);
                uint8_t bits = bsva_a2_size[pat];
                for (int i = 4; i < 6; i++) {
                    if (!((bits >> i) & 1u)) continue;
                    int qx = (i == 4) ? 0 : 16;
                    md_rect_blit((int)tx, (int)t0, qx, 0, k,
                                 bsva_at_a2[pat * 6u + i], 16);
                }
            }
        }
        /* shadow nibbles (z3): quadrant halves -> 8x8, per-pixel clip */
        for (uint32_t ty = t0; ty < t1; ty++) {
            if (md_tm_row(3u, (uint16_t)ty, row) != 0) continue;
            for (uint16_t tx = 0; tx < s_md_x; tx++) {
                uint16_t v = row[tx] & 0xFu;
                if (!v) continue;
                for (int qy = 0; qy < 2; qy++) {
                    for (int qx = 0; qx < 2; qx++) {
                        if (!((v >> (qy * 2 + qx)) & 1u)) continue;
                        int bx = (int)(tx * 16u + qx * 8u) - s_md_cx0;
                        int by = (int)ty * 16u + qy * 8u - s_md_band_y0;
                        for (int yy = 0; yy < 8; yy++) {
                            int r = by + yy;
                            if (r < 0 || r >= 32) continue;
                            uint32_t *brow = s_md_band +
                                (uint32_t)r * MD_VIEW_W;
                            for (int xx = 0; xx < 8; xx++) {
                                int vx = bx + xx;
                                if (vx < 0 || vx >= (int)MD_VIEW_W) continue;
                                uint32_t px = brow[vx];
                                brow[vx] = ((px >> 1) & 0x7F7F7F7Fu) |
                                           (px & 0xFF000000u);
                            }
                        }
                    }
                }
            }
        }
        /* z2 */
        for (uint32_t ty = t0; ty < t1; ty++) {
            if (md_tm_row(2u, (uint16_t)ty, row) != 0) continue;
            for (uint16_t tx = 0; tx < s_md_x; tx++) {
                uint16_t v = row[tx];
                if (v == 0) continue;
                if (v < 0x0800u) {
                    int ni = md_find_normal(v);
                    if (ni >= 0) {
                        md_cell_blit(s_md_fd, (uint32_t)s_md_cells_off,
                                     (uint16_t)ni, (int)tx, (int)ty);
                    }
                    continue;
                }
                md_draw_at(tx, ty, v);
            }
        }
        /* overPlayer tiles recorded from z0/z1 */
        for (int k = 0; k < over_n; k++) {
            uint16_t v = over[k * 2];
            uint32_t tx = over[k * 2 + 1] & 0xFFu;
            uint32_t ty = (over[k * 2 + 1] >> 8) & 0xFFu;
            int ni = md_find_normal(v);
            if (ni >= 0) {
                md_cell_blit(s_md_fd, (uint32_t)s_md_cells_off,
                             (uint16_t)ni, (int)tx, (int)ty);
            }
        }
        /* player soul flame at the spawn tile (-1 = none) */
        if (s_md_ptx >= 0 && t0 <= (uint32_t)s_md_pty &&
            (uint32_t)s_md_pty < t1) {
            md_cell_blit(s_md_fd, (uint32_t)s_md_cells_off,
                         (uint16_t)(s_md_normal_n - 1u),
                         (int)s_md_ptx, (int)s_md_pty);
        }
        for (int yy = 0; yy < strip_h; yy++) {
            ((posix_write_fn)vfn(FW_POSIX_WRITE))(
                fo, s_md_band + (uint32_t)yy * MD_VIEW_W,
                (uint32_t)w * 4u);
        }
        wy += strip_h;
    }
    ((posix_close_fn)vfn(FW_POSIX_CLOSE))(fo);
    return 0;
}

static int map_compose(void)
{
    int rc = md_load(MAPDATA_PATH);
    if (rc != 0) return rc;
    return md_render_window(MAP_PATH, (int)MD_CAM_X, 0,
                            (int)MD_VIEW_W, (int)s_md_y * 16);
}

/* ---------------- seisen map viewer (test page) ----------------
 * Triggered by the presence of /data/bsvm_seisen_mapdata.bin.
 * 212x420 map window + 5 buttons (pan / exit). Panning re-renders
 * the window into one of two alternating files so the firmware
 * re-decodes and shows the new view. Exit via FW_PAGE_FINISH. */
#define MV_W 212
#define MV_H 420
#define MV_STEP_X 106
#define MV_STEP_Y 140

static void *s_mv_img;
static void *s_mv_btn[5];
static int s_mv_cx, s_mv_cy;
static int s_mv_flip;

static void mv_redraw(void)
{
    static const char *const paths[2] = {VIEW_A_PATH, VIEW_B_PATH};
    const char *p = paths[s_mv_flip & 1];
    s_mv_flip ^= 1;
    if (md_render_window(p, s_mv_cx, s_mv_cy, MV_W, MV_H) != 0) return;
    ((void (*)(void *, const char *))(uintptr_t)VENA(FW_IMAGE_SET_SRC))(
        s_mv_img, p);
}

static void mv_pan(int dx, int dy)
{
    int mwx = (int)s_md_x * 16 - MV_W;
    int mwy = (int)s_md_y * 16 - MV_H;
    s_mv_cx += dx;
    s_mv_cy += dy;
    if (s_mv_cx < 0) s_mv_cx = 0;
    if (s_mv_cx > mwx) s_mv_cx = mwx;
    if (s_mv_cy < 0) s_mv_cy = 0;
    if (s_mv_cy > mwy) s_mv_cy = mwy;
    mv_redraw();
    log_kv("mvx", (uint32_t)s_mv_cx);
    log_kv("mvy", (uint32_t)s_mv_cy);
    log_flush();
}

static void cb_mv_left(void *e)  { (void)e; mv_pan(-MV_STEP_X, 0); }
static void cb_mv_right(void *e) { (void)e; mv_pan(MV_STEP_X, 0); }
static void cb_mv_up(void *e)    { (void)e; mv_pan(0, -MV_STEP_Y); }
static void cb_mv_down(void *e)  { (void)e; mv_pan(0, MV_STEP_Y); }
static void cb_mv_exit(void *e)
{
    (void)e;
    log_line("MV-EXIT");
    log_flush();
    (void)((int32_t (*)(void *))(uintptr_t)VENA(FW_PAGE_FINISH))(&s_bsvm_page);
}

static void viewer_start(void *root)
{
    s_mv_cx = 0;
    s_mv_cy = 0;
    s_mv_flip = 0;
    void *font = ((void *(*)(const char *, uint16_t, uint16_t))(uintptr_t)
                  VENA(FW_FONT_CREATE))("MiSans-Regular", 18u, 0u);
    s_mv_img = ((void *(*)(void *))(uintptr_t)VENA(FW_IMAGE_CREATE))(root);
    if (s_mv_img != 0) {
        ((void (*)(void *, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_SIZE))(
            s_mv_img, MV_W, MV_H);
        ((void (*)(void *, uint32_t, int32_t, int32_t))(uintptr_t)VENA(
            FW_OBJ_ALIGN))(s_mv_img, ALIGN_TOP_LEFT, 0, 0);
    }
    /* 左 右 上 下 退出 (MiSans has CJK; ASCII arrows may be missing) */
    static const char *const txt[5] = {
        "\xE5\xB7\xA6", "\xE5\x8F\xB3", "\xE4\xB8\x8A", "\xE4\xB8\x8B",
        "\xE9\x80\x80\xE5\x87\xBA"
    };
    static void (*const cbs[5])(void *) = {
        cb_mv_left, cb_mv_right, cb_mv_up, cb_mv_down, cb_mv_exit
    };
    for (int i = 0; i < 5; i++) {
        void *lbl = ui_label(root, i * 42, 424, 42, 40, 0, font);
        s_mv_btn[i] = lbl;
        if (lbl == 0) continue;
        ((void (*)(void *, const char *))(uintptr_t)VENA(FW_LABEL_TEXT))(
            lbl, txt[i]);
        ((void (*)(void *, uint32_t))(uintptr_t)VENA(FW_OBJ_ADD_FLAG))(
            lbl, LV_FLAG_CLICKABLE);
        (void)((void *(*)(void *, void (*)(void *), uint32_t, void *))(
            uintptr_t)VENA(FW_EVENT_ADD))(lbl, cbs[i], LV_EVENT_CLICKED, 0);
    }
    mv_redraw();
    log_line("MV-READY");
    log_kv("mw", s_md_x * 16u);
    log_kv("mh", s_md_y * 16u);
    log_flush();
}

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
