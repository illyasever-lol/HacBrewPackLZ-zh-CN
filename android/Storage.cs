using Android;
using Android.App;
using Android.Content;
using Android.Content.PM;
using Android.Database;
using Android.OS;
using Android.Provider;
using Uri = Android.Net.Uri;

namespace HacBrewPackLz;

/// <summary>
/// Files as the rest of the program knows them: paths. Android's file pickers hand back something else, which is
/// turned back into the path it stands for. That only works with access to all files, which is what lets games be read
/// where they are and the compressed copies be saved beside them.
/// </summary>
internal static class Storage
{
    private const string ExternalStorage = "com.android.externalstorage.documents";
    private const string Downloads = "com.android.providers.downloads.documents";
    private const string Media = "com.android.providers.media.documents";

    public static string Primary => Android.OS.Environment.ExternalStorageDirectory!.AbsolutePath;

    /// <summary>A path as people know the place: "Download/Games" rather than "/storage/emulated/0/Download/Games".</summary>
    public static string Friendly(string path)
    {
        string primary = Primary.TrimEnd('/') + "/";
        if (path.StartsWith(primary, StringComparison.Ordinal))
            return path[primary.Length..];
        if (path.TrimEnd('/') == Primary.TrimEnd('/'))
            return "Phone storage";
        if (path.StartsWith("/storage/", StringComparison.Ordinal))
        {
            int slash = path.IndexOf('/', "/storage/".Length);
            return slash < 0 ? "Memory card" : "Memory card/" + path[(slash + 1)..];
        }
        return path;
    }

    public static bool HasAccess(Context context) =>
        OperatingSystem.IsAndroidVersionAtLeast(30)
            ? Android.OS.Environment.IsExternalStorageManager
            : context.CheckSelfPermission(Manifest.Permission.WriteExternalStorage) == Permission.Granted;

    public static void AskForAccess(Activity activity)
    {
        if (!OperatingSystem.IsAndroidVersionAtLeast(30))
        {
            activity.RequestPermissions([Manifest.Permission.ReadExternalStorage, Manifest.Permission.WriteExternalStorage], 1);
            return;
        }
        try
        {
            activity.StartActivity(new Intent(Android.Provider.Settings.ActionManageAppAllFilesAccessPermission, Uri.Parse("package:" + activity.PackageName)));
        }
        catch (ActivityNotFoundException)
        {
            activity.StartActivity(new Intent(Android.Provider.Settings.ActionManageAllFilesAccessPermission));
        }
    }

    /// <summary>Asks for game files, or for any one file with <paramref name="multiple"/> off.</summary>
    public static Intent PickFiles(bool multiple)
    {
        var intent = new Intent(Intent.ActionOpenDocument);
        intent.AddCategory(Intent.CategoryOpenable);
        intent.SetType("*/*");
        intent.PutExtra(Intent.ExtraAllowMultiple, multiple);
        intent.PutExtra("android.content.extra.SHOW_ADVANCED", true);
        return intent;
    }

    public static Intent PickFolder()
    {
        var intent = new Intent(Intent.ActionOpenDocumentTree);
        intent.PutExtra("android.content.extra.SHOW_ADVANCED", true);
        return intent;
    }

    /// <summary>Every file the picker gave back, as paths, with the names of any that couldn't be turned into one.</summary>
    public static (List<string> paths, List<string> failed) PathsFrom(Context context, Intent? data)
    {
        var uris = new List<Uri>();
        if (data?.ClipData is { } clip)
        {
            for (int i = 0; i < clip.ItemCount; i++)
            {
                if (clip.GetItemAt(i)?.Uri is { } uri)
                    uris.Add(uri);
            }
        }
        else if (data?.Data is { } single)
        {
            uris.Add(single);
        }

        var paths = new List<string>();
        var failed = new List<string>();
        foreach (var uri in uris)
        {
            if (FileOf(context, uri) is { } path)
                paths.Add(path);
            else
                failed.Add(DisplayName(context, uri) ?? uri.LastPathSegment ?? uri.ToString()!);
        }
        return (paths, failed);
    }

    public static string? FolderFrom(Intent? data)
    {
        if (data?.Data is not { } uri || !DocumentsContract.IsTreeUri(uri))
            return null;
        string? id = DocumentsContract.GetTreeDocumentId(uri);
        return uri.Authority switch
        {
            ExternalStorage => FromStorageId(id),
            Downloads when id == "downloads" => System.IO.Path.Combine(Primary, Android.OS.Environment.DirectoryDownloads!),
            Downloads when id?.StartsWith("raw:", StringComparison.Ordinal) == true => id[4..],
            _ => null,
        };
    }

