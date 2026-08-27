// main/credentials.example.h
#pragma once

#define IO_USERNAME        "your_adafruit_username"
#define IO_KEY             "aio_XXXXXXXXXXXXXXXX"
#define FEED_NAME          "gps-location"

#define SIM_APN            "internet"

// Full command access, including PWROFF/SETINTERVAL.
#define AUTHORIZED_NUMBERS { "+90XXXXXXXXXX" }

// Can query location/status (LOC, STATUS, HELP) but not issue commands that
// change device behavior or power state. Leave empty ({ }) if you don't
// want a viewer tier.
#define VIEWER_NUMBERS     { }