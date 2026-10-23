# Building from source

Requires **ESP-IDF v5.4.x** (tested 5.4.2). All components are bundled; no component-manager downloads needed.

```bash
# transmitter
cd firmware/tx
idf.py set-target esp32s3
idf.py build

# receiver
cd ../rx
idf.py set-target esp32s3
idf.py build
```

Flash with `idf.py -p <port> flash`, or produce a single image for the web flasher:

```bash
cd build
esptool.py --chip esp32s3 merge_bin -o ../link.bin --flash_mode dio --flash_size 2MB \
  0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0x10000 link_tx.bin   # link_rx.bin for the receiver
```

## Wi-Fi channel

Default is 13 (`firmware/common/linkproto/include/link_proto.h`). Override at configure time; both boards must match:

```bash
idf.py -DLINK_DIAG="LINK_WIFI_CHANNEL=6" build
```

## Tuning knobs

| Setting | File | Default |
|---|---|---|
| Jitter-buffer depth (frames of 3 ms) | `rx/main/jbuf.h` `JB_TARGET_FRAMES` | 4 |
| Encoder pins | `rx/main/main.c` `ENC_GPIO_A` / `ENC_GPIO_B` | GPIO8 (D5) / GPIO9 (D6) |
| Encoder counts per detent / direction | `ENC_COUNTS_PER_DETENT` / `ENC_REVERSE` | 4 / 0 |
| Volume step, max boost level, default | `VOL_STEP` / `VOL_MAX` / `VOL_DEFAULT` | 2 / 124 (+12 dB) / 100 |
| TX send queue depth | `tx/main/main.c` `TXQ_DEPTH` | 1 (newest frame only) |
| Radio rate | `tx/main/main.c` `radio_init()` | 802.11b 11 Mbit/s |

## Host simulation

`firmware/test/sim.c` simulates the codec and jitter buffer under loss, jitter and clock drift:

```bash
cd firmware/test
gcc -O2 -I../common/linkproto/include -I../rx/main sim.c ../rx/main/jbuf.c -lm -o sim && ./sim
```

## Experimental: microphone (`LINK_MIC`)

Both projects contain an unfinished mic return path behind a build flag:

```bash
idf.py -DLINK_MIC=1 build
```

With this flag, the transmitter enumerates as a headset (PID `0x8012`) and the receiver samples an analog mic on GPIO4. It is **not usable as is**: radio bursts couple into the ESP32-S3 ADC and produce a loud 166 Hz tone. The planned fix is an I2S MEMS mic. Default builds don't include any of this code.
