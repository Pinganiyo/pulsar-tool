#include "PCH.h"
#include "Config.h"

#include <fstream>
#include <algorithm>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

using namespace std::string_view_literals;

namespace Pulsar
{
    static std::string Trim(const std::string& s)
    {
        auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    int ParseVKey(const std::string& a_input)
    {
        std::string in = Trim(a_input);
        if (in.empty()) return -1;

        // Numeric forms: "0x77" / "119"
        if (in.size() > 2 && (in[0] == '0') && (in[1] == 'x' || in[1] == 'X')) {
            try { return std::stoi(in.substr(2), nullptr, 16); }
            catch (...) { return -1; }
        }
        if (std::all_of(in.begin(), in.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
            try { return std::stoi(in, nullptr, 10); }
            catch (...) { return -1; }
        }

        // Symbolic forms - case-insensitive
        std::string up = in;
        std::transform(up.begin(), up.end(), up.begin(), ::toupper);

        // F1..F24
        if (up.size() >= 2 && up[0] == 'F') {
            try {
                int n = std::stoi(up.substr(1));
                if (n >= 1 && n <= 24) return 0x70 + (n - 1);  // VK_F1=0x70
            } catch (...) {}
        }
        // Numpad0..Numpad9
        if (up.rfind("NUMPAD", 0) == 0 && up.size() == 7 && std::isdigit(static_cast<unsigned char>(up[6]))) {
            return 0x60 + (up[6] - '0');  // VK_NUMPAD0=0x60
        }
        // Single-char A-Z / 0-9
        if (up.size() == 1) {
            char c = up[0];
            if (c >= 'A' && c <= 'Z') return c;          // VK_A=0x41 = 'A'
            if (c >= '0' && c <= '9') return c;          // VK_0=0x30 = '0'
        }
        return -1;
    }

    Config LoadConfig()
    {
        Config cfg;

        // Find INI next to the DLL
        char modulePath[MAX_PATH]{};
        HMODULE hModule = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&LoadConfig), &hModule);
        GetModuleFileNameA(hModule, modulePath, MAX_PATH);

        std::string iniPath(modulePath);
        auto pos = iniPath.rfind('\\');
        if (pos != std::string::npos) {
            iniPath = iniPath.substr(0, pos + 1);
        }
        iniPath += "PULSAR.ini";

        std::ifstream file(iniPath);
        if (!file.is_open()) {
            PLOG_INFO("[PULSAR] No config file at {}, using defaults (Host mode)"sv, iniPath);
            return cfg;
        }

        PLOG_INFO("[PULSAR] Loading config from {}"sv, iniPath);

        std::string line;
        while (std::getline(file, line)) {
            line = Trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') {
                continue;
            }

            auto eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key = Trim(line.substr(0, eq));
            std::string value = Trim(line.substr(eq + 1));

            // Strip inline comments
            auto comment = value.find(';');
            if (comment != std::string::npos) {
                value = Trim(value.substr(0, comment));
            }

            std::string keyLower = key;
            std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(), ::tolower);

