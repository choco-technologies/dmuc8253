#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmuc8253.h"
#include "../src/dmuc8253_panel.h"
#include <string.h>

static dmuc8253_config_t panel(void)
{
    dmuc8253_config_t c = { .width = 240, .height = 320, .rotation = dmuc8253_rotation_0, .threshold = 128 };
    return c;
}

DMOD_TEST_STEP(validate_accepts_the_panel_of_the_t_deck_pro)
{
    dmuc8253_config_t c = panel();
    DMOD_TEST_EXPECT_TRUE(dmuc8253_validate_config(&c));
    c.rotation = dmuc8253_rotation_270;
    DMOD_TEST_EXPECT_TRUE(dmuc8253_validate_config(&c));
}

DMOD_TEST_STEP(validate_rejects_bad_configurations)
{
    dmuc8253_config_t c = panel();
    DMOD_TEST_EXPECT_FALSE(dmuc8253_validate_config(NULL));
    c.width = 0;
    DMOD_TEST_EXPECT_FALSE(dmuc8253_validate_config(&c));
    c = panel();
    c.width = 244;                          /* wider than the RAM of the controller */
    DMOD_TEST_EXPECT_FALSE(dmuc8253_validate_config(&c));
    c = panel();
    c.width = 100;                          /* the RAM is in bytes of 8 pixels */
    DMOD_TEST_EXPECT_FALSE(dmuc8253_validate_config(&c));
    c = panel();
    c.height = 321;
    DMOD_TEST_EXPECT_FALSE(dmuc8253_validate_config(&c));
    c = panel();
    c.rotation = (dmuc8253_rotation_t)4;
    DMOD_TEST_EXPECT_FALSE(dmuc8253_validate_config(&c));
    c = panel();
    c.mirror = (dmuc8253_mirror_t)4;
    DMOD_TEST_EXPECT_FALSE(dmuc8253_validate_config(&c));
    c.mirror = dmuc8253_mirror_xy;
    DMOD_TEST_EXPECT_TRUE(dmuc8253_validate_config(&c));
}

DMOD_TEST_STEP(color_to_mono)
{
    DMOD_TEST_EXPECT_TRUE(dmuc8253_color_to_mono(0xFFFFFFFFU));
    DMOD_TEST_EXPECT_FALSE(dmuc8253_color_to_mono(0xFF000000U));
    DMOD_TEST_EXPECT_TRUE(dmuc8253_color_to_mono(0xFF808080U));         /* 128: white */
    DMOD_TEST_EXPECT_FALSE(dmuc8253_color_to_mono(0xFF7F7F7FU));
    DMOD_TEST_EXPECT_TRUE(dmuc8253_color_to_mono(0xFF00FF00U));         /* green is light */
    DMOD_TEST_EXPECT_FALSE(dmuc8253_color_to_mono(0xFF0000FFU));        /* blue is dark */
    DMOD_TEST_EXPECT_TRUE(dmuc8253_color_to_mono(0x00FFFFFFU));         /* the alpha is ignored */
}

/* ---- Geometry ---- */

DMOD_TEST_STEP(a_pixel_goes_there_and_back)
{
    for (int r = 0; r < 4; r++)
    {
        dmuc8253_rotation_t rotation = (dmuc8253_rotation_t)r;
        uint32_t w = uc8253_rotation_swaps(rotation) ? 320U : 240U;
        uint32_t h = uc8253_rotation_swaps(rotation) ? 240U : 320U;
        for (uint32_t y = 0; y < h; y += 7)
        {
            for (uint32_t x = 0; x < w; x += 5)
            {
                uint32_t px, py, bx, by;
                uc8253_to_panel(rotation, 240, 320, x, y, &px, &py);
                DMOD_TEST_EXPECT_TRUE(px < 240U && py < 320U);
                uc8253_from_panel(rotation, 240, 320, px, py, &bx, &by);
                DMOD_TEST_EXPECT_TRUE(bx == x && by == y);
            }
        }
    }
}

