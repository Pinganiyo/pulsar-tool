#pragma once

// Recent local quest-stage sets with timestamps, so stages advanced while the
// peer was disconnected can be replayed when it reconnects without a save
// bridge (the host skips re-sending its save for a reconnect within 90 s).
// Game-free (tests/test_stage_journal.cpp).

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace Pulsar::QuestSync {

    struct StageEntry {
        std::uint32_t quest = 0;
        std::uint16_t stage = 0;
        std::uint64_t atMs  = 0;
    };

    class StageJournal {
    public:
        static constexpr std::size_t kMaxEntries = 1024;

        void Record(std::uint32_t a_quest, std::uint16_t a_stage, std::uint64_t a_nowMs)
        {
            if (!m_entries.empty() && m_entries.back().quest == a_quest &&
                m_entries.back().stage == a_stage)
                return;   // the engine re-fires the same stage; keep one
            m_entries.push_back({ a_quest, a_stage, a_nowMs });
            if (m_entries.size() > kMaxEntries) m_entries.pop_front();
        }

        // Stages set at or after a_sinceMs, oldest first, each (quest, stage) once.
        std::vector<StageEntry> Since(std::uint64_t a_sinceMs) const
        {
            std::vector<StageEntry> out;
            for (const auto& e : m_entries) {
                if (e.atMs < a_sinceMs) continue;
                bool dup = false;
                for (const auto& o : out)
                    if (o.quest == e.quest && o.stage == e.stage) { dup = true; break; }
                if (!dup) out.push_back(e);
            }
            return out;
        }

        bool ContainsSince(std::uint32_t a_quest, std::uint16_t a_stage, std::uint64_t a_sinceMs) const
        {
            for (const auto& e : m_entries)
                if (e.atMs >= a_sinceMs && e.quest == a_quest && e.stage == a_stage) return true;
            return false;
        }

        std::size_t Size() const { return m_entries.size(); }
        void        Clear() { m_entries.clear(); }

    private:
        std::deque<StageEntry> m_entries;
    };
}
