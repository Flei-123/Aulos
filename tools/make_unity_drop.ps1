# Assembles unity_drop\Assets - copy that folder into a Unity project and the
# runtime is live. Run after build_msvc.bat:
#   powershell -ExecutionPolicy Bypass -File tools\make_unity_drop.ps1
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$drop = Join-Path $root 'unity_drop'
$dll  = Join-Path $root 'build\aulos.dll'
if (-not (Test-Path $dll)) { throw "build\aulos.dll not found - run build_msvc.bat first" }

if (Test-Path $drop) { Remove-Item -Recurse -Force $drop }
foreach ($d in @('Assets\Plugins\x86_64', 'Assets\Aulos',
                 'Assets\StreamingAssets\banks', 'Assets\StreamingAssets\audio')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $drop $d) | Out-Null
}

Copy-Item $dll                       "$drop\Assets\Plugins\x86_64\aulos.dll" -Force
Copy-Item "$root\bindings\unity\*.cs"         "$drop\Assets\Aulos\" -Force
Copy-Item "$root\assets\*.wav"       "$drop\Assets\StreamingAssets\audio\" -Force

# The demo bank references samples flat ("engine_loop.wav"); Unity uses
# StreamingAssets as the asset root, so they get an "audio/" prefix.
$bank = Get-Content "$root\examples\demo_bank.json" -Raw
$bank = [regex]::Replace($bank, '"([A-Za-z0-9_]+\.wav)"', '"audio/$1"')
Set-Content -Path "$drop\Assets\StreamingAssets\banks\game.json" -Value $bank -Encoding UTF8

$readme = @'
Aulos - Unity drop-in
=====================

Copy the "Assets" folder into your Unity project (it merges with the existing one).

  Assets\Plugins\x86_64\aulos.dll        native runtime (x64, MSVC)
  Assets\Aulos\*.cs                      bindings + AulosListener + AulosSource
  Assets\StreamingAssets\banks\game.json the sound bank - edit this, no recompile
  Assets\StreamingAssets\audio\*.wav     the sample set

Scene setup
  1. Put AulosListener on the Main Camera. Leave "banks" at banks/game.json.
  2. Put AulosSource on anything that makes noise and type the event name:
     vehicle_engine, footstep, wind, radio, ui_beep, drone_beep.
  3. Press Play. Drive a parameter at runtime:
        GetComponent<AulosSource>().SetParameter("rpm", 4200f);

Notes
  - Unity is left handed, the runtime is right handed. Aulos.cs mirrors z for
    you, so a source to your right is heard on the right. Do not mirror twice.
  - Set the plugin platform to Windows x86_64 in the inspector if Unity does
    not pick it up from the Plugins\x86_64 folder.
  - Adding or retuning a sound = editing game.json. No C# change, no rebuild.
  - Editor play mode reloads the DLL on domain reload; AulosListener.OnDestroy
    shuts the system down so the handle does not leak between sessions.
'@
Set-Content -Path "$drop\LIESMICH.txt" -Value $readme -Encoding UTF8

Get-ChildItem -Recurse $drop | Where-Object { -not $_.PSIsContainer } |
    ForEach-Object { $_.FullName.Substring($drop.Length + 1) + '  (' + $_.Length + ' B)' }
