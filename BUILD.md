# Building PULSAR

PULSAR is four separate build products. You do not need all of them: the DLL
and the tool are enough to play, the backend only matters if you want to run
your own lobby.

| Product | Path | Toolchain |
| --- | --- | --- |
| `PULSAR.dll` (SFSE plugin) | `src/`, `include/` | CMake + MSVC, C++23 |
| `Pulsar.exe` (tool, GUI) | `lobby/pulsar` | .NET 8, Avalonia |
| `PulsarCompanion.exe` (daemon) | `lobby/companion` | .NET 8 |
| Lobby module (backend) | `lobby/server/spacetimedb` | .NET 8 → WASM, SpacetimeDB |
| Relay (optional, Linux) | `lobby/relay` | .NET 8, Docker |

## Prerequisites

- **Visual Studio 2026** with the C++ desktop workload (MSVC v14.4x, Windows SDK).
  Visual Studio 2022 17.14 works too, see "Visual Studio 2022" below.
- **CMake 3.30** or newer. Shipped with Visual Studio.
- **vcpkg.** Dependencies are declared in `vcpkg.json` and restored in manifest
  mode. See the note below, it is the most common thing to get wrong.
- **.NET 8 SDK** (8.0.100 or newer) for every managed part.
- **SpacetimeDB CLI**, only if you touch the lobby module.
- **Starfield 1.16.244 on Steam**, plus SFSE. See "Game version" below.

### About vcpkg and `VCPKG_ROOT`

Opening the folder in the Visual Studio IDE works without any setup: VS ships
its own vcpkg and activates manifest mode on its own when it sees `vcpkg.json`.

Building from the command line does **not** work without setup, because the
CMake presets resolve the toolchain from `$env{VCPKG_ROOT}`. If that variable is
empty, the toolchain path collapses to `/scripts/buildsystems/vcpkg.cmake`, and
configuring fails with `Could not find a package configuration file provided by
"spdlog"`. That error names spdlog but has nothing to do with spdlog.

Point the variable at your own vcpkg checkout, or reuse the one Visual Studio
already installed:

```
set VCPKG_ROOT=C:\Program Files\Microsoft Visual Studio\18\Community\VC\vcpkg
```

Adjust the edition in that path (`Community`, `Professional`, `Enterprise`).

## 1. Clone with submodules and apply the patches

```
git clone --recurse-submodules <repo-url>
cd pulsar
cd extern/CommonLibSF
git apply ../../patches/commonlibsf.patch
cd ../..
```

The submodule is pinned to CommonLibSF commit `0fdb49d` (2026-06-18), the
revision the patch was written against. Up to 0.3.0 the repository had a
`.gitmodules` entry but no recorded commit, so `--recurse-submodules` fetched
nothing. If you cloned an older revision, check out `0fdb49d` in
`extern/CommonLibSF` and run `git submodule update --init --recursive` inside it
before applying the patch. A newer CommonLibSF revision may not take the patch.

**Do not skip the patch step.** PULSAR needs two changes to CommonLibSF: two
address IDs that upstream leaves at `0` although they resolve on 1.16.244, and
an allocator swap in `BSTArray` that avoids a crash in the thread scrap heap.
Without them the plugin either fails to resolve addresses or crashes at
runtime. Details in `patches/README.md`.

You have to re-apply the patch after every submodule update.

## 2. Build the DLL

Development build, keeps all logging and emits a PDB:

```
cmake --preset vs
cmake --build build/vs --config RelWithDebInfo --target PULSAR
```

Hardened release build, used for anything you hand to another person:

```
cmake --build build/vs --config Release --target PULSAR
powershell -File tools/verify_hardening.ps1
```

The `Release` configuration defines `PULSAR_HARDENED=1`. That compiles out the
verbose log levels, emits no PDB, and strips unused code. `verify_hardening.ps1`
then greps the built binary for anything that must never ship: decompiled engine
names, raw RVAs, developer machine paths baked in through `__FILE__`, and
internal feature phrases. Treat a failure there as a release blocker.

Output lands in `build/vs/RelWithDebInfo/PULSAR.dll` or `build/vs/Release/PULSAR.dll`.

### Visual Studio 2022

