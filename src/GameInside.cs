using System.Buffers.Binary;
using System.Text;

namespace HacBrewPackLz;

/// <summary>
/// A game opened to be looked at, as Details and Look inside show it in every version: its parts as folders, named for what they're for and
/// in the same order whatever order a game keeps them in, what most of it is first and its DLC last; what each file is, by its first bytes
/// before its name; a file read to be shown the way its kind is shown; and any of it saved to Files from games. The same as the Switch app's
/// (switch/app/app_inside.cpp), which it was made from, so a game looks the same inside on a PC, a phone and a Switch.
/// </summary>
internal sealed class GameInside : IDisposable
{
    /// <summary>Where what's saved from inside games goes, in a folder named after each game: never where extracted games go, as a few of a
    /// game's parts there could be taken for all of it. Finding games never looks in it.</summary>
    public const string FilesFromGames = "Files from games";

    /// <summary>How much of a file is looked at to tell whether it's text, read to show it as text, and shown as bytes.</summary>
    public const int TextLooked = 256 << 10;
    public const int TextRead = 1 << 20;
    public const int BytesShown = 64 << 10;
    /// <summary>The biggest picture file that's read to be shown.</summary>
    public const long PictureMost = 24L << 20;
    private const int MostLines = 20000;
    private const int MostFound = 2000;

    public sealed class Node
    {
        public required string Name { get; init; }
        public bool Folder { get; init; }
        public int Parent { get; init; } = -1;
        public List<int> Children { get; } = [];
        /// <summary>For a file, the part it's in and where in it.</summary>
        public int Part { get; init; } = -1;
        public long Offset { get; init; }
        /// <summary>A file's size, or all a folder holds.</summary>
        public long Size { get; set; }
        /// <summary>What's straight inside a folder.</summary>
        public int Items { get; set; }
        /// <summary>What it is, in a word: by its name until it's been read, then by its bytes.</summary>
        public string Kind { get; set; } = "Data";
    }

    /// <summary>What's shown for whatever's under the pointer, the way its kind is shown.</summary>
    public sealed class Preview
    {
        public required string Kind { get; init; }
        /// <summary>What there is to say about it in a few words: its kind, its size, how many lines or how big a picture.</summary>
        public List<string> Chips { get; init; } = [];
        public List<string> Lines { get; init; } = [];
        public int TotalLines { get; init; }
        /// <summary>Its bytes as they are, the first of them, for what's shown as bytes.</summary>
        public byte[]? Raw { get; init; }
        /// <summary>A picture file's bytes, for each version to decode its own way: in a folder, the first picture straight inside it.</summary>
        public byte[]? Picture { get; init; }
        public int PictureWidth { get; set; }
        public int PictureHeight { get; set; }
        /// <summary>For a folder, what it's made of all the way down, by kind, the most first.</summary>
        public List<(string Kind, int Files, long Bytes)> Breakdown { get; init; } = [];
    }

    private TitleSet? _set;
    private readonly List<(string Name, DataSource Data)> _parts = [];

    /// <summary>The name the list has for it.</summary>
    public string Name { get; }
    /// <summary>What a copy of it is called, which is what the folder for what's saved from it is called.</summary>
    public string Stem { get; private set; } = "";
    public List<Node> Nodes { get; } = [];
    public int Files { get; private set; }
    public long Bytes { get; private set; }

    public string? Publisher { get; private set; }
    public string? DisplayVersion { get; private set; }
    /// <summary>The languages its name is in, as people call them.</summary>
    public List<string> Languages { get; } = [];
    /// <summary>Its icon as the game has it, the JPEG, which the list only keeps small.</summary>
    public byte[]? Icon { get; private set; }

    public ulong GameId { get; private set; }
    public uint GameVersion { get; private set; }
    public bool WithUpdate { get; private set; }
    public int Dlcs { get; private set; }
    /// <summary>The system version it asks for, as its record says, its update's if that asks for more.</summary>
    public uint RequiredSystem { get; private set; }
    /// <summary>The newest keys it's made with, and whether it needs a ticket, which only a game with a rights ID does.</summary>
    public int Generation { get; private set; }
    public bool NeedsTicket { get; private set; }
    public bool KeysNewEnough { get; private set; }

    private GameInside(string name) => Name = name;

