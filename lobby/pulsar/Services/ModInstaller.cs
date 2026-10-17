using System;
using System.IO;

namespace Pulsar.Services;

// One-click install of the PULSAR mod payload that ships next to the tool. In the released
// package the layout is  <exeDir>/mod/Data/...  (staged by tools/package_release.ps1, the
// same tree uploaded to the pulsar-releases GitHub release). Install = copy that Data tree
// into <Starfield>/Data, preserving structure (DLL, ESP plugin, Scripts, and anything we add later).
public static class ModInstaller
{
    public sealed record InstallResult(bool Ok, int Copied, string Message);

    // Locate the staged mod payload's Data dir next to the REAL exe. Use Environment.ProcessPath
    // (not AppContext.BaseDirectory, which for a single-file publish points at a temp extract).
    public static string? FindModDataDir()
    {
        string? exe = Environment.ProcessPath;
        var exeDir = string.IsNullOrEmpty(exe) ? AppContext.BaseDirectory : Path.GetDirectoryName(exe);
        if (string.IsNullOrEmpty(exeDir)) return null;

        string[] candidates =
        {
            Path.Combine(exeDir, "mod", "Data"),     // released package layout
            Path.Combine(exeDir, "Data"),            // flattened payload
            Path.Combine(exeDir, "..", "mod", "Data"),
            Path.Combine(exeDir, "..", "..", "..", "..", "data"), // repo data dir
            Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "data"),
            Path.Combine(AppContext.BaseDirectory, "mod", "Data"),
        };
        foreach (var c in candidates)
        {
            try { if (Directory.Exists(c) && HasModFiles(c)) return Path.GetFullPath(c); }
            catch { /* ignore malformed candidate */ }
        }

        // Walk up from exeDir searching for a data folder containing PULSAR.esp
        try
        {
            var cur = new DirectoryInfo(exeDir);
            for (int i = 0; i < 5 && cur != null; i++, cur = cur.Parent)
            {
                var candidate = Path.Combine(cur.FullName, "data");
                if (Directory.Exists(candidate) && HasModFiles(candidate))
                    return Path.GetFullPath(candidate);
            }
        }
        catch { }

        return null;
    }

    // Sanity gate so we never copy some unrelated Data folder: the payload must carry our DLL or plugin.
    private static bool HasModFiles(string dataDir)
    {
        try
        {
            return File.Exists(Path.Combine(dataDir, "SFSE", "Plugins", "PULSAR.dll"))
                || File.Exists(Path.Combine(dataDir, "PULSAR.esp"));
        }
        catch { return false; }
    }

    public static InstallResult Install(string sourceDataDir, string destDataDir)
    {
        try
        {
            if (!Directory.Exists(sourceDataDir))
                return new InstallResult(false, 0, "Mod files not found next to the tool.");

            // Guard against source == game Data (tool placed inside the Starfield folder):
            // copying a file onto itself would just error per-file.
            if (string.Equals(
                    Path.GetFullPath(sourceDataDir).TrimEnd('\\', '/'),
                    Path.GetFullPath(destDataDir).TrimEnd('\\', '/'),
                    StringComparison.OrdinalIgnoreCase))
                return new InstallResult(false, 0, "The tool is inside the game's Data folder - move it elsewhere to install.");

            int copied = 0;
            foreach (var src in Directory.EnumerateFiles(sourceDataDir, "*", SearchOption.AllDirectories))
            {
                var rel = Path.GetRelativePath(sourceDataDir, src);
                var dst = Path.Combine(destDataDir, rel);
                var dstDir = Path.GetDirectoryName(dst);
                if (!string.IsNullOrEmpty(dstDir)) Directory.CreateDirectory(dstDir);
                File.Copy(src, dst, overwrite: true);
                copied++;
            }

            // Hygiene: PULSAR ships as the master-flagged PULSAR.esp (same filename as 0.1.0). If a
            // stray PULSAR.esm is present (e.g. from an .esm test build), drop it once the .esp is in
            // place so the Data folder holds exactly one PULSAR plugin.
            try
            {
                var esp = Path.Combine(destDataDir, "PULSAR.esp");
                var strayEsm = Path.Combine(destDataDir, "PULSAR.esm");
                if (File.Exists(esp) && File.Exists(strayEsm)) File.Delete(strayEsm);
            }
            catch { /* non-fatal cleanup */ }

            return new InstallResult(true, copied, $"Installed {copied} mod file(s) into the game.");
        }
        catch (UnauthorizedAccessException)
        {
            return new InstallResult(false, 0,
                "No permission to write to the game folder. Close Starfield, or run the tool as administrator.");
        }
        catch (IOException ex)
        {
            return new InstallResult(false, 0,
                "Could not copy (is Starfield running?): " + ex.Message);
        }
        catch (Exception ex)
        {
            return new InstallResult(false, 0, "Install failed: " + ex.Message);
        }
    }
}
