#pragma once

// Client-side "are we still in the same place as the host?" watch, used to ask
// the host for a regroup (save bridge) automatically when the client's space
// world has diverged - the "missing station / POI, stuck inside the ship" bug.
// Game-free so it can be unit-tested (tests/test_divergence_watch.cpp).
//
// A regroup reloads the save, so a false positive is worse than the bug. The
// rules are deliberately conservative: only while the client is in space, only
// after the mismatch persists, a long cooldown, and a per-session cap.

#include <cstdint>

namespace Pulsar::Coop {

    struct DivergenceConfig {
        std::uint64_t dwellMs      = 20000;    // mismatch must last this long
        std::uint64_t cooldownMs   = 120000;   // between automatic requests
        std::uint64_t staleMs      = 5000;     // host cell older than this = unknown
        std::uint32_t maxPerSession = 3;
    };

    // Procedural cells get per-machine FF-prefixed ids, so two different FF ids
    // can be the same place. Treat them as matching (same rule as CellDiag).
    inline bool SameCell(std::uint32_t a_local, std::uint32_t a_remote)
    {
        if (a_local == a_remote) return true;
        const bool lp = (a_local & 0xFF000000u) == 0xFF000000u;
        const bool rp = (a_remote & 0xFF000000u) == 0xFF000000u;
        return lp && rp;
    }

    class DivergenceWatch {
    public:
        explicit DivergenceWatch(DivergenceConfig a_cfg = {}) : m_cfg(a_cfg) {}

        // One tick. a_remoteCellAtMs = when the host's cell was last received
        // (0 = never). Returns true when a regroup should be requested now.
        bool Tick(std::uint64_t a_nowMs, bool a_localInSpace, std::uint32_t a_localCell,
                  std::uint32_t a_remoteCell, std::uint64_t a_remoteCellAtMs)
        {
            const bool known = a_localCell != 0 && a_remoteCell != 0 && a_remoteCellAtMs != 0 &&
                               a_nowMs >= a_remoteCellAtMs &&
                               a_nowMs - a_remoteCellAtMs <= m_cfg.staleMs;
            if (!a_localInSpace || !known || SameCell(a_localCell, a_remoteCell)) {
                m_mismatchSinceMs = 0;
                return false;
            }
            if (m_mismatchSinceMs == 0) m_mismatchSinceMs = a_nowMs;
            if (a_nowMs - m_mismatchSinceMs < m_cfg.dwellMs) return false;
            if (m_requests >= m_cfg.maxPerSession) return false;
            if (m_lastRequestMs != 0 && a_nowMs - m_lastRequestMs < m_cfg.cooldownMs) return false;

            m_lastRequestMs   = a_nowMs;
            m_mismatchSinceMs = 0;   // a new full dwell is needed after a request
            ++m_requests;
            return true;
        }

        // A save bridge happened (any cause): the worlds were just re-synced.
        void OnBridged(std::uint64_t /*a_nowMs*/) { m_mismatchSinceMs = 0; }

        // New co-op session: the per-session cap starts over.
        void ResetSession()
        {
            m_mismatchSinceMs = 0;
            m_lastRequestMs   = 0;
            m_requests        = 0;
        }

        std::uint32_t Requests() const { return m_requests; }

    private:
        DivergenceConfig m_cfg;
        std::uint64_t    m_mismatchSinceMs = 0;
        std::uint64_t    m_lastRequestMs   = 0;
        std::uint32_t    m_requests        = 0;
    };
}
