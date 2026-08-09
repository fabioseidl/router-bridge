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

Raw TCP, no telnet option negotiation. Unplugging USB does not disturb this session. Up to
four clients may attach at once; a fifth is refused with `router-bridge: too many clients`.

Find the IP with `wifi status` on the control channel. `TCP_NODELAY` is set on every accepted
socket, so single keystrokes leave immediately rather than waiting for Nagle.

Stock `telnet` mostly works, but it can inject IAC sequences into what is meant to be a
transparent pipe. `nc` is the documented client. If you must use `telnet`, the IAC-escaping
exception of SPEC §5.4.1 exists but is off by default.

### SoftAP fallback

The access point the bridge associates with is usually *the router under test*. When that
router wedges — which is exactly when you need its console — the STA link dies with it. So
after 30 seconds without an association the ESP32 raises its own AP and serves the same
console there:

| | |
|---|---|
| AP address | **`192.168.4.1`** (fixed) |
| Console | `nc 192.168.4.1 23` |
| Security | WPA2 always; an open AP is refused |

It returns to STA on its own once the configured network reappears — you are never stranded
in AP mode. Turn the whole behaviour off with `ap off` if a second SSID broadcasting is
unwelcome.

Set the credentials before you need them:

```
bridge> ap set rescue-console <passphrase>   # 8 chars minimum, WPA2
bridge> wifi set <ssid> <pass>
bridge> wifi status
```

## Control channel

The out-of-band configuration interface, on the **"COM"** port at 115200. It is deliberately
not on the data path: SPEC §5.1 forbids an in-band escape sequence, since any reserved
attention byte would corrupt a binary transfer.

```sh
pio device monitor      # or: screen /dev/cu.usbserial-A5069RR4 115200
```

You get a `bridge>` prompt. `help` lists everything.

| Command | Effect |
|---|---|
| `baud <n>` | Router link baud, applied live and persisted |
| `line <8N1>` | Data bits, parity, stop bits — same grammar as the argument |
| `wifi set <ssid> <pass>` | STA credentials, then reassociate |
| `wifi status` | State, SSID, IP, RSSI, disconnect count |
| `ap set <ssid> <pass>` | SoftAP credentials (WPA2, 8+ chars) |
| `ap on` / `ap off` | Enable or disable the fallback |
| `stats` | Byte counts, overflow and drop counters, Wi-Fi, uptime, memory |
| `break` | Send a UART break — interrupts some bootloaders |
| `reset` | Soft-reset **the ESP32**, never the router |
| `log dump` / `log clear` | The 64 KB PSRAM scrollback of router output |

Credentials are never echoed back and never written to the log.

`stats` is the acceptance check for SPEC §8.9: `uart overflow` must read `0` across a full
router boot.

### Reading what scrolled past

`log dump` replays the most recent 64 KB of router output from PSRAM. Useful when the router
booted before you attached — the ring-log is filled by the same fan-out that feeds Paths A
and B, so it captures output nobody was watching.

## Two USB cables

Path A and the control channel are **different physical ports**. To use both at once, plug a
cable into each USB-C connector. With only the "COM" cable connected you get diagnostics and
configuration but no router console; with only the "USB" cable you get the console but no way
to configure it.
