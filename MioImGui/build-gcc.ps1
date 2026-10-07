# ---------------------------------------------------------------------------
# build-gcc.ps1 —— Mio 一键构建（MinGW-w64 GCC，无需 Visual Studio / Windows SDK）
# 用法:  powershell -NoProfile -ExecutionPolicy Bypass -File .\build-gcc.ps1
# 产物:  .\build-gcc\MioImGui.exe  +  .\build-gcc\MioZegoProbe.dll
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Stop'

# 你机器上的 MinGW-w64 (GCC 16.2.0, UCRT, x86_64)
$MINGW = 'D:\x86_64-16.2.0-release-win32-seh-ucrt-rt_v14-rev1\mingw64'

if (-not (Test-Path -LiteralPath $MINGW)) {
    throw "找不到 MinGW: $MINGW  （改这一行，或换回 MSVC 构建）"
}

# 关键：<MINGW>\x86_64-w64-mingw32\bin 里的 as.exe / ld.exe 依赖 <MINGW>\bin 下的 DLL。
# 不把 <MINGW>\bin 放进 PATH 会报 0xC0000135 (STATUS_DLL_NOT_FOUND)，
# 在 CMake 里表现为 “The C compiler ... is broken / not able to compile a simple test program”。
$env:PATH = "$MINGW\bin;$MINGW\x86_64-w64-mingw32\bin;" + $env:PATH

$proj  = $PSScriptRoot
$build = Join-Path $proj 'build-gcc'

Write-Host '[1/2] 配置 CMake ...' -ForegroundColor Cyan
cmake -S $proj -B $build -G "MinGW Makefiles" `
      -DCMAKE_MAKE_PROGRAM="$MINGW\bin\mingw32-make.exe" `
      -DCMAKE_C_COMPILER="$MINGW\bin\gcc.exe" `
      -DCMAKE_CXX_COMPILER="$MINGW\bin\g++.exe" `
      -DCMAKE_BUILD_TYPE=Release

Write-Host '[2/2] 编译 ...' -ForegroundColor Cyan
cmake --build $build --parallel

Write-Host ''
Write-Host '构建完成:' -ForegroundColor Green
Write-Host "  $build\MioImGui.exe"
Write-Host "  $build\MioZegoProbe.dll"
Write-Host ''
Write-Host '提醒: 使用顺序是「先开 Mio，再开 TT」。' -ForegroundColor Yellow