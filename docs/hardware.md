# Verified facts — step 1 (SPEC §0 / §2)

Verification host: macOS (Darwin 25.5.0, Apple Silicon), user `fabioseidl`.
Date of verification: **2026-08-09**.

Everything in this file is the recorded output of a command run on this machine, or a file
read from the installed package tree. Assumptions carried over from SPEC.md that have **not**
been verified are marked `UNVERIFIED`.

---

## 1. Toolchain — verified

### 1.1 PlatformIO Core

| Fact | Value |
|---|---|
| Installed Core | **6.1.19** |
| Latest on PyPI | **6.1.19** (checked via `pypi.org/pypi/platformio/json`) |
| Binary location | `~/.platformio/penv/bin/pio` — **not on `PATH`** |

`pio` is not on `PATH` in this shell. Invoke as `~/.platformio/penv/bin/pio`, or add
`~/.platformio/penv/bin` to `PATH`. Core is current; no upgrade needed.

`pio pkg exec` **is** available in 6.1.19 (`-p/--package`, `-c/--call`).

### 1.2 Platform: `platformio/espressif32`

Registry listing (`pio pkg show platformio/espressif32`), newest first:

| Platform version | Published | `framework-espidf` required | ESP-IDF version |
|---|---|---|---|
| **7.0.1** | 2026-05-12 | `~4.60001.0` | **6.0.1** |
| 7.0.0 | 2026-04-30 | `~4.60000.0` | 6.0.0 |
| **6.13.0** | 2026-02-26 | `~3.50503.0` | **5.5.3** |
| 6.12.0 | 2025-07-31 | — | — |

Read from each release's `platform.json` on GitHub, not inferred.

Locally installed platforms: `espressif32` (HEAD), `@5.4.0`, `@6.8.1`, `@6.9.0`, `@6.13.0`,
plus a pioarduino fork `@54.3.21` and `@51.3.6`.

### 1.3 Framework: `platformio/framework-espidf`

Registry versions, newest first: `4.60001.0`, `4.60000.0`, `3.50503.0`, `3.50502.0`,
`3.50500.0`, `3.50402.0`, `3.50401.0`, `3.50400.0`, `3.50301.0`.

Installed at `~/.platformio/packages/framework-espidf`:

- `version.txt` → `5.5.3`
- `components/esp_common/include/esp_idf_version.h` → `MAJOR 5`, `MINOR 5`, `PATCH 3`

So **ESP-IDF 5.5.3 is already on disk**; IDF 6.0.1 is available but not downloaded (~384 MB
installed size per the registry).

### 1.4 esptool

| Fact | Value |
|---|---|
| Pinned by platform 6.13.0 **and** 7.0.1 | `platformio/tool-esptoolpy@~2.41100.0` |
| Installed at `~/.platformio/packages/tool-esptoolpy` | **esptool.py v4.11.0** |

Subcommands `chip_id` and `flash_id` (underscores) are correct for v4.11.0 — confirmed from
`esptool.py --help`. The hyphenated spelling belongs to esptool v5 and must not be used here.

**Gotcha:** the shipped `esptool.py` is not executable, so
`pio pkg exec -p tool-esptoolpy -c "esptool.py chip_id"` fails with `Permission denied`.
Working invocation:

```sh
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py chip_id
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py flash_id
```

(There is also a stray `tool-esptoolpy@src-…` directory holding esptool 5.0.2, pulled in by an
unrelated project. Do not use it for this project.)

---

## 2. Corrections to SPEC §0

Four claims in SPEC §0 are contradicted by the installed packages and the registry. §0 itself
flagged them as possibly stale and demanded verification; this is that verification.

### 2.1 Platform 7.0.1 does **not** ship ESP-IDF 5.5.x — it ships 6.0.1

SPEC §0 says platform 7.0.1's "`espidf` framework package tracked ESP-IDF 5.5.x". It does not:
`platform.json` for v7.0.1 requires `framework-espidf ~4.60001.0`, which is **IDF 6.0.1**.

