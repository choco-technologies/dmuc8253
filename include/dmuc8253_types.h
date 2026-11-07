#ifndef DMUC8253_TYPES_H
#define DMUC8253_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "dmdrvi_ioctl.h"

/**
 * @brief Rotation of the picture: clockwise, as the clients see the screen
 *
 * The panel is portrait (width < height). 90 and 270 swap the width and the height: the node is then landscape. The controller has no
 * transposition, so the driver turns the picture while it sends it (the framebuffer is always as the clients see it).
 */
typedef enum
{
    dmuc8253_rotation_0 = 0,
    dmuc8253_rotation_90,
    dmuc8253_rotation_180,
    dmuc8253_rotation_270,
} dmuc8253_rotation_t;

/**
 * @brief Mirroring of the panel: the scan directions of the controller (PSR: SHL for the columns, UD for the lines)
 *
 * How the panel is mounted in the device: the picture of the controller's default scan can appear mirrored. Applied before the rotation.
 */
typedef enum
{
    dmuc8253_mirror_none = 0,
    dmuc8253_mirror_x,                      /**< Left and right swapped */
    dmuc8253_mirror_y,                      /**< Top and bottom swapped */
    dmuc8253_mirror_xy,                     /**< Both (the same as a rotation by 180) */
} dmuc8253_mirror_t;

/**
 * @brief Configuration of a display (the ini section of the driver)
 */
typedef struct
{
    uint16_t            width;                  /**< Width of the panel in rotation 0, pixels (a multiple of 8, 8..240) */
    uint16_t            height;                 /**< Height of the panel in rotation 0, lines (1..320) */
    dmuc8253_rotation_t rotation;               /**< Rotation of the picture */
    dmuc8253_mirror_t   mirror;                 /**< Mirroring of the panel (the scan directions of the controller) */
    bool                partial;                /**< Updates of an area refresh just that area (fast, 0.7 s); off: always the whole screen, slowly */
    uint16_t            full_refresh_interval;  /**< A full refresh (1.1 s, clears the ghosts) after this many partial ones; 0: never */
    uint32_t            clear_color;            /**< 0xAARRGGBB the framebuffer starts with: the background of the splash screen */
} dmuc8253_config_t;

/** @brief The framebuffer a splash logo is drawn into (MONO1: 8 pixels per byte, the leftmost in the top bit, set = white) */
typedef struct
{
    uint8_t  *buffer;           /**< Start of the framebuffer */
    uint32_t  stride;           /**< Bytes of one line */
    uint16_t  width;            /**< Pixels per line */
    uint16_t  height;           /**< Lines */
    uint32_t  background;       /**< 0xRRGGBB the logo is blended over before it is made black or white */
} dmuc8253_splash_target_t;

/** Commands of the node, besides the generic graphics ones (DMDRVI_IOCTL_GFX_*) */
typedef enum
{
    dmuc8253_ioctl_cmd_get_config = DMDRVI_IOCTL_CUSTOM_BASE,   /**< arg: dmuc8253_config_t* */
    dmuc8253_ioctl_cmd_full_refresh,                            /**< The whole screen, a full refresh (clears the ghosts); arg: NULL */
    dmuc8253_ioctl_cmd_max
} dmuc8253_ioctl_cmd_t;

#endif /* DMUC8253_TYPES_H */
