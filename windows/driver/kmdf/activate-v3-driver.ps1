$ErrorActionPreference = 'Continue'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$log = Join-Path $dir 'activate-v3.log'
Start-Transcript -Path $log -Force | Out-Null

Write-Host "=== Activate v3 driver $(Get-Date) ==="

# 1. Delete every existing librepods-kmdf package so ours is the only choice.
$enum = pnputil /enum-drivers
$published = $null
foreach ($line in $enum) {
    if ($line -match 'Published Name:\s*(\S+)') { $published = $Matches[1] }
    if ($line -match 'Original Name:\s*librepods-kmdf\.inf' -and $published) {
        Write-Host "deleting stale package $published"
        pnputil /delete-driver $published /uninstall /force
        $published = $null
    }
}

# 2. Stage the current build (DriverVer newest by construction).
$inf = Join-Path $dir 'bin\Release\x64\LibrePodsKmdf\librepods-kmdf.inf'
Write-Host "installing $inf"
Select-String -Path $inf -Pattern 'DriverVer' | ForEach-Object { Write-Host $_.Line.Trim() }
pnputil /add-driver $inf /install

# 3. Restart the Bluetooth enumerator subtree so every profile PDO rebinds to the new package.
Write-Host "restarting Bluetooth enumerator"
pnputil /restart-device "BTH\MS_BTHBRB\6&10fb0ba2&0&1"

Write-Host "=== resulting bindings ==="
pnputil /enum-devices /connected /class Bluetooth

Stop-Transcript | Out-Null
