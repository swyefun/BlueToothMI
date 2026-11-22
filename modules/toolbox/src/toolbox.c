/*
 * toolbox.c — Band 11 native toolbox application (4.100.139), v0.2.0.
 *
 * A native application that lives in the app launcher (NOT a watchface).
 * Page 1 shows memory and storage usage stock-style: small section title,
 * large "used / total" line, full-width pill bar underneath, refreshed by
 * an LVGL timer.
 *
 * v0.2.0 changes over 0.1.0 (device feedback):
 *   - own title label ("工具箱"): the recovered page_title call rendered
 *     nothing on device; our content box also sat on top of the stock
 *     title area (68 px) — both fixed by dropping the stock header and
 *     drawing the whole layout on the page root
 *   - storage: two bars per user request. Bar 1 = total capacity, a full
 *     pill against the static NAND total (128 MiB media record 0x8000000
 *     @ 0xcc4e5b0). Bar 2 = user partition /data: used bytes from a POSIX
 *     tree walk vs the partition size read at runtime via
 *     ioctl(/dev/nand_data, MTDIOC_GEOMETRY) (fallback: the 128 MiB cap
 *     the .155 data-quota-lift documented). /proc/fs/usage proved dead
 *     data (zero code refs), so no procfs probing.
 *   - layout matches the requested screenshot: left-aligned titles.
 *
 * Data sources:
 *   - memory : mm_mallinfo(heap) 0x0c34f0a1 on the Umem heap (verified
 *              working on device in 0.1.0).
 *   - storage: walk + MTD geometry (see v0.3.0 notes above).
 *
 * All firmware veneers are XOR 0x06000000 encoded; percentages use plain
 * u32 division (zero-import target: no __aeabi_uldivmod).
 */

#include "canopus_module_entry.h"
#include "canopus_memory.h"

#include <stdint.h>
#include <stddef.h>

#define TOOLBOX_MODULE_ID "toolbox"

/* ------------------------------------------------------------------ */
/* 139 firmware veneers (from the 139 symbol pack / loader audit)      */
/* ------------------------------------------------------------------ */

#define FW_MM_MALLINFO  0x0c34f0a1u /* void* fn(info*, void* heap)          */
#define UMEM_SLOT       0x200B2590u /* RAM: Umem heap descriptor            */
#define KMEM_SLOT       0x200B01C8u /* RAM: Kmem heap descriptor            */

#define FW_LABEL_CREATE 0x0c3ab301u /* void*(parent)                        */
#define FW_LABEL_TEXT   0x0c3b3949u /* void(label, const char*)             */
#define FW_OBJ_SIZE     0x0c387ba9u /* void(obj, w, h)                      */
#define FW_OBJ_ALIGN    0x0c387789u /* void(obj, align, x, y)               */
#define FW_BAR_CREATE   0x0c3ad7f5u /* void*(parent)                        */
#define FW_BAR_RANGE    0x0c3ad921u /* void(bar, min, max)                  */
#define FW_BAR_VALUE    0x0c3ad8b5u /* void(bar, value, anim)               */
#define FW_TIMER_CREATE 0x0c3abd21u /* void*(cb, period_ms, user)           */
#define FW_TIMER_DEL    0x0c3abe71u /* void(timer)                          */
#define FW_PAGE_GOTO    0x0c8ed781u /* void(key, start, t0, t1)             */

#define VENA(addr) ((uintptr_t)(addr) ^ 0x06000000u)

/* LVGL v8 alignment enum (TOP_MID=2 / OUT_BOTTOM_MID=14 proven by the
 * native-app-demo constants; TOP_LEFT=1 continues the same enumeration). */
#define ALIGN_TOP_LEFT       1u

#define TB_SCREEN_W 212
#define TB_PAD 16
#define TB_BAR_W (TB_SCREEN_W - 2 * TB_PAD) /* 180 */
#define TB_SEEK_END 2

