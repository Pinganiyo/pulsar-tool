<#
  package_release.ps1 - builds the HARDENED PULSAR release and assembles the Alpha ZIP
  for the pulsar-releases GitHub upload.

  Guarantees (the "every release is secured this way" rule):
    1. Builds the Release (hardened) config: verbose logs compiled out, folded/stripped.
       A PRIVATE PDB is produced for crash symbolication of tester reports - it is
       archived to dist\symbols\ and NEVER put in the shipped ZIP.
    2. ASSERTS the hardened DLL: only the sanitized PLOG_DIAG channel survives, no
       verbose leak strings (FUN_/RVAs/[PULSAR] internals/dev paths).
    3. Bundles ONLY shippable files (mod + tool + LICENSE + README). NEVER the PDB,
       NEVER source, NEVER the dev RelWithDebInfo build.
    4. Ships the plugin as a MASTER: the dev-edited PULSAR.esp passed via -EspPath is written
       into the package as PULSAR.esp with the TES4 master flag SET (same filename, so existing
       saves see no "missing content" warning; see the staging step).

  Usage:
    .\tools\package_release.ps1 -Version 0.7.0
    (-EspPath defaults to data\PULSAR.esp, the editable dev plugin; the build master-flags
     it into the shipped PULSAR.esp. -BuildDir defaults to build\vs, else build\vs2022.)

  PULSAR Revived: packages are named PULSAR-Revived-* (LICENSE-ADDITIONAL: a modified
  version must not ship under the plain PULSAR name).
#>
param(
    [string]$EspPath = "",
    [string]$Version = "0.1.0",
    [string]$BuildDir = "",
    # cmake on PATH, else the one bundled with Visual Studio 2022.
    [string]$CMake = "",
    # Local self-host stack: bundle the SpacetimeDB CLI (BSL 1.1 - redistribution granted,
    # one local instance per user) + the lobby module source so "Database: Local" works
    # from the ZIP. If the binary is absent, the tool falls back to the Settings path.
    [string]$SpacetimePath = "$env:LOCALAPPDATA\SpacetimeDB\spacetime.exe",
    # DEFAULT OFF: the local-stack bundle ships the lobby MODULE SOURCE (lobby/server/
    # spacetimedb/Lib.cs), which contains OFFICIAL_RELAY_KEYS - a secret that must NEVER
    # reach a public package. The public Alpha distributes tool + mod as BINARIES ONLY
    # (no source) and uses the official online DB. Only enable for a private/local build,
    # and even then the secret-scan gate (step 4c) redacts + refuses real keys.
    [bool]$BundleLocalStack = $false
)

$ErrorActionPreference = "Stop"
$root  = Split-Path -Parent $PSScriptRoot          # repo root
if (-not $EspPath) { $EspPath = Join-Path $root "data\PULSAR.esp" }
if (-not $BuildDir) {
    $BuildDir = Join-Path $root "build\vs"
    if (-not (Test-Path $BuildDir)) { $BuildDir = Join-Path $root "build\vs2022" }
}
if (-not $CMake) {
    $CMake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if (-not $CMake) {
        $CMake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    }
}
$build = $BuildDir
$dll   = Join-Path $build "Release\PULSAR.dll"
$pdb   = Join-Path $build "Release\PULSAR.pdb"
$tool  = Join-Path $root "lobby\_dist\Pulsar"
$out   = Join-Path $root "dist"
$stage = Join-Path $out  "PULSAR-Revived-$Version"

function Fail($m) { Write-Host "FAIL: $m" -ForegroundColor Red; exit 1 }

# --- 1. Build the hardened DLL ---
Write-Host "[1/5] Building hardened Release DLL ..." -ForegroundColor Cyan
& $CMake --build $build --config Release --target PULSAR | Out-Host
if (-not (Test-Path $dll)) { Fail "hardened DLL not produced: $dll" }

# --- 2. Assert hardening ---
Write-Host "[2/5] Verifying hardening ..." -ForegroundColor Cyan

# The hardened Release intentionally emits a PRIVATE PDB (crash symbolication of tester
# reports). It must EXIST here, and it must be archived to dist\symbols - but it must
# NEVER reach the staged ZIP (the staging step below only copies the DLL).
if (-not (Test-Path $pdb)) {
    Fail "no PDB beside the Release DLL ($pdb) - the /DEBUG private-symbol build is required for crash symbolication"
}