The `vs` preset uses the VS 2026 generator. Visual Studio 2022 17.14 (MSVC
14.44, bundled CMake 3.31) also builds the plugin. Rather than editing
`CMakePresets.json`, add a `CMakeUserPresets.json` next to it:

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "vs2022",
      "inherits": "vs",
      "generator": "Visual Studio 17 2022",
      "binaryDir": "${sourceDir}/build/vs2022"
    }
  ]
}
```

Then point `VCPKG_ROOT` at
`C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg`, use
`cmake --preset vs2022`, and build from `build/vs2022`. The first configure
takes a while because vcpkg downloads its own CMake and builds every
dependency.

The `Release` target runs `verify_hardening.ps1` automatically as a post-build
step. A successful run reports something like `verify_hardening OK: no leaks;
30 curated PLOG_DIAG string(s) ship.`

Both configurations were built from a clean checkout of this repository, so if
yours fails, the difference is in your environment, not in the sources.

### Known quirk: new source files need two passes

The CMake globs use `CONFIGURE_DEPENDS`. When you add a new `.cpp`, the first
build re-runs the configure step but does not compile the new file, so you get
`LNK2019` unresolved symbols. Build a second time and it links. This is
expected, not a broken checkout.

## 3. Build the tool and the companion

```
dotnet build lobby/pulsar/Pulsar.csproj -c Release
dotnet build lobby/companion/Companion.csproj -c Release
dotnet test  lobby/companion.tests/Companion.Tests.csproj
```

For distributable single-file executables:

```
dotnet publish lobby/pulsar/Pulsar.csproj -c Release -r win-x64 --self-contained ^
  -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true
dotnet publish lobby/companion/Companion.csproj -c Release -r win-x64 --self-contained ^
  -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true
```

The companion must sit next to the tool: `Pulsar.exe` launches it and hides its
console window.

## 4. Optional: the lobby backend

The module is C# compiled to WASM and hosted by SpacetimeDB.

```
spacetime start                     # local instance on 127.0.0.1:3000
spacetime publish pulsar-lobby -p lobby/server/spacetimedb -y
```

Point the companion at it:

```
PULSAR_STDB_URI = http://127.0.0.1:3000
PULSAR_STDB_DB  = pulsar-lobby
```

For a public deployment put a TLS terminator in front so clients reach it over
`wss`, and never expose port 3000 directly. `lobby/DEPLOY.md` and
`lobby/deploy/` contain a working Caddy plus Docker Compose setup.

Note that `OFFICIAL_RELAY_KEYS` in `lobby/server/spacetimedb/Lib.cs` is empty in
this repository by design. It marks operator-run relays as verified. If you run
your own instance, supply the keys through an environment variable or an
owner-set table rather than committing them.

## 5. Packaging a release

```
powershell -File tools/package_release.ps1 -EspPath <path-to-PULSAR.esp> -Version 0.3.0
```

The script builds the hardened DLL, verifies the hardening, sets the master flag
on the ESP, scans for secrets, and produces the mod and tool archives.

Build the DLL first in a separate step. Running CMake from inside the packaging
script makes the configure step write to stderr, which aborts the PowerShell
script.

Code signing is optional and needs your own certificate, see `tools/sign.ps1`.
Unsigned builds trigger a SmartScreen warning on first launch.

## Game version

PULSAR resolves engine functions by address ID against **Starfield 1.16.244**.
Those IDs are version specific, so a game update breaks the plugin until the
addresses are re-resolved against the new version's address library.

Only the **Steam** build is supported, which is an SFSE limitation, not ours.

## Papyrus scripts

`scripts/Source` holds `PulsarBridge.psc` and `PulsarFurnSync.psc`. Compile them
with the Creation Kit's Papyrus compiler against Starfield's base scripts.

The source of a third script, `PulsarRemoteControl.psc`, was lost. Only the
compiled `PulsarRemoteControl.pex` shipped in the archive still exists, and it
is required for co-op because it spawns the remote player. `scripts/Source` now
also holds a reconstruction of that script, rebuilt from the `.pex` string and
debug tables, the C++ call sites and `docs/PULSAR_Technical.md`. It has never
been compiled or run. Do not compile it over the shipped `.pex` and assume
nothing changed. See `data/README.md`.