DMOD_TEST_STEP(the_corners_of_the_picture_turn_clockwise)
{
    uint32_t px, py;
    /* 90: the top left corner of the picture is the top right of the panel */
    uc8253_to_panel(dmuc8253_rotation_90, 240, 320, 0, 0, &px, &py);
    DMOD_TEST_EXPECT_TRUE(px == 239U && py == 0U);
    /* the picture is 320 wide: its top right corner is the bottom right of the panel */
    uc8253_to_panel(dmuc8253_rotation_90, 240, 320, 319, 0, &px, &py);
    DMOD_TEST_EXPECT_TRUE(px == 239U && py == 319U);
    /* 270: the top left corner is the bottom left of the panel */
    uc8253_to_panel(dmuc8253_rotation_270, 240, 320, 0, 0, &px, &py);
    DMOD_TEST_EXPECT_TRUE(px == 0U && py == 319U);
    uc8253_to_panel(dmuc8253_rotation_180, 240, 320, 0, 0, &px, &py);
    DMOD_TEST_EXPECT_TRUE(px == 239U && py == 319U);
}

DMOD_TEST_STEP(a_window_is_whole_bytes_of_the_ram)
{
    /* a 10x4 area at (13, 5) of a picture in rotation 0: columns 8..31 (bytes 1..3), lines 5..8 */
    uc8253_window_t w = uc8253_window_of(dmuc8253_rotation_0, 240, 320, 13, 5, 22, 8);
    DMOD_TEST_EXPECT_TRUE(w.x0 == 8U && w.x1 == 23U && w.y0 == 5U && w.y1 == 8U);
    DMOD_TEST_EXPECT_EQ(uc8253_window_row_bytes(&w), 2);
    DMOD_TEST_EXPECT_EQ(uc8253_window_bytes(&w), 8);
    /* the same area in rotation 90 lies at x 240-1-8..240-1-5 of the panel, lines 13..22 */
    w = uc8253_window_of(dmuc8253_rotation_90, 240, 320, 13, 5, 22, 8);
    DMOD_TEST_EXPECT_TRUE(w.x0 == 224U && w.x1 == 239U && w.y0 == 13U && w.y1 == 22U);
    /* the whole picture of a landscape node is the whole panel */
    w = uc8253_window_of(dmuc8253_rotation_90, 240, 320, 0, 0, 319, 239);
    DMOD_TEST_EXPECT_TRUE(w.x0 == 0U && w.x1 == 239U && w.y0 == 0U && w.y1 == 319U);
}

/* A 16x16 picture, only the pixel (x, y) black; on a 16x16 panel it must be the one that to_panel says, and nothing else */
DMOD_TEST_STEP(packing_puts_the_pixel_where_the_panel_has_it)
{
    for (int r = 0; r < 4; r++)
    {
        dmuc8253_rotation_t rotation = (dmuc8253_rotation_t)r;
        uint8_t fb[2 * 16], out[2 * 16];
        memset(fb, 0xFF, sizeof(fb));
        uc8253_put_pixel(fb, 2, 3, 10, false);
        uc8253_window_t whole = { 0, 0, 15, 15 };
        uc8253_pack_window(rotation, 16, 16, fb, 2, &whole, out);
        uint32_t px, py, black = 0;
        uc8253_to_panel(rotation, 16, 16, 3, 10, &px, &py);
        for (uint32_t y = 0; y < 16; y++)
            for (uint32_t x = 0; x < 16; x++)
                if (!uc8253_get_bit(out, 2, x, y))
                {
                    black++;
                    DMOD_TEST_EXPECT_TRUE(x == px && y == py);
                }
        DMOD_TEST_EXPECT_EQ(black, 1);
    }
}

