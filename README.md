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
- Replies go back to the *sender*, not to a hardcoded number
- Automatic recovery: per-state retry counters, MC60 power-cycle and FSM
  restart instead of a dead halt state

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
| `AUTHORIZED_NUMBERS` | Numbers allowed to issue SMS commands |

`main/credentials.h` is gitignored.

## SMS commands

| Command | Effect |
|---|---|
| `LOC` / `GPS` / `GETGPS` | Replies with the current GPS fix as a Google Maps link |
| `STATUS` | Replies with the current state-machine state and uptime |
| `PWROFF` | Pulses PWRKEY to power off the MC60 and returns the state machine to `STATE_POWER_ON` |

Commands arriving mid-transaction are queued and executed from the top level
of the main loop, never from inside the AT engine.

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
`process_sms_command()` drains that queue and executes `LOC` / `STATUS` /
`PWROFF` — called only from the top level of the main loop, so it can't
re-enter the AT engine mid-transaction.

NMEA fields are split manually rather than with `strtok`, because `strtok`
collapses consecutive commas and GGA is full of empty fields on a weak fix.

## Known quirks

- Incoming SMS can carry UDH control-character prefixes, so command matching
  strips non-printables before comparing.
- The symbol `ctx` collides with `libnet80211` at link time — hence `g_ctx`.

## License

MIT — see [LICENSE](LICENSE).
