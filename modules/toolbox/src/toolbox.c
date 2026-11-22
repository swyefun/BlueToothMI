/*
 * toolbox.c — Band 11 native toolbox application (4.100.139), v0.4.0.
 *
 * A native application that lives in the app launcher (NOT a watchface).
 * Page 1 shows memory and storage stock-style: small section title,
 * large value line, full-width pill bar underneath, refreshed by an
 * LVGL timer.
 *
 * Layout (device-confirmed constraints):
 *   - the stock header area occupies the top 68 px of the page root;
 *     the whole layout starts below it and every object is placed
 *     directly on the root (the recovered stock page_title call rendered
 *     nothing in 0.1.0 and is not used)
 *
 * Storage (0.4.0):
 *   - Bar 1 "总容量": static full pill against the NAND total 128 MiB
 *     (media record 0x8000000 @ 0xcc4e5b0)
 *   - Bar 2 "用户分区": /data used bytes vs 128 MiB. /proc/fs/usage is
 *     dead data on .139 (zero code refs), so used comes from a
 *     NON-recursive /data walk (explicit directory stack, max 8 deep,
 *     max 2048 files, file size via lseek(SEEK_END))
 *   - 0.2.0/0.3.0 hard-locked on load while 0.1.0 (no walk) did not.
 *     0.4.0 adds a full stage probe: every milestone overwrites
 *     /data/canopus/toolbox_stage.txt (walk start/done, raw dirent
 *     hexdumps, tick counters), so one lock-up + log export pins the
 *     exact failing step. The dirent layout assumption (+1 d_name) is
 *     verified by the dumps.
 *
 * Memory: mm_mallinfo(heap) 0x0c34f0a1 on the Umem heap (verified
 * working on device in 0.1.0). All firmware veneers are XOR 0x06000000
 * encoded; no 64-bit division (zero-import target).
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

#define FW_POSIX_OPEN   0x0c342c55u /* int open(const char*, int, ...)      */
#define FW_POSIX_WRITE  0xc33dc4fu  /* int write(int, const void*, uint32_t)*/
#define FW_POSIX_CLOSE  0xc33818du  /* int close(int)                       */
#define FW_POSIX_LSEEK  0x0c33d511u /* int64 lseek(int, int64, int)         */
#define FW_POSIX_OPENDIR 0x0c351749u/* void* opendir(const char*)           */
#define FW_POSIX_READDIR 0x0c34956bu/* void* readdir(void*)                 */
#define FW_POSIX_CLOSEDIR 0x0c34cd41u/* int closedir(void*)                 */

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
#define DATA_TOTAL_BYTES 0x08000000u /* NAND media record @ 0xcc4e5b0 */

typedef int (*posix_open_fn)(const char *, int, ...);
typedef int (*posix_write_fn)(int, const void *, uint32_t);
typedef int (*posix_close_fn)(int);
typedef long long (*posix_lseek_fn)(int, long long, int);
typedef void *(*posix_opendir_fn)(const char *);
typedef void *(*posix_readdir_fn)(void *);
typedef int (*posix_closedir_fn)(void *);

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
static struct res_sample s_user; /* /data walk vs 128 MiB */

static uint32_t s_refresh_count;
static uint32_t s_walk_files;
static uint32_t s_walk_truncated;

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
/* Stage probe: every milestone overwrites the stage file so one       */
/* hard-lock + log export pins the exact failing step.                 */
/* ------------------------------------------------------------------ */

static void stage_write(const char *line, uint32_t len)
{
    posix_open_fn open_f = (posix_open_fn)(uintptr_t)VENA(FW_POSIX_OPEN);
    posix_write_fn write_f = (posix_write_fn)(uintptr_t)VENA(FW_POSIX_WRITE);
    posix_close_fn close_f = (posix_close_fn)(uintptr_t)VENA(FW_POSIX_CLOSE);
    int fd = open_f("/data/canopus/toolbox_stage.txt", 2 | 4 | 0x200, 0666);
    if (fd >= 0) {
        (void)write_f(fd, line, len);
        (void)close_f(fd);
    }
}

static void stage_mark(const char *msg, uint32_t a, uint32_t b)
{
    char line[80];
    int j = 0, k;

    for (k = 0; msg[k] != '\0' && j < 60; k++) line[j++] = msg[k];
    line[j++] = ' ';
    format_u32(line + j, a);
    while (line[j] != '\0') j++;
    line[j++] = ' ';
    format_u32(line + j, b);
    while (line[j] != '\0') j++;
    line[j++] = '\r';
    line[j++] = '\n';
    stage_write(line, (uint32_t)j);
}

/* ------------------------------------------------------------------ */
/* Storage: NON-recursive /data walk (explicit dir stack)              */
/* ------------------------------------------------------------------ */

