# install.ps1 — Install the ZWO CAA Rotator plugin for TheSkyX on Windows
#
# Usage (from a PowerShell prompt after 'make'):
#   .\install.ps1
#   .\install.ps1 -Uninstall
#
# hidapi runtime requirement:
#   The plugin needs hidapi.dll at runtime.  When built with MSYS2/MinGW64 the
#   DLL is statically linked (-static-libstdc++ -static-libgcc) but hidapi
#   itself is a separate DLL.  This script checks that hidapi.dll is either
#   bundled alongside the plugin or present in the system DLL search path,
#   and prints instructions if it is missing.

param([switch]$Uninstall)

$ErrorActionPreference = "Stop"

# ── Paths ─────────────────────────────────────────────────────────────────────
$TSX_HOME    = "$env:PROGRAMFILES\Software Bisque\TheSkyX Professional Edition"
$PLUGIN_DIR  = "$TSX_HOME\RotatorPlugIns"
$LIST_DIR    = "$TSX_HOME\Miscellaneous Files"
$LIB_NAME    = "libx2caarotator.dll"
$UI_FILE     = "x2caarotator.ui"
$LIST_FILE   = "rotatorlist ZWO CAA.txt"
$SCRIPT_DIR  = $PSScriptRoot

# ── Helpers ───────────────────────────────────────────────────────────────────
function Write-Warn { param([string]$msg)
    Write-Host "  WARNING: $msg" -ForegroundColor Yellow
}
function Write-Fatal { param([string]$msg)
    Write-Host "  ERROR:   $msg" -ForegroundColor Red
    exit 1
}

# ── Uninstall ─────────────────────────────────────────────────────────────────
if ($Uninstall) {
    Write-Host "Uninstalling ZWO CAA Rotator plugin..."
    Remove-Item -ErrorAction SilentlyContinue "$PLUGIN_DIR\$LIB_NAME"
    Remove-Item -ErrorAction SilentlyContinue "$PLUGIN_DIR\$UI_FILE"
    Remove-Item -ErrorAction SilentlyContinue "$LIST_DIR\$LIST_FILE"
    Write-Host "Done."
    exit 0
}

# ── Pre-flight checks ─────────────────────────────────────────────────────────
if (-not (Test-Path $PLUGIN_DIR)) {
    Write-Fatal "Plugin directory not found: $PLUGIN_DIR`nIs TheSkyX installed?"
}

if (-not (Test-Path "$LIST_DIR")) {
    Write-Fatal "Miscellaneous Files directory not found: $LIST_DIR"
}

if (-not (Test-Path "$SCRIPT_DIR\$LIB_NAME")) {
    Write-Fatal "$LIB_NAME not found — build with 'make' first."
}

# ── Runtime library check ─────────────────────────────────────────────────────
Write-Host "Checking runtime libraries..."

$libOk = $true

# Look for hidapi.dll in: script dir, system32, PATH directories.
# When built with MSYS2, the DLL is typically named hidapi.dll or libhidapi.dll.
$hidapiNames = @("hidapi.dll", "libhidapi.dll")
$hidapiFound = $false

foreach ($name in $hidapiNames) {
    # 1. Bundled next to the plugin .dll (ideal — self-contained install)
    if (Test-Path "$SCRIPT_DIR\$name") { $hidapiFound = $true; break }

    # 2. System DLL search path (PATH directories + System32)
    $resolved = (Get-Command $name -ErrorAction SilentlyContinue)
    if ($resolved) { $hidapiFound = $true; break }
}

if (-not $hidapiFound) {
    Write-Host ""
    Write-Warn "hidapi.dll not found in the install directory or system PATH."
    Write-Host ""
    Write-Host "  The plugin requires hidapi.dll to communicate with the ZWO CAA." -ForegroundColor Yellow
    Write-Host "  Choose one of:" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  Option A — Bundle hidapi.dll alongside the plugin (recommended):" -ForegroundColor Cyan
    Write-Host "    Copy hidapi.dll from your MSYS2 installation into this directory:"
    Write-Host "      C:\msys64\mingw64\bin\hidapi.dll  →  $SCRIPT_DIR\"
    Write-Host "    Then re-run this script."
    Write-Host ""
    Write-Host "  Option B — Install MSYS2 runtime and add it to PATH:" -ForegroundColor Cyan
    Write-Host "    1. Install MSYS2 from https://www.msys2.org/"
    Write-Host "    2. In MSYS2 MINGW64 shell: pacman -S mingw-w64-x86_64-hidapi"
    Write-Host "    3. Add C:\msys64\mingw64\bin to your system PATH."
    Write-Host ""
    Write-Host "  Option C — Use vcpkg:" -ForegroundColor Cyan
    Write-Host "    vcpkg install hidapi:x64-windows"
    Write-Host "    Copy the resulting hidapi.dll to $SCRIPT_DIR\"
    Write-Host ""
    $libOk = $false
}

# Also check for hidapi.dll in the script dir to offer to copy it alongside the plugin
if (-not $hidapiFound) {
    Write-Host "  *** Installation will proceed, but the plugin may fail to load" -ForegroundColor Yellow
    Write-Host "  *** in TheSkyX until hidapi.dll is in place. ***" -ForegroundColor Yellow
    Write-Host ""
}

# If hidapi.dll is in the script dir, offer to install it alongside the plugin dll
$bundledHidapi = $null
foreach ($name in $hidapiNames) {
    if (Test-Path "$SCRIPT_DIR\$name") {
        $bundledHidapi = "$SCRIPT_DIR\$name"
        break
    }
}

# ── Install ───────────────────────────────────────────────────────────────────
Write-Host "Installing plugin..."

Copy-Item "$SCRIPT_DIR\$LIB_NAME"  "$PLUGIN_DIR\$LIB_NAME"  -Force
Write-Host "  -> $PLUGIN_DIR\$LIB_NAME"

Copy-Item "$SCRIPT_DIR\$UI_FILE"   "$PLUGIN_DIR\$UI_FILE"   -Force
Write-Host "  -> $PLUGIN_DIR\$UI_FILE"

Copy-Item "$SCRIPT_DIR\$LIST_FILE" "$LIST_DIR\$LIST_FILE"   -Force
Write-Host "  -> $LIST_DIR\$LIST_FILE"

# If hidapi.dll is bundled with the release, install it next to the plugin
if ($bundledHidapi) {
    $hidapiDest = "$PLUGIN_DIR\$(Split-Path $bundledHidapi -Leaf)"
    Copy-Item $bundledHidapi $hidapiDest -Force
    Write-Host "  -> $hidapiDest"
}

Write-Host ""
Write-Host "Installation complete."
if (-not $libOk) {
    Write-Host "  Remember to place hidapi.dll before starting TheSkyX." -ForegroundColor Yellow
} else {
    Write-Host "Restart TheSkyX and select 'ZWO CAA Rotator' from the Rotator device list."
}