DMOD_TEST_STEP(packing_a_window_takes_just_its_bytes)
{
    uint8_t fb[4 * 8];                      /* 32x8 picture */
    for (int i = 0; i < 32; i++)
        fb[i] = (uint8_t)i;
    uc8253_window_t w = { 8, 2, 23, 3 };    /* bytes 1 and 2 of the lines 2 and 3 */
    uint8_t out[4];
    uc8253_pack_window(dmuc8253_rotation_0, 32, 8, fb, 4, &w, out);
    DMOD_TEST_EXPECT_EQ(out[0], 9);
    DMOD_TEST_EXPECT_EQ(out[1], 10);
    DMOD_TEST_EXPECT_EQ(out[2], 13);
    DMOD_TEST_EXPECT_EQ(out[3], 14);
}

/* ---- Splash logo ---- */

#define TEST_LOGO_PATH      "dmuc8253_test_logo.dmvir"
#define TEST_WIDTH          16
#define TEST_HEIGHT         8
#define TEST_STRIDE         2

static void put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t *p, uint32_t v) { put_le16(p, (uint16_t)v); put_le16(p + 2, (uint16_t)(v >> 16)); }

/* A 4x2 RGB565A8 .dmvir: row 0 black, white, black (transparent), black (half transparent); row 1 black. `compression` makes it a packed
 * .dmvi instead. */
static bool write_test_logo(const char *compression)
{
    uint8_t file[56 + 4 * 2 * 2 + 4 * 2];
    const uint16_t colors[8] = { 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 };
    const uint8_t alpha[8] = { 255, 255, 0, 128, 255, 255, 255, 255 };

    memset(file, 0, sizeof(file));
    memcpy(file, "DMVI", 4);
    put_le16(&file[6], 1);
    put_le32(&file[8], sizeof(file));
    put_le16(&file[12], 4);
    put_le16(&file[14], 2);
    file[16] = 3;                                   /* RGB565A8 */
    put_le32(&file[20], 4 * 2);                     /* stride */
    put_le32(&file[24], 56);                        /* pixels */
    put_le32(&file[28], 4);                         /* alpha_stride */
    put_le32(&file[32], 56 + 16);                   /* alpha */
    strcpy((char *)&file[40], compression);
    put_le32(&file[52], sizeof(file) - 56);
    for (int i = 0; i < 8; i++)
    {
        put_le16(&file[56 + 2 * i], colors[i]);
        file[56 + 16 + i] = alpha[i];
    }
    void *fp = Dmod_FileOpen(TEST_LOGO_PATH, "wb");
    if (fp == NULL)
        return false;
    bool written = Dmod_FileWrite(file, 1, sizeof(file), fp) == sizeof(file);
    Dmod_FileClose(fp);
    return written;
}

static dmuc8253_splash_target_t target(uint8_t *fb, uint32_t stride, uint16_t width, uint16_t height, uint32_t background)
{
    dmuc8253_splash_target_t t = { fb, stride, width, height, background };
    return t;
}

DMOD_TEST_STEP(splash_logo_is_drawn_centered)
{
    uint8_t fb[TEST_STRIDE * TEST_HEIGHT];
    memset(fb, 0xFF, sizeof(fb));                   /* a white screen */
    DMOD_TEST_EXPECT_TRUE(write_test_logo(""));
    dmuc8253_splash_target_t t = target(fb, TEST_STRIDE, TEST_WIDTH, TEST_HEIGHT, 0xFFFFFF);
    DMOD_TEST_EXPECT_EQ(dmuc8253_splash_draw(TEST_LOGO_PATH, &t), 0);
    /* The 4x2 logo sits at x 6..9, y 3..4 */
    DMOD_TEST_EXPECT_FALSE(uc8253_get_bit(fb, TEST_STRIDE, 6, 3));      /* black */
    DMOD_TEST_EXPECT_TRUE(uc8253_get_bit(fb, TEST_STRIDE, 7, 3));       /* white */
    DMOD_TEST_EXPECT_TRUE(uc8253_get_bit(fb, TEST_STRIDE, 8, 3));       /* transparent: the background */
    DMOD_TEST_EXPECT_FALSE(uc8253_get_bit(fb, TEST_STRIDE, 9, 3));      /* black, half transparent, over white: 127 - just under the half */
    DMOD_TEST_EXPECT_FALSE(uc8253_get_bit(fb, TEST_STRIDE, 6, 4));
    DMOD_TEST_EXPECT_FALSE(uc8253_get_bit(fb, TEST_STRIDE, 9, 4));
    DMOD_TEST_EXPECT_TRUE(uc8253_get_bit(fb, TEST_STRIDE, 5, 3));       /* around it: untouched */
    DMOD_TEST_EXPECT_TRUE(uc8253_get_bit(fb, TEST_STRIDE, 10, 4));
    DMOD_TEST_EXPECT_TRUE(uc8253_get_bit(fb, TEST_STRIDE, 6, 2));
}

