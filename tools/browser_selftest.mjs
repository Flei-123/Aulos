/*
 * Headless browser check for the WebAssembly build.
 *
 * Loads bindings/web/demo in a real Chromium, runs the OfflineAudioContext
 * self test (which renders through an AudioWorkletProcessor, i.e. on the audio
 * thread) and diffs the result against the WAV the native binary wrote for the
 * same bank. The page is served without COOP/COEP on purpose: if the build
 * needed cross-origin isolation, this would fail.
 *
 *   python3 tools/serve_web_demo.py 8099 &
 *   node tools/browser_selftest.mjs [url]
 */
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const url = process.argv[2] || "http://localhost:8099/bindings/web/demo/";

// ---- native reference ----------------------------------------------------
const wav = fs.readFileSync(path.join(root, "build/flat.wav"));
const native = new Int16Array(wav.buffer, wav.byteOffset + 44,
                             (wav.length - 44) / 2);
let nativePeak = 0;
for (let i = 0; i < native.length; ++i)
    if (Math.abs(native[i]) > nativePeak) nativePeak = Math.abs(native[i]);
const nativeHead = [];
for (let i = 0; i < 8; ++i) nativeHead.push(native[i * 2]);   // left channel

// ---- browser run ---------------------------------------------------------
const browser = await chromium.launch({ args: ["--autoplay-policy=no-user-gesture-required"] });
const page = await browser.newPage();
const errors = [];
page.on("pageerror", (e) => errors.push(String(e)));
page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });

await page.goto(url, { waitUntil: "networkidle" });
await page.getByRole("button", { name: /Render 4 s offline/ }).click();
await page.waitForFunction(() => document.getElementById("log").textContent.includes("rendered"),
                           null, { timeout: 60000 });
const report = await page.textContent("#log");
const isolated = await page.evaluate(() => globalThis.crossOriginIsolated === true);
await browser.close();

console.log(report);
console.log("crossOriginIsolated:", isolated);
if (errors.length) { console.error("page errors:\n" + errors.join("\n")); process.exit(1); }

// ---- compare -------------------------------------------------------------
const num = (label) => {
    const m = report.match(new RegExp(label + "\\s*:\\s*([-\\d.e ]+)"));
    if (!m) throw new Error("missing line: " + label);
    return m[1].trim().split(/\s+/).map(Number);
};

const frames = Number(report.match(/rendered (\d+) frames/)[1]);
const peak = num("peak")[0];
const head = num("first 8 samples \\(L\\)");
const started = Number(report.match(/started=(\d+)/)[1]);
const dropped = Number(report.match(/dropped=(\d+)/)[1]);

let ok = true;
const check = (name, cond, detail) => {
    console.log(`${cond ? "ok  " : "FAIL"}  ${name}${detail ? "  " + detail : ""}`);
    if (!cond) ok = false;
};

check("rendered 4 s", frames === 192000, `${frames} frames`);
check("one voice started, none dropped", started === 1 && dropped === 0);
check("no cross-origin isolation needed", isolated === false);

// the reference WAV is 16 bit, so equality is expected to that precision
const headInt = head.map((v) => Math.trunc(v * 32767));
check("first 8 samples equal native",
      headInt.every((v, i) => v === nativeHead[i]),
      `browser ${headInt.join(",")} vs native ${nativeHead.join(",")}`);

const peakInt = Math.trunc(peak * 32767);
check("peak equal native within 1 LSB", Math.abs(peakInt - nativePeak) <= 1,
      `browser ${peakInt} vs native ${nativePeak}`);

console.log(ok ? "PASS - browser render matches native" : "FAIL");
process.exit(ok ? 0 : 1);
