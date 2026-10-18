#pragma once

// Background-trait NPCs for the JOINING player (client).
//
// TraitQuest (Starfield.esm 0x001687DB) sets up the background traits once, for
// the player who owns the save: its stage 1/99 fragment Clear()s the alias of
// every trait NPC the player does NOT need, e.g. Andy Singh (House of the
// Enlightened chest) when the player is not "Raised Enlightened". The client
// plays in the HOST's world, so an Enlightened client next to a Universal host
// finds Andy Singh's alias cleared and cannot get their chest - and the same for
// the other trait NPCs. The client's own traits are restored on join (perk
// replace), so the only thing missing is the alias.
//
// The rule below is a copy of QF_TraitQuest_001687DB's logic (decompiled from
// the shipped .pex). The game side force-fills the missing alias on the client,
// once the NPC is loaded. Game-free (tests/test_trait_aliases.cpp).

#include <array>
#include <cstdint>

namespace Pulsar::Coop::TraitAliases {

    struct Traits {
        bool extrovert   = false;   // TRAIT_Extrovert              0x00227FD7
        bool introvert   = false;   // TRAIT_Introvert              0x00227FD8
        bool taskmaster  = false;   // TRAIT_Taskmaster             0x00227FE0
        bool universal   = false;   // TRAIT_RaisedUniversal        0x00227FD1
        bool enlightened = false;   // TRAIT_RaisedEnlightened      0x00227FD2
        bool serpent     = false;   // TRAIT_SerpentsEmbrace        0x00227FD0
        bool neon        = false;   // TRAIT_NeonStreetRat          0x00227FD3
        bool fc          = false;   // TRAIT_FreestarCollectiveSettler 0x00227FD5
        bool uc          = false;   // TRAIT_UnitedColoniesNative   0x00227FD4
    };

    struct AliasInfo {
        const char*   name;
        std::uint32_t varIndex;   // index of ::Alias_<name>_var in the fragment script
        std::uint32_t npcBase;    // the alias's unique actor (ALUA)
    };

    inline constexpr std::array<AliasInfo, 6> kAliases{ {
        { "KeeperAquilus",          7, 0x0001E5D6 },
        { "AndySingh",              5, 0x00012921 },
        { "Mirza",                  8, 0x002582B4 },
        { "MadameSauvage",          9, 0x0026FDDA },
        { "MayorCartwright_FC",    11, 0x00224859 },
        { "AdministratorRookes_UC",10, 0x000124B6 },
    } };

    inline constexpr std::uint32_t kTraitQuest = 0x001687DB;
    inline constexpr const char*   kTraitQuestScript = "Fragments:Quests:QF_TraitQuest_001687DB";

    // Does a player with these traits keep alias a_index (into kAliases)?
    // Mirrors the fragment: the two keepers also serve Extrovert, Introvert and
    // Taskmaster dialogue, so those traits keep both.
    inline bool Needs(std::size_t a_index, const Traits& t)
    {
        const bool keeperShared = t.extrovert || t.introvert || t.taskmaster;
        switch (a_index) {
            case 0: return keeperShared || t.universal;    // KeeperAquilus
            case 1: return keeperShared || t.enlightened;  // AndySingh
            case 2: return t.serpent;                      // Mirza
            case 3: return t.neon;                         // MadameSauvage
            case 4: return t.fc;                           // MayorCartwright_FC
            case 5: return t.uc;                           // AdministratorRookes_UC
            default: return false;
        }
    }
}
