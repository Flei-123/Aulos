# Aulos

A small, engine-agnostic **game audio middleware runtime** in C++17. Roughly
what FMOD Studio or Wwise do at runtime, minus the authoring tool: your game
says `aul_play("engine_start")` and the library owns everything after that -
voice management, 3D panning, distance attenuation, doppler, buses, parameter
curves, fades and voice stealing.

Sound behaviour lives in a JSON bank, not in your C++ code. Changing a volume,
a rolloff curve or a random pitch range must never require a recompile.

```
miniaudio   ->  device I/O, decoding, resampling   (not written here)
Aulos       ->  events, voices, buses, 3D, params  (this library)
your game   ->  aul_play("footstep") and forget
```

Status: the runtime works and is measured, see **Verification** below. It is not
a released product; there is no authoring GUI and no streaming yet.

## Why

Every engine ships an audio API, and every serious game outgrows it within a
year: no bus hierarchy, no random variation, no priority based voice stealing,
no data driven parameter curves, no "the sound designer changes it without a
programmer". Middleware exists exactly to close that gap - and the free options
are either dead, GPL, or an SDK you cannot ship in a hobby project without
reading a lawyer's opinion first. Aulos is MIT, one static library, ~1.3k lines.

## Build

No dependencies beyond a C++17 compiler. miniaudio is vendored in `extern/`.

```bash
./build.sh                # -> build/libaulos.a + libaulos.so, test_aulos,
                          #    bench_aulos, aulos_demo
./build/test_aulos examples/test_bank.json assets   # 46 measured assertions
./build/bench_aulos                                 # cost per voice
```

Or with CMake, which also builds the standalone example and registers ctest:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DAULOS_SHARED=ON
cmake --build build --parallel && ctest --test-dir build --output-on-failure
./build/hello_aulos examples/hello_bank.json assets hello.wav
```

Windows, MSVC (this is what produces aulos.dll for C# / Unity hosts):

```bat
build_msvc.bat            :: -> build\aulos.dll, test_aulos.exe, bench_aulos.exe
```

## Use

```c
#include "aulos.h"

aul_config cfg = {0};
cfg.sample_rate   = 48000;
cfg.max_voices    = 64;
cfg.enable_device = 1;              /* 0 = offline, you call aul_render() */
cfg.asset_root    = "assets";

aul_system *sys;
aul_create(&cfg, &sys);
aul_load_bank(sys, "banks/game.json");

/* every frame */
aul_set_listener(sys, camPos, camForward, camUp, camVelocity);
aul_update(sys);