The last platform release bundling IDF 5.5.3 is **6.13.0**.

### 2.2 PlatformIO **does** support ESP-IDF 6.x

SPEC §0 states "PlatformIO does not support ESP-IDF 6.x." That was true when written but is
now false: `framework-espidf` 4.60000.0 (IDF 6.0.0) was published 2026-04-21 and platform
7.0.0 shipped it on 2026-04-30.

This invalidates the stated *rationale* for targeting 5.5.x. See the open decision in §5.

### 2.3 `driver/uart.h` is **not** provided by the `driver` component in 5.5.3

SPEC §0 says: "In 5.5, `driver/uart.h` and `driver/usb_serial_jtag.h` are still provided by
the `driver` component. […] component names and headers changed in 6.0."

The component split already happened before 5.5.3. In the installed tree:

```
components/esp_driver_uart/include/driver/uart.h
components/esp_driver_usb_serial_jtag/include/driver/usb_serial_jtag.h
```

There are 26 `esp_driver_*` components present. The legacy `driver` component still exists but
only carries `i2c/`, `touch_sensor/`, `twai/` and a `deprecated/` tree — it does not own UART
or USB-Serial-JTAG.

**Consequence for the build:** the `#include` paths in SPEC (`driver/uart.h`,
`driver/usb_serial_jtag.h`) are correct and unchanged. What changes is
`idf_component_register(... REQUIRES ...)` in `src/CMakeLists.txt`, which must name
`esp_driver_uart` and `esp_driver_usb_serial_jtag`, not `driver`.

### 2.4 `pio pkg exec` works, but the §2.2 command as written does not

See §1.4 — executable bit, not a Core limitation.

---

## 3. Build-system findings that change `platformio.ini` / `sdkconfig.defaults`

Read from the installed platform 6.13.0 builder and the installed IDF 5.5.3 Kconfig.

### 3.1 `board = esp32-s3-devkitc-1` is the **N8, no-PSRAM** variant

`~/.platformio/platforms/espressif32@6.13.0/boards/esp32-s3-devkitc-1.json`:

```json
"name":  "Espressif ESP32-S3-DevKitC-1-N8 (8 MB QD, No PSRAM)",
"upload": { "flash_size": "8MB", "maximum_size": 8388608, "maximum_ram_size": 327680 }
"build":  { "flash_mode": "qio", "f_flash": "80000000L", "mcu": "esp32s3" }
```

Used unmodified against an N16R8 module this would flash as 8 MB and leave PSRAM off. It still
makes a reasonable base, but requires explicit overrides in `platformio.ini`:

- `board_upload.flash_size = 16MB`
- `board_upload.maximum_size = 16777216`

No stock board in platform 6.13.0 matches N16R8 exactly. `4d_systems_esp32s3_gen4_r8n16.json`
is nominally 16 MB flash + 8 MB PSRAM but is a different vendor board — using it would import
unrelated assumptions. Recommendation: keep `esp32-s3-devkitc-1` and override.

### 3.2 PlatformIO cross-checks flash size against sdkconfig

`builder/frameworks/espidf.py:2098-2106` compares `board.upload.flash_size` against
`ESPTOOLPY_FLASHSIZE` from the generated sdkconfig and **warns on mismatch**. Both must be set
consistently:

- `platformio.ini` → `board_upload.flash_size = 16MB`
- `sdkconfig.defaults` → `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`

### 3.3 Verified Kconfig symbol names (IDF 5.5.3, esp32s3)

Confirmed present in `components/esp_psram/esp32s3/Kconfig.spiram`,
`components/esptool_py/Kconfig.projbuild`, `components/esp_system/Kconfig`:

| Symbol | Purpose |
|---|---|
| `CONFIG_SPIRAM` | enable PSRAM |
| `CONFIG_SPIRAM_MODE_OCT` | octal (OPI) mode — the N16R8 case |
| `CONFIG_SPIRAM_MODE_QUAD` | quad mode — fallback if the fitted part is not octal |
| `CONFIG_SPIRAM_SPEED_80M` | PSRAM clock |
| `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` | 16 MB flash |
| `CONFIG_ESP_CONSOLE_UART_DEFAULT` | IDF console on UART0 |
| `CONFIG_ESP_CONSOLE_SECONDARY_NONE` | see §3.4 |

