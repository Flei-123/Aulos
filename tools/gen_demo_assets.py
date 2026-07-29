#!/usr/bin/env python3
"""Generates the demo assets used by tools/aulos_demo.cpp.

These are synthesised on purpose: the repository stays small and every sound is
reproducible byte for byte. They are not meant to be pretty, they are meant to
make the runtime features audible (3D movement, rpm -> pitch, occlusion -> low
pass, bus ducking, fades).
"""
import math
import os
import struct
import wave

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets")
RATE = 48000


def write_wav(name, samples, channels=1, rate=RATE):
    path = os.path.join(OUT, name)
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for s in samples:
            v = max(-1.0, min(1.0, s))
            frames += struct.pack("<h", int(v * 32767))
        w.writeframes(bytes(frames))
    return path


class Rng:
    """Deterministic xorshift32 so the assets never change between runs."""

    def __init__(self, seed=0x1234567):
        self.s = seed

    def next(self):
        x = self.s
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        self.s = x & 0xFFFFFFFF
        return self.s

    def bipolar(self):
        return (self.next() / 4294967295.0) * 2.0 - 1.0


def saw(phase):
    return 2.0 * (phase - math.floor(phase + 0.5))


def engine_loop(seconds=0.5, base=70.0, amp=0.42):
    """Seamless engine loop: the cycle count is an integer so it loops clean."""
    n = int(seconds * RATE)
    cycles = round(base * seconds)          # integer -> perfect loop point
    f = cycles / seconds
    rng = Rng(0xBEEF)
    out = []
    for i in range(n):
        t = i / RATE
        ph = f * t
        s = 0.0
        s += 0.60 * saw(ph)                 # fundamental
        s += 0.25 * saw(ph * 2.0)           # first harmonic, gives the "growl"
        s += 0.12 * saw(ph * 3.01)          # slightly detuned -> beating
        s += 0.06 * math.sin(2 * math.pi * ph * 0.5)
        s += 0.04 * rng.bipolar()           # combustion noise
        # mild soft clip, keeps it from sounding like a pure buzzer
        s = math.tanh(s * 1.6) * 0.62
        out.append(s * amp)
    return out


def wind_loop(seconds=4.0, amp=0.18):
    """Brown-ish noise through a one pole low pass, crossfaded to loop."""
    n = int(seconds * RATE)
    rng = Rng(0xC0FFEE)
    raw = []
    lp = 0.0
    a = 1.0 - math.exp(-2.0 * math.pi * 320.0 / RATE)
    for _ in range(n):
        lp += a * (rng.bipolar() - lp)
        raw.append(lp)
    peak = max(1e-9, max(abs(v) for v in raw))
    raw = [v / peak for v in raw]
    # slow gusts
    for i in range(n):
        t = i / RATE
        g = 0.62 + 0.38 * (0.5 + 0.5 * math.sin(2 * math.pi * t / seconds))
        raw[i] *= g
    xf = int(0.25 * RATE)
    for i in range(xf):
        k = i / xf
        raw[i] = raw[i] * k + raw[n - xf + i] * (1.0 - k)
    return [v * amp for v in raw[: n - xf]]


def step(seed, tone, amp=0.5):
    """Short percussive hit: noise burst + a thump, exponential decay."""
    n = int(0.22 * RATE)
    rng = Rng(seed)
    out = []
    lp = 0.0
    a = 1.0 - math.exp(-2.0 * math.pi * 1800.0 / RATE)
    for i in range(n):
        t = i / RATE
        env = math.exp(-t * 34.0)
        lp += a * (rng.bipolar() - lp)
        body = math.sin(2 * math.pi * tone * t) * math.exp(-t * 18.0)
        out.append((lp * 1.4 * env + body * 0.7) * amp)
    return out


def beep(freq, seconds=0.18, amp=0.45):
    n = int(seconds * RATE)
    out = []
    for i in range(n):
        t = i / RATE
        env = min(1.0, t / 0.005) * math.exp(-t * 9.0)
        s = math.sin(2 * math.pi * freq * t) + 0.3 * math.sin(4 * math.pi * freq * t)
        out.append(s * env * amp * 0.7)
    return out


def music_loop(seconds=8.0, amp=0.22):
    """Four bar pad: a chord every two seconds, sine partials with slow attack."""
    n = int(seconds * RATE)
    chords = [
        (110.00, 130.81, 164.81),   # A minor
        (98.00, 123.47, 146.83),    # G major
        (87.31, 110.00, 130.81),    # F major
        (98.00, 116.54, 146.83),    # G / Bb
    ]
    out = [0.0] * n
    bar = n // len(chords)
    for c, notes in enumerate(chords):
        for i in range(bar):
            t = i / RATE
            env = min(1.0, t / 0.30) * min(1.0, (bar - i) / (0.30 * RATE))
            s = 0.0
            for k, f in enumerate(notes):
                s += math.sin(2 * math.pi * f * t) / (1.6 + k)
                s += 0.18 * math.sin(2 * math.pi * f * 2 * t) / (1.6 + k)
            out[c * bar + i] = s * env * amp * 0.5
    xf = int(0.20 * RATE)
    for i in range(xf):
        k = i / xf
        out[i] = out[i] * k + out[n - xf + i] * (1.0 - k)
    return out[: n - xf]


def main():
    os.makedirs(OUT, exist_ok=True)
    write_wav("engine_loop.wav", engine_loop())
    write_wav("wind_loop.wav", wind_loop())
    write_wav("step_a.wav", step(0x11, 92.0))
    write_wav("step_b.wav", step(0x22, 104.0))
    write_wav("step_c.wav", step(0x33, 84.0))
    write_wav("beep.wav", beep(880.0))
    write_wav("radio_loop.wav", music_loop())
    print("wrote demo assets to", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
