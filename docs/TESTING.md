# Two-player test checklist

Every fix in this fork is "done" only after it passes the relevant scenario
below in a real two-player session. Code that compiles is not a passed test.

## Supported test configuration

- Starfield **1.16.244, Steam**, with the matching SFSE, on **both** PCs.
- The **same `PULSAR.dll` build** on both PCs (compare file size and timestamp).
- PULSAR only, no other mods.
- A **fresh save** made right after character creation, unless a scenario says
  otherwise. Back up saves before every session.

A result from any other setup is a hint, not a pass or a fail.

## Before each session

1. Build `RelWithDebInfo` (keeps the logs, emits a PDB). See [BUILD.md](../BUILD.md).
2. Copy `PULSAR.dll` and `PULSAR.pdb` to `<Starfield>\Data\SFSE\Plugins\` on both PCs.
3. Delete or move the old `PULSAR.log` on both PCs so the logs only cover this session.
4. Note the commit hash you are testing.

## Recording a result

For each scenario write down: commit hash, host or client, the exact steps,
what happened, and attach **both** players' logs
(`Documents\My Games\Starfield\SFSE\Logs\PULSAR.log`). One scenario per log
when you can.

## Automated tests (no game needed)

`pulsar_tests.exe` checks the parts of PULSAR that run without Starfield: the
character profile codec, the encryption channel, the relay protocol, the packet
table, and a loopback session (two ENet peers on 127.0.0.1 doing the real key
exchange and sending encrypted packets). Run it after every change, before any
in-game test:

```
cmake --build build/vs2022 --config RelWithDebInfo --target pulsar_tests
build\vs2022\tests\RelWithDebInfo\pulsar_tests.exe
```

It prints one line per test and exits non-zero if anything fails. Pass part of a
test name to run only those, e.g. `pulsar_tests.exe Crypto`. New tests go in
`tests/`; code under test must not include game headers (the test build swaps
`PCH.h` for `tests/stub/PCH.h`).

## Solo buddy (one PC)

Press **F9** in game (no co-op peer connected) to toggle an AI teammate that
follows you and fights with you, driven by the game's own AI. By default it is a
copy of your character; set `BuddyBaseFormID = 0x...` in `PULSAR.ini` to use a
different Starfield.esm NPC. Use it to check things like combat, doors and
cell changes with a second character around.

It does **not** test co-op sync: the buddy is a local NPC, nothing about it goes
over the network, and it is removed as soon as a peer connects. Real co-op
scenarios below still need two PCs. In `PULSAR.log`, look for `Buddy:` lines;
`dispatched=false` means the game rejected a script call.

## Fake peer (one PC)

`pulsar_fakepeer.exe` is a headless second player. It connects to a PULSAR host
running in Starfield on the same PC, does the real key exchange and handshake,
and sends position packets at 30 Hz. The host treats it as a connected client:
it spawns the remote-player clone and moves it. Unlike the F9 buddy, this goes
through the host's networking and clone code (`NetworkHost`, `RemotePlayerManager`).
There is no second game, so nothing the client would do in its own world is
tested: no save loading, no inventory, no NPC or quest sync on a client.

Build it with the tests:

```
cmake --build build/vs2022 --config RelWithDebInfo --target pulsar_fakepeer
```

Run it:

1. Start Starfield as host: `Mode = host` in `PULSAR.ini` (the default), port
   7777 unless you changed `Port`. Load a save and stand somewhere open.
2. In a terminal:
   ```
   build\vs2022\tests\RelWithDebInfo\pulsar_fakepeer.exe --mode circle
   ```
3. The clone should appear next to you within a few seconds and walk a circle.
   Ctrl+C disconnects; the host should despawn the clone.

Modes (`--mode`):

- `stand` (default): stand at `--offset` (default `200,0,0` game units) from
  where the host was when the bot connected, turned toward the host. Walk
  around it to check the clone from all sides.
- `circle`: walk a circle of `--radius` (300) around that point, one lap per
  `--period` seconds (12). Checks walk animation and turning.
- `follow`: copy the host's own movement `--delay` ms (1000) late, shifted by
  `--offset`. Checks interpolation and how the clone keeps up.

The bot waits for the host's first position packet before it sends anything,
and copies the host's cell ID into its own packets (the host ignores client
positions from another interior cell). If the host changes cell, `stand` and
`circle` re-anchor at the host's new position. `--anchor X,Y,Z[,CELL]` uses a
fixed point instead.

What happens on the host, in order:

1. Connect, then `PKT_CRYPTO_HELLO` both ways. Logs `co-op: secure channel established`.
2. `PKT_HANDSHAKE` from the bot. The host quicksaves and sends the save
   (`PKT_SAVE_TRANSFER`, several MB). The bot checks the size, throws it away
   (or writes it with `--save-out PATH`) and replies `PKT_CLIENT_LOAD_COMPLETE`,
   which makes the host respawn the clone. `--no-handshake` skips the quicksave;
   the clone still spawns from the first position packet. The host only sends a
   save once per 90 s, so a quick reconnect does not trigger another one.
3. `PKT_POSITION` from the bot: the host spawns the clone on the first one and
   moves it with the rest.

The bot logs the first packet of each type it receives, and every 5 seconds a
line with a count per type (`sent POSITION=150 | recv POSITION=301 SAVE_TRANSFER=1 ...`).
If the host disconnects or sends `PKT_HOST_REQUEST_RECONNECT`, the bot reconnects
after 3 seconds, like the real client (`--no-reconnect` exits instead).

Options:

| Option | Default | Meaning |
| --- | --- | --- |
| `--host ADDR` | `127.0.0.1` | Host address |
| `--port N` | `7777` | Host port |
| `--secret S` | none | E2E auth secret. Only for lobby/relay sessions; plain `PULSAR.ini` hosting uses none |
| `--no-e2e` | | Plaintext, only for a host with `E2EEncryption = 0` |
| `--name S` | `FakePeer` | Name sent in the handshake |
| `--mode M` | `stand` | `stand`, `circle` or `follow` |
| `--offset X,Y,Z` | `200,0,0` | Offset from the anchor |
| `--radius R`, `--period S` | `300`, `12` | Circle size and lap time |
| `--delay MS` | `1000` | Follow lag |
| `--rate HZ` | `30` | Position packets per second |
| `--anchor X,Y,Z[,CELL]` | | Fixed anchor, CELL in hex |
| `--no-handshake` | | Skip the handshake (no quicksave on the host) |
| `--no-load-complete` | | Do not reply to the save |
| `--save-out PATH` | | Write the received save to a file |
| `--duration S` | until Ctrl+C | Quit after S seconds |
| `--no-reconnect` | | Exit when the link drops |

If the bot logs `decrypt FAILED`, its secret or `--no-e2e` does not match the
host. If it never logs `connected`, the host is not listening on that port.
Look in `PULSAR.log` on the host for `Handshake: FakePeer`, `PKT stats` and the
`RemotePlayer` spawn lines. The host takes one client, so stop the bot before a
real client joins.

`pulsar_tests` runs the bot against a stand-in host in the same process
(`FakePeer.*` tests), so the handshake is checked on every test run. Whether
the clone looks right in game has to be checked by hand.

## Scenarios

### A. Connect and join
1. Both load a save, host starts **Play directly**, client joins by IP.
2. Wait for the save transfer and load to finish.
- [ ] Client ends up next to the host.
- [ ] Client has their own items, perks and powers, not the host's.
- Logs: `RestoreClientState` once on the client; no `cache is EMPTY`.

### B. Client profile across regroups (0.4.0)
1. After A, note the client's inventory and perks.
2. Press **F8** on the client. Repeat 3 times.
3. Fast travel with the host, then F8. Land on a planet, then F8.
- [ ] After every step the client still has their own items and perks.
- [ ] Items picked up during the session are kept.
- Logs: one `RestoreClientState` per bridge; `ReapplyCachedDbProfile` on each
  mid-session bridge; no `cache is EMPTY`; no `LockCache` without a matching
  `UnlockCache`.

### C. HUD messages on restore
1. During A and B, watch the client's HUD while the restore runs.
- [ ] No flood of one "added" or "removed" message per item.
- If messages still appear, note whether they say "added" or "removed".

### D. Space: takeoff, flight, docking
1. Both board the host's ship. Host takes off, flies, and docks at a station.
2. Both walk into the station, then back to the ship and take off again.
- [ ] Client stays inside the ship during flight (no drifting).
- [ ] Client has normal gravity in the station and after leaving it.
- [ ] Client sees the same station / POIs as the host.
- Logs: `FlightSync: body cache reset for save-bridge load` on every save
  receipt.

### E. Same-system cruise (POI divergence)
1. In space, host cruises to a station or POI in the **same** system.
- [ ] Client sees the same station / POI and is not stuck inside the ship.
- If it diverges, note whether F8 fixes it.

### F. Disconnect and reconnect
1. Client quits the session, then rejoins.
2. Repeat with the host disconnecting.
- [ ] Rejoin works without restarting the game.
- [ ] Client's character is their own after rejoining.

### G. Vehicle (known crash)
Only when working on the vehicle fix: both enter the same ground vehicle.
- [ ] No crash on entry. Otherwise attach the crash log and dump.

## Regression run

Before merging anything into `main` that touches networking, save bridging,
inventory or space sync, run A, B, D and F.
