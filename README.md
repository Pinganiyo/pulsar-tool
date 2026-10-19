# PULSAR Revived

**A modified fork of PULSAR, the two-player co-op mod for Starfield.**

> Based on PULSAR by ramisotti13-eng
> https://github.com/ramisotti13-eng/pulsar
> (source repository: https://github.com/ramisotti13-eng/pulsar-tool)

This is **not** the original PULSAR. It is a continuation of it, picked up after
the original author stopped development and invited anyone to take it over.
All the credit for the architecture, the reverse engineering and the existing
feature set goes to ramisotti13-eng and to everyone who tested the original
alpha. This fork's changes are listed in [Changes in this fork](#changes-in-this-fork)
and in the git history.

"PULSAR Revived" is a working name and may change. Under the original's
[additional license terms](LICENSE-ADDITIONAL) this fork must not be called
plainly "PULSAR", must keep the attribution above, and must be marked as
modified.

**This page is a living document.** The plan and the status below are updated
as work lands.

## The goal

Turn the 0.3.0 alpha into a co-op mod two people can play through Starfield
with, without constantly working around it. Reliability first, completeness
second, polish third.

Scope is deliberately narrow: **two players, Starfield 1.16.244 on Steam, with
SFSE.** More players, a dedicated server, or compatibility with every other mod
are not goals.

## Roadmap

These are milestones, not dates. Some single bugs may take longer than a whole
milestone.

| Version | Theme | What it means |
| --- | --- | --- |
| **0.3.1** | Stabilize | A clean, reproducible build. A repeatable two-player test setup. Better logs and connection errors, so failures can be diagnosed. |
| **0.4.0** | Core fixes | Fix space station / POI divergence. Restore the client's own character after every regroup and world transition. Fix ghost equipment and stuck gravity. Disconnect / reconnect recovery. |
| **0.5.0** | Gameplay | Fix the shared-vehicle crash, or disable shared riding with a clear warning. Verify combat, deaths, revives, loot, inventory, doors, containers, NPCs, quests and cell transitions. No soft-locks. |
| **0.6.0** | Completeness | Revisit equipment sync and its animation-graph crash, corpse loot sync, host-authoritative NPC firing, long sessions and progression across saves. |
| **0.7.0** | Playable | A defined, substantial campaign route completed by two players with no reproducible crash or progression-blocking bug. Remaining cosmetic issues documented. |
| **0.7.5** | Clone animations | The partner's clone plays strafe, backwards and other player movement animations instead of an NPC forward run (the original author's "anim-graph swap"). Plan: [docs/design/specs/2026-10-19-clone-animation-states.md](docs/design/specs/2026-10-19-clone-animation-states.md). |
| **1.0.0** | Stable | Reliable install/update/uninstall, connectivity, no known critical defects in the supported configuration, regression tests, a release process and a troubleshooting guide. |

## Where we are

**Code version: 0.7.0. Verified in game: nothing yet.**

Versions 0.3.1 to 0.7.0 were developed without a two-player test setup. Every
change builds, passes the automated tests (77 game-free tests plus the companion
tests, run on every push by CI) and is listed with how it was checked in
[CHANGELOG.md](CHANGELOG.md). But no roadmap goal counts as met until it has been
played: the version numbers mark the code for each milestone, not a playable 0.7.0.

**Next step: the first two-player session.** Both players install the same
0.7.0 build, then run [docs/TESTING.md](docs/TESTING.md) scenarios A, B, D and F,
in this order of importance:

1. **Connect** (A): both on 0.7.0 connect; a different version is now refused
   with a clear message in the log.
2. **Your own character after regroups** (B): items and perks stay yours after
   F8 (which now works on the client), fast travel and landing.
3. **Space** (D, E): no drifting, normal gravity after stations, same stations
   and POIs as the host. If they diverge, F8 on the client should regroup; then
   try `AutoRegroupInSpace = 1`.
4. **Reconnect** (F): drop and rejoin within 90 s; quests the host advanced
   meanwhile should catch up.
