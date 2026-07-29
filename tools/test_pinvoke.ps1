# Drives build\aulos.dll through .NET P/Invoke - the exact path Unity takes.
# Offline (enableDevice = 0), so it measures the rendered buffer instead of
# trusting a device. Run after build_msvc.bat:
#   powershell -ExecutionPolicy Bypass -File tools\test_pinvoke.ps1
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$dll  = Join-Path $root 'build\aulos.dll'
if (-not (Test-Path $dll)) { throw "build\aulos.dll not found - run build_msvc.bat first" }

$src = @"
using System;
using System.Runtime.InteropServices;

public static class N
{
    const string LIB = @"__DLL__";

    [StructLayout(LayoutKind.Sequential)]
    public struct Vec3 { public float x, y, z;
        public Vec3(float a, float b, float c) { x = a; y = b; z = c; } }

    [StructLayout(LayoutKind.Sequential)]
    public struct Config { public uint sampleRate; public uint maxVoices;
        public int enableDevice; public IntPtr assetRoot; }

    [StructLayout(LayoutKind.Sequential)]
    public struct Stats { public uint activeVoices; public uint maxVoices;
        public ulong started, stolen, dropped, commandsDropped;
        public float peakLeft, peakRight; }

    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    public static extern int aul_create(ref Config config, out IntPtr system);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    public static extern void aul_destroy(IntPtr system);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    public static extern int aul_load_bank(IntPtr system, [MarshalAs(UnmanagedType.LPStr)] string path);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr aul_last_error(IntPtr system);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    public static extern void aul_update(IntPtr system);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    public static extern void aul_set_listener(IntPtr system, Vec3 p, Vec3 f, Vec3 u, Vec3 v);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    public static extern uint aul_play_3d(IntPtr system, [MarshalAs(UnmanagedType.LPStr)] string ev, Vec3 pos);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    public static extern void aul_stop(IntPtr system, uint inst, float fade);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    public static extern int aul_is_playing(IntPtr system, uint inst);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    public static extern void aul_set_parameter(IntPtr system, uint inst,
        [MarshalAs(UnmanagedType.LPStr)] string name, float value);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    public static extern void aul_set_bus_volume(IntPtr system, [MarshalAs(UnmanagedType.LPStr)] string bus, float v);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    public static extern float aul_get_bus_volume(IntPtr system, [MarshalAs(UnmanagedType.LPStr)] string bus);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    public static extern int aul_event_exists(IntPtr system, [MarshalAs(UnmanagedType.LPStr)] string ev);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    public static extern void aul_get_stats(IntPtr system, out Stats s);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    public static extern void aul_stop_all(IntPtr system, float fade);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    public static extern void aul_render(IntPtr system, [In, Out] float[] buf, uint frames);
}
"@.Replace('__DLL__', $dll)

Add-Type -TypeDefinition $src -Language CSharp

$script:fail = 0
function Check($name, $cond, $detail) {
    if ($cond) { "  PASS  {0,-46} {1}" -f $name, $detail }
    else       { "  FAIL  {0,-46} {1}" -f $name, $detail; $script:fail++ }
}

# Unity is left handed (forward = +z); the runtime is right handed
# (forward = -z). unity/Aulos.cs mirrors z - this reproduces that mapping.
function FromUnity([float]$x, [float]$y, [float]$z) { New-Object N+Vec3 $x, $y, (-$z) }

function PeakOf($sys, [int]$blocks) {
    $buf = New-Object float[] 1024
    $pl = 0.0; $pr = 0.0
    for ($i = 0; $i -lt $blocks; $i++) {
        [N]::aul_render($sys, $buf, 512)
        for ($k = 0; $k -lt 1024; $k += 2) {
            $l = [Math]::Abs($buf[$k]); $r = [Math]::Abs($buf[$k + 1])
            if ($l -gt $pl) { $pl = $l }
            if ($r -gt $pr) { $pr = $r }
        }
    }
    @($pl, $pr)
}

$assetRoot = [Runtime.InteropServices.Marshal]::StringToHGlobalAnsi((Join-Path $root 'assets'))
$cfg = New-Object N+Config
$cfg.sampleRate = 48000; $cfg.maxVoices = 32; $cfg.enableDevice = 0; $cfg.assetRoot = $assetRoot
$sys = [IntPtr]::Zero