/* whenever something happens */
aul_instance car = aul_play_3d(sys, "vehicle_engine", carPos);
aul_set_parameter(sys, car, "rpm", 4200.0f);
aul_set_position(sys, car, carPos, carVelocity);
aul_stop(sys, car, 0.25f);          /* 250 ms fade out */
```

The full C API is 20 functions in [`include/aulos.h`](include/aulos.h).

### Coordinates

Right handed, the listener looks down `-forward`, `right = cross(forward, up)`
- the OpenGL / Godot convention. A source at `+x` in front of a listener with
`forward = (0,0,-1)` is heard on the right. Unity is left handed and looks down
`+z`, so `bindings/unity/Aulos.cs` mirrors `z` on every vector it passes in.
That mirror is an isometry: distances, rolloff and doppler are unaffected, only the
left/right axis flips. Units are metres, doppler assumes 343 m/s.

### Threading contract

* Everything except `aul_render()` is safe from any game thread and never
  blocks the audio thread. Calls become commands in a lock free ring.
* `aul_render()` belongs to the device callback. Call it yourself only when
  `enable_device = 0` (offline rendering, tests, bouncing to file).
* `aul_update()` runs once per game frame: recycles finished voices, refreshes
  the stats snapshot.
* Handles are generation tagged (`slot | generation << 16`). Calling anything
  on a voice that already finished is a silent no-op, never a crash. That is
  the property that makes gameplay code simple: nobody has to track lifetimes.

## Integrating it

Aulos is a library, not a plugin. The C API in `include/aulos.h` is the whole
product; every host below talks to that same API. Nothing in `src/` knows what
an engine is.

**Your own C/C++ engine** - vendor the repo and add two lines of CMake:

```cmake
add_subdirectory(extern/aulos)
target_link_libraries(my_game PRIVATE aulos::aulos)
```

That gives you a static library plus the header. `examples/hello_aulos.c` is a
complete integration in one file - create, load bank, set listener, play, move,
set a parameter, render - and it is run by CI, so it cannot rot.

**Unreal** - same static target; put the `aul_*` calls behind a subsystem and
mirror nothing, Unreal is left handed like Unity (see Coordinates) so flip `y`
or `z` once in your wrapper.

**Godot** - right handed already, so vectors go straight through. Link the
static library into a GDExtension.

**C#, Python, Rust, anything with an FFI** - build with `-DAULOS_SHARED=ON` and
call the exported C functions. The API is deliberately plain C: no classes, no
callbacks into the host, no ownership tricks, handles are `uint32_t`.

**Unity** - `bindings/unity/` is that FFI case, already written: `Aulos.cs`
(P/Invoke), `AulosListener` and `AulosSource` (two MonoBehaviours). Copy the
three files plus `aulos.dll` into your project. It is one supported host, not
the point of the library.

**The browser** - `bindings/web/` builds the same sources with emscripten and
runs the mixer inside an `AudioWorkletProcessor`, so Phaser, three.js, PlayCanvas
or a plain canvas game get the identical runtime:

```js
import { Aulos } from "aulos-audio";
const aulos = await Aulos.create(new AudioContext({ sampleRate: 48000 }),
                                 { bank: "banks/game.json", assets: ["engine_loop.wav"] });
aulos.node.connect(aulos.node.context.destination);
const car = aulos.play3d("vehicle_engine", -30, 0, -4);
```

No `SharedArrayBuffer`, no pthreads, therefore no COOP/COEP headers and no
cross-origin isolation - it works in a sandboxed iframe on a static host. The
wasm binary is inlined in the module because an `AudioWorkletGlobalScope` has no
`fetch()` to load a side file with. CI diffs the wasm render against the native
one sample by sample: with resampling off the two are **bit identical**
(384000/384000 samples), with a pitch sweep they stay within one quantisation
step before sub-sample libm drift sets in. See `bindings/web/README.md`.

## The bank

```jsonc
{
  "buses": [
    { "name": "master", "volume": 1.0 },
    { "name": "sfx", "parent": "master", "volume": 1.0 }
  ],
  "events": [
    {
      "name": "vehicle_engine",
      "bus": "sfx",
      "sample": "engine_loop.wav",
      "loop": true,
      "spatial": true,
      "min_distance": 6.0, "max_distance": 150.0,
      "rolloff": "inverse",          // or "linear"
      "doppler": 1.0,                // 0 = off, 1 = physical
      "priority": 200,               // higher survives voice stealing
      "parameters": [
        { "name": "rpm", "target": "pitch",
          "curve": [[1200, 0.80], [2600, 1.45], [6400, 3.20]] },
        { "name": "muffle", "target": "lowpass",
          "curve": [[0.0, 20000.0], [1.0, 300.0]] }
      ]
    },
    {
      "name": "footstep",
      "samples": ["step_a.wav", "step_b.wav", "step_c.wav"],  // random pick
      "pitch_random": 0.12, "volume_random": 0.15,            // no machine gun
      "spatial": true, "max_instances": 4
    }
  ]
}
```

JSON with `//` comments and trailing commas is accepted, because sound banks are
edited by hand. Parameter targets are `volume`, `pitch` and `lowpass`; up to 8
parameters per event; unknown parameter names are ignored on purpose so a bank
can add a parameter before the game sets it.

## Verification

Two layers, both reproducible: `./build/test_aulos` asserts on numbers,
`tools/analyse_demo.py` asserts on rendered audio.

**Unit tests - 46 assertions, all measured, none mocked.** Gain math, bus
hierarchy, equal power panning, inverse and linear rolloff, parameter curves,
occlusion filter response, random variation, fades, handle validity, one shots,
the voice pool as a hard ceiling, priority stealing, instance limits, and a load
test that proves no commands are dropped and no slots leak after 1000 plays.

