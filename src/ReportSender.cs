using System.Text;
using System.Text.Json;

namespace HacBrewPackLz;

/// <summary>
/// Sends a report to the report service, which keeps it, grouped with others about the same problem, for whoever reads its inbox. A report goes
/// only when it's asked for, or with the person's agreement for problems the app finds itself, and there are never keys in it. Whenever it
/// can't be sent, it's saved as a file instead, as it always was. See report-service\README.md.
/// </summary>
internal static class ReportSender
{
    /// <summary>Where reports go: the report service in report-service/. With no address, reports are saved as files.</summary>
    public const string ServiceUrl = "https://hacbrewpacklz-reports.hacbrewpacklz.workers.dev";

    /// <summary>Somewhere else to send them, for testing, or "none" for nowhere, so a test never reaches the real service.</summary>
    private static string Address => Environment.GetEnvironmentVariable("HACBREWPACKLZ_REPORT_SITE") is { Length: > 0 } test ? (test == "none" ? "" : test) : ServiceUrl;

    public static bool CanSend => Address.Length > 0;

    /// <param name="Status">"received", "known" or "fixed" when it went, and empty when it was saved instead.</param>
    /// <param name="SavedTo">Where it was saved, when it couldn't be sent.</param>
    public sealed record Outcome(string Status, int Id, string FixedIn, int Issue, string? SavedTo, string? Problem)
    {
        public bool Sent => Status.Length > 0;

        /// <summary>What to tell the person.</summary>
        public string Said => Status switch
        {
            "received" => $"Sent as report {Id}. Thanks: it goes straight to the developer.",
            "fixed" => FixedIn.Length > 0 ? $"That's a known problem, fixed in version {FixedIn}. Updating to it should sort it out." : "That's a known problem, already fixed.",
            "known" => "That's a known problem, and it's being worked on.",
            _ when SavedTo is null => Problem ?? "It couldn't be sent.",
            // With nowhere to send it yet, saving it was all there was to do.
            _ when Problem is null => $"Saved to {SavedTo}, with no keys in it.",
            _ => $"It couldn't be sent ({Problem}), so it's saved to {SavedTo}.",
        };
    }

    /// <summary>The report as it's shown and sent: the person's own name taken out of paths.</summary>
    public static string Prepared(string report)
    {
        string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        return home.Length > 3 ? report.Replace(home, "~", StringComparison.OrdinalIgnoreCase) : report;
    }

    /// <param name="kind">manual, failed-job, crash, freeze or game-didnt-start.</param>
    /// <param name="fingerprint">What makes it the same problem as another, such as the error and the step, or empty for a message of its own.</param>
    public static Outcome Send(string kind, string fingerprint, string title, string said, string report, int count = 1)
    {
        report = Prepared(report);
        if (!CanSend)
            return Save(report, said, null);
        var tried = TrySend(kind, fingerprint, title, said, report, AppInfo.Version, count);
        return tried.Sent ?? Save(report, said, tried.Problem);
    }

    /// <summary>Sent once, with nothing saved when it can't be.</summary>
    /// <param name="TryAgain">Whether it could go another time: the service was out of reach, or had had enough for now. Otherwise it was
    /// turned away for what it is, and sending it again won't change that.</param>
    public readonly record struct Tried(Outcome? Sent, string? Problem, bool TryAgain);

    /// <param name="version">The version it happened on, which isn't this one when it's sent after an update.</param>
    public static Tried TrySend(string kind, string fingerprint, string title, string said, string report, string version, int count = 1)
    {
        if (!CanSend)
            return new Tried(null, "there's nowhere to send it", true);
        var (code, root) = Post("/report", json =>
        {
            json.WriteString("app", AppInfo.Name);
            json.WriteString("version", version);
            json.WriteString("platform", Platform);
            json.WriteString("kind", kind);
            json.WriteString("fingerprint", fingerprint);
            json.WriteString("title", title);
            json.WriteString("said", said);
            json.WriteString("report", Prepared(report));
            json.WriteString("device", Device);
            json.WriteNumber("count", Math.Max(count, 1));
        });
        if (code == 0)
            return new Tried(null, "the report service couldn't be reached", true);
        string? Get(string name) => root is { } r && r.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;
        int Number(string name) => root is { } r && r.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.Number ? value.GetInt32() : 0;
        if (code is < 200 or > 299)
            return new Tried(null, Get("error") ?? $"the service said {code}", code is 429 or >= 500);
        return new Tried(new Outcome(Get("status") ?? "received", Number("id"), Get("fixedIn") ?? "", Number("issue"), null, null), null, false);
    }

    /// <summary>A problem this sent before, seen again, only counted. Whether that's dealt with: it was counted, or the service has no such
    /// problem, so there's nothing to count it against.</summary>
    public static bool Seen(string fingerprint, string version, int count)
    {
        if (!CanSend)
            return false;
        var (code, _) = Post("/seen", json =>
        {
            json.WriteString("app", AppInfo.Name);
            json.WriteString("version", version);
            json.WriteString("fingerprint", fingerprint);
            json.WriteString("device", Device);
            json.WriteNumber("count", Math.Clamp(count, 1, 1000));
        });
        return code is (>= 200 and <= 299) or 404;
    }

    /// <summary>What came back, with 0 when nothing did.</summary>
    private static (int Code, JsonElement? Reply) Post(string path, Action<Utf8JsonWriter> write)
    {
        try
        {
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(30) };
            http.DefaultRequestHeaders.UserAgent.ParseAdd(AppInfo.Name);
            var body = new MemoryStream();
            using (var json = new Utf8JsonWriter(body))
            {
                json.WriteStartObject();
                write(json);
                json.WriteEndObject();
            }
            using var content = new ByteArrayContent(body.ToArray());
            content.Headers.ContentType = new System.Net.Http.Headers.MediaTypeHeaderValue("application/json");
            using var response = http.PostAsync(Address.TrimEnd('/') + path, content).GetAwaiter().GetResult();
            string text = response.Content.ReadAsStringAsync().GetAwaiter().GetResult();
            JsonElement? reply = null;
            try
            {
                using var document = JsonDocument.Parse(text);
                reply = document.RootElement.Clone();
            }
            catch (JsonException)
            {
            }
            return ((int)response.StatusCode, reply);
        }
        catch (Exception e) when (e is HttpRequestException or TaskCanceledException or IOException)
        {
            return (0, null);
        }
    }

    private static Outcome Save(string report, string said, string? problem)
    {
        try
        {
            string text = said.Length > 0 ? $"What I was doing: {said}{Environment.NewLine}{Environment.NewLine}{report}" : report;
            return new Outcome("", 0, "", 0, Help.SaveReport(text), problem);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return new Outcome("", 0, "", 0, null, $"It couldn't be sent or saved: {e.Message}");
        }
    }

    private static string Platform =>
        OperatingSystem.IsAndroid() ? "Android" : OperatingSystem.IsWindows() ? "Windows" : OperatingSystem.IsMacOS() ? "Mac" : OperatingSystem.IsLinux() ? "Linux" : "PC";

    /// <summary>A name this copy made up for itself the first time, so the service can tell one device from another and nothing more.</summary>
    internal static string Device
    {
        get
        {
            string path = Path.Combine(Settings.Folder, "device.txt");
            try
            {
                if (File.Exists(path) && File.ReadAllText(path).Trim() is { Length: 32 } known)
                    return known;
                string made = Guid.NewGuid().ToString("N");
                Directory.CreateDirectory(Settings.Folder);
                File.WriteAllText(path, made);
                return made;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                return "";
            }
        }
    }
}
