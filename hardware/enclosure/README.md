# Enclosure: Gauge Cap + Hub (concept v1)

Two-piece enclosure for the ToF (VL53L1X) build. Nothing is soldered: the two boards press
onto pins, one keyed Qwiic / STEMMA QT cable joins them, and the lids press on.

**Status:** concept model. Parts are interference-checked in CAD but have not been printed
or fitted to a tank yet. For home heating oil tanks only; not rated for gasoline, propane
or any location that needs explosion-proof equipment.

## Files

| Path | What |
|------|------|
| `stl/` | Printable parts, millimetres |
| `gauge_cap.step`, `hub.step` | Assemblies with board placeholders |
| `build.py` | Parametric source (CadQuery) |

Rebuild: `pip install cadquery numpy`, then `python3 build.py out`.

## Measure first

- **Vial outside diameter.** The model assumes 32 mm. Change `vial_od` (or just `liner_t`) and reprint `cap_liner`.
- **Board features.** Light pipe, reset pinhole and QT socket positions are approximate. Check against the board in hand; the board outline is set by `fb_l` / `fb_w`.
- **Sensor reading.** The sensor reads through the clear vial top. Confirm the live distance on the settings page tracks the puck before trusting thresholds.

## Print

| Part | Material | Notes |
|------|----------|-------|
| `cap_body` | PETG or ASA | Opening up, no supports |
| `cap_lid` | PETG or ASA | Top face down |
| `cap_liner` | TPU 95A | Reprint for other vial diameters |
| `hub_base` | PETG or ASA | Press magnets in after printing |
| `hub_lid` | PETG or ASA | Top face down, no supports |
| `hub_grommet` | TPU 95A | Slit one side to fit over the cable |
| `hub_lightpipe` | Clear PETG | Or a cut length of 3 mm clear filament |

## Buy

| Part | Qty |
|------|-----|
| ESP32 board with Qwiic / STEMMA QT socket, Feather format (modelled: Adafruit ESP32 Feather V2) | 1 |
| VL53L1X breakout with Qwiic / STEMMA QT sockets, 1.0 x 0.7 in (Adafruit 3967) | 1 |
| Qwiic cable, JST-SH 4-pin to 4-pin, 200 mm (keep under ~400 mm) | 1 |
| Braided sleeve, 4 mm | 200 mm |
| Neodymium disc magnets, 10 x 3 mm | 3 |
| Nylon thumbscrew, M4 x 12 | 1 |
| USB-C cable and listed 5 V / 1 A adapter | 1 |

## Assemble

1. Press the ToF board, sensor down, onto the four pins in the cap. Plug in the cable and lay it in the slot.
2. Press the cap lid on. Its tab closes the slot and grips the cable.
3. Press the ESP32 board onto the four pins in the hub base. Plug the cable into the QT socket and seat the grommet in its notch.
4. Drop the hub lid over the base. Push the cap onto the vial, turn the thumbscrew finger-tight, and set the hub on the tank.
5. Plug in USB-C and run the normal web setup.

## Wiring

| Wire | Signal |
|------|--------|
| Black | GND |
| Red | 3.3 V |
| Blue | SDA |
| Yellow | SCL |