5. **Vehicle** (G): entering the host's vehicle is refused with a message
   instead of crashing; try `SharedVehicleRide = carry`.

Without a second PC: `pulsar_fakepeer.exe` plays a headless second player
against your game, and F9 spawns an AI buddy (see docs/TESTING.md).

### Settings added in this fork (PULSAR.ini)

| Setting | Default | What it does |
| --- | --- | --- |
| `AutoRegroupInSpace` | `0` | Client asks for a regroup when out of sync with the host in space for 20 s (max 3 per session). |
| `SharedVehicleRide` | `off` | `off`: entering the host's vehicle is refused (it crashes). `carry`: refused, but you ride along as a carried passenger. `enter`: old behaviour. |
| `CoopPartnerCountsAsFollower` | `1` | Your partner counts as a human follower while near (same area, within 40 m), so Extrovert / Introvert react to them. |
| `CoopTraitNpcsForPartner` | `1` | The joining player's background-trait NPCs (temple keepers for Raised Universal / Enlightened, and the others) are set up in the host's world. |
| `CloneUsePlayerStates` | `0` | Experimental (toward 0.7.5): the partner's clone uses the player movement animations (strafe, backwards) instead of the NPC ones. Test it and report. |
| `SyncCloneWeapons` | `1` | Show the partner's weapon on their clone. Set `0` if the game crashes when they draw or swap weapons. |
| `HotkeyBuddy` | `F9` | Solo testing: toggle an AI teammate. |
| `BuddyBaseFormID` | `0` | Buddy appearance: `0` = a copy of you, else a Starfield.esm NPC base ID. |

### Still open

