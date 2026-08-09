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

**SPEC §10 steps 1–6 implemented.** The firmware builds warning-free, boots, and brings up
both console paths. What remains is bench validation that needs hardware I cannot drive
remotely — see "What has not been proven" below.

| Step | State |
|---|---|
| 1. Verify hardware and toolchain | **Done** — [docs/hardware.md](docs/hardware.md) |
| 2. Skeleton that builds and flashes | Skeleton done; **loopback self-test never run** |
| 3. Path A: USB CDC ↔ UART1 | **Implemented**, unvalidated |
| 4. NVS config + `esp_console` control channel | **Done and exercised on hardware** |
| 5. Wi-Fi STA + TCP server + SoftAP fallback | **Implemented**, unvalidated |
| 6. PSRAM ring-log and `stats` | **Done** — 64 KB in PSRAM, `stats` verified |
| 7. Documentation and acceptance pass | Docs done; **acceptance pass outstanding** |

Boot output on the current firmware:

```
main: psram: initialised, 8388608 bytes (8 MB)
main: pin check GPIO18 (expected: router TX -> our RX): driven HIGH — connected and alive
bridge: uart1 LIVE: tx=GPIO17 rx=GPIO18, 115200 baud 8N1
bridge: ringlog: 65536 bytes in PSRAM
usb: Path A up: USB Serial/JTAG <-> UART1
tcp: Path B up: listening on :23
console: control channel on UART0 — type 'help'
```

### What has not been proven

Everything below needs someone at the bench. None of it is a known defect — it is untested.

- **The UART1 loopback self-test (SPEC §10.2) has never run.** It is compiled out behind
  `BRIDGE_ENABLE_LOOPBACK_TEST` and needs the router disconnected and GPIO17 jumpered to
  GPIO18. This is the check that proves the transport is 8-bit clean before the TX path is
  ever pointed at a live console — acceptance criterion §8.5.
- **Path A has never carried a byte.** The native USB port had no cable attached during
  development, so `/dev/cu.usbmodem*` never enumerated.
- **Path B has never carried a byte.** No Wi-Fi credentials have been set on the device.
- **Writing to the router is untested**, and it is not yet known whether the vendor firmware
  even leaves console RX enabled.
- **No router boot log has been captured**, so whether the Broadcom CFE bootloader is locked
  is still unknown. That is the single fact most likely to change what this tool can do.

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
pio device monitor     # UART0 — diagnostics and the `bridge>` control channel
```

Then, with a cable in the **native** USB-C port:

```sh
screen /dev/cu.usbmodem* 115200        # Path A — the router console
```

And over the network, once `wifi set <ssid> <pass>` has been run on the control channel:

```sh
nc <esp-ip> 23                         # Path B — the same console
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
  will faithfully transmit destructive commands. **UART1 is now bidirectional** — the step-2
  skeleton's receive-only guarantee is gone by design, because SPEC §5.1 requires a two-way
  pipe. Every byte arriving from USB or TCP is a keystroke to the router.
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
