# install.ps1 — Install the ZWO CAA Rotator plugin for TheSkyX on Windows
#
# Usage (from Developer PowerShell after 'make'):
#   .\install.ps1
#   .\install.ps1 -Uninstall

param([switch]$Uninstall)

$ErrorActionPreference = "Stop"

$TSX_HOME    = "$env:PROGRAMFILES\Software Bisque\TheSkyX Professional Edition"
$PLUGIN_DIR  = "$TSX_HOME\RotatorPlugIns"
$LIST_DIR    = "$TSX_HOME\Miscellaneous Files"
$LIB_NAME    = "libx2caarotator.dll"
$UI_FILE     = "x2caarotator.ui"
$LIST_FILE   = "rotatorlist ZWO CAA.txt"
$SCRIPT_DIR  = $PSScriptRoot

if ($Uninstall) {
    Write-Host "Uninstalling ZWO CAA Rotator plugin..."
    Remove-Item -ErrorAction SilentlyContinue "$PLUGIN_DIR\$LIB_NAME"
    Remove-Item -ErrorAction SilentlyContinue "$PLUGIN_DIR\$UI_FILE"
    Remove-Item -ErrorAction SilentlyContinue "$LIST_DIR\$LIST_FILE"
    Write-Host "Done."
    exit 0
}

if (-not (Test-Path $PLUGIN_DIR)) {
    Write-Error "Plugin directory not found: $PLUGIN_DIR`nIs TheSkyX installed?"
}

if (-not (Test-Path "$SCRIPT_DIR\$LIB_NAME")) {
    Write-Error "$LIB_NAME not found — build with 'make' first."
}

Write-Host "Installing plugin..."
Copy-Item "$SCRIPT_DIR\$LIB_NAME"  "$PLUGIN_DIR\$LIB_NAME"  -Force
Write-Host "  -> $PLUGIN_DIR\$LIB_NAME"
Copy-Item "$SCRIPT_DIR\$UI_FILE"   "$PLUGIN_DIR\$UI_FILE"   -Force
Write-Host "  -> $PLUGIN_DIR\$UI_FILE"
Copy-Item "$SCRIPT_DIR\$LIST_FILE" "$LIST_DIR\$LIST_FILE"   -Force
Write-Host "  -> $LIST_DIR\$LIST_FILE"

Write-Host ""
Write-Host "Installation complete."
Write-Host "Restart TheSkyX and select 'ZWO CAA Rotator' from the Rotator device list."
