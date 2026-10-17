using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Pulsar.Services;

// One lobby row as published by the daemon in pulsar_lobbies.json.
public sealed record LobbyInfo(
    ulong LobbyId,
    string Name,
    string Status,
    long MemberCount,
    long MaxPlayers,
    bool HasPassword,
    uint ModCount = 0,
    string ModList = "");

// One relay row as published by the daemon in pulsar_relays.json (Phase 2 backend).
// Sessions/SoftCap/LoadPct/Accepting are the self-reported load (heartbeat); older
// relays that don't report them default to 0 load + Accepting=true.
public sealed record RelayInfo(
    string Name,
    string Region,
    string PublicHost,
    bool Verified,
    int Sessions,
    int SoftCap,
    int LoadPct,
    bool Accepting,
    long LastSeenUnixMs);

// Pure-C# port of the WinForms file-handoff contract (companion-gui/MainForm.cs):
// writes pulsar_cmd.json (GUI->daemon), polls pulsar_ack.json + pulsar_lobbies.json
// (daemon->GUI), and owns the `companion serve` child process. Framework-agnostic -
// it raises events; the ViewModel marshals them onto the UI thread.
public sealed class DaemonBridge : IDisposable
{
    private const string PwSalt = "PULSAR-lobby-v1:";

    private readonly AppConfig _config;
    private readonly string _sharedDir;
    private readonly string _cmdPath;
    private readonly string _ackPath;
    private readonly string _lobbiesPath;
    private readonly string _relaysPath;

    private Process? _daemon;
    private string _lastLobbiesJson = "";
    private string _lastRelaysJson = "";
    private string _lastAckCmdId = "";
    private string _pendingCmdId = "";
    private string _pendingKind = "";       // host | join | joinCode | register | close
    private bool _launchAfterPending;
    private bool _daemonReady;

    public DaemonBridge(AppConfig config)
    {
        _config = config;
        _sharedDir = AppConfig.SharedDir;
        Directory.CreateDirectory(_sharedDir);
        _cmdPath = Path.Combine(_sharedDir, "pulsar_cmd.json");
        _ackPath = Path.Combine(_sharedDir, "pulsar_ack.json");
        _lobbiesPath = Path.Combine(_sharedDir, "pulsar_lobbies.json");
        _relaysPath = Path.Combine(_sharedDir, "pulsar_relays.json");
    }

    // ── events (the ViewModel subscribes; it must marshal to the UI thread) ─────
    public event Action<string>? Log;
    public event Action? Connected;
    public event Action? Disconnected;
    public event Action? DaemonReady;                 // daemon "subscribed" line seen
    public event Action<string>? ProfileSaved;        // arg = "HH:mm"
    public event Action<IReadOnlyList<LobbyInfo>>? LobbiesUpdated;
    public event Action<IReadOnlyList<RelayInfo>>? RelaysUpdated;

    // Load-aware auto-pick: the shell sets this (best accepting relay from the master-
    // list) right before Start() in default Online mode. Empty → fall back to the DB
    // host's :7777. A manual Advanced RelayOverride still wins over this.
    public string AutoRelayAddr { get; set; } = "";

    // Ack callbacks, keyed by command kind.
    public event Action<bool, string>? RegisterAck;   // (ok, error)
    public event Action<bool, string, string>? HostAck; // (ok, error, code)
    public event Action<bool, string>? DirectHostAck;   // (ok, error) - self-host, no DB lobby
    public event Action<bool, string>? CloseAck;      // (ok, error)
    public event Action<bool, string, string>? JoinAck; // (ok, error, kind)
    public event Action<bool, string>? ResetAck;      // (ok, error) - character wipe
    public event Action? LaunchRequested;             // launch-after-pending fired

    public bool IsConnected => _daemon is { HasExited: false };
    public bool IsDaemonReady => _daemonReady;

