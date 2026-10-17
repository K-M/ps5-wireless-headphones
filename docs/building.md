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
| Dongle hardware volume | `rx/main/main.c` `DONGLE_VOLUME` | 60 |
| Radio rate | `tx/main/main.c` `radio_init()` | 802.11b 11 Mbit/s |

## Host simulation

`firmware/test/sim.c` simulates the codec and jitter buffer under loss, jitter and clock drift:

```bash
cd firmware/test
gcc -O2 -I../common/linkproto/include -I../rx/main sim.c ../rx/main/jbuf.c -lm -o sim && ./sim
```
