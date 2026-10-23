# Latency measurement

This kit measures how much latency the wireless link adds. It records one click as it arrives through two paths, and gets the time difference from one phone recording.

Requires Python 3 and numpy. A Mac is needed for the Multi-Output Device, or use any host that can play to two outputs at once.

## Steps

1. **Generate the test signal:**
   ```
   python3 make_clicks.py
   ```
   This writes `clicks.wav`: one 1 ms click every 0.5 s, 60 s long, 48 kHz.
2. **Set up two paths from the same computer.** On a Mac, open Audio MIDI Setup, create a Multi-Output Device, and include:
   - the built-in headphone output, with wired earbud **A**;
   - **ESP32-S3 USB Audio**, the transmitter. Earbud **B** goes into the receiver's dongle.

   Use plain wired earbuds for both. Don't use the XM4: its ANC/DSP adds its own delay.
3. **Record.** Put both earbuds against the phone's microphone, side by side. Play `clicks.wav` and record the full 60 s.
4. **Analyse:**
   ```
   ffmpeg -i rec.m4a -ac 1 -ar 48000 rec.wav
   python3 measure_latency.py rec.wav
   ```
   This prints the median delay of B after A (Δ).
5. **Calibrate.** Repeat steps 3–4 with the same USB dongle plugged **directly** into the computer, in place of the transmitter, and include it in the Multi-Output Device. This gives Δ₀. It covers the computer's output skew between devices and the dongle's own delay.

**Latency added by the wireless link = Δ − Δ₀.** Report the median, the spread, and how many clicks were paired. The script prints all three.

### Why the calibration matters

A Multi-Output Device doesn't guarantee that its outputs are in sync; offsets of several milliseconds are common. Subtracting Δ₀ removes that offset and the dongle's DAC delay. What remains is the transmitter, the radio, the jitter buffer and the USB host.

The analysis was checked on synthetic data: an injected 20.29 ms offset was recovered as 20.29 ms.