"=== aulos.dll through .NET P/Invoke (the Unity path) ==="
$r = [N]::aul_create([ref]$cfg, [ref]$sys)
Check 'aul_create through the managed struct' ($r -eq 0 -and $sys -ne [IntPtr]::Zero) "result $r"

$r = [N]::aul_load_bank($sys, (Join-Path $root 'examples\demo_bank.json'))
$err = [Runtime.InteropServices.Marshal]::PtrToStringAnsi([N]::aul_last_error($sys))
Check 'aul_load_bank reads the demo bank' ($r -eq 0) "result $r ($err)"

Check 'aul_event_exists finds a bank event' ([N]::aul_event_exists($sys, 'vehicle_engine') -eq 1) 'vehicle_engine'
Check 'an unknown event is reported missing'  ([N]::aul_event_exists($sys, 'nope') -eq 0) 'nope'

[N]::aul_set_bus_volume($sys, 'music', 0.25)
[N]::aul_update($sys)
$bv = [N]::aul_get_bus_volume($sys, 'music')
Check 'bus volume round trips as float' ([Math]::Abs($bv - 0.25) -lt 0.001) "read back $bv"

# --- Unity coordinates: camera at the origin looking down +z (Unity forward) --
[N]::aul_set_listener($sys, (FromUnity 0 0 0), (FromUnity 0 0 1), (FromUnity 0 1 0), (FromUnity 0 0 0))

$h = [N]::aul_play_3d($sys, 'vehicle_engine', (FromUnity 8 0 0))
Check 'aul_play_3d returns a live handle' ($h -ne 0) "handle 0x$('{0:X}' -f $h)"
[N]::aul_set_parameter($sys, $h, 'rpm', 4200.0)
[N]::aul_update($sys)
Check 'the voice is playing after update' ([N]::aul_is_playing($sys, $h) -eq 1) 'is_playing 1'

$p = PeakOf $sys 200
Check 'aul_render fills the managed float[]' (($p[0] + $p[1]) -gt 0.001) ("peak L {0:F4} R {1:F4}" -f $p[0], $p[1])
Check 'a source at Unity +x is heard on the RIGHT' ($p[1] -gt 4 * $p[0]) `
    ("R/L {0:F1}x" -f ($p[1] / [Math]::Max($p[0], 1e-6)))

$st = New-Object N+Stats
[N]::aul_update($sys); [N]::aul_get_stats($sys, [ref]$st)
Check 'aul_get_stats marshals the struct' ($st.activeVoices -eq 1 -and $st.maxVoices -eq 32) `
    "active $($st.activeVoices)/$($st.maxVoices) started $($st.started) dropped $($st.dropped)"
Check 'no commands were dropped' ($st.commandsDropped -eq 0) "commands_dropped $($st.commandsDropped)"

[N]::aul_stop($sys, $h, 0.0)
[N]::aul_render($sys, (New-Object float[] 1024), 512)

# mirror image: the same source on the other side
$h2 = [N]::aul_play_3d($sys, 'vehicle_engine', (FromUnity -8 0 0))
[N]::aul_set_parameter($sys, $h2, 'rpm', 4200.0)
[N]::aul_update($sys)
$q = PeakOf $sys 200
Check 'a source at Unity -x is heard on the LEFT' ($q[0] -gt 4 * $q[1]) `
    ("L/R {0:F1}x" -f ($q[0] / [Math]::Max($q[1], 1e-6)))
Check 'both sides are equally loud' ([Math]::Abs($q[0] - $p[1]) -lt 0.02) `
    ("left {0:F4} vs right {1:F4}" -f $q[0], $p[1])

[N]::aul_stop_all($sys, 0.0)
[N]::aul_render($sys, (New-Object float[] 1024), 512)
[N]::aul_update($sys); [N]::aul_get_stats($sys, [ref]$st)
Check 'stop_all releases every voice' ($st.activeVoices -eq 0) "active $($st.activeVoices)"

[N]::aul_destroy($sys)
[Runtime.InteropServices.Marshal]::FreeHGlobal($assetRoot)
""
if ($script:fail -eq 0) { "ALL P/INVOKE CHECKS PASSED" } else { "$($script:fail) CHECK(S) FAILED"; exit 1 }
