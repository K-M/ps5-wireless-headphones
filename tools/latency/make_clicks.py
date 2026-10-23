# Generates clicks.wav: 48 kHz stereo, a 1 ms full-scale click every 0.5 s for 60 s.
import wave, struct
fs, period, n_clicks, width = 48000, 0.5, 120, 48
frames = bytearray()
for i in range(int(fs * period * n_clicks)):
    v = 30000 if (i % int(fs * period)) < width else 0
    frames += struct.pack('<hh', v, v)
with wave.open('clicks.wav', 'wb') as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(fs); w.writeframes(bytes(frames))
print('wrote clicks.wav')
