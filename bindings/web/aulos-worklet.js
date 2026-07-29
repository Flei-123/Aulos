/*
 * Aulos AudioWorkletProcessor.
 *
 * The whole runtime lives in here, on the audio thread: the wasm module is
 * instantiated inside the AudioWorkletGlobalScope and process() calls
 * aulw_render_planar() straight into the output buffers. Nothing is copied
 * across a thread boundary per block.
 *
 * Note what this does NOT need: no SharedArrayBuffer, no pthreads, no
 * cross-origin isolation, so no COOP/COEP headers and it works inside a
 * sandboxed iframe. The wasm binary is inlined in aulos-wasm.mjs precisely
 * because an AudioWorkletGlobalScope has no fetch() to load a side file with.
 *
 * Control messages arrive over port.postMessage and are applied between
 * blocks. The main thread mints its own handle ids, so it can call
 * stop(handle) in the same frame it called play() without a round trip.
 */
import createAulos from "./aulos-wasm.mjs";

// AudioWorkletGlobalScope exposes neither TextDecoder nor TextEncoder in some
// browsers; the emscripten runtime probes for them, so give it working ones.
if (typeof TextDecoder === "undefined") {
    globalThis.TextDecoder = class {
        decode(buf) {
            let s = "";
            const b = buf instanceof Uint8Array ? buf : new Uint8Array(buf.buffer || buf);
            for (let i = 0; i < b.length; ++i) {
                const c = b[i];
                if (c === 0) break;
                s += String.fromCharCode(c);   // banks and event names are ASCII
            }
            return s;
        }
    };
}

class AulosProcessor extends AudioWorkletProcessor {
    constructor(options) {
        super();
        this.ready = false;
        this.sys = 0;
        this.handles = new Map();      // main-thread id -> aul_instance
        this.pending = [];
        this.frames = 0;
        this.port.onmessage = (e) => this.onMessage(e.data);
        this.boot(options.processorOptions || {});
    }

    async boot(opts) {
        const M = await createAulos();
        this.M = M;

        // stage the bank and the samples into the module's in-memory filesystem
        M.FS.mkdir("/assets");
        for (const [name, bytes] of Object.entries(opts.assets || {}))
            M.FS.writeFile("/assets/" + name, new Uint8Array(bytes));
        M.FS.writeFile("/bank.json", new Uint8Array(opts.bank));

        const root = M.stringToNewUTF8("/assets");
        this.sys = M._aulw_create(sampleRate, opts.maxVoices || 64, root);
        if (!this.sys) { this.port.postMessage({ type: "error", error: "aulw_create failed" }); return; }

        const bankPath = M.stringToNewUTF8("/bank.json");
        if (M._aul_load_bank(this.sys, bankPath) !== 0) {
            this.port.postMessage({ type: "error", error: M.UTF8ToString(M._aul_last_error(this.sys)) });
            return;
        }

        this.maxBlock = 1024;
        this.scratch = M._malloc(this.maxBlock * 2 * 4);
        this.left = M._malloc(this.maxBlock * 4);
        this.right = M._malloc(this.maxBlock * 4);
        this.statsPtr = M._malloc(8 * 8);
        this.strCache = new Map();

        this.ready = true;
        for (const m of this.pending) this.apply(m);
        this.pending.length = 0;
        this.port.postMessage({ type: "ready", sampleRate });
    }

    str(s) {                            // event / parameter / bus names are reused every frame
        let p = this.strCache.get(s);
        if (!p) { p = this.M.stringToNewUTF8(s); this.strCache.set(s, p); }
        return p;
    }

    onMessage(m) {
        if (!this.ready) { this.pending.push(m); return; }
        this.apply(m);
    }

    apply(m) {
        const M = this.M, sys = this.sys;
        switch (m.type) {
            case "play":
                this.handles.set(m.id, M._aul_play(sys, this.str(m.event)));
                break;
            case "play3d":
                this.handles.set(m.id, M._aulw_play_3d(sys, this.str(m.event), m.x, m.y, m.z));
                break;
            case "stop":
                M._aul_stop(sys, this.handles.get(m.id) || 0, m.fade || 0);
                this.handles.delete(m.id);
                break;
            case "stopAll":
                M._aul_stop_all(sys, m.fade || 0);
                this.handles.clear();
                break;
            case "position":
                M._aulw_set_position(sys, this.handles.get(m.id) || 0, m.x, m.y, m.z, m.vx, m.vy, m.vz);
                break;
            case "volume":
                M._aul_set_volume(sys, this.handles.get(m.id) || 0, m.value);
                break;
            case "pitch":
                M._aul_set_pitch(sys, this.handles.get(m.id) || 0, m.value);
                break;
            case "parameter":
                M._aul_set_parameter(sys, this.handles.get(m.id) || 0, this.str(m.name), m.value);
                break;
            case "listener":
                M._aulw_set_listener(sys, m.px, m.py, m.pz, m.fx, m.fy, m.fz,
                                     m.ux, m.uy, m.uz, m.vx, m.vy, m.vz);
                break;
            case "bus":
                M._aul_set_bus_volume(sys, this.str(m.name), m.value);
                break;
        }
    }

    process(inputs, outputs) {
        const out = outputs[0];
        if (!this.ready) return true;
        const n = out[0].length;
        const M = this.M;

        M._aul_update(this.sys);                      // what a game does per frame
        M._aulw_render_planar(this.sys, this.scratch, this.left, this.right, n);

        out[0].set(M.HEAPF32.subarray(this.left >> 2, (this.left >> 2) + n));
        if (out.length > 1)
            out[1].set(M.HEAPF32.subarray(this.right >> 2, (this.right >> 2) + n));

        this.frames += n;
        if (this.frames >= sampleRate / 10) {         // ~10 stats updates per second
            this.frames = 0;
            M._aulw_get_stats(this.sys, this.statsPtr);
            const s = M.HEAPF64.subarray(this.statsPtr >> 3, (this.statsPtr >> 3) + 8);
            this.port.postMessage({
                type: "stats",
                active: s[0], maxVoices: s[1], started: s[2], stolen: s[3],
                dropped: s[4], commandsDropped: s[5], peakLeft: s[6], peakRight: s[7],
            });
        }
        return true;
    }
}

registerProcessor("aulos", AulosProcessor);
