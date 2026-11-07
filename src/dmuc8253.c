#define DMOD_ENABLE_REGISTRATION    ON
#include "dmod.h"
#include "dmuc8253.h"
#include "dmuc8253_panel.h"
#include "dmuc8253_splash.h"
#include "dmdrvi.h"
#include "dmini.h"
#include "dmosi.h"
#include "dmgpio_types.h"
#include <errno.h>
#include <string.h>

/*
 * dmuc8253: an e-paper display with a UC8253 controller on SPI (the 3.1" GDEQ031T10 of the LilyGO T-Deck Pro: 240x320, black and white).
 *
 * The controller keeps two pictures in a RAM of its own: the one on the screen ("old", command 0x10) and the next one ("new", 0x13); a
 * refresh turns the screen from the first into the second. The driver keeps a framebuffer (MONO1: 1 bit per pixel, set = white) that
 * clients draw into, and PRESENT sends the area drawn to the new picture, refreshes just that area (a partial refresh, 0.7 s, only
 * the pixels that change are driven) and then writes the area to the old picture too, so the next refresh starts from what is on the
 * screen. The very first refresh, and every full_refresh_interval-th one, is a full one (1.1 s, the whole screen, no ghosts). The node
 * follows the generic graphics interface (DMDRVI_IOCTL_GFX_*) of dmdrvi.
 *
 * The wiring is done by dmdevfs: the sections of the display, of its SPI bus (dmspi), of the data/command line, of the BUSY line and
 * of the reset are in one friends_group, and the roles of the friends (friend_role=...) tell this driver which node is which:
 *
 *   spi    the dmspi node the display is on (required)
 *   dc     the data/command line: low for a command, high for data (required)
 *   busy   the BUSY line, a dmgpio input: low while the controller works (required)
 *   reset  the reset line, low = reset (when the configuration says reset=yes; without it the controller is reset by command)
 *
 * The chip select is the business of dmspi (friend_role=chip_select, nss_mode=soft): it frames every write. The panel is started once
 * the required friends are there, whichever order they come in. The command sequences are those of the vendor's library (GxEPD2).
 */

/* Magic set to "UC82" */
#define DMUC8253_CONTEXT_MAGIC      0x55433832

#define CMD_PSR                     0x00    /* panel setting */
#define CMD_POF                     0x02    /* power off */
#define CMD_PON                     0x04    /* power on */
#define CMD_DTM1                    0x10    /* the old picture (what is on the screen) */
#define CMD_DRF                     0x12    /* display refresh */
#define CMD_DTM2                    0x13    /* the new picture */
#define CMD_CDI                     0x50    /* VCOM and data interval */
#define CMD_PTL                     0x90    /* partial window */
#define CMD_PTIN                    0x91    /* partial in */
#define CMD_PTOUT                   0x92    /* partial out */
#define CMD_CCSET                   0xE0    /* cascade setting */
#define CMD_TSSET                   0xE5    /* force temperature */

#define BUSY_POLL_MS                5U
#define BUSY_TIMEOUT_MS             10000U  /* the vendor's: a refresh takes 1.1 s */

typedef struct
{
    char   *path;                   /**< Node of the friend (NULL: not there) */
    void   *file;                   /**< Kept open once used */
} node_t;

typedef struct
{
    uint16_t x0, y0, x1, y1;        /**< Inclusive, in the picture of the clients; x0 > x1: nothing */
} area_t;

struct dmdrvi_context
{
    uint32_t            magic;
    dmuc8253_config_t   config;
    uint8_t             instance;
    uint16_t            width;              /**< As shown (after the rotation) */
    uint16_t            height;
    uint32_t            stride;
    uint32_t            framebuffer_size;
    uint8_t            *framebuffer;
    uint8_t            *packed;             /**< The window of the panel on its way to the controller */
    dmosi_mutex_t       lock;
    node_t              spi;
    node_t              dc;
    node_t              busy;
    node_t              reset;
    bool                reset_wired;        /**< The configuration says there is a reset line to wait for */
    bool                started;            /**< The panel was initialised and shows the framebuffer */
    bool                display_enabled;
    uint32_t            partial_count;      /**< Partial refreshes since the last full one */
    area_t              dirty;              /**< Written through write() / FILL_RECT, not presented yet */
};