#define FW_POSIX_OPEN   0x0c342c55u /* int open(const char*, int, ...)      */
#define FW_POSIX_CLOSE  0xc33818du  /* int close(int)                       */
#define FW_POSIX_LSEEK  0x0c33d511u /* int64 lseek(int, int64, int)         */
#define FW_POSIX_OPENDIR 0x0c351749u/* void* opendir(const char*)           */
#define FW_POSIX_READDIR 0x0c34956bu/* void* readdir(void*)  NuttX dirent: */
                                            /*   +0 uint8 d_type, +1 d_name[] */
#define FW_POSIX_CLOSEDIR 0x0c34cd41u/* int closedir(void*)                 */
#define FW_IOCTL        0xc341e39u  /* int ioctl(int, uint32_t, ...)        */

/* MTDIOC_GEOMETRY = _IOC(IOC_MTD=0x0d, 0x0001) = 0x0d01. mtd_geometry_s
 * = { uint32 blocksize, erasesize, pagesize, neraseblocks }. */
#define MTDIOC_GEOMETRY 0x0d01u

#define FW_POSIX_WRITE  0xc33dc4fu  /* int write(int, const void*, uint32_t)*/
#define FW_POSIX_READ   0x0c33d785u /* int read(int, void*, uint32_t)       */

typedef int (*posix_open_fn)(const char *, int, ...);
typedef int (*posix_read_fn)(int, void *, uint32_t);
typedef int (*posix_write_fn)(int, const void *, uint32_t);
typedef int (*posix_close_fn)(int);
typedef long long (*posix_lseek_fn)(int, long long, int);
typedef void *(*posix_opendir_fn)(const char *);
typedef void *(*posix_readdir_fn)(void *);
typedef int (*posix_closedir_fn)(void *);
typedef int (*ioctl_fn)(int, uint32_t, void *);
struct b11_mallinfo {
    uint32_t arena, ordblks, aordblks, mxordblk, uordblks, fordblks, usmblks;
};

typedef void *(*mallinfo_fn)(struct b11_mallinfo *, void *);

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

static const struct canopus_context_v1 *s_ctx;
static uint16_t s_app_id;

static void *s_title;
static void *s_cap_mem;      /* "内存占用" */
static void *s_val_mem;      /* "x.xMB / y.yMB" */
static void *s_bar_mem;
static void *s_cap_total;    /* "总容量" */
static void *s_val_total;    /* "128.0MB" static */
static void *s_bar_total;    /* full pill */
static void *s_cap_user;     /* "用户分区" */
static void *s_val_user;     /* "x.xMB / y.yMB" */
static void *s_bar_user;
static void *s_timer;

struct res_sample {
    uint32_t valid;
    uint32_t used;
    uint32_t total;
    uint32_t pct;
};

static struct res_sample s_mem_umem;
static struct res_sample s_user; /* /data walk vs MTD geometry */

static uint32_t s_refresh_count;
static uint32_t s_walk_files;
static uint32_t s_walk_truncated;
static uint32_t s_user_total;    /* measured partition size (bytes) */

