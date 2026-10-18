// pulsar_fakepeer: headless PULSAR client for one-PC testing. Connects to a PULSAR
// host (Starfield with PULSAR.ini isHost=1) and plays the second player: key
// exchange, handshake, then PKT_POSITION at ~30 Hz so the host spawns the clone.
// Usage: see PrintUsage() or docs/TESTING.md ("Fake peer").

#include "FakePeer.h"

#include <windows.h>
#include <timeapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using namespace Pulsar::FakePeer;

namespace {

    std::atomic<bool> g_stop{ false };

    BOOL WINAPI OnCtrl(DWORD)
    {
        g_stop = true;
        return TRUE;
    }

    void PrintUsage()
    {
        std::printf(
            "pulsar_fakepeer - headless PULSAR client (plays player 2 against a local host)\n"
            "\n"
            "  --host ADDR          host address (default 127.0.0.1)\n"
            "  --port N             host port (default 7777)\n"
            "  --secret S           E2E auth secret; only for a lobby/relay session (default none)\n"
            "  --no-e2e             plaintext; only if the host runs E2EEncryption=0\n"
            "  --name S             handshake player name (default FakePeer)\n"
            "  --mode M             stand | circle | follow (default stand)\n"
            "  --offset X,Y,Z       offset from the anchor, game units (default 200,0,0)\n"
            "  --radius R           circle radius (default 300)\n"
            "  --period S           circle: seconds per lap (default 12)\n"
            "  --delay MS           follow: lag behind the host by MS (default 1000)\n"
            "  --rate HZ            position packets per second (default 30)\n"
            "  --anchor X,Y,Z[,CELL] fixed anchor instead of the host's first position\n"
            "                       (CELL hex, default 0 = no cell check on the host)\n"
            "  --no-handshake       skip PKT_HANDSHAKE (host then does not quicksave/send a save)\n"
            "  --no-load-complete   do not answer a save with PKT_CLIENT_LOAD_COMPLETE\n"
            "  --save-out PATH      write a received save file to PATH\n"
            "  --duration S         quit after S seconds (default: run until Ctrl+C)\n"
            "  --no-reconnect       exit instead of reconnecting when the link drops\n");
    }

    bool ParseFloats(const char* a_s, float* a_out, int a_n)
    {
        for (int i = 0; i < a_n; ++i) {
            char* end = nullptr;
            a_out[i] = std::strtof(a_s, &end);
            if (end == a_s) return false;
            a_s = end;
            if (i + 1 < a_n) {
                if (*a_s != ',') return false;
                ++a_s;
            }
        }
        return true;
    }

    void Log(const std::string& a_line)
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        std::printf("[%02u:%02u:%02u.%03u] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, a_line.c_str());
        std::fflush(stdout);
    }
}

int main(int argc, char** argv)
{
    Options o;
    double durationS   = 0.0;
    bool   reconnect   = true;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") { PrintUsage(); return 0; }
        else if (a == "--host")    o.address = next();
        else if (a == "--port")    o.port = static_cast<uint16_t>(std::strtoul(next(), nullptr, 10));
        else if (a == "--secret")  o.secret = next();
        else if (a == "--no-e2e")  o.e2e = false;
        else if (a == "--name")    o.name = next();
        else if (a == "--mode") {
            const std::string m = next();
            if (m == "stand") o.mode = Mode::kStand;
            else if (m == "circle") o.mode = Mode::kCircle;
            else if (m == "follow") o.mode = Mode::kFollow;
            else { std::fprintf(stderr, "unknown mode '%s'\n", m.c_str()); return 2; }
        }
        else if (a == "--offset") {
            if (!ParseFloats(next(), o.offset, 3)) { std::fprintf(stderr, "--offset wants X,Y,Z\n"); return 2; }
        }
        else if (a == "--radius")  o.radius = std::strtof(next(), nullptr);
        else if (a == "--period")  o.periodS = std::strtof(next(), nullptr);
        else if (a == "--delay")   o.followDelayMs = std::atoi(next());
        else if (a == "--rate")    o.rateHz = std::atoi(next());
        else if (a == "--anchor") {
            const char* s = next();
            if (!ParseFloats(s, o.anchor, 3)) { std::fprintf(stderr, "--anchor wants X,Y,Z[,CELL]\n"); return 2; }
            int commas = 0;
            const char* p = s;
            for (; *p && commas < 3; ++p) if (*p == ',') ++commas;
            if (commas == 3) o.anchorCell = static_cast<uint32_t>(std::strtoul(p, nullptr, 16));
            o.hasAnchor = true;
        }
        else if (a == "--no-handshake")     o.sendHandshake = false;
        else if (a == "--no-load-complete") o.sendLoadComplete = false;
        else if (a == "--save-out")         o.saveOut = next();
        else if (a == "--duration")         durationS = std::atof(next());
        else if (a == "--no-reconnect")     reconnect = false;
        else { std::fprintf(stderr, "unknown argument '%s'\n\n", a.c_str()); PrintUsage(); return 2; }
    }

    if (enet_initialize() != 0) { std::fprintf(stderr, "enet_initialize failed\n"); return 1; }
    SetConsoleCtrlHandler(OnCtrl, TRUE);
    timeBeginPeriod(1);   // 1 ms sleeps so 30 Hz stays steady

    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    auto nextStats = start + std::chrono::seconds(5);
    int exitCode = 0;

    FakePeer bot(o);
    bot.log = Log;
    if (!bot.Connect()) { exitCode = 1; g_stop = true; }

    const auto connectDeadline = [] { return Clock::now() + std::chrono::seconds(10); };
    auto deadline = connectDeadline();
    bool warnedNoHostPos = false;

    while (!g_stop) {
        bot.Service(1);
        bot.Tick();

        const auto now = Clock::now();
        if (durationS > 0 && now - start >= std::chrono::duration<double>(durationS)) break;

        if (!bot.Connected() && !bot.DisconnectedByHost() && now > deadline) {
            Log("no answer from the host - is Starfield running as PULSAR host on that port?");
            deadline = connectDeadline();
        }
        if (bot.Connected() && bot.SecureReady() && !bot.HasHostPosition() && !o.hasAnchor &&
            !warnedNoHostPos && now - start > std::chrono::seconds(10)) {
            Log("still no PKT_POSITION from the host - nothing to anchor on (use --anchor to force)");
            warnedNoHostPos = true;
        }
        if (bot.WantsReconnect()) {
            if (!reconnect) { exitCode = 3; break; }
            bot.Disconnect();
            Log("reconnecting in 3 s");
            for (int i = 0; i < 30 && !g_stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (g_stop) break;
            bot.Connect();
            deadline = connectDeadline();
        }
        if (now >= nextStats) {
            Log(bot.StatsLine());
            nextStats = now + std::chrono::seconds(5);
        }
    }

    Log("final: " + bot.StatsLine());
    bot.Disconnect();
    timeEndPeriod(1);
    enet_deinitialize();
    return exitCode;
}
