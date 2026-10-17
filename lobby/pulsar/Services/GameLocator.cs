using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;

namespace Pulsar.Services;

// Best-effort auto-detection of the Starfield install so the user does not have to
// browse for the exe. Scans Steam libraries (parsed from libraryfolders.vdf) plus
// common locations on every fixed drive. Registry-free to keep the net8.0 build
// dependency-clean. Prefers sfse_loader.exe (co-op needs SFSE) over Starfield.exe.
public static class GameLocator
{
    public static string? FindStarfield()
    {
        foreach (var dir in CandidateGameDirs())
        {
            var hit = ExeIn(dir);
            if (hit != null) return hit;
        }
        return null;
    }

    private static string? ExeIn(string gameDir)
    {
        try
        {
            var sfse = Path.Combine(gameDir, "sfse_loader.exe");
            if (File.Exists(sfse)) return sfse;
            var sf = Path.Combine(gameDir, "Starfield.exe");
            if (File.Exists(sf)) return sf;
        }
        catch { /* ignore malformed paths */ }
        return null;
    }

    private static IEnumerable<string> CandidateGameDirs()
    {
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var d in Collect())
            if (!string.IsNullOrEmpty(d) && seen.Add(d))
                yield return d;
    }

    private static IEnumerable<string> Collect()
    {
        // 1. Environment variable overrides (useful for Linux, Proton/Wine, custom setups)
        foreach (var envVar in new[] { "STARFIELD_PATH", "STARFIELD_DIR", "STARFIELD_ROOT", "GAME_DIR" })
        {
            var val = Environment.GetEnvironmentVariable(envVar)?.Trim();
            if (!string.IsNullOrEmpty(val))
            {
                if (File.Exists(val))
                    val = Path.GetDirectoryName(val);
                if (!string.IsNullOrEmpty(val) && SafeDirExists(val))
                    yield return val;
            }
        }

        // 2. User profile Games folders (e.g. C:\Users\<user>\Games\Starfield*)
        var userProfile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        if (!string.IsNullOrEmpty(userProfile))
        {
            var userGames = Path.Combine(userProfile, "Games");
            foreach (var sfDir in FindStarfieldSubdirs(userGames))
                yield return sfDir;
        }

        // 3. Fixed drives common paths (e.g. D:\Games\Starfield*, C:\Games\Starfield*)
        foreach (var drive in FixedDrives())
        {
            var driveGames = Path.Combine(drive, "Games");
            foreach (var sfDir in FindStarfieldSubdirs(driveGames))
                yield return sfDir;

            yield return Path.Combine(drive, "Games", "Starfield");
            yield return Path.Combine(drive, "SteamLibrary", "steamapps", "common", "Starfield");
            yield return Path.Combine(drive, "Steam", "steamapps", "common", "Starfield");
        }

        // 4. Linux / Wine / Heroic / Lutris paths
        if (!OperatingSystem.IsWindows() || !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("WINEPREFIX")))
        {
            var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            if (!string.IsNullOrEmpty(home))
            {
                yield return Path.Combine(home, ".wine", "drive_c", "Games", "Starfield");
                yield return Path.Combine(home, "Games", "Heroic", "Starfield");
                yield return Path.Combine(home, "Games", "Starfield");
            }
        }

        // 5. Steam roots (parsed from libraryfolders.vdf)
        foreach (var steam in SteamRoots())
        {
            yield return Path.Combine(steam, "steamapps", "common", "Starfield");
            foreach (var lib in SteamLibraries(steam))
                yield return Path.Combine(lib, "steamapps", "common", "Starfield");
        }
    }

    private static IEnumerable<string> FindStarfieldSubdirs(string parentDir)
    {
        if (!SafeDirExists(parentDir)) yield break;
        string[] matches;
        try { matches = Directory.GetDirectories(parentDir, "*Starfield*", SearchOption.TopDirectoryOnly); }
        catch { yield break; }
        foreach (var dir in matches)
            yield return dir;
    }


    private static IEnumerable<string> SteamRoots()
    {
        var roots = new List<string>
        {
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Steam"),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Steam"),
        };
        foreach (var drive in FixedDrives())
        {
            roots.Add(Path.Combine(drive, "Steam"));
            roots.Add(Path.Combine(drive, "SteamLibrary"));
        }
        foreach (var r in roots)
            if (SafeDirExists(r)) yield return r;
    }

    // Parse libraryfolders.vdf "path" entries to discover extra Steam libraries.
    private static IEnumerable<string> SteamLibraries(string steamRoot)
    {
        string vdf;
        try
        {
            var p = Path.Combine(steamRoot, "steamapps", "libraryfolders.vdf");
            if (!File.Exists(p)) yield break;
            vdf = File.ReadAllText(p);
        }
        catch { yield break; }

        foreach (Match m in Regex.Matches(vdf, "\"path\"\\s+\"([^\"]+)\""))
        {
            var path = m.Groups[1].Value.Replace("\\\\", "\\");
            if (SafeDirExists(path)) yield return path;
        }
    }

    private static IEnumerable<string> FixedDrives()
    {
        DriveInfo[] drives;
        try { drives = DriveInfo.GetDrives(); }
        catch { yield break; }
        foreach (var d in drives)
        {
            bool ok = false;
            try { ok = d.DriveType == DriveType.Fixed && d.IsReady; } catch { }
            if (ok) yield return d.RootDirectory.FullName;
        }
    }

    private static bool SafeDirExists(string p)
    {
        try { return Directory.Exists(p); } catch { return false; }
    }
}
