// Aulos - C# bindings for Unity (and any other .NET host).
//
// Put the native library next to this file in Assets/Plugins:
//   Windows  Assets/Plugins/x86_64/aulos.dll
//   Linux    Assets/Plugins/x86_64/libaulos.so
//   macOS    Assets/Plugins/aulos.bundle
//
// Build the shared library with:
//   cmake -B build -DAULOS_SHARED=ON && cmake --build build --config Release
//
// The bindings are a thin 1:1 mapping of include/aulos.h. Gameplay code should
// use AulosRuntime, which owns a single system instance.
using System;
using System.Runtime.InteropServices;
using UnityEngine;

namespace Aulos
{
    public enum AulResult
    {
        Ok = 0,
        InvalidArg = -1,
        OutOfMemory = -2,
        File = -3,
        Parse = -4,
        Device = -5,
        NotFound = -6
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct AulVec3
    {
        public float x, y, z;

        public AulVec3(float x, float y, float z) { this.x = x; this.y = y; this.z = z; }

        // Handedness matters for panning: the runtime is right handed with the
        // listener looking down -z (the OpenGL convention, see aulos.h), Unity
        // is left handed and looks down +z. Mirroring z converts between the
        // two. Without this a car on your right would be heard on your left.
        // The mirror is an isometry, so distances, doppler and rolloff are
        // untouched - only the left/right axis flips.
        public static AulVec3 From(Vector3 v) => new AulVec3(v.x, v.y, -v.z);
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct AulConfig
    {
        public uint sampleRate;
        public uint maxVoices;
        public int enableDevice;
        public IntPtr assetRoot;      // set through AulosRuntime, not by hand
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct AulStats
    {
        public uint activeVoices;
        public uint maxVoices;
        public ulong started;
        public ulong stolen;
        public ulong dropped;
        public ulong commandsDropped;
        public float peakLeft;
        public float peakRight;
    }

    /// <summary>Raw P/Invoke surface. Prefer AulosRuntime.</summary>
    public static class Native
    {
#if UNITY_IOS && !UNITY_EDITOR
        private const string LIB = "__Internal";
#else
        private const string LIB = "aulos";
#endif

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        public static extern AulResult aul_create(ref AulConfig config, out IntPtr system);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        public static extern void aul_destroy(IntPtr system);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        public static extern AulResult aul_load_bank(IntPtr system,
            [MarshalAs(UnmanagedType.LPStr)] string path);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr aul_last_error(IntPtr system);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        public static extern void aul_update(IntPtr system);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        public static extern void aul_set_listener(IntPtr system, AulVec3 position,
            AulVec3 forward, AulVec3 up, AulVec3 velocity);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        public static extern uint aul_play(IntPtr system,
            [MarshalAs(UnmanagedType.LPStr)] string eventName);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        public static extern uint aul_play_3d(IntPtr system,
            [MarshalAs(UnmanagedType.LPStr)] string eventName, AulVec3 position);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        public static extern void aul_stop(IntPtr system, uint instance, float fadeSeconds);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        public static extern void aul_stop_all(IntPtr system, float fadeSeconds);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        public static extern int aul_is_playing(IntPtr system, uint instance);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        public static extern void aul_set_position(IntPtr system, uint instance,
            AulVec3 position, AulVec3 velocity);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        public static extern void aul_set_volume(IntPtr system, uint instance, float volume);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        public static extern void aul_set_pitch(IntPtr system, uint instance, float pitch);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        public static extern void aul_set_parameter(IntPtr system, uint instance,
            [MarshalAs(UnmanagedType.LPStr)] string name, float value);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        public static extern void aul_set_bus_volume(IntPtr system,
            [MarshalAs(UnmanagedType.LPStr)] string bus, float volume);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        public static extern float aul_get_bus_volume(IntPtr system,
            [MarshalAs(UnmanagedType.LPStr)] string bus);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        public static extern void aul_get_stats(IntPtr system, out AulStats stats);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        public static extern int aul_event_exists(IntPtr system,
            [MarshalAs(UnmanagedType.LPStr)] string eventName);

        /// <summary>Offline rendering. Only valid when the system was created
        /// with enableDevice = 0; with a device open, the audio callback owns
        /// this call and you must not touch it.</summary>
        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        public static extern void aul_render(IntPtr system,
            [In, Out] float[] interleavedStereo, uint frameCount);
    }

    /// <summary>
    /// One process wide Aulos system. AulosListener creates it, everything else
    /// just calls the static helpers.
    ///
    /// Every call is a no-op while the system is down, so gameplay code never
    /// needs a null check and a build without the plugin degrades to silence
    /// instead of a crash.
    /// </summary>
    public static class AulosRuntime
    {
        private static IntPtr _sys = IntPtr.Zero;

        public static bool IsRunning => _sys != IntPtr.Zero;

        public static bool Create(uint sampleRate = 48000, uint maxVoices = 64,
                                  string assetRoot = null)
        {
            if (_sys != IntPtr.Zero) return true;

            assetRoot = assetRoot ?? Application.streamingAssetsPath;
            IntPtr root = Marshal.StringToHGlobalAnsi(assetRoot);
            try
            {
                var cfg = new AulConfig
                {
                    sampleRate = sampleRate,
                    maxVoices = maxVoices,
                    enableDevice = 1,
                    assetRoot = root
                };
                var r = Native.aul_create(ref cfg, out _sys);
                if (r != AulResult.Ok)
                {
                    Debug.LogError($"[Aulos] aul_create failed: {r}");
                    _sys = IntPtr.Zero;
                    return false;
                }
                return true;
            }
            catch (DllNotFoundException e)
            {
                Debug.LogError("[Aulos] native library not found in Assets/Plugins: " + e.Message);
                _sys = IntPtr.Zero;
                return false;
            }
            finally
            {
                Marshal.FreeHGlobal(root);
            }
        }

        public static void Destroy()
        {
            if (_sys == IntPtr.Zero) return;
            Native.aul_destroy(_sys);
            _sys = IntPtr.Zero;
        }

        public static string LastError()
        {
            if (_sys == IntPtr.Zero) return "aulos is not running";
            return Marshal.PtrToStringAnsi(Native.aul_last_error(_sys)) ?? "unknown";
        }

        public static bool LoadBank(string path)
        {
            if (_sys == IntPtr.Zero) return false;
            var r = Native.aul_load_bank(_sys, path);
            if (r != AulResult.Ok) Debug.LogError($"[Aulos] bank: {LastError()}");
            return r == AulResult.Ok;
        }

        public static void Update() { if (_sys != IntPtr.Zero) Native.aul_update(_sys); }

        public static void SetListener(Vector3 pos, Vector3 forward, Vector3 up, Vector3 velocity)
        {
            if (_sys == IntPtr.Zero) return;
            Native.aul_set_listener(_sys, AulVec3.From(pos), AulVec3.From(forward),
                                    AulVec3.From(up), AulVec3.From(velocity));
        }

        public static uint Play(string ev) =>
            _sys == IntPtr.Zero ? 0u : Native.aul_play(_sys, ev);

        public static uint Play3D(string ev, Vector3 pos) =>
            _sys == IntPtr.Zero ? 0u : Native.aul_play_3d(_sys, ev, AulVec3.From(pos));

        public static void Stop(uint inst, float fade = 0.05f)
        { if (_sys != IntPtr.Zero) Native.aul_stop(_sys, inst, fade); }

        public static void StopAll(float fade = 0.1f)
        { if (_sys != IntPtr.Zero) Native.aul_stop_all(_sys, fade); }

        public static bool IsPlaying(uint inst) =>
            _sys != IntPtr.Zero && Native.aul_is_playing(_sys, inst) != 0;

        public static void SetPosition(uint inst, Vector3 pos, Vector3 velocity)
        { if (_sys != IntPtr.Zero) Native.aul_set_position(_sys, inst, AulVec3.From(pos), AulVec3.From(velocity)); }

        public static void SetVolume(uint inst, float v)
        { if (_sys != IntPtr.Zero) Native.aul_set_volume(_sys, inst, v); }

        public static void SetPitch(uint inst, float v)
        { if (_sys != IntPtr.Zero) Native.aul_set_pitch(_sys, inst, v); }

        public static void SetParameter(uint inst, string name, float value)
        { if (_sys != IntPtr.Zero) Native.aul_set_parameter(_sys, inst, name, value); }

        public static void SetBusVolume(string bus, float v)
        { if (_sys != IntPtr.Zero) Native.aul_set_bus_volume(_sys, bus, v); }

        public static float GetBusVolume(string bus) =>
            _sys == IntPtr.Zero ? 0f : Native.aul_get_bus_volume(_sys, bus);

        public static bool EventExists(string ev) =>
            _sys != IntPtr.Zero && Native.aul_event_exists(_sys, ev) != 0;

        public static AulStats Stats()
        {
            if (_sys == IntPtr.Zero) return default;
            Native.aul_get_stats(_sys, out var s);
            return s;
        }
    }
}
