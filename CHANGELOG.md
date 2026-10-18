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

## 0.7.2 - Your partner counts as a human follower

- **Extrovert / Introvert now notice your co-op partner.** Read from
  Starfield.esm: both traits switch on `TRAIT_CND_HasHumanFollower`, which is
  "PlayerFollowers_CountFollowing >= 1, and >= 2 if Vasco is active". The
  partner's clone is not a recruited follower, so before this Extrovert never
  counted your friend and Introvert treated you as alone next to them. While
  the partner is in the same area and within 40 m, PULSAR now raises that count
  (`CoopPartnerCountsAsFollower`, on by default).
  - The follower script (SQ_FollowersScript) owns the count and rewrites it with
    whole numbers at any time, so PULSAR adds 1.5 instead of 1: a value ending
    in .5 is always PULSAR's, so it never removes a real follower from the count,
    and a leftover caught in a save is removed later. No other condition or
    script in Starfield.esm reads this global.
  - *Logic tested (7 tests). In game: not tested yet - see docs/TESTING.md.*

## 0.7.1 - Distances are metres

First in-game check (one PC, with the fake peer): the version check, encrypted
connection, the 90 s reconnect rule and the clone spawn all worked. The clone was
placed ~350 m away, which showed that **Starfield positions are in metres**, not
the ~1.4 cm "game units" some code comments assume.

- **Correction to 0.5.0:** the "passenger carry could never board" units fix was
  wrong. The original 4 / 1 / 0.6 m values were right and are restored. Same for
  the F9 buddy's follow distances (6 m / 40 m), which I had scaled up ~70x.
- **Vehicle guard:** "the host's vehicle" is now within 20 m (was 1500).
- **Fake peer:** stands 2 m from the host and circles at 3 m by default (was 200
  and 300, i.e. hundreds of metres away); walk speed thresholds are in m/s.
- **Revive range (original 0.3.0 code):** the revive key reached a downed partner
  up to 250 m away (`kReviveRadius = 250`, commented "~3 m"). Now 3 m.
  *Not tested in game.*
- Other original constants with the same "game units" comment, to check in game:
  cell-content probe match radius 500 ("~7 m"), dialogue "same scene" distance
  6000 ("~60 m"), NPC door radius 3000 ("~30 m"). Left unchanged: they are
  probably far too large, but changing them without play could break matching.

## 0.7.0 - Release preparation

This is the end of the unattended 0.3.1 -> 0.7.0 run. **None of 0.3.1-0.7.0 has
been played yet.** The roadmap goal for 0.7.0 ("a two-player playthrough without
major workarounds") can only be met by playing; see docs/TESTING.md for the
checklist and README "Where we are" for what to verify first.

- **The shipped DLL passes the leak check again.** The hardened Release build
  refused the new tester-visible log lines (version refused, quest stages
  replayed, automatic regroup, vehicle refused). They were added to the
  allow-list on purpose; the build now reports 35 curated lines and no leaks.
- **Packaging works in this repository.** `tools/package_release.ps1` looked for
  the compiled scripts and the .ba2 archive in `scripts\` (they are in `data\`),
  required the VS 2026 build folder and `cmake` on PATH, and needed `-EspPath`.
  It now defaults to `data\PULSAR.esp`, finds `build\vs` or `build\vs2022` and
  the VS-bundled CMake, and ships LICENSE-ADDITIONAL and this changelog.
- **Named and credited as a modified version**, as LICENSE-ADDITIONAL requires:
  packages are `PULSAR-Revived-*.zip`, the tool's window and About page say
  "PULSAR Revived", "Modified version. Based on PULSAR by ramisotti13-eng", with
  links to the original and to this fork.
- **Tool update checks point at this fork** (`Pinganiyo/pulsar-tool`) instead of
  the original, unmaintained repositories.
- **Continuous integration:** `.github/workflows/build.yml` builds the plugin,
  the tests and the tool on every push and pull request and runs
  `pulsar_tests.exe` and the companion tests. *Not yet run: it starts on the
  next push to GitHub.*

## 0.6.0 - Completeness

- **Equipment sync is not "disabled", and weapons are now a setting.** The 0.3.0
  notes say equipment sync is off; in the code, armor has synced onto the
  partner's clone since June 2026, and weapons were switched back on in a later
  experiment although weapon equips had been linked to a clone animation crash.
  New `SyncCloneWeapons` setting (default on, same as before): set it to 0 if
  the game crashes when your partner draws or swaps weapons. *Not tested in game.*
- **PULSAR.ini parsing is tested.** Every setting testers are told to change goes
  through it; 5 tests cover defaults, the new settings, key spellings, comments
  and bad values. *Tested.*
- Reviewed for long sessions: corpse-loot packets (sender caps at 24 items,
  receiver checks every length) and growing per-NPC caches (bounded in practice,
  a few MB at most). No change needed.

Still open in this milestone, needing play sessions: whether weapon sync is
stable, corpse loot sync in play, host-authoritative NPC firing (not built),
and long sessions with progression across saves.

## 0.5.0 - Gameplay

- **Riding the host's vehicle no longer crashes the client - it is refused.**
  Formally entering the vehicle the host is driving crashes the client. New
  `SharedVehicleRide` setting in PULSAR.ini: `off` (default) refuses that entry
  with an on-screen message; `carry` also refuses it but carries you as a
  passenger when you walk up to the vehicle (experimental); `enter` keeps the
  old behaviour. *Rules tested (6 tests); not tested in game.*
- **Passenger carry could never board.** Its distances were written as metres
  but compared with positions in game units (~1.4 cm), so you had to stand
  within ~6 cm of the vehicle. Fixed. *Not tested in game.*
- **Quest stages missed during a short disconnect are replayed.** A client that
  reconnects within 90 s does not get a fresh save, and quest stages the host
  set meanwhile were never sent, so the client's quests fell behind. The host
  now replays them; the client skips stages it reached itself, so stage scripts
  do not run twice. *Journal tested (4 tests); not tested in game.*

Still open in this milestone, needing play sessions: verifying combat, deaths,
revives, loot, inventory persistence, doors, containers, NPCs and cell
transitions with two players.

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
