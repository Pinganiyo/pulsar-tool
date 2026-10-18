#pragma once

namespace Plugin
{
    using namespace std::string_view_literals;

    static constexpr auto Name{ "PULSAR"sv };
    static constexpr auto Author{ "PULSAR-Team"sv };
    // AUTHORITATIVE runtime version: this drives both the SFSE-reported plugin
    // version AND the "PULSAR v..." line REX prints into PULSAR.log, so a
    // tester's log finally tells us which build they run. CMakeLists.txt adds
    // include/ with BEFORE, so this checked-in header shadows the version that
    // cmake/CommonLibSF.cmake generates from PROJECT_VERSION. MUST be kept in
    // sync with project(VERSION ...) in CMakeLists.txt on every release bump.
    static constexpr auto Version{
        REL::Version{ 0, 7, 3, 0 }
    };
}
