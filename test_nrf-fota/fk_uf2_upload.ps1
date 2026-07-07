# =====================================================================
# fk_uf2_upload.ps1 — NoBuild UF2 flash (bez prekladu, bez bumpu build#).
#
# 1200bps touch -> doska nabehne do Adafruit bootloadera (objaví sa UF2 USB disk
# s INFO_UF2.TXT) -> skopíruje sa .uf2 -> bootloader flashne a rebootne.
# Žiadne PlatformIO (žiadny pre/post skript), takže build# sa nehýbe.
#
# Použitie:  fk_uf2_upload.ps1 -Uf2 <cesta k .uf2> [-Port COMx]
# Port sa autodetekuje podľa VID (promicro=239A, sensecap/xiao=2886), inak -Port.
# =====================================================================
param(
    [Parameter(Mandatory = $true)][string]$Uf2,
    [string]$Port
)
$ErrorActionPreference = "Stop"

if (-not (Test-Path $Uf2)) { Write-Host "CHYBA: $Uf2 neexistuje." -ForegroundColor Red; exit 1 }

# --- port pre 1200bps touch ---
if (-not $Port) {
    $name = Split-Path $Uf2 -Leaf
    $vid = if ($name -match '^promicro') { '239A' } elseif ($name -match '^sensecap|^xiao') { '2886' } else { $null }
    $devs = pio device list --json-output | ConvertFrom-Json
    $cand = @($devs | Where-Object { $_.hwid -match 'VID:PID=' })
    $pick = if ($vid) { @($cand | Where-Object { $_.hwid -match "VID:PID=$vid" }) } else { @() }
    if ($pick.Count -ge 1) { $Port = $pick[0].port }
    elseif ($cand.Count -eq 1) { $Port = $cand[0].port }
    else {
        Write-Host "Viac/ziadny port - zadaj -Port. Dostupne:" -ForegroundColor Yellow
        $cand | ForEach-Object { Write-Host ("  {0}  {1}" -f $_.port, $_.hwid) }
        exit 1
    }
}

# UF2 disky = removable s INFO_UF2.TXT (Adafruit/MS UF2 bootloader marker)
function Get-Uf2Drives {
    Get-Volume | Where-Object { $_.DriveLetter } | ForEach-Object {
        $d = $_.DriveLetter
        if (Test-Path ("${d}:\INFO_UF2.TXT")) { "${d}:" }
    }
}
$before = @(Get-Uf2Drives)

Write-Host "1200bps touch -> bootloader ($Port)..." -ForegroundColor Cyan
try {
    $sp = New-Object System.IO.Ports.SerialPort($Port, 1200)
    $sp.Open(); Start-Sleep -Milliseconds 80; $sp.Close(); $sp.Dispose()
} catch { Write-Host "  (touch warning: $_)" -ForegroundColor DarkYellow }

# počkaj na UF2 disk (max ~12 s)
$drive = $null
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 200
    $now = @(Get-Uf2Drives)
    $new = @($now | Where-Object { $before -notcontains $_ })
    if ($new.Count -ge 1) { $drive = $new[0]; break }
    if (-not $drive -and $before.Count -eq 0 -and $now.Count -ge 1) { $drive = $now[0]; break }
}
if (-not $drive) {
    Write-Host "CHYBA: UF2 disk sa neobjavil. Skus rucny dvojklik-reset a spusti znova." -ForegroundColor Red
    exit 1
}

Write-Host ("Kopirujem {0} -> {1} ..." -f (Split-Path $Uf2 -Leaf), $drive) -ForegroundColor Cyan
Copy-Item $Uf2 "$drive\" -Force
Write-Host "OK - bootloader flashne a rebootne." -ForegroundColor Green
