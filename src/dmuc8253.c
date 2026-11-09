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
 * refresh turns the screen from the first into the second. The driver keeps a framebuffer (MONO1: 1 bit per pixel, set = white; or RGB565
 * for clients that draw in colors - then it makes the 1-bit picture of what is presented, see pixel_format) that
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
#define PSR_UD                      0x08    /* the lines scanned up (G0 first) */
#define PSR_SHL                     0x04    /* the columns shifted right (S0 first) */
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
    uint32_t            stride;             /**< Bytes of a line of the plane (MONO1) */
    uint32_t            fb_stride;          /**< Bytes of a line of the framebuffer of the clients */
    uint32_t            framebuffer_size;
    uint8_t            *framebuffer;        /**< What the clients draw into (config.pixel_format) */
    uint8_t            *plane;              /**< The 1-bit picture the panel is sent: the framebuffer itself when that is MONO1 */
    uint8_t            *packed;             /**< The window of the panel on its way to the controller */
    dmosi_mutex_t       lock;
    node_t              spi;
    node_t              dc;
    node_t              busy;
    node_t              reset;
    bool                reset_wired;        /**< The configuration says there is a reset line to wait for */
    bool                started;            /**< The panel was initialised and shows the framebuffer */
    bool                awake;              /**< The controller has been reset once (by the line, when there is one) */
    bool                display_enabled;
    uint32_t            partial_count;      /**< Partial refreshes since the last full one */
    area_t              dirty;              /**< Written through write() / FILL_RECT, not presented yet */
    /* settle_ms: the refresh is made by a thread of the driver (see present()) */
    dmosi_thread_t      worker;
    dmosi_semaphore_t   wake;               /**< Posted when an area is pending */
    dmosi_mutex_t       pending_lock;       /**< Only the pending area (the panel's lock is held for a whole refresh) */
    dmosi_mutex_t       plane_lock;         /**< The plane: the thread packs from it while PRESENT makes the 1-bit picture of a new frame */
    area_t              pending;            /**< Presented, not refreshed yet; x0 > x1: nothing */
    uint32_t            due;                /**< Tick of the refresh of the pending area */
    bool                stopping;
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
    if (config->mirror > dmuc8253_mirror_xy || config->pixel_format > dmuc8253_pixel_format_rgb565 || config->threshold == 0 ||
        config->settle_ms > 2000U || (config->settle_ms != 0U && config->pixel_format != dmuc8253_pixel_format_rgb565))
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
    const char *mirror = dmini_get_string(ini, NULL, "mirror", "none");

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
    if (strcmp(mirror, "none") == 0)
        c->mirror = dmuc8253_mirror_none;
    else if (strcmp(mirror, "x") == 0)
        c->mirror = dmuc8253_mirror_x;
    else if (strcmp(mirror, "y") == 0)
        c->mirror = dmuc8253_mirror_y;
    else if (strcmp(mirror, "xy") == 0)
        c->mirror = dmuc8253_mirror_xy;
    else
    {
        DMOD_LOG_ERROR("Invalid mirror '%s' (none, x, y or xy)\n", mirror);
        return -EINVAL;
    }
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
    const char *format = dmini_get_string(ini, NULL, "pixel_format", "mono1");
    int threshold = dmini_get_int(ini, NULL, "threshold", 128);
    if (strcmp(format, "mono1") == 0)
        c->pixel_format = dmuc8253_pixel_format_mono1;
    else if (strcmp(format, "rgb565") == 0)
        c->pixel_format = dmuc8253_pixel_format_rgb565;
    else
    {
        DMOD_LOG_ERROR("Invalid pixel_format '%s' (mono1 or rgb565)\n", format);
        return -EINVAL;
    }
    if (threshold < 1 || threshold > 255)
    {
        DMOD_LOG_ERROR("Invalid threshold %d (1..255)\n", threshold);
        return -EINVAL;
    }
    c->threshold = (uint8_t)threshold;
    int settle = dmini_get_int(ini, NULL, "settle_ms", 0);
    if (settle < 0 || settle > 2000)
    {
        DMOD_LOG_ERROR("Invalid settle_ms %d (0..2000)\n", settle);
        return -EINVAL;
    }
    c->settle_ms = (uint16_t)settle;
    if (c->settle_ms != 0U && c->pixel_format != dmuc8253_pixel_format_rgb565)
    {
        /* The 1-bit picture of a frame is made when it is presented: a client of a MONO1 framebuffer draws into the plane itself */
        DMOD_LOG_ERROR("settle_ms needs pixel_format=rgb565\n");
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
    context->fb_stride = (c->pixel_format == dmuc8253_pixel_format_rgb565) ? (uint32_t)context->width * 2U : context->stride;
    context->framebuffer_size = context->fb_stride * context->height;
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

/*
 * Waits until the controller has done a command that keeps it busy (power on/off, a refresh): BUSY goes low when it starts and high
 * when it is done. The start is waited for first: the controller lowers the line some time after the command. A command that is done
 * before the line is seen low (or takes no time) ends the first wait after BUSY_START_MS. Returns the milliseconds waited (about), or
 * a negative errno.
 */
#define BUSY_START_MS               20U

static int wait_busy(dmdrvi_context_t context)
{
    int level = 1;
    uint32_t start = 0;
    for (; start < BUSY_START_MS && level != 0; start++)
    {
        if ((level = gpio_get(&context->busy)) < 0)
            return level;
        if (level != 0)
            dmosi_thread_sleep(1);
    }
    for (uint32_t waited = 0; waited < BUSY_TIMEOUT_MS; waited += BUSY_POLL_MS)
    {
        if ((level = gpio_get(&context->busy)) < 0)
            return level;
        if (level != 0)
            return (int)(start + waited);
        dmosi_thread_sleep(BUSY_POLL_MS);
    }
    DMOD_LOG_ERROR("UC8253: BUSY did not go high in %u ms\n", (unsigned)BUSY_TIMEOUT_MS);
    return -ETIMEDOUT;
}

/* A soft reset and the panel setting, as the vendor's library starts every sequence of writes (the pictures in the RAM of the controller
 * stay: a partial refresh needs the old one). The first time the controller is also reset by the line, when there is one: it may be asleep
 * (the firmware that was there before can have put it into deep sleep, only the line wakes it, no command does). */
static int controller_init(dmdrvi_context_t context)
{
    if (!context->awake && context->reset.path != NULL)
    {
        (void)gpio_set(&context->reset, true);
        dmosi_thread_sleep(10);
        (void)gpio_set(&context->reset, false);
        dmosi_thread_sleep(10);
        (void)gpio_set(&context->reset, true);
        dmosi_thread_sleep(10);
    }
    COMMAND(context, CMD_PSR, 0x1E, 0x0D);                      /* soft reset */
    dmosi_thread_sleep(2);
    context->awake = true;
    /* 0x1F: the waveforms of the OTP, black and white (KW), the lines scanned up (UD), the columns shifted right (SHL), booster on, no reset;
       a mirror reverses a scan direction */
    uint8_t psr[2] = { 0x1F, 0x0D };
    if (context->config.mirror == dmuc8253_mirror_x || context->config.mirror == dmuc8253_mirror_xy)
        psr[0] &= (uint8_t)~PSR_SHL;
    if (context->config.mirror == dmuc8253_mirror_y || context->config.mirror == dmuc8253_mirror_xy)
        psr[0] &= (uint8_t)~PSR_UD;
    return send_command_data(context, CMD_PSR, psr, sizeof(psr));
}

static void plane_lock(dmdrvi_context_t context)
{
    if (context->plane_lock != NULL)
        dmosi_mutex_lock(context->plane_lock);
}

static void plane_unlock(dmdrvi_context_t context)
{
    if (context->plane_lock != NULL)
        dmosi_mutex_unlock(context->plane_lock);
}

static int set_window(dmdrvi_context_t context, const uc8253_window_t *w)
{
    uint8_t window[7] = {
        (uint8_t)w->x0, (uint8_t)w->x1, (uint8_t)(w->y0 >> 8), (uint8_t)w->y0, (uint8_t)(w->y1 >> 8), (uint8_t)w->y1, 0x01
    };
    return send_command_data(context, CMD_PTL, window, sizeof(window));
}

/* The window of the plane packed for the controller (into context->packed). The plane may change while the panel is refreshed (a frame
 * presented meanwhile), so this is done once for a refresh: what is sent as the new picture is what the panel will show, and the same bytes are
 * the old picture afterwards - the controller compares the two in the next partial refresh. */
static void pack_picture(dmdrvi_context_t context, const uc8253_window_t *w)
{
    const dmuc8253_config_t *c = &context->config;
    plane_lock(context);
    uc8253_pack_window(c->rotation, c->width, c->height, context->plane, context->stride, w, context->packed);
    plane_unlock(context);
}

/* The packed window into the picture register (CMD_DTM1: old, CMD_DTM2: new) of the controller. The whole screen goes without a partial
 * window (as the vendor's clearScreen does), a window of it between PTIN and PTOUT. */
static int send_picture(dmdrvi_context_t context, uint8_t reg, const uc8253_window_t *w)
{
    bool whole = w->x0 == 0U && w->y0 == 0U && w->x1 == context->config.width - 1U && w->y1 == context->config.height - 1U;
    int ret = whole ? 0 : send_command(context, CMD_PTIN);
    if (ret == 0 && !whole)
        ret = set_window(context, w);
    if (ret == 0)
        ret = send_command(context, reg);
    if (ret != 0)
        return ret;
    ret = send_data(context, context->packed, uc8253_window_bytes(w));
    return (ret != 0 || whole) ? ret : send_command(context, CMD_PTOUT);
}

/* A command, and the wait until the controller has done it */
static int command_and_wait(dmdrvi_context_t context, uint8_t command)
{
    int ret = send_command(context, command);
    if (ret == 0)
        ret = wait_busy(context);
    return ret < 0 ? ret : 0;
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
        pack_picture(context, &w);
        ret = send_picture(context, CMD_DTM1, &w);
        if (ret == 0)
            ret = send_picture(context, CMD_DTM2, &w);
        if (ret == 0)
            ret = refresh(context, &w, true);
        if (ret != 0)
            return ret;
        context->partial_count = 0;
        return 0;
    }
    pack_picture(context, &w);
    if ((ret = send_picture(context, CMD_DTM2, &w)) != 0 || (ret = refresh(context, &w, false)) != 0)
        return ret;
    /* The old picture is what is on the screen now: the bytes that were sent as the new one (not the plane: it may be a later frame) */
    if ((ret = controller_init(context)) != 0 || (ret = send_picture(context, CMD_DTM1, &w)) != 0)
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

static bool is_rgb565(const dmdrvi_context_t context)
{
    return context->config.pixel_format == dmuc8253_pixel_format_rgb565;
}

static void fill_rect(dmdrvi_context_t context, const dmdrvi_gfx_fill_rect_t *rect)
{
    if (rect->x >= context->width || rect->y >= context->height || rect->width == 0 || rect->height == 0)
        return;
    uint32_t w = rect->width > context->width - rect->x ? context->width - rect->x : rect->width;
    uint32_t h = rect->height > context->height - rect->y ? context->height - rect->y : rect->height;
    bool white = uc8253_color_is_white_at(rect->color, context->config.threshold);
    uint32_t r = (rect->color >> 16) & 0xFFU, g = (rect->color >> 8) & 0xFFU, b = rect->color & 0xFFU;
    uint16_t pixel = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));

    for (uint32_t y = rect->y; y < (uint32_t)rect->y + h; y++)
    {
        for (uint32_t x = rect->x; x < (uint32_t)rect->x + w; x++)
        {
            if (is_rgb565(context))
            {
                context->framebuffer[y * context->fb_stride + x * 2U] = (uint8_t)pixel;
                context->framebuffer[y * context->fb_stride + x * 2U + 1U] = (uint8_t)(pixel >> 8);
            }
            else
            {
                uc8253_put_pixel(context->framebuffer, context->stride, x, y, white);
            }
        }
    }
    dirty_add(context, rect->x, rect->y, (uint16_t)(rect->x + w - 1U), (uint16_t)(rect->y + h - 1U));
}

/* The area (NULL: all) goes to the screen now. The first one, and every full_refresh_interval-th, is a full refresh. */
static int present_now(dmdrvi_context_t context, const dmdrvi_gfx_rect_t *area, bool force_full, bool convert)
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
    if (convert && is_rgb565(context))                          /* the 1-bit picture of what the clients drew */
        uc8253_rgb565_to_plane(context->framebuffer, context->fb_stride, context->plane, context->stride, x0, y0, x1, y1, c->threshold);
    bool full = force_full || !c->partial ||
                (c->full_refresh_interval != 0U && context->partial_count >= c->full_refresh_interval);
    int ret = show(context, x0, y0, x1, y1, full);
    if (ret == 0)
        dirty_clear(context);
    dmosi_mutex_unlock(context->lock);
    return ret;
}

static void pending_add(dmdrvi_context_t context, const dmdrvi_gfx_rect_t *area)
{
    area_t *p = &context->pending;
    uint16_t x0 = 0, y0 = 0, x1 = (uint16_t)(context->width - 1U), y1 = (uint16_t)(context->height - 1U);
    if (area != NULL)
    {
        if (area->x >= context->width || area->y >= context->height || area->width == 0 || area->height == 0)
            return;
        x0 = area->x;
        y0 = area->y;
        x1 = (uint16_t)(x0 + area->width - 1U > x1 ? x1 : x0 + area->width - 1U);
        y1 = (uint16_t)(y0 + area->height - 1U > y1 ? y1 : y0 + area->height - 1U);
    }
    if (p->x0 > p->x1)
    {
        *p = (area_t){ x0, y0, x1, y1 };
        context->due = dmosi_get_tick_count() + context->config.settle_ms;      /* the first one starts the wait: it is not prolonged */
        dmosi_semaphore_post(context->wake, 1);
        return;
    }
    p->x0 = x0 < p->x0 ? x0 : p->x0;
    p->y0 = y0 < p->y0 ? y0 : p->y0;
    p->x1 = x1 > p->x1 ? x1 : p->x1;
    p->y1 = y1 > p->y1 ? y1 : p->y1;
}

/*
 * The thread of settle_ms: waits until the pending area is due and refreshes it - the whole of what was presented meanwhile, from the
 * framebuffer as it is then. A button shown pressed and the page the touch opens a moment later are one refresh, not two (0.7 s each);
 * and the client is not held up by the refresh. What is drawn into the framebuffer during the refresh is presented again, so it is shown
 * by the next one.
 */
static void worker_main(void *arg)
{
    dmdrvi_context_t context = arg;
    while (!context->stopping)
    {
        (void)dmosi_semaphore_wait(context->wake, 1, -1);
        for (;;)
        {
            int32_t left = (int32_t)(context->due - dmosi_get_tick_count());
            if (left <= 0 || context->stopping)
                break;
            dmosi_thread_sleep((uint32_t)(left < 20 ? left : 20));
        }
        if (context->stopping)
            break;
        dmosi_mutex_lock(context->pending_lock);
        area_t a = context->pending;
        context->pending = (area_t){ 1, 1, 0, 0 };
        dmosi_mutex_unlock(context->pending_lock);
        if (a.x0 > a.x1)
            continue;
        dmdrvi_gfx_rect_t rect = { a.x0, a.y0, (uint16_t)(a.x1 - a.x0 + 1U), (uint16_t)(a.y1 - a.y0 + 1U) };
        (void)present_now(context, &rect, false, false);
    }
}

/* PRESENT: refreshed now, or - with settle_ms - by the thread of the driver after a moment */
static int present(dmdrvi_context_t context, const dmdrvi_gfx_rect_t *area, bool force_full)
{
    if (context->worker == NULL || force_full)
        return present_now(context, area, force_full, true);
    /* The frame is finished now (the client has presented it): its 1-bit picture is made now, not when the thread gets to it - the client
     * goes on drawing the next frame into the framebuffer, and a refresh must not catch it half drawn */
    {
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
        plane_lock(context);
        uc8253_rgb565_to_plane(context->framebuffer, context->fb_stride, context->plane, context->stride, x0, y0, x1, y1,
                               context->config.threshold);
        plane_unlock(context);
    }
    dmosi_mutex_lock(context->pending_lock);
    pending_add(context, area);
    dmosi_mutex_unlock(context->pending_lock);
    dirty_clear(context);
    return 0;
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
        context->awake = false;
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
        context->plane, context->stride, context->width, context->height, context->config.clear_color & 0xFFFFFFU
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

/* The framebuffer of the clients shows what the plane does (the splash screen): a client that draws only a part of it leaves the rest */
static void sync_framebuffer(dmdrvi_context_t context)
{
    if (is_rgb565(context))
        uc8253_plane_to_rgb565(context->plane, context->stride, context->framebuffer, context->fb_stride, context->width, context->height);
}

/* ---- DMDRVI interface ---- */

static void worker_stop(dmdrvi_context_t context)
{
    if (context->worker != NULL)
    {
        context->stopping = true;
        dmosi_semaphore_post(context->wake, 1);
        (void)dmosi_thread_join(context->worker);
        dmosi_thread_destroy(context->worker);
        context->worker = NULL;
    }
    if (context->wake != NULL)
        dmosi_semaphore_destroy(context->wake);
    if (context->pending_lock != NULL)
        dmosi_mutex_destroy(context->pending_lock);
    if (context->plane_lock != NULL)
        dmosi_mutex_destroy(context->plane_lock);
    context->wake = NULL;
    context->pending_lock = NULL;
    context->plane_lock = NULL;
}

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
    context->plane = is_rgb565(context) ? Dmod_Malloc((size_t)context->stride * context->height) : context->framebuffer;
    context->packed = Dmod_Malloc((size_t)context->config.width / 8U * context->config.height);
    if (context->lock == NULL || context->framebuffer == NULL || context->plane == NULL || context->packed == NULL)
    {
        DMOD_LOG_ERROR("UC8253: out of memory (framebuffer %u bytes)\n", (unsigned)context->framebuffer_size);
        goto fail;
    }
    /* The picture starts as clear_color: the panel never shows the garbage of the heap */
    dmdrvi_gfx_fill_rect_t all = { 0, 0, context->width, context->height, context->config.clear_color };
    fill_rect(context, &all);
    if (is_rgb565(context))                                     /* (the plane is what the panel is sent: the same clear color) */
        uc8253_rgb565_to_plane(context->framebuffer, context->fb_stride, context->plane, context->stride, 0, 0, context->width - 1U,
                               context->height - 1U, context->config.threshold);
    show_splash(context, splash_logo_path(config));
    sync_framebuffer(context);
    context->pending = (area_t){ 1, 1, 0, 0 };
    if (context->config.settle_ms != 0U)
    {
        context->pending_lock = dmosi_mutex_create(false);
        context->plane_lock = dmosi_mutex_create(false);
        context->wake = dmosi_semaphore_create(0, 1);
        if (context->pending_lock != NULL && context->plane_lock != NULL && context->wake != NULL)
            context->worker = dmosi_thread_create(worker_main, context, 2, 4096, "uc8253", dmosi_process_current());
        if (context->worker == NULL)
            DMOD_LOG_ERROR("UC8253: no thread for settle_ms - the panel is refreshed by PRESENT itself\n");
    }

    dev_num->flags = DMDRVI_NUM_MAJOR;
    dev_num->major = (dmdrvi_dev_id_t)context->instance;
    return context;

fail:
    worker_stop(context);
    if (context->lock != NULL)
        dmosi_mutex_destroy(context->lock);
    if (context->plane != context->framebuffer)
        Dmod_Free(context->plane);
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
    worker_stop(context);
    /* The nodes of the friends are not touched: dmdevfs tears them down before this call, in reverse order of the sections */
    Dmod_Free(context->spi.path);
    Dmod_Free(context->dc.path);
    Dmod_Free(context->busy.path);
    Dmod_Free(context->reset.path);
    dmosi_mutex_destroy(context->lock);
    if (context->plane != context->framebuffer)
        Dmod_Free(context->plane);
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
    uint32_t first_line = (uint32_t)offset / context->fb_stride;
    uint32_t last_line = ((uint32_t)offset + (uint32_t)count - 1U) / context->fb_stride;
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
            info->pixel_format = is_rgb565(context) ? DMDRVI_GFX_PIXEL_FORMAT_RGB565 : DMDRVI_GFX_PIXEL_FORMAT_MONO1;
            info->bytes_per_pixel = is_rgb565(context) ? 2 : 0;
            info->stride = context->fb_stride;
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
