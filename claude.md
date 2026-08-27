# ESP32-S3 GPS Tracker (MC60)

## Hardware
- ESP32-S3, Quectel MC60 GSM/GPRS/GNSS module, SSD1306 128x64 OLED on I2C
- MC60 on hardware UART; OLED at 0x3C, no reset pin (-1)

## Build
PlatformIO / Arduino framework. `pio run -t upload`, monitor at 115200 baud.

## Architecture
State machine in main.cpp, driven from `loop()` via `currentState`.
`sendCommand()` is a blocking AT helper (busy-waits up to its timeout arg,
buffering bytes as they arrive) — used for request/response AT exchanges.
Transitions go through `enterState()`, which resets `stateTimer` and
`retryCounter` — never assign `currentState` directly.

Serial bytes are pumped into `mc60Buffer` at the top of `loop()` and also
inside `sendCommand()`'s wait loop (so an SMS URC arriving mid-transaction
isn't lost). `checkForSMS()` only parses `+CMT:` URCs out of that buffer and
enqueues authorized ones onto `smsQueue` — it never dispatches directly, and
runs re-entrantly (called from inside `sendCommand()`), so it must stay
side-effect-free beyond parsing. `processSmsCommand()` drains that queue and
executes LOC/STATUS/PWROFF — called only from the top level of `loop()`,
never from inside `sendCommand()`, so it can't re-enter the AT engine
mid-transaction.

`STATE_FATAL_ERROR` is not a true dead end: after `FATAL_ERROR_RESTART_MS`
with no recovery it calls `ESP.restart()`. `STATE_RUNNING` tracks
`consecutivePublishFailures`; past `MAX_PUBLISH_FAILURES` it closes and
reopens the MQTT connection (`STATE_MQTT_OPEN`) rather than silently
publishing into a dead socket forever.

## Rules
- No `delay()` inside state handlers. It blocks SMS reception. (`STATE_POWER_ON`'s
  PWRKEY pulse and `STATE_WAIT_GPS_FIX` are both timer-based for this reason.)
- Retry counters reset on every `enterState()` call — they must not leak across
  states. Only increment `retryCounter` in "stay in this state and retry"
  branches, which must NOT call `enterState()`. A transition that changes state
  on every attempt (e.g. the old POWER_ON<->WAIT_FOR_BOOT boot retry) needs its
  own dedicated counter (`bootRetryCounter`) instead, since `enterState()` would
  otherwise reset it every cycle.
- NMEA parsing splits commas manually (preserving empty fields). Do not use
  `strtok` — it collapses consecutive commas and shifts field indices on weak fixes.
- `src/credentials.h` is gitignored. Never print its values or write them into
  committed files.
- Adafruit IO JSON payload requires a `"value"` key for the map dashboard block.