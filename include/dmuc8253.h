#ifndef DMUC8253_H
#define DMUC8253_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmod_types.h"
#include "dmuc8253_defs.h"
#include "dmuc8253_types.h"

/**
 * @brief Check a configuration without touching hardware
 *
 * @return true when the size and the rotation make a panel the controller can show.
 */
dmod_dmuc8253_api(1.0, bool, _validate_config, ( const dmuc8253_config_t *config ));

/**
 * @brief Convert a 0xAARRGGBB color to the pixel of the framebuffer (1 bit)
 *
 * @return true for white (the bit is set), false for black. The alpha is ignored; a color is white when its luminance is 128 or more.
 */
dmod_dmuc8253_api(1.0, bool, _color_to_mono, ( uint32_t argb ));

/**
 * @brief Draw the logo of a .dmvir file in the middle of a monochrome framebuffer
 *
 * A .dmvir is an uncompressed .dmvi (dmview's image format): the 56-byte header and the raw pixels - RGB565, RGB565A8 or ARGB8888.
 * Transparent pixels are blended over target->background, then every pixel becomes black or white (see dmuc8253_color_to_mono); a logo
 * larger than the framebuffer is cropped. The file is read row by row. The driver calls it for the splash screen; it is exported for
 * the tests and for code that draws a logo itself.
 *
 * @return 0 on success, -ENOENT when the file does not exist, another negative errno when it is not a logo it can draw
 */
dmod_dmuc8253_api(1.0, int, _splash_draw, ( const char *path, const dmuc8253_splash_target_t *target ));

#endif // DMUC8253_H