### 3.4 **Critical:** the IDF console defaults to *also* writing to USB Serial/JTAG

`components/esp_system/Kconfig:296-315`:

```
choice ESP_CONSOLE_SECONDARY
    default ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
```

With IDF defaults, setting the primary console to UART0 is **not sufficient**. The secondary
console still emits log output on USB Serial/JTAG — which is Path A, the router data channel.
That would inject `I (1234) wifi: …` lines straight into the console stream and break the
8-bit-clean requirement of SPEC §5.1.

`sdkconfig.defaults` **must** contain `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`. SPEC §3.2 asks
only for "console output routed to UART0" and does not mention this; it is the non-obvious
half of the requirement.

---

## 4. Hardware — VERIFIED 2026-08-09

The board was connected and every SPEC §2 assumption about the *board* was checked. All of
them hold. One assumption about the *bridge IC* was wrong.

### 4.1 Silicon — `esptool.py chip_id` / `flash_id`

```
Chip is ESP32-S3 (QFN56) (revision v0.2)
Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)
Crystal is 40MHz
MAC: 28:84:85:60:d3:84
Manufacturer: 68        (Boya)
Device: 4018
Detected flash size: 16MB
Flash type set in eFuse: quad (4 data lines)
Flash voltage set by eFuse to 3.3V
```

| SPEC §2 assumption | Verdict |
|---|---|
| Chip is ESP32-S3 | **CONFIRMED** — ESP32-S3 QFN56 rev v0.2 |
| 16 MB flash | **CONFIRMED** — 16 MB, Boya, eFuse quad @ 3.3 V |
| 8 MB PSRAM | **CONFIRMED** — embedded 8 MB, AP_3v3 |
| PSRAM is octal | **CONFIRMED at runtime** — see §4.2 |

The vendor's ESP32-C3 block diagram (SPEC §2.1) was indeed nonsense; the part is a genuine
N16R8. `CONFIG_ESPTOOLPY_FLASHMODE_QIO` in `sdkconfig.defaults` matches the eFuse setting.

### 4.2 PSRAM octal mode — confirmed from the boot log

Acceptance criterion §8.2. Boot log of the step-2 skeleton:

```
I (188) octal_psram: Readlatency  : 0x02 (10 cycles@Fixed)
I (193) octal_psram: DriveStrength: 0x00 (1/1)
I (197) MSPI Timing: Enter psram timing tuning
I (202) esp_psram: Found 8MB PSRAM device
I (205) esp_psram: Speed: 80MHz
I (644) esp_psram: SPI SRAM memory test OK
I (718) esp_psram: Adding pool of 8192K of PSRAM memory to heap allocator
I (725) spi_flash: detected chip: boya
I (727) spi_flash: flash io: qio
```

The `octal_psram` tag is the proof of OPI mode — the quad path logs under a different tag.
`CONFIG_SPIRAM_MODE_OCT=y` and `CONFIG_SPIRAM_SPEED_80M=y` are correct as written.

Consequence: **GPIO33–37 are consumed by PSRAM** and are unusable, exactly as SPEC §2.3 warns.

Firmware's own report, and free heap with the PSRAM pool added:

```
I (675) app_init: ESP-IDF:          5.5.3
I (769) bridge: flash: 16777216 bytes (16 MB)
I (773) bridge: psram: initialised, 8388608 bytes (8 MB)
I (11196) bridge: free heap: 8718680 bytes
```

### 4.3 USB connectors — one SPEC assumption **wrong**

| Connector | Enumerates as | VID:PID | Role |
|---|---|---|---|
| "COM"/"UART" | `/dev/cu.usbserial-A5069RR4` | `0403:6001` | **FTDI FT232R** — UART0, flashing, auto-reset |
| "USB" (native) | `/dev/cu.usbmodem1234561` | `303a:4001` | ESP32-S3 native USB |

