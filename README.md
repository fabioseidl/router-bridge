# ESP32-S3 Router Serial Console Bridge

Bridges a router's UART debug console to a MacBook two ways:

- **Path A (primary):** USB-C to the ESP32-S3's native USB. A serial terminal on macOS
  behaves as if wired directly to the router console.
- **Path B:** Wi-Fi. The ESP32-S3 exposes the same console on a raw TCP socket, so the
  console is reachable with no USB cable attached.

Path B is the whole reason this is an ESP32 rather than a $3 CP2102 adapter.

Scope is an **interactive shell / bootloader console** (U-Boot, OpenWrt, vendor CLI) — not
firmware transfer, not automated log harvesting. The transport is still 8-bit clean, because
that costs nothing and protects ANSI escapes and line editing.

## Current status

**SPEC §10 step 2 of 7.** The repository builds and flashes, and both the ESP32 and the
router are fully characterised. The bridge itself is not implemented.

| Step | State |
|---|---|
| 1. Verify hardware and toolchain | **Done** — [docs/hardware.md](docs/hardware.md) |
| 2. Skeleton that builds and flashes | **Done**; router link diagnosed, rewire pending (below) |
| 3. Path A: USB CDC ↔ UART1 | Not started |
| 4. NVS config + `esp_console` control channel | Not started |
| 5. Wi-Fi STA + TCP server | Not started |
| 6. PSRAM ring-log and `stats` | Not started |
| 7. Documentation and acceptance pass | Not started |

What the current firmware does: reports chip/flash/PSRAM, classifies the idle state of
GPIO17/18, scans every safe GPIO to find which pin the router is actually driving, passively
probes the router link at 115200/57600/38400/9600 in both TX/RX orientations, then streams
anything it receives to the UART0 log.

### Router link: working

Console captured at **115200 8N1** on GPIO18, with live Linux kernel output from the router:

```
pin check GPIO18 (expected: router TX -> our RX): driven HIGH — connected and alive
probe 115200 baud: 58 bytes, 100% printable
rx: cfg80211: Calling CRDA to update world regulatory domain\r\n
GPIO18: best guess 115200 baud (100% printable)
```

Router is a **Sagemcom F@st 5670V2** (Broadcom GPON, board `GPON-BCM114-0010 REV1.0`),
console header J1 at **3.3 V** — no level shifter needed.

The link spent a day looking like an open circuit because the harness was landed on the devkit
pins silkscreened **`TX`**/**`RX`** — which on this board are **UART0, GPIO43/44**, the ESP32's
own console port. Moving the two wires to the pins *numbered* 17 and 18 fixed it with no
firmware change. The full evidence chain is in
[docs/hardware.md §4.5](docs/hardware.md); the trap is called out in
[docs/wiring.md](docs/wiring.md).

## Verified hardware

ESP32-S3-WROOM-1 **N16R8**, confirmed by `esptool` and the boot log — not taken from the
silkscreen, which on this board vendor's documentation is unreliable.

- ESP32-S3 QFN56 rev v0.2, MAC `28:84:85:60:d3:84`
- 16 MB Boya flash, QIO, 3.3 V (eFuse)
- 8 MB PSRAM in **octal** mode at 80 MHz (`octal_psram` driver active in the boot log)
- "COM" USB-C is an **FTDI FT232R** (`0403:6001`) — *not* the CH343/CP2102N the spec assumed

## Quick start

```sh
alias pio=~/.platformio/penv/bin/pio   # pio is not on PATH on this machine

pio run                # build
pio run -t upload      # flash via the FT232R "COM" port (auto-reset, no buttons)
pio device monitor     # UART0 diagnostics — NOT the router console
```

The two port paths in `platformio.ini` are already filled in with this machine's real device
nodes. On a different machine, change them. Full instructions: [docs/usage.md](docs/usage.md).
Wiring as built: [docs/wiring.md](docs/wiring.md).

## Toolchain

PlatformIO Core 6.1.19, `platform = espressif32@6.13.0`, `framework = espidf`, **ESP-IDF
5.5.3**, C, no Arduino.

The platform version is pinned deliberately: 6.13.0 is the last release bundling IDF 5.5.x.
Platform 7.0.x moved to IDF 6.0.x — contrary to the project spec, PlatformIO *does* now
support IDF 6.x. The reasoning behind staying on 5.5.3 anyway is in
[docs/hardware.md §5](docs/hardware.md).

## Risks and failure modes

- **Wrong TX/RX identification** — harmless; nothing appears. Swap and retry. The firmware
  now detects this automatically.
- **Wiring the router to the pins silkscreened `TX`/`RX`** — those are UART0 (GPIO43/44), not
  the router link. The ESP32 then types its own boot log into the router's console on every
  reset, and the FT232R contends with the router's TX driver. **This already happened once**
  — see [docs/hardware.md §4.5](docs/hardware.md). The firmware's read-only guarantee covers
  UART1 only and does not protect against it.
- **Router console at 1.8 V logic** — **retired.** Measured at 3.3 V on J1. No level shifter
  required.
- **Soldering to a live board** — power down and discharge first.
- **Writing to a bootloader console can brick the router.** The bridge is transparent and
  will faithfully transmit destructive commands. The current skeleton is deliberately
  receive-only, with no TX pin assigned at all.
- **Some vendor firmware disables console RX.** Read-only behaviour is a router limitation,
  not a bridge bug.
- **ESP32 reset while attached** — UART1 pins float briefly during boot. Series resistors
  limit any transient injected into the router's RX.
- **3.3 V is deliberately not connected** between the boards. Only GND and the two signal
  lines. See [docs/wiring.md](docs/wiring.md) for why.
- **PlatformIO's IDF package is community-maintained and lags upstream.** If a needed fix
  ever exists only in a newer IDF, decide deliberately between moving platform versions and
  leaving PlatformIO — do not pin `framework-espidf` to an unsupported external path.

## Non-goals (v1)

No control of the router's reset or bootstrap pins. No web UI, no MQTT, no OTA. No RFC 2217.
No modification of, or dependency on, the router's firmware.
