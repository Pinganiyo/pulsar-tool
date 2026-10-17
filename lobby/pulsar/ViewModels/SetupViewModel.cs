using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Globalization;
using System.IO;
using System.Linq;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Pulsar.Services;

namespace Pulsar.ViewModels;

// The "Setup" install-check page. On open (and after any fix) it runs a list of
// checks against the configured Starfield install + the user's StarfieldCustom.ini
// and offers one-click fixes for the ini settings PULSAR needs. Built for
// non-technical Beta testers: reassuring banner, plain-English details, big Fix
// buttons.
public partial class SetupViewModel : ViewModelBase
{
    private readonly MainWindowViewModel _shell;

    // Stable keys so the view-model can find a row regardless of display order.
    private const string KStarfieldPath = "starfield-path";
    private const string KSfse = "sfse";
    private const string KAddrLib = "addrlib";
    private const string KDll = "dll";
    private const string KEsp = "esp";
    private const string KScripts = "scripts";
    private const string KLooseFiles = "loose-files";
    private const string KAltTab = "alt-tab";
    private const string KForceLoad = "force-load";

    // Map each row VM to its stable key so Fix knows which fixer to run.
    private readonly Dictionary<SetupCheckViewModel, string> _keys = new();

    // Backup is taken at most once per app session (and never overwrites an existing
    // .bak). This guards the "first modification in a session" rule.
    private bool _backupTakenThisSession;

    public SetupViewModel(MainWindowViewModel shell)
    {
        _shell = shell;

        Checks.Add(Make(KStarfieldPath, "Starfield path"));
        Checks.Add(Make(KSfse, "SFSE installed"));
        Checks.Add(Make(KAddrLib, "Address Library"));
        Checks.Add(Make(KDll, "PULSAR mod (DLL)"));
        Checks.Add(Make(KEsp, "PULSAR plugin (ESP)"));
        Checks.Add(Make(KScripts, "Co-op scripts"));
        Checks.Add(Make(KLooseFiles, "Loose files enabled"));
        Checks.Add(Make(KAltTab, "Co-op alt-tab"));
        Checks.Add(Make(KForceLoad, "Plugin enabled (Plugins.txt)"));
    }

    public override string Title => "Setup";
    public override string Glyph => char.ConvertFromUtf32(0xE9D5); // Checklist