    public void Dispose()
    {
        _set?.Dispose();
        _set = null;
    }

    /// <summary>Opens the game's files, the game first, then its update and its DLC, and lays out what's inside as folders.</summary>
    /// <exception cref="ToolException">Why it couldn't be opened.</exception>
    public static GameInside Open(string name, IReadOnlyList<string> files, Keyset keyset, CancellationToken token = default)
    {
        var inside = new GameInside(name);
        try
        {
            inside.Read(files, keyset, token);
        }
        catch
        {
            inside.Dispose();
            throw;
        }
        return inside;
    }

    private void Read(IReadOnlyList<string> files, Keyset keyset, CancellationToken token)
    {
        var problems = new List<InputIssue>();
        _set = TitleSet.Load(files, keyset, token: token);
        TitleJob? main = null;
        var dlcs = new List<TitleJob>();
        foreach (var job in _set.Jobs)
        {
            if (job.Main.Meta.Type == Cnmt.MetaAddOnContent)
                dlcs.Add(job);
            else
                main ??= job;
        }
        if (main is null && dlcs.Count > 0)
        {
            main = dlcs[0];
            dlcs.RemoveAt(0);
        }
        if (main is null)
            throw new ToolException(_set.Problems.Count > 0 ? _set.Problems[0].Message : "Nothing in it can be read");

        uint version = main.Update?.Meta.Version ?? main.Main.Meta.Version;
        Stem = main.Stem(version, false);
        GameId = main.ApplicationId;
        GameVersion = version;
        WithUpdate = main.Update is not null;
        Dlcs = dlcs.Count;
        RequiredSystem = Math.Max(main.Main.Meta.RequiredSystemVersion, main.Update?.Meta.RequiredSystemVersion ?? 0);
        foreach (var content in main.Contents)
        {
            Generation = Math.Max(Generation, content.Primary.GenerationIndex);
            NeedsTicket = NeedsTicket || content.Primary.HasRightsId;
        }
        KeysNewEnough = NeedsTicket ? keyset.TryTitleKek(Generation) is not null : keyset.TryKeyAreaKey(0, Generation) is not null;

        // The top, then each part as a folder of its own.
        Nodes.Add(new Node { Name = Name, Folder = true });
        var folders = new Dictionary<(int, string), int>();
        int FolderIn(int parent, string folderName)
        {
            if (folders.TryGetValue((parent, folderName), out int found))
                return found;
            int index = Nodes.Count;
            Nodes.Add(new Node { Name = folderName, Folder = true, Parent = parent, Kind = "Folder" });
            Nodes[parent].Children.Add(index);
            folders[(parent, folderName)] = index;
            return index;
        }
        void FileIn(int parent, string fileName, int part, long offset, long size)
        {
            int index = Nodes.Count;
            Nodes.Add(new Node { Name = fileName, Parent = parent, Part = part, Offset = offset, Size = size, Kind = KindByName(fileName) });
            Nodes[parent].Children.Add(index);
        }
        void AddSection(ContentPair content, int section, string prefix)
        {
            token.ThrowIfCancellationRequested();
            if (content.SectionAt(section) is not { } source)
                return;
            // A DLC's data is all a DLC is, so it's called that alone.
            string own = PartName(content, section, source.Section);
            string partName = prefix.Length == 0 ? own : own == "Data" ? prefix : $"{prefix} {own}";
            int part = _parts.Count;
            int top = FolderIn(0, partName);
            if (source.Section.FsType == NcaFsSection.FsPartition)
            {
                var archive = source.Owner.OpenPfs0(source.Section, source.Original);
                _parts.Add((partName, archive.Data));
                foreach (var file in archive.Files)
                    FileIn(top, file.Name, part, file.Offset, file.Size);
                return;
            }
            var image = source.Owner.OpenRomFsImage(source.Section, source.Original);
            var romfs = RomFsArchive.Open(image, source.Owner.Name);
            _parts.Add((partName, image));
            int PlaceOf(string path, bool isFolder, out string last)
            {
                last = "";
                int at = top;
                var pieces = path.Split('/', StringSplitOptions.RemoveEmptyEntries);
                for (int i = 0; i < pieces.Length; i++)
                {
                    if (i == pieces.Length - 1 && !isFolder)
                    {
                        last = pieces[i];
                        return at;
                    }
                    at = FolderIn(at, pieces[i]);
                }
                return at;
            }
            foreach (string folder in romfs.Directories)
                PlaceOf(folder, true, out _);
            foreach (var file in romfs.Files)
            {
                int parent = PlaceOf(file.Path, false, out string last);
                FileIn(parent, last, part, file.Offset, file.Size);
            }
        }
        foreach (var content in main.Contents)
        {
            foreach (int section in content.SectionsInFileOrder())
                AddSection(content, section, "");
        }
        for (int i = 0; i < dlcs.Count; i++)
        {
            foreach (var content in dlcs[i].Contents)
            {
                foreach (int section in content.SectionsInFileOrder())
                    AddSection(content, section, dlcs.Count == 1 ? "DLC" : $"DLC {i + 1}");
            }
        }

        // The parts in the same order whatever order a game keeps them in: what most of it is first, its DLC last.
        string[] order = ["Game data", "Program", "Icons and names", "Manual", "Legal notices", "Start-up logo", "Data", "Part"];
        int Rank(int index)
        {
            string partName = Nodes[index].Name;
            for (int i = 0; i < order.Length; i++)
            {
                if (partName.StartsWith(order[i], StringComparison.Ordinal))
                    return i;
            }
            return 100;
        }
        var ranked = Nodes[0].Children.OrderBy(Rank).ToList();
        Nodes[0].Children.Clear();
        Nodes[0].Children.AddRange(ranked);

        // What each folder holds, and its own in order: folders first, then files, each by name.
        for (int i = Nodes.Count - 1; i >= 0; i--)
        {
            var node = Nodes[i];
            if (!node.Folder)
            {
                Files++;
                Bytes += node.Size;
                continue;
            }
            node.Size = node.Children.Sum(c => Nodes[c].Size);
            node.Items = node.Children.Count;
        }
        for (int i = 1; i < Nodes.Count; i++)
        {
            if (!Nodes[i].Folder)
                continue;
            var sorted = Nodes[i].Children.OrderBy(c => Nodes[c].Folder ? 0 : 1).ThenBy(c => Nodes[c].Name, StringComparer.OrdinalIgnoreCase).ToList();
            Nodes[i].Children.Clear();
            Nodes[i].Children.AddRange(sorted);
        }

        ReadControl(token);
    }