static uint32_t walk_dir(const char *root)
{
    static char dirstack[8][192];
    static char child[192];
    posix_opendir_fn opendir_f = (posix_opendir_fn)(uintptr_t)VENA(FW_POSIX_OPENDIR);
    posix_readdir_fn readdir_f = (posix_readdir_fn)(uintptr_t)VENA(FW_POSIX_READDIR);
    posix_closedir_fn closedir_f = (posix_closedir_fn)(uintptr_t)VENA(FW_POSIX_CLOSEDIR);
    posix_open_fn open_f = (posix_open_fn)(uintptr_t)VENA(FW_POSIX_OPEN);
    posix_close_fn close_f = (posix_close_fn)(uintptr_t)VENA(FW_POSIX_CLOSE);
    posix_lseek_fn lseek_f = (posix_lseek_fn)(uintptr_t)VENA(FW_POSIX_LSEEK);

    int sp = 0;
    uint32_t sum = 0u;
    uint32_t dumped = 0u;
    int k;

    for (k = 0; root[k] != '\0' && k < 190; k++) dirstack[0][k] = root[k];
    dirstack[0][k] = '\0';
    sp = 1;

    while (sp > 0 && s_walk_files < 2048u) {
        char *cur = dirstack[sp - 1];
        uint32_t plen = ustrlen(cur);
        void *dir = opendir_f(cur);
        if (dir == 0) { sp--; continue; }
        sp--;

        for (;;) {
            void *ent = readdir_f(dir);
            const char *name;
            uint32_t nlen, clen;
            int fd;

            if (ent == 0) break;
            /* Assumed NuttX dirent: +0 uint8_t d_type, +1 char d_name[].
             * The first raw entries are hexdumped to the stage probe to
             * confirm this layout on device. */
            name = (const char *)ent + 1;
            if (name[0] == '\0') break;
            if (name[0] == '.' && (name[1] == '\0' ||
                (name[1] == '.' && name[2] == '\0'))) {
                continue;
            }
            nlen = ustrlen(name);
            clen = plen + 1u + nlen + 1u;
            if (clen > 190u) continue;

            {
                uint32_t j = 0, q;
                for (q = 0; q < plen; q++) child[j++] = cur[q];
                child[j++] = '/';
                for (q = 0; q < nlen; q++) child[j++] = name[q];
                child[j] = '\0';
            }

            if (dumped < 4u) {
                /* raw first bytes of the dirent record (layout evidence) */
                char rawdump[112];
                const char hexd[17] = "0123456789abcdef";
                uint32_t j = 0, q;
                rawdump[j++] = 'e'; rawdump[j++] = 'n'; rawdump[j++] = 't';
                rawdump[j++] = ' ';
                for (q = 0; q < 24u; q++) {
                    unsigned char b = (unsigned char)((const char *)ent)[q];
                    rawdump[j++] = hexd[b >> 4];
                    rawdump[j++] = hexd[b & 0xF];
                    rawdump[j++] = ' ';
                    if ((q & 7u) == 7u) rawdump[j++] = '|';
                }
                rawdump[j++] = '\r';
                rawdump[j++] = '\n';
                stage_write(rawdump, j);
                dumped++;
            }

            /* directory? try opendir (cheap and unambiguous) */
            {
                void *sub = opendir_f(child);
                if (sub != 0) {
                    (void)closedir_f(sub);
                    s_walk_files++;
                    if (sp < 8) {
                        for (k = 0; child[k] != '\0'; k++) dirstack[sp][k] = child[k];
                        dirstack[sp][k] = '\0';
                        sp++;
                    }
                    continue;
                }
            }

            fd = open_f(child, 0);
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
    }
    return sum;
}

static void sample_storage(void)
{
    s_user.valid = 0u;
    s_user.pct = 0u;
    s_walk_files = 0u;
    s_walk_truncated = 0u;

    stage_mark("walk-start", 0u, 0u);
    s_user.used = walk_dir("/data");
    stage_mark("walk-done", s_walk_files, s_user.used);

    s_user.total = DATA_TOTAL_BYTES;
    {
        uint32_t scale = DATA_TOTAL_BYTES / 100u;
        s_user.pct = s_user.used >= DATA_TOTAL_BYTES ? 100u
                   : (scale != 0u ? (s_user.used / scale) : 0u);
        if (s_user.pct > 100u) s_user.pct = 100u;
        s_user.valid = 1u;
    }
    stage_mark("storage-done", s_user.pct, s_user.total);
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

    format_pair(val, s_user.used, s_user.total, s_user.valid);
    label_set(s_val_user, val);
    if (s_bar_user != 0) {
        ((void (*)(void *, int32_t, uint32_t))(uintptr_t)VENA(FW_BAR_VALUE))(
            s_bar_user, (int32_t)s_user.pct, 1u);
    }
}

static void toolbox_tick(void *timer)
{
    (void)timer;
    s_refresh_count++;
    stage_mark("tick", s_refresh_count, 0u);
    sample_heap(UMEM_SLOT, &s_mem_umem);
    stage_mark("heap-done", s_mem_umem.pct, s_mem_umem.arena);
    sample_storage();
    refresh_views();
    stage_mark("refresh-done", s_refresh_count, s_user.used);
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

    stage_mark("create", 0u, 0u);

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

    stage_mark("ui-done", 0u, 0u);
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
    "0.4.0",
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
