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
        var exeDir = ExeDir();
        if (string.IsNullOrEmpty(exeDir)) return null;

        string[] candidates =
        {
            Path.Combine(exeDir, "mod", "Data"),     // released package layout
            Path.Combine(exeDir, "Data"),            // flattened payload
            Path.Combine(exeDir, "..", "mod", "Data"),
        };
        foreach (var c in candidates)
        {
            try { if (Directory.Exists(c) && HasModFiles(c)) return Path.GetFullPath(c); }
            catch { /* ignore malformed candidate */ }
        }

        // Dev checkout: the tool runs from lobby/pulsar/bin/<cfg>/<tfm>/ and the payload is
        // the repository's data/ folder (no DLL there - see FindDevBuildDll).
        var repo = FindRepoRoot(exeDir);
        return repo == null ? null : Path.Combine(repo, "data");
    }

    // Walk up from the exe looking for the repository root: a folder holding both
    // CMakeLists.txt and data/PULSAR.esp. Null outside a dev checkout.
    public static string? FindRepoRoot(string? startDir = null)
    {
        try
        {
            var cur = new DirectoryInfo(startDir ?? ExeDir() ?? "");
            for (int i = 0; i < 10 && cur != null; i++, cur = cur.Parent)
            {
                if (File.Exists(Path.Combine(cur.FullName, "CMakeLists.txt"))
                    && File.Exists(Path.Combine(cur.FullName, "data", "PULSAR.esp")))
                    return cur.FullName;
            }
        }
        catch { /* unreadable parent - treat as not a dev checkout */ }
        return null;
    }

    // Dev checkout only: the newest PULSAR.dll built under build/<preset>/<config>/.
    public static string? FindDevBuildDll(string repoRoot)
    {
        try
        {
            var buildDir = Path.Combine(repoRoot, "build");
            if (!Directory.Exists(buildDir)) return null;
            FileInfo? newest = null;
            foreach (var f in Directory.EnumerateFiles(buildDir, "PULSAR.dll", SearchOption.AllDirectories))
            {
                var fi = new FileInfo(f);
                if (newest == null || fi.LastWriteTimeUtc > newest.LastWriteTimeUtc) newest = fi;
            }
            return newest?.FullName;
        }
        catch { return null; }
    }

    private static string? ExeDir()
    {
        string? exe = Environment.ProcessPath;
        return string.IsNullOrEmpty(exe) ? AppContext.BaseDirectory : Path.GetDirectoryName(exe);
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

            // Installing straight from a dev checkout's data/ folder: skip its README and add
            // the locally built DLL (+PDB for readable crash traces), which data/ does not hold.
            string? repo = FindRepoRoot(sourceDataDir);
            bool fromRepo = repo != null && string.Equals(
                Path.GetFullPath(Path.Combine(repo, "data")).TrimEnd('\\', '/'),
                Path.GetFullPath(sourceDataDir).TrimEnd('\\', '/'),
                StringComparison.OrdinalIgnoreCase);
            string? devDll = fromRepo ? FindDevBuildDll(repo!) : null;
            if (fromRepo && devDll == null)
                return new InstallResult(false, 0,
                    "Dev checkout found, but no built PULSAR.dll under build/. Build the plugin first (see BUILD.md).");

            int copied = 0;
            foreach (var src in Directory.EnumerateFiles(sourceDataDir, "*", SearchOption.AllDirectories))
            {
                if (fromRepo && src.EndsWith(".md", StringComparison.OrdinalIgnoreCase)) continue;
                var rel = Path.GetRelativePath(sourceDataDir, src);
                var dst = Path.Combine(destDataDir, rel);
                var dstDir = Path.GetDirectoryName(dst);
                if (!string.IsNullOrEmpty(dstDir)) Directory.CreateDirectory(dstDir);
                File.Copy(src, dst, overwrite: true);
                copied++;
            }

            if (devDll != null)
            {
                var pluginDir = Path.Combine(destDataDir, "SFSE", "Plugins");
                Directory.CreateDirectory(pluginDir);
                File.Copy(devDll, Path.Combine(pluginDir, "PULSAR.dll"), overwrite: true);
                copied++;
                var pdb = Path.ChangeExtension(devDll, ".pdb");
                if (File.Exists(pdb))
                {
                    File.Copy(pdb, Path.Combine(pluginDir, "PULSAR.pdb"), overwrite: true);
                    copied++;
                }
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

            return new InstallResult(true, copied, devDll != null
                ? $"Installed {copied} mod file(s) into the game (dev build: {devDll})."
                : $"Installed {copied} mod file(s) into the game.");
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
