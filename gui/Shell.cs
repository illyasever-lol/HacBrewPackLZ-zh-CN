using System.Runtime.InteropServices;

namespace HacBrewPackLz;

/// <summary>The standard Windows file and folder pickers, called directly through their COM interfaces.</summary>
internal static unsafe class FileDialogs
{
    private const uint ClsCtxInproc = 1;
    private const uint OptionOverwritePrompt = 0x2;
    private const uint OptionPickFolders = 0x20;
    private const uint OptionForceFileSystem = 0x40;
    private const uint OptionAllowMultiSelect = 0x200;
    private const uint OptionPathMustExist = 0x800;
    private const uint OptionFileMustExist = 0x1000;
    private const uint NameFileSystemPath = 0x80058000;

    // IFileDialog slots, after IUnknown and IModalWindow.
    private const int SlotShow = 3;
    private const int SlotSetFileTypes = 4;
    private const int SlotSetOptions = 9;
    private const int SlotGetOptions = 10;
    private const int SlotSetFolder = 12;
    private const int SlotSetFileName = 15;
    private const int SlotSetTitle = 17;
    private const int SlotGetResult = 20;
    private const int SlotSetDefaultExtension = 22;
    private const int SlotGetResults = 27;

    private const int SlotArrayGetCount = 7;
    private const int SlotArrayGetItemAt = 8;
    private const int SlotItemGetDisplayName = 5;

    private static Guid s_openClass = new("DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7");
    private static Guid s_saveClass = new("C0B4E2F3-BA21-4773-8DBA-335EC946EB8B");
    private static Guid s_openInterface = new("D57C7288-D4AD-4768-BE02-9D969532D960");
    private static Guid s_saveInterface = new("84BCCD23-5FDE-4CDB-AEA4-AF64B83D78AB");
    private static Guid s_itemInterface = new("43826D1E-E718-42EE-BC55-A1E261C37BFE");

    public static List<string> OpenFiles(IntPtr owner, string title, string filterName, string filterSpec, bool multiple) =>
        Open(owner, title, filterName, filterSpec, multiple ? OptionAllowMultiSelect | OptionFileMustExist : OptionFileMustExist);

    public static List<string> OpenFolders(IntPtr owner, string title, bool multiple) =>
        Open(owner, title, null, null, OptionPickFolders | (multiple ? OptionAllowMultiSelect : 0));

    public static string? SaveFile(IntPtr owner, string title, string filterName, string filterSpec, string extension, string? startPath)
    {
        if (Win32.CoCreateInstance(ref s_saveClass, IntPtr.Zero, ClsCtxInproc, ref s_saveInterface, out var dialog) != 0)
            return null;
        try
        {
            Prepare(dialog, title, filterName, filterSpec, OptionOverwritePrompt | OptionPathMustExist);
            fixed (char* defaultExtension = extension)
                ((delegate* unmanaged<IntPtr, char*, int>)Method(dialog, SlotSetDefaultExtension))(dialog, defaultExtension);
            if (startPath is { Length: > 0 })
            {
                fixed (char* name = Path.GetFileName(startPath))
                    ((delegate* unmanaged<IntPtr, char*, int>)Method(dialog, SlotSetFileName))(dialog, name);
                SetStartFolder(dialog, Path.GetDirectoryName(startPath));
            }
            if (((delegate* unmanaged<IntPtr, IntPtr, int>)Method(dialog, SlotShow))(dialog, owner) != 0)
                return null;

            IntPtr item;
            if (((delegate* unmanaged<IntPtr, IntPtr*, int>)Method(dialog, SlotGetResult))(dialog, &item) != 0)
                return null;
            string? path = ItemPath(item);
            Release(item);
            return path;
        }
        finally
        {
            Release(dialog);
        }
    }

    private static List<string> Open(IntPtr owner, string title, string? filterName, string? filterSpec, uint options)
    {
        var paths = new List<string>();
        if (Win32.CoCreateInstance(ref s_openClass, IntPtr.Zero, ClsCtxInproc, ref s_openInterface, out var dialog) != 0)
            return paths;
        try
        {
            Prepare(dialog, title, filterName, filterSpec, options | OptionPathMustExist);
            if (((delegate* unmanaged<IntPtr, IntPtr, int>)Method(dialog, SlotShow))(dialog, owner) != 0)
                return paths;

            IntPtr results;
            if (((delegate* unmanaged<IntPtr, IntPtr*, int>)Method(dialog, SlotGetResults))(dialog, &results) != 0)
                return paths;
            uint count;
            ((delegate* unmanaged<IntPtr, uint*, int>)Method(results, SlotArrayGetCount))(results, &count);
            for (uint i = 0; i < count; i++)
            {
                IntPtr item;
                if (((delegate* unmanaged<IntPtr, uint, IntPtr*, int>)Method(results, SlotArrayGetItemAt))(results, i, &item) != 0)
                    continue;
                if (ItemPath(item) is { } path)
                    paths.Add(path);
                Release(item);
            }
            Release(results);
        }
        finally
        {
            Release(dialog);
        }
        return paths;
    }

