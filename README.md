# PS5 Wireless Headphones (any wired headphone, ~20 ms)

Turn **any headphone with a 3.5 mm input** — Sony WH-1000XM4/XM5, Bose, studio cans — into a **low-latency wireless PS5 headset** with two ESP32-S3 boards, a cheap USB audio dongle and a few hobby modules.

The PS5 has no Bluetooth audio, and Bluetooth dongles add 150–250 ms with most headphones. This link skips Bluetooth entirely: uncompressed 48 kHz / 16-bit stereo over ESP-NOW into your headphones' wired input.

<!-- TODO: hero photo — headphones with the receiver clipped on, PS5 in background -->
![Hero photo](images/hero.jpg)

```
PS5 ──USB──▶ [ESP32-S3 transmitter] ~~ ESP-NOW 2.4 GHz ~~▶ [ESP32-S3 receiver] ──USB──▶ USB-to-3.5 mm dongle ──▶ headphones
             shows up as "USB Headphones"                    USB host, on the headband
```

## Highlights

- **Plug and play on PS5** — the transmitter enumerates as a standard USB Audio Class 1 headset. No PS5 settings beyond picking *USB Headphones* as output.
- **~20 ms end to end** (calculated from measured buffer levels, see [Latency](#latency)). No perceptible lip-sync offset in testing.
- **Lossless audio** — raw 16-bit PCM, not a lossy codec. Each packet also carries a compressed copy of the previous frame, so a single lost packet is repaired invisibly.
- **Keeps every headphone feature** — ANC, EQ and battery life are untouched because the headphones run in wired mode.
- **One-click install** from the browser: **[web flasher](https://k-m.github.io/ps5-wireless-headphones/)** (Chrome/Edge).

## Parts

| Part | Notes |
|---|---|
| Waveshare ESP32-S3-Pico | Transmitter, plugs into the PS5. Any ESP32-S3 board with native USB should work. |
| Waveshare ESP32-S3-Nano | Receiver (USB host). |
| USB-to-3.5 mm audio dongle | Must support 48 kHz 16-bit stereo (most cheap USB-C/USB-A dongles do). Tested: Generalplus `1b3f:2008`. |
| USB OTG adapter | Nano's USB-C port → dongle. |
| 1S LiPo (500 mAh) | ~2.5–3 h of use. |
| TP4056 charger module (with protection) | Replace R_PROG (R3, `122`) with 3 kΩ (`302`) for a 400 mA charge on a 500 mAh cell. Stock is 1 A. |
| MT3608 boost module | Set to **5.05 V before connecting**. The receiver browns out below ~4.5 V, so it cannot run straight off the LiPo. |
| 470 µF capacitor | Across the Nano's VBUS and GND. |

STL files for the enclosures are in [`hardware/`](hardware/).

## Wiring (receiver)

```
LiPo ──▶ TP4056 B+/B−
TP4056 OUT+/OUT− ──[switch]──▶ MT3608 VIN+/VIN−
MT3608 OUT+ ──▶ Nano VBUS pin      (NOT VIN)
MT3608 OUT− ──▶ Nano GND
470 µF across VBUS–GND at the Nano
Nano USB-C ──OTG adapter──▶ USB audio dongle ──3.5 mm──▶ headphones
```

The Nano's VBUS pin is wired straight to its USB-C port, so powering VBUS is what powers the dongle. Powering VIN instead leaves the dongle dead.

The transmitter needs no wiring: plug it into a PS5 USB port.

## Install

1. Open the **[web flasher](https://k-m.github.io/ps5-wireless-headphones/)** in Chrome or Edge.
2. Pick the Wi-Fi channel (13 is the default; both boards must match).
3. Flash the transmitter (hold BOOT while plugging in) and the receiver (bridge B1 to GND, tap RESET).
4. Plug the transmitter into the PS5 → **Settings → Sound → Audio Output → Output Device → USB Headphones**.
5. Power the receiver, turn on the headphones, and set the PS5 volume to ~30% to start.

Prefer the command line? Prebuilt images are in [`docs/firmware/`](docs/firmware/): flash the `.bin` at offset `0x0` with `esptool.py --chip esp32s3 write_flash 0x0 <file>.bin`.

## Latency

Calculated per stage from the receiver's logged buffer levels (not a single end-to-end measurement):

| Stage | Time |
|---|---|
| Assemble 3 ms audio frame on the transmitter | ≤ 3 ms |
| Radio send (11 Mbit/s DSSS, measured) | ~1.4 ms |
| Jitter buffer (4 frames, measured average) | ~11.7 ms |
| USB host queue to dongle | ~2–4 ms |
| Dongle DAC | ~1 ms |
| **Total** | **≈ 20 ms** |

## How it works

- **Transmitter:** TinyUSB UAC1 device (44.1/48 kHz, adaptive isochronous). PS5 audio is resampled to 48 kHz if needed, cut into 3 ms frames, and broadcast over ESP-NOW v2 at 11 Mbit/s DSSS (tested more reliable than OFDM rates for 740-byte packets).
- **Packet:** 12-byte header + 576 bytes PCM + 152-byte IMA-ADPCM copy of the previous frame.
- **Receiver:** a 4-frame jitter buffer; recovers lost frames from the backup copy, fades out/in on longer gaps, and corrects clock drift by resampling a whole frame by ±1 sample (inaudible).
- **USB host:** Espressif `usb_host_uac` driving the dongle, with the periodic-OUT FIFO bias enabled so 192-byte isochronous packets fit.

Details in [`docs/troubleshooting.md`](docs/troubleshooting.md) and [`docs/building.md`](docs/building.md).

## Known limitations

- **No microphone.** Most headphones (including the XM4) don't carry the mic over the 3.5 mm cable. Use the DualSense mic for chat.
- **Shares 2.4 GHz.** On a busy Wi-Fi channel you'll hear occasional ticks; switch channels in the flasher.
- **Needs 5 V on the receiver** (hence the boost module).

## Roadmap

- [ ] Mic return path (receiver mic → PS5 as a USB headset)
- [ ] Automatic channel selection
- [ ] Custom single-board receiver PCB

Contributions welcome. See issues labelled *good first issue*.

## License

MIT for this project's code. Bundled third-party components keep their own licenses: TinyUSB (MIT), Espressif `usb_host_uac` (Apache-2.0), Espressif `cmake_utilities` (Apache-2.0).

Not affiliated with Sony Interactive Entertainment. "PS5" is used only to describe compatibility.
