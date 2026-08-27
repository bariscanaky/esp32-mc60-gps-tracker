# ESP32-MC60 GPS Tracker

Custom-PCB GPS tracker built around an ESP32-S3-WROOM and a Quectel MC60
GSM/GPRS/GNSS module. It publishes its position to Adafruit IO over MQTT via
GPRS, and accepts SMS commands from an allowlist of authorized numbers.

Written in C on native ESP-IDF (no Arduino framework, no PlatformIO).

> Third-year Mechatronics Engineering project. The firmware is a non-blocking
> state machine — no `delay()` in the main path, so incoming SMS is still
> handled while the module is negotiating the network.

## Features

- GNSS fix acquisition from the MC60 (NMEA GGA/RMC parsing)
- Location published to an Adafruit IO feed over MQTT (`AT+QMTOPEN` /
  `AT+QMTCONN` / `AT+QMTPUB`), with a `value` key so the dashboard map block
  renders correctly
- SMS command interface with sender extraction from the `+CMT:` URC and a
  last-10-digits allowlist (so `+90…`, `0…` and bare formats all match)
- Two-tier SMS authorization: admin numbers get the full command set,
  viewer numbers can query location/status only
- Replies go back to the *sender*, not to a hardcoded number
- Automatic recovery: per-state retry counters, MC60 power-cycle and FSM
  restart instead of a dead halt state
- Last known GPS fix and a per-reset-reason boot counter persist in NVS, so
  a `LOC` request right after a reboot can fall back to the last-known
  position, and `STATUS` reports brownout/panic/software reset history

## Hardware

| Part | Notes |
|---|---|
| ESP32-S3-WROOM | Custom board, not a devkit. Flashed over UART0. |
| Quectel MC60 | GSM/GPRS/GNSS. Hardware UART. PWRKEY pulse to boot; needs ~5 s before it answers AT. |
| Power | Li-ion. GSM TX bursts pull hard — a marginal cell will brown the module out. |

<!-- TODO: pin table + a photo of the board here. Bir kart fotoğrafı README'yi
     bir anda ciddi gösteriyor, mutlaka ekle. -->

## PCB design files

Schematic and PCB layout live in [`hardware/esp32v4/`](hardware/esp32v4/) as
an Altium Designer project (`.PrjPcb` / `.SchDoc` / `.PcbDoc`, plus the custom
schematic/PCB libraries the design depends on). Open `esp32v4.PrjPcb` in
Altium Designer to view or edit.

## Build

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

## Configuration

Credentials are **not** in the repository. Copy the template and fill it in:

```bash
cp main/credentials.example.h main/credentials.h
```

| Macro | Meaning |
|---|---|
| `IO_USERNAME` / `IO_KEY` | Adafruit IO account and key |
| `FEED_NAME` | Target feed |
| `SIM_APN` | Carrier APN |
| `AUTHORIZED_NUMBERS` | Admin numbers — full command access |
| `VIEWER_NUMBERS` | Viewer numbers — `LOC`/`STATUS`/`HELP` only. Leave `{ }` for none. |

`main/credentials.h` is gitignored.

## SMS commands

| Command | Tier | Effect |
|---|---|---|
| `LOC` / `GPS` / `GETGPS` | admin, viewer | Replies with the current GPS fix as a Google Maps link, or the last-known persisted fix if there's no live one |
| `STATUS` | admin, viewer | Replies with state, uptime, current upload interval, and NVS-persisted reset counts |
| `HELP` | admin, viewer | Lists available commands (admin sees the full set) |
| `SETINTERVAL <seconds>` | admin | Changes the Adafruit IO upload interval for this boot (10–3600s); not persisted across reboots |
| `PWROFF` | admin | Pulses PWRKEY to power off the MC60 and returns the state machine to `STATE_POWER_ON` |

Commands arriving mid-transaction are queued (along with the sender's
authorization tier) and executed from the top level of the main loop, never
from inside the AT engine.

## Architecture

A state machine in `main.c`, driven from `app_main()`'s loop via
`current_state`. Transitions go through `enter_state()`, which resets the
per-state retry counter — state handlers never assign `current_state`
directly.

`mc60_send_command()` (in `mc60.c`) is the blocking AT helper: it waits its
full timeout argument, buffering incoming bytes into a rolling buffer as they
arrive so an SMS URC that lands mid-transaction isn't lost. `mc60_pump()`
parses `+CMT:` URCs out of that buffer and queues authorized senders'
commands onto an SMS queue; it never dispatches directly, since it also runs
re-entrantly from inside `mc60_send_command()`'s wait loop. `main.c`'s
`process_sms_command()` drains that queue and executes the commands above,
gating admin-only ones on the sender's role — called only from the top
level of the main loop, so it can't re-enter the AT engine mid-transaction.

NMEA fields are split manually rather than with `strtok`, because `strtok`
collapses consecutive commas and GGA is full of empty fields on a weak fix.

`storage.c` wraps NVS: it persists the last successful GPS fix (overwritten
on every read) and a running count of reset reasons (brownout/panic/software
restart), initialized once from `app_main()`. Both are best-effort — if NVS
fails to open, every `storage_*` call becomes a no-op/false for that boot
rather than halting the tracker.

## Known quirks

- Incoming SMS can carry UDH control-character prefixes, so command matching
  strips non-printables before comparing.
- The symbol `ctx` collides with `libnet80211` at link time — hence `g_ctx`.

## License

MIT — see [LICENSE](LICENSE).
