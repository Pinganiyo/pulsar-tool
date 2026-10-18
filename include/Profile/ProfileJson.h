#pragma once

// Minimal JSON helpers for the profile formats (no dependency needed). Game-free
// so the codec can be unit-tested outside Starfield (see tests/).

#include <cstdint>
#include <cstdlib>
#include <string>

namespace Pulsar::Profile::Json {

    inline std::string Escape(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:   out += c; break;
            }
        }
        return out;
    }

    // Position just past `"key":` (and any spaces/tabs), or npos.
    inline size_t ValuePos(const std::string& json, const std::string& key)
    {
        const std::string search = "\"" + key + "\"";
        auto pos = json.find(search);
        if (pos == std::string::npos) return std::string::npos;
        pos = json.find(':', pos + search.size());
        if (pos == std::string::npos) return std::string::npos;
        ++pos;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
        return pos;
    }

    // String value for `key`, with the escapes Escape() writes undone. A quote
    // preceded by a backslash does not end the string.
    inline std::string ExtractString(const std::string& json, const std::string& key)
    {
        auto pos = ValuePos(json, key);
        if (pos == std::string::npos || pos >= json.size() || json[pos] != '"') return "";
        std::string out;
        for (size_t i = pos + 1; i < json.size(); ++i) {
            const char c = json[i];
            if (c == '"') return out;
            if (c == '\\' && i + 1 < json.size()) {
                const char n = json[++i];
                switch (n) {
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    default:  out += n;    break;   // \" and \\ (and anything else) literal
                }
                continue;
            }
            out += c;
        }
        return "";   // unterminated string
    }

    inline uint32_t ExtractUInt(const std::string& json, const std::string& key)
    {
        auto pos = ValuePos(json, key);
        if (pos == std::string::npos) return 0;
        return static_cast<uint32_t>(std::strtoul(json.c_str() + pos, nullptr, 10));
    }

    inline int32_t ExtractInt(const std::string& json, const std::string& key)
    {
        auto pos = ValuePos(json, key);
        if (pos == std::string::npos) return 0;
        return static_cast<int32_t>(std::strtol(json.c_str() + pos, nullptr, 10));
    }

    inline bool HasKey(const std::string& json, const std::string& key)
    {
        return ValuePos(json, key) != std::string::npos;
    }
}
