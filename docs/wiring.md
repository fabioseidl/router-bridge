# Wiring as built

Board: ESP32-S3-WROOM-1 **N16R8** on a third-party dev board, verified 2026-08-09
(see [hardware.md](hardware.md) §4). MAC `28:84:85:60:d3:84`.

## Router ↔ ESP32-S3

Router: Sagemcom F@st 5670V2, board `GPON-BCM114-0010 REV1.0`. Console header **J1**,
verified 2026-08-09 (hardware.md §4.5). Silkscreen `3V3 RX TX GND`, pin 1 at the board edge.

Three wires. The 3.3 V line is deliberately **not** connected.

| J1 pin | Router label | ESP32-S3 pin | Signal | Note |
|---|---|---|---|---|
| 4 | GND | GND | ground reference | Connect first, remove last |
| 2 | RX (router input) | **GPIO17** | UART1 TX | Series resistor 220–470 Ω recommended |
| 3 | TX (router output) | **GPIO18** | UART1 RX | Series resistor 220–470 Ω recommended |
| 1 | 3V3 | — | — | **NOT CONNECTED** |

J1's labels are from the **router's** perspective, so the link is crossed: their TX to our
RX. Console logic level measured at **3.3 V** — no level shifter needed.

### The pin marked `TX` on the ESP32 devkit is not the TX you want

> **This is the mistake that cost a full day of diagnosis. See hardware.md §4.5.**

The devkit's header row ends `… 41 42 2 1 RX TX GND`. Those pins silkscreened **`TX`** and
**`RX`** are **UART0 — GPIO43 and GPIO44** — the ESP32's own console and flashing port. They
are not the router link, and the boot log says so on every reset:

```
I (652) cpu_start: GPIO 44 and 43 are used as console UART I/O pins
```

The router goes on the pins **numbered** `17` and `18`, on the opposite header. Wiring it to
`TX`/`RX` instead is silently destructive in two ways: the ESP32 transmits its entire ROM and
IDF boot log into the router's console input on every reset, and the FT232R's transmitter
contends with the router's TX driver on GPIO44. The firmware's RX-only guarantee does not
protect against this, because that guarantee is about UART1.

### J1 has no pin header fitted

J1 ships as four bare plated through-holes. A ganged female connector housing cannot mate
with it. Clear the holes with desoldering braid, then either solder the three wires directly
or fit a 4-pin 2.54 mm male header and use individual female jumpers — not a ganged shell,
which would force a wire onto `3V3`.

Count from the `3V3` end (marked `1`). The silkscreen only reads right-way-up with the board
inverted, and an off-by-one puts the ESP32 on the supply rail — which also measures 3.3 V, so
a multimeter will not catch the error.

### Why 3.3 V stays disconnected

The ESP32-S3 is powered from the MacBook's USB (5 V → onboard LDO → 3.3 V). Bonding that rail
to the router's 3.3 V regulator output would put two regulators in parallel with no
current-sharing mechanism, back-power the router whenever the Mac is attached, and ask the
router's rail to supply the ESP32's Wi-Fi transmit peaks (roughly 350–500 mA), which it is
generally not specified for. Both devices only need to share **GND** for the UART to work.

## Pin budget on this module

GPIO17 and GPIO18 are in the safe set. The constraints that rule out everything else:

| Pins | Status |
|---|---|
| GPIO26–32 | In-package SPI flash. Using them crashes the firmware. |
| GPIO33–37 | Octal PSRAM. **Confirmed in use** — the boot log shows the `octal_psram` driver active (hardware.md §4.2). |
| GPIO19, GPIO20 | Native USB D−/D+. In use by Path A. |
| GPIO43, GPIO44 | UART0 TX/RX to the FT232R bridge. Confirmed in the boot log: `GPIO 44 and 43 are used as console UART I/O pins`. |
| GPIO0, 3, 45, 46 | Strapping pins. Avoid. |
| GPIO48 (or 38) | Onboard RGB LED, board-revision dependent. Not used. |
| **Safe for use** | GPIO1–18, 21, 38–42, 47 |

## UART assignment

| UART | Pins | Purpose |
|---|---|---|
| UART0 | GPIO43/44 → FT232R → "COM" USB-C | ESP-IDF log, firmware upload, `esp_console` control channel. **Never carries router data.** |
| UART1 | GPIO17/18 → router | The bridged console link. |
| USB Serial/JTAG | GPIO19/20 → "USB" USB-C | Path A data channel to the MacBook. |

Keeping the router console off UART0 is deliberate: the ESP32 ROM bootloader prints its own
reset-time messages on UART0, and if the router were wired there those messages would be
injected into the router's console input — enough to interrupt U-Boot autoboot or corrupt a
running shell.

## Retired risk

The router's console TX idle voltage was not measured before soldering — 1.8 V would have
required a level shifter and a spec revision. It has since been measured at J1 as **3.3 V**
(hardware.md §4.4). No level shifter is required and no spec revision follows.
