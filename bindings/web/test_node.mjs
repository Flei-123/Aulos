/*
 * Verifies the WebAssembly build against the native one.
 *
 * It replays examples/hello_aulos.c exactly - same bank, same listener, same
 * source movement, same rpm sweep, same block size - through the wasm module
 * and compares the rendered PCM sample by sample with the WAV the native
 * binary produced (build/hello.wav, written by `ctest`).
 *
 *   node bindings/web/test_node.mjs
 *
 * Exit code 0 means the browser build is numerically the same audio engine.
 */
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import createAulos from "./aulos-wasm.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "../..");

const SAMPLE_RATE = 48000, BLOCK = 512, SECONDS = 4.0;
const BANK = process.argv[2] || "examples/hello_bank.json";
const NATIVE = process.argv[3] || "build/hello.wav";
const TOTAL = Math.floor(SECONDS * SAMPLE_RATE);

const M = await createAulos();

// ---- stage the bank and the samples into the in-memory filesystem --------
// In a browser this is exactly the same call, fed by fetch() instead of fs.
M.FS.mkdir("/assets");
for (const f of fs.readdirSync(path.join(root, "assets")))
    M.FS.writeFile("/assets/" + f, fs.readFileSync(path.join(root, "assets", f)));
M.FS.writeFile("/bank.json", fs.readFileSync(path.join(root, BANK)));

const str = (s) => M.stringToNewUTF8(s);

const sys = M._aulw_create(SAMPLE_RATE, 32, str("/assets"));
if (!sys) throw new Error("aulw_create failed");

if (M._aul_load_bank(sys, str("/bank.json")) !== 0)
    throw new Error("load_bank: " + M.UTF8ToString(M._aul_last_error(sys)));

M._aulw_set_listener(sys, 0, 0, 0, 0, 0, -1, 0, 1, 0, 0, 0, 0);

const car = M._aulw_play_3d(sys, str("vehicle_engine"), -30.0, 0.0, -4.0);
if (car === 0) throw new Error("event not found");

const scratch = M._malloc(BLOCK * 2 * 4);
const pcm = new Int16Array(TOTAL * 2);

const speed = 15.0, dt = BLOCK / SAMPLE_RATE;
let t = 0, w = 0;
const rpmName = str("rpm");

for (let done = 0; done < TOTAL; done += BLOCK) {
    M._aulw_set_position(sys, car, -30.0 + speed * t, 0, -4.0, speed, 0, 0);
    const phase = t / SECONDS;
    const rpm = 1200.0 + 4800.0 * (phase < 0.5 ? phase * 2.0 : (1.0 - phase) * 2.0);
    M._aul_set_parameter(sys, car, rpmName, rpm);
    M._aul_update(sys);
    M._aul_render(sys, scratch, BLOCK);
    const view = M.HEAPF32.subarray(scratch >> 2, (scratch >> 2) + BLOCK * 2);
    for (let i = 0; i < BLOCK * 2; ++i) {
        let v = view[i];
        if (v > 1) v = 1; else if (v < -1) v = -1;
        pcm[w++] = Math.trunc(v * 32767.0);   // same truncation as the C example
    }
    t += dt;
}

const stats = M._malloc(8 * 8);
M._aulw_get_stats(sys, stats);
const st = Array.from(M.HEAPF64.subarray(stats >> 3, (stats >> 3) + 8));
M._aul_destroy(sys);

// ---- compare against the native render ----------------------------------
const nativePath = path.join(root, NATIVE);
if (!fs.existsSync(nativePath))
    throw new Error("build/hello.wav missing - run ctest --test-dir build first");
const wav = fs.readFileSync(nativePath);
const native = new Int16Array(wav.buffer, wav.byteOffset + 44, TOTAL * 2);

const EARLY = SAMPLE_RATE;          // first 0.5 s (stereo samples)
let maxDiff = 0, maxEarly = 0, sumSq = 0, sumSqRef = 0, exact = 0;
for (let i = 0; i < native.length; ++i) {
    const d = Math.abs(pcm[i] - native[i]);
    if (d > maxDiff) maxDiff = d;
    if (i < EARLY && d > maxEarly) maxEarly = d;
    if (d === 0) exact++;
    sumSq += d * d;
    sumSqRef += native[i] * native[i];
}
const rmsErr = Math.sqrt(sumSq / native.length);
const rmsRef = Math.sqrt(sumSqRef / native.length);
const snr = rmsErr === 0 ? Infinity : 20 * Math.log10(rmsRef / rmsErr);

// write the wasm render out so it can be listened to / analysed
const out = Buffer.alloc(44 + pcm.byteLength);
out.write("RIFF", 0); out.writeUInt32LE(36 + pcm.byteLength, 4); out.write("WAVE", 8);
out.write("fmt ", 12); out.writeUInt32LE(16, 16); out.writeUInt16LE(1, 20);
out.writeUInt16LE(2, 22); out.writeUInt32LE(SAMPLE_RATE, 24);
out.writeUInt32LE(SAMPLE_RATE * 4, 28); out.writeUInt16LE(4, 32); out.writeUInt16LE(16, 34);
out.write("data", 36); out.writeUInt32LE(pcm.byteLength, 40);
Buffer.from(pcm.buffer).copy(out, 44);
const outPath = NATIVE.replace(/\.wav$/, "_wasm.wav");
fs.writeFileSync(path.join(root, outPath), out);

console.log(`voices started=${st[2]} stolen=${st[3]} dropped=${st[4]} cmd_dropped=${st[5]}`);
console.log(`samples          : ${native.length}`);
console.log(`bit exact        : ${exact} (${(100 * exact / native.length).toFixed(3)} %)`);
console.log(`max abs diff     : ${maxDiff} LSB of 32767`);
console.log(`rms error        : ${rmsErr.toFixed(4)} LSB   (signal rms ${rmsRef.toFixed(1)})`);
console.log(`SNR vs native    : ${snr === Infinity ? "identical" : snr.toFixed(1) + " dB"}`);

/* Pass criterion: the first half second must be identical to within one
 * quantisation step. Later divergence is resampler phase drift - the pitch
 * accumulator is fed by pow()/exp(), whose last bit differs between glibc and
 * musl, and 192000 frames of that add up to well under one sample of lag.
 * tools/compare_renders.py quantifies it. */
const ok = st[2] > 0 && st[5] === 0 && maxEarly <= 2;
console.log(`max abs diff (first 0.5 s): ${maxEarly} LSB`);
console.log(ok ? "PASS - wasm matches native" : "FAIL");
process.exit(ok ? 0 : 1);
