# Changelog

Changes in this fork (PULSAR Revived), based on PULSAR 0.3.0 by
ramisotti13-eng. Both players must run the same version.

**How to read this:** every entry says how it was checked.
- *Tested:* covered by the automated tests (`pulsar_tests.exe`, no game needed).
- *Not tested in game:* builds and passes the automated tests, but nobody has
  played it yet. Treat these as fixes to verify, not proven fixes.

The roadmap goals in the README (for example "0.7.0: a two-player campaign
without major workarounds") are only met once verified in game. A version
number here means the code for that milestone is in, not that the goal is met.

## 0.4.0 - Core fixes

- **F8 now works for the client.** It used to do nothing on the client (the
  handler returned for anyone but the host), although the README tells the
  client to press it. It now asks the host to regroup the client. This is the
  recovery for the space station / POI divergence and "stuck inside the ship".
  *Not tested in game.*
- **Optional automatic regroup in space** (`AutoRegroupInSpace = 1` in
  PULSAR.ini, off by default). If the client is in space and its cell disagrees
  with the host's for 20 s, it asks the host to regroup. Max 3 per session, 2 min
  apart. Off by default because a false trigger reloads the save. *Trigger rules
  tested (7 tests); not tested in game.*
- **Auto-reconnect could stop for good.** If creating the network host failed
  during a reconnect, no retry was ever scheduled. It now keeps the old host and
  retries in 5 s. *Not tested in game.*
- **Character gains merge is now tested.** The rule that decides which items a
  client keeps from a session moved to its own file, unchanged, with 8 tests
  (gains, losses, host items excluded, restore leftovers cancel, failed re-adds
  kept). *Tested.*
- Profile restore after a regroup and the space-flight physics fix from 0.3.1
  are part of this milestone too. *Not tested in game.*

Still open in this milestone: host gear left as an unusable "ghost" on the
client (the remaining cause is the game's inventory UI cache and needs in-game
reverse engineering), and confirming the gravity and POI fixes in game.

## 0.3.1 - Stabilize

- **Version check on connect.** The client used to send a hard-coded 0.1.0 and
  the host never checked it, so players on different builds could connect and
  desync or crash. The client now sends its real version; the host refuses a
  different one, logs who runs what, and the refused client logs which version
  to install and stops auto-reconnecting. Peers on 0.3.0 or older are refused
  as "0.3.0 or older". *Rules and reason codes tested; not tested in game.*
- **Handshake player name is bounded.** It was logged without making sure it was
  null-terminated. *Tested.*
- **Fresh clones build.** The CommonLibSF submodule commit is now recorded;
  Visual Studio 2022 builds documented.
- **Automated tests** (`pulsar_tests.exe`): profile codec, encryption, relay
  protocol, packet table, loopback networking, fake peer, version rules and
  release bookkeeping.
- **Fixed by the tests:** names with quotes broke the saved character profile;
  a truncated profile was accepted as valid (items could be lost); any 28-byte
  packet was accepted as an authentic encrypted message. *Tested.*
- **Profile restore after a mid-session regroup** no longer skips the restore
  when the local profile file is missing. *Not tested in game.*
- **Space flight sync** drops its cached physics-body pointers on every save
  load instead of writing through freed memory (candidate for the stuck-gravity
  report). *Not tested in game.*
- **Tool:** finds the game and the mod files in more places; installs a dev
  build's `PULSAR.dll`; can install before SFSE is present.
- **One-PC testing:** F9 buddy (AI teammate) and `pulsar_fakepeer.exe` (a
  headless second player). See docs/TESTING.md. *Not tested in game.*
- **Release bookkeeping:** `tools/bump_version.ps1` sets the version in all four
  places; a test fails if they drift apart or this file has no entry.
