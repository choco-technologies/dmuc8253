#ifndef DMUC8253_PANEL_H
#define DMUC8253_PANEL_H

/*
 * The geometry of the panel, without hardware: how the picture of the clients (the framebuffer, MONO1, as they see the screen) lies on
 * the panel (portrait, 8 pixels in a byte of the controller's RAM, the leftmost in the top bit, set = white - the same as the
 * framebuffer), and the packing of an area of it for the controller. Header-only so that the tests build against it as it is.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "dmuc8253_types.h"

#define UC8253_PANEL_MAX_WIDTH      240U
#define UC8253_PANEL_MAX_HEIGHT     320U

/** An area of the panel in pixels, both corners inclusive; x0 is a multiple of 8, x1 + 1 too (the controller's RAM is in bytes) */
typedef struct
{
    uint16_t x0, y0, x1, y1;
} uc8253_window_t;

static inline bool uc8253_rotation_swaps(dmuc8253_rotation_t rotation)
{
    return rotation == dmuc8253_rotation_90 || rotation == dmuc8253_rotation_270;
}

/** Where the pixel (x, y) of the picture of the clients lies on a panel of pw x ph pixels */
static inline void uc8253_to_panel(dmuc8253_rotation_t rotation, uint32_t pw, uint32_t ph, uint32_t x, uint32_t y,
                                   uint32_t *px, uint32_t *py)
{
    switch (rotation)
    {
        case dmuc8253_rotation_90:  *px = pw - 1U - y; *py = x;            break;
        case dmuc8253_rotation_180: *px = pw - 1U - x; *py = ph - 1U - y;  break;
        case dmuc8253_rotation_270: *px = y;           *py = ph - 1U - x;  break;
        default:                    *px = x;           *py = y;            break;
    }
}

/** The pixel of the picture that lies on the panel pixel (px, py): the inverse of uc8253_to_panel */
static inline void uc8253_from_panel(dmuc8253_rotation_t rotation, uint32_t pw, uint32_t ph, uint32_t px, uint32_t py,
                                     uint32_t *x, uint32_t *y)
{
    switch (rotation)
    {
        case dmuc8253_rotation_90:  *x = py;           *y = pw - 1U - px;  break;
        case dmuc8253_rotation_180: *x = pw - 1U - px; *y = ph - 1U - py;  break;
        case dmuc8253_rotation_270: *x = ph - 1U - py; *y = px;            break;
        default:                    *x = px;           *y = py;            break;
    }
}

/** The area of the panel the rectangle [x0, x1] x [y0, y1] of the picture lies on, widened to whole bytes of the RAM */
static inline uc8253_window_t uc8253_window_of(dmuc8253_rotation_t rotation, uint32_t pw, uint32_t ph,
                                               uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    uint32_t ax, ay, bx, by;
    uc8253_to_panel(rotation, pw, ph, x0, y0, &ax, &ay);
    uc8253_to_panel(rotation, pw, ph, x1, y1, &bx, &by);
    uc8253_window_t w;
    w.x0 = (uint16_t)((ax < bx ? ax : bx) & ~7U);
    w.x1 = (uint16_t)((ax > bx ? ax : bx) | 7U);
    w.y0 = (uint16_t)(ay < by ? ay : by);
    w.y1 = (uint16_t)(ay > by ? ay : by);
    return w;
}

static inline uint32_t uc8253_window_row_bytes(const uc8253_window_t *w)
{
    return ((uint32_t)w->x1 - w->x0 + 1U) / 8U;
}

static inline uint32_t uc8253_window_bytes(const uc8253_window_t *w)
{
    return uc8253_window_row_bytes(w) * ((uint32_t)w->y1 - w->y0 + 1U);
}

static inline bool uc8253_get_bit(const uint8_t *fb, uint32_t stride, uint32_t x, uint32_t y)
{
    return ((fb[y * stride + x / 8U] >> (7U - x % 8U)) & 1U) != 0U;
}

/**
 * Packs a window of the panel from the picture: the bytes the controller's RAM takes, line after line (uc8253_window_row_bytes
 * each). The window's x0 and x1 are on bytes, so the rotation 0 is a copy of whole bytes of the lines.
 */