static bool is_valid_context(dmdrvi_context_t context)
{
    return context != NULL && context->magic == DMUC8253_CONTEXT_MAGIC;
}

/* ---- API ---- */

dmod_dmuc8253_api_declaration(1.0, bool, _validate_config, ( const dmuc8253_config_t *config ))
{
    if (config == NULL || config->width == 0 || config->height == 0 || config->rotation > dmuc8253_rotation_270)
        return false;
    return config->width <= UC8253_PANEL_MAX_WIDTH && config->height <= UC8253_PANEL_MAX_HEIGHT && (config->width % 8U) == 0U;
}

dmod_dmuc8253_api_declaration(1.0, bool, _color_to_mono, ( uint32_t argb ))
{
    return uc8253_color_is_white(argb);
}

dmod_dmuc8253_api_declaration(1.0, int, _splash_draw, ( const char *path, const dmuc8253_splash_target_t *target ))
{
    return dmuc8253_splash_draw_file(path, target);
}

/* ---- Configuration ---- */

static bool string_to_switch(const char *text, bool fallback)
{
    if (text == NULL)
        return fallback;
    if (strcmp(text, "on") == 0 || strcmp(text, "yes") == 0 || strcmp(text, "true") == 0 || strcmp(text, "1") == 0)
        return true;
    if (strcmp(text, "off") == 0 || strcmp(text, "no") == 0 || strcmp(text, "false") == 0 || strcmp(text, "0") == 0)
        return false;
    return fallback;
}

/* A number, decimal or 0x hex */
static bool parse_number(const char *text, uint32_t *value)
{
    uint64_t n = 0;
    unsigned base = 10;
    if (text == NULL || *text == '\0')
        return false;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        base = 16;
        text += 2;
    }
    if (*text == '\0')
        return false;
    for (; *text != '\0'; text++)
    {
        int d = (*text >= '0' && *text <= '9') ? *text - '0'
              : (*text >= 'a' && *text <= 'f') ? *text - 'a' + 10
              : (*text >= 'A' && *text <= 'F') ? *text - 'A' + 10 : -1;
        if (d < 0 || (unsigned)d >= base)
            return false;
        n = n * base + (unsigned)d;
        if (n > 0xFFFFFFFFULL)
            return false;
    }
    *value = (uint32_t)n;
    return true;
}

static int read_config(dmdrvi_context_t context, dmini_context_t ini)
{
    dmuc8253_config_t *c = &context->config;
    int rotation = dmini_get_int(ini, NULL, "rotation", 0);
    const char *clear = dmini_get_string(ini, NULL, "clear_color", NULL);
    int interval = dmini_get_int(ini, NULL, "full_refresh_interval", 0);

    context->instance = (uint8_t)dmini_get_int(ini, NULL, "instance", 0);
    c->width    = (uint16_t)dmini_get_int(ini, NULL, "width", (int)UC8253_PANEL_MAX_WIDTH);
    c->height   = (uint16_t)dmini_get_int(ini, NULL, "height", (int)UC8253_PANEL_MAX_HEIGHT);
    c->partial  = string_to_switch(dmini_get_string(ini, NULL, "partial", NULL), true);
    c->clear_color = 0xFFFFFFFFU;
    if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270)
    {
        DMOD_LOG_ERROR("Invalid rotation %d (0, 90, 180 or 270)\n", rotation);
        return -EINVAL;
    }
    c->rotation = (dmuc8253_rotation_t)(rotation / 90);
    if (interval < 0 || interval > 0xFFFF)
    {
        DMOD_LOG_ERROR("Invalid full_refresh_interval %d (0..65535)\n", interval);
        return -EINVAL;
    }
    c->full_refresh_interval = (uint16_t)interval;
    if (clear != NULL && !parse_number(clear, &c->clear_color))
    {
        DMOD_LOG_ERROR("Invalid clear_color '%s'\n", clear);
        return -EINVAL;
    }
    context->reset_wired = string_to_switch(dmini_get_string(ini, NULL, "reset", NULL), false);
    context->display_enabled = string_to_switch(dmini_get_string(ini, NULL, "display", NULL), true);

    if (!dmuc8253_validate_config(c))
    {
        DMOD_LOG_ERROR("Invalid display configuration (%ux%u, a width is a multiple of 8 up to 240, a height up to 320)\n", c->width, c->height);
        return -EINVAL;
    }
    bool swap = uc8253_rotation_swaps(c->rotation);
    context->width  = swap ? c->height : c->width;
    context->height = swap ? c->width : c->height;
    context->stride = ((uint32_t)context->width + 7U) / 8U;
    context->framebuffer_size = context->stride * context->height;
    return 0;
}