    private static void Prepare(IntPtr dialog, string title, string? filterName, string? filterSpec, uint options)
    {
        uint existing;
        ((delegate* unmanaged<IntPtr, uint*, int>)Method(dialog, SlotGetOptions))(dialog, &existing);
        ((delegate* unmanaged<IntPtr, uint, int>)Method(dialog, SlotSetOptions))(dialog, existing | options | OptionForceFileSystem);
        fixed (char* titleChars = title)
            ((delegate* unmanaged<IntPtr, char*, int>)Method(dialog, SlotSetTitle))(dialog, titleChars);

        if (filterName is null || filterSpec is null)
            return;
        fixed (char* name = filterName, spec = filterSpec, allName = "所有文件", allSpec = "*.*")
        {
            var filters = stackalloc COMDLG_FILTERSPEC[2];
            filters[0] = new COMDLG_FILTERSPEC { Name = (IntPtr)name, Spec = (IntPtr)spec };
            filters[1] = new COMDLG_FILTERSPEC { Name = (IntPtr)allName, Spec = (IntPtr)allSpec };
            ((delegate* unmanaged<IntPtr, uint, COMDLG_FILTERSPEC*, int>)Method(dialog, SlotSetFileTypes))(dialog, 2, filters);
        }
    }

    private static void SetStartFolder(IntPtr dialog, string? folder)
    {
        if (string.IsNullOrEmpty(folder) || !Directory.Exists(folder))
            return;
        if (Win32.SHCreateItemFromParsingName(folder, IntPtr.Zero, ref s_itemInterface, out var item) != 0)
            return;
        ((delegate* unmanaged<IntPtr, IntPtr, int>)Method(dialog, SlotSetFolder))(dialog, item);
        Release(item);
    }

    private static string? ItemPath(IntPtr item)
    {
        char* name;
        if (((delegate* unmanaged<IntPtr, uint, char**, int>)Method(item, SlotItemGetDisplayName))(item, NameFileSystemPath, &name) != 0)
            return null;
        string? path = new string(name);
        Win32.CoTaskMemFree((IntPtr)name);
        return path;
    }

    private static void* Method(IntPtr instance, int slot) => (*(void***)instance)[slot];

    private static void Release(IntPtr instance) =>
        ((delegate* unmanaged<IntPtr, uint>)Method(instance, 2))(instance);
}

/// <summary>Lets files be dragged in from Explorer, and knows when they're being held over the window so it can say so.</summary>
internal static unsafe class DropTarget
{
    private const int NoError = 0;
    private const int NoInterface = unchecked((int)0x80004002);
    private const uint EffectNone = 0;
    private const uint EffectCopy = 1;
    private const ushort FormatFileList = 15;
    private const uint AspectContent = 1;
    private const uint MediumGlobal = 1;

    private static readonly Guid UnknownInterface = new("00000000-0000-0000-C000-000000000046");
    private static readonly Guid DropInterface = new("00000122-0000-0000-C000-000000000046");

    private static IntPtr s_object;
    private static Action<bool>? s_hover;
    private static Action<List<string>>? s_dropped;

    public static bool Register(IntPtr window, Action<bool> hover, Action<List<string>> dropped)
    {
        s_hover = hover;
        s_dropped = dropped;
        if (s_object == IntPtr.Zero)
        {
            var table = (IntPtr*)NativeMemory.Alloc((nuint)(7 * sizeof(IntPtr)));
            table[0] = (IntPtr)(delegate* unmanaged<IntPtr, Guid*, IntPtr*, int>)&QueryInterface;
            table[1] = (IntPtr)(delegate* unmanaged<IntPtr, uint>)&AddRef;
            table[2] = (IntPtr)(delegate* unmanaged<IntPtr, uint>)&Release;
            table[3] = (IntPtr)(delegate* unmanaged<IntPtr, IntPtr, uint, long, uint*, int>)&DragEnter;
            table[4] = (IntPtr)(delegate* unmanaged<IntPtr, uint, long, uint*, int>)&DragOver;
            table[5] = (IntPtr)(delegate* unmanaged<IntPtr, int>)&DragLeave;
            table[6] = (IntPtr)(delegate* unmanaged<IntPtr, IntPtr, uint, long, uint*, int>)&Drop;
            var instance = (IntPtr*)NativeMemory.Alloc((nuint)sizeof(IntPtr));
            *instance = (IntPtr)table;
            s_object = (IntPtr)instance;
        }
        return Win32.RegisterDragDrop(window, s_object) == 0;
    }

