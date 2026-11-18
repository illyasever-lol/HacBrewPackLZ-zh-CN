namespace HacBrewPackLz;

/// <summary>
/// Checking a game for damage, as Check this game for damage does in every version: reading all of it back through its hashes, every section
/// of the game, its update and its DLC, whatever kind of file it's in, as the Switch reads a game to check it. A file that's whole reads back as
/// its hashes say; anything changed in it since it was made doesn't. How its parts are named isn't damage: games repacked by other tools often
/// keep names that no longer match their hashes, and play all the same.
/// </summary>
internal static class GameCheck
{
    /// <summary>What reading a game through to check it calls itself, which a job's progress goes by.</summary>
    public const string Reading = "正在读取全部内容";

    /// <param name="jobs">The game and its DLC, as the list put them together.</param>
    /// <param name="spoolFolder">Where a solid NCZ's data waits while it's read, as when it's analyzed: beside the game if there's nowhere else.</param>
    /// <exception cref="ToolException">What doesn't read back as it should.</exception>
    public static void Run(IReadOnlyList<TitleJob> jobs, string? spoolFolder)
    {
        foreach (var job in jobs)
            ReadAll(job, spoolFolder);
    }

    /// <summary>Every section of it read through, in the order they lie in its file, so each hash is checked on the way and a solid NCZ is read
    /// once from front to back.</summary>
    private static void ReadAll(TitleJob job, string? spoolFolder)
    {
        NczSource.SpoolFolder = spoolFolder ?? Path.GetDirectoryName(Path.GetFullPath(job.Main.Archive.FilePath));
        try
        {
            var parts = new List<DataSource>();
            foreach (var content in job.Contents)
            {
                foreach (int i in content.SectionsInFileOrder())
                {
                    var (owner, section, original) = content.SectionAt(i)!.Value;
                    parts.Add(section.FsType == NcaFsSection.FsPartition ? owner.OpenPfs0(section, original).Data : owner.OpenRomFsImage(section, original));
                }
            }
            var progress = new ProgressReporter(Reading, parts.Sum(p => p.Length));
            var buffer = new byte[1 << 20];
            long done = 0;
            foreach (var part in parts)
            {
                for (long position = 0; position < part.Length;)
                {
                    Ui.Token.ThrowIfCancellationRequested();
                    int n = (int)Math.Min(buffer.Length, part.Length - position);
                    part.Read(position, buffer, 0, n);
                    position += n;
                    done += n;
                    progress.Report(done, done);
                }
            }
            progress.Done();
        }
        finally
        {
            job.ReleaseMemory();
        }
    }
}