/* ---- The nodes of the friends ---- */

static void node_close(node_t *node)
{
    if (node->file != NULL)
        Dmod_FileClose(node->file);
    node->file = NULL;
}

static void node_forget(node_t *node)
{
    node_close(node);
    Dmod_Free(node->path);
    node->path = NULL;
}

static void *node_file(node_t *node)
{
    if (node->file == NULL && node->path != NULL)
        node->file = Dmod_FileOpen(node->path, "r+");
    return node->file;
}

static int gpio_set(node_t *node, bool high)
{
    dmgpio_pins_state_t state = high ? dmgpio_pins_state_all_high : dmgpio_pins_state_all_low;
    void *file = node_file(node);
    if (file == NULL)
        return -ENODEV;
    if (Dmod_Ioctl(file, dmgpio_ioctl_cmd_set_pins_state, &state) != 0)
    {
        node_close(node);
        return -EIO;
    }
    return 0;
}

/* The level of a dmgpio input: 1 high, 0 low, a negative errno when the node cannot be read */
static int gpio_get(node_t *node)
{
    dmgpio_pins_mask_t high = 0;
    void *file = node_file(node);
    if (file == NULL)
        return -ENODEV;
    if (Dmod_Ioctl(file, dmgpio_ioctl_cmd_get_high_pins_state, &high) != 0)
    {
        node_close(node);
        return -EIO;
    }
    return high != 0 ? 1 : 0;
}

static int spi_write(dmdrvi_context_t context, const void *data, size_t size)
{
    void *file = node_file(&context->spi);
    if (file == NULL)
        return -ENODEV;
    if (Dmod_FileWrite(data, 1, size, file) != size)
    {
        node_close(&context->spi);
        return -EIO;
    }
    return 0;
}

/* ---- The controller ---- */

static int send_command(dmdrvi_context_t context, uint8_t command)
{
    int ret = gpio_set(&context->dc, false);
    return ret != 0 ? ret : spi_write(context, &command, 1);
}

static int send_data(dmdrvi_context_t context, const void *data, size_t size)
{
    int ret = gpio_set(&context->dc, true);
    return ret != 0 ? ret : spi_write(context, data, size);
}

static int send_command_data(dmdrvi_context_t context, uint8_t command, const uint8_t *data, size_t size)
{
    int ret = send_command(context, command);
    return (ret != 0 || size == 0) ? ret : send_data(context, data, size);
}

#define COMMAND(context, command, ...)                                                          \
    do {                                                                                        \
        static const uint8_t data_[] = { __VA_ARGS__ };                                         \
        int r_ = send_command_data((context), (command), data_, sizeof(data_));                 \
        if (r_ != 0)                                                                            \
            return r_;                                                                          \
    } while (0)

/* Waits while the controller works (BUSY low). The vendor gives it a millisecond to raise the line first. */
static int wait_busy(dmdrvi_context_t context)
{
    dmosi_thread_sleep(1);
    for (uint32_t waited = 0; waited < BUSY_TIMEOUT_MS; waited += BUSY_POLL_MS)
    {
        int level = gpio_get(&context->busy);
        if (level < 0)
            return level;
        if (level != 0)
            return 0;
        dmosi_thread_sleep(BUSY_POLL_MS);
    }
    DMOD_LOG_ERROR("UC8253: BUSY did not go high in %u ms\n", (unsigned)BUSY_TIMEOUT_MS);
    return -ETIMEDOUT;
}