    /// <summary>The names, the languages and the icon, from its control data, which aren't worth giving up the rest for.</summary>
    private void ReadControl(CancellationToken token)
    {
        int control = _parts.FindIndex(p => p.Name == "Icons and names");
        if (control < 0)
            return;
        try
        {
            var nacp = Nodes.FirstOrDefault(n => n.Part == control && !n.Folder && n.Name == "control.nacp" && n.Size >= Nacp.Size);
            if (nacp is not null)
            {
                var bytes = ReadAt(nacp, Nacp.Size);
                var titles = Nacp.Titles(bytes);
                DisplayVersion = Nacp.DisplayVersion(bytes) is { Length: > 0 } shown ? shown : null;
                for (int i = 0; i < titles.Count && i < LanguageNames.Length; i++)
                {
                    // British English is English too, as far as anyone choosing a game goes.
                    if (titles[i].Name.Length > 0 && !(i == 1 && titles[0].Name.Length > 0))
                        Languages.Add(LanguageNames[i]);
                }
                Publisher = titles.FirstOrDefault(t => t.Publisher.Length > 0).Publisher;
            }
        }
        catch (Exception e) when (e is ToolException or IOException)
        {
        }
        token.ThrowIfCancellationRequested();
        try
        {
            // The American English icon, as the list's is, or the first there is.
            Node? best = null;
            foreach (var node in Nodes)
            {
                if (node.Folder || node.Part != control || !node.Name.StartsWith("icon_", StringComparison.Ordinal) || node.Size > 1 << 20)
                    continue;
                if (best is null || node.Name == "icon_AmericanEnglish.dat")
                    best = node;
            }
            if (best is not null)
                Icon = ReadAt(best, (int)best.Size);
        }
        catch (Exception e) when (e is ToolException or IOException)
        {
        }
    }

    /// <summary>The languages a game has its name in, in the NACP's own order.</summary>
    private static readonly string[] LanguageNames =
    [
        "English", "English (UK)", "Japanese", "French", "German", "Spanish (Latin America)", "Spanish", "Italian", "Dutch", "French (Canada)",
        "Portuguese", "Russian", "Korean", "Chinese (Traditional)", "Chinese (Simplified)", "Portuguese (Brazil)",
    ];

    /// <summary>The game's parts, named for what they're for.</summary>
    private static string PartName(ContentPair content, int section, NcaFsSection fs)
    {
        string name = content.Type switch
        {
            Cnmt.ContentTypeProgram => section == 1 ? "Game data" : section == 2 ? "Start-up logo" : fs.FsType == NcaFsSection.FsPartition ? "Program" : $"Part {section}",
            Cnmt.ContentTypeControl => "Icons and names",
            Cnmt.ContentTypeHtmlDocument => "Manual",
            Cnmt.ContentTypeLegalInformation => "Legal notices",
            Cnmt.ContentTypeData => "Data",
            _ => $"Part {content.Type}",
        };
        return content.IdOffset != 0 ? $"{name} {content.IdOffset + 1}" : name;
    }

    /// <summary>Where a file or folder is, inside the game: "Game data/Content/Portraits/Abigail.png".</summary>
    public string PathOf(int index)
    {
        var pieces = new List<string>();
        for (int i = index; i > 0; i = Nodes[i].Parent)
            pieces.Add(Nodes[i].Name);
        pieces.Reverse();
        return string.Join('/', pieces);
    }

    /// <summary>The file or folder at that path inside the game, as <see cref="PathOf"/> writes it, or -1. Case doesn't matter.</summary>
    public int Find(string path)
    {
        int at = 0;
        foreach (string piece in path.Replace('\\', '/').Split('/', StringSplitOptions.RemoveEmptyEntries))
        {
            int next = Nodes[at].Children.FirstOrDefault(c => string.Equals(Nodes[c].Name, piece, StringComparison.OrdinalIgnoreCase), -1);
            if (next < 0)
                return -1;
            at = next;
        }
        return at;
    }

    /// <summary>Files and folders whose names have the text in them, from anywhere in the game, case aside.</summary>
    public List<int> Search(string text)
    {
        var found = new List<int>();
        for (int i = 1; i < Nodes.Count && found.Count < MostFound; i++)
        {
            if (Nodes[i].Name.Contains(text, StringComparison.OrdinalIgnoreCase))
                found.Add(i);
        }
        return found;
    }

    /// <summary>The start of a file, or all of it.</summary>
    public byte[] ReadAt(Node file, int count)
    {
        var data = new byte[Math.Min(count, file.Size)];
        if (data.Length > 0)
            _parts[file.Part].Data.Read(file.Offset, data, 0, data.Length);
        return data;
    }

