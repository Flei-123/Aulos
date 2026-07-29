#!/usr/bin/env python3
"""Compares two 16 bit stereo WAV renders segment by segment.

Used to tell a real behavioural difference (wrong gain, wrong pitch curve,
missing voice) apart from harmless numerical drift between two libm
implementations, which shows up as an error that grows with time while the
correlation stays at 1.0.

    python3 tools/compare_renders.py build/hello.wav build/hello_wasm.wav
"""
import sys, wave
import numpy as np


def read(path):
    w = wave.open(path, "rb")
    d = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
    return d.reshape(-1, w.getnchannels())


a = read(sys.argv[1])
b = read(sys.argv[2])
n = min(len(a), len(b))
a, b = a[:n], b[:n]
sr = 48000
print(f"frames {n}  ({n/sr:.2f} s)")

segs = 8
step = n // segs
for i in range(segs):
    x, y = a[i * step:(i + 1) * step], b[i * step:(i + 1) * step]
    d = y - x
    ref = np.sqrt((x ** 2).mean())
    print(f"  {i*step/sr:4.2f}-{(i+1)*step/sr:4.2f}s  max|d|={np.abs(d).max():7.1f}"
          f"  rms|d|={np.sqrt((d**2).mean()):8.3f}  rms_ref={ref:8.1f}"
          f"  snr={20*np.log10(ref/max(np.sqrt((d**2).mean()),1e-9)):6.1f} dB")

for name, sl in (("first 0.25 s", slice(0, sr // 4)), ("last 0.25 s", slice(-sr // 4, None))):
    x = a[sl, 0] - a[sl, 0].mean()
    y = b[sl, 0] - b[sl, 0].mean()
    c = np.correlate(y, x, "full")
    lag = int(c.argmax()) - (len(x) - 1)
    denom = np.linalg.norm(x) * np.linalg.norm(y)
    print(f"  {name}: best lag {lag} samples, peak correlation {c.max()/denom:.6f}")

# broadband energy check: are they the same sound at all?
for ch, label in ((0, "L"), (1, "R")):
    A = np.abs(np.fft.rfft(a[:, ch] * np.hanning(n)))
    B = np.abs(np.fft.rfft(b[:, ch] * np.hanning(n)))
    num = float((A * B).sum())
    print(f"  spectrum cosine {label}: {num/(np.linalg.norm(A)*np.linalg.norm(B)):.6f}"
          f"   energy ratio {float((b[:,ch]**2).sum()/(a[:,ch]**2).sum()):.6f}")