**SPEC §2.1 assumed the bridge IC is a CH343 or CP2102N. It is neither — it is an FTDI
FT232R.** Practical consequences:

- The device node is `/dev/cu.usbserial-<FTDI serial>`, not `wchusbserial*`. The serial
  number is baked into the FT232R, so the node name is stable across reboots and USB ports —
  better than the CH343 case, and it can be hard-coded in `platformio.ini` (it now is).
- macOS has a built-in FTDI driver; no install needed.
- Auto-reset over DTR/RTS works. `pio run -t upload` flashed successfully with no button
  presses.

The `303a:4001` PID on the native port is **TinyUSB CDC**, not the `303a:1001` USB-Serial/JTAG
that SPEC §2.1 predicted. That is a property of the firmware that was on the board when it
arrived, not of the silicon: it shipped running the stock ESP-IDF TinyUSB console example,
which emits

```
\x1b[0;32mI (349688) example: log -> USB\x1b[0m\r\nexample: print -> stdout\r\n...
```

Because that example does not implement the ROM reset handshake, esptool could not connect
over the native port (`Invalid head of packet (0x1B)` — the 0x1B is the ESC of those colour
codes). Once this project's firmware is flashed and SPEC §5.3 puts the built-in USB
Serial/JTAG driver in charge, the native port will present as `303a:1001`. **Flash via the
FT232R "COM" port.**

### 4.4 Router — IDENTIFIED 2026-08-09

Read from the case label and the PCB silkscreen.

| Field | Value |
|---|---|
| Model | Sagemcom F@st 5670V2 (TIM LIVE, Brazil) |
| Manufacturer | Sagemcom Broadband SAS |
| Board ID | `GPON-BCM114-0010 REV1.0` — Broadcom xPON platform |
| NAND flash | Spansion `S34ML02G200TFI00`, 2 Gbit SLC = 256 MB |
| Management | `https://192.168.1.1`, user `admin` |
| Power | 12 VDC / 2 A |

Inferred, not verified: the SoC is a BCM68xx-class part under the heatsink, so the bootloader
is expected to be **CFE** rather than U-Boot. Whether CFE is locked is unknown, and matters —
SPEC's interactive-console goal assumes a console that accepts input.

Retired by the §4.5 investigation:

- **Console logic level: 3.3 V — measured.** J1's `TX` and `RX` pads both read 3.3 V against
  router GND with the router running. **No level shifter is required.** This closes the
  1.8 V risk that wiring.md carried as accepted-but-not-retired.
- **Which header, and which pin is which — verified.** See §4.5.

Still open:

- Router console baud rate (§2.4). The probe never had a live link to measure, so the
  candidate sweep in `main.c` has not yet produced an answer.
- Whether the vendor firmware leaves console RX enabled.
- Board revision / RGB LED on GPIO38 vs GPIO48 — not needed by any requirement; deferred.

### 4.5 Router link — **ROOT CAUSE FOUND: wired to UART0, not UART1**

The link was never an open circuit. The harness was landed on the ESP32 devkit pins
silkscreened **`TX`** and **`RX`**, which on this board are **UART0 — GPIO43 and GPIO44** —
not GPIO17/18. The firmware was watching two pins that genuinely had nothing attached, while
the router was connected all along to the ESP32's own console port.

The board's own boot log states the mapping on every reset:

```
I (652) cpu_start: GPIO 44 and 43 are used as console UART I/O pins
```

#### Why this took six tests to find

Every measurement was correct. They were measurements of different conductors.

| Evidence | Reading | What it actually meant |
|---|---|---|
| Firmware pin check | GPIO17/18 **FLOATING** | True — nothing was attached to those pins |
| Multimeter at J1 | `TX`/`RX` both **3.3 V** | True — the console header is alive and 3.3 V |
| Multimeter continuity | GND/TX/RX **all good** | True — along the harness, which was intact |
| Multimeter at the ESP32 | RX swinging 0↔3.3 V, TX steady 3.3 V | The router **was** reaching the ESP32 — via GPIO43/44 |
| Full 24-pin scan | **all float** | True, and decisive by omission — see below |

