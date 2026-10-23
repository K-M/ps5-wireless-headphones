# Troubleshooting

Read the receiver's log on its UART TX pin (GPIO43, 115200 baud) with any USB-serial adapter. An Arduino Uno works: tie the Uno's RESET to GND, Uno pin 1 → Nano GPIO43, GND → GND. A status line prints every 5 s:

```
up 95s | rssi -24 dBm | pkts 1668 | played 1668 backup 0 lost 0 late 0 | drift -0/+0 | buffer 11.7 ms | dongle on vol 100 | usb writes ok 94549 err 0
```

| Field | Healthy | Meaning |
|---|---|---|
| `pkts` | ~1667 per 5 s | Packets received (333/s) |
| `backup` | 0–few | Frames rebuilt from the backup copy (inaudible) |
| `lost` / `late` | 0 | Frames concealed → audible tick |
| `drift` | mostly 0, one direction only | Clock-drift corrections |
| `buffer` | ~11–12 ms | Jitter-buffer depth |
| `usb writes err` | 0 | Dongle write failures |

## Symptoms

| Symptom | Cause | Fix |
|---|---|---|
| PS5 doesn't list *USB Headphones* | Transmitter not flashed / still in download mode | Re-flash, then unplug and re-plug |
| Total silence, `dongle` not `on` | Dongle has no power | Power the Nano's **VBUS** pin (not VIN); check continuity VBUS → OTG adapter VBUS |
| `EP MPS (192) exceeds supported limit (128)` in the log | USB host FIFO too small | Already fixed in `sdkconfig.defaults` (`CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT=y`); rebuild from clean |
| Receiver reboots, `Brownout detector was triggered` | Supply below ~4.5 V at the Nano | Check the boost output under load (5.0–5.2 V); fit 470 µF at the Nano's VBUS and 100–220 µF at the boost input; charge the battery |
| `pkts 0` | Channel mismatch, or transmitter not streaming | Flash both boards with the same channel; play something on the PS5 |
| Ticks/dropouts, `lost` and `late` rising in bursts | Busy Wi-Fi channel | Re-flash both boards on another channel (13 is usually quietest outside the US) |
| Boost output ≈ battery voltage | Converter dead or not switching | Replace it. Two cheap modules (an MT3608 and a PowerBoost 500C) failed this way during this build |
| TP4056 / HW-107 gets hot | Charging at stock 1 A | Change R_PROG to 3 kΩ (400 mA) |
| Doesn't charge | Switch is ON | By design, USB-C only reaches the charger with the switch OFF |
| Volume too low even at PS5 max | PS5 USB output is quiet | Turn the knob past 100 to use the digital boost (up to +12 dB) |
| Knob changes volume the wrong way | Encoder A/B swapped | Swap the D5/D6 wires, or set `ENC_REVERSE 1` in `rx/main/main.c` |
| Knob skips or needs two clicks per step | Encoder resolution differs | Change `ENC_COUNTS_PER_DETENT` (4 for most EC11/KY-040; try 2) |
| Clicks with every `lost` count, worse after moving the receiver | Channel congestion, not range | Change channel before changing buffer depth; deeper buffers add latency and didn't fix congestion in testing |