/* The controller is reset (the line, when there is one) and its panel setting is made: 0x1F = the waveforms of the OTP, KW, the scan as it is */
static int controller_init(dmdrvi_context_t context)
{
    if (context->reset.path != NULL)
    {
        (void)gpio_set(&context->reset, true);
        dmosi_thread_sleep(10);
        (void)gpio_set(&context->reset, false);
        dmosi_thread_sleep(10);
        (void)gpio_set(&context->reset, true);
        dmosi_thread_sleep(10);
    }
    else
    {
        COMMAND(context, CMD_PSR, 0x1E, 0x0D);                  /* soft reset */
        dmosi_thread_sleep(2);
    }
    COMMAND(context, CMD_PSR, 0x1F, 0x0D);
    return 0;
}

static int set_window(dmdrvi_context_t context, const uc8253_window_t *w)
{
    uint8_t window[7] = {
        (uint8_t)w->x0, (uint8_t)w->x1, (uint8_t)(w->y0 >> 8), (uint8_t)w->y0, (uint8_t)(w->y1 >> 8), (uint8_t)w->y1, 0x01
    };
    return send_command_data(context, CMD_PTL, window, sizeof(window));
}

/* The picture of the clients of [x0, x1] x [y0, y1] into the picture register (CMD_DTM1: old, CMD_DTM2: new) of the controller */
static int write_picture(dmdrvi_context_t context, uint8_t reg, const uc8253_window_t *w)
{
    const dmuc8253_config_t *c = &context->config;
    uc8253_pack_window(c->rotation, c->width, c->height, context->framebuffer, context->stride, w, context->packed);
    int ret = send_command(context, CMD_PTIN);
    if (ret == 0)
        ret = set_window(context, w);
    if (ret == 0)
        ret = send_command(context, reg);
    if (ret != 0)
        return ret;
    ret = send_data(context, context->packed, uc8253_window_bytes(w));
    return ret != 0 ? ret : send_command(context, CMD_PTOUT);
}

/* A command, and the wait until the controller has done it */
static int command_and_wait(dmdrvi_context_t context, uint8_t command)
{
    int ret = send_command(context, command);
    return ret != 0 ? ret : wait_busy(context);
}

/* Turns the screen into the new picture: a full refresh (the whole screen) or a partial one (the window) */
static int refresh(dmdrvi_context_t context, const uc8253_window_t *w, bool full)
{
    int ret;

    if (full)
    {
        COMMAND(context, CMD_CCSET, 0x02);                      /* TSFIX */
        COMMAND(context, CMD_TSSET, 0x5A);                      /* the temperature the fast waveform is for: 90 */
        COMMAND(context, CMD_CDI, 0x97);
    }
    else
    {
        if ((ret = send_command(context, CMD_PTIN)) != 0 || (ret = set_window(context, w)) != 0)
            return ret;
        COMMAND(context, CMD_CCSET, 0x02);
        COMMAND(context, CMD_TSSET, 0x79);                      /* the fast partial waveform: 121 */
        COMMAND(context, CMD_CDI, 0xD7);
    }
    if ((ret = command_and_wait(context, CMD_PON)) != 0 || (ret = command_and_wait(context, CMD_DRF)) != 0)
        return ret;
    if (!full && (ret = send_command(context, CMD_PTOUT)) != 0)
        return ret;
    /* The drive voltages off: a panel left powered fades */
    return command_and_wait(context, CMD_POF);
}