DMOD_TEST_STEP(splash_logo_is_blended_over_the_background)
{
    uint8_t fb[TEST_STRIDE * TEST_HEIGHT];
    memset(fb, 0x00, sizeof(fb));                   /* a black screen */
    DMOD_TEST_EXPECT_TRUE(write_test_logo(""));
    dmuc8253_splash_target_t t = target(fb, TEST_STRIDE, TEST_WIDTH, TEST_HEIGHT, 0x000000);
    DMOD_TEST_EXPECT_EQ(dmuc8253_splash_draw(TEST_LOGO_PATH, &t), 0);
    DMOD_TEST_EXPECT_TRUE(uc8253_get_bit(fb, TEST_STRIDE, 7, 3));       /* the white pixel of the logo */
    DMOD_TEST_EXPECT_FALSE(uc8253_get_bit(fb, TEST_STRIDE, 8, 3));      /* transparent: black */
    DMOD_TEST_EXPECT_FALSE(uc8253_get_bit(fb, TEST_STRIDE, 6, 3));
}

DMOD_TEST_STEP(splash_logo_is_cropped_to_a_small_screen)
{
    uint8_t fb[1 * 2];
    fb[0] = fb[1] = 0xFF;
    DMOD_TEST_EXPECT_TRUE(write_test_logo(""));
    dmuc8253_splash_target_t t = target(fb, 1, 2, 2, 0xFFFFFF);
    DMOD_TEST_EXPECT_EQ(dmuc8253_splash_draw(TEST_LOGO_PATH, &t), 0);
    /* The 4x2 logo centered on 2x2: its columns 1 and 2 (white, transparent) of row 0 and (black, black) of row 1 */
    DMOD_TEST_EXPECT_TRUE(uc8253_get_bit(fb, 1, 0, 0));
    DMOD_TEST_EXPECT_TRUE(uc8253_get_bit(fb, 1, 1, 0));
    DMOD_TEST_EXPECT_FALSE(uc8253_get_bit(fb, 1, 0, 1));
    DMOD_TEST_EXPECT_FALSE(uc8253_get_bit(fb, 1, 1, 1));
}

DMOD_TEST_STEP(splash_logo_errors)
{
    uint8_t fb[TEST_STRIDE * TEST_HEIGHT];
    dmuc8253_splash_target_t t = target(fb, TEST_STRIDE, TEST_WIDTH, TEST_HEIGHT, 0xFFFFFF);
    DMOD_TEST_EXPECT_EQ(dmuc8253_splash_draw("there_is_no_such_logo.dmvir", &t), -2);          /* ENOENT */
    DMOD_TEST_EXPECT_TRUE(write_test_logo("lz"));                                              /* a packed .dmvi */
    DMOD_TEST_EXPECT_EQ(dmuc8253_splash_draw(TEST_LOGO_PATH, &t), -95);                        /* ENOTSUP */
    DMOD_TEST_EXPECT_EQ(dmuc8253_splash_draw(NULL, &t), -22);                                  /* EINVAL */
    DMOD_TEST_EXPECT_EQ(dmuc8253_splash_draw(TEST_LOGO_PATH, NULL), -22);
}

