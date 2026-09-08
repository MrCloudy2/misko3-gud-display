# ------------------------------------------------------------------
# Flash the MiSKo3 firmware from Windows.
#
# The PowerShell counterpart of flash.sh. It finds whichever programmer is
# installed, picks the file that tool wants, and then checks that the board
# enumerated. Nothing is compiled, so no ARM toolchain is needed here.
#
#   powershell -ExecutionPolicy Bypass -File .\flash.ps1
#
# NOTE: this script has not been run on Windows. flash.sh is the tested path.
# If something here is wrong, the fallback is always to open build\misko3.hex
# in the STM32CubeProgrammer GUI and press Download.
# ------------------------------------------------------------------

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot

$Build  = 'build'
$VidPid = 'VID_1D50&PID_614D'

if (-not (Test-Path $Build)) {
    Write-Error "No $Build directory. Build first, or copy a prebuilt one."
}

# ---- find a programmer ------------------------------------------------
$cubeCandidates = @(
    "$env:ProgramFiles\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe"
    "${env:ProgramFiles(x86)}\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe"
) + @(
    Get-ChildItem -Path "C:\ST", "$env:LOCALAPPDATA\Programs" -Recurse -Filter 'STM32_Programmer_CLI.exe' `
        -ErrorAction SilentlyContinue -Depth 6 | ForEach-Object { $_.FullName }
)
$cube = $cubeCandidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1

function Have($name) { $null -ne (Get-Command $name -ErrorAction SilentlyContinue) }

# SWD is capped at 1000 kHz everywhere. About twenty FMC bus pins switch right
# beside SWDIO, and at full speed debug transfers get corrupted.
if     ($cube -and (Test-Path "$Build\misko3.hex")) { $tool = 'cube' }
elseif ((Have 'st-flash') -and (Test-Path "$Build\misko3.bin")) { $tool = 'stflash' }
elseif ((Have 'openocd')  -and (Test-Path "$Build\misko3.elf")) { $tool = 'openocd' }
else {
    Write-Host "No programmer found."
    Write-Host ""
    Write-Host "In ${Build}:"
    Get-ChildItem $Build -ErrorAction SilentlyContinue | ForEach-Object { "  $($_.Name)" }
    Write-Host ""
    Write-Host "Install STM32CubeProgrammer:"
    Write-Host "  https://www.st.com/en/development-tools/stm32cubeprog.html"
    exit 1
}

Write-Host "Tool: $tool"
Write-Host ""

switch ($tool) {
    'cube'    { & $cube -c port=SWD freq=1000 -w "$Build\misko3.hex" -rst }
    'stflash' { & st-flash --freq=1000k --reset write "$Build\misko3.bin" 0x08000000 }
    'openocd' { & openocd -f interface/stlink.cfg -c "transport select hla_swd" `
                          -f target/stm32g4x.cfg -c "adapter speed 1000" `
                          -c "program $Build/misko3.elf verify reset exit" }
}

# ---- did it enumerate -------------------------------------------------
Write-Host ""
Write-Host "Waiting for the device to enumerate ..."

for ($i = 0; $i -lt 10; $i++) {
    $dev = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
           Where-Object { $_.InstanceId -match $VidPid }
    if ($dev) {
        Write-Host ""
        Write-Host "USB: OK"
        $dev | ForEach-Object { "  $($_.FriendlyName)" }
        Write-Host ""
        Write-Host "The gamepad works on Windows. The display does not:"
        Write-Host "there is no GUD driver for Windows, so interface 0 stays unclaimed."
        exit 0
    }
    Start-Sleep -Seconds 1
}

Write-Host ""
Write-Host "The device did not enumerate within 10 seconds."
Write-Host "Flashing probably worked but the reset did not. Unplug and replug the cable."
exit 1