/* Shows the framebuffer from [x0, x1] x [y0, y1] (the picture of the clients); the caller holds the lock. full: the whole screen, slowly. */
static int show(dmdrvi_context_t context, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, bool full)
{
    const dmuc8253_config_t *c = &context->config;
    uc8253_window_t whole = { 0, 0, (uint16_t)(c->width - 1U), (uint16_t)(c->height - 1U) };
    uc8253_window_t w = full ? whole : uc8253_window_of(c->rotation, c->width, c->height, x0, y0, x1, y1);
    int ret;

    if (!context->started)
        return -EAGAIN;
    /* The vendor starts every sequence of writes with the panel setting again (the controller forgets it after a refresh) */
    if ((ret = controller_init(context)) != 0)
        return ret;
    if (full)
    {
        /* Both pictures are the new one: nothing is left of what was on the screen before */
        if ((ret = write_picture(context, CMD_DTM1, &w)) != 0 || (ret = write_picture(context, CMD_DTM2, &w)) != 0 ||
            (ret = refresh(context, &w, true)) != 0)
            return ret;
        context->partial_count = 0;
        return 0;
    }
    if ((ret = write_picture(context, CMD_DTM2, &w)) != 0 || (ret = refresh(context, &w, false)) != 0)
        return ret;
    /* The old picture is what is on the screen now */
    if ((ret = controller_init(context)) != 0 || (ret = write_picture(context, CMD_DTM1, &w)) != 0)
        return ret;
    context->partial_count++;
    return 0;
}

/* ---- The framebuffer ---- */

static void dirty_add(dmdrvi_context_t context, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    area_t *d = &context->dirty;
    if (d->x0 > d->x1)
    {
        d->x0 = x0; d->y0 = y0; d->x1 = x1; d->y1 = y1;
        return;
    }
    d->x0 = x0 < d->x0 ? x0 : d->x0;
    d->y0 = y0 < d->y0 ? y0 : d->y0;
    d->x1 = x1 > d->x1 ? x1 : d->x1;
    d->y1 = y1 > d->y1 ? y1 : d->y1;
}

static void dirty_clear(dmdrvi_context_t context)
{
    context->dirty = (area_t){ 1, 1, 0, 0 };
}

static void fill_rect(dmdrvi_context_t context, const dmdrvi_gfx_fill_rect_t *rect)
{
    if (rect->x >= context->width || rect->y >= context->height || rect->width == 0 || rect->height == 0)
        return;
    uint32_t w = rect->width > context->width - rect->x ? context->width - rect->x : rect->width;
    uint32_t h = rect->height > context->height - rect->y ? context->height - rect->y : rect->height;
    bool white = dmuc8253_color_to_mono(rect->color);

    for (uint32_t y = rect->y; y < (uint32_t)rect->y + h; y++)
        for (uint32_t x = rect->x; x < (uint32_t)rect->x + w; x++)
            uc8253_put_pixel(context->framebuffer, context->stride, x, y, white);
    dirty_add(context, rect->x, rect->y, (uint16_t)(rect->x + w - 1U), (uint16_t)(rect->y + h - 1U));
}

/* PRESENT: the area (NULL: all) goes to the screen. The first one, and every full_refresh_interval-th, is a full refresh. */
static int present(dmdrvi_context_t context, const dmdrvi_gfx_rect_t *area, bool force_full)
{
    const dmuc8253_config_t *c = &context->config;
    uint32_t x0 = 0, y0 = 0, x1 = context->width - 1U, y1 = context->height - 1U;
    if (area != NULL)
    {
        if (area->x >= context->width || area->y >= context->height || area->width == 0 || area->height == 0)
            return 0;
        x0 = area->x;
        y0 = area->y;
        x1 = x0 + area->width - 1U > x1 ? x1 : x0 + area->width - 1U;
        y1 = y0 + area->height - 1U > y1 ? y1 : y0 + area->height - 1U;
    }
    dmosi_mutex_lock(context->lock);
    bool full = force_full || !c->partial ||
                (c->full_refresh_interval != 0U && context->partial_count >= c->full_refresh_interval);
    int ret = show(context, x0, y0, x1, y1, full);
    if (ret == 0)
        dirty_clear(context);
    dmosi_mutex_unlock(context->lock);
    return ret;
}

/* Starts the panel once the required friends are there (whichever order they come in) and shows the framebuffer: the splash screen */
static void try_start(dmdrvi_context_t context)
{
    if (context->started || context->spi.path == NULL || context->dc.path == NULL || context->busy.path == NULL ||
        (context->reset_wired && context->reset.path == NULL))
        return;
    dmosi_mutex_lock(context->lock);
    context->started = true;
    int ret = show(context, 0, 0, context->width - 1U, context->height - 1U, true);
    if (ret == 0)
    {
        dirty_clear(context);
        DMOD_LOG_INFO("UC8253 %ux%u started (rotation %u)\n", context->width, context->height, (unsigned)context->config.rotation * 90U);
    }
    else
    {
        context->started = false;
        DMOD_LOG_ERROR("UC8253: the start failed (%d; SPI / D/C / BUSY nodes: %s / %s / %s)\n", ret, context->spi.path,
                       context->dc.path, context->busy.path);
    }
    dmosi_mutex_unlock(context->lock);
}

/* ---- DMOD lifecycle ---- */

int dmod_init(const Dmod_Config_t *Config)
{
    (void)Config;
    DMOD_LOG_INFO("DMUC8253 module initialized\n");
    return 0;
}

int dmod_deinit(void)
{
    DMOD_LOG_INFO("DMUC8253 module deinitialized\n");
    return 0;
}

/* ---- Splash screen ---- */

/* Environment variable with the path of the splash logo (set by dmod-boot, see DMBOOT_SPLASH_LOGO_PATH there); the splash_logo key of
 * the configuration overrides it. */
#define SPLASH_LOGO_ENV             "SPLASH_LOGO"

/* Path of the splash logo: the splash_logo key, else the SPLASH_LOGO environment variable; NULL: no logo ("none" turns it off) */
static const char *splash_logo_path(dmini_context_t ini)
{
    const char *path = dmini_get_string(ini, NULL, "splash_logo", NULL);
    if (path == NULL)
        path = Dmod_GetEnv(SPLASH_LOGO_ENV);
    return (path == NULL || path[0] == '\0' || strcmp(path, "none") == 0) ? NULL : path;
}

/* Draws the logo in the middle of the cleared framebuffer: the panel shows it as soon as it is started */
static void show_splash(dmdrvi_context_t context, const char *logo_path)
{
    if (logo_path == NULL)
        return;
    dmuc8253_splash_target_t target = {
        context->framebuffer, context->stride, context->width, context->height, context->config.clear_color & 0xFFFFFFU
    };
    int ret = dmuc8253_splash_draw_file(logo_path, &target);
    if (ret == -ENOENT)
    {
        DMOD_LOG_INFO("No splash logo at %s\n", logo_path);
    }
    else if (ret != 0)
    {
        DMOD_LOG_ERROR("Cannot show splash logo %s (%d)\n", logo_path, ret);
    }
}

/* ---- DMDRVI interface ---- */

dmod_dmdrvi_dif_api_declaration(2.0, dmuc8253, dmdrvi_context_t, _create, ( dmini_context_t config, dmdrvi_dev_num_t* dev_num ))
{
    if (config == NULL || dev_num == NULL)
        return NULL;
    dmdrvi_context_t context = Dmod_Malloc(sizeof(struct dmdrvi_context));
    if (context == NULL)
        return NULL;
    memset(context, 0, sizeof(*context));
    context->magic = DMUC8253_CONTEXT_MAGIC;
    dirty_clear(context);

    if (read_config(context, config) != 0)
        goto fail;
    context->lock = dmosi_mutex_create(false);
    context->framebuffer = Dmod_Malloc(context->framebuffer_size);
    context->packed = Dmod_Malloc((size_t)context->config.width / 8U * context->config.height);
    if (context->lock == NULL || context->framebuffer == NULL || context->packed == NULL)
    {
        DMOD_LOG_ERROR("UC8253: out of memory (framebuffer %u bytes)\n", (unsigned)context->framebuffer_size);
        goto fail;
    }
    /* The picture starts as clear_color: the panel never shows the garbage of the heap */
    dmdrvi_gfx_fill_rect_t all = { 0, 0, context->width, context->height, context->config.clear_color };
    fill_rect(context, &all);
    show_splash(context, splash_logo_path(config));

    dev_num->flags = DMDRVI_NUM_MAJOR;
    dev_num->major = (dmdrvi_dev_id_t)context->instance;
    return context;

fail:
    if (context->lock != NULL)
        dmosi_mutex_destroy(context->lock);
    Dmod_Free(context->framebuffer);
    Dmod_Free(context->packed);
    context->magic = 0;
    Dmod_Free(context);
    return NULL;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmuc8253, void, _free, ( dmdrvi_context_t context ))
{
    if (!is_valid_context(context))
        return;
    /* The nodes of the friends are not touched: dmdevfs tears them down before this call, in reverse order of the sections */
    Dmod_Free(context->spi.path);
    Dmod_Free(context->dc.path);
    Dmod_Free(context->busy.path);
    Dmod_Free(context->reset.path);
    dmosi_mutex_destroy(context->lock);
    Dmod_Free(context->framebuffer);
    Dmod_Free(context->packed);
    context->magic = 0;
    Dmod_Free(context);
}

