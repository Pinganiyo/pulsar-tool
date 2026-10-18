// Profile v2 codec: ProfileData <-> JSON blob. Kept free of game headers so it
// can be unit-tested outside Starfield (tests/test_profile_codec.cpp).

#include "PCH.h"
#include "Profile/ClientProfile.h"
#include "Profile/ProfileJson.h"

#include <cstdlib>
#include <string>
#include <vector>

using namespace std::string_view_literals;

namespace Pulsar::Profile {

    // ─── v2 JSON codec ───

    static void AppendModsCSV(std::string& out, const std::vector<uint32_t>& mods)
    {
        for (size_t i = 0; i < mods.size(); ++i) {
            if (i) out += ' ';
            out += std::to_string(mods[i]);
        }
    }

    // Phase C0 - perks as a flat space-separated "formID rank formID rank ..."
    // list (a single JSON string, so it never collides with the inventory array's
    // closing ']' that DecodeProfile keys on). Raw formID (base-game perks stable).
    static void AppendPerksCSV(std::string& out, const std::vector<ProfilePerk>& perks)
    {
        for (size_t i = 0; i < perks.size(); ++i) {
            if (i) out += ' ';
            out += std::to_string(perks[i].formID);
            out += ' ';
            out += std::to_string(static_cast<uint32_t>(static_cast<uint8_t>(perks[i].rank)));
        }
    }

    static std::vector<ProfilePerk> ParsePerksCSV(const std::string& csv)
    {
        std::vector<ProfilePerk> out;
        size_t p = 0;
        auto nextU = [&](uint32_t& v) -> bool {
            while (p < csv.size() && (csv[p] == ' ' || csv[p] == ',')) ++p;
            if (p >= csv.size()) return false;
            v = static_cast<uint32_t>(std::strtoul(csv.c_str() + p, nullptr, 10));
            while (p < csv.size() && csv[p] != ' ' && csv[p] != ',') ++p;
            return true;
        };
        uint32_t fid = 0, rank = 0;
        while (nextU(fid)) {
            if (!nextU(rank)) break;       // malformed trailing token - drop
            ProfilePerk pk;
            pk.formID = fid;
            pk.rank   = static_cast<int8_t>(static_cast<uint8_t>(rank));
            out.push_back(std::move(pk));
        }
        return out;
    }

    std::vector<uint8_t> EncodeProfile(const ProfileData& a_data)
    {
        std::string j;
        j += "{\n";
        j += "  \"version\": " + std::to_string(a_data.version) + ",\n";
        j += "  \"playerName\": \"" + Json::Escape(a_data.playerName) + "\",\n";
        j += "  \"playerLevel\": " + std::to_string(a_data.playerLevel) + ",\n";
        j += "  \"raceName\": \"" + Json::Escape(a_data.raceName) + "\",\n";
        j += "  \"gender\": " + std::to_string(a_data.gender) + ",\n";
        j += "  \"credits\": " + std::to_string(a_data.credits) + ",\n";
        j += "  \"o2\": " + std::to_string(a_data.o2) + ",\n";
        j += "  \"o2Max\": " + std::to_string(a_data.o2Max) + ",\n";
        j += "  \"healthBase\": " + std::to_string(a_data.healthBase) + ",\n";
        j += "  \"carryWeightBase\": " + std::to_string(a_data.carryWeightBase) + ",\n";
        j += "  \"o2Base\": " + std::to_string(a_data.o2Base) + ",\n";
        j += "  \"skillPoints\": " + std::to_string(a_data.skillPoints) + ",\n";
        j += "  \"xp\": " + std::to_string(a_data.xp) + ",\n";   // V3
        {
            std::string perksCsv;
            AppendPerksCSV(perksCsv, a_data.perks);
            j += "  \"perks\": \"" + perksCsv + "\",\n";
        }
        {
            // Powers (gap #2) - flat space-separated formID list (reuses the mods CSV
            // writer; a single JSON string so it can't collide with the inventory array).
            std::string powersCsv;
            AppendModsCSV(powersCsv, a_data.powers);
            j += "  \"powers\": \"" + powersCsv + "\",\n";
        }
        {
            // Research (gap #1) - completed-project formID list (same flat CSV form).
            std::string researchCsv;
            AppendModsCSV(researchCsv, a_data.research);
            j += "  \"research\": \"" + researchCsv + "\",\n";
        }
        j += "  \"inventory\": [\n";
        for (size_t i = 0; i < a_data.inventory.size(); ++i) {
            const auto& it = a_data.inventory[i];
            std::string modsCsv;
            AppendModsCSV(modsCsv, it.mods);
            j += "    {\"formID\": " + std::to_string(it.formID);
            j += ", \"count\": " + std::to_string(it.count);
            j += ", \"plugin\": \"" + Json::Escape(it.plugin) + "\"";
            j += ", \"localID\": " + std::to_string(it.localID);
            j += ", \"equipped\": " + std::string(it.equipped ? "1" : "0");
            j += ", \"modSentinel\": " + std::to_string(it.modSentinel);
            j += ", \"mods\": \"" + modsCsv + "\"}";
            if (i + 1 < a_data.inventory.size()) j += ",";
            j += "\n";
        }
        j += "  ]\n}\n";
        return std::vector<uint8_t>(j.begin(), j.end());
    }

