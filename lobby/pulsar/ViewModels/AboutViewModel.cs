using System;
using System.Diagnostics;
using System.Reflection;
using CommunityToolkit.Mvvm.Input;

namespace Pulsar.ViewModels;

public partial class AboutViewModel : ViewModelBase
{
    public AboutViewModel(MainWindowViewModel shell) { }

    public override string Title => "About";
    public override string Glyph => char.ConvertFromUtf32(0xE946); // Info

    // Modified fork: LICENSE-ADDITIONAL requires a distinct name, this view to mark
    // the version as modified, and the attribution notice below.
    public string AppName => "PULSAR Revived";

    public string Version =>
        "Version " + (Assembly.GetExecutingAssembly().GetName().Version?.ToString(3) ?? "1.0.0");

    public string Author => "Modified version. Based on PULSAR by ramisotti13-eng";

    public string OriginalUrl => "https://github.com/ramisotti13-eng/pulsar";

    // Contact goes through the project page rather than a personal address, so
    // nothing here has to be scrubbed before the source can be published.
    public string Email => ProjectUrl;

    public string ProjectUrl => "https://github.com/Pinganiyo/pulsar-tool";

    [RelayCommand]
    private void OpenProject()
    {
        try { Process.Start(new ProcessStartInfo { FileName = ProjectUrl, UseShellExecute = true }); }
        catch { /* best-effort: no browser / blocked */ }
    }
}
