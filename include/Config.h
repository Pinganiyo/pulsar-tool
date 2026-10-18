#pragma once

#include <cstdint>
#include <istream>
#include <string>

namespace Pulsar
{
    struct Config
    {
        bool        isHost = true;
        std::string hostIP = "127.0.0.1";
        uint16_t    port = 7777;

        // Character info from Launcher (save header)
        std::string playerName;
        uint32_t    playerLevel = 0;
        std::string raceName;
        uint16_t    gender = 0;

        // Hotkey VK codes - user-rebindable via PULSAR.ini.
        // Defaults: F7 / F8 (free in vanilla Starfield).
        int         hotkeyAutoFollow      = 0x76;  // VK_F7
        int         hotkeyManualBridge    = 0x77;  // VK_F8
        // L.7 Path B Phase 1 - run BCPS round-trip test on the
        // newest save and write the recompressed result to a sidecar
        // "<savename>_pulsar_rt.sfs" for engine-load verification.
        // Default F10 (F9 = vanilla Quickload conflict).
        int         hotkeySaveRoundTrip   = 0x79;  // VK_F10
        // L.7 Path B Phase 2 - diff the two newest saves whose
        // names start with "PULSAR_" (or fall back to two newest
        // overall) and write candidate pilot-state byte ranges to
        // tools/save_diff_pilot.txt. Default F11.
        int         hotkeySaveDiff        = 0x7A;  // VK_F11

        // Player-facing manual character save/restore (were hardcoded F10/F11).
        int         hotkeySaveCharacter    = 0x79;  // VK_F10 - "save my character now"
        int         hotkeyRestoreCharacter = 0x7A;  // VK_F11 - "restore my character"

        // Solo testing aid (Debug/Buddy.h): toggles an AI teammate that follows the
        // player. Only works while no co-op peer is connected. Note F9 is also the
        // dev diagnostic dump when devHotkeys = 1.
        // ini: HotkeyBuddy = F9, BuddyBaseFormID = 0 (0 = a copy of the player,
        // otherwise a Starfield.esm NPC base FormID in hex, e.g. 0x0001234A)
        int           hotkeyBuddy     = 0x78;  // VK_F9
        std::uint32_t buddyBaseFormID = 0;

        // Developer / diagnostic hotkeys. OFF by default for release: when devHotkeys
        // is false the dev hotkeys below are NOT polled (no collisions, no confusion
        // for normal players). The Pulsar tool exposes only the player hotkeys.
        // ini: devHotkeys = 1
        bool        devHotkeys              = false;
        int         hotkeyDevFlashlightDump = 0x75; // VK_F6  - light-field dump
        int         hotkeyDevDiag           = 0x78; // VK_F9  - diagnostic dump
        int         hotkeyDevManualRevive   = 0x79; // VK_F10 - debug manual revive

        // KANDIDAT 2 (de-risk): drive tracked NPCs via IMovementDirectControl
        // instead of the data.location lerp + SetPosition snap. Opt-in on the
        // CLIENT (it runs in TickLerp). ini: npcDirectControl = 1
        bool        npcDirectControl      = false;

        // A/B diag: when npcDirectControl is on, skip IMovementDirectControl and
        // drive tracked NPCs via graph-vars + mesh-move only. ini: npcNoImc = 1
        bool        npcNoImc              = false;

        // P3: let weaponDrawn remote NPCs run their own combat AI (visible
        // aim/fire + they damage the local player) instead of puppeting them;
        // position stays loosely host-corrected. ini: npcCombatAiLive = 1
        bool        npcCombatAiLive       = false;

