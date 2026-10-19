# Partner clone animations: player states instead of NPC states (0.7.5)

**Status:** step 1 in 0.7.4 (experiment behind a setting, off by default). Needs
in-game testing before it becomes the default.

## Problem

The partner's clone plays a forward run when the partner strafes or backs up.
The original author's note (RemotePlayerManager.cpp, T126): writing
`Direction = 0.5` to the clone "still plays forward", "fluid backward requires
full NPC anim-graph swap (Beta scope)".

## What the game data says

Read from `Starfield - Animations.ba2` (`meshes/animtextdata/tables/graphs/*.agx`,
plain XML) and `Starfield.esm`:

1. **One graph for every human.** The Human race (RACE `0x0000347D`) points both
   sexes at `AnimTextData\Tables\Graphs\Human_Root.agx`. There is no separate
   "player graph" file to swap to.
2. **`Direction` is a fraction of a turn, for everyone.** `Biped_ReadyLocomotion.agx`
   tests `Direction >= 0.9 | Direction <= 0.1` (forward), `0.4 <= Direction <= 0.6`
   (backwards), and its blend nodes set `Direction Is Radians: False`.
   - The clone receives the host player's value unchanged - already correct.
   - **Bug found:** NPCSync wrote radians (`atan2(velocity) - yaw`) into it, so a
     sideways NPC sent ~1.57, outside the graph's range. Fixed in 0.7.4
     (`Animation/DirectionMap.h`, `RadiansToDirection`).
3. **Player and NPC locomotion are different STATES of the same graph.**
   `Human_Root.agx` assigns states by the `iState` variable:

   | iState | State | iState | State |
   | --- | --- | --- | --- |
   | 0 | HumanDefault | 10 | NPCDefault |
   | 1 | HumanSneak | 11 | NPCSneak |
   | 2 | HumanSighted | 12 | NPCSighted |
   | 3 | HumanSneakSighted | 13 | NPCSneakSighted |
   | 4 | HumanBlocking | 14 | NPCBlocking |
   | 5 | HumanInjured | 15 | NPCInjured |
   | 6 | HumanBleedOut | 16 | NPCBleedOut |
   | 7 | HumanZeroG | 17 | NPCReady |
   | 8 | HumanSwimming | 18 | NPCZeroG |
   |   |   | 19 | NPCSwimming |

   A "Swap Graph" node also switches sub-graphs (`Player_Gun.agx` /
   `Player_Melee.agx` vs `NPC_Gun.agx` / `NPC_Melee.agx`, `Furniture.agx`,
   `Inventory.agx`).

   RemotePlayerManager deliberately maps the host's state to NPC states
   (10 normal, 11 sneak, 17 weapon ready - the "Sarah pattern"). So the clone
   always runs NPC locomotion. **The "graph swap" is most likely "run the clone
   in the player states".**

## Plan

1. **0.7.4 (done, untested):** `CloneUsePlayerStates = 1` gives the clone the
   host's own `iState` (0-8) and relax state instead of the NPC mapping. Test
   with the fake peer in follow mode (it replays the host's animation state):
   strafe, back up, sneak, aim. Watch for: correct strafe/back animations, a
   T-pose or frozen pose, weapon pose, crashes.
2. **If the clone stays in NPC states anyway:** the engine may reassign `iState`
   from the actor's type every frame ("Assign iStates" in Human_Root). Then find
   where the engine writes it (reverse-engineer the assign step) and override it
   for the clone only - same approach the existing graph-variable forcing uses.
3. **If player states play but weapons look wrong:** also drive the Swap Graph
   to `Player_Gun.agx` / `Player_Melee.agx` for the clone (find the event or
   variable that selects the sub-graph).
4. **0.7.5:** make the working combination the default, keep the setting as an
   off-switch, document remaining limits.

## Risks

- Player states may read variables only the player graph registers (the
  author found `IsPlayer`, `IsFirstPerson`, ... are not registered on NPC
  graphs; reading an unregistered variable returns false/0). Possible symptom:
  wrong but harmless pose.
- Crashes are not expected from writing an int variable, but the clone's graph
  has crashed before when mutated at the wrong time (weapon equip). The write
  happens in the same place and at the same time as today's `iState` write.