            if (keyLower == "mode") {
                std::string valLower = value;
                std::transform(valLower.begin(), valLower.end(), valLower.begin(), ::tolower);
                cfg.isHost = (valLower != "client");
                PLOG_INFO("[PULSAR] Config: Mode = {}"sv, cfg.isHost ? "host" : "client");
            }
            else if (keyLower == "hostip" || keyLower == "host") {
                cfg.hostIP = value;
                PLOG_INFO("[PULSAR] Config: HostIP = {}"sv, cfg.hostIP);
            }
            else if (keyLower == "port") {
                cfg.port = static_cast<uint16_t>(std::stoi(value));
                PLOG_INFO("[PULSAR] Config: Port = {}"sv, cfg.port);
            }
            else if (keyLower == "playername") {
                cfg.playerName = value;
                PLOG_INFO("[PULSAR] Config: PlayerName = {}"sv, cfg.playerName);
            }
            else if (keyLower == "playerlevel") {
                cfg.playerLevel = static_cast<uint32_t>(std::stoi(value));
                PLOG_INFO("[PULSAR] Config: PlayerLevel = {}"sv, cfg.playerLevel);
            }
            else if (keyLower == "racename") {
                cfg.raceName = value;
                PLOG_INFO("[PULSAR] Config: RaceName = {}"sv, cfg.raceName);
            }
            else if (keyLower == "gender") {
                cfg.gender = static_cast<uint16_t>(std::stoi(value));
                PLOG_INFO("[PULSAR] Config: Gender = {}"sv, cfg.gender);
            }
            else if (keyLower == "hotkeyautofollow" || keyLower == "hotkey_autofollow") {
                int vk = ParseVKey(value);
                if (vk > 0) {
                    cfg.hotkeyAutoFollow = vk;
                    PLOG_INFO("[PULSAR] Config: HotkeyAutoFollow = {} (VK 0x{:02X})"sv, value, vk);
                } else {
                    PLOG_WARN("[PULSAR] Config: HotkeyAutoFollow value '{}' unparseable - keeping default 0x{:02X}"sv,
                              value, cfg.hotkeyAutoFollow);
                }
            }
            else if (keyLower == "hotkeymanualbridge" || keyLower == "hotkey_manualbridge") {
                int vk = ParseVKey(value);
                if (vk > 0) {
                    cfg.hotkeyManualBridge = vk;
                    PLOG_INFO("[PULSAR] Config: HotkeyManualBridge = {} (VK 0x{:02X})"sv, value, vk);
                } else {
                    PLOG_WARN("[PULSAR] Config: HotkeyManualBridge value '{}' unparseable - keeping default 0x{:02X}"sv,
                              value, cfg.hotkeyManualBridge);
                }
            }
            else if (keyLower == "hotkeysaveroundtrip" || keyLower == "hotkey_saveroundtrip") {
                int vk = ParseVKey(value);
                if (vk > 0) {
                    cfg.hotkeySaveRoundTrip = vk;
                    PLOG_INFO("[PULSAR] Config: HotkeySaveRoundTrip = {} (VK 0x{:02X})"sv, value, vk);
                } else {
                    PLOG_WARN("[PULSAR] Config: HotkeySaveRoundTrip value '{}' unparseable - keeping default 0x{:02X}"sv,
                              value, cfg.hotkeySaveRoundTrip);
                }
            }
            else if (keyLower == "hotkeysavediff" || keyLower == "hotkey_savediff") {
                int vk = ParseVKey(value);
                if (vk > 0) {
                    cfg.hotkeySaveDiff = vk;
                    PLOG_INFO("[PULSAR] Config: HotkeySaveDiff = {} (VK 0x{:02X})"sv, value, vk);
                } else {
                    PLOG_WARN("[PULSAR] Config: HotkeySaveDiff value '{}' unparseable - keeping default 0x{:02X}"sv,
                              value, cfg.hotkeySaveDiff);
                }
            }
            else if (keyLower == "hotkeysavecharacter" || keyLower == "hotkey_savecharacter") {
                int vk = ParseVKey(value);
                if (vk > 0) {
                    cfg.hotkeySaveCharacter = vk;
                    PLOG_INFO("[PULSAR] Config: HotkeySaveCharacter = {} (VK 0x{:02X})"sv, value, vk);
                } else {
                    PLOG_WARN("[PULSAR] Config: HotkeySaveCharacter '{}' unparseable - keeping 0x{:02X}"sv,
                              value, cfg.hotkeySaveCharacter);
                }
            }
            else if (keyLower == "hotkeyrestorecharacter" || keyLower == "hotkey_restorecharacter") {
                int vk = ParseVKey(value);
                if (vk > 0) {
                    cfg.hotkeyRestoreCharacter = vk;
                    PLOG_INFO("[PULSAR] Config: HotkeyRestoreCharacter = {} (VK 0x{:02X})"sv, value, vk);
                } else {
                    PLOG_WARN("[PULSAR] Config: HotkeyRestoreCharacter '{}' unparseable - keeping 0x{:02X}"sv,
                              value, cfg.hotkeyRestoreCharacter);
                }
            }
            else if (keyLower == "hotkeybuddy" || keyLower == "hotkey_buddy") {
                int vk = ParseVKey(value);
                if (vk > 0) {
                    cfg.hotkeyBuddy = vk;
                    PLOG_INFO("[PULSAR] Config: HotkeyBuddy = {} (VK 0x{:02X})"sv, value, vk);
                } else {
                    PLOG_WARN("[PULSAR] Config: HotkeyBuddy '{}' unparseable - keeping 0x{:02X}"sv,
                              value, cfg.hotkeyBuddy);
                }
            }
            else if (keyLower == "buddybaseformid" || keyLower == "buddy_base_formid") {
                cfg.buddyBaseFormID = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 0));
                PLOG_INFO("[PULSAR] Config: BuddyBaseFormID = 0x{:08X}"sv, cfg.buddyBaseFormID);
            }
            else if (keyLower == "devhotkeys" || keyLower == "dev_hotkeys") {
                std::string v = value;
                std::transform(v.begin(), v.end(), v.begin(), ::tolower);
                cfg.devHotkeys = (v == "1" || v == "true" || v == "yes" || v == "on");
                PLOG_INFO("[PULSAR] Config: DevHotkeys = {}"sv, cfg.devHotkeys);
            }
            else if (keyLower == "hotkeydevflashlightdump") {
                int vk = ParseVKey(value); if (vk > 0) cfg.hotkeyDevFlashlightDump = vk;
            }
            else if (keyLower == "hotkeydevdiag") {
                int vk = ParseVKey(value); if (vk > 0) cfg.hotkeyDevDiag = vk;
            }
            else if (keyLower == "hotkeydevmanualrevive") {
                int vk = ParseVKey(value); if (vk > 0) cfg.hotkeyDevManualRevive = vk;
            }
            else if (keyLower == "npcdirectcontrol" || keyLower == "npc_directcontrol") {
                std::string v = value;
                std::transform(v.begin(), v.end(), v.begin(), ::tolower);
                cfg.npcDirectControl = (v == "1" || v == "true" || v == "yes" || v == "on");
                PLOG_INFO("[PULSAR] Config: NpcDirectControl = {}"sv, cfg.npcDirectControl);
            }
            else if (keyLower == "npcnoimc" || keyLower == "npc_noimc") {
                std::string v = value;
                std::transform(v.begin(), v.end(), v.begin(), ::tolower);
                cfg.npcNoImc = (v == "1" || v == "true" || v == "yes" || v == "on");
                PLOG_INFO("[PULSAR] Config: NpcNoImc = {}"sv, cfg.npcNoImc);
            }
            else if (keyLower == "npccombatailive" || keyLower == "npc_combatailive") {
                std::string v = value;
                std::transform(v.begin(), v.end(), v.begin(), ::tolower);
                cfg.npcCombatAiLive = (v == "1" || v == "true" || v == "yes" || v == "on");
                PLOG_INFO("[PULSAR] Config: NpcCombatAiLive = {}"sv, cfg.npcCombatAiLive);
            }
            else if (keyLower == "dbauthoritative" || keyLower == "db_authoritative") {
                std::string v = value;
                std::transform(v.begin(), v.end(), v.begin(), ::tolower);
                cfg.dbAuthoritative = (v == "1" || v == "true" || v == "yes" || v == "on");
                PLOG_INFO("[PULSAR] Config: DbAuthoritative = {}"sv, cfg.dbAuthoritative);
            }
            else if (keyLower == "autoregroupinspace" || keyLower == "auto_regroup_in_space") {
                std::string v = value;
                std::transform(v.begin(), v.end(), v.begin(), ::tolower);
                cfg.autoRegroupInSpace = (v == "1" || v == "true" || v == "yes" || v == "on");
                PLOG_INFO("[PULSAR] Config: AutoRegroupInSpace = {}"sv, cfg.autoRegroupInSpace);
            }
            else if (keyLower == "coopnomenupause" || keyLower == "coop_no_menu_pause") {
                std::string v = value;
                std::transform(v.begin(), v.end(), v.begin(), ::tolower);
                cfg.coopNoMenuPause = (v == "1" || v == "true" || v == "yes" || v == "on");
                PLOG_INFO("[PULSAR] Config: CoopNoMenuPause = {}"sv, cfg.coopNoMenuPause);
            }
            else if (keyLower == "cooptimeweathersync" || keyLower == "coop_time_weather_sync") {
                std::string v = value;
                std::transform(v.begin(), v.end(), v.begin(), ::tolower);
                cfg.coopTimeWeatherSync = (v == "1" || v == "true" || v == "yes" || v == "on");
                PLOG_INFO("[PULSAR] Config: CoopTimeWeatherSync = {}"sv, cfg.coopTimeWeatherSync);
            }
            else if (keyLower == "e2eencryption" || keyLower == "e2e_encryption") {
                std::string v = value;
                std::transform(v.begin(), v.end(), v.begin(), ::tolower);
                cfg.e2eEncryption = (v == "1" || v == "true" || v == "yes" || v == "on");
                PLOG_INFO("[PULSAR] Config: E2EEncryption = {}"sv, cfg.e2eEncryption);
            }
            else if (keyLower == "e2erelayauth" || keyLower == "e2e_relay_auth") {
                std::string v = value;
                std::transform(v.begin(), v.end(), v.begin(), ::tolower);
                cfg.e2eRelayAuth = (v == "1" || v == "true" || v == "yes" || v == "on");
                PLOG_INFO("[PULSAR] Config: E2ERelayAuth = {}"sv, cfg.e2eRelayAuth);
            }
            else if (keyLower == "coopplanetfasttravelfollow" || keyLower == "coop_planet_fasttravel_follow") {
                std::string v = value;
                std::transform(v.begin(), v.end(), v.begin(), ::tolower);
                cfg.coopPlanetFastTravelFollow = (v == "1" || v == "true" || v == "yes" || v == "on");
                PLOG_INFO("[PULSAR] Config: CoopPlanetFastTravelFollow = {}"sv, cfg.coopPlanetFastTravelFollow);
            }
            else if (keyLower == "coopsharekillxp" || keyLower == "coop_share_kill_xp") {
                std::string v = value;
                std::transform(v.begin(), v.end(), v.begin(), ::tolower);
                cfg.coopShareKillXp = (v == "1" || v == "true" || v == "yes" || v == "on");
                PLOG_INFO("[PULSAR] Config: CoopShareKillXp = {}"sv, cfg.coopShareKillXp);
            }
            else if (keyLower == "cooptamecombatai" || keyLower == "coop_tame_combat_ai") {
                std::string v = value;
                std::transform(v.begin(), v.end(), v.begin(), ::tolower);
                cfg.coopTameCombatAI = (v == "1" || v == "true" || v == "yes" || v == "on");
                PLOG_INFO("[PULSAR] Config: CoopTameCombatAI = {}"sv, cfg.coopTameCombatAI);
            }
            else if (keyLower == "traceevents" || keyLower == "trace_events") {
                std::string v = value;
                std::transform(v.begin(), v.end(), v.begin(), ::tolower);
                cfg.traceEvents = (v == "1" || v == "true" || v == "yes" || v == "on");
                PLOG_INFO("[PULSAR] Config: TraceEvents = {}"sv, cfg.traceEvents);
            }
        }

        return cfg;
    }

    std::string GetPluginDir()
    {
        char modulePath[MAX_PATH]{};
        HMODULE hModule = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&LoadConfig), &hModule);
        GetModuleFileNameA(hModule, modulePath, MAX_PATH);

        std::string dir(modulePath);
        auto pos = dir.rfind('\\');
        if (pos != std::string::npos) {
            dir = dir.substr(0, pos + 1);
        }
        return dir;
    }
}
