# Set the PULSAR version everywhere it is declared:
#   include/Plugin.h, CMakeLists.txt, lobby/pulsar/Pulsar.csproj,
#   lobby/companion/Companion.csproj
# tests/test_version_compat.cpp (Release.VersionIsTheSameEverywhere) fails if
# they drift apart. Add the CHANGELOG.md entry yourself.
#
#   powershell -File tools/bump_version.ps1 -Version 0.4.0

param([Parameter(Mandatory = $true)][string]$Version)

$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^(\d+)\.(\d+)\.(\d+)$') { throw "Version must be MAJOR.MINOR.PATCH, got '$Version'" }
$maj, $min, $pat = $Matches[1], $Matches[2], $Matches[3]
$root = Split-Path -Parent $PSScriptRoot

function Update([string]$rel, [string]$pattern, [string]$replacement) {
    $path = Join-Path $root $rel
    $text = [IO.File]::ReadAllText($path)
    $new = [regex]::Replace($text, $pattern, $replacement)
    if ($new -eq $text -and -not ($text -match [regex]::Escape($replacement))) {
        throw "No version found to update in $rel"
    }
    [IO.File]::WriteAllText($path, $new)
    Write-Host "updated $rel"
}

Update 'include/Plugin.h' 'REL::Version\{\s*\d+,\s*\d+,\s*\d+,\s*0\s*\}' "REL::Version{ $maj, $min, $pat, 0 }"
Update 'CMakeLists.txt' '(project\(\s*PULSAR\s*VERSION\s+)\d+\.\d+\.\d+' "`${1}$Version"
foreach ($proj in 'lobby/pulsar/Pulsar.csproj', 'lobby/companion/Companion.csproj') {
    Update $proj '<Version>[^<]+</Version>' "<Version>$Version</Version>"
    Update $proj '<AssemblyVersion>[^<]+</AssemblyVersion>' "<AssemblyVersion>$Version.0</AssemblyVersion>"
    Update $proj '<FileVersion>[^<]+</FileVersion>' "<FileVersion>$Version.0</FileVersion>"
}
Write-Host "PULSAR version is now $Version. Add a '## $Version' entry to CHANGELOG.md."