    public ObservableCollection<SetupCheckViewModel> Checks { get; } = new();

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(BannerIsGood))]
    private string _bannerText = "";

    // Drives the banner color (green when all good, amber when issues remain).
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(BannerIsGood))]
    private bool _allGood;

    public bool BannerIsGood => AllGood;

    // The resolved StarfieldCustom.ini path, shown in the status line so users can
    // see exactly which file the fixes touch.
    [ObservableProperty] private string _iniPathDisplay = "";

    // Any fix available across all rows (drives the "Fix all" button enabled state).
    [ObservableProperty] private bool _anyFixable;

    private SetupCheckViewModel Make(string key, string name)
    {
        var vm = new SetupCheckViewModel(this, name);
        _keys[vm] = key;
        return vm;
    }

    private SetupCheckViewModel Row(string key) => _keys.First(kv => kv.Value == key).Key;

    // ── path helpers ────────────────────────────────────────────────────────────

    private string StarfieldExe => _shell.Config.StarfieldPath?.Trim() ?? "";

    private string? GameRoot
    {
        get
        {
            var exe = StarfieldExe;
            if (string.IsNullOrEmpty(exe)) return null;
            try { return Path.GetDirectoryName(exe); }
            catch { return null; }
        }
    }

    private string? GameData
    {
        get
        {
            var root = GameRoot;
            return string.IsNullOrEmpty(root) ? null : Path.Combine(root, "Data");
        }
    }

    // %USERPROFILE%\Documents\My Games\Starfield\StarfieldCustom.ini (OneDrive-aware).
    // Single source of truth lives in PluginRegistration so the updater resolves the same path.
    public static string StarfieldCustomIniPath => PluginRegistration.StarfieldCustomIniPath;

    // Status line for the one-click install action.
    [ObservableProperty] private string _installStatus = "";

    // ── commands ────────────────────────────────────────────────────────────────

    [RelayCommand]
    public void Recheck() => RunChecks();

    // One-click: copy the mod payload that ships next to the tool into the game, then
    // apply the required ini settings and re-check. Makes the whole install a single button.
    [RelayCommand]
    private void InstallMod()
    {
        if (!StarfieldPathOk())
        {
            InstallStatus = "Set the Starfield path first (Co-op page or Settings).";
            return;
        }
        if (System.Diagnostics.Process.GetProcessesByName("Starfield").Length > 0 ||
            System.Diagnostics.Process.GetProcessesByName("sfse_loader").Length > 0)
        {
            InstallStatus = "Close Starfield first, then install.";
            return;
        }
        // SFSE (sfse_loader.exe) is required at runtime to inject the DLL into the game process.
        // We allow installing the mod files regardless so the game Data folder is prepared;
        // the SFSE check row below still flags it as missing.
        var src = ModInstaller.FindModDataDir();
        if (src == null)
        {
            InstallStatus = "Mod files not found next to the tool. Extract the whole PULSAR package so the 'mod' folder sits next to Pulsar.exe, or run the tool from inside a PULSAR source checkout.";
            return;
        }
        var res = ModInstaller.Install(src, GameData!);
        _shell.Log("Setup: install mod -> " + res.Message);
        if (res.Ok)
        {
            FixAll();   // applies loose-files / alt-tab / force-load ini settings + re-checks (sets AllGood)
            InstallStatus = AllGood
                ? res.Message + " Game settings applied. You are ready to play."
                : res.Message + " Settings applied - check the remaining items below (e.g. SFSE, Address Library).";
        }
        else
        {
            InstallStatus = res.Message;
            RunChecks();
        }
    }

    [RelayCommand]
    private void FixAll()
    {
        // Apply every fixable row, then re-check once. We re-read keys each pass
        // because applying force-load depends on the current ini contents.
        if (!StarfieldPathOk()) return;
        try
        {
            EnsureBackup();
            var doc = LoadIni();
            bool changed = false;
            foreach (var row in Checks)
            {
                if (!row.CanFix) continue;
                changed |= ApplyFix(_keys[row], doc);
            }
            if (changed) SaveIni(doc);
        }
        catch (Exception ex) { _shell.Log("Setup: fix all failed - " + ex.Message); }
        RunChecks();
    }

    // Called by a row's Fix button.
    public void FixOne(SetupCheckViewModel row)
    {
        if (!StarfieldPathOk()) return;
        if (!_keys.TryGetValue(row, out var key)) return;
        try
        {
            EnsureBackup();
            var doc = LoadIni();
            if (ApplyFix(key, doc)) SaveIni(doc);
        }
        catch (Exception ex) { _shell.Log("Setup: fix failed - " + ex.Message); }
        RunChecks();
    }

    // ── ini load/save + backup ──────────────────────────────────────────────────

    private static IniDocument LoadIni() => IniDocument.Load(StarfieldCustomIniPath);

    private static void SaveIni(IniDocument doc) => doc.Save(StarfieldCustomIniPath);

    // Copy StarfieldCustom.ini → StarfieldCustom.ini.pulsar.bak once per session, and
    // never clobber an existing .bak. No-op if the ini doesn't exist yet (the fix
    // will create a fresh one - nothing to back up).
    private void EnsureBackup()
    {
        if (_backupTakenThisSession) return;
        _backupTakenThisSession = true;
        try
        {
            var ini = StarfieldCustomIniPath;
            if (!File.Exists(ini)) return;
            var bak = ini + ".pulsar.bak";
            if (File.Exists(bak)) return;
            File.Copy(ini, bak);
            _shell.Log("Setup: backed up StarfieldCustom.ini → " + Path.GetFileName(bak));
        }
        catch (Exception ex) { _shell.Log("Setup: backup failed - " + ex.Message); }
    }

    // ── fixers (return true if they changed the document) ───────────────────────

    private bool ApplyFix(string key, IniDocument doc) => key switch
    {
        KLooseFiles => FixLooseFiles(doc),
        KAltTab => FixAltTab(doc),
        KForceLoad => FixForceLoad(doc),
        _ => false // report-only checks have no fix
    };

    private static bool FixLooseFiles(IniDocument doc)
    {
        doc.SetValue("Archive", "bInvalidateOlderFiles", "1");
        // sResourceDataDirsFinal must be PRESENT with an EMPTY value.
        doc.SetValue("Archive", "sResourceDataDirsFinal", "");
        return true;
    }

    private static bool FixAltTab(IniDocument doc)
    {
        doc.SetValue("General", "bPauseOnLostFocus", "0");
        doc.SetValue("General", "bAlwaysActive", "1");
        return true;
    }

    // Enable PULSAR via Plugins.txt (coexistence-friendly) instead of sTestFile. sTestFile
    // makes Starfield ignore Plugins.txt entirely, so the player's other mods stop loading;
    // listing *PULSAR.esm in Plugins.txt loads PULSAR alongside them. The fix also removes
    // any of OUR sTestFile entries (current PULSAR.esm or legacy PULSAR.esp) so they no longer
    // bypass Plugins.txt. A foreign mod's sTestFile entry, if any, is left to the user (the
    // check below surfaces it as a warning).
    // Returns true if the INI was changed (drives SaveIni); the Plugins.txt write is a side
    // effect that always runs and persists itself.
    // Switch PULSAR to the Plugins.txt load mechanism: drop our sTestFile entries (so they no
    // longer bypass Plugins.txt) and enable *PULSAR.esm. The sTestFile removal runs on the
    // shared doc (FixAll/FixOne save it once); Plugins.txt is written immediately by Enable().
    private bool FixForceLoad(IniDocument doc)
    {
        bool iniChanged = PluginRegistration.RemovePulsarTestFiles(doc);
        if (!PluginRegistration.Enable())
            _shell.Log("Setup: could not write Plugins.txt (close Starfield / check permissions).");
        return iniChanged;
    }

    // True if some sTestFileN names PULSAR's plugin (current or legacy).
    private static bool PulsarTestFilePresent(IniDocument doc)
    {
        for (int n = 1; n <= 10; n++)
            if (PluginRegistration.IsPulsarPlugin(doc.GetValue("General", "sTestFile" + n.ToString(CultureInfo.InvariantCulture))))
                return true;
        return false;
    }

    // True if some sTestFileN names a NON-PULSAR plugin. These also make Starfield ignore
    // Plugins.txt, but they belong to another mod's setup so we never touch them - we warn.
    private static bool ForeignTestFilePresent(IniDocument doc)
    {
        for (int n = 1; n <= 10; n++)
        {
            var v = doc.GetValue("General", "sTestFile" + n.ToString(CultureInfo.InvariantCulture));
            if (v != null && v.Trim().Length > 0 && !PluginRegistration.IsPulsarPlugin(v)) return true;
        }
        return false;
    }

    // ── checks ──────────────────────────────────────────────────────────────────

    private bool StarfieldPathOk() => !string.IsNullOrEmpty(StarfieldExe) && File.Exists(StarfieldExe);

    public void RunChecks()
    {
        IniPathDisplay = "StarfieldCustom.ini: " + StarfieldCustomIniPath;

        bool pathOk = StarfieldPathOk();

        // 1) Starfield path - gates everything else.
        var pathRow = Row(KStarfieldPath);
        if (pathOk)
        {
            pathRow.Status = CheckStatus.Ok;
            pathRow.Detail = StarfieldExe;
        }
        else
        {
            pathRow.Status = CheckStatus.Fail;
            pathRow.Detail = string.IsNullOrEmpty(StarfieldExe)
                ? "Not set - open Settings/Play and pick sfse_loader.exe or Starfield.exe."
                : "File not found: " + StarfieldExe;
        }
        pathRow.CanFix = false;

        if (!pathOk)
        {
            // Block all downstream checks with a clear, identical message.
            const string blocked = "blocked - set the Starfield path in Settings";
            foreach (var row in Checks)
            {
                if (row == pathRow) continue;
                row.Status = CheckStatus.Warn;
                row.Detail = blocked;
                row.CanFix = false;
            }
            FinishBanner();
            return;
        }

        var root = GameRoot!;
        var data = GameData!;

        // 2) SFSE installed (report-only). Required, but we don't ship it (licence: only the
        // SFSE plugin API header is MIT; the binary may not be re-hosted). Link the source.
        var sfseExe = Path.Combine(root, "sfse_loader.exe");
        ReportFile(Row(KSfse), sfseExe,
            okDetail: "sfse_loader.exe found in game folder.",
            failDetail: "SFSE not found - PULSAR needs it. Get it (matching your game version), then re-check.");
        var sfseRow = Row(KSfse);
        sfseRow.LinkUrl = "https://sfse.silverlock.org/";
        sfseRow.LinkText = "Get SFSE";

        // 2b) Address Library (versionlib-*.bin) - REQUIRED: PULSAR resolves engine
        // addresses through it; without it the mod won't load. Report-only (we don't
        // redistribute it via this check).
        var addrRow = Row(KAddrLib);
        var pluginsDir = Path.Combine(data, "SFSE", "Plugins");
        bool hasAddrLib = false;
        try { hasAddrLib = Directory.Exists(pluginsDir) && Directory.EnumerateFiles(pluginsDir, "versionlib-*.bin").Any(); }
        catch { hasAddrLib = false; }
        addrRow.Status = hasAddrLib ? CheckStatus.Ok : CheckStatus.Fail;
        addrRow.Detail = hasAddrLib
            ? "Address Library (versionlib) found in Data\\SFSE\\Plugins."
            : "Address Library MISSING - PULSAR can't load without it. Get the 'all-in-one' (versionlib-*.bin) for your game version, then re-check.";
        addrRow.CanFix = false;
        addrRow.LinkUrl = "https://www.nexusmods.com/starfield/mods/3256";
        addrRow.LinkText = "Get Address Library";

        // 3) PULSAR mod DLL (report-only).
        var dll = Path.Combine(data, "SFSE", "Plugins", "PULSAR.dll");
        ReportFile(Row(KDll), dll,
            okDetail: "Data\\SFSE\\Plugins\\PULSAR.dll found.",
            failDetail: "PULSAR.dll missing - copy it to Data\\SFSE\\Plugins.");

        // 4) PULSAR plugin - PULSAR.esp (master-flagged, report-only).
        var esp = Path.Combine(data, "PULSAR.esp");
        ReportFile(Row(KEsp), esp,
            okDetail: "Data\\PULSAR.esp found.",
            failDetail: "PULSAR.esp missing - copy it to the Data folder (or click \"Install mod\").");

        // 5) Co-op scripts (report-only).
        var pex = Path.Combine(data, "Scripts", "PulsarRemoteControl.pex");
        ReportFile(Row(KScripts), pex,
            okDetail: "Data\\Scripts\\PulsarRemoteControl.pex found.",
            failDetail: "Co-op scripts missing - copy the Scripts folder into Data.");

        // 6/7/8) ini-driven checks. Read once.
        IniDocument doc;
        try { doc = LoadIni(); }
        catch { doc = IniDocument.Parse(null); }

        // 6) Loose files enabled (fixable).
        var looseRow = Row(KLooseFiles);
        bool invalidate = string.Equals(doc.GetValue("Archive", "bInvalidateOlderFiles")?.Trim(), "1", StringComparison.Ordinal);
        bool dirsFinalPresent = doc.GetValue("Archive", "sResourceDataDirsFinal") != null;
        if (invalidate && dirsFinalPresent)
        {
            looseRow.Status = CheckStatus.Ok;
            looseRow.Detail = "[Archive] bInvalidateOlderFiles=1, sResourceDataDirsFinal present - loose files load.";
            looseRow.CanFix = false;
        }
        else
        {
            looseRow.Status = CheckStatus.Fail;
            looseRow.Detail = "Loose files NOT enabled - PULSAR scripts/plugin won't load. Needs [Archive] bInvalidateOlderFiles=1 + sResourceDataDirsFinal=.";
            looseRow.CanFix = true;
        }

        // 7) Co-op alt-tab (fixable).
        var altRow = Row(KAltTab);
        bool noPause = string.Equals(doc.GetValue("General", "bPauseOnLostFocus")?.Trim(), "0", StringComparison.Ordinal);
        bool alwaysActive = string.Equals(doc.GetValue("General", "bAlwaysActive")?.Trim(), "1", StringComparison.Ordinal);
        if (noPause && alwaysActive)
        {
            altRow.Status = CheckStatus.Ok;
            altRow.Detail = "[General] bPauseOnLostFocus=0, bAlwaysActive=1 - game keeps running when alt-tabbed.";
            altRow.CanFix = false;
        }
        else
        {
            altRow.Status = CheckStatus.Fail;
            altRow.Detail = "Game pauses when it loses focus - in co-op this freezes you for your partner. Needs [General] bPauseOnLostFocus=0 + bAlwaysActive=1.";
            altRow.CanFix = true;
        }

        // 8) Plugin enabled in Plugins.txt (fixable). This is the coexistence-friendly load
        // mechanism: *PULSAR.esm in Plugins.txt loads PULSAR alongside the player's other mods.
        // The old sTestFile mechanism made Starfield ignore Plugins.txt, so other mods didn't load.
        var forceRow = Row(KForceLoad);
        bool enabled = PluginRegistration.IsEnabled();
        bool pulsarTestFile = PulsarTestFilePresent(doc);   // legacy: still loaded via our sTestFile
        bool foreignTestFile = ForeignTestFilePresent(doc); // another mod's sTestFile blocks Plugins.txt

        if (enabled && !pulsarTestFile && !foreignTestFile)
        {
            forceRow.Status = CheckStatus.Ok;
            forceRow.Detail = "*PULSAR.esp is enabled in Plugins.txt - it loads alongside your other mods.";
            forceRow.CanFix = false;
        }
        else if (foreignTestFile)
        {
            // Can't fully auto-fix: a non-PULSAR sTestFileN makes the game ignore Plugins.txt.
            // We can still enable PULSAR + drop our own sTestFile, but the foreign one is the user's.
            forceRow.Status = CheckStatus.Warn;
            forceRow.Detail = "Another mod's sTestFileN entry in StarfieldCustom.ini makes Starfield ignore Plugins.txt, so plugins listed there (including PULSAR) won't load. Remove that non-PULSAR sTestFile line, or load that mod via Plugins.txt too.";
            forceRow.CanFix = !enabled || pulsarTestFile;
        }
        else
        {
            forceRow.Status = CheckStatus.Fail;
            forceRow.Detail = pulsarTestFile
                ? "PULSAR still loads via sTestFile, which makes Starfield ignore Plugins.txt (your other mods won't load). Fix switches PULSAR to Plugins.txt (*PULSAR.esp) and removes the sTestFile entry."
                : "PULSAR is not enabled in Plugins.txt. Fix adds *PULSAR.esp so Starfield loads it (alongside your other mods).";
            forceRow.CanFix = true;
        }

        FinishBanner();
    }

    private static void ReportFile(SetupCheckViewModel row, string path, string okDetail, string failDetail)
    {
        bool exists = File.Exists(path);
        row.Status = exists ? CheckStatus.Ok : CheckStatus.Fail;
        row.Detail = exists ? okDetail : failDetail;
        row.CanFix = false; // report-only
    }

    private void FinishBanner()
    {
        int issues = Checks.Count(c => c.Status != CheckStatus.Ok);
        AnyFixable = Checks.Any(c => c.CanFix);
        AllGood = issues == 0;
        BannerText = AllGood
            ? "✓ All set - you're ready to host/join"
            : issues + (issues == 1 ? " issue - click Fix" : " issues - click Fix");
    }
}