**Every toolchain.** The same 46 assertions pass on gcc 12 / Linux, MSVC 19 /
Windows x64 and clang / macOS - CI runs all three plus the example on every
push. On Windows the shared build is additionally driven
through .NET P/Invoke (`test_pinvoke.ps1`, 12 checks): struct marshalling,
`float[]` rendering, bus round trip and voice accounting across the managed
boundary - the exact path Unity takes.

**Signal tests.** `build/aulos_demo` renders scripted scenes offline, then
`tools/analyse_demo.py` measures the WAV against physics and against the bank:

| check | expected | measured |
|---|---|---|
| doppler shift, 25 m/s fly-by | `(c+v)/(c-v)` = 1.157 | **1.157** |
| stereo balance, source hard left / hard right | ±1.0 | **-0.995 / +0.995** |
| occlusion, spectral centroid behind a wall | clearly darker | **0.52x** |
| rpm -> pitch, 5 points along the bank curve | bank curve | **max error 0.11 %** |
| music fade, `aul_stop(radio, 3.0)` | audible drop | **-13.9 dB** |
| full mix | no clipping, no dropouts | **0 samples, 0 s** |

```bash
for s in car doppler rpm steps amb swarm full; do
  ./build/aulos_demo examples/demo_bank.json assets build/stem_$s.wav $s
done
python3 tools/analyse_demo.py build      # exits non-zero if anything drifts
```

Two real bugs were found this way and are fixed: the doppler direction vector
was inverted (an approaching car dropped in pitch), and occlusion used a single
6 dB/oct pole, which is not audible as "behind a wall".

## Cost

`./build/bench_aulos`, single thread, 48 kHz stereo, 512 frame blocks, on the
machine this was developed on (measure your own):

Ryzen 7 3800X, Windows, MSVC 19 (`build\bench_aulos.exe`):

| workload | 64 voices | 256 voices |
|---|---|---|
| 2D mono | 1.9 % of one core | 6.9 % |
| 3D positioned | 1.9 % | 7.4 % |
| 3D + lowpass + 2 parameters | 2.4 % | 9.8 % |

A Linux container on a Xeon lands within ~20 % of those numbers with gcc 12,
so the cost is dominated by the mixer, not by the toolchain.

A game budget of 64 concurrent 3D voices costs under 3 % of one core, which is
the whole point of a fixed voice pool: the cost is bounded by design, not by how
many `Play()` calls gameplay happens to make.

## Layout

```
include/aulos.h        the entire public C API
src/aulos.cpp          the runtime: mixer, voices, buses, commands, bank loader
src/aul_json.h         a small JSON parser that tolerates comments
tests/test_aulos.cpp   46 measured assertions
tests/bench_aulos.cpp  cost per voice
tools/aulos_demo.cpp   offline scene renderer (full / car / doppler / rpm / ...)
tools/analyse_demo.py  measures the rendered WAVs, exits non-zero on drift
tools/gen_*_assets.py  regenerates every WAV in assets/ deterministically
examples/hello_aulos.c smallest possible integration, no engine involved
examples/*.json        the banks used by the tests, the demo and the example
bindings/unity/        C# bindings and two MonoBehaviours - one host, not the API
bindings/web/          emscripten build, AudioWorklet runtime, npm package
tools/browser_selftest.mjs  runs the wasm build in a real headless Chromium
tools/compare_renders.py    diffs two renders segment by segment
.github/workflows/     CI: Linux, Windows, macOS, plus a wasm/browser job
```

## Not there yet

Streaming from disk (everything is decoded into RAM), reverb and sends, HRTF,
a real authoring tool, and an Unreal/Godot binding. On the web the bank and its
samples are staged into the wasm heap up front, so a large bank costs memory
before it costs anything else. The voice pool is a hard
ceiling by design - if you need 500 voices, raise `max_voices` and pay for it.

## License

MIT, see [LICENSE](LICENSE). miniaudio is public domain / MIT-0.
