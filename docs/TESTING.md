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
