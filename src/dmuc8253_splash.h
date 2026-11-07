#ifndef DMUC8253_SPLASH_H
#define DMUC8253_SPLASH_H

#include "dmuc8253.h"

/**
 * @brief Draws the logo of a .dmvir file in the middle of the framebuffer (the implementation of dmuc8253_splash_draw).
 *
 * @return 0 on success, -ENOENT when the file does not exist, another negative errno when it is not a logo this driver can draw
 */
int dmuc8253_splash_draw_file(const char *path, const dmuc8253_splash_target_t *target);

#endif /* DMUC8253_SPLASH_H */
