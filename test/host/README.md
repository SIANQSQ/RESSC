# Host regression tests

Run from `ESP Step` with MinGW g++ and its DLL directory on PATH:

```powershell
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
g++ -std=c++17 -Wall -Wextra -Werror -Itest/host -Iinclude test/host/test_motion.cpp -o .pio/test_motion.exe
& ./.pio/test_motion.exe
python examples/http_client.py --help
```

Compiles the actual firmware/HTTP handlers against deterministic Arduino,
FreeRTOS, Wi-Fi and WebServer mocks. Covers four-axis absolute mm positioning,
counts/directions/pulse widths, repeated targets, reverse travel, rounding,
per-axis limits and group atomicity, STOP isolation, legacy-command limit
enforcement, PPR invalidation/manual ZERO, microsecond wraparound, startup
manual origin, all-axis 10 mm forward/return distance, 32000 pulses per leg, acceleration, and zero return, self-test command exclusion, STOP cancellation,
self-test timeout failure, reassigned pins and absence of GPIO13..15 setup,
per-pulse guards, GET validation/results, snapshot mm fields/nulls/staleness,
stalled networking, queue saturation/backpressure and allocation failures.

The mocks do not emulate actual ESP32 scheduling, RF, GPIO electrical levels,
driver behavior or lost steps. Validate these on hardware.