# Leak scan via the SHARED verifier (same logic the build's POST_BUILD gate runs, so the
# two can never drift). It also runs at build time; this is the belt-and-suspenders pass.
$verify = Join-Path $PSScriptRoot "verify_hardening.ps1"
& powershell -NoProfile -ExecutionPolicy Bypass -File $verify -DllPath $dll -Config Release
if ($LASTEXITCODE -ne 0) { Fail "hardening verification failed (see above) - DLL is not safe to ship" }

# Archive the private PDB (versioned), kept OUT of the ZIP, for symbolicating dumps.
$symDir = Join-Path $out "symbols"
New-Item -ItemType Directory -Force -Path $symDir | Out-Null
Copy-Item $pdb (Join-Path $symDir "PULSAR-$Version.pdb") -Force
Write-Host ("   PDB archived (private): {0}" -f (Join-Path $symDir "PULSAR-$Version.pdb")) -ForegroundColor Green

# --- 3. Check required inputs ---
Write-Host "[3/5] Checking inputs ..." -ForegroundColor Cyan
if (-not (Test-Path $EspPath)) { Fail "ESP not found: $EspPath  (pass -EspPath)" }
# Compiled scripts live in data\Scripts (scripts\Source holds the .psc sources).
$pex = Get-ChildItem (Join-Path $root "data\Scripts") -Filter *.pex -ErrorAction SilentlyContinue
if (-not $pex) { Fail "no .pex scripts found in data\Scripts\" }
if (-not (Test-Path (Join-Path $tool "Pulsar.exe")))          { Fail "tool not built: $tool\Pulsar.exe  (publish lobby\pulsar first)" }
if (-not (Test-Path (Join-Path $tool "PulsarCompanion.exe"))) { Fail "daemon not built: $tool\PulsarCompanion.exe" }

# --- 4. Stage ---
Write-Host "[4/5] Staging ..." -ForegroundColor Cyan
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$modData = Join-Path $stage "mod\Data"
New-Item -ItemType Directory -Force -Path (Join-Path $modData "SFSE\Plugins") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $modData "Scripts") | Out-Null

Copy-Item $dll     (Join-Path $modData "SFSE\Plugins\PULSAR.dll")

# Ship the plugin as a MASTER, keeping the PULSAR.esp filename. A master is defined by the ESM
# bit in the TES4 header (record-header flags @0x08, bit 0x01), NOT by the .esm extension - so
# we SET the flag and KEEP the .esp name. Why keep .esp: existing saves reference PULSAR.esp, so
# renaming to .esm trips Starfield's "this save refers to missing content: PULSAR.esp" warning
# for every existing player. Keeping the name avoids that while still making PULSAR a real master
# other mods can build on. The dev keeps an unflagged .esp for the Creation Kit; the build sets
# the single flag bit at package time.
# Refs: UESP Mod File Format/TES4 (flags @0x08, 0x01 = master, regardless of extension);
# LOOT "Changing plugin types in Starfield". All PULSAR forms resolve by EditorID at runtime
# (RemotePlayerManager::ResolveGlobs), so the flag is invisible to the DLL and the Papyrus scripts.
$espBytes = [System.IO.File]::ReadAllBytes($EspPath)
if ($espBytes.Length -lt 12 -or [System.Text.Encoding]::ASCII.GetString($espBytes, 0, 4) -ne "TES4") {
    Fail "input plugin has no TES4 header (not a valid Starfield plugin): $EspPath"
}
$espBytes[0x08] = $espBytes[0x08] -bor 0x01   # set ESM master flag (bit 0x01) in TES4 header flags
[System.IO.File]::WriteAllBytes((Join-Path $modData "PULSAR.esp"), $espBytes)
Write-Host "   plugin shipped as master PULSAR.esp (ESM flag set; same filename, no save warning)" -ForegroundColor Green

