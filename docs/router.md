# Router hardware — Sagemcom F@st 5670V2

The device under test. Compiled 2026-08-09.

Every row carries how it was established. **Confirmed** means read directly off the case
label, the PCB silkscreen, a component marking, or measured electrically. **Inferred** means
reasoned from those observations. **Unknown** means nobody has looked, or it cannot be seen
without disassembly.

Nothing here came from the router's own firmware — see "Why this is incomplete" below.

## Identity — confirmed, case label

| Field | Value |
|---|---|
| Model | F@st 5670V2 |
| Manufacturer | Sagemcom Broadband SAS |
| Part number | 253977461 |
| SAP | TM04014786 |
| Power input | 12 VDC / 2 A |
| Laser class | Class 1 |
| Anatel homologation | 06803-22-05950 |
| Origin | Manufactured in Manaus, Brazil |
| Management URL | `https://192.168.1.1`, user `admin` |

Serial, PON ID and MAC are on the label and deliberately omitted — they identify the
individual unit.

## Board — confirmed, silkscreen and component markings

| Item | Marking | Meaning |
|---|---|---|
| Board ID | `GPON-BCM114-0010 REV1.0` | "BCM" confirms a Broadcom platform |
| PCB spec | FCP-4 94V-0, UL E232205, date 22-H | FR-4, manufactured 2022 |
| NAND flash | `S34ML02G200TFI00` Spansion, lot 224 | 2 Gbit SLC = **256 MB**, TSOP-48 |
| Telephony SLIC | Silicon Labs `Si32285`, lot 2271 | Dual ProSLIC → 2 FXS ports |
| LAN magnetics | `UTV24C01`, `TG1153iDH` | Discrete magnetics modules |
| Optical subassembly | Shielded can, PCB area P1C30 | GPON BOSA under the shield |
| Backup power | Large radial capacitor at P1C4 | Likely FXS ring or dying-gasp |

### Ports counted on the board

- 1× GPON SC/APC (green connector)
- 4× RJ45 in a single yellow gang block
- 1× RJ45 in a separate red housing — likely the 2.5G port
- 2× RJ11 (white), matching the dual SLIC
- 1× USB Type-A

The Telekom-branded 5670v2 manual lists 4× GbE, 1× 2.5G, 2× USB and 2 phone ports. This board
has only **one** USB, so port count is per-carrier, not per-model. Treat the TIM variant as
its own thing.

## Debug console — confirmed, measured

This is the part this project established directly. Full evidence chain in
[hardware.md §4.5](hardware.md).

| Property | Value | How |
|---|---|---|
| Header | **J1**, 4-pin, bare plated holes (no header fitted) | Silkscreen `3V3 RX TX GND`, pin 1 at board edge |
| Logic level | **3.3 V** | Multimeter, J1 pads against router GND, router running |
| Baud | **115200 8N1** | Passive sweep: 100% printable at 115200 vs 21% at 57600, 61% at 38400 |
| Console output | **Enabled** | Live kernel output captured (`cfg80211: Calling CRDA to update world regulatory domain`) |
| Console input | **Enabled** | Keystrokes echo back with ONLCR translation (`\n` → `\r\n`) |
| Shell | **None attached** | Commands echo verbatim, then nothing — no prompt, no `login:`, no error |

Labels on J1 are from the **router's** perspective, so the link to the ESP32 is crossed:
J1 `TX` → GPIO18, J1 `RX` → GPIO17, J1 `GND` → GND, J1 `3V3` left disconnected.

The echoing-but-not-executing behaviour is consistent with TIM's firmware locking the console:
the UART layer accepts and echoes input, but no getty sits behind it. This retires SPEC §9's
"some vendor firmware disables console RX" — input is accepted here; it simply goes nowhere.

## Software — what the console actually revealed

The only firmware evidence obtained so far is two lines the router emitted spontaneously on
its console, captured during the link bring-up:

```
cfg80211: Calling CRDA to update world regulatory domain
cfg80211: Exceeded CRDA call max attempts. Not calling CRDA
```

Thin, but not nothing. What follows from them:

| Finding | Confidence | Reasoning |
|---|---|---|
| **Linux**, not a proprietary RTOS | Confirmed | `cfg80211` is the Linux kernel wireless configuration subsystem. Nothing else prints that. |
| Full `mac80211`/`cfg80211` wireless stack | Confirmed | The message originates inside it |
| Kernel console output is **enabled** in production firmware | Confirmed | These lines arrived unprompted, with nobody attached |
| Regulatory handled by **CRDA**, the userspace agent | Confirmed | The kernel would not call CRDA if it were using the in-kernel regulatory database |
| Kernel predates ~4.15, likely a Broadcom SDK 4.1.x | Inferred | CRDA was the mechanism before in-kernel regdb replaced it; Broadcom xPON SDKs of this board's era ship kernels in that range |
| **CRDA is absent or unreachable** in the installed firmware | Confirmed | "Exceeded CRDA call max attempts" is the kernel giving up after repeated failures |
| Wireless regulatory domain stays at world (`00`) | Inferred | The direct consequence of the line above — a conservative channel and transmit-power set rather than the Brazilian domain |
| A tty layer with ONLCR is attached to the console | Confirmed | Typed `\n` returns as `\r\n`, which is line-discipline behaviour, not a wire echo |
| No getty / login shell on the console | Confirmed | Commands echo in full, then nothing — no prompt, no error, no `login:` |

That last pair is the interesting combination: the console has a live terminal line
discipline but nothing reading from it. Input is consumed and discarded.

### Software still unknown

Everything that matters operationally, in other words:

- Firmware version, build date, and TIM branding revision
- Exact kernel version and architecture
- CFE bootloader version, and whether it is locked
- Init system, userspace, busybox version
- Flash partition layout and which partition is active
- Whether a dual-image / failsafe scheme is in use
- Installed services, open ports, TR-069 ACS configuration

## Inferred, not verified

| Item | Inference | Basis |
|---|---|---|
| SoC | Broadcom xPON, BCM68xx / BCM6855-class | Board name plus Broadcom PON platform; the part is under the large heatsink |
| Bootloader | **CFE**, not U-Boot | Follows from a Broadcom CPE platform |
| Wi-Fi | Two separate radios, 2.4 GHz + 5 GHz | Two independent square heatsinks, each with its own antenna feed group |
| 2.4 GHz chain | 2×2 | Antenna pads labelled `2.4G ANT1`–`ANT2` |
| 5 GHz chain | 4×4 | Antenna pads labelled `5G ANT1`–`ANT4` |
| Power tree | Multiple buck rails | 4R7, 2R2, 1R5, R68, 150 inductors — typical multi-rail CPE design |

## Unknown

- Exact SoC part number, core architecture and clock — under the heatsink
- RAM size and type — the DRAM was not identifiable in the photographs
- Wi-Fi generation, Wi-Fi 5 vs Wi-Fi 6 — not on the label, radio chips are covered
- Firmware version and build
- CFE version, and whether the bootloader is locked
- Flash partition layout

## Why this is incomplete

Everything above comes from the outside of the device. The router's own account of itself —
`/proc/cpuinfo`, `/proc/meminfo`, `/proc/mtd`, `/proc/cmdline`, `nvram show`, `uname -a` — has
never been read, because there are only two ways in and both are currently shut:

1. **The serial console is locked.** Input is accepted and echoed but no shell responds.
   Getting past it would mean guessing credentials, which is out of scope.
2. **The web UI is unreachable from the development machine.** `https://192.168.1.1` resolves
   to a *different* device from this Mac's network — a Zyxel unit upstream, not this Sagemcom.
   The Sagemcom's management address lives on its own LAN.

### The two ways to finish this

- **Capture the boot log.** Power-cycle the router with a capture running; the 64 KB PSRAM
  scrollback holds the whole sequence. That yields the SoC, RAM size, flash layout, kernel
  version, CFE version, the kernel command line, and whether a getty ever spawns. Read-only,
  no risk, and it is the single highest-value measurement still outstanding.
- **Reach the web UI.** Join a machine to the router's own LAN and read `192.168.1.1` with the
  label credentials. That gives firmware version and configuration, though not the SoC detail
  a boot log would.
