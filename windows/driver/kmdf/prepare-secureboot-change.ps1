# prepare-secureboot-change.ps1
# One-time prep before installing the LibrePods test-signed driver:
#   1) shows BitLocker recovery protectors for C: (save the 48-digit key!)
#   2) suspends BitLocker for 2 reboots so the Secure Boot toggle does NOT
#      trigger the recovery prompt
#   3) prints the remaining manual steps
# Self-elevates via UAC. Safe to re-run.

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File',("`"{0}`"" -f $PSCommandPath)
    exit
}

Write-Host "=== BitLocker status (C:) ===" -ForegroundColor Cyan
manage-bde -status C:
Write-Host ""

$vol = Get-BitLockerVolume -MountPoint C: -ErrorAction SilentlyContinue
$rp  = $null
if ($vol) {
    $rp = $vol.KeyProtector | Where-Object { $_.KeyProtectorType -eq 'RecoveryPassword' }
}

if ($vol -and $vol.ProtectionStatus -eq 'On') {
    if ($rp) {
        Write-Host "=== RECOVERY KEY(S) - save these BEFORE continuing ===" -ForegroundColor Yellow
        $rp | ForEach-Object {
            Write-Host ("  KeyProtectorId: {0}" -f $_.KeyProtectorId) -ForegroundColor Yellow
            Write-Host ("  Password:       {0}" -f $_.RecoveryPassword) -ForegroundColor Yellow
        }
        Write-Host "  (also check https://aka.ms/myrecoverykey if this PC escrows keys to your Microsoft account)" -ForegroundColor DarkYellow
    } else {
        Write-Host "WARNING: BitLocker is ON but no RecoveryPassword protector exists." -ForegroundColor Red
        Write-Host "Do NOT proceed. Add one first:  manage-bde -protectors -add C: -rp" -ForegroundColor Red
        Read-Host "Press Enter to close"
        exit 1
    }
} else {
    Write-Host "BitLocker is not actively protecting C: (or status unknown) - see manage-bde output above." -ForegroundColor Yellow
}
Write-Host ""

$choice = Read-Host "Type YES to suspend BitLocker for 2 reboots (covers the Secure Boot change)"
if ($choice -ne 'YES') { Write-Host "Aborted - nothing changed."; Read-Host "Press Enter to close"; exit 0 }

if ($vol -and $vol.ProtectionStatus -eq 'On') {
    Suspend-BitLocker -MountPoint C: -RebootCount 2
    Write-Host "BitLocker SUSPENDED for 2 reboots." -ForegroundColor Green
} else {
    Write-Host "Nothing to suspend." -ForegroundColor Green
}
Write-Host ""
Write-Host "NEXT STEPS (manual, one-time):" -ForegroundColor Cyan
Write-Host " 1. Shut down this machine/VM."
Write-Host " 2. Disable Secure Boot:"
Write-Host "    - VMware: VM Settings -> Options -> Boot Options -> uncheck 'Enable Secure Boot'"
Write-Host "    - Physical: enter UEFI/BIOS setup -> Secure Boot -> Disabled"
Write-Host " 3. Boot Windows, then run (it will ask for UAC):"
Write-Host "      $PSScriptRoot\install-test-driver.cmd bthl2cap"
Write-Host " 4. If it reports 3010/reboot: reboot, then run the same command once more."
Write-Host " 5. Tell Devin when done - it will verify the driver bound and AACP connects."
Write-Host ""
Read-Host "Press Enter to close"