- Host gear left as an unusable "ghost" item on the client (needs in-game work
  on the game's inventory UI cache).
- Explosion knockdowns ragdolling both players, clone flicker on cell changes,
  ship hatch animations, door sync edge cases when the client leads.
- Host-authoritative NPC firing (not built).
- Everything in "Where we are" above that has not been played yet.

Upstream's full known-issues list: [What does not work](#what-does-not-work) and
[docs/OVERVIEW.md](docs/OVERVIEW.md).

## Changes in this fork

See [CHANGELOG.md](CHANGELOG.md). Everything up to and including commit `48f58d3`
is the original PULSAR 0.3.0.

---

## Original PULSAR README

Kept as the original author wrote it. Statements in the first person are theirs.

### I am not developing this any more, and it is open for anyone to take over

I ran out of time to continue PULSAR. Rather than let it go quiet and
disappear, I have published everything.

The full source is in this repository under GPL-3.0: the SFSE plugin, the tool,
the companion daemon, the relay and the lobby backend. Nothing is held back and
there is no gatekeeping. Fork it, publish it, rename it, take it in a different
direction. You do not need to ask me.

It works, and real people have played it. It also has bugs that break co-op in
specific situations, and nobody is fixing them right now. Both of those are
true, and [docs/OVERVIEW.md](docs/OVERVIEW.md) tells you which is which before
you invest any time.

If you want to pick it up, start at [BUILD.md](BUILD.md) and
[docs/design/](docs/design/).

---

Co-op multiplayer for Starfield. Play through the game together with a friend.

### Status for players

You can still download and play it. The release on this page works.

**The official lobby and relay stay up until the end of 2026.** They are paid
for until then, so until that point everything works out of the box: host, share
a code, play. After that, assume they are gone. If you are reading this in 2027
or later, plan for it.

That deadline is survivable by design, and deliberately so. No lobby address is
compiled into the tool: it reads one from a config file or an environment
variable, and falls back to a local stack when neither is set. Hosting your own
lobby and your own relay is documented further down, and the source for both is
in this repository. Nothing about the mod depends on my server continuing to
exist.

If you are taking the project over, that end-of-2026 date is your runway for
standing up your own instance without any gap for players.

**Bug reports no longer lead to fixes from me.** Filing an issue is still worth
doing, because it leaves a record for whoever continues the project, but please
do not wait on a reply. Most known problems are already listed further down and
in [docs/OVERVIEW.md](docs/OVERVIEW.md).

### What you need

* **Starfield, the Steam version.** The mod loads through SFSE, which only supports Steam. The Game Pass and Microsoft Store version is not supported, and the mod will never run on consoles.
* **Starfield 1.16.244.** The plugin resolves engine functions by address, and those addresses are tied to the exact game version. A game update breaks the mod until someone re-resolves them.
* **SFSE (Starfield Script Extender)**: https://sfse.silverlock.org/
* Windows. A friend with the same setup.

### Install

The mod itself is one click. It is bundled inside the tool, so you never download mod files separately and you never copy anything by hand. The only thing you set up yourself is SFSE.

1. Install **SFSE**: drop its files into your Starfield folder, next to the game exe.
2. Start **PULSAR** (Pulsar.exe). If you do not have it yet, grab it from the [Releases](https://github.com/ramisotti13-eng/pulsar-tool/releases) on this page, unzip it anywhere except the Starfield folder, and run it.
3. Open the **Setup** page and click **Install mod**. That one click copies the whole mod into Starfield. Then click **Fix all** so the required game settings are set.

Close Starfield before installing or updating.

### Play together

Both players run the game through the tool, and **both have to be in game with a save loaded** (not sitting in the main menu) for the connection to work.

1. On the **Co-op** page, type your name.
2. One of you clicks **Host** and shares the code that appears. The other clicks **Join** and enters that code. Starfield launches for both of you.
3. Each of you loads a savegame. A fresh save, right after character creation, is the smoothest starting point. Use the same one when you join a lobby.
4. Once the connection is made, the host's current game is sent over to the joiner and loaded, so you both end up in the same place in the same world.

**The save sync in step 4 can take a while, so please be patient.** A Starfield save is several megabytes. It has to transfer to the joiner and then load, and during that time it can look like nothing is happening for a few seconds. That is expected. It is not frozen and it is not a crash, just let it finish.

Even though the joiner loads the host's game, your own character is restored right after, so both of you keep your own level, skills, perks and inventory. You can reset your character any time in the tool under Settings.

Tip: press **F8** in game to regroup at the host after a fast travel, or any time you drift apart.

For a private game without the lobby, use **Play directly**: the host shares an IP and port, and the other joins by IP (over the internet you also forward that port on your router).

### Good to know

A few things that look like problems but usually are not:

* **A character vanishing on a planet is normally not a disconnect.** It is a cell transition: the engine has to reload that character on the other side. Give it a few seconds, or move closer, and it comes back.
* **Lost each other or out of sync? Press F8.** That pulls the client back to the host. It is the manual regroup and it fixes most "where did they go" moments.
* **On a planet surface each of you can fast travel on your own.** The host fast traveling does not drag the client along on purpose. Use F8 afterwards to regroup at the host.
* **The other player is shown as a copy of their character.** A short flicker of that copy when changing cells is cosmetic and known. It does not mean something broke.
* **The first time someone enters a vehicle, the game shows a short tutorial popup.** That popup is local to whoever entered, so the other player will not see it. That is normal.
* **If the tool shows offline, use Reconnect** on the Co-op page. A relay can hiccup for a moment.

### Other mods

PULSAR was never tested alongside other mods. It is technically possible to run
them together, so you are welcome to try, but expect that some combinations will
not work. If something breaks, try again with only PULSAR active before you
conclude it is a PULSAR problem.

### What works

* **2 player co-op.** More is not supported: the engine does not run headless and the networking is built around a single peer. Four players might be reachable with real work, a large server is not. The reasoning is in [docs/OVERVIEW.md](docs/OVERVIEW.md).
* **Host and join by a short code**, or connect directly by IP for LAN and direct play.
* **Your character carries over.** Level, skills, perks, inventory and credits are saved and follow you between sessions and across savegames. Both of you keep your own progress.
* **Shared combat.** Enemies, damage and kills are synced, and both players get the kill XP in a shared fight.
* **Downed and revive.** When you go down you can be picked back up instead of dying.
* **The world stays in sync.** NPC positions, doors, grabbed objects, time and weather, and basic quest progress.
* **Follow into space and on the ground.** The client comes along through takeoff, landing and ground vehicle driving. Some of this is rough.
* **Proximity voice chat.** Push to talk or open mic, and the other player gets quieter with distance.
* **Fast travel and regroup.** Travel on your own, then press F8 to pull yourself to the host.
* **Encrypted connection.** All co-op traffic is encrypted peer to peer, and relays mean you usually do not need to forward a port. The lobby only does matchmaking and never sees game data.
* **Character backup.** Export and import your character to a file, and reset it from Settings whenever you want.

### What does not work

Stated plainly, because you will run into these:

* Station and point-of-interest spawns can diverge in space, leaving the client with a stale position that looks like being stuck inside the ship.
* A mid-session regroup does not always re-apply the client's own character profile.
* Host worn gear can persist as an unusable ghost item on the client.
* The client's gravity can stay disabled after a station or space transition.
* Riding a vehicle together crashes the client on entry.
* Equipment sync is implemented but disabled: applying it to the remote copy crashes its animation graph.

The full list, cosmetic issues included, is in [docs/OVERVIEW.md](docs/OVERVIEW.md).

### Host your own relay (optional, advanced)

A relay just passes traffic between two players when a direct connection is hard. The tool picks one from the public list automatically, so you do not need your own while public relays are registered. The official one is funded until the end of 2026; after that, a community relay is what keeps NAT-blocked players connecting.

You might want your own as a private relay for your group, or to serve a region that does not have one yet. The relay source is in this repository under `lobby/relay`, with a Dockerfile. The standalone repository is at https://github.com/ramisotti13-eng/pulsar-relay

A relay only forwards traffic. It never loads your save or touches your character, and because the connection is encrypted it only ever sees encrypted bytes.

### Running your own lobby

The lobby backend is in this repository too, under `lobby/server` and
`lobby/deploy`. No lobby address is compiled into the tool: it reads one from a
config file or an environment variable, and otherwise falls back to a local
stack. That is deliberate, so the project does not stop working when the
official lobby goes offline at the end of 2026. See [BUILD.md](BUILD.md).

`lobby/deploy` contains a working Caddy and Docker Compose setup, so standing up
a replacement is a deployment job rather than a development one.

### Source, license, contributing

Everything is here: the SFSE plugin (`src/`, `include/`), the tool
(`lobby/pulsar`), the companion daemon (`lobby/companion`), the relay
(`lobby/relay`) and the lobby backend (`lobby/server`).

* [docs/OVERVIEW.md](docs/OVERVIEW.md) covers the architecture and the honest state of things.
* [BUILD.md](BUILD.md) is the build guide. You must apply the patches in `patches/` after cloning, or the plugin will not work.
* [docs/design/](docs/design/) holds one design document per feature, written before it was built. They record why each thing ended up the way it did, including approaches that were rejected. That reasoning is what you cannot recover from reading the code. Note the warning there about stale line numbers.
* [docs/PULSAR_Technical.md](docs/PULSAR_Technical.md) is the technical reference for the plugin itself.

Licensed under GPL-3.0, see [LICENSE](LICENSE), with additional attribution
terms in [LICENSE-ADDITIONAL](LICENSE-ADDITIONAL): keep the credit to the
original, mark your version as modified, and do not name your fork plainly
"PULSAR". Everything else the GPL grants you is untouched, commercial use
included.

Third-party components are listed in [THIRD-PARTY.md](THIRD-PARTY.md).

### Disclaimer

Not affiliated with, endorsed by, or connected to Bethesda Softworks or ZeniMax.
Starfield is their trademark. This repository contains no Bethesda assets.

Mods carry risk. Back up your saves.

---

Built by ramisotti13-eng. Thanks to everyone who tested the alpha. I hope
someone takes it further.
