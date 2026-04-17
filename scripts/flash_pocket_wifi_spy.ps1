param(
    [string]$Port = "COM16",
    [string]$Environment = "T-Dongle-S3",
    [string]$ImagePath
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$configPath = Join-Path $projectRoot "platformio_pocket_wifi_spy.ini"
$pioPath = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
$pythonPath = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\python.exe"
$esptoolPath = Join-Path $env:USERPROFILE ".platformio\packages\tool-esptoolpy\esptool.py"
$releaseDir = Join-Path $projectRoot "release"

function Get-EsptoolInvocation {
    if ((Test-Path $pythonPath) -and (Test-Path $esptoolPath)) {
        return @{
            Command = $pythonPath
            Arguments = @($esptoolPath)
        }
    }

    $pythonCommand = Get-Command python -ErrorAction SilentlyContinue
    if ($null -ne $pythonCommand) {
        return @{
            Command = $pythonCommand.Source
            Arguments = @("-m", "esptool")
        }
    }

    $pyCommand = Get-Command py -ErrorAction SilentlyContinue
    if ($null -ne $pyCommand) {
        return @{
            Command = $pyCommand.Source
            Arguments = @("-3", "-m", "esptool")
        }
    }

    throw "No esptool invocation found. Install PlatformIO or Python 3 with 'pip install esptool'."
}

if ([string]::IsNullOrWhiteSpace($ImagePath)) {
    if (Test-Path $releaseDir) {
        $latestImage = Get-ChildItem -Path $releaseDir -Filter "pocket-wifi-spy-$Environment-*-merged.bin" |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        if ($null -ne $latestImage) {
            $ImagePath = $latestImage.FullName
        }
    }
}

if (-not [string]::IsNullOrWhiteSpace($ImagePath)) {
    if (-not (Test-Path $ImagePath)) {
        throw "Merged image not found: $ImagePath"
    }

    $esptoolInvocation = Get-EsptoolInvocation
    $esptoolArgs = @()
    $esptoolArgs += $esptoolInvocation.Arguments
    $esptoolArgs += @("--chip", "esp32s3", "--port", $Port, "--baud", "921600", "--before", "default_reset", "--after", "hard_reset", "write_flash", "-z", "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "16MB", "0x0000", $ImagePath)

    & $esptoolInvocation.Command @esptoolArgs
    if ($LASTEXITCODE -ne 0) {
        throw "esptool flash failed with exit code $LASTEXITCODE"
    }
    exit 0
}

if (-not (Test-Path $pioPath)) {
    $pioCommand = Get-Command pio -ErrorAction SilentlyContinue
    if ($null -eq $pioCommand) {
        throw "PlatformIO CLI not found. Install PlatformIO or ensure $pioPath exists."
    }
    $pioPath = $pioCommand.Source
}

Push-Location $projectRoot
try {
    & $pioPath run -c $configPath -e $Environment -t upload --upload-port $Port
    if ($LASTEXITCODE -ne 0) {
        throw "PlatformIO flash failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}