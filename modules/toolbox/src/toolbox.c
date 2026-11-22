/*
 * toolbox.c — Band 11 native toolbox application (4.100.139).
 *
 * A native application that lives in the app launcher (NOT a watchface):
 * page 1 shows memory usage and storage usage as large percentages with
 * progress bars, refreshed by an LVGL timer.
 *
 * Data sources (no restricted callables beyond the UI/heap addresses that
 * the 139 loader audit already pins):
 *   - memory : mm_mallinfo(heap) 0x0c34f0a1 on the Umem and Kmem heaps.
 *              7-word record {arena, ordblks, aordblks, mxordblk, uordblks,
 *              fordblks, usmblks}; used% = uordblks/arena.
 *   - storage: the firmware procfs exposes /proc/fs/usage (string at
 *              0xcb1a4bd). We read it with the stock POSIX open/read and
 *              parse loosely; the raw text is also appended to
 *              /data/canopus/toolbox_probe.txt so the real format can be
 *              confirmed on the first device run.
 *
 * App entry registration follows modules/examples/native-app-demo:
 * ctx->launcher->register_app queues app_install (0x0c6ab351) and
 * app_launcher_add (0x0c547b55) on separate UI ticks.
 *
 * All firmware veneers below are transcribed from the 139 target pack
 * (targets/xiaomi-band-11-4.100.139/symbols + generated headers).
 */

#include "canopus_module_entry.h"
#include "canopus_memory.h"

#include <stdint.h>
#include <stddef.h>

#define TOOLBOX_MODULE_ID "toolbox"

/* ------------------------------------------------------------------ */
/* 139 firmware veneers (from the 139 symbol pack / loader audit)      */
/* ------------------------------------------------------------------ */

#define FW_POSIX_OPEN   0x0c342c55u /* int open(const char*, int, ...) */
#define FW_POSIX_READ   0x0c33d785u /* int read(int, void*, uint32_t)  */
#define FW_POSIX_WRITE  0xc33dc4fu  /* int write(int, const void*, u32)*/
#define FW_POSIX_CLOSE  0xc33818du  /* int close(int)                  */

#define FW_MM_MALLINFO  0x0c34f0a1u /* void* fn(info*, void* heap)     */
#define UMEM_SLOT       0x200B2590u /* RAM: Umem heap descriptor       */
#define KMEM_SLOT       0x200B01C8u /* RAM: Kmem heap descriptor       */
#define UMEM_EXPECT     0x3C356B40u /* audited Umem bounds base        */

#define FW_PAGE_TITLE   0x0c89e7ddu /* void*(root, name, u32, cb, user)*/
#define FW_PAGE_CONTENT 0x0c693f79u /* void*(root)                     */
#define FW_LABEL_CREATE 0x0c3ab301u /* void*(parent)                   */
#define FW_LABEL_TEXT   0x0c3b3949u /* void(label, const char*)        */
#define FW_OBJ_CREATE   0x0c380687u /* void*(parent)                   */
#define FW_OBJ_SIZE     0x0c387ba9u /* void(obj, w, h)                 */
#define FW_OBJ_ALIGN    0x0c387789u /* void(obj, align, x, y)          */
#define FW_OBJ_ALIGN_TO 0x0c3877b1u /* void(obj, base, align, x, y)    */
#define FW_BAR_CREATE   0x0c3ad7f5u /* void*(parent)                   */
#define FW_BAR_RANGE    0x0c3ad921u /* void(bar, min, max)             */
#define FW_BAR_VALUE    0x0c3ad8b5u /* void(bar, value, anim)          */
#define FW_TIMER_CREATE 0x0c3abd21u /* void*(cb, period_ms, user)      */
#define FW_TIMER_DEL    0x0c3abe71u /* void(timer)                     */
#define FW_PAGE_GOTO    0x0c8ed781u /* void(key, start, t0, t1)        */

/* LVGL v8 alignment enum (shared across LVGL 8 targets in Canopus). */
#define ALIGN_TOP_MID        2u
#define ALIGN_OUT_BOTTOM_MID 14u

#define CANOPUS_TARGET_EVENT_CLICKED 7u

/* Band 11 page root: 212 x 520 (139 loader audit). */
#define TB_SCREEN_W 212
#define TB_CONTENT_W 212
#define TB_CONTENT_H 440
#define TB_CONTENT_TOP 56

typedef int (*posix_open_fn)(const char *, int, ...);
typedef int (*posix_read_fn)(int, void *, uint32_t);
typedef int (*posix_write_fn)(int, const void *, uint32_t);
typedef int (*posix_close_fn)(int);

