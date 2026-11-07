# dmuc8253 API

## The node

`/dev/dmuc8253<instance>` (`instance` of the configuration, 0 by default). The device file *is* the framebuffer: `read()` and `write()`
at a byte offset (`DMDRVI_GFX_PIXEL_FORMAT_MONO1`: 8 pixels per byte, the leftmost in the most significant bit, set = white,
`stride = (width + 7) / 8`). Nothing reaches the screen until `PRESENT` or `flush()`.

| Command | |
|---|---|
| `DMDRVI_IOCTL_GFX_GET_INFO` | `dmdrvi_gfx_info_t`: the size as shown (after the rotation), MONO1, `bytes_per_pixel` 0, stride, 1 buffer |
| `DMDRVI_IOCTL_GFX_GET_FRAMEBUFFER` | the address of the framebuffer, to draw into directly; then `PRESENT` the area drawn |
| `DMDRVI_IOCTL_GFX_FILL_RECT` | fills a rectangle of the framebuffer (clipped; color 0xAARRGGBB: white from a luminance of 128, black below); not shown until `PRESENT` |
| `DMDRVI_IOCTL_GFX_PRESENT` | sends `area` (NULL: the whole screen) to the screen and refreshes it; blocks 0.7 s (partial) or 1.1 s (full) |
| `DMDRVI_IOCTL_GFX_SET/GET_DISPLAY_ENABLED` | the flag only: the screen keeps its picture without power |
| `DMDRVI_IOCTL_GFX_SET/GET_BACKLIGHT`, `_SWAP_BUFFERS`, `_WAIT_VSYNC` | `-ENOTSUP`: no backlight, one buffer, no scan-out |
| `dmuc8253_ioctl_cmd_get_config` | `dmuc8253_config_t` |
| `dmuc8253_ioctl_cmd_full_refresh` | the whole screen, a full refresh (clears the ghosts) |

`flush()` presents what was written through `write()` or `FILL_RECT` since the last `PRESENT` (the lines touched). A client that draws
through the pointer of `GET_FRAMEBUFFER` must `PRESENT` the area itself.

## Functions (module API)

| | |
|---|---|
| `bool dmuc8253_validate_config(const dmuc8253_config_t*)` | the size fits the controller (a width is a multiple of 8, up to 240 x 320), the rotation is known |
| `bool dmuc8253_color_to_mono(uint32_t argb)` | the pixel of the framebuffer: true (white) from a luminance of 128 |
| `int dmuc8253_splash_draw(const char *path, const dmuc8253_splash_target_t *target)` | draws the logo of a `.dmvir` in the middle of a MONO1 framebuffer (what the driver does for the splash screen); `-ENOENT` / `-ENOTSUP` / `-EINVAL` |
