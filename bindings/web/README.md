# Aulos in the browser

The same C++ runtime as everywhere else, compiled to WebAssembly and pulled by
an `AudioWorkletProcessor`. Banks, buses, 3D panning, distance rolloff,
doppler, parameter curves and priority based voice stealing behave exactly as
they do natively - verified sample by sample against the native build, not
assumed (see *Verification* below).

```js
import { Aulos } from "aulos-audio";

const ctx = new AudioContext({ sampleRate: 48000 });
const aulos = await Aulos.create(ctx, {
    bank: "banks/game.json",
    assets: ["engine_loop.wav", "beep.wav"],
    assetRoot: "assets/",
});
aulos.node.connect(ctx.destination);

aulos.setListener(0, 0, 0);                        // right handed, looks down -z
const car = aulos.play3d("vehicle_engine", -30, 0, -4);

function frame(dt) {
    x += 15 * dt;
    aulos.setPosition(car, x, 0, -4, 15, 0, 0);    // velocity feeds doppler
    aulos.setParameter(car, "rpm", rpm);           // the bank maps rpm -> pitch
}
```

`play()` / `play3d()` return a handle synchronously - the handle is minted on
the main thread and resolved inside the worklet - so you can stop or modulate a
sound in the same frame you started it, without awaiting anything.

## What it does not need

- **No `SharedArrayBuffer`**, no `pthreads`, no `-sAUDIO_WORKLET`.
- **No COOP/COEP headers**, so no cross-origin isolation - it runs inside a
  sandboxed iframe, on a plain static host, in a CodeSandbox-style preview.
- **No `.wasm` side file.** An `AudioWorkletGlobalScope` has no `fetch()`, so
  the binary is inlined into `aulos-wasm.mjs` (`-sSINGLE_FILE`). One import,
  388 KB, done.

The runtime is single threaded by construction: the whole mixer lives in the
worklet and control messages arrive over `port.postMessage`, which is why none
of the above is required. Nothing is copied per audio block - `process()`
renders straight into the output buffers.

## Building

```bash
git clone https://github.com/emscripten-core/emsdk /opt/emsdk
/opt/emsdk/emsdk install latest && /opt/emsdk/emsdk activate latest
export PATH=$PATH:/opt/emsdk/upstream/emscripten

bindings/web/build_wasm.sh          # -> bindings/web/aulos-wasm.mjs
```

The build defines `AULOS_NO_DEVICE`, which compiles miniaudio's device layer
out (`MA_NO_DEVICE_IO`); the browser owns the device and pulls from us.
Decoding and resampling stay in, so WAV assets are loaded by the same code path
as on desktop.

## Verification

`bindings/web/test_node.mjs` replays `examples/hello_aulos.c` - same bank, same
listener, same source movement, same 512 frame blocks - through the wasm module
under Node and diffs the PCM against the WAV the native binary wrote.

| bank | result |
|---|---|
| `tools/flat_bank.json` (nothing resamples) | **384000 / 384000 samples bit identical**, max diff 0 |
| `examples/hello_bank.json` (pitch sweep + doppler) | first 0.5 s within 1 LSB; then sub-sample drift, spectrum cosine 0.99997, energy ratio 0.99992 |

The residual on the second row is the pitch accumulator: it is fed by
`pow()`/`exp()`, whose last bit differs between glibc and musl, and 192000
frames of that add up to under one sample of lag (`tools/compare_renders.py`
prints the per-segment breakdown and the correlation lag). It is not a
behavioural difference - with resampling switched off the two builds agree bit
for bit.

`tools/browser_selftest.mjs` then does the same thing in a real headless
Chromium through an `OfflineAudioContext` + `AudioWorkletNode`, and asserts
`crossOriginIsolated === false` while it does. Both run in CI on every push.

## Demo

```bash
python3 tools/serve_web_demo.py 8099
# http://localhost:8099/bindings/web/demo/
```

A live engine sound you can drag through the stereo field with an rpm slider,
plus the offline self test.

## API

| method | notes |
|---|---|
| `Aulos.create(ctx, {bank, assets, assetRoot, maxVoices, workletUrl})` | fetches the bank and samples, boots the worklet |
| `play(event) -> handle` | 2D |
| `play3d(event, x, y, z) -> handle` | spatial |
| `stop(handle, fade)`, `stopAll(fade)` | fade in seconds |
| `setPosition(handle, x, y, z, vx, vy, vz)` | velocity drives doppler |
| `setVolume`, `setPitch`, `setParameter(handle, name, value)` | parameter names come from the bank |
| `setBusVolume(name, value)` | bus tree from the bank |
| `setListener(px,py,pz, fx,fy,fz, ux,uy,uz, vx,vy,vz)` | right handed, mirror z for left handed engines |
| `aulos.stats` | `{active, started, stolen, dropped, commandsDropped, peakLeft, peakRight}`, refreshed 10x/s |
| `aulos.node` | the `AudioWorkletNode` - route it through your own graph if you want |
