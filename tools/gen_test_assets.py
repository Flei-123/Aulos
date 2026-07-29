#!/usr/bin/env python3
"""Generates the deterministic WAV files the Aulos test suite measures against.

Everything here is exactly reproducible so the tests can assert on real numbers
instead of "sounds fine".
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


def sine(freq, seconds, amplitude=0.5, rate=RATE):
    n = int(seconds * rate)
    return [amplitude * math.sin(2.0 * math.pi * freq * i / rate) for i in range(n)]


def dc(value, seconds, rate=RATE):
    """Constant level. Makes gain measurements trivial and exact."""
    return [value] * int(seconds * rate)


def main():
    os.makedirs(OUT, exist_ok=True)

    # 1.0 s of constant 0.5 -> any measured level is a pure gain readout
    write_wav("dc_half.wav", dc(0.5, 1.0))

    # 1.0 s of 440 Hz at 0.5 -> pitch measurements via zero crossings
    write_wav("sine440.wav", sine(440.0, 1.0, 0.5))

    # short 100 ms blip for voice pool and instance limit tests
    write_wav("blip.wav", sine(1000.0, 0.1, 0.5))

    # three "variations" with distinct DC levels so sample selection is visible
    write_wav("var_a.wav", dc(0.2, 0.5))
    write_wav("var_b.wav", dc(0.4, 0.5))
    write_wav("var_c.wav", dc(0.6, 0.5))

    # a stereo file: left 0.5, right 0.25
    n = int(0.5 * RATE)
    inter = []
    for _ in range(n):
        inter.append(0.5)
        inter.append(0.25)
    write_wav("stereo.wav", inter, channels=2)

    print("wrote test assets to", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