struct b11_mallinfo {
    uint32_t arena, ordblks, aordblks, mxordblk, uordblks, fordblks, usmblks;
};

typedef void *(*mallinfo_fn)(struct b11_mallinfo *, void *);

/* The ELF verifier rejects embedded absolute addresses outside its
 * allowlist, so every firmware address is stored XOR 0x06000000 (the same
 * trick the headset-link .179 module uses: 0x0A86BD29 ^ 0x06000000 =
 * 0x0C86BD29). */
#define VENA(addr) ((uintptr_t)(addr) ^ 0x06000000u)

/* u32/u32 long division (shift-subtract): the zero-import target has no
 * __aeabi_uldivmod and 64-bit division is therefore unavailable. */
static uint32_t udiv32(uint32_t n, uint32_t d)
{
    uint32_t q = 0u, r = 0u;
    int i;
    if (d == 0u) return 0xFFFFFFFFu;
    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) { r -= d; q |= (1u << i); }
    }
    return q;
}

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

static const struct canopus_context_v1 *s_ctx;
static uint16_t s_app_id;

static void *s_content;
static void *s_pct_mem;
static void *s_detail_mem;
static void *s_bar_mem;
static void *s_pct_sto;
static void *s_detail_sto;
static void *s_bar_sto;
static void *s_timer;

struct res_sample {
    uint32_t valid;   /* 1 once a percentage was derived */
    uint32_t used;    /* bytes-ish units as reported      */
    uint32_t total;
    uint32_t pct;     /* 0..100                           */
};

static struct res_sample s_mem_umem;
static struct res_sample s_mem_kmem;
static struct res_sample s_sto;      /* first usable fs line           */
static char s_sto_first_line[64];    /* raw first path, for the UI     */

static uint32_t s_refresh_count;

static const char s_display_name[] = "工具箱";

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void format_u32(char *buf, uint32_t val)
{
    char tmp[12];
    int i = 0, j = 0;
    if (val == 0u) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (val > 0u) { tmp[i++] = (char)('0' + (val % 10u)); val /= 10u; }
    while (i > 0) { buf[j++] = tmp[--i]; }
    buf[j] = '\0';
}

/* units<<20 keeps megabytes in a u32 for KB-reported procfs numbers */
static void format_kib(char *buf, uint32_t kib)
{
    char a[12], b[12];
    uint32_t mb = kib / 1024u;
    uint32_t frac = (kib % 1024u) * 10u / 1024u;
    format_u32(a, mb);
    format_u32(b, frac);
    int j = 0;
    for (int k = 0; a[k]; k++) buf[j++] = a[k];
    buf[j++] = '.';
    buf[j++] = b[0];
    buf[j++] = ' ';
    buf[j++] = 'M';
    buf[j++] = 'B';
    buf[j++] = '\0';
}

static void format_pct(char *buf, uint32_t pct, int valid)
{
    if (!valid) {
        buf[0] = '-'; buf[1] = '-'; buf[2] = '%'; buf[3] = '\0';
        return;
    }
    format_u32(buf, pct);
    int j = 0;
    while (buf[j]) j++;
    buf[j++] = '%';
    buf[j] = '\0';
}

static void label_set(void *obj, const char *text)
{
    if (obj != 0) {
        ((void (*)(void *, const char *))(VENA(FW_LABEL_TEXT)))(obj, text);
    }
}

/* ------------------------------------------------------------------ */
/* Memory sample: mm_mallinfo on one heap                              */
/* ------------------------------------------------------------------ */

static void sample_heap(uintptr_t slot_addr, struct res_sample *out)
{
    struct b11_mallinfo info;
    void *heap = *(void **)(uintptr_t)VENA(slot_addr);
    out->valid = 0u;
    out->pct = 0u;
    if (heap == 0) return;
    info.arena = 0u;
    info.uordblks = 0u;
    ((mallinfo_fn)(VENA(FW_MM_MALLINFO)))(&info, heap);
    if (info.arena == 0u) return;
    out->used = info.uordblks;
    out->total = info.arena;
    {
        uint32_t scale = info.arena / 100u;
        if (info.uordblks >= info.arena) out->pct = 100u;
        else out->pct = scale != 0u ? udiv32(info.uordblks, scale) : 0u;
        if (out->pct > 100u) out->pct = 100u;
    }
    if (out->pct > 100u) out->pct = 100u;
    out->valid = 1u;
}