dmod_dmdrvi_dif_api_declaration(2.0, dmuc8253, void, _friend_changed, ( dmdrvi_context_t context, const dmdrvi_friend_info_t* info ))
{
    if (!is_valid_context(context) || info == NULL || info->friend_role == NULL)
        return;
    node_t *node = strcmp(info->friend_role, "spi") == 0 ? &context->spi
                 : strcmp(info->friend_role, "dc") == 0 ? &context->dc
                 : strcmp(info->friend_role, "busy") == 0 ? &context->busy
                 : strcmp(info->friend_role, "reset") == 0 ? &context->reset : NULL;
    if (node == NULL)
        return;                                         /* chip_select is dmspi's */
    node_forget(node);
    if (info->state != dmdrvi_dev_state_ready || info->node_path == NULL)
        return;
    node->path = Dmod_StrDup(info->node_path);
    if (node->path == NULL)
    {
        DMOD_LOG_ERROR("UC8253: out of memory for the path of a friend\n");
        return;
    }
    try_start(context);
}

dmod_dmdrvi_dif_api_declaration(2.0, dmuc8253, void*, _open, ( dmdrvi_context_t context, int flags, const dmdrvi_dev_num_t* dev_num ))
{
    return is_valid_context(context) ? context : NULL;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmuc8253, void, _close, ( dmdrvi_context_t context, void* handle ))
{
}

dmod_dmdrvi_dif_api_declaration(2.0, dmuc8253, dmdrvi_ssize_t, _read, ( dmdrvi_context_t context, void* handle, void* buffer, size_t size, dmdrvi_offset_t offset ))
{
    if (!is_valid_context(context) || (buffer == NULL && size != 0) || offset < 0)
        return -EINVAL;
    if (size == 0 || offset >= (dmdrvi_offset_t)context->framebuffer_size)
        return 0;
    size_t available = context->framebuffer_size - (size_t)offset;
    size_t count = size < available ? size : available;
    memcpy(buffer, context->framebuffer + offset, count);
    return (dmdrvi_ssize_t)count;
}

/* Writes into the framebuffer; what is written reaches the panel at the next PRESENT / flush */
dmod_dmdrvi_dif_api_declaration(2.0, dmuc8253, dmdrvi_ssize_t, _write, ( dmdrvi_context_t context, void* handle, const void* buffer, size_t size, dmdrvi_offset_t offset ))
{
    if (!is_valid_context(context) || (buffer == NULL && size != 0) || offset < 0)
        return -EINVAL;
    if (size == 0)
        return 0;
    if (offset >= (dmdrvi_offset_t)context->framebuffer_size)
        return -ENOSPC;
    size_t available = context->framebuffer_size - (size_t)offset;
    size_t count = size < available ? size : available;
    memcpy(context->framebuffer + offset, buffer, count);
    uint32_t first_line = (uint32_t)offset / context->stride;
    uint32_t last_line = ((uint32_t)offset + (uint32_t)count - 1U) / context->stride;
    dirty_add(context, 0, (uint16_t)first_line, (uint16_t)(context->width - 1U), (uint16_t)last_line);
    return (dmdrvi_ssize_t)count;
}