DMOD_TEST_STEP(validate_checks_the_pixel_format_and_the_threshold)
{
    dmuc8253_config_t c = panel();
    c.pixel_format = dmuc8253_pixel_format_rgb565;
    DMOD_TEST_EXPECT_TRUE(dmuc8253_validate_config(&c));
    c.pixel_format = (dmuc8253_pixel_format_t)2;
    DMOD_TEST_EXPECT_FALSE(dmuc8253_validate_config(&c));
    c = panel();
    c.threshold = 0;
    DMOD_TEST_EXPECT_FALSE(dmuc8253_validate_config(&c));
    c.threshold = 255;
    DMOD_TEST_EXPECT_TRUE(dmuc8253_validate_config(&c));
}

DMOD_TEST_STEP(rgb565_pixels_are_white_from_the_threshold)
{
    DMOD_TEST_EXPECT_TRUE(uc8253_rgb565_is_white(0xFFFF, 128));         /* white */
    DMOD_TEST_EXPECT_FALSE(uc8253_rgb565_is_white(0x0000, 128));        /* black */
    DMOD_TEST_EXPECT_TRUE(uc8253_rgb565_is_white(0x07E0, 128));         /* pure green: light */
    DMOD_TEST_EXPECT_FALSE(uc8253_rgb565_is_white(0x001F, 128));        /* pure blue: dark */
    /* a middle gray (0x8410) is 132 of 255: white with 128, black with 160 - the threshold moves the strokes of the text */
    DMOD_TEST_EXPECT_TRUE(uc8253_rgb565_is_white(0x8410, 128));
    DMOD_TEST_EXPECT_FALSE(uc8253_rgb565_is_white(0x8410, 160));
}

DMOD_TEST_STEP(rgb565_area_becomes_the_plane)
{
    /* 16 x 2 framebuffer: the left half white, the right half black; only the first line is converted */
    uint8_t fb[16 * 2 * 2];
    uint8_t plane[2 * 2] = { 0x00, 0x00, 0xAA, 0xAA };
    for (unsigned y = 0; y < 2; y++)
        for (unsigned x = 0; x < 16; x++)
        {
            uint16_t p = x < 8 ? 0xFFFF : 0x0000;
            fb[y * 32 + x * 2] = (uint8_t)p;
            fb[y * 32 + x * 2 + 1] = (uint8_t)(p >> 8);
        }
    uc8253_rgb565_to_plane(fb, 32, plane, 2, 0, 0, 15, 0, 128);
    DMOD_TEST_EXPECT_EQ(plane[0], 0xFF);
    DMOD_TEST_EXPECT_EQ(plane[1], 0x00);
    DMOD_TEST_EXPECT_EQ(plane[2], 0xAA);                                /* the second line was not in the area */
    uc8253_rgb565_to_plane(fb, 32, plane, 2, 4, 1, 11, 1, 128);         /* a part of the line: only its bits change */
    DMOD_TEST_EXPECT_EQ(plane[2], 0xAF);                                /* x 4..7 white: 1010 1111 */
    DMOD_TEST_EXPECT_EQ(plane[3], 0x0A);                                /* x 8..11 black: 0000 1010 */
}

DMOD_TEST_STEP(plane_becomes_an_rgb565_framebuffer)
{
    uint8_t plane[2] = { 0xF0, 0x0F };                                  /* 16 x 1: 4 white, 8 black, 4 white... as bits */
    uint8_t fb[16 * 2];
    uc8253_plane_to_rgb565(plane, 2, fb, 32, 16, 1);
    for (unsigned x = 0; x < 16; x++)
    {
        bool white = x < 4 || (x >= 12);
        DMOD_TEST_EXPECT_EQ(fb[x * 2], white ? 0xFF : 0x00);
        DMOD_TEST_EXPECT_EQ(fb[x * 2 + 1], white ? 0xFF : 0x00);
    }
}
