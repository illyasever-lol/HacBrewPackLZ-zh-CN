# Builds the hardware self-test NSP with devkitA64 and hacBrewPackLZ.
# usage: .\build.ps1 -Keyset <prod.keys> [-Tool <hacbrewpacklz.exe>]
param(
    [Parameter(Mandatory = $true)][string]$Keyset,
    [string]$Tool = (Join-Path $PSScriptRoot '..\..\_tools\hacbrewpacklz\hacbrewpacklz.exe'),
    [string]$DevkitPro = 'C:\devkitPro'
)

$build = Join-Path $PSScriptRoot 'build'
$exefs = Join-Path $build 'exefs'
$control = Join-Path $build 'control'
$romfs = Join-Path $build 'romfs'
foreach ($dir in $exefs, $control) { New-Item -ItemType Directory -Force $dir | Out-Null }

$bin = Join-Path $DevkitPro 'devkitA64\bin'
$tools = Join-Path $DevkitPro 'tools\bin'
$libnx = Join-Path $DevkitPro 'libnx'
$elf = Join-Path $build 'main.elf'

$saved = $env:DEVKITPRO
$env:DEVKITPRO = $DevkitPro.Replace('\', '/')
try {
    & "$bin\aarch64-none-elf-gcc.exe" "-specs=$libnx\switch.specs" -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -O2 -Wall `
        -D__SWITCH__ "-I$libnx\include" (Join-Path $PSScriptRoot 'source\main.c') "-L$libnx\lib" -lnx -o $elf
    if ($LASTEXITCODE -ne 0) { throw 'compile failed' }
} finally {
    $env:DEVKITPRO = $saved
}

& "$tools\elf2nso.exe" $elf (Join-Path $exefs 'main')
if ($LASTEXITCODE -ne 0) { throw 'elf2nso failed' }
& "$tools\npdmtool.exe" (Join-Path $PSScriptRoot 'selftest.json') (Join-Path $exefs 'main.npdm') 2>$null
if ($LASTEXITCODE -ne 0) { throw 'npdmtool failed' }
& "$tools\nacptool.exe" --create 'hacBrewPackLZ self-test' 'hacBrewPackLZ' '1.0.0' (Join-Path $control 'control.nacp') | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'nacptool failed' }
Copy-Item (Join-Path $libnx 'default_icon.jpg') (Join-Path $control 'icon_AmericanEnglish.dat') -Force

if (-not (Test-Path $romfs)) {
    python -I (Join-Path $PSScriptRoot 'make_romfs.py') $romfs
    if ($LASTEXITCODE -ne 0) { throw 'make_romfs failed' }
}

& $Tool -k $Keyset --exefsdir $exefs --romfsdir $romfs --controldir $control --nologo `
    -o (Join-Path $PSScriptRoot 'hacbrewpacklz-selftest.nsp') --verify
if ($LASTEXITCODE -ne 0) { throw 'hacbrewpacklz failed' }