# Ship the script archive too: "PULSAR - Main.ba2" is auto-loaded by the engine for PULSAR.esp
# and carries the Papyrus scripts PulsarBridge + PulsarRemoteControl. We ALSO ship all scripts
# loose (below); the archive is the robust path that loads even if loose-file invalidation is
# not set. Co-op needs PulsarRemoteControl (spawns the remote player) - shipping was missing it
# pre-0.1.2, so this is a hard requirement now.
$ba2 = Join-Path $root "data\PULSAR - Main.ba2"
if (-not (Test-Path $ba2)) { Fail "missing script archive: $ba2  (engine auto-loads it for PULSAR.esp; required)" }
Copy-Item $ba2 (Join-Path $modData "PULSAR - Main.ba2")
Write-Host "   archive shipped: PULSAR - Main.ba2 (PulsarBridge + PulsarRemoteControl)" -ForegroundColor Green

# Loose Papyrus scripts (all required for co-op): PulsarRemoteControl (spawn), PulsarBridge
# (native translate), PulsarFurnSync (vehicle/dock/transit/space helpers).
$pex | ForEach-Object { Copy-Item $_.FullName (Join-Path $modData "Scripts\$($_.Name)") }
$pexNames = ($pex | ForEach-Object { $_.Name }) -join ", "
Write-Host ("   loose scripts shipped: {0}" -f $pexNames) -ForegroundColor Green
Copy-Item (Join-Path $tool "Pulsar.exe")          $stage
Copy-Item (Join-Path $tool "PulsarCompanion.exe") $stage
Copy-Item (Join-Path $root "LICENSE")             (Join-Path $stage "LICENSE.txt")
Copy-Item (Join-Path $root "LICENSE-ADDITIONAL")  (Join-Path $stage "LICENSE-ADDITIONAL.txt")
Copy-Item (Join-Path $root "CHANGELOG.md")        (Join-Path $stage "CHANGELOG.md")

@"
PULSAR Revived $Version - co-op for Starfield (modified version)
Based on PULSAR by ramisotti13-eng - https://github.com/ramisotti13-eng/pulsar
This fork: https://github.com/Pinganiyo/pulsar-tool  (see CHANGELOG.md: most fixes are
not yet verified in game)

1. Start Pulsar.exe.
2. Open the Setup page and click "Install mod" (copies the mod into Starfield),
   then "Fix all" for the required game settings.
3. On the Co-op page: type your name, then Host a game (share the code) or Join with
   a friend's code. Starfield launches automatically.

Needs: Starfield 1.16.244 on Steam + SFSE. 2 players, both on the same PULSAR Revived
version. See LICENSE.txt and LICENSE-ADDITIONAL.txt.
Issues: https://github.com/Pinganiyo/pulsar-tool/issues
"@ | Set-Content -Encoding UTF8 (Join-Path $stage "README.txt")

