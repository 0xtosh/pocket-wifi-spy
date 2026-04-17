param(
    [string]$Destination = "standalone-github-pocket-wifi-spy"
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$destinationRoot = Join-Path $projectRoot $Destination
$releaseDir = Join-Path $projectRoot "release"

$latestImage = Get-ChildItem -Path $releaseDir -Filter "pocket-wifi-spy-T-Dongle-S3-*-merged.bin" |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if ($null -eq $latestImage) {
    throw "No Pocket WiFi Spy merged firmware image found in $releaseDir. Run package_pocket_wifi_spy.bat first."
}

$manifestBaseName = $latestImage.BaseName -replace "-merged$", ""
$latestManifest = Join-Path $releaseDir ($manifestBaseName + ".txt")
if (-not (Test-Path $latestManifest)) {
    throw "Missing release manifest for $($latestImage.Name)"
}

$includePaths = @(
    ".gitignore",
    "LICENSE",
    "platformio_pocket_wifi_spy.ini",
    "targets.txt.example",
    "boards",
    "examples\pocket_wifi_spy",
    "build_pocket_wifi_spy.bat",
    "build_and_flash_pocket_wifi_spy.bat",
    "flash_pocket_wifi_spy.bat",
    "package_pocket_wifi_spy.bat",
    "scripts\build_pocket_wifi_spy.ps1",
    "scripts\build_and_flash_pocket_wifi_spy.ps1",
    "scripts\flash_pocket_wifi_spy.ps1",
    "scripts\package_pocket_wifi_spy.ps1",
    "scripts\export_pocket_wifi_spy_github_package.ps1"
)

if (Test-Path $destinationRoot) {
    Remove-Item -Recurse -Force $destinationRoot
}

New-Item -ItemType Directory -Force -Path $destinationRoot | Out-Null

foreach ($relativePath in $includePaths) {
    $sourcePath = Join-Path $projectRoot $relativePath
    $targetPath = Join-Path $destinationRoot $relativePath

    if (-not (Test-Path $sourcePath)) {
        throw "Missing export source path: $sourcePath"
    }

    if ((Get-Item $sourcePath) -is [System.IO.DirectoryInfo]) {
        New-Item -ItemType Directory -Force -Path $targetPath | Out-Null
        Copy-Item -Path (Join-Path $sourcePath "*") -Destination $targetPath -Recurse -Force
    } else {
        $targetDir = Split-Path -Parent $targetPath
        New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
        Copy-Item -Path $sourcePath -Destination $targetPath -Force
    }
}

Copy-Item -Path (Join-Path $projectRoot "README_pocket_wifi_spy.md") -Destination (Join-Path $destinationRoot "README.md") -Force

$destinationReleaseDir = Join-Path $destinationRoot "release"
New-Item -ItemType Directory -Force -Path $destinationReleaseDir | Out-Null

$finalImagePath = Join-Path $destinationReleaseDir "pocket-wifi-spy-T-Dongle-S3-final-merged.bin"
$finalManifestPath = Join-Path $destinationReleaseDir "pocket-wifi-spy-T-Dongle-S3-final.txt"
Copy-Item -Path $latestImage.FullName -Destination $finalImagePath -Force
Copy-Item -Path $latestManifest -Destination $finalManifestPath -Force

$publishingPath = Join-Path $destinationRoot "PUBLISHING.md"
@(
    "# Publishing Pocket WiFi Spy",
    "",
    "This folder is a standalone GitHub-ready export of Pocket WiFi Spy.",
    "",
    "Suggested steps:",
    "1. Create a new GitHub repository named pocket-wifi-spy.",
    "2. Copy the contents of this folder into that repository.",
    "3. Commit and push the files.",
    "4. Optionally create a GitHub release and attach release/pocket-wifi-spy-T-Dongle-S3-final-merged.bin.",
    "5. Point users to README.md for build and flash instructions."
) | Set-Content -Path $publishingPath

Write-Host "Pocket WiFi Spy GitHub package exported to: $destinationRoot"
Write-Host "Bundled final firmware: $finalImagePath"
