# macOS usage

Verified on this machine 2026-08-09. Device node names below are **real**, not examples —
see [hardware.md](hardware.md) §4.3.

## The two ports, and which is which

| Port | Node | Carries |
|---|---|---|
| "COM" USB-C (FTDI FT232R) | `/dev/cu.usbserial-A5069RR4` | ESP32 diagnostics, firmware upload, control channel. **Not router data.** |
| "USB" USB-C (native) | `/dev/cu.usbmodem*` | **Path A — the router console.** |

The FT232R's serial number is baked into the chip, so `usbserial-A5069RR4` is stable across
reboots and across which physical USB port you use.

Always use `/dev/cu.*`, **never** `/dev/tty.*` — on macOS the `tty.*` node blocks on DCD and
will appear to hang.

## PlatformIO is not on `PATH`

```sh
alias pio=~/.platformio/penv/bin/pio
```

## Build / flash / monitor

```sh
pio run                # build
pio run -t upload      # flash via the FT232R "COM" port; auto-reset, no buttons needed
pio run -t menuconfig  # Kconfig UI (NOT idf.py menuconfig)
pio device monitor     # UART0 diagnostics only — NOT the router console
pio run -t clean
```

Flashing resets the ESP32 and therefore **interrupts the bridge**. Any router console session
in progress will drop.

## Path A — router console over USB

Open the **native** USB port with an ordinary terminal, not `pio device monitor`:

```sh
screen /dev/cu.usbmodem* 115200
# or
picocom -b 115200 /dev/cu.usbmodem*
```

The baud you give the Mac-side terminal is **irrelevant** to the router link. USB CDC has no
real baud rate; the router's actual baud is set on the ESP32 through the control channel
(`baud <n>`).

### Getting out of `screen`

`Ctrl-A` then `K`, then `y`. Or `Ctrl-A` then `\`, then `y`.

Do **not** just close the terminal window — that leaves a stale lock and the port appears
busy on the next attempt. If that happens:

```sh
screen -ls              # find the dead session
screen -X -S <id> quit  # kill it
```

## Path B — router console over Wi-Fi

```sh
nc <esp-ip> 23
```

Raw TCP, no telnet option negotiation. Unplugging USB does not disturb this session.

## Status of this document

Paths A and B are **not implemented yet** — the repository currently contains the step-2
skeleton (see the README). The commands above describe the intended finished behaviour and
the verified port names; they will work once SPEC §10 steps 3 and 5 are complete.