        // Phase C - DB-authoritative progression restore. When ON and the DB
        // profile is non-empty, the DB (items+mods+AVs+perks+skillpoints) is the
        // restore source for EVERY join regardless of which save is loaded, so
        // co-op gains survive (the user's "load any save, keep my progress"
        // model). When the DB is empty it bootstraps from the loaded save. OFF =
        // legacy save-authoritative behaviour. ini: dbAuthoritative = 0 to disable.
        // DEFAULT ON (2026-06-27): this IS the co-op character-persistence model
        // (the lobby/DB system exists for it). It was inadvertently shipping OFF
        // because the deployed ini never set the key, so the client restore fell
        // back to "live save overrides" → host AVs leaked in (broken O2 on cold
        // join, perks/powers/research/credits not restored 1:1). The dbAuth path
        // is the verified 1:1-from-DB restore; the !inventory.empty() gate in
        // ApplyProfileData keeps the empty-DB bootstrap correct.
        bool        dbAuthoritative       = true;

        // Client: ask the host for a regroup (save bridge) automatically when, in
        // space, this client's cell disagrees with the host's for 20 s (the
        // "missing station / stuck inside the ship" divergence). Max 3 per session,
        // 2 min apart (Coop/DivergenceWatch.h). OFF by default until verified in
        // game: a false trigger reloads the save. F8 on the client does the same
        // on demand. ini: AutoRegroupInSpace = 1
        bool        autoRegroupInSpace    = false;

        // Client entering the vehicle the HOST is driving (VehicleSync/RidePolicy.h):
        // 0 = off (default, entry refused with a message - formal entry crashes the
        // client), 1 = carry (refused, but walking up boards you as a carried
        // passenger; experimental), 2 = enter (old behaviour, known crash).
        // ini: SharedVehicleRide = off | carry | enter
        int         sharedVehicleRide     = 0;

        // Sync the partner's WEAPON onto their clone (armor always syncs). ON is the
        // 0.3.0 behaviour; weapon equips on the clone were once linked to a clone
        // anim-graph crash, so set 0 if the partner's clone crashes the game when
        // they draw or swap weapons. ini: SyncCloneWeapons = 0
        bool        syncCloneWeapons      = true;

        // co-op "no menu pause". The engine sets RE::Main::isGameMenuPaused
        // (Main+0x448) when a pausing menu opens; ~150 subsystem updates each zero their
        // frame delta when it is set, so ALL NPCs/timers freeze the moment one player
        // opens a menu. When this is ON (default) and a co-op session is connected, we
        // force that flag back to false every game-thread tick so the world keeps running
        // (like online games), gated to skip LoadingMenu/transitions. ini: coopNoMenuPause = 0
        bool        coopNoMenuPause       = true;

        // one-shot co-op time + weather sync. When ON (default), the client
        // requests the host's game time-of-day + current weather once on join and
        // applies it so both see the same time/weather. ini: coopTimeWeatherSync = 0
        bool        coopTimeWeatherSync   = true;

        // end-to-end encryption of all co-op traffic (ECDH P-256 + AES-256-GCM).
        // ON by default. The relay only ever sees ciphertext. BOTH players must have the
        // same setting (mismatch = no connection). ini: e2eEncryption = 0 (debug only).
        bool        e2eEncryption         = true;

        // E2E relay AUTHENTICATION (anti-MITM): mix the per-lobby secret delivered over
        // the trusted DB into the key derivation, so a malicious relay operator cannot
        // MITM the key exchange (prerequisite for untrusted public community relays).
        // DEFAULT ON (2026-06-27, verified in-game on 2 PCs: both derived the same
        // secret+ECDH key and the secure channel established). Both peers must match - a
        // peer without the feature/secret cannot connect (version-coupled, which is fine
        // for a fresh public release with no old installed base). ini: e2eRelayAuth = 0 to
        // disable (debug / escape hatch).
        bool        e2eRelayAuth          = true;