    /// <summary>What's at that place, read and made ready to show the way its kind is shown, or as bytes when that's asked for.</summary>
    public Preview Show(int index, bool asBytes, CancellationToken token = default)
    {
        var node = Nodes[index];
        if (node.Folder)
        {
            var kinds = new Dictionary<string, (int files, long bytes)>();
            var stack = new Stack<int>(node.Children);
            while (stack.Count > 0)
            {
                var child = Nodes[stack.Pop()];
                if (child.Folder)
                {
                    foreach (int c in child.Children)
                        stack.Push(c);
                    continue;
                }
                var (files, bytes) = kinds.GetValueOrDefault(child.Kind);
                kinds[child.Kind] = (files + 1, bytes + child.Size);
            }
            var chips = new List<string> { "Folder", ItemsWord(node.Items), ProgressReporter.Format(node.Size) };
            // And the first picture straight inside it, such as a game's icon, which says more than any count.
            byte[]? picture = null;
            foreach (int c in node.Children)
            {
                var child = Nodes[c];
                if (child.Folder || child.Kind != "Picture" || child.Size > PictureMost)
                    continue;
                token.ThrowIfCancellationRequested();
                var data = ReadAt(child, (int)child.Size);
                if (KindByBytes(data, child.Kind, child.Name) == "Picture" && PictureSize(data, out _, out _))
                {
                    picture = data;
                    chips.Add("Showing " + child.Name);
                    break;
                }
            }
            return new Preview
            {
                Kind = "Folder", Chips = chips, Picture = picture,
                Breakdown = kinds.Select(k => (k.Key, k.Value.files, k.Value.bytes)).OrderByDescending(k => k.bytes).ToList(),
            };
        }
        if (node.Size == 0)
            return new Preview { Kind = node.Kind, Chips = [node.Kind, "Empty"] };

        token.ThrowIfCancellationRequested();
        int want = (int)Math.Min(node.Size, TextRead);
        if (node.Kind == "Picture" && node.Size <= PictureMost)
            want = (int)node.Size;
        var read = ReadAt(node, want);
        string kind = KindByBytes(read, node.Kind, node.Name);
        var said = new List<string> { kind, ProgressReporter.Format(node.Size) };
        if (!asBytes && kind == "Picture" && want == node.Size && PictureSize(read, out int width, out int height))
        {
            node.Kind = kind;
            said.Add($"{width} × {height}");
            return new Preview { Kind = kind, Chips = said, Picture = read, PictureWidth = width, PictureHeight = height };
        }
        if (!asBytes && (IsUtf16(read) || ReadsAsText(read.AsSpan(0, Math.Min(read.Length, TextLooked)))))
        {
            string text = IsUtf16(read) ? FromUtf16(read) : Encoding.UTF8.GetString(read);
            var lines = LinesOf(text, out int total);
            node.Kind = "Text";
            said[0] = "Text";
            string count = $"{WithCommas(total)} {(total == 1 ? "line" : "lines")}";
            said.Add(want < node.Size ? $"{count} in its first {ProgressReporter.Format(want)}" : count);
            return new Preview { Kind = "Text", Chips = said, Lines = lines, TotalLines = total };
        }
        // Any other kind, as yet: its bytes as they are, which is how every kind can be looked at.
        node.Kind = kind;
        var raw = read.AsSpan(0, Math.Min(read.Length, BytesShown)).ToArray();
        said.Add(raw.Length < node.Size ? $"Its first {ProgressReporter.Format(raw.Length)} as bytes" : "As bytes");
        return new Preview { Kind = kind, Chips = said, Raw = raw };
    }

