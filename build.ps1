# build.ps1 - probe local toolchain, then build dsh-pet-standalone.
#
# Route selection:
#   1) MSVC (cl)       - when a developer prompt / VS environment is active
#   2) MinGW g++       - preferred here; gcc/g++ detected anywhere on PATH
#   3) fail            - point user at GitHub Actions instead
#
# Produces: build/dsh-pet-standalone.exe and runs --selftest (embedded-only,
# zero external files).
#
# Note: native tool stderr (g++ warnings etc.) must not abort the script;
# real failures are detected via $LASTEXITCODE / Test-Path below.
$ErrorActionPreference = 'Continue'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path   # repo root
$OutDir = Join-Path $Root 'build'
$Exe = Join-Path $OutDir 'dsh-pet-standalone.exe'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Get-VersionLine([string]$cmd, [string]$argline) {
    try { return (cmd /c "`"$cmd`" $argline 2>&1" | Select-Object -First 1) } catch { return '(?)' }
}

Write-Host '===== toolchain probe =====' -ForegroundColor Cyan
$cl      = Get-Command cl      -ErrorAction SilentlyContinue
$gpp     = Get-Command g++     -ErrorAction SilentlyContinue
$cmake   = Get-Command cmake   -ErrorAction SilentlyContinue
$ninja   = Get-Command ninja   -ErrorAction SilentlyContinue
$ffmpeg  = Get-Command ffmpeg  -ErrorAction SilentlyContinue
$ffprobe = Get-Command ffprobe -ErrorAction SilentlyContinue
$rustc   = Get-Command rustc   -ErrorAction SilentlyContinue
$node    = Get-Command node    -ErrorAction SilentlyContinue

"cl      : $(if ($cl) { $cl.Source + ' | ' + (Get-VersionLine $cl.Source '--version') } else { 'NOT FOUND (no MSVC)' })"
"g++     : $(if ($gpp) { $gpp.Source + ' | ' + (Get-VersionLine $gpp.Source '-dumpfullversion') } else { 'NOT FOUND' })"
"cmake   : $(if ($cmake) { $cmake.Source + ' | ' + (Get-VersionLine $cmake.Source '--version') } else { 'NOT FOUND' })"
"ninja   : $(if ($ninja) { $ninja.Source + ' | ' + (Get-VersionLine $ninja.Source '--version') } else { 'NOT FOUND' })"
"ffmpeg  : $(if ($ffmpeg) { $ffmpeg.Source } else { 'NOT FOUND' })"
"ffprobe : $(if ($ffprobe) { $ffprobe.Source } else { 'NOT FOUND' })"
"rustc   : $(if ($rustc) { $rustc.Source } else { 'NOT FOUND' })"
"node    : $(if ($node) { $node.Source } else { 'NOT FOUND' })"

if ($gpp) {
    # Always put the MinGW bin dir FIRST on PATH: cc1plus/ld load their companion DLLs
    # (libisl, libmpfr, libgmp, libzstd...) from mingw64\bin; if that dir sits later in
    # PATH, an earlier directory's same-named DLLs can shadow them and the compiler
    # dies silently (observed on systems whose PATH already lists mingw elsewhere).
    $mingwBin = Split-Path -Parent $gpp.Source
    $env:PATH = "$mingwBin;$env:PATH"
}

Remove-Item $Exe -ErrorAction SilentlyContinue

# ---- embedded single-exe pipeline: staging -> embedded.rc -> RCDATA ----
# v7 default (FULL QUALITY): the ORIGINAL upstream thumb WebM files are embedded
# (no re-encode; decoded in memory with libvpx at runtime). Set DSH_WEBM_SRC to
# the upstream assets/thumb dir, or keep the repo layout A (dsh-pet/assets/thumb
# checked out next to standalone/) so it is found automatically.
# Legacy small-size mode: set $env:EMBED_SMALL=1 to embed the old .pka v2 lossy
# packs instead (then set $env:DSH_REAL_PACKS like v6 did).
Write-Host '===== embedded assets =====' -ForegroundColor Cyan
$VpxRoot = $env:VPX_ROOT
if ($env:EMBED_SMALL -eq '1') {
    throw 'EMBED_SMALL (v2 .pka small-size mode) has been removed in v9 — this build only ships the full-quality dual-stream packs. Unset EMBED_SMALL and DSH_REAL_PACKS.'
} else {
    Write-Host '  FULL-QUALITY mode: convert upstream thumb WebM -> dual-stream (main + alpha) packs' -ForegroundColor Cyan
    # locate the upstream thumb dir
    $WebmSrc = $env:DSH_WEBM_SRC
    if (-not $WebmSrc) {
        foreach ($c in @((Join-Path $Root '..\dsh-pet\assets\thumb'), (Join-Path $Root 'assets\thumb'), (Join-Path $Root 'assets\embedded'))) {
            if (Test-Path $c) { $WebmSrc = $c; break }
        }
    }
    if (-not $WebmSrc -or -not (Test-Path $WebmSrc)) {
        throw 'FULL-QUALITY build needs the upstream thumb WebM dir. Set DSH_WEBM_SRC, or clone PC2005-cloud/dsh-pet next to this repo (layout A).'
    }
    $count = @(Get-ChildItem $WebmSrc -Filter '*.webm' | Measure-Object).Count
    Write-Host "  webm source: $WebmSrc ($count files)" -ForegroundColor Green
    if (-not $env:FFMPEG -or -not (Test-Path $env:FFMPEG)) {
        $ffCandidates = @(
            (Join-Path $Root '..\ffmpegdl\ext\imageio_ffmpeg\binaries\ffmpeg-win-x86_64-v7.1.exe'),
            (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
        )
        foreach ($fc in $ffCandidates) { if ($fc -and (Test-Path $fc)) { $env:FFMPEG = $fc; break } }
    }
    if (-not $env:FFMPEG -or -not (Test-Path $env:FFMPEG)) {
        throw 'FULL-QUALITY build needs ffmpeg (VP9 + libvpx). Set FFMPEG=<ffmpeg.exe> (e.g. the imageio-ffmpeg wheel).'
    }
    & python (Join-Path $Root 'scripts\stage-webm2.py') `
        --src $WebmSrc `
        --manifest (Join-Path $Root 'assets\embedded-manifest.json') `
        --out (Join-Path $Root 'assets\embedded') `
        --config (Join-Path $Root 'assets\default-config.jsonc') `
        --alpha-width 320 `
        --ffmpeg $env:FFMPEG
    if ($LASTEXITCODE -ne 0) { throw 'stage-webm2.py failed' }
    & python (Join-Path $Root 'scripts\rcgen.py')
    if ($LASTEXITCODE -ne 0) { throw 'rcgen.py failed' }

    # libvpx (static): libvpx.a + libwinpthread.a + headers. MSYS2 package
    # mingw-w64-x86_64-libvpx extracts to a mingw64/ dir; point VPX_ROOT at it.
    if (-not $VpxRoot -or -not (Test-Path (Join-Path $VpxRoot 'lib\libvpx.a'))) {
        throw 'FULL-QUALITY build needs libvpx static libs: set VPX_ROOT=<unpacked mingw-w64-x86_64-libvpx dir> (see README)'
    }
    Write-Host "  libvpx: $VpxRoot" -ForegroundColor Green
}
$RcEmbedded = Join-Path $Root 'src\embedded.rc'
$rcList = @('src\app.rc')
if (Test-Path $RcEmbedded) { $rcList += 'src\embedded.rc' } else {
    Write-Host '  WARN: src\embedded.rc missing — exe will have NO embedded animations' -ForegroundColor Yellow
}

if ($cl) {
    Write-Host '===== route: MSVC cl =====' -ForegroundColor Cyan
    Push-Location $Root
    try {
        & cl /nologo /std:c++17 /O2 /MT /utf-8 /EHsc /D_CRT_SECURE_NO_WARNINGS `
            /I src src\main.cpp src\pet.cpp src\anim.cpp src\inflate.cpp src\jsonc.cpp src\config.cpp src\resources.cpp src\bubble.cpp src\httpserver.cpp `
            /Fo"$OutDir\\" /Fe$Exe user32.lib gdi32.lib shell32.lib advapi32.lib ws2_32.lib gdiplus.lib 2>&1 | ForEach-Object { $_ }
        if (-not (Test-Path $Exe)) { throw 'MSVC build failed - no exe produced' }
    } finally { Pop-Location }
} elseif ($gpp) {
    # Direct g++ (no cmake needed; the same flags produce the CI build under MSVC).
    # NOTE: relative paths are used on purpose — some hosts/sandboxes silently fail
    # deep subprocess writes to absolute output paths; this form is battle-tested.
    Write-Host '===== route: MinGW g++ (static, stripped) =====' -ForegroundColor Cyan
    $log = Join-Path $OutDir 'gpp-log.txt'
    $extraSrc = @('src\webm.cpp')
    $extraInc = @()
    $extraLibs = @()
    if ($env:EMBED_SMALL -eq '1') {
        $extraSrc = @()   # webm backend compiled out (stub TU still included below)
    } else {
        $extraInc = @('-I', (Join-Path $VpxRoot 'include'))
        $extraLibs = @(
            (Join-Path $VpxRoot 'lib\libvpx.a'),
            (Join-Path $VpxRoot 'lib\libwinpthread.a')
        )
    }
    $cxx = @('-std=c++17', '-Os', '-s', '-static', '-fno-exceptions', '-fno-rtti', '-municode', '-mwindows',
             '-D_WIN32_WINNT=0x0601', '-ffunction-sections', '-fdata-sections', '-pipe', '-Wl,--gc-sections',
             '-I', 'src') + $extraInc + @('src\main.cpp', 'src\pet.cpp', 'src\anim.cpp', 'src\inflate.cpp',
             'src\jsonc.cpp', 'src\config.cpp', 'src\resources.cpp', 'src\bubble.cpp', 'src\httpserver.cpp') + $extraSrc + $rcList +
             $extraLibs + @('-o', 'build\dsh-pet-standalone.exe', '-luser32', '-lgdi32', '-lshell32', '-ladvapi32',
                            '-lws2_32', '-lgdiplus')
    Set-Location $Root
    & $gpp.Source @cxx 1> $log 2>&1
    if (-not (Test-Path $Exe)) {
        Write-Host 'g++ build failed (detail in build\gpp-log.txt)' -ForegroundColor Yellow
        Get-Content $log -ErrorAction SilentlyContinue | Select-Object -Last 25 | ForEach-Object { $_ }
        throw 'MinGW build failed - no exe produced'
    }
} else {
    Write-Host 'ERROR: no usable C++ toolchain (cl or g++).' -ForegroundColor Red
    Write-Host 'Options:' -ForegroundColor Yellow
    Write-Host '  * install MinGW-w64 (winlibs.com) and rerun this script' -ForegroundColor Yellow
    Write-Host '  * push the repo to GitHub and let Actions build it (see README)' -ForegroundColor Yellow
    exit 1
}

$size = (Get-Item $Exe).Length
Write-Host ("===== built: {0}  ({1:N1} KB) =====" -f $Exe, ($size / 1KB)) -ForegroundColor Green

Write-Host '===== DLL imports (objdump) =====' -ForegroundColor Cyan
if (Get-Command objdump -ErrorAction SilentlyContinue) {
    $imports = objdump -p $Exe | Select-String 'DLL Name'
    $imports | ForEach-Object { '  ' + $_.Line.Trim() }
    $nonSystem = $imports | Where-Object { $_.Line -notmatch 'KERNEL32|USER32|GDI32|SHELL32|ADVAPI32|msvcrt|ntdll|gdiplus|WS2_32' }
    if ($nonSystem) {
        Write-Host 'WARNING: non-system DLL imports:' -ForegroundColor Yellow
        $nonSystem | ForEach-Object { '  ' + $_.Line.Trim() }
    } else {
        Write-Host '  all imports are Windows system DLLs: OK' -ForegroundColor Green
    }
} else {
    Write-Host '  (objdump not found; skip import check)'
}

Write-Host '===== self-test =====' -ForegroundColor Cyan
# 1) embedded-only selftest: no external assets anywhere -> proves RCDATA decode,
#    350x197 geometry, tray API and the "no popup" precondition (embedded anims exist).
& $Exe --selftest 2>&1 | Out-Null
Start-Sleep -Milliseconds 600
$r = Get-Content (Join-Path $Root 'selftest-result.txt') -Encoding UTF8 -ErrorAction SilentlyContinue
$r | Select-Object -Last 3 | ForEach-Object { $_ }
if (-not ($r -match 'ALL SELFTESTS PASSED')) { Write-Host 'EMBEDDED SELF-TEST FAILED' -ForegroundColor Red; exit 1 }
Write-Host 'EMBEDDED SELF-TEST PASSED (single exe, zero external files)' -ForegroundColor Green
# 2) optional: external override path roundtrip with the synthetic test set.
$assets = Join-Path $Root '..\test-assets\packed'
if (Test-Path $assets) {
    & $Exe --selftest $assets 2>&1 | Out-Null
    Start-Sleep -Milliseconds 600
    $r2 = Get-Content (Join-Path $Root 'selftest-result.txt') -Encoding UTF8 -ErrorAction SilentlyContinue
    $r2 | Select-Object -Last 3 | ForEach-Object { $_ }
    if (-not ($r2 -match 'ALL SELFTESTS PASSED')) { Write-Host 'EXTERNAL SELF-TEST FAILED' -ForegroundColor Red; exit 1 }
    Write-Host 'EXTERNAL SELF-TEST PASSED (optional override path)' -ForegroundColor Green
}

Write-Host '===== done =====' -ForegroundColor Cyan
exit 0