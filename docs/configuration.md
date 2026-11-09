# dmuc8253 configuration

The section of the display (`driver_name=dmuc8253`):

| Key | Default | |
|---|---|---|
| `instance` | 0 | the node is `/dev/dmuc8253<instance>` |
| `width`, `height` | 240, 320 | the panel in rotation 0 (a width is a multiple of 8, at most 240; a height at most 320) |
| `rotation` | 0 | 0, 90, 180 or 270, clockwise (90 and 270 swap the width and the height of the node). The T-Deck Pro is used at 90 |
| `mirror` | none | `x`, `y` or `xy`: the scan directions of the controller reversed (left-right, top-bottom, both) - how the panel is mounted in the device. The T-Deck Pro needs none |
| `partial` | yes | an update of an area refreshes just that area (fast, 0.7 s); `no`: always the whole screen, a full refresh |
| `full_refresh_interval` | 0 | a full refresh (1.1 s, clears the ghosts) after this many partial ones; 0: never |
| `clear_color` | 0xFFFFFFFF | 0xAARRGGBB the framebuffer starts with (white); the background of the splash screen |
| `pixel_format` | mono1 | the framebuffer of the clients: `mono1` (1 bit per pixel, what the panel shows) or `rgb565` - for a GUI that draws in colors (dmview draws RGB565 or ARGB8888 only): the driver makes the 1-bit picture of the area it presents, a pixel is white when its luminance is at least `threshold`. RGB565 costs 2 bytes per pixel (150 kB for 240 x 320) |
| `threshold` | 128 | 1..255, with `rgb565` (and for the colors of `FILL_RECT`): white from this luminance. Higher makes thin black strokes (text) bolder, lower thin white ones (text on black) |
| `splash_logo` | `$SPLASH_LOGO` | path of the splash logo (`.dmvir`) drawn in the middle of the screen at start; `none`: no logo |
| `reset` | no | `yes`: the panel waits for a `reset` friend before it is started, and the controller is reset by the line the first time (a controller left in deep sleep by the firmware that was there before is woken only by the line); later resets are the soft reset command, which keeps the pictures in its RAM. Without a line the controller is reset by command |
| `display` | on | the state reported by `GFX_GET_DISPLAY_ENABLED` (the screen keeps its picture without power) |

## Friends

`dmdevfs` tells every driver of a `friends_group` about the others (`friend_role=`), whatever order they are created in. The panel is
started once the required ones are there:

| `friend_role` | |
|---|---|
| `spi` | the dmspi node the display is on (required): `mode=0`, `nss_mode=soft`, 4 MHz (the vendor's library) |
| `dc` | the data/command line, a dmgpio output: low for a command, high for data (required) |
| `busy` | the BUSY line, a dmgpio input: low while the controller works (required) |
| `reset` | the reset line, a dmgpio output, low = reset (with `reset=yes`) |
| `chip_select` | not this driver's: dmspi drives it around every write |

Declare the display last in the file: the order is the order of the creation and, reversed, of the teardown.

## A shared bus

On the T-Deck Pro the display shares SCK and MOSI with the SD card and the LoRa radio. Their chip selects must be high, and the radio
must be powered: an unpowered SX1262 loads the lines through its input protection, and a clock of a few MHz from the SPI controller does
not reach the display (measured: no SCK edges on the pin at 4 MHz with the radio off) - the screen then keeps its old picture. The board
file sets them as outputs with their level at the start (`state=` of dmgpio, which needs a dmgpio with that key).

## Refreshing

The controller has the picture on the screen ("old", register 0x10) and the next one ("new", 0x13). `PRESENT` of an area, in the order of
the vendor's library (GxEPD2): the area goes to the new picture, a partial refresh turns the area of the screen into it (waveform of the
controller, forced temperature 121: 0.7 s), the drive voltages are switched off (a panel left powered fades), and the area goes to the old
picture too, so the next refresh starts from what is on the screen. The areas are whole bytes of the controller's RAM (8 pixels) in the
panel's orientation, so an area is widened to them. A full refresh writes both pictures and refreshes the whole screen (1.1 s).

The first refresh, at the start, is a full one: nothing is known of what the screen shows. `PRESENT` blocks until the screen is done.

## Splash screen

The framebuffer starts as `clear_color` and a logo is drawn in the middle (cropped when it is larger than the screen) before the panel is
started: the first thing the screen shows is the logo. The logo is a `.dmvir` file - an *uncompressed* `.dmvi` (dmview's image format),
`RGB565`, `RGB565A8` or `ARGB8888`; transparent pixels are blended over `clear_color`, then every pixel becomes black or white (luminance
128 and more is white). The logo comes from `splash_logo`, else from the `SPLASH_LOGO` environment variable that dmod-boot sets.