    /// <summary>
    /// A file, or all of a folder, copied to Files from games in <paramref name="filesFromGames"/>, in a folder named after the game, the way it
    /// is inside it. A file cut short is deleted, as it would look like the whole of it. Returns where it went.
    /// </summary>
    /// <param name="progress">How much of it has been saved, as it goes: bytes and files of how many.</param>
    /// <param name="chosenFolder">The folder was chosen to put it in, as Android's Save to… does: it goes straight in, as itself, rather than in
    /// a folder named after the game, and under a name of its own when something there has its name already.</param>
    public string Save(int index, string filesFromGames, Action<long, long, int, int>? progress = null, CancellationToken token = default, bool chosenFolder = false)
    {
        var files = new List<int>();
        long bytes = 0;
        var stack = new Stack<int>();
        stack.Push(index);
        while (stack.Count > 0)
        {
            int at = stack.Pop();
            var node = Nodes[at];
            if (node.Folder)
            {
                for (int i = node.Children.Count - 1; i >= 0; i--)
                    stack.Push(node.Children[i]);
            }
            else
            {
                files.Add(at);
                bytes += node.Size;
            }
        }
        string root = Path.Combine(filesFromGames, LocalNames.ForOutputName(Stem.Length > 0 ? Stem : Name));
        // Into a chosen folder, the paths start at what's saved, and all of a game still goes in a folder named after it.
        int above = 0;
        if (chosenFolder && index != 0)
        {
            root = filesFromGames;
            above = Nodes[index].Parent;
        }
        string Under(int at)
        {
            var pieces = new List<string>();
            for (int i = at; i > 0 && i != above; i = Nodes[i].Parent)
                pieces.Add(LocalNames.ForFileSystem(Nodes[i].Name));
            pieces.Reverse();
            return pieces.Count == 0 ? root : Path.Combine([root, .. pieces]);
        }
        string top = Under(index);
        string free = chosenFolder ? FreeName(top) : top;
        string PathFor(int at) => free == top ? Under(at) : free + Under(at)[top.Length..];
        var buffer = new byte[4 << 20];
        long done = 0;
        int saved = 0;
        var seen = new HashSet<string>(LocalNames.PathComparer);
        foreach (int at in files)
        {
            token.ThrowIfCancellationRequested();
            var node = Nodes[at];
            string path = PathFor(at);
            // Two names that differ only by case would silently go over each other on most Windows and Mac disks, as extracting says too.
            if (!seen.Add(path))
                throw new ToolException($"'{PathOf(at)}' would overwrite another file on this file system, whose names ignore case or can't hold every character");
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            bool whole = false;
            try
            {
                using (var output = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None))
                {
                    for (long written = 0; written < node.Size;)
                    {
                        token.ThrowIfCancellationRequested();
                        int n = (int)Math.Min(buffer.Length, node.Size - written);
                        _parts[node.Part].Data.Read(node.Offset + written, buffer, 0, n);
                        output.Write(buffer, 0, n);
                        written += n;
                        done += n;
                        progress?.Invoke(done, bytes, saved, files.Count);
                    }
                }
                whole = true;
            }
            finally
            {
                if (!whole)
                    File.Delete(path);
            }
            saved++;
            progress?.Invoke(done, bytes, saved, files.Count);
        }
        return PathFor(index);
    }

    /// <summary>A name nothing has yet beside it: "Abigail.png", or else "Abigail (2).png" and so on, as a phone names a second copy.</summary>
    private static string FreeName(string path)
    {
        if (!File.Exists(path) && !Directory.Exists(path))
            return path;
        string folder = Path.GetDirectoryName(path)!, stem = Path.GetFileNameWithoutExtension(path), extension = Path.GetExtension(path);
        for (int n = 2; ; n++)
        {
            string other = Path.Combine(folder, $"{stem} ({n}){extension}");
            if (!File.Exists(other) && !Directory.Exists(other))
                return other;
        }
    }

    /// <summary>Where what's saved from games goes for this game file: beside where extracted games go, which is beside the file itself unless
    /// the options say otherwise.</summary>
    public static string FilesFromGamesFor(string gameFile, string? extractFolder) =>
        Path.Combine(extractFolder is { Length: > 0 } chosen ? chosen : Path.GetDirectoryName(Path.GetFullPath(gameFile))!, FilesFromGames);

    /// <summary>Whether a path is in a folder of what's been saved from games, which is never taken for a game, nor gone through for games.</summary>
    public static bool IsInFilesFromGames(string path) =>
        path.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar).Any(p => string.Equals(p, FilesFromGames, StringComparison.OrdinalIgnoreCase));

    /// <summary>Bytes as they're looked at: where each line starts, its bytes, and each as a character where it's one, as on the Switch.</summary>
    public static IEnumerable<string> ByteLines(byte[] raw, int perLine, int count)
    {
        var line = new StringBuilder();
        for (int start = 0; start < count; start += perLine)
        {
            line.Clear();
            line.Append(start.ToString("X8")).Append("  ");
            int end = Math.Min(start + perLine, count);
            for (int i = start; i < start + perLine; i++)
                line.Append(i < end ? raw[i].ToString("X2") + " " : "   ");
            line.Append(' ');
            for (int i = start; i < end; i++)
                line.Append(raw[i] is >= 0x20 and < 0x7F ? (char)raw[i] : '.');
            yield return line.ToString();
        }
    }

    public static string ItemsWord(int items) => items == 1 ? "1 item" : $"{WithCommas(items)} items";

    /// <summary>A count as the Switch writes it, a comma between each three figures, whatever the PC's own way is.</summary>
    public static string WithCommas(long count) => count.ToString("N0", System.Globalization.CultureInfo.InvariantCulture);

    // -------------------------------------------------------------------------------------------------------------------
    // What a file is

    private static readonly Dictionary<string, string> Kinds = new(StringComparer.OrdinalIgnoreCase)
    {
        ["txt"] = "Text", ["json"] = "Text", ["xml"] = "Text", ["ini"] = "Text", ["cfg"] = "Text", ["csv"] = "Text", ["lua"] = "Text", ["js"] = "Text",
        ["html"] = "Text", ["htm"] = "Text", ["css"] = "Text", ["yaml"] = "Text", ["yml"] = "Text", ["md"] = "Text", ["log"] = "Text", ["tsv"] = "Text",
        ["glsl"] = "Text", ["hlsl"] = "Text", ["shader"] = "Text", ["py"] = "Text", ["cs"] = "Text", ["toml"] = "Text", ["properties"] = "Text",
        ["png"] = "Picture", ["jpg"] = "Picture", ["jpeg"] = "Picture", ["bmp"] = "Picture", ["tga"] = "Picture", ["gif"] = "Picture", ["webp"] = "Picture",
        ["dds"] = "Texture", ["bntx"] = "Texture", ["bflim"] = "Texture", ["ktx"] = "Texture", ["astc"] = "Texture", ["xtx"] = "Texture",
        ["bfstm"] = "Sound", ["bfwav"] = "Sound", ["bwav"] = "Sound", ["bcstm"] = "Sound", ["bars"] = "Sound", ["ogg"] = "Sound", ["wav"] = "Sound",
        ["mp3"] = "Sound", ["opus"] = "Sound", ["adx"] = "Sound", ["hca"] = "Sound", ["wem"] = "Sound", ["bnk"] = "Sound", ["fsb"] = "Sound",
        ["bank"] = "Sound", ["acb"] = "Sound", ["awb"] = "Sound", ["flac"] = "Sound",
        ["mp4"] = "Video", ["webm"] = "Video", ["usm"] = "Video", ["bk2"] = "Video", ["mov"] = "Video", ["avi"] = "Video", ["mkv"] = "Video",
        ["sarc"] = "Archive", ["szs"] = "Archive", ["zs"] = "Archive", ["zip"] = "Archive", ["arc"] = "Archive", ["narc"] = "Archive",
        ["bfarc"] = "Archive", ["rarc"] = "Archive", ["pack"] = "Archive", ["cpk"] = "Archive", ["bundle"] = "Archive", ["assets"] = "Archive",
        ["resource"] = "Archive", ["pak"] = "Archive", ["7z"] = "Archive", ["gz"] = "Archive", ["lz4"] = "Archive", ["bea"] = "Archive",
        ["ttf"] = "Font", ["otf"] = "Font", ["bfttf"] = "Font", ["bfotf"] = "Font", ["bffnt"] = "Font", ["fnt"] = "Font",
        ["bfres"] = "Model", ["fbx"] = "Model", ["obj"] = "Model", ["gltf"] = "Model", ["glb"] = "Model",
        ["nso"] = "Program", ["nro"] = "Program", ["nrr"] = "Program", ["npdm"] = "Program", ["so"] = "Program", ["dll"] = "Program",
    };

    private static string ExtensionOf(string name)
    {
        int dot = name.LastIndexOf('.');
        return dot < 0 ? "" : name[(dot + 1)..];
    }

    /// <summary>What a file is, in a word, by its name, which is all there is to go by before it's read.</summary>
    public static string KindByName(string name)
    {
        if (Kinds.TryGetValue(ExtensionOf(name), out string? kind))
            return kind;
        // The program's own files have no extension: main, rtld, sdk and subsdk0 to 9.
        if (name is "main" or "rtld" or "sdk" || name.StartsWith("subsdk", StringComparison.Ordinal))
            return "Program";
        // A game's icons are JPEGs, named for the language they're for.
        if (name.StartsWith("icon_", StringComparison.Ordinal) && ExtensionOf(name).Equals("dat", StringComparison.OrdinalIgnoreCase))
            return "Picture";
        return "Data";
    }

    /// <summary>What its first bytes say it is, which goes before its name: a file named as a picture that isn't one is data.</summary>
    public static string KindByBytes(ReadOnlySpan<byte> data, string byName, string name)
    {
        if (data.Length >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
            return "Picture";
        if (data.Length >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
            return "Picture";
        if (data.Length >= 2 && data[0] == 'B' && data[1] == 'M' && byName != "Text")
            return "Picture";
        if ((data.Length >= 4 && data[..4].SequenceEqual("GIF8"u8)) || (data.Length >= 12 && data[..4].SequenceEqual("RIFF"u8) && data[8..12].SequenceEqual("WEBP"u8)))
            return "Picture";
        // TGA pictures don't start with anything that says so.
        if (byName == "Picture" && !ExtensionOf(name).Equals("tga", StringComparison.OrdinalIgnoreCase))
            return "Data";
        return byName;
    }

    /// <summary>Whether bytes read as text: UTF-8, with no NULs and few other control characters.</summary>
    public static bool ReadsAsText(ReadOnlySpan<byte> data)
    {
        if (data.Length == 0)
            return false;
        int odd = 0;
        for (int i = 0; i < data.Length;)
        {
            byte c = data[i];
            if (c == 0)
                return false;
            if (c < 0x80)
            {
                if (c < 0x20 && c != '\n' && c != '\r' && c != '\t' && c != '\f')
                    odd++;
                i++;
                continue;
            }
            int more = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
            if (more < 0)
                return false;
            // A character cut off where the read stopped is still text.
            for (int k = 1; k <= more; k++)
            {
                if (i + k >= data.Length)
                    return true;
                if ((data[i + k] & 0xC0) != 0x80)
                    return false;
            }
            i += more + 1;
        }
        return odd * 50 < data.Length;
    }

    private static bool IsUtf16(byte[] data) => data.Length >= 2 && ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF));

    /// <summary>Text kept as UTF-16, which some games' text is.</summary>
    private static string FromUtf16(byte[] data)
    {
        var encoding = data[0] == 0xFE ? Encoding.BigEndianUnicode : Encoding.Unicode;
        return encoding.GetString(data, 2, (data.Length - 2) & ~1).Replace("\0", "");
    }

    /// <summary>Its lines, as many as there's any point in, with tabs as spaces and other control characters left out.</summary>
    public static List<string> LinesOf(string text, out int total)
    {
        var lines = new List<string>();
        var line = new StringBuilder();
        total = 0;
        int start = text.Length > 0 && text[0] == '﻿' ? 1 : 0;
        for (int i = start; i < text.Length; i++)
        {
            char c = text[i];
            if (c == '\r')
                continue;
            if (c == '\n')
            {
                total++;
                if (lines.Count < MostLines)
                    lines.Add(line.ToString());
                line.Clear();
            }
            else if (c == '\t')
            {
                line.Append(' ', 4 - line.Length % 4);
            }
            else if (c >= 0x20 && line.Length < 400)
            {
                line.Append(c);
            }
        }
        if (line.Length > 0)
        {
            total++;
            if (lines.Count < MostLines)
                lines.Add(line.ToString());
        }
        return lines;
    }

    /// <summary>How big a picture is, from its header, before it's decoded: false when there's no telling.</summary>
    public static bool PictureSize(ReadOnlySpan<byte> data, out int width, out int height)
    {
        width = height = 0;
        if (data.Length >= 24 && data[0] == 0x89 && data[1] == 'P')
        {
            width = BinaryPrimitives.ReadInt32BigEndian(data[16..]);
            height = BinaryPrimitives.ReadInt32BigEndian(data[20..]);
            return width > 0 && height > 0;
        }
        if (data.Length >= 26 && data[0] == 'B' && data[1] == 'M')
        {
            width = BinaryPrimitives.ReadInt32LittleEndian(data[18..]);
            height = Math.Abs(BinaryPrimitives.ReadInt32LittleEndian(data[22..]));
            return width > 0 && height > 0;
        }
        if (data.Length >= 10 && data[..4].SequenceEqual("GIF8"u8))
        {
            width = BinaryPrimitives.ReadUInt16LittleEndian(data[6..]);
            height = BinaryPrimitives.ReadUInt16LittleEndian(data[8..]);
            return width > 0 && height > 0;
        }
        // A JPEG says its size in its frame header, after whatever comes before it.
        if (data.Length >= 4 && data[0] == 0xFF && data[1] == 0xD8)
        {
            int at = 2;
            while (at + 9 < data.Length)
            {
                if (data[at] != 0xFF)
                    return false;
                byte marker = data[at + 1];
                if (marker == 0xFF)
                {
                    at++;
                    continue;
                }
                int length = BinaryPrimitives.ReadUInt16BigEndian(data[(at + 2)..]);
                if (marker is >= 0xC0 and <= 0xCF && marker is not (0xC4 or 0xC8 or 0xCC))
                {
                    height = BinaryPrimitives.ReadUInt16BigEndian(data[(at + 5)..]);
                    width = BinaryPrimitives.ReadUInt16BigEndian(data[(at + 7)..]);
                    return width > 0 && height > 0;
                }
                at += 2 + length;
            }
        }
        return false;
    }
}
