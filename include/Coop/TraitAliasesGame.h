#pragma once

// Game-thread side of Coop/TraitAliases.h (client only). Setting:
// CoopTraitNpcsForPartner (default on).
namespace Pulsar::Coop::TraitAliases {

    // Main.cpp game-thread tick: force-fill the trait NPC aliases this client's
    // own traits need, once each NPC is loaded nearby.
    void Tick(bool a_enabled);

    // A save bridge arrived: the host's world again has the aliases cleared.
    void OnSaveLoaded();
}