    // ── daemon lifecycle ───────────────────────────────────────────────────────
    public bool Start(out string error)
    {
        error = "";
        if (IsConnected) return true;

        var exe = GuessCompanionExe();
        if (!File.Exists(exe))
        {
            error = "PulsarCompanion.exe not found:\n" + exe;
            return false;
        }

        var name = string.IsNullOrWhiteSpace(_config.PlayerName) ? "Player" : _config.PlayerName.Trim();
        var stdbUri = _config.ResolveStdbUri();
        string relay;
        if (_config.DbMode == DbMode.Local)
            // One-click local stack runs the relay on 127.0.0.1:7777 (Phase 4).
            relay = LocalStack.LocalRelayAddr;
        else if (_config.Advanced && _config.RelayOverride.Trim().Length > 0)
            relay = _config.RelayOverride.Trim();
        else if (AutoRelayAddr.Trim().Length > 0)
            relay = AutoRelayAddr.Trim();      // load-aware auto-pick from the master-list
        else
            relay = HostOnly(stdbUri) + ":7777";

        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = exe,
                WorkingDirectory = Path.GetDirectoryName(exe) ?? _sharedDir,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                RedirectStandardInput = true,
            };
            psi.ArgumentList.Add("serve");
            psi.ArgumentList.Add(name);   // ArgumentList quotes safely
            psi.Environment["PULSAR_STDB_URI"] = stdbUri;
            psi.Environment["PULSAR_RELAY_ADDR"] = relay;
            psi.Environment["PULSAR_AUTOSAVE"] = _config.AutoSave ? "1" : "0";
            // Mirror the "Show community relays" toggle: the daemon's host-side relay pick
            // honours verified-only unless the user opted into community relays.
            psi.Environment["PULSAR_RELAY_VERIFIED_ONLY"] = _config.ShowCommunityRelays ? "0" : "1";

            _daemon = new Process { StartInfo = psi, EnableRaisingEvents = true };
            _daemon.OutputDataReceived += (_, e) =>
            {
                if (e.Data == null) return;
                Log?.Invoke("daemon: " + e.Data);
                if (e.Data.Contains("subscribed"))
                {
                    _daemonReady = true;
                    DaemonReady?.Invoke();
                }
                if (e.Data.Contains("auto-saved profile"))
                    ProfileSaved?.Invoke(DateTime.Now.ToString("HH:mm"));
            };
            _daemon.ErrorDataReceived += (_, e) => { if (e.Data != null) Log?.Invoke("daemon! " + e.Data); };
            _daemon.Exited += (_, _) =>
            {
                _daemonReady = false;
                Disconnected?.Invoke();
                Log?.Invoke("Daemon stopped.");
            };

            _daemonReady = false;
            _daemon.Start();
            _daemon.BeginOutputReadLine();
            _daemon.BeginErrorReadLine();

            Connected?.Invoke();
            Log?.Invoke($"Daemon started (server={stdbUri}, relay={relay}).");
            return true;
        }
        catch (Exception ex)
        {
            error = "Start failed: " + ex.Message;
            Log?.Invoke(error);
            return false;
        }
    }

    public void Stop()
    {
        try
        {
            if (_daemon is { HasExited: false })
            {
                _daemon.Kill(true);
                _daemon.WaitForExit(2000);
            }
        }
        catch { /* best-effort */ }
        _daemon = null;
        _daemonReady = false;
    }

    // Called on window close.
    public void Shutdown() => Stop();

    // ── commands ────────────────────────────────────────────────────────────────
    public void Register(string name)
    {
        var json = "{\"action\":\"register\",\"cmdId\":" + Esc(NewId(out var id)) + ",\"name\":" + Esc(name) + "}";
        Send("register", json, id, false, $"Claiming '{name}' ...");
    }

    public void Host(string lobbyName, string password, int max, bool launchAfter,
                     uint modCount = 0, string modList = "")
    {
        var json = "{\"action\":\"host\",\"cmdId\":" + Esc(NewId(out var id))
                 + ",\"name\":" + Esc(lobbyName)
                 + ",\"max\":" + max
                 + ",\"passwordHash\":" + Esc(HashPw(password))
                 + ",\"modCount\":" + modCount
                 + ",\"modList\":" + Esc(modList ?? "") + "}";
        Send("host", json, id, launchAfter, $"Hosting '{lobbyName}' ...");
    }

    public void Join(ulong lobbyId, string password, bool launchAfter)
    {
        var json = "{\"action\":\"join\",\"cmdId\":" + Esc(NewId(out var id))
                 + ",\"lobbyId\":" + lobbyId
                 + ",\"passwordHash\":" + Esc(HashPw(password)) + "}";
        Send("join", json, id, launchAfter, $"Joining lobby {lobbyId} ...");
    }

    public void JoinByCode(string code, string password, bool launchAfter)
    {
        var json = "{\"action\":\"joinCode\",\"cmdId\":" + Esc(NewId(out var id))
                 + ",\"code\":" + Esc(code)
                 + ",\"passwordHash\":" + Esc(HashPw(password)) + "}";
        Send("joinCode", json, id, launchAfter, $"Joining by code {code} ...");
    }

    public void Close()
    {
        var json = "{\"action\":\"close\",\"cmdId\":" + Esc(NewId(out var id)) + "}";
        Send("close", json, id, false, "Closing lobby ...");
    }

    // Character wipe: the daemon deletes THIS identity's DB profile + local mirror.
    // Requires the daemon to be connected; ResetAck fires with the result.
    public void ResetProfile()
    {
        var json = "{\"action\":\"resetProfile\",\"cmdId\":" + Esc(NewId(out var id)) + "}";
        Send("resetProfile", json, id, false, "Wiping character ...");
    }

    // Self-host (pure direct): no DB lobby, no relay, no session. The DLL StartHosts on
    // its game port; the friend connects straight to the host's IP:port. HostAck fires.
    public void HostDirect(bool launchAfter)
    {
        var json = "{\"action\":\"hostDirect\",\"cmdId\":" + Esc(NewId(out var id)) + "}";
        Send("hostDirect", json, id, launchAfter, "Hosting (direct) ...");
    }

    // Join by IP (pure direct). JoinAck fires with kind "joinDirect".
    public void JoinDirect(string ip, int port, bool launchAfter)
    {
        var addr = ip + ":" + port;
        var json = "{\"action\":\"joinDirect\",\"cmdId\":" + Esc(NewId(out var id))
                 + ",\"addr\":" + Esc(addr) + "}";
        Send("joinDirect", json, id, launchAfter, $"Joining {addr} (direct) ...");
    }

    private void Send(string kind, string json, string cmdId, bool launchAfter, string logMsg)
    {
        try
        {
            _pendingKind = kind;
            _pendingCmdId = cmdId;
            _launchAfterPending = launchAfter;
            File.WriteAllText(_cmdPath, json);
            Log?.Invoke(logMsg);
        }
        catch (Exception ex)
        {
            Log?.Invoke("command write failed: " + ex.Message);
            _pendingCmdId = "";
        }
    }

    // ── polling (call from a ~1s timer on the UI thread) ────────────────────────
    public void Poll()
    {
        RefreshLobbies();
        RefreshRelays();
        CheckAck();
    }

    // Manual refresh (the "refresh" button): bust the change-cache so the next read
    // re-parses + re-emits the current published lobby list immediately.
    public void RefreshLobbiesNow()
    {
        _lastLobbiesJson = "";
        RefreshLobbies();
    }

    private void RefreshLobbies()
    {
        string json;
        try { json = File.Exists(_lobbiesPath) ? File.ReadAllText(_lobbiesPath) : "[]"; }
        catch { return; }
        if (json == _lastLobbiesJson) return;
        _lastLobbiesJson = json;

        var list = new List<LobbyInfo>();
        try
        {
            using var doc = JsonDocument.Parse(json);
            foreach (var e in doc.RootElement.EnumerateArray())
            {
                ulong id = e.TryGetProperty("lobbyId", out var v) ? v.GetUInt64() : 0;
                string nm = Str(e, "name");
                string st = Str(e, "status");
                long mc = e.TryGetProperty("memberCount", out var m) ? m.GetInt64() : 0;
                long mx = e.TryGetProperty("maxPlayers", out var x) ? x.GetInt64() : 0;
                bool hasPw = e.TryGetProperty("hasPassword", out var hp) && hp.GetBoolean();
                uint modCnt = e.TryGetProperty("modCount", out var mcd) ? mcd.GetUInt32() : 0;
                string modLst = Str(e, "modList");
                list.Add(new LobbyInfo(id, nm, st, mc, mx, hasPw, modCnt, modLst));
            }
        }
        catch (Exception ex)
        {
            Log?.Invoke("lobbies parse error: " + ex.Message);
            return;
        }
        LobbiesUpdated?.Invoke(list);
    }

    // The daemon writes pulsar_relays.json while serving (Phase 2 backend). We read it
    // on every poll - the file can also linger from a previous serve session, so the
    // Servers page can show the last-known list even before the daemon is started.
    private void RefreshRelays()
    {
        string json;
        try { json = File.Exists(_relaysPath) ? File.ReadAllText(_relaysPath) : "[]"; }
        catch { return; }
        if (json == _lastRelaysJson) return;
        _lastRelaysJson = json;

        var list = new List<RelayInfo>();
        try
        {
            using var doc = JsonDocument.Parse(json);
            foreach (var e in doc.RootElement.EnumerateArray())
            {
                string nm = Str(e, "name");
                string rg = Str(e, "region");
                string host = Str(e, "publicHost");
                bool verified = e.TryGetProperty("verified", out var vf) && vf.GetBoolean();
                int sessions = e.TryGetProperty("sessions", out var ss) ? ss.GetInt32() : 0;
                int softCap  = e.TryGetProperty("softCap", out var scp) ? scp.GetInt32() : 0;
                int loadPct  = e.TryGetProperty("loadPct", out var lp) ? lp.GetInt32() : 0;
                // Absent (older relay) → treat as accepting so it still appears/usable.
                bool accepting = !e.TryGetProperty("accepting", out var ac) || ac.GetBoolean();
                long seen = e.TryGetProperty("lastSeenUnixMs", out var ls) ? ls.GetInt64() : 0;
                list.Add(new RelayInfo(nm, rg, host, verified, sessions, softCap, loadPct, accepting, seen));
            }
        }
        catch (Exception ex)
        {
            Log?.Invoke("relays parse error: " + ex.Message);
            return;
        }
        RelaysUpdated?.Invoke(list);
    }

    private void CheckAck()
    {
        if (!File.Exists(_ackPath)) return;
        string text;
        try { text = File.ReadAllText(_ackPath); } catch { return; }
        try
        {
            using var doc = JsonDocument.Parse(text);
            var root = doc.RootElement;
            string cmdId = Str(root, "cmdId");
            if (cmdId.Length == 0 || cmdId == _lastAckCmdId) return;
            _lastAckCmdId = cmdId;
            if (cmdId != _pendingCmdId) return;

            bool ok = root.TryGetProperty("ok", out var o) && o.GetBoolean();
            string err = Str(root, "error");
            string code = Str(root, "code");
            string kind = _pendingKind;
            bool launchAfter = _launchAfterPending;
            _pendingCmdId = "";
            _pendingKind = "";
            _launchAfterPending = false;

            switch (kind)
            {
                case "register":
                    RegisterAck?.Invoke(ok, err);
                    break;
                case "close":
                    CloseAck?.Invoke(ok, err);
                    break;
                case "resetProfile":
                    ResetAck?.Invoke(ok, err);
                    break;
                case "host":
                    HostAck?.Invoke(ok, err, code);
                    if (ok && launchAfter) LaunchRequested?.Invoke();
                    break;
                case "hostDirect":
                    DirectHostAck?.Invoke(ok, err);
                    if (ok && launchAfter) LaunchRequested?.Invoke();
                    break;
                case "join":
                case "joinCode":
                case "joinDirect":
                    JoinAck?.Invoke(ok, err, kind);
                    if (ok && launchAfter) LaunchRequested?.Invoke();
                    break;
            }
        }
        catch { /* partial write; retry next poll */ }
    }

    // ── helpers ─────────────────────────────────────────────────────────────────
    public static string HashPw(string pw)
    {
        pw = (pw ?? "").Trim();
        if (pw.Length == 0) return "";
        using var sha = SHA256.Create();
        return Convert.ToHexString(sha.ComputeHash(Encoding.UTF8.GetBytes(PwSalt + pw)));
    }

    private static string HostOnly(string s)
    {
        var h = s;
        int i = h.IndexOf("://", StringComparison.Ordinal);
        if (i >= 0) h = h.Substring(i + 3);
        int slash = h.IndexOf('/');
        if (slash >= 0) h = h.Substring(0, slash);
        int colon = h.IndexOf(':');
        if (colon >= 0) h = h.Substring(0, colon);
        return h;
    }

    private static string Str(JsonElement e, string p) =>
        e.TryGetProperty(p, out var v) && v.ValueKind == JsonValueKind.String ? (v.GetString() ?? "") : "";

    private static string Esc(string s) =>
        "\"" + (s ?? "").Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";

    private static string NewId(out string id)
    {
        id = Guid.NewGuid().ToString("N");
        return id;
    }

    // Locate PulsarCompanion.exe: AppContext.BaseDirectory, then walk up <=8 parents
    // for companion\bin\{Release,Debug}\net8.0\PulsarCompanion.exe.
    public static string GuessCompanionExe()
    {
        var baseDir = AppContext.BaseDirectory;
        foreach (var rel in new[] { "PulsarCompanion.exe", Path.Combine("companion", "PulsarCompanion.exe") })
        {
            var c = Path.Combine(baseDir, rel);
            if (File.Exists(c)) return c;
        }
        var dir = baseDir;
        for (int i = 0; i < 8 && dir != null; i++)
        {
            foreach (var cfg in new[] { "Release", "Debug" })
            {
                var c = Path.Combine(dir, "companion", "bin", cfg, "net8.0", "PulsarCompanion.exe");
                if (File.Exists(c)) return c;
            }
            dir = Directory.GetParent(dir)?.FullName;
        }
        return "PulsarCompanion.exe";
    }

    public static string GuessStarfieldExe()
    {
        var found = GameLocator.FindStarfield();
        if (!string.IsNullOrEmpty(found) && File.Exists(found)) return found;

        foreach (var p in new[]
        {
            @"<Starfield>\sfse_loader.exe",
            @"C:\Program Files (x86)\Steam\steamapps\common\Starfield\sfse_loader.exe",
            @"C:\Program Files (x86)\Steam\steamapps\common\Starfield\Starfield.exe",
        })
            if (File.Exists(p)) return p;
        return @"<Starfield>\sfse_loader.exe";
    }

    public void Dispose() => Stop();
}