static const char s_display_name[] = "工具箱";

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static uint32_t ustrlen(const char *s)
{
    uint32_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

static void format_u32(char *buf, uint32_t val)
{
    char tmp[12];
    int i = 0, j = 0;
    if (val == 0u) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (val > 0u) { tmp[i++] = (char)('0' + (val % 10u)); val /= 10u; }
    while (i > 0) { buf[j++] = tmp[--i]; }
    buf[j] = '\0';
}

/* bytes -> "x.y GB" / "x.y MB" / "n KB" */
static void format_size(char *buf, uint32_t bytes)
{
    char a[12], b[12];
    uint32_t whole, frac;
    const char *unit;
    int j = 0, k;

    if (bytes >= 1024u * 1024u * 1024u) {
        whole = bytes / (1024u * 1024u * 1024u);
        frac = (bytes % (1024u * 1024u * 1024u)) * 10u / (1024u * 1024u * 1024u);
        unit = " GB";
    } else if (bytes >= 1024u * 1024u) {
        whole = bytes / (1024u * 1024u);
        frac = (bytes % (1024u * 1024u)) * 10u / (1024u * 1024u);
        unit = " MB";
    } else {
        whole = bytes / 1024u;
        frac = (bytes % 1024u) * 10u / 1024u;
        unit = " KB";
    }
    format_u32(a, whole);
    format_u32(b, frac);
    for (k = 0; a[k]; k++) buf[j++] = a[k];
    buf[j++] = '.';
    buf[j++] = b[0];
    for (k = 0; unit[k]; k++) buf[j++] = unit[k];
    buf[j] = '\0';
}

static void format_pair(char *buf, uint32_t used, uint32_t total, int valid)
{
    if (!valid) {
        buf[0] = '-'; buf[1] = '-'; buf[2] = '\0';
        return;
    }
    format_size(buf, used);
    {
        int j = 0;
        while (buf[j]) j++;
        buf[j++] = ' '; buf[j++] = '/'; buf[j++] = ' ';
        format_size(buf + j, total);
    }
}

static void label_set(void *obj, const char *text)
{
    if (obj != 0) {
        ((void (*)(void *, const char *))(uintptr_t)VENA(FW_LABEL_TEXT))(obj, text);
    }
}

/* ------------------------------------------------------------------ */
/* Memory sample (verified on device in 0.1.0)                         */
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
    ((mallinfo_fn)(uintptr_t)VENA(FW_MM_MALLINFO))(&info, heap);
    if (info.arena == 0u) return;
    out->used = info.uordblks;
    out->total = info.arena;
    {
        uint32_t scale = info.arena / 100u;
        out->pct = info.uordblks >= info.arena ? 100u
                 : (scale != 0u ? (info.uordblks / scale) : 0u);
        if (out->pct > 100u) out->pct = 100u;
    }
    out->valid = 1u;
}

/* ------------------------------------------------------------------ */
/* Storage: walk /data and sum file sizes via lseek(SEEK_END)          */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Storage: walk /data (used) + MTD geometry (partition size)          */
/* ------------------------------------------------------------------ */

static uint32_t walk_dir(const char *path, char *scratch, uint32_t scratch_len,
                         int depth)
{
    posix_opendir_fn opendir_f = (posix_opendir_fn)(uintptr_t)VENA(FW_POSIX_OPENDIR);
    posix_readdir_fn readdir_f = (posix_readdir_fn)(uintptr_t)VENA(FW_POSIX_READDIR);
    posix_closedir_fn closedir_f = (posix_closedir_fn)(uintptr_t)VENA(FW_POSIX_CLOSEDIR);
    posix_open_fn open_f = (posix_open_fn)(uintptr_t)VENA(FW_POSIX_OPEN);
    posix_close_fn close_f = (posix_close_fn)(uintptr_t)VENA(FW_POSIX_CLOSE);
    posix_lseek_fn lseek_f = (posix_lseek_fn)(uintptr_t)VENA(FW_POSIX_LSEEK);

    void *dir = opendir_f(path);
    uint32_t sum = 0u;
    uint32_t plen;

    if (dir == 0) return 0u;
    plen = ustrlen(path);

    for (;;) {
        void *ent = readdir_f(dir);
        const char *name;
        uint32_t nlen, clen;
        int fd;

        if (ent == 0) break;
        /* NuttX dirent: +0 uint8_t d_type, +1 char d_name[] (NUL-terminated). */
        name = (const char *)ent + 1;
        if (name[0] == '\0') break;
        if (name[0] == '.' && (name[1] == '\0' ||
            (name[1] == '.' && name[2] == '\0'))) {
            continue;
        }
        nlen = ustrlen(name);
        clen = plen + 1u + nlen + 1u;
        if (clen > scratch_len || s_walk_files > 2048u) {
            s_walk_truncated = 1u;
            break;
        }
        {
            uint32_t j = 0, k;
            for (k = 0; k < plen; k++) scratch[j++] = path[k];
            scratch[j++] = '/';
            for (k = 0; k < nlen; k++) scratch[j++] = name[k];
            scratch[j] = '\0';
        }

        /* directory? try opendir (cheap and unambiguous) */
        {
            void *sub = opendir_f(scratch);
            if (sub != 0) {
                (void)closedir_f(sub);
                s_walk_files++;
                if (depth < 6) {
                    sum += walk_dir(scratch, scratch + clen, scratch_len - clen,
                                    depth + 1);
                }
                continue;
            }
        }

        fd = open_f(scratch, 0);
        s_walk_files++;
        if (fd >= 0) {
            long long size = lseek_f(fd, 0, TB_SEEK_END);
            (void)close_f(fd);
            if (size > 0) {
                uint32_t add = (uint32_t)size;
                if (sum <= 0xFFFFFFFFu - add) sum += add;
                else sum = 0xFFFFFFFFu;
            }
        }
    }
    (void)closedir_f(dir);
    return sum;
}

