#include "dmod.h"
#include "dmuc8253_splash.h"
#include "dmuc8253_panel.h"
#include <errno.h>
#include <string.h>

/*
 * The splash logo: an uncompressed .dmvi (dmview's image format, see dmview's
 * docs/image-format.md) - the header and the raw pixels, nothing to decode or
 * unpack. It is read row by row, so no more than a few lines of the image are
 * ever in memory.
 */

/* "DMVI" */
#define SPLASH_MAGIC                0x49564D44U
#define SPLASH_VERSION_MAJOR        0U
#define SPLASH_HEADER_SIZE          56U
#define SPLASH_COMPRESSION_OFFSET   40U

/* dmvi pixel formats this driver draws */
#define SPLASH_FORMAT_RGB565        1U
#define SPLASH_FORMAT_ARGB8888      2U
#define SPLASH_FORMAT_RGB565A8      3U

typedef struct
{
    uint32_t    file_size;
    uint16_t    width;
    uint16_t    height;
    uint8_t     format;
    uint32_t    stride;
    uint32_t    pixels;
    uint32_t    alpha_stride;
    uint32_t    alpha;
} splash_image_t;

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t pixel_size(uint8_t format)
{
    return (format == SPLASH_FORMAT_ARGB8888) ? 4U : 2U;
}

/* Checks that the rows of a plane (stride bytes apart, row_bytes each) lie in the file. */
static bool plane_fits(const splash_image_t *image, uint32_t offset, uint32_t stride, uint32_t row_bytes)
{
    uint64_t end = (uint64_t)offset + (uint64_t)stride * (image->height - 1U) + row_bytes;
    return offset >= SPLASH_HEADER_SIZE && stride >= row_bytes && end <= image->file_size;
}

static int parse_header(const uint8_t *h, uint32_t file_size, splash_image_t *image)
{
    image->file_size    = get_le32(&h[8]);
    image->width        = get_le16(&h[12]);
    image->height       = get_le16(&h[14]);
    image->format       = h[16];
    image->stride       = get_le32(&h[20]);
    image->pixels       = get_le32(&h[24]);
    image->alpha_stride = get_le32(&h[28]);
    image->alpha        = get_le32(&h[32]);

    if (get_le32(&h[0]) != SPLASH_MAGIC || get_le16(&h[4]) != SPLASH_VERSION_MAJOR ||
        image->file_size != file_size || image->width == 0 || image->height == 0)
        return -EINVAL;
    if (h[SPLASH_COMPRESSION_OFFSET] != '\0')
        return -ENOTSUP;                    /* Packed: a .dmvi, not a .dmvir */
    if (image->format != SPLASH_FORMAT_RGB565 && image->format != SPLASH_FORMAT_ARGB8888 &&
        image->format != SPLASH_FORMAT_RGB565A8)
        return -ENOTSUP;
    if (!plane_fits(image, image->pixels, image->stride, image->width * pixel_size(image->format)))
        return -EINVAL;
    if (image->format == SPLASH_FORMAT_RGB565A8 && !plane_fits(image, image->alpha, image->alpha_stride, image->width))
        return -EINVAL;
    return 0;
}

static int read_header(void *file, splash_image_t *image)
{
    uint8_t header[SPLASH_HEADER_SIZE];
    Dmod_FileSize_t file_size = Dmod_FileSize(file);

    if (file_size < SPLASH_HEADER_SIZE || file_size > UINT32_MAX ||
        Dmod_FileRead(header, 1, sizeof(header), file) != sizeof(header))
        return -EINVAL;
    return parse_header(header, (uint32_t)file_size, image);
}

static bool read_at(void *file, uint32_t offset, void *buffer, size_t size)
{
    return Dmod_FileSeek(file, (Dmod_FileOffset_t)offset, DMOD_SEEK_SET) == 0 &&
           Dmod_FileRead(buffer, 1, size, file) == size;
}

/* 0xAARRGGBB of pixel x of a row read from the file (alpha: its alpha row, RGB565A8 only). */
static uint32_t image_color(const splash_image_t *image, const uint8_t *row, const uint8_t *alpha, uint32_t x)
{
    if (image->format == SPLASH_FORMAT_ARGB8888)
        return get_le32(&row[4U * x]);

    uint32_t v = get_le16(&row[2U * x]);
    uint32_t r = ((v >> 11) & 0x1FU) * 255U / 31U;
    uint32_t g = ((v >> 5) & 0x3FU) * 255U / 63U;
    uint32_t b = (v & 0x1FU) * 255U / 31U;
    uint32_t a = (alpha != NULL) ? alpha[x] : 0xFFU;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* 0xFFRRGGBB of a color blended over the background. */
static uint32_t blend(uint32_t argb, uint32_t background)
{
    uint32_t a = argb >> 24;
    uint32_t out = 0xFF000000U;
    for (uint32_t shift = 0; shift < 24; shift += 8)
    {
        uint32_t fg = (argb >> shift) & 0xFFU;
        uint32_t bg = (background >> shift) & 0xFFU;
        out |= ((fg * a + bg * (255U - a) + 127U) / 255U) << shift;
    }
    return out;
}

/* Draws image row y (already read into row/alpha) at screen line line, from column x0 on. The framebuffer is MONO1: a pixel is white
 * (the bit is set) when the logo blended over the background is light. */
static void draw_row(const dmuc8253_splash_target_t *target, const splash_image_t *image,
                     const uint8_t *row, const uint8_t *alpha, uint32_t line, int32_t x0)
{
    for (uint32_t x = 0; x < image->width; x++)
    {
        int32_t column = x0 + (int32_t)x;
        if (column < 0 || column >= (int32_t)target->width)
            continue;
        uint32_t color = blend(image_color(image, row, alpha, x), target->background);
        uc8253_put_pixel(target->buffer, target->stride, (uint32_t)column, line, uc8253_color_is_white(color));
    }
}

/* Draws the image centered - cropped when it is larger than the screen. */
static int draw_image(void *file, const splash_image_t *image, const dmuc8253_splash_target_t *target)
{
    uint32_t row_bytes = image->width * pixel_size(image->format);
    bool has_alpha = (image->format == SPLASH_FORMAT_RGB565A8);
    uint8_t *row = Dmod_Malloc(row_bytes + (has_alpha ? image->width : 0U));
    if (row == NULL)
        return -ENOMEM;

    uint8_t *alpha = has_alpha ? row + row_bytes : NULL;
    int32_t x0 = ((int32_t)target->width - (int32_t)image->width) / 2;
    int32_t y0 = ((int32_t)target->height - (int32_t)image->height) / 2;
    int ret = 0;

    for (uint32_t y = 0; y < image->height && ret == 0; y++)
    {
        int32_t line = y0 + (int32_t)y;
        if (line < 0 || line >= (int32_t)target->height)
            continue;
        if (!read_at(file, image->pixels + y * image->stride, row, row_bytes) ||
            (has_alpha && !read_at(file, image->alpha + y * image->alpha_stride, alpha, image->width)))
            ret = -EIO;
        else
            draw_row(target, image, row, alpha, (uint32_t)line, x0);
    }
    Dmod_Free(row);
    return ret;
}

int dmuc8253_splash_draw_file(const char *path, const dmuc8253_splash_target_t *target)
{
    if (path == NULL || target == NULL || target->buffer == NULL)
        return -EINVAL;

    void *file = Dmod_FileOpen(path, "rb");
    if (file == NULL)
        return -ENOENT;

    splash_image_t image;
    int ret = read_header(file, &image);
    if (ret == 0)
        ret = draw_image(file, &image, target);
    Dmod_FileClose(file);
    return ret;
}