The scan (`scan_all_safe_pins()`) is what closed it. Finding *no* driven pin anywhere in the
safe set eliminated "the wire is on a mislabelled GPIO" and left only the pins the scan
cannot sample. GPIO43/44 are excluded by construction: they carry the log the scan reports
through. The fault was in the one blind spot, so the all-float branch now names it outright.

Physical confirmation: photographs show the harness terminating on the header row
`… 41 42 2 1 RX TX GND`, and the router's J1 populated on the other end.

#### Consequences that had to be checked

**The ESP-IDF boot log was being typed into the router's console.** GPIO43 is UART0's
transmitter and was wired to J1 pin 2, the router's RX. Every reset during diagnosis injected
the ROM banner and the full IDF log into a Broadcom CFE console. This is the exact hazard
wiring.md's "UART assignment" section exists to prevent. On a locked CFE this is noise; on an
unlocked one it can interrupt autoboot.

**Two push-pull outputs were contending on GPIO44.** The FT232R's transmitter and the
router's console TX were both driving that pin whenever the harness was attached.

Neither is a design fault — both are precisely what the pin budget forbids, and the wiring
simply did not match the documented plan.

#### What is now verified about J1

J1 is the debug UART, no longer inferred. Silkscreen reads `3V3 RX TX GND` with pin 1 at the
board edge. Labels are from the **router's** perspective, so the link must be crossed.

| J1 pin | Label | Direction | Connects to |
|---|---|---|---|
| 1 | `3V3` | supply | **nothing** — see wiring.md |
| 2 | `RX` | router input | ESP32 **GPIO17** (our TX) |
| 3 | `TX` | router output | ESP32 **GPIO18** (our RX) |
| 4 | `GND` | ground | ESP32 GND |

Two traps worth recording. The silkscreen only reads right-way-up with the board inverted,
and `3V3` sits at the opposite end from `GND`, so an off-by-one lands on the supply rail —
which also measures 3.3 V, so a multimeter will not catch it. And J1 ships as bare plated
holes with no pin header, so a ganged female housing cannot mate with it; the holes need
clearing and either direct-soldered wires or a 4-pin male header.

#### How to resolve

Move the two signal wires off `TX`/`RX` and onto the pins **numbered** `17` and `18` on the
opposite header, keeping the crossover in the table above. Then reset and watch
`pio device monitor`. The firmware reports the outcome itself: GPIO18 must read
`driven HIGH`, and the baud probe should lock onto the console. Power-cycle the router during
the capture to catch the CFE boot log.

The firmware is **RX-only on UART1** — no TX pin is assigned at all, so it cannot type into
the router. That guarantee covers UART1 only and is void under the miswiring above. The
256-value loopback test of SPEC §10.2 stays compiled out behind `BRIDGE_ENABLE_LOOPBACK_TEST`
and must only be enabled with the router disconnected and a GPIO17↔GPIO18 jumper fitted.

### 4.5 Router link — **OPEN CIRCUIT, unresolved**

No router data has been received. Three separate tests agree, and together they rule out the
benign explanations.

**Test 1 — passive baud probe, nominal orientation.** Zero bytes at every candidate rate:

```
I (3467)  bridge: probe 115200 baud: 0 bytes, 0% printable
I (6067)  bridge: probe  57600 baud: 0 bytes, 0% printable
I (8667)  bridge: probe  38400 baud: 0 bytes, 0% printable
I (11267) bridge: probe   9600 baud: 0 bytes, 0% printable
```

**Test 2 — 150 s continuous capture** across a requested router power-cycle: **0 bytes**.

**Test 3 — pin-level classification.** This is the decisive one. Each pin is sampled twice,
once with an internal pull-down and once with a pull-up. An external driver (a router TX
output idling high, even through a 470 Ω series resistor) overpowers the ~45 kΩ internal pull
and reads the same both ways; an unconnected pin follows whichever pull is enabled.