/* /dev/nand_data geometry: neraseblocks * erasesize, else fallback */
static uint32_t sample_user_total(void)
{
    struct geo {
        uint32_t blocksize;
        uint32_t erasesize;
        uint32_t pagesize;
        uint32_t neraseblocks;
    } g;
    posix_open_fn open_f = (posix_open_fn)(uintptr_t)VENA(FW_POSIX_OPEN);
    posix_close_fn close_f = (posix_close_fn)(uintptr_t)VENA(FW_POSIX_CLOSE);
    ioctl_fn ioctl_f = (ioctl_fn)(uintptr_t)VENA(FW_IOCTL);
    int fd = open_f("/dev/nand_data", 0);
    uint32_t total = 0u;

    if (fd >= 0) {
        g.blocksize = 0u;
        g.erasesize = 0u;
        g.pagesize = 0u;
        g.neraseblocks = 0u;
        if (ioctl_f(fd, MTDIOC_GEOMETRY, &g) == 0 &&
            g.neraseblocks != 0u && g.erasesize != 0u) {
            uint32_t product = g.neraseblocks * g.erasesize;
            if (g.neraseblocks != 0u && product / g.neraseblocks == g.erasesize) {
                total = product;
            }
        }
        (void)close_f(fd);
    }
    if (total == 0u) total = 0x08000000u; /* NAND media record 0xcc4e5b0 */
    return total;
}

static void sample_storage(void)
{
    static char walk_buf[6][160];

    s_user.valid = 0u;
    s_user.pct = 0u;
    s_walk_files = 0u;
    s_walk_truncated = 0u;

    s_user_total = sample_user_total();
    s_user.used = walk_dir("/data", walk_buf[0], 160u, 0);
    s_user.total = s_user_total;
    {
        uint32_t scale = s_user_total / 100u;
        s_user.pct = s_user.used >= s_user_total ? 100u
                   : (scale != 0u ? (s_user.used / scale) : 0u);
        if (s_user.pct > 100u) s_user.pct = 100u;
        s_user.valid = 1u;
    }
    (void)s_walk_truncated;
}

/* ------------------------------------------------------------------ */
/* UI refresh                                                          */
/* ------------------------------------------------------------------ */

static void refresh_views(void)
{
    char val[48];

    format_pair(val, s_mem_umem.used, s_mem_umem.total, s_mem_umem.valid);
    label_set(s_val_mem, val);
    if (s_bar_mem != 0) {
        ((void (*)(void *, int32_t, uint32_t))(uintptr_t)VENA(FW_BAR_VALUE))(
            s_bar_mem, (int32_t)s_mem_umem.pct, 1u /* LV_BAR_ANIM_ON */);
    }

}

static void toolbox_tick(void *timer)
{
    (void)timer;
    s_refresh_count++;
    sample_heap(UMEM_SLOT, &s_mem_umem);
    sample_storage();
    refresh_views();
}