        // follow the host THROUGH a takeoff INTO space. OFF for alpha: the
        // only co-location mechanism is the save-load bridge, and loading a save
        // while the client is in space hits an engine null-lock CTD (see
        // reference_crash_22ce9c6.md); aborting that load instead just strands the
        // client in space (every later bridge aborts too). So we do NOT drag the
        // client into space on takeoff at all (it stays on the ground, load-safe);
        // landing back to the ground still follows normally. A proper non-save-load
        // space-follow is the deep work. ini: coopSpaceFollow = 0 (to disable).
        // 2026-06-24: re-enabled now that CrashGuard Guard #5 shields the in-space
        // load CTD (FUN_1422ce9b0 null-lock) and D2.3 force-proceeds again, so the
        // client can safely bridge into the host's space position on takeoff.
        bool        coopSpaceFollow       = true;

        // Auto-co-location on a PLANET fast-travel. DEFAULT OFF (2026-06-27, user
        // request): on planets each player fast-travels INDEPENDENTLY - the host
        // map-jumping on/off a planet surface (a procedural cell) must NOT drag the
        // client. The client just stays put; F8 remains the manual "regroup at host".
        // Off only gates the PROCEDURAL (planet-surface) co-location cases; space
        // takeoff/landing (coopSpaceFollow) and static city-to-city follow are
        // unaffected. ini: coopPlanetFastTravelFollow = 1 to restore the old auto-drag.
        bool        coopPlanetFastTravelFollow = false;
        // on 1.16.244 carrying a STANDING (unseated) client inside a flying
        // ship needs the explicit hknp world-commit (body data-block SetTransform),
        // not just the cache memcpy. When true, the passive client commits the host
        // ship rotation into the live physics world each tick so the body physically
        // rotates and carries the standing player. ini: coopSpaceBodyCommit = 0 (off).
        bool        coopSpaceBodyCommit   = true;
        // co-op kill-XP-sharing. When either player kills an NPC in a shared
        // area, both players receive that kill's XP. Captured by hooking the engine
        // XP-award primitive (source==7=KILL); applied to the peer with no
        // re-multiply. Dedup-guarded so both-shoot never double-counts.
        // ini: coopShareKillXp = 0 (to disable).
        bool        coopShareKillXp       = true;

        // While the co-op partner is near (same cell, <= 40 m) the game counts one
        // more human follower, so Extrovert / Introvert and anything else using
        // TRAIT_CND_HasHumanFollower see the partner (Coop/FollowerBonus.h).
        // ini: CoopPartnerCountsAsFollower = 0 (to disable).
        bool        coopPartnerCountsAsFollower = true;
        // tame combat-NPC movement (circle/dodge/avoid/strafe/flank) while
        // in a co-op session so the independently-run AIs on host + client don't
        // reposition differently → less NPC position divergence. Runtime GMST
        // zeroing, restored on disconnect. ini: coopTameCombatAI = 0 (to disable).
        bool        coopTameCombatAI      = true;

        // EventTrace - dev-only diagnostic telemetry (quest progress, scripts,
        // NPC spawns, NPC dialogue) to PULSAR.log + NAS for co-op sync design.
        // Compiled OUT of the hardened release entirely; this flag only toggles
        // it in the dev build. Default ON so a dev session always captures it.
        // ini: traceEvents = 0 (to silence).
        bool        traceEvents           = true;
    };

    // Parse a Win32 virtual-key code from a config string. Accepts:
    // "F1".."F24"           → VK_F1..VK_F24
    // "A".."Z" / "0".."9"   → ASCII upper letter / digit code
    // "Numpad0".."Numpad9"  → VK_NUMPAD0..VK_NUMPAD9
    // "0x76" / decimal int  → raw VK code
    // Returns -1 on parse failure (caller keeps default).
    int ParseVKey(const std::string& a_input);

    // Get the plugin directory (where PULSAR.dll lives)
    std::string GetPluginDir();

    // Loads config from SFSE/Plugins/PULSAR.ini
    Config LoadConfig();

    // The PULSAR.ini parser (LoadConfig = find the file + this). Unit-tested.
    Config ParseConfig(std::istream& a_in);

    // Access the global config loaded once at plugin start (defined in Main.cpp).
    // Lets non-Main modules read hotkey/dev settings without re-reading the file.
    const Config& GetConfig();
}