    private static string? FileOf(Context context, Uri uri)
    {
        if (uri.Scheme == "file")
            return uri.Path;
        if (!DocumentsContract.IsDocumentUri(context, uri))
            return null;
        string? id = DocumentsContract.GetDocumentId(uri);
        if (id is null)
            return null;
        switch (uri.Authority)
        {
            case ExternalStorage:
                return FromStorageId(id);
            case Downloads:
                if (id.StartsWith("raw:", StringComparison.Ordinal))
                    return id[4..];
                if (id.StartsWith("msf:", StringComparison.Ordinal) && FromMediaStore(context, id[4..]) is { } media)
                    return media;
                // What's in the Downloads list is almost always in the Download folder, under the name it's listed as. Only taken for it when
                // the size is the same too: another file of that name there, with the picked one in a folder inside, would be read in its place.
                if (DisplayName(context, uri) is { } name && SizeOf(context, uri) is { } size)
                {
                    string guess = System.IO.Path.Combine(Primary, Android.OS.Environment.DirectoryDownloads!, name);
                    if (File.Exists(guess) && new FileInfo(guess).Length == size)
                        return guess;
                }
                return null;
            case Media:
                int colon = id.IndexOf(':');
                return colon > 0 ? FromMediaStore(context, id[(colon + 1)..]) : null;
            default:
                return null;
        }
    }

    /// <summary>"primary:Games/x.nsp" is on the phone's own storage, and "1234-ABCD:x.nsp" on a memory card.</summary>
    private static string? FromStorageId(string? id)
    {
        if (id is null)
            return null;
        int colon = id.IndexOf(':');
        string volume = colon >= 0 ? id[..colon] : id;
        string inside = colon >= 0 ? id[(colon + 1)..] : "";
        string root = volume switch
        {
            "primary" => Primary,
            "home" => System.IO.Path.Combine(Primary, Android.OS.Environment.DirectoryDocuments!),
            _ => "/storage/" + volume,
        };
        return inside.Length > 0 ? System.IO.Path.Combine(root, inside) : root;
    }

    private static string? FromMediaStore(Context context, string id)
    {
        try
        {
            using ICursor? cursor = context.ContentResolver!.Query(MediaStore.Files.GetContentUri("external")!, ["_data"], "_id=?", [id], null);
            if (cursor is not null && cursor.MoveToFirst() && cursor.GetString(0) is { Length: > 0 } path && File.Exists(path))
                return path;
        }
        catch (Java.Lang.Exception)
        {
        }
        return null;
    }

    /// <summary>How large the picked file is, as the picker says, or null if it doesn't.</summary>
    private static long? SizeOf(Context context, Uri uri)
    {
        try
        {
            using ICursor? cursor = context.ContentResolver!.Query(uri, [IOpenableColumns.Size], null, null, null);
            if (cursor is not null && cursor.MoveToFirst() && !cursor.IsNull(0))
                return cursor.GetLong(0);
        }
        catch (Java.Lang.Exception)
        {
        }
        return null;
    }

    private static string? DisplayName(Context context, Uri uri)
    {
        try
        {
            using ICursor? cursor = context.ContentResolver!.Query(uri, [IOpenableColumns.DisplayName], null, null, null);
            if (cursor is not null && cursor.MoveToFirst())
                return cursor.GetString(0);
        }
        catch (Java.Lang.Exception)
        {
        }
        return null;
    }

    /// <summary>Where keys are kept inside the app, which is one of the places the rest of the program looks for them.</summary>
    public static string KeysFolder => System.IO.Path.Combine(System.Environment.GetFolderPath(System.Environment.SpecialFolder.UserProfile), ".switch");

    /// <summary>
    /// Copies the chosen keys into the app, with title.keys when it's beside them, so they go on working wherever the
    /// file was. Returns what went wrong, if anything.
    /// </summary>
    public static string? KeepKeys(Context context, Intent? data)
    {
        if (data?.Data is not { } uri)
            return null;
        try
        {
            Directory.CreateDirectory(KeysFolder);
            string target = System.IO.Path.Combine(KeysFolder, "prod.keys");
            using (var input = context.ContentResolver!.OpenInputStream(uri) ?? throw new IOException("it couldn't be opened"))
            {
                var text = new MemoryStream();
                input.CopyTo(text);
                if (text.Length > 1 << 20 || !System.Text.Encoding.UTF8.GetString(text.ToArray()).Contains('='))
                    return "That doesn't look like a keys file. It's the prod.keys your Switch's keys were dumped to.";
                File.WriteAllBytes(target, text.ToArray());
            }
            string titleKeys = System.IO.Path.Combine(KeysFolder, "title.keys");
            if (FileOf(context, uri) is { } path && System.IO.Path.GetDirectoryName(path) is { } folder && File.Exists(System.IO.Path.Combine(folder, "title.keys")))
                File.Copy(System.IO.Path.Combine(folder, "title.keys"), titleKeys, true);
            return null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or Java.Lang.Exception)
        {
            return $"Couldn't keep the keys: {e.Message}";
        }
    }
}