    static std::vector<uint32_t> ParseModsCSV(const std::string& csv)
    {
        std::vector<uint32_t> out;
        size_t p = 0;
        while (p < csv.size()) {
            while (p < csv.size() && (csv[p] == ' ' || csv[p] == ',')) ++p;
            if (p >= csv.size()) break;
            out.push_back(static_cast<uint32_t>(std::strtoul(csv.c_str() + p, nullptr, 10)));
            while (p < csv.size() && csv[p] != ' ' && csv[p] != ',') ++p;
        }
        return out;
    }

    bool DecodeProfile(std::span<const uint8_t> a_blob, ProfileData& a_out)
    {
        if (a_blob.empty()) return false;
        std::string json(reinterpret_cast<const char*>(a_blob.data()), a_blob.size());

        // Structural check before trusting anything: EncodeProfile always writes an
        // object with "version" and a closed "inventory" array as the last member.
        // A truncated or foreign blob used to decode as a partial/empty profile and
        // report success, which could silently drop the client's items.
        {
            const auto first = json.find_first_not_of(" \t\r\n");
            const auto last  = json.find_last_not_of(" \t\r\n");
            if (first == std::string::npos || json[first] != '{' || json[last] != '}')
                return false;
            if (!Json::HasKey(json, "version")) return false;
            const auto inv = json.find("\"inventory\"");
            if (inv == std::string::npos) return false;
            const auto open  = json.find('[', inv);
            const auto close = json.rfind(']');
            if (open == std::string::npos || close == std::string::npos || close < open)
                return false;
        }

        a_out = ProfileData{};
        a_out.version    = Json::ExtractUInt(json, "version");
        a_out.playerName = Json::ExtractString(json, "playerName");
        a_out.playerLevel= Json::ExtractUInt(json, "playerLevel");
        a_out.raceName   = Json::ExtractString(json, "raceName");
        a_out.gender     = static_cast<uint16_t>(Json::ExtractUInt(json, "gender"));
        a_out.credits    = Json::ExtractUInt(json, "credits");
        auto extractFloat = [&](const char* key, float& out) {
            std::string needle = std::string("\"") + key + "\"";
            auto pos = json.find(needle);
            if (pos == std::string::npos) return;
            pos = json.find(':', pos);
            if (pos != std::string::npos) out = std::strtof(json.c_str() + pos + 1, nullptr);
        };
        extractFloat("o2", a_out.o2);
        extractFloat("o2Max", a_out.o2Max);
        extractFloat("healthBase", a_out.healthBase);
        extractFloat("carryWeightBase", a_out.carryWeightBase);
        extractFloat("o2Base", a_out.o2Base);
        extractFloat("xp", a_out.xp);   // V3

        // Phase C0 - skill points + perks (perks is a flat string, parsed before the
        // inventory array; ExtractString stops at the closing quote so it can't run
        // into the array).
        a_out.skillPoints = Json::ExtractUInt(json, "skillPoints");
        a_out.perks       = ParsePerksCSV(Json::ExtractString(json, "perks"));
        a_out.powers      = ParseModsCSV(Json::ExtractString(json, "powers"));   // gap #2
        a_out.research    = ParseModsCSV(Json::ExtractString(json, "research")); // gap #1

        // Parse the inventory array of {...} objects (reuse the per-object scan style).
        auto invPos = json.find("\"inventory\"");
        if (invPos != std::string::npos) {
            auto arrStart = json.find('[', invPos);
            auto arrEnd   = json.rfind(']');
            if (arrStart != std::string::npos && arrEnd != std::string::npos && arrEnd > arrStart) {
                std::string arr = json.substr(arrStart + 1, arrEnd - arrStart - 1);
                size_t p = 0;
                while (p < arr.size()) {
                    auto objStart = arr.find('{', p);
                    if (objStart == std::string::npos) break;
                    auto objEnd = arr.find('}', objStart);
                    if (objEnd == std::string::npos) break;
                    std::string obj = arr.substr(objStart, objEnd - objStart + 1);
                    ProfileItem it;
                    it.formID      = Json::ExtractUInt(obj, "formID");
                    it.count       = Json::ExtractInt(obj, "count");
                    it.plugin      = Json::ExtractString(obj, "plugin");
                    it.localID     = Json::ExtractUInt(obj, "localID");
                    it.equipped    = Json::ExtractUInt(obj, "equipped") != 0;
                    it.modSentinel = static_cast<uint16_t>(Json::ExtractUInt(obj, "modSentinel"));
                    it.mods        = ParseModsCSV(Json::ExtractString(obj, "mods"));
                    if (it.count != 0) a_out.inventory.push_back(it);
                    p = objEnd + 1;
                }
            }
        }
        return true;
    }

