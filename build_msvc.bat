@echo off
rem Aulos - MSVC build (Windows x64).
rem Produces build\aulos.dll (the Unity plugin), build\test_aulos.exe and
rem build\bench_aulos.exe. Needs Visual Studio 2019 or newer with the
rem "Desktop development with C++" workload. No other dependency.
setlocal
cd /d "%~dp0"

set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * ^
      -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
      -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS (
  for /d %%d in ("%ProgramFiles%\Microsoft Visual Studio\*") do (
    for /d %%e in ("%%d\*") do (
      if exist "%%e\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%e\VC\Auxiliary\Build\vcvars64.bat"
    )
  )
)
if not defined VCVARS (
  echo [ERR] no Visual Studio C++ toolchain found. Install the
  echo       "Desktop development with C++" workload, or run this from a
  echo       "x64 Native Tools Command Prompt".
  exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 (echo [ERR] vcvars64 failed & exit /b 1)
if not exist build mkdir build

echo [1/5] miniaudio
cl /nologo /c /O2 /W3 /I extern src\miniaudio_impl.c /Fobuild\miniaudio_impl.obj >build\_cl1.log 2>&1
if errorlevel 1 (echo [ERR] miniaudio & type build\_cl1.log & exit /b 1)

echo [2/5] aulos
cl /nologo /c /O2 /W4 /EHsc /std:c++17 /I include /I extern src\aulos.cpp /Fobuild\aulos.obj >build\_cl2.log 2>&1
if errorlevel 1 (echo [ERR] aulos & type build\_cl2.log & exit /b 1)

echo [3/5] tests
cl /nologo /O2 /EHsc /std:c++17 /I include /I extern tests\test_aulos.cpp build\aulos.obj build\miniaudio_impl.obj /Fobuild\ /Febuild\test_aulos.exe >build\_cl3.log 2>&1
if errorlevel 1 (echo [ERR] tests & type build\_cl3.log & exit /b 1)

echo [4/5] benchmark
cl /nologo /O2 /EHsc /std:c++17 /I include /I extern tests\bench_aulos.cpp build\aulos.obj build\miniaudio_impl.obj /Fobuild\ /Febuild\bench_aulos.exe >build\_cl4.log 2>&1
if errorlevel 1 (echo [ERR] bench & type build\_cl4.log & exit /b 1)

echo [5/5] aulos.dll
cl /nologo /LD /O2 /EHsc /std:c++17 /DAULOS_BUILD_SHARED /I include /I extern src\aulos.cpp src\miniaudio_impl.c /Fobuild\ /Febuild\aulos.dll >build\_cl5.log 2>&1
if errorlevel 1 (echo [ERR] dll & type build\_cl5.log & exit /b 1)

echo.
echo built: build\aulos.dll  build\test_aulos.exe  build\bench_aulos.exe
echo run:   build\test_aulos.exe examples\test_bank.json assets
echo        powershell -ExecutionPolicy Bypass -File tools\test_pinvoke.ps1
echo        powershell -ExecutionPolicy Bypass -File tools\make_unity_drop.ps1