```
I (778) bridge: pin check GPIO18 (expected: router TX -> our RX): FLOATING — nothing is driving this pin
I (818) bridge: pin check GPIO17 (expected: our TX -> router RX): FLOATING — nothing is driving this pin
```

**Both pins float.** A powered router's console TX idles HIGH. Had GPIO18 been connected to a
live router TX, it would have read "driven HIGH" regardless of whether the router was saying
anything. It does not.

**Test 4 — swapped-orientation probe.** The firmware re-probes with RX on GPIO17 in case the
two signal wires are reversed at the header. Also zero bytes at every rate. So this is not a
TX/RX swap.

#### What this rules out

| Hypothesis | Status |
|---|---|
| Router booted and simply idle | **Ruled out** — an idle TX still holds the line HIGH |
| TX/RX reversed at the header | **Ruled out** — test 4 |
| Wrong baud rate | **Ruled out** — a wrong baud yields garbage bytes, not zero bytes |
| Router console TX at 1.8 V | **Unlikely** — 1.8 V is below the ESP32-S3's V_IH (≈0.75 × 3.3 V = 2.48 V) so it would read as a stuck-LOW line, not floating |

#### Remaining candidates, in order of likelihood

1. **The router is not powered.** Nothing else produces a floating line so cleanly.
2. **A wire is not making contact** — unsoldered, cold joint, or a connector not seated.
3. **The wires land on different ESP32 pins** than GPIO17/18.
4. **The header pins identified as console TX/RX are something else** — SPEC §2.4's
   pre-solder step ("confirm which header pin emits the boot log") was not carried out
   before soldering.
5. **GND is not connected.** This alone would not cause a float on a driven line, but it is
   cheap to re-check while probing the others.

#### How to resolve

The firmware now reports this automatically on every boot — reflash nothing, just watch
`pio device monitor`. With the router powered on, **GPIO18 must read "driven HIGH"**. Until
it does, no amount of firmware work will produce router data.

A multimeter between router GND and the header pin believed to be console TX should read
≈3.3 V with the router running. That measurement simultaneously answers SPEC §2.4's
unretired 3.3 V-vs-1.8 V question.

The FT232R is also available as an independent check: wire it to the same header pins and see
whether *it* sees a boot log. That isolates "router/header problem" from "ESP32 wiring
problem".

Note the firmware is deliberately **RX-only on UART1**. GPIO17 is wired to the router's
console RX, so anything transmitted is keystrokes; SPEC §9 warns that writing to a bootloader
console can brick the router. The 256-value loopback test of SPEC §10.2 is compiled out
behind `BRIDGE_ENABLE_LOOPBACK_TEST` and must only be enabled with the router disconnected
and a GPIO17↔GPIO18 jumper fitted.

### 4.6 Reproducing these checks

```sh
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py --port /dev/cu.usbserial-A5069RR4 chip_id
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py --port /dev/cu.usbserial-A5069RR4 flash_id
~/.platformio/penv/bin/pio run -t upload
~/.platformio/penv/bin/pio device monitor
```

---

## 5. Open decision for the project owner — IDF 5.5.3 vs 6.0.1

SPEC §0 mandates 5.5.x on the grounds that PlatformIO cannot do 6.x. That ground no longer
holds (§2.2). The choice is now a real one:

| | ESP-IDF 5.5.3 (platform `espressif32@6.13.0`) | ESP-IDF 6.0.1 (platform `espressif32@7.0.1`) |
|---|---|---|
| Already on disk | **yes** | no (~384 MB download) |
| Age of PlatformIO support | ~6 months, settled | ~3 months |
| Matches SPEC as written | yes | requires a §0 amendment |
| Upstream support horizon | shorter | longer |

Recommendation: **stay on 5.5.3 / platform 6.13.0**, pinned explicitly. Nothing in SPEC §5
needs an API introduced in 6.0, 5.5.3 is installed and proven, and this project's risk budget
belongs on the hardware, not the toolchain. SPEC §0's *conclusion* survives even though its
*reasoning* was stale.