    bool RunSelfTest()
    {
        ProfileData a;
        a.version    = 2;
        a.playerName = "SelfTest";
        a.credits    = 12345;
        a.o2         = 87.5f;
        ProfileItem it;
        it.formID      = 0x0001A2B3;
        it.count       = 3;
        it.plugin      = "Starfield.esm";
        it.localID     = 0x1A2B3;
        it.equipped    = true;
        it.mods        = { 111u, 222u, 333u };
        it.modSentinel = 7;
        a.inventory.push_back(it);
        // Phase C0 - perks + skill points round-trip.
        a.skillPoints = 4;
        a.perks.push_back({ 0x0024AB10u, 3, "", 0 });
        a.perks.push_back({ 0x0024AB20u, 1, "", 0 });

        std::vector<uint8_t> blob = EncodeProfile(a);
        ProfileData b;
        const bool decoded = DecodeProfile(blob, b);

        const bool ok =
            decoded &&
            b.version == a.version &&
            b.playerName == a.playerName &&
            b.credits == a.credits &&
            (b.o2 > a.o2 - 0.01f && b.o2 < a.o2 + 0.01f) &&
            b.inventory.size() == 1 &&
            b.inventory[0].count == it.count &&
            b.inventory[0].plugin == it.plugin &&
            b.inventory[0].localID == it.localID &&
            b.inventory[0].equipped == it.equipped &&
            b.inventory[0].mods == it.mods &&
            b.inventory[0].modSentinel == it.modSentinel &&
            b.skillPoints == a.skillPoints &&
            b.perks.size() == 2 &&
            b.perks[0].formID == a.perks[0].formID &&
            b.perks[0].rank == a.perks[0].rank &&
            b.perks[1].formID == a.perks[1].formID &&
            b.perks[1].rank == a.perks[1].rank;

        if (ok) {
            PLOG_INFO("[PROFILE-TEST] codec round-trip PASS"sv);
        } else {
            PLOG_WARN("[PROFILE-TEST] codec round-trip FAIL (decoded={}, items={}, perks={}, skillPts={})"sv,
                      decoded, b.inventory.size(), b.perks.size(), b.skillPoints);
        }
        return ok;
    }

}
