#!/usr/bin/env python3
"""Measures the rendered demo stems instead of trusting them.

Usage:  python3 tools/analyse_demo.py build

Every check prints a measured number and a PASS/FAIL against a value that was
derived from physics or from the bank, not from a previous run of this script.
"""
import math
import os
import sys
import wave

import numpy as np

BUILD = sys.argv[1] if len(sys.argv) > 1 else "build"
RATE = 48000
SPEED_OF_SOUND = 343.0

fails = 0


def load(name):
    path = os.path.join(BUILD, name)
    with wave.open(path, "rb") as w:
        rate = w.getframerate()
        n = w.getnframes()
        raw = np.frombuffer(w.readframes(n), dtype="<i2").astype(np.float64) / 32768.0
    return raw.reshape(-1, 2), rate


def db(v):
    return 20 * math.log10(max(float(v), 1e-12))


def rms(v):
    return float(np.sqrt(np.mean(v ** 2))) if len(v) else 0.0


def seg(sig, rate, t0, t1):
    return sig[int(t0 * rate):int(t1 * rate)]


def centroid(v, rate, fmax=8000.0):
    if len(v) < 2048:
        return 0.0
    win = np.hanning(len(v))
    spec = np.abs(np.fft.rfft(v * win))
    freqs = np.fft.rfftfreq(len(v), 1 / rate)
    m = freqs <= fmax
    p = (spec[m] ** 2)
    return float((freqs[m] * p).sum() / max(p.sum(), 1e-12))


def fundamental(v, rate, lo=40.0, hi=1200.0):
    """Autocorrelation pitch estimate. The engine is a saw, this is reliable."""
    if len(v) < 8192:
        return 0.0
    v = v - v.mean()
    if rms(v) < 1e-5:
        return 0.0
    ac = np.correlate(v, v, mode="full")[len(v) - 1:]
    ac /= max(ac[0], 1e-12)
    lag_lo, lag_hi = int(rate / hi), min(int(rate / lo), len(ac) - 1)
    k = int(np.argmax(ac[lag_lo:lag_hi])) + lag_lo
    # parabolic refinement around the peak
    if 0 < k < len(ac) - 1:
        a, b, c = ac[k - 1], ac[k], ac[k + 1]
        d = a - 2 * b + c
        if abs(d) > 1e-12:
            k = k + 0.5 * (a - c) / d
    return rate / k if k else 0.0


def check(label, value, lo, hi, unit=""):
    global fails
    ok = lo <= value <= hi
    if not ok:
        fails += 1
    print(f"  [{'PASS' if ok else 'FAIL'}] {label:<46} {value:9.3f}{unit}"
          f"  expected {lo:.3f} .. {hi:.3f}")


def balance(x, rate, t0, t1):
    l, r = rms(seg(x[:, 0], rate, t0, t1)), rms(seg(x[:, 1], rate, t0, t1))
    return (r - l) / max(r + l, 1e-9)


# ---------------------------------------------------------------- doppler ----
print("\n=== doppler fly-by (stem_doppler.wav) ===")
print("  25 m/s, constant rpm, passes the listener at t = 9 s")
x, rate = load("stem_doppler.wav")
mono = x.mean(axis=1)

f_before = fundamental(seg(mono, rate, 5.0, 6.0), rate)
f_after = fundamental(seg(mono, rate, 12.0, 13.0), rate)
v = 25.0
theory = ((SPEED_OF_SOUND / (SPEED_OF_SOUND - v)) /
          (SPEED_OF_SOUND / (SPEED_OF_SOUND + v)))
measured = f_before / max(f_after, 1e-9)
print(f"  approaching  {f_before:8.2f} Hz")
print(f"  receding     {f_after:8.2f} Hz")
check("shift ratio matches (c+v)/(c-v)", measured, theory * 0.97, theory * 1.03,
      f"  theory {theory:.3f}")
check("level peaks at the closest approach",
      db(rms(seg(mono, rate, 8.8, 9.2))) - db(rms(seg(mono, rate, 3.2, 3.6))),
      6.0, 40.0, " dB")

# -------------------------------------------------------------------- car ----
print("\n=== drive-by (stem_car.wav) ===")
print("  pass 1: 5-13 s left to right, pass 2: 15-27 s right to left,")
print("  occluded (low pass) between 21 s and 24 s")
x, rate = load("stem_car.wav")
mono = x.mean(axis=1)

b_early = balance(x, rate, 5.5, 6.5)
b_late = balance(x, rate, 11.5, 12.5)
print(f"  balance at 6 s   {b_early:+.3f}   (-1 = hard left)")
print(f"  balance at 12 s  {b_late:+.3f}   (+1 = hard right)")
check("pass 1 starts on the left", b_early, -1.01, -0.55)
check("pass 1 ends on the right", b_late, 0.55, 1.01)
check("pass 2 runs the other way", balance(x, rate, 16.0, 17.0) - balance(x, rate, 25.5, 26.5),
      0.9, 2.02)