# --- 4b. Bundle the local self-host stack (SpacetimeDB binary + lobby module source) ---
# SpacetimeDB is BSL 1.1: the license grants redistribution, and the Additional Use Grant
# permits up to one production instance (each user runs exactly one local instance). We
# ship its LICENSE/attribution alongside. The relay is optional - direct/LAN self-host
# connects straight to the host IP and needs no relay.
if ($BundleLocalStack) {
    if (Test-Path $SpacetimePath) {
        $stDir = Join-Path $stage "spacetime"
        New-Item -ItemType Directory -Force -Path $stDir | Out-Null
        Copy-Item $SpacetimePath (Join-Path $stDir "spacetime.exe")

        $upstreamLic = Join-Path (Split-Path -Parent $SpacetimePath) "LICENSE.txt"
        if (Test-Path $upstreamLic) { Copy-Item $upstreamLic (Join-Path $stDir "LICENSE-SpacetimeDB.txt") }
        @"
This package bundles the SpacetimeDB CLI/server (clockworklabs/SpacetimeDB),
licensed under the Business Source License 1.1 (BSL). Redistribution is permitted;
each user runs a single local instance. Source + full license:
https://github.com/clockworklabs/SpacetimeDB
"@ | Set-Content -Encoding UTF8 (Join-Path $stDir "NOTICE-SpacetimeDB.txt")

        # Module source so the bundled spacetime can publish the lobby module on first run.
        $modSrc = Join-Path $root "lobby\server\spacetimedb"
        $modDst = Join-Path $stage "module"
        if (Test-Path $modSrc) {
            New-Item -ItemType Directory -Force -Path $modDst | Out-Null
            Copy-Item (Join-Path $modSrc "*") $modDst -Recurse -Force
            Remove-Item (Join-Path $modDst "bin") -Recurse -Force -ErrorAction SilentlyContinue
            Remove-Item (Join-Path $modDst "obj") -Recurse -Force -ErrorAction SilentlyContinue
            # SECURITY: strip OFFICIAL_RELAY_KEYS from the bundled module source - the
            # official relay API key must NEVER ship. Empties the array; a local relay just
            # registers as unverified (correct for self-host). The 4c gate double-checks.
            $libDst = Join-Path $modDst "Lib.cs"
            if (Test-Path $libDst) {
                $lib = Get-Content $libDst -Raw
                $lib = [regex]::Replace($lib, '(OFFICIAL_RELAY_KEYS\s*=\s*\{)[^}]*(\})', '$1 $2')
                Set-Content -Path $libDst -Value $lib -Encoding UTF8
            }
        } else {
            Write-Host "   WARN: module source not found ($modSrc) - local module publish will be skipped." -ForegroundColor Yellow
        }

        # Optional relay (only for relay-mediated local play; direct self-host needs none).
        $relayExe = Join-Path $root "lobby\relay\bin\Release\net8.0\PulsarRelay.exe"
        if (Test-Path $relayExe) { Copy-Item $relayExe $stage }

        Write-Host "   Local self-host stack bundled (SpacetimeDB + module)." -ForegroundColor Green
    } else {
        Write-Host "   WARN: SpacetimeDB binary not found ($SpacetimePath) - local self-host NOT bundled; the tool falls back to the Settings path / guided download." -ForegroundColor Yellow
    }
}

# --- 4c. SECURITY gate: no secret may ship ---
# Belt-and-suspenders (like the DLL hardening gate): scan every staged TEXT file for a
# long hex secret (e.g. OFFICIAL_RELAY_KEYS, 48 hex) or the key array itself. A hit means
# a bundled module / future file would leak a key into a public package -> refuse to ship.
Write-Host "[4c] Secret scan ..." -ForegroundColor Cyan
$secretHits = Get-ChildItem $stage -Recurse -File -Include *.cs,*.json,*.txt,*.ini,*.env,*.config -ErrorAction SilentlyContinue |
    Select-String -Pattern '[0-9a-fA-F]{32,}' -ErrorAction SilentlyContinue |
    Where-Object { $_.Line -notmatch 'commit|sha|hash|version' }
if ($secretHits) {
    Write-Host "   secret-like string(s) in staged package:" -ForegroundColor Red
    $secretHits | Select-Object -First 6 | ForEach-Object { Write-Host ("   {0}: {1}" -f $_.Path, $_.Line.Trim()) -ForegroundColor Red }
    Fail "secret-like string found in the staged package - refusing to ship (redact or exclude the file)"
}
Write-Host "   clean (no secret-like strings in shippable text)" -ForegroundColor Green

# --- 5. Zip ---
Write-Host "[5/5] Zipping ..." -ForegroundColor Cyan
# (a) Full TOOL package -> pulsar-tool release: exes + bundled mod/Data (one-click setup) + docs.
$zip = Join-Path $out "PULSAR-Revived-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
Write-Host "DONE (tool): $zip" -ForegroundColor Green

# (b) Slim MOD-ONLY zip -> pulsar-mod release: just mod/Data (DLL+ESP master+PEX), ~1-2 MB, so the
# in-tool auto-update (ModUpdater.FindModData -> mod/Data) pulls the mod without the 160 MB tool.
$modZip = Join-Path $out "PULSAR-Revived-Mod-$Version.zip"
if (Test-Path $modZip) { Remove-Item $modZip -Force }
Compress-Archive -Path (Join-Path $stage "mod") -DestinationPath $modZip
Write-Host "DONE (mod):  $modZip" -ForegroundColor Green

Write-Host ""
Write-Host "Publish:  tool zip -> pulsar-tool release   |   mod zip -> pulsar-mod release" -ForegroundColor Cyan
Write-Host "(PDB + source + secrets never included; both repos ship binaries only.)"
