"""
Measure wireless-link latency from one microphone recording that contains, for every
click, two arrivals: the WIRED reference earbud (first) and the WIRELESS-path earbud (second).

usage:  python3 measure_latency.py recording.wav
Accepts WAV (convert phone .m4a first:  ffmpeg -i rec.m4a -ac 1 -ar 48000 rec.wav)
Needs: numpy
"""
import sys, wave
import numpy as np

path = sys.argv[1]
with wave.open(path, 'rb') as w:
    fs = w.getframerate(); ch = w.getnchannels(); sw = w.getsampwidth()
    raw = w.readframes(w.getnframes())
x = np.frombuffer(raw, dtype={2: np.int16, 4: np.int32}[sw]).astype(float)
x = x.reshape(-1, ch).mean(axis=1)
# causal high-pass (~100 Hz) to remove rumble without smearing energy before an onset
a = np.exp(-2 * np.pi * 100 / fs)
y = np.empty_like(x); prev_x = 0.0; prev_y = 0.0
for n in range(len(x)):
    prev_y = a * (prev_y + x[n] - prev_x); prev_x = x[n]; y[n] = prev_y
env = np.abs(y)
thr = 0.2 * np.percentile(env, 99.995)     # ~20 % of the click peaks

# onsets: first sample above threshold after >= 3 ms of quiet
quiet = int(0.003 * fs)
onsets, last = [], -10**9
for i in np.flatnonzero(env > thr):
    if i - last > quiet:
        onsets.append(i)
    last = i
onsets = np.array(onsets) / fs

# pair each onset with the next one 2..80 ms later; skip both, continue
delays, i = [], 0
while i < len(onsets) - 1:
    d = onsets[i + 1] - onsets[i]
    if 0.002 <= d <= 0.080:
        delays.append(d * 1000); i += 2
    else:
        i += 1
if len(delays) < 5:
    sys.exit(f'only {len(delays)} click pairs found - move the earbuds closer to the mic / raise volume')
d = np.array(delays)
med = np.median(d); keep = d[np.abs(d - med) < 3]   # drop outliers (echoes, missed onsets)
print(f'pairs: {len(d)} (used {len(keep)})')
print(f'wireless minus wired: mean {keep.mean():.2f} ms, median {np.median(keep):.2f} ms, '
      f'std {keep.std():.2f} ms, min {keep.min():.2f}, max {keep.max():.2f}')