c_open_a = centroid(seg(mono, rate, 19.0, 20.0), rate)
c_occl = centroid(seg(mono, rate, 22.0, 23.5), rate)
c_open_b = centroid(seg(mono, rate, 25.0, 26.0), rate)
print(f"  centroid before  {c_open_a:8.1f} Hz")
print(f"  centroid behind  {c_occl:8.1f} Hz")
print(f"  centroid after   {c_open_b:8.1f} Hz")
check("occlusion darkens the timbre", c_occl / max(c_open_a, 1e-9), 0.0, 0.75, "x")
check("timbre opens up again", c_open_b / max(c_occl, 1e-9), 1.3, 99.0, "x")

# --------------------------------------------------------------------- rpm ---
print("\n=== rpm curve (stem_rpm.wav) ===")
print("  parked 8 m away, rpm ramps 1200 -> 6400 between 2 s and 22 s")
print("  the sample loops at exactly 70.0 Hz, so pitch = f / 70")
x, rate = load("stem_rpm.wav")
mono = x.mean(axis=1)

CURVE = [(1200, 0.80), (2600, 1.45), (4400, 2.30), (6400, 3.20)]   # from the bank
BASE_HZ = 70.0


def curve_at(rpm):
    if rpm <= CURVE[0][0]:
        return CURVE[0][1]
    if rpm >= CURVE[-1][0]:
        return CURVE[-1][1]
    for (r0, v0), (r1, v1) in zip(CURVE, CURVE[1:]):
        if r0 <= rpm <= r1:
            return v0 + (v1 - v0) * (rpm - r0) / (r1 - r0)
    return CURVE[-1][1]


worst = 0.0
for t in (4.0, 8.0, 12.0, 16.0, 20.0):
    # the ramp keeps rising inside the window, so autocorrelation reports the
    # frequency at the CENTRE of the window, not at its start
    rpm_now = 1200.0 + (t + 0.3 - 2.0) / 20.0 * 5200.0
    want = BASE_HZ * curve_at(rpm_now)
    got = fundamental(seg(mono, rate, t, t + 0.6), rate)
    err = abs(got - want) / want
    worst = max(worst, err)
    print(f"  t={t:>4.1f}s  rpm {rpm_now:6.0f}  expected {want:7.2f} Hz  "
          f"measured {got:7.2f} Hz  error {err * 100:5.2f} %")
check("pitch follows the bank curve (worst case)", worst * 100, 0.0, 1.0, " %")

# ------------------------------------------------------------------ steps ----
print("\n=== footsteps (stem_steps.wav) ===")
print("  a 3 m circle around the listener, one step every 0.42 s")
x, rate = load("stem_steps.wav")
bals = [balance(x, rate, t, t + 0.4) for t in np.arange(2.0, 8.6, 0.42)]
bals = [b for b in bals if abs(b) > 1e-4]
check("steps sweep across the stereo field", max(bals) - min(bals), 0.8, 2.02)
check("the circle stays roughly balanced", abs(float(np.mean(bals))), 0.0, 0.35)

# --------------------------------------------------------------- ambience ----
print("\n=== ambience + music (stem_amb.wav) ===")
print("  aul_stop(radio, 3.0 s) fires at 21 s, wind keeps running")
x, rate = load("stem_amb.wav")
mono = x.mean(axis=1)
before = db(rms(seg(mono, rate, 19.0, 20.5)))
during = db(rms(seg(mono, rate, 22.5, 23.5)))
after = db(rms(seg(mono, rate, 25.0, 27.0)))
print(f"  rms 19-20.5 s {before:7.1f} dBFS")
print(f"  rms 22.5-23.5 s {during:7.1f} dBFS")
print(f"  rms 25-27 s   {after:7.1f} dBFS")
check("music fade actually lowers the level", before - after, 3.0, 40.0, " dB")
check("wind survives the music fade", after, -45.0, -5.0, " dBFS")

# ------------------------------------------------------------------- full ----
print("\n=== full mix (stem_full.wav) ===")
x, rate = load("stem_full.wav")
mono = x.mean(axis=1)
peak = float(np.max(np.abs(x)))
print(f"  peak          {peak:.3f} ({db(peak):+.1f} dBFS)")
print(f"  rms           {rms(mono):.4f} ({db(rms(mono)):+.1f} dBFS)")
check("normalised close to -3 dBFS", db(peak), -3.6, -2.4, " dBFS")
check("no clipping", float(np.sum(np.abs(x) >= 0.9999)), 0.0, 0.0, " samples")
check("loudness in a sane range", db(rms(mono)), -30.0, -14.0, " dBFS")
check("no dc offset", abs(float(mono.mean())), 0.0, 0.002)

# silence detector: a dropout would show up as a block of exact zeros
z = np.abs(mono) < 1e-6
runs, run = 0, 0
longest = 0
for s in z:
    run = run + 1 if s else 0
    longest = max(longest, run)
check("no dropouts (longest silent run)", longest / rate, 0.0, 0.05, " s")

print(f"\n{'ALL CHECKS PASSED' if fails == 0 else str(fails) + ' CHECK(S) FAILED'}")
sys.exit(1 if fails else 0)
