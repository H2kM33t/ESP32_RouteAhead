# ESP32_RouteAhead — firmware

Handlebar navigation display. Receives turn-by-turn data from the phone over BLE and
renders it on a small colour TFT, so the rider never has to look at their phone.

Companion app: [BikeNavApp_RouteAhead](https://github.com/H2kM33t/BikeNavApp_RouteAhead)

## Hardware

| | |
|---|---|
| MCU | ESP32-C3 (`esp32-c3-devkitm-1`) |
| Display | 1.8" SPI TFT, ST7735 driver, 128×160 native — driven in landscape as 160×128 |
| Graphics | LovyanGFX 1.2.x, full-screen sprite (double-buffered) |
| Link | BLE GATT server, NimBLE-Arduino 2.x |

### Wiring

Panel config lives in `platformio.ini` as build flags rather than in a library header,
so it survives a clean checkout. These are **confirmed against the board**, not guessed:

| Flag | GPIO | Panel pin |
|---|---|---|
| `TFT_SCLK` | 4 | SCK / CLK |
| `TFT_MOSI` | 6 | SDA / DIN / MOSI |
| `TFT_CS` | 7 | CS |
| `TFT_DC` | 2 | DC / A0 / RS |
| `TFT_RST` | 3 | RES / RST |
| `TFT_BL` | −1 | BLK / LED — wired straight to 3V3 |

`TFT_BL` is `-1` because the backlight has no GPIO. This one cost real time: it was
previously set to GPIO3 on a guess, which is the panel's **reset** line — so the firmware
was resetting the display several times a second while believing it was setting
brightness. The `#if TFT_BL >= 0` guard around the PWM block is what makes `-1` mean "no
backlight control" rather than "drive pin −1".

### Why LovyanGFX and not TFT_eSPI

TFT_eSPI crashes on boot on the C3. It defines `SPI_PORT` as `SPI2_HOST` (== 1), but this
ESP-IDF's `REG_SPI_BASE(i)` only returns a real base when `i == 2` and yields 0 otherwise,
so every register pointer lands near address `0x10` and the first draw store-faults inside
`begin_tft_write()`. The define is unguarded, so it can't be overridden from
`platformio.ini`. LovyanGFX handles the C3's GPIO matrix properly.

### If the picture looks wrong

Three things account for almost every ST7735 problem, in order of likelihood:

1. **Wrong colour order.** Blue and red swapped, or a blue cast over everything, is a BGR
   panel being fed RGB. Flip `cfg.rgb_order` in `Display.h`.
2. **Wrong RAM offsets.** ST7735 panels ship with different internal offsets, identified
   by the colour of the plastic tab on the ribbon. Symptoms: a coloured border strip down
   one edge, or the image shifted a few pixels. Adjust `cfg.offset_x` / `cfg.offset_y` in
   `Display.h` (common values are 0, 1, 2 and 26).
3. **SPI too fast for the wiring.** 27 MHz is safe on flying leads. Drop `SPI_FREQUENCY`
   to `20000000` if you see sparkle or torn frames; only go to 40 MHz on a proper PCB.

Three bring-up environments exist for when none of that helps. Each must be asked for
explicitly with `-e`, because `default_envs` in `platformio.ini` otherwise pins uploads to
the real firmware — without that line `pio run -t upload` flashes *every* env in the file
and the board ends up running whichever finished last:

```bash
pio run -e display-test -t upload   # backlight, then solid colours
pio run -e pin-scan     -t upload   # sweeps plausible pin sets, paints a number for each
pio run -e panel-probe  -t upload   # holds the pins, sweeps bus/panel settings
```

If the serial monitor prints nothing, it's almost certainly `ARDUINO_USB_CDC_ON_BOOT` —
already set in `platformio.ini`, but worth knowing about, since without it `Serial`
goes to the UART pins instead of the C3's native USB.

## Building

```bash
pio run --target upload
```

Serial monitor at 115200. On boot it prints the negotiated MTU, which is the first thing
to check if frames aren't arriving.

Current footprint: **25.5 KB static RAM (8.0%)** and **586 KB flash (45.8%)**. The
framebuffer is a further 40 KB (160×128×16bpp) taken from the heap at startup, which
leaves plenty of headroom alongside NimBLE. If allocation ever fails it retries at 8-bit
colour, and failing that draws straight to the panel — flickery, but visible.

## What's on screen

Beeline-style: the road you are about to ride is the largest thing on the screen, not an
inset panel. The turn and its distance sit in a compact header; everything that can wait
is a thin footer.

```
┌────────────────────────────────────┐
│  ➜  275 m                          │  the turn and its distance
│     Ring Road                      │  the street you're turning ONTO
├────────────────────────────────────┤
│        ╱     ⛽                     │  side roads in grey, places as glyphs
│   ────┤                            │
│       │  ●                         │  ← green dot marks where the turn happens
│       ⌃                            │  the route in blue, chevrons marching up it
│       ▲                            │  ← the bike, anchored on the centre line
├────────────────────────────────────┤
│  62 km/h  │  4.2 km  │  18 min     │  three even cells
└────────────────────────────────────┘
```

The map is drawn the way a map app draws one, and each part of that is deliberate:

- **The bike is anchored, not fitted.** It sits on the horizontal centre line at 80% of
  the map's height, every frame, and the zoom is chosen so the geometry fits *around* it.
  An earlier version fitted the geometry's bounding box and centred that, which let the
  bike itself wander around the frame whenever the road bent — the screen looked
  lopsided and the rider's own position moved for no reason they could see.
- **Side roads are drawn underneath in grey**, cased so they read as roads rather than
  stray lines. A route line alone in black gives no sense of place; the side roads are
  what make a junction look like a junction.
- **The route is a cased blue ribbon with chevrons travelling along it.** Blue-for-your-
  route and grey-for-everything-else is a convention every rider already knows. The
  chevrons are spaced in *pixels*, not metres, so they look the same however far the map
  is zoomed, and they animate forward — which tells you which way "ahead" is without a
  single word.
- **Landmarks** (fuel, food, hospital, ATM, junction) are dim glyphs beside the road.
  They deliberately do *not* drive the zoom: a petrol station 200 m off the route would
  otherwise zoom the whole map out to include it, so they are simply clipped instead.
- **The green dot is the maneuver itself**, and it grows a pulsing halo inside 80 m.
  Without it the rider can see the road bend but not which bend the countdown refers to.

Everything eases rather than snaps — the distance readout, the zoom, the turn accent. A
number that jumps 300 → 250 → 200 reads as a glitch at a glance; one that slides reads as
movement.

A three-phase startup animation plays at power-on while the radio comes up: the chevron
rises, the wordmark wipes in, a progress bar sweeps. It costs ~1.6 s and earns it by
proving the panel is alive before any phone is involved — a device that shows nothing
until it connects feels broken when it isn't.

Other states: `RouteAhead / Waiting for phone` (no BLE connection), `Ready / Pick a
destination` (linked but no route), an amber `Off route - rerouting` banner, an arrival
screen with expanding rings, and `No signal` if no frame has arrived for six seconds — a
frozen last-known turn would be worse than admitting the link is dead. The waiting screens
carry a breathing dot so they never look frozen.

The palette matches the app's, so the two screens read as one product.

## The wire protocol

`BleManager::parsePacket` here and `NavPacket.kt` in the app are two halves of one
contract. **Change one and you must change the other.**

| Bytes | Meaning |
|-------|---------|
| 0     | protocol version (`NAV_PROTOCOL_VERSION`, currently 4) |
| 1     | flags: bit0 hasRoute, bit1 offRoute, bit2 arrived, bit3 rerouting |
| 2     | maneuver ordinal (`ManeuverType`) |
| 3–4   | uint16 distance to the maneuver, metres |
| 5–6   | uint16 speed, 0.1 km/h units |
| 7–8   | uint16 ETA, seconds |
| 9–10  | uint16 remaining distance, decametres |
| 11    | number of route points (0–`MAX_ROUTE_POINTS`, 20) |
| 12…   | per point: int16 x, int16 y — centimetres, bike-relative, +Y ahead, +X right |
| then  | uint8 landmark count (0–`MAX_LANDMARKS`, 5) |
| then  | per landmark: int16 x, int16 y, uint8 type (`LandmarkType`) |
| then  | uint8 branch count (0–`MAX_BRANCHES`, 6) |
| then  | per branch: int16 x, int16 y, uint8 heading ÷2°, uint8 length ÷2 m |
| then  | uint8 street name length, then that many UTF-8 bytes (max `MAX_STREET_NAME`, 22) |

All little-endian. Worst case **178 bytes**, against the 182 usable at the negotiated
MTU of 185. Every variable-length block is length-prefixed and appears in a fixed order,
which is what lets the parser validate the whole frame before touching `navState`.

A side road is sent as an anchor plus a direction rather than a polyline, because that is
all a 160px screen can show: *where* it leaves the route and *which way* it goes. Six
bytes against the eight a two-point line would need — the difference between six of them
fitting in a frame and four. Route points dropped from 24 to 20 in v4 to pay for the new
blocks; Douglas-Peucker keeps the corners regardless of the budget, so the visible cost
is a slightly coarser curve on long straights.

Three things worth knowing:

1. **`ManeuverType` and `LandmarkType` orders are frozen** — both arrive as raw ordinals
   and must match the `Maneuver` and `Landmark.Type` enums in the app. Append at the end
   only. The app's `NavPacketTest` fails if an ordinal moves.
2. **MTU matters.** The default ATT MTU of 23 bytes leaves 20 for the payload, well under
   the 178-byte worst case. `NimBLEDevice::setMTU(185)` here plus the phone's request is
   what makes full frames possible.
3. **Frames are validated before `navState` is touched.** A short or truncated write is
   dropped whole rather than half-applied, so the screen never mixes two frames.

Set `NAV_DEBUG` to `1` at the top of `BleManager.cpp` to print every decoded frame — and
every rejected one, with the reason — to the serial monitor.

BLE identifiers, which must match the app exactly:

| | |
|---|---|
| Device name | `BikeNav-RouteAhead` |
| Service UUID | `4fafc201-1fb5-459e-8fcc-c5c9c331914b` |
| Characteristic UUID | `beb5483e-36e1-4688-b7f5-ea07361b26a8` |

## Concurrency

`navState` is written by the NimBLE host task and read by `loop()`. On the single-core C3
those are two FreeRTOS tasks sharing one core, so a write can be preempted mid-update.
Every access goes through `navStateLock()` / `navStateUnlock()`, and `render()` copies the
whole struct out under the lock and draws from the copy — so the lock is held for
microseconds rather than for a whole frame.

Without this you get torn reads: the new frame's `numPoints` alongside the old frame's
coordinates, which draws a polyline with a spike through it.

## Library versions

`lib_deps` pins NimBLE-Arduino 2.x. Its callback signatures differ from 1.x — 2.x adds a
`NimBLEConnInfo&` to every callback and a reason code to `onDisconnect`. If you pin 1.x
you'll need to drop those parameters again.