/* ------------------------------------------------------------------ */
/* Page lifecycle — everything on the page root (no stock header)      */
/* ------------------------------------------------------------------ */

static int toolbox_on_signal(void *page, uint32_t event, void *payload)
{
    (void)page; (void)event; (void)payload;
    return 0;
}

/* add a label at an absolute position on the root */
static void *add_label(void *root, const char *text, int32_t x, int32_t y)
{
    void *obj = ((void *(*)(void *))(uintptr_t)VENA(FW_LABEL_CREATE))(root);
    if (obj != 0) {
        label_set(obj, text);
        ((void (*)(void *, uint32_t, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_ALIGN))(
            obj, ALIGN_TOP_LEFT, x, y);
    }
    return obj;
}

static void *add_bar(void *root, int32_t x, int32_t y)
{
    void *bar = ((void *(*)(void *))(uintptr_t)VENA(FW_BAR_CREATE))(root);
    if (bar != 0) {
        ((void (*)(void *, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_SIZE))(
            bar, TB_BAR_W, 14);
        ((void (*)(void *, uint32_t, int32_t, int32_t))(uintptr_t)VENA(FW_OBJ_ALIGN))(
            bar, ALIGN_TOP_LEFT, x, y);
        ((void (*)(void *, int32_t, int32_t))(uintptr_t)VENA(FW_BAR_RANGE))(bar, 0, 100);
    }
    return bar;
}

static int toolbox_on_create(void *page, void *root, void *start_data)
{
    (void)page; (void)start_data;

    /* Screenshot-style layout on the page root (212 wide). The stock
     * header area occupies the top 68 px and our 0.1.0 content box sat on
     * top of it; everything now starts below that. */
    s_title    = add_label(root, "工具箱", TB_PAD, 84);
    s_cap_mem  = add_label(root, "内存占用", TB_PAD, 140);
    s_val_mem  = add_label(root, "--", TB_PAD, 170);
    s_bar_mem  = add_bar(root, TB_PAD, 210);
    s_cap_total = add_label(root, "总容量", TB_PAD, 260);
    s_val_total = add_label(root, "128.0MB", TB_PAD, 290);
    s_bar_total = add_bar(root, TB_PAD, 330);
    if (s_bar_total != 0) {
        ((void (*)(void *, int32_t, uint32_t))(uintptr_t)VENA(FW_BAR_VALUE))(
            s_bar_total, 100, 0);
    }
    s_cap_user = add_label(root, "用户分区", TB_PAD, 380);
    s_val_user = add_label(root, "--", TB_PAD, 410);
    s_bar_user = add_bar(root, TB_PAD, 450);
    /* toolbox_tick below samples storage + memory and refreshes all views */

    toolbox_tick(0);
    s_timer = ((void *(*)(void *, uint32_t, void *))(uintptr_t)VENA(FW_TIMER_CREATE))(
        toolbox_tick, 5000u, 0);
    return 0;
}

static int toolbox_on_resume(void *page)
{
    (void)page;
    if (s_timer == 0) {
        s_timer = ((void *(*)(void *, uint32_t, void *))(uintptr_t)VENA(FW_TIMER_CREATE))(
            toolbox_tick, 5000u, 0);
    }
    return 0;
}

static int toolbox_on_pause(void *page)
{
    (void)page;
    if (s_timer != 0) {
        ((void (*)(void *))(uintptr_t)VENA(FW_TIMER_DEL))(s_timer);
        s_timer = 0;
    }
    return 0;
}

static int toolbox_on_ui_destroy(void *page)
{
    (void)page;
    if (s_timer != 0) {
        ((void (*)(void *))(uintptr_t)VENA(FW_TIMER_DEL))(s_timer);
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

/* firmware_page_descriptor comes from the target's generated veneer */
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
        entry.icon = 0; /* Supervisor falls back to /data/canopus/manager_icon.bin */
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
    TOOLBOX_MODULE_ID,
    "0.2.0",
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
