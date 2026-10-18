using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Pulsar.Services;

public enum DbMode
{
    Online,
    Local
}

// Persisted GUI settings. Stored SEPARATELY from the WinForms lobbygui.json so the
// two front-ends never clash: %LOCALAPPDATA%\PULSAR\pulsar_gui.json.
public sealed class AppConfig
{
    public string PlayerName { get; set; } = "Player";

    // Official SpacetimeDB endpoint. No address is compiled in; the built-in default,
    // if any, comes from OfficialEndpoint (env var or side-car file). ONLY a user
    // override is persisted: empty string = "use whatever the build provides".
    public string OfficialStdbUriOverride { get; set; } = "";

    // Effective official endpoint: the user's override if set, else the build's
    // default, which may legitimately be empty on a self-host-only build.
    [JsonIgnore]
    public string OfficialStdbUri
    {
        get => string.IsNullOrWhiteSpace(OfficialStdbUriOverride)
            ? OfficialEndpoint.Uri
            : OfficialStdbUriOverride.Trim();
        // Store an override only when it actually differs from the build default, so the
        // common "user didn't change it" case leaves the json clean.
        set => OfficialStdbUriOverride =
            (string.IsNullOrWhiteSpace(value) || value.Trim() == OfficialEndpoint.Uri)
                ? ""
                : value.Trim();
    }

    public DbMode DbMode { get; set; } = DbMode.Online;

    public string StarfieldPath { get; set; } = "";

    public bool AutoLaunch { get; set; } = true;
    public bool AutoSave { get; set; } = true;

    // Advanced mode reveals the relay override field.
    public bool Advanced { get; set; } = false;
    public string RelayOverride { get; set; } = "";

    // SECURITY (F4): the relay master-list shows ONLY verified relays unless this is
    // enabled. Community/unverified relays can observe relayed traffic until E2E lands.
    public bool ShowCommunityRelays { get; set; } = false;

    // Local STDB endpoint (Phase 4 one-click local stack points here).
    public string LocalStdbUri { get; set; } = "ws://127.0.0.1:3000";

    // Path to the spacetime CLI used by the one-click local stack (Phase 4). Default
    // is the standard SpacetimeDB install location under %LOCALAPPDATA%.
    public string SpacetimeExePath { get; set; } =
        Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "SpacetimeDB", "spacetime.exe");

    // GitHub repos (owner/name) checked for updates on startup (Phase 5). Tool and
    // mod ship as SEPARATE artifacts in SEPARATE repos: independent versions,
    // independent patch notes. The tool can only NOTIFY (a running exe can't replace
    // itself) -> "open release page"; the mod auto-updates from its release .zip.
    // These are the real release repos; they are still PRIVATE for now (the update
    // check returns nothing / 404 until they are flipped public + a release is cut,
    // which the checker handles silently). Overridable via Settings.
    // PULSAR Revived publishes tool and mod from the fork's single repo (the
    // original repos are unmaintained). The mod updater takes the release's first
    // .zip; both packages contain mod/Data.
    public string ToolRepo { get; set; } = "Pinganiyo/pulsar-tool";
    public string ModRepo  { get; set; } = "Pinganiyo/pulsar-tool";

    // Legacy single-repo setting (pre tool/mod split). Kept ONLY so old configs still
    // deserialize; Load() migrates a non-empty value into ToolRepo, then clears it.
    public string UpdateRepo { get; set; } = "";

    public List<string> RecentServers { get; set; } = new();

    [JsonIgnore]
    public static string SharedDir =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PULSAR");

    [JsonIgnore]
    public static string ConfigPath => Path.Combine(SharedDir, "pulsar_gui.json");

    private static readonly JsonSerializerOptions s_jsonOptions = new()
    {
        WriteIndented = true,
        Converters = { new JsonStringEnumConverter() }
    };

    public static AppConfig Load()
    {
        try
        {
            Directory.CreateDirectory(SharedDir);
            if (File.Exists(ConfigPath))
            {
                var cfg = JsonSerializer.Deserialize<AppConfig>(File.ReadAllText(ConfigPath), s_jsonOptions) ?? new AppConfig();
                bool migrated = false;
                // Migrate the old single UpdateRepo into the tool channel (one-time).
                if (!string.IsNullOrWhiteSpace(cfg.UpdateRepo))
                {
                    cfg.ToolRepo = cfg.UpdateRepo.Trim();
                    cfg.UpdateRepo = "";
                    migrated = true;
                }
                // Heal stale placeholder repos saved by pre-release dev builds (e.g.
                // "PulsarDev/...") so the update check targets the REAL repos instead of a
                // dead one. Reset any such value to the current built-in default.
                var def = new AppConfig();
                if ((cfg.ToolRepo ?? "").StartsWith("PulsarDev/", StringComparison.OrdinalIgnoreCase))
                {
                    cfg.ToolRepo = def.ToolRepo;
                    migrated = true;
                }
                if ((cfg.ModRepo ?? "").StartsWith("PulsarDev/", StringComparison.OrdinalIgnoreCase))
                {
                    cfg.ModRepo = def.ModRepo;
                    migrated = true;
                }
                if (migrated) cfg.Save();
                return cfg;
            }
        }
        catch { /* fall through to defaults */ }
        return new AppConfig();
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(SharedDir);
            File.WriteAllText(ConfigPath, JsonSerializer.Serialize(this, s_jsonOptions));
        }
        catch { /* best-effort */ }
    }

    // Resolve the STDB URI for the active DB mode, defaulting the scheme to wss://.
    public string ResolveStdbUri()
    {
        var uri = (DbMode == DbMode.Local ? LocalStdbUri : OfficialStdbUri).Trim();

        // No official endpoint configured anywhere: fall back to the local stack
        // rather than to somebody else's server. Self-host is the default.
        if (uri.Length == 0)
            uri = LocalStdbUri.Trim();

        if (uri.Length == 0)
            return "";
        if (!uri.Contains("://"))
            uri = "wss://" + uri;
        return uri;
    }
}