    [UnmanagedCallersOnly]
    private static int QueryInterface(IntPtr self, Guid* id, IntPtr* result)
    {
        if (*id == UnknownInterface || *id == DropInterface)
        {
            *result = self;
            return NoError;
        }
        *result = IntPtr.Zero;
        return NoInterface;
    }

    // One object lives as long as the program, so there's nothing to count.
    [UnmanagedCallersOnly]
    private static uint AddRef(IntPtr self) => 1;

    [UnmanagedCallersOnly]
    private static uint Release(IntPtr self) => 1;

    [UnmanagedCallersOnly]
    private static int DragEnter(IntPtr self, IntPtr data, uint keys, long point, uint* effect)
    {
        bool files = HasFiles(data);
        *effect = files ? EffectCopy : EffectNone;
        if (files)
            Guarded(() => s_hover?.Invoke(true));
        return NoError;
    }

    [UnmanagedCallersOnly]
    private static int DragOver(IntPtr self, uint keys, long point, uint* effect)
    {
        *effect &= EffectCopy;
        return NoError;
    }

    [UnmanagedCallersOnly]
    private static int DragLeave(IntPtr self)
    {
        Guarded(() => s_hover?.Invoke(false));
        return NoError;
    }

    [UnmanagedCallersOnly]
    private static int Drop(IntPtr self, IntPtr data, uint keys, long point, uint* effect)
    {
        *effect = EffectCopy;
        var files = ReadFiles(data);
        Guarded(() =>
        {
            s_hover?.Invoke(false);
            if (files.Count > 0)
                s_dropped?.Invoke(files);
        });
        return NoError;
    }

    private static void Guarded(Action action)
    {
        try
        {
            action();
        }
        catch (Exception e)
        {
            GuiProgram.Fatal(e);
        }
    }

    private static FORMATETC FileListFormat => new() { Format = FormatFileList, Aspect = AspectContent, Index = -1, Medium = MediumGlobal };

    private static bool HasFiles(IntPtr data)
    {
        var format = FileListFormat;
        return ((delegate* unmanaged<IntPtr, FORMATETC*, int>)(*(void***)data)[5])(data, &format) == NoError;
    }

    private static List<string> ReadFiles(IntPtr data)
    {
        var files = new List<string>();
        var format = FileListFormat;
        STGMEDIUM medium;
        if (((delegate* unmanaged<IntPtr, FORMATETC*, STGMEDIUM*, int>)(*(void***)data)[3])(data, &format, &medium) != NoError)
            return files;

        uint count = Win32.DragQueryFileW(medium.Data, 0xFFFFFFFF, null, 0);
        var buffer = stackalloc char[0x8000];
        for (uint i = 0; i < count; i++)
        {
            uint length = Win32.DragQueryFileW(medium.Data, i, buffer, 0x8000);
            if (length > 0)
                files.Add(new string(buffer, 0, (int)length));
        }
        Win32.ReleaseStgMedium(ref medium);
        return files;
    }
}

/// <summary>Shows a long job on the program's taskbar button, for when the window is out of sight.</summary>
internal static unsafe class TaskbarProgress
{
    private const uint ClsCtxInproc = 1;
    private const int SlotInit = 3;
    private const int SlotSetValue = 9;
    private const int SlotSetState = 10;
    private const int StateNone = 0;
    private const int StateNormal = 2;
    private const ulong Scale = 1000;

    private static Guid s_class = new("56FDF344-FD6D-11D0-958A-006097C9A090");
    private static Guid s_interface = new("EA1AFB91-9E28-4B86-90E9-9E9F8A5EEFAF");
    private static IntPtr s_list;
    private static bool s_tried;

    public static void Set(IntPtr window, double? fraction)
    {
        if (!s_tried)
        {
            s_tried = true;
            if (Win32.CoCreateInstance(ref s_class, IntPtr.Zero, ClsCtxInproc, ref s_interface, out s_list) == 0)
                ((delegate* unmanaged<IntPtr, int>)(*(void***)s_list)[SlotInit])(s_list);
        }
        if (s_list == IntPtr.Zero)
            return;

        ((delegate* unmanaged<IntPtr, IntPtr, int, int>)(*(void***)s_list)[SlotSetState])(s_list, window, fraction is null ? StateNone : StateNormal);
        if (fraction is { } value)
            ((delegate* unmanaged<IntPtr, IntPtr, ulong, ulong, int>)(*(void***)s_list)[SlotSetValue])(s_list, window, (ulong)(Math.Clamp(value, 0, 1) * Scale), Scale);
    }
}
