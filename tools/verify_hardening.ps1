<#
  verify_hardening.ps1 - the SINGLE source of truth for "is this DLL safe to ship".

  Scans a built PULSAR.dll for identifying / reverse-engineering strings and FAILS
  (non-zero exit) if any are present. Called from TWO places so the check can never be
  forgotten or skipped:
    1. CMake POST_BUILD on the Release target  -> every hardened build is auto-scanned;
       a leak aborts the build, so an un-checked DLL cannot even be produced.
    2. tools\package_release.ps1                -> belt-and-suspenders before zipping.

  Exit codes: 0 = clean, 1 = leak found, 2 = bad input. Pass -Config to no-op on the
  dev (RelWithDebInfo/Debug) builds, which intentionally keep full logs.
#>
param(
    [Parameter(Mandatory = $true)][string]$DllPath,
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"

# Only the hardened Release build is held to this standard. Dev builds keep full logs.
if ($Config -and $Config -ne "Release") {
    Write-Host "verify_hardening: skipped (config=$Config, not Release)" -ForegroundColor DarkGray
    exit 0
}

if (-not (Test-Path $DllPath)) {
    Write-Host "verify_hardening FAIL: DLL not found: $DllPath" -ForegroundColor Red
    exit 2
}

# Fast extraction: 1:1 byte->char via the Latin1 codepage (28591 works on Windows
# PowerShell 5.1, unlike the [Encoding]::Latin1 property which is .NET Core only), then
# split into printable-ASCII runs.
$bytes = [System.IO.File]::ReadAllBytes($DllPath)
$blob  = [System.Text.Encoding]::GetEncoding(28591).GetString($bytes)
$lines = [regex]::Split($blob, '[^\x20-\x7E]+')

# The ONLY [PULSAR] strings allowed to ship: the curated, sanitized PLOG_DIAG channel.
# Adding a new tester-visible diag line is a DELIBERATE act: extend this list on purpose.
$diagAllow = @(
    '[PULSAR] CrashGuard: DockShipIterationLoop absorbed',
    '[PULSAR] CrashGuard: CondEval shielded',
    '[PULSAR] CrashGuard: CondEval SEH-caught',
    '[PULSAR] CrashGuard: TtGetGround gated',
    '[PULSAR] co-op: relay-auth secret applied',
    '[PULSAR] co-op: peer connected',
    '[PULSAR] co-op: peer disconnected',
    '[PULSAR] co-op: sent time+weather snapshot',
    '[PULSAR] co-op: applied host time+weather',
    '[PULSAR] co-op: secure channel handshake sent',
    '[PULSAR] co-op: secure channel established',
    '[PULSAR] co-op: crypto self-test',
    # Connection-lifecycle diagnostics (0.2.1) - sanitized, user-level, no RVAs.
    # These ship so a tester's log shows WHERE a connection hangs (relay setup ->
    # rendezvous -> hole-punch -> direct/relay -> connected, plus the failures).
    '[PULSAR] [Relay] enabled via',
    '[PULSAR] [Relay] host registered session',
    '[PULSAR] [Relay] client: rendezvous',
    '[PULSAR] [Relay] client joined session',
    '[PULSAR] [Relay] host punched toward client',
    '[PULSAR] [Relay] punch + direct connect',
    '[PULSAR] [Relay] no peer endpoint',
    '[PULSAR] [Relay] DIRECT P2P established',
    '[PULSAR] [Relay] direct timed out',
    '[PULSAR] [Relay] host: DIRECT peer',
    '[PULSAR] [Relay] host: peer via relay',
    '[PULSAR] [Relay] bad session hex',
    '[PULSAR] [Relay] relay addr needs host:port',
    '[PULSAR] Connecting to',
    '[PULSAR] Failed to initiate connection',
    '[PULSAR] Client connected!',
    '[PULSAR] crypto init failed',
    # PULSAR Revived 0.3.1-0.5.0 - user-level co-op events a tester's log must show.
    '[PULSAR] co-op: refused ''',                       # version mismatch (host side)
    '[PULSAR] co-op: the host refused this connection', # version mismatch (client side)
    '[PULSAR] co-op: replayed ',                        # quest stages missed while disconnected
    '[PULSAR] co-op: out of sync with the host in space', # automatic regroup
    '[PULSAR] co-op: refused entering the host''s vehicle' # SharedVehicleRide
)

$fail = $false

# (a) Verbose reversing aids that must NEVER ship: engine decompiled names, RVAs /
#     addresses, dev-machine source paths (from __FILE__), dev mounts, feature phrases.
#     The mount check is deliberately generic (any UNC share, any drive-letter
#     path to a source file) rather than naming one machine: a check that hunts
#     for one specific hostname stops working the moment somebody else builds
#     this, and it leaks that hostname into every published copy of the script.
$leak = $lines | Where-Object {
    $_ -match 'FUN_14' -or $_ -match '0x14[0-9a-fA-F]{4}' -or
    $_ -match '\\\\[^\\]+\\' -or $_ -match 'PulsarMods' -or
    $_ -match 'ship destruction' -or
    $_ -match '[A-Za-z]:\\.*\.(cpp|cxx|hpp|h)\b'
}
if ($leak.Count -gt 0) {
    Write-Host ("verify_hardening FAIL: {0} verbose/leak string(s):" -f $leak.Count) -ForegroundColor Red
    $leak | Select-Object -Unique -First 12 | ForEach-Object { Write-Host "   $_" }
    $fail = $true
}

# (b) Any [PULSAR] string outside the allow-list = a raw REX::INFO / verbose log slipped
#     past the PULSAR_HARDENED gate. Catch it before it ever ships.
$rogue = $lines | Where-Object { $_ -match '\[PULSAR\]' } | Where-Object {
    $line = $_
    -not ($diagAllow | Where-Object { $line.StartsWith($_) })
}
if ($rogue.Count -gt 0) {
    Write-Host ("verify_hardening FAIL: {0} un-sanitized [PULSAR] string(s):" -f $rogue.Count) -ForegroundColor Red
    $rogue | Select-Object -Unique -First 12 | ForEach-Object { Write-Host "   $_" }
    $fail = $true
}

if ($fail) {
    Write-Host "verify_hardening: DLL is NOT safe to ship." -ForegroundColor Red
    exit 1
}

$diagCount = ($lines | Where-Object { $_ -match '\[PULSAR\]' }).Count
Write-Host ("verify_hardening OK: no leaks; {0} curated PLOG_DIAG string(s) ship." -f $diagCount) -ForegroundColor Green
exit 0