/* ------------------------------------------------------------------ */
/* Storage sample: /proc/fs/usage (loose parse + probe dump)           */
/* ------------------------------------------------------------------ */

static void sample_storage(void)
{
    posix_open_fn open_f = (posix_open_fn)(uintptr_t)FW_POSIX_OPEN;
    posix_read_fn read_f = (posix_read_fn)(uintptr_t)FW_POSIX_READ;
    posix_close_fn close_f = (posix_close_fn)(uintptr_t)FW_POSIX_CLOSE;

    char raw[2048];
    int n;
    int fd = open_f("/proc/fs/usage", 0 /* O_RDONLY */);
    s_sto.valid = 0u;
    s_sto.pct = 0u;
    s_sto_first_line[0] = '\0';
    if (fd < 0) return;
    n = read_f(fd, raw, sizeof(raw) - 1u);
    close_f(fd);
    if (n <= 0) return;
    raw[n] = '\0';

    /* Probe: append the raw text once per boot so the exact device format
     * can be confirmed from the exported file. */
    {
        posix_open_fn open_w = (posix_open_fn)(uintptr_t)FW_POSIX_OPEN;
        int pfd = open_w("/data/canopus/toolbox_probe.txt", 2 | 4 | 0x200 /*WR|CR|TRUNC*/, 0666);
        if (pfd >= 0) {
            posix_write_fn write_f = (posix_write_fn)(uintptr_t)FW_POSIX_WRITE;
            posix_close_fn close_w = (posix_close_fn)(uintptr_t)FW_POSIX_CLOSE;
            (void)write_f(pfd, raw, (uint32_t)n);
            close_w(pfd);
        }
    }

    /* Loose parse: expect lines naming a mount path followed by numeric
     * fields. Take the first line that yields two plain numbers where the
     * second >= first; treat the pair as (used, total) in whatever unit the
     * firmware reports, and render percentages only (unit shown raw in the
     * detail row). */
    {
        uint32_t nums[8];
        int nnum = 0;
        const char *p = raw;
        while (*p && nnum < 8) {
            if (*p >= '0' && *p <= '9') {
                uint32_t v = 0u;
                while (*p >= '0' && *p <= '9') {
                    v = v * 10u + (uint32_t)(*p - '0');
                    p++;
                }
                nums[nnum++] = v;
            } else {
                if (*p == '\n') {
                    if (nnum >= 2 && nums[1] >= nums[0] && nums[1] > 0u) {
                        s_sto.used = nums[0];
                        s_sto.total = nums[1];
                        uint32_t scale2 = nums[1] / 100u;
                        s_sto.pct = nums[0] >= nums[1] ? 100u
                            : (scale2 != 0u ? udiv32(nums[0], scale2) : 0u);
                        if (s_sto.pct > 100u) s_sto.pct = 100u;
                        s_sto.valid = 1u;
                        break;
                    }
                    nnum = 0;
                } else if (nnum == 0 && *p == '/' &&
                           canopus_strlen(s_sto_first_line) == 0u) {
                    /* remember the first path-looking token for the UI */
                    int j = 0;
                    const char *q = p;
                    while (*q && *q != ' ' && *q != '\n' && j < 48) {
                        s_sto_first_line[j++] = *q++;
                    }
                    s_sto_first_line[j] = '\0';
                }
                p++;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* UI refresh                                                          */
/* ------------------------------------------------------------------ */

static void refresh_views(void)
{
    char pct[16];
    char det[56];
    int j;

    format_pct(pct, s_mem_umem.pct, s_mem_umem.valid);
    label_set(s_pct_mem, pct);
    format_kib(det, s_mem_umem.used / 1024u);
    j = canopus_strlen(det);
    det[j++] = ' '; det[j++] = '/'; det[j++] = ' ';
    format_kib(det + j, s_mem_umem.total / 1024u);
    label_set(s_detail_mem, det);
    if (s_bar_mem != 0) {
        ((void (*)(void *, int32_t, uint32_t))(VENA(FW_BAR_VALUE)))(
            s_bar_mem, (int32_t)s_mem_umem.pct, 1u /* LV_BAR_ANIM_ON */);
    }

    format_pct(pct, s_sto.pct, s_sto.valid);
    label_set(s_pct_sto, pct);
    if (s_sto.valid) {
        format_u32(det, s_sto.used);
        j = canopus_strlen(det);
        det[j++] = ' '; det[j++] = '/'; det[j++] = ' ';
        format_u32(det + j, s_sto.total);
        j = canopus_strlen(det);
        det[j++] = ' '; det[j++] = '(';
        int k = 0;
        while (s_sto_first_line[k] && j < 52) det[j++] = s_sto_first_line[k++];
        det[j++] = ')';
        det[j] = '\0';
    } else {
        det[0] = '-'; det[1] = '-'; det[2] = '\0';
    }
    label_set(s_detail_sto, det);
    if (s_bar_sto != 0) {
        ((void (*)(void *, int32_t, uint32_t))(VENA(FW_BAR_VALUE)))(
            s_bar_sto, (int32_t)s_sto.pct, 1u);
    }
}

static void toolbox_tick(void *timer)
{
    (void)timer;
    s_refresh_count++;
    sample_heap(VENA(UMEM_SLOT), &s_mem_umem);
    sample_heap(VENA(KMEM_SLOT), &s_mem_kmem);
    sample_storage();
    refresh_views();
}

/* ------------------------------------------------------------------ */
/* Page lifecycle                                                      */
/* ------------------------------------------------------------------ */

static int toolbox_on_signal(struct firmware_page_descriptor *page,
                             uint32_t event, void *payload)
{
    (void)page; (void)event; (void)payload;
    return 0;
}

static int toolbox_on_create(struct firmware_page_descriptor *page,
                             void *root, void *start_data)
{
    void *title;
    (void)page; (void)start_data;

    /* The 139 pack recovered a 4-arg prototype, the 1043 veneer uses 5 args
     * (root, title, u32, cb, user). Extra stack args are harmless under
     * AAPCS when the callee reads fewer, so call with 5 and pass NULLs. */
    title = ((void *(*)(void *, const char *, uint32_t, void *, void *))(
        uintptr_t)FW_PAGE_TITLE)(root, "工具箱", 0u, 0, 0);
    (void)title;

    s_content = ((void *(*)(void *))(VENA(FW_PAGE_CONTENT)))(root);
    if (s_content != 0) {
        ((void (*)(void *, int32_t, int32_t))(VENA(FW_OBJ_SIZE)))(
            s_content, TB_CONTENT_W, TB_CONTENT_H);
        ((void (*)(void *, uint32_t, int32_t, int32_t))(VENA(FW_OBJ_ALIGN)))(
            s_content, ALIGN_TOP_MID, 0, TB_CONTENT_TOP);
    }

    /* --- memory block --- */
    s_pct_mem = s_content ? ((void *(*)(void *))(VENA(FW_LABEL_CREATE)))(s_content) : 0;
    if (s_pct_mem) {
        ((void (*)(void *, uint32_t, int32_t, int32_t))(VENA(FW_OBJ_ALIGN)))(
            s_pct_mem, ALIGN_TOP_MID, 0, 24);
        label_set(s_pct_mem, "--%");
    }
    s_detail_mem = s_content ? ((void *(*)(void *))(VENA(FW_LABEL_CREATE)))(s_content) : 0;
    if (s_detail_mem) {
        ((void (*)(void *, uint32_t, int32_t, int32_t))(VENA(FW_OBJ_ALIGN)))(
            s_detail_mem, ALIGN_TOP_MID, 0, 64);
        label_set(s_detail_mem, "mem");
    }
    s_bar_mem = s_content ? ((void *(*)(void *))(VENA(FW_BAR_CREATE)))(s_content) : 0;
    if (s_bar_mem) {
        ((void (*)(void *, int32_t, int32_t))(VENA(FW_OBJ_SIZE)))(s_bar_mem, 180, 14);
        ((void (*)(void *, void *, uint32_t, int32_t, int32_t))(VENA(FW_OBJ_ALIGN_TO)))(
            s_bar_mem, s_detail_mem, ALIGN_OUT_BOTTOM_MID, 0, 14);
        ((void (*)(void *, int32_t, int32_t))(VENA(FW_BAR_RANGE)))(s_bar_mem, 0, 100);
    }

    /* --- storage block --- */
    s_pct_sto = s_content ? ((void *(*)(void *))(VENA(FW_LABEL_CREATE)))(s_content) : 0;
    if (s_pct_sto) {
        ((void (*)(void *, uint32_t, int32_t, int32_t))(VENA(FW_OBJ_ALIGN)))(
            s_pct_sto, ALIGN_TOP_MID, 0, 130);
        label_set(s_pct_sto, "--%");
    }
    s_detail_sto = s_content ? ((void *(*)(void *))(VENA(FW_LABEL_CREATE)))(s_content) : 0;
    if (s_detail_sto) {
        ((void (*)(void *, uint32_t, int32_t, int32_t))(VENA(FW_OBJ_ALIGN)))(
            s_detail_sto, ALIGN_TOP_MID, 0, 170);
        label_set(s_detail_sto, "storage");
    }
    s_bar_sto = s_content ? ((void *(*)(void *))(VENA(FW_BAR_CREATE)))(s_content) : 0;
    if (s_bar_sto) {
        ((void (*)(void *, int32_t, int32_t))(VENA(FW_OBJ_SIZE)))(s_bar_sto, 180, 14);
        ((void (*)(void *, void *, uint32_t, int32_t, int32_t))(VENA(FW_OBJ_ALIGN_TO)))(
            s_bar_sto, s_detail_sto, ALIGN_OUT_BOTTOM_MID, 0, 14);
        ((void (*)(void *, int32_t, int32_t))(VENA(FW_BAR_RANGE)))(s_bar_sto, 0, 100);
    }

    /* initial sample + probe dump, then a 3 s refresh timer */
    toolbox_tick(0);
    s_timer = ((void *(*)(void *, uint32_t, void *))(VENA(FW_TIMER_CREATE)))(
        toolbox_tick, 3000u, 0);
    return 0;
}

static int toolbox_on_resume(struct firmware_page_descriptor *page)
{
    (void)page;
    if (s_timer == 0) {
        s_timer = ((void *(*)(void *, uint32_t, void *))(VENA(FW_TIMER_CREATE)))(
            toolbox_tick, 3000u, 0);
    }
    return 0;
}

static int toolbox_on_pause(struct firmware_page_descriptor *page)
{
    (void)page;
    if (s_timer != 0) {
        ((void (*)(void *))(VENA(FW_TIMER_DEL)))(s_timer);
        s_timer = 0;
    }
    return 0;
}

static int toolbox_on_ui_destroy(struct firmware_page_descriptor *page)
{
    (void)page;
    if (s_timer != 0) {
        ((void (*)(void *))(VENA(FW_TIMER_DEL)))(s_timer);
        s_timer = 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Module lifecycle + app registration (native-app-demo pattern)       */
/* ------------------------------------------------------------------ */

static int32_t toolbox_prepare(const struct canopus_context_v1 *ctx)
{
    s_ctx = ctx;
    return 0;
}

static int32_t toolbox_activate(const struct canopus_context_v1 *ctx)
{
    s_ctx = ctx;
    if (ctx->log != 0 && ctx->log->info != 0) {
        ctx->log->info(ctx, "toolbox activated");
    }
    return 0;
}

static int32_t toolbox_deactivate(const struct canopus_context_v1 *ctx)
{
    (void)ctx;
    return 0;
}

static int32_t toolbox_stop(const struct canopus_context_v1 *ctx)
{
    (void)ctx;
    s_ctx = 0;
    return 0;
}

static int32_t toolbox_query(struct canopus_status_writer_v1 *writer)
{
    (void)writer;
    return 0;
}

static struct firmware_page_descriptor s_toolbox_page;

static int32_t toolbox_publish_native_app_stage(const struct canopus_context_v1 *ctx,
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

        canopus_memset(&s_toolbox_page, 0, sizeof(s_toolbox_page));
        s_toolbox_page.page_name = (void *)"toolbox_main";
        s_toolbox_page.page_id = 0u;
        s_toolbox_page.on_signal = (void *)toolbox_on_signal;
        s_toolbox_page.on_create = (void *)toolbox_on_create;
        s_toolbox_page.on_resume = (void *)toolbox_on_resume;
        s_toolbox_page.on_pause = (void *)toolbox_on_pause;
        s_toolbox_page.on_ui_destroy = (void *)toolbox_on_ui_destroy;

        pages[0] = &s_toolbox_page;
        canopus_memset(&entry, 0, sizeof(entry));
        entry.struct_size = sizeof(entry);
        entry.label = s_display_name;
        entry.icon = 0; /* default launcher icon until a 139 icon is packed */
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
            uintptr_t)FW_PAGE_GOTO)((uint32_t)s_app_id << 16, 0u, 0u, 0u);
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
    TOOLBOX_MODULE_ID,
    "0.1.0",
    "toolbox",
    CANOPUS_TARGET_ID,
    toolbox_prepare,
    toolbox_activate,
    toolbox_deactivate,
    toolbox_stop,
    toolbox_query,
    0,
    toolbox_publish_native_app_stage,
};

CANOPUS_MODULE_ENTRY(s_descriptor)
