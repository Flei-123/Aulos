/*
 * Aulos for the browser - main thread API.
 *
 *   import { Aulos } from "./aulos.js";
 *
 *   const ctx = new AudioContext({ sampleRate: 48000 });
 *   const aulos = await Aulos.create(ctx, {
 *       bank: "banks/game.json",
 *       assets: ["engine_loop.wav", "step_a.wav"],
 *       assetRoot: "assets/",
 *   });
 *   aulos.node.connect(ctx.destination);
 *
 *   const car = aulos.play3d("vehicle_engine", -30, 0, -4);
 *   aulos.setParameter(car, "rpm", 4200);
 *
 * Handles are minted here, not in the audio thread, so every call is
 * synchronous from the game's point of view - no promise per sound.
 */

const WORKLET_URL = new URL("./aulos-worklet.js", import.meta.url);

export class Aulos {
    constructor(node, sampleRate) {
        this.node = node;
        this.sampleRate = sampleRate;
        this.nextId = 1;
        this.stats = {
            active: 0, maxVoices: 0, started: 0, stolen: 0,
            dropped: 0, commandsDropped: 0, peakLeft: 0, peakRight: 0,
        };
        node.port.onmessage = (e) => {
            if (e.data.type === "stats") this.stats = e.data;
            else if (e.data.type === "error") console.error("[aulos]", e.data.error);
        };
    }

    static async create(ctx, opts) {
        const assetRoot = opts.assetRoot ?? "assets/";
        await ctx.audioWorklet.addModule(opts.workletUrl ?? WORKLET_URL);

        const bank = await (await fetch(opts.bank)).arrayBuffer();
        const assets = {};
        await Promise.all((opts.assets || []).map(async (name) => {
            assets[name] = await (await fetch(assetRoot + name)).arrayBuffer();
        }));

        const node = new AudioWorkletNode(ctx, "aulos", {
            numberOfInputs: 0,
            numberOfOutputs: 1,
            outputChannelCount: [2],
            processorOptions: { bank, assets, maxVoices: opts.maxVoices ?? 64 },
        });

        const ready = new Promise((resolve, reject) => {
            const onMsg = (e) => {
                if (e.data.type === "ready") { node.port.removeEventListener("message", onMsg); resolve(); }
                else if (e.data.type === "error") reject(new Error(e.data.error));
            };
            node.port.addEventListener("message", onMsg);
            node.port.start();
        });
        await ready;

        return new Aulos(node, ctx.sampleRate);
    }

    _send(m) { this.node.port.postMessage(m); }

    play(event) {
        const id = this.nextId++;
        this._send({ type: "play", id, event });
        return id;
    }

    play3d(event, x, y, z) {
        const id = this.nextId++;
        this._send({ type: "play3d", id, event, x, y, z });
        return id;
    }

    stop(id, fade = 0)            { this._send({ type: "stop", id, fade }); }
    stopAll(fade = 0)             { this._send({ type: "stopAll", fade }); }
    setVolume(id, value)          { this._send({ type: "volume", id, value }); }
    setPitch(id, value)           { this._send({ type: "pitch", id, value }); }
    setParameter(id, name, value) { this._send({ type: "parameter", id, name, value }); }
    setBusVolume(name, value)     { this._send({ type: "bus", name, value }); }

    setPosition(id, x, y, z, vx = 0, vy = 0, vz = 0) {
        this._send({ type: "position", id, x, y, z, vx, vy, vz });
    }

    /* Right handed, listener looks down -forward: the OpenGL / Godot
     * convention, same as the native API. Mirror z for left handed hosts. */
    setListener(px, py, pz, fx = 0, fy = 0, fz = -1, ux = 0, uy = 1, uz = 0,
                vx = 0, vy = 0, vz = 0) {
        this._send({ type: "listener", px, py, pz, fx, fy, fz, ux, uy, uz, vx, vy, vz });
    }
}

export default Aulos;
