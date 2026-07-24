param(
    [string]$Port = "COM5",
    [ValidateSet("radar", "printsphere", "full")]
    [string]$Mode = "radar"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$printSphereRoot = Join-Path (Split-Path -Parent $repoRoot) "PrintSphere"

$esptool = "C:\Users\chris\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
$esptoolArgs = @(
    "-m", "esptool",
    "--chip", "esp32s3",
    "-p", $Port,
    "-b", "460800",
    "--before", "default_reset",
    "--after", "hard_reset",
    "write_flash",
    "--flash_mode", "dio",
    "--flash_size", "16MB",
    "--flash_freq", "80m"
)

$radarBin = Join-Path $repoRoot ".pio\build\esp32-s3-amoled-175\firmware.bin"
$bootloaderBin = Join-Path $printSphereRoot "build\bootloader\bootloader.bin"
$partitionBin = Join-Path $printSphereRoot "build\partition_table\partition-table.bin"
$otaDataBin = Join-Path $printSphereRoot "build\ota_data_initial.bin"
$printSphereBin = Join-Path $printSphereRoot "build\printsphere_idf.bin"

$requiredFiles = switch ($Mode) {
    "radar" { @($radarBin) }
    "printsphere" { @($printSphereBin) }
    "full" { @($bootloaderBin, $partitionBin, $otaDataBin, $radarBin, $printSphereBin) }
}

foreach ($file in $requiredFiles) {
    if (-not (Test-Path -LiteralPath $file)) {
        throw "Required file not found: $file"
    }
}

$flashParts = switch ($Mode) {
    "radar" {
        @("0x110000", $radarBin)
    }
    "printsphere" {
        @("0x610000", $printSphereBin)
    }
    "full" {
        @(
            "0x0", $bootloaderBin,
            "0x8000", $partitionBin,
            "0x109000", $otaDataBin,
            "0x110000", $radarBin,
            "0x610000", $printSphereBin
        )
    }
}

Write-Host "[flash] mode=$Mode port=$Port"
if ($Mode -eq "full") {
    Write-Host "[flash] writing bootloader, partition table, OTA data, Capsule Radar, and PrintSphere"
} elseif ($Mode -eq "radar") {
    Write-Host "[flash] writing Capsule Radar OTA slot only at 0x110000"
} else {
    Write-Host "[flash] writing PrintSphere OTA slot only at 0x610000"
}

& $esptool @esptoolArgs @flashParts
if ($LASTEXITCODE -ne 0) {
    throw "esptool failed with exit code $LASTEXITCODE"
}