static inline void uc8253_pack_window(dmuc8253_rotation_t rotation, uint32_t pw, uint32_t ph, const uint8_t *fb, uint32_t stride,
                                      const uc8253_window_t *w, uint8_t *out)
{
    uint32_t row_bytes = uc8253_window_row_bytes(w);

    for (uint32_t py = w->y0; py <= w->y1; py++, out += row_bytes)
    {
        if (rotation == dmuc8253_rotation_0)
        {
            memcpy(out, fb + py * stride + w->x0 / 8U, row_bytes);
            continue;
        }
        memset(out, 0, row_bytes);
        for (uint32_t px = w->x0; px <= w->x1; px++)
        {
            uint32_t x, y;
            uc8253_from_panel(rotation, pw, ph, px, py, &x, &y);
            if (uc8253_get_bit(fb, stride, x, y))
                out[(px - w->x0) / 8U] |= (uint8_t)(0x80U >> (px % 8U));
        }
    }
}

/** The luminance (0..255) of a color, with the weights of the eye */
static inline uint32_t uc8253_luminance(uint32_t r, uint32_t g, uint32_t b)
{
    return (r * 299U + g * 587U + b * 114U) / 1000U;
}

/** The color 0xAARRGGBB as a pixel of the framebuffer: white (true) when its luminance is at least threshold */
static inline bool uc8253_color_is_white_at(uint32_t argb, uint32_t threshold)
{
    return uc8253_luminance((argb >> 16) & 0xFFU, (argb >> 8) & 0xFFU, argb & 0xFFU) >= threshold;
}

/** The same with the middle of the range as the threshold */
static inline bool uc8253_color_is_white(uint32_t argb)
{
    return uc8253_color_is_white_at(argb, 128U);
}

/** A pixel of RGB565 (the channels stretched to 8 bits, so that white is 255) */
static inline bool uc8253_rgb565_is_white(uint16_t p, uint32_t threshold)
{
    uint32_t r = ((uint32_t)(p >> 11) & 31U) * 255U / 31U, g = ((uint32_t)(p >> 5) & 63U) * 255U / 63U, b = ((uint32_t)p & 31U) * 255U / 31U;
    return uc8253_luminance(r, g, b) >= threshold;
}

static inline void uc8253_put_pixel(uint8_t *fb, uint32_t stride, uint32_t x, uint32_t y, bool white)
{
    uint8_t mask = (uint8_t)(0x80U >> (x % 8U));
    uint8_t *byte = &fb[y * stride + x / 8U];
    *byte = white ? (uint8_t)(*byte | mask) : (uint8_t)(*byte & ~mask);
}

/** The area [x0, x1] x [y0, y1] of an RGB565 framebuffer as pixels of a MONO1 plane (stride in bytes of each) */
static inline void uc8253_rgb565_to_plane(const uint8_t *fb, uint32_t fb_stride, uint8_t *plane, uint32_t plane_stride,
                                          uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t threshold)
{
    for (uint32_t y = y0; y <= y1; y++)
    {
        const uint8_t *line = fb + y * fb_stride;
        for (uint32_t x = x0; x <= x1; x++)
        {
            uint16_t p = (uint16_t)(line[x * 2U] | (line[x * 2U + 1U] << 8));
            uc8253_put_pixel(plane, plane_stride, x, y, uc8253_rgb565_is_white(p, threshold));
        }
    }
}

/** The whole MONO1 plane of w x h pixels as black and white pixels of an RGB565 framebuffer */
static inline void uc8253_plane_to_rgb565(const uint8_t *plane, uint32_t plane_stride, uint8_t *fb, uint32_t fb_stride, uint32_t w, uint32_t h)
{
    for (uint32_t y = 0; y < h; y++)
    {
        for (uint32_t x = 0; x < w; x++)
        {
            uint16_t p = uc8253_get_bit(plane, plane_stride, x, y) ? 0xFFFFU : 0x0000U;
            fb[y * fb_stride + x * 2U] = (uint8_t)p;
            fb[y * fb_stride + x * 2U + 1U] = (uint8_t)(p >> 8);
        }
    }
}

#endif /* DMUC8253_PANEL_H */