static bool ioctl_needs_arg(int command)
{
    return command == (int)DMDRVI_IOCTL_GFX_GET_INFO || command == (int)DMDRVI_IOCTL_GFX_GET_FRAMEBUFFER ||
           command == (int)DMDRVI_IOCTL_GFX_FILL_RECT || command == (int)DMDRVI_IOCTL_GFX_SET_DISPLAY_ENABLED ||
           command == (int)DMDRVI_IOCTL_GFX_GET_DISPLAY_ENABLED || command == (int)dmuc8253_ioctl_cmd_get_config;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmuc8253, int, _ioctl, ( dmdrvi_context_t context, void* handle, int command, void* arg ))
{
    if (!is_valid_context(context))
        return -EINVAL;
    if (arg == NULL && ioctl_needs_arg(command))
        return -EINVAL;

    switch (command)
    {
        case DMDRVI_IOCTL_GFX_GET_INFO:
        {
            dmdrvi_gfx_info_t *info = (dmdrvi_gfx_info_t *)arg;
            info->width = context->width;
            info->height = context->height;
            info->pixel_format = DMDRVI_GFX_PIXEL_FORMAT_MONO1;
            info->bytes_per_pixel = 0;
            info->stride = context->stride;
            info->framebuffer_size = context->framebuffer_size;
            info->buffer_count = 1;
            return 0;
        }
        case DMDRVI_IOCTL_GFX_GET_FRAMEBUFFER:
            *(void **)arg = context->framebuffer;
            return 0;
        case DMDRVI_IOCTL_GFX_FILL_RECT:
            fill_rect(context, (const dmdrvi_gfx_fill_rect_t *)arg);
            return 0;
        case DMDRVI_IOCTL_GFX_PRESENT:
            return present(context, (const dmdrvi_gfx_rect_t *)arg, false);
        case dmuc8253_ioctl_cmd_full_refresh:
            return present(context, NULL, true);
        case DMDRVI_IOCTL_GFX_SET_DISPLAY_ENABLED:
            context->display_enabled = *(const bool *)arg;
            return 0;                                   /* the screen keeps its picture without power: nothing to switch */
        case DMDRVI_IOCTL_GFX_GET_DISPLAY_ENABLED:
            *(bool *)arg = context->display_enabled;
            return 0;
        case DMDRVI_IOCTL_GFX_SET_BACKLIGHT:
        case DMDRVI_IOCTL_GFX_GET_BACKLIGHT:
        case DMDRVI_IOCTL_GFX_SWAP_BUFFERS:
        case DMDRVI_IOCTL_GFX_WAIT_VSYNC:
            return -ENOTSUP;                            /* no backlight, one buffer, no scan-out to wait for */
        case dmuc8253_ioctl_cmd_get_config:
            *(dmuc8253_config_t *)arg = context->config;
            return 0;
        default:
            return -ENOTTY;
    }
}

/* flush(): what was written through write() / FILL_RECT since the last PRESENT goes to the screen */
dmod_dmdrvi_dif_api_declaration(2.0, dmuc8253, int, _flush, ( dmdrvi_context_t context, void* handle ))
{
    if (!is_valid_context(context))
        return -EINVAL;
    if (context->dirty.x0 > context->dirty.x1)
        return 0;
    dmdrvi_gfx_rect_t area = { context->dirty.x0, context->dirty.y0, (uint16_t)(context->dirty.x1 - context->dirty.x0 + 1U),
                               (uint16_t)(context->dirty.y1 - context->dirty.y0 + 1U) };
    return present(context, &area, false);
}

dmod_dmdrvi_dif_api_declaration(2.0, dmuc8253, int, _stat, ( dmdrvi_context_t context, const char* path, dmdrvi_stat_t* stat ))
{
    if (!is_valid_context(context) || stat == NULL)
        return -EINVAL;
    stat->size = (dmdrvi_size_t)context->framebuffer_size;
    stat->mode = 0666;
    return 0;
}
