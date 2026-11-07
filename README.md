# dmuc8253

DMOD driver for e-paper displays with a **UC8253** controller on SPI (240x320 at most, black and white): the 3.1" GDEQ031T10 of the
LilyGO T-Deck Pro.

[![CI](https://github.com/choco-technologies/dmuc8253/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmuc8253/actions/workflows/ci.yml)

The controller keeps two pictures in a RAM of its own: the one on the screen and the next one; a refresh turns the first into the second.
The driver keeps a **framebuffer** (`DMDRVI_GFX_PIXEL_FORMAT_MONO1`: 1 bit per pixel, 8 pixels per byte, set = white) that clients draw into,
and `PRESENT` sends the area drawn to the panel and refreshes just that area (0.7 s). The first refresh, and every
`full_refresh_interval`-th one, is a full refresh (1.1 s, the whole screen, no ghosts). The node follows the generic graphics interface of
[dmdrvi](https://github.com/choco-technologies/dmdrvi) (`DMDRVI_IOCTL_GFX_*`), so generic code needs only the path: `/dev/dmuc82530`.

It talks to the panel through the nodes of other drivers - [dmspi](https://github.com/choco-technologies/dmspi) for the bus and
[dmgpio](https://github.com/choco-technologies/dmgpio) for the data/command, BUSY and reset lines - so it has no hardware port and runs
wherever those do. The panel is portrait; `rotation=90` makes the node landscape (the driver turns the picture while it sends it).

## Use

One `friends_group` holds the display and everything it is wired to; `friend_role` says who is who (`spi`, `dc`, `busy`, `reset`;
`chip_select` is the business of dmspi). [configs/board/t-deck-pro/display.ini](configs/board/t-deck-pro/display.ini) is the whole display of
the T-Deck Pro (it also holds the chip selects of the SD card and the radio that share the bus). In the flash modules of the board:

```text
dmdma driver=mcu/esp32s3.ini
dmgpio
dmspi
dmuc8253 driver=board/t-deck-pro/display.ini
```

```c
void *fp = Dmod_FileOpen("/dev/dmuc82530", "r+");
dmdrvi_gfx_info_t info;
Dmod_Ioctl(fp, DMDRVI_IOCTL_GFX_GET_INFO, &info);             /* 320x240, MONO1, stride 40 */
dmdrvi_gfx_fill_rect_t rect = { 0, 0, 320, 40, 0xFF000000U }; /* 0xAARRGGBB: black */
Dmod_Ioctl(fp, DMDRVI_IOCTL_GFX_FILL_RECT, &rect);
Dmod_Ioctl(fp, DMDRVI_IOCTL_GFX_PRESENT, NULL);               /* to the screen (or an area of it) */
```

The display shows a **splash screen** from the start (`clear_color`, white, and a `.dmvir` logo, `splash_logo` or `$SPLASH_LOGO`), as the other
display drivers do; the logo is made black and white (luminance 128).

See [docs/api-reference.md](docs/api-reference.md) and [docs/configuration.md](docs/configuration.md).

## Building

The code does not depend on the CPU, so there is no port: the module is built per architecture, as dmst7789 or dmspiflash are.

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build                       # the host: the tests
cmake -S . -B build-esp -DDMOD_TOOLS_NAME=arch/xtensa/esp32s3 -DDMOD_CPU_FAMILY=esp32s3 && cmake --build build-esp   # an architecture
```

`DMOD_CPU_FAMILY` (stm32f7 by default) only picks the package of dmgpio whose headers are used (its packages are named by family; the
headers are the same for every architecture).
