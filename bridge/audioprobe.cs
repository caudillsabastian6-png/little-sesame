using System;
using System.Runtime.InteropServices;

[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
class MMDeviceEnumeratorComObject { }

[ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceEnumerator
{
    int EnumAudioEndpoints(int dataFlow, int stateMask, out IMMDeviceCollection devices);
    int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice device);
    int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice device);
    int RegisterEndpointNotificationCallback(IntPtr client);
    int UnregisterEndpointNotificationCallback(IntPtr client);
}

[ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-C0A23830C9B3"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceCollection
{
    int GetCount(out int count);
    int Item(int index, out IMMDevice device);
}

[ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDevice
{
    int Activate(ref Guid iid, int ctx, IntPtr activationParams, out IntPtr iface);
    int OpenPropertyStore(int access, out IPropertyStore props);
    int GetId(out IntPtr id);
    int GetState(out int state);
}

[ComImport, Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IPropertyStore
{
    int GetCount(out int count);
    int GetAt(int index, out PropertyKey key);
    int GetValue(ref PropertyKey key, out PropVariant value);
    int SetValue(ref PropertyKey key, ref PropVariant value);
    int Commit();
}

[StructLayout(LayoutKind.Sequential)]
struct PropertyKey { public Guid fmtid; public int pid; }

[StructLayout(LayoutKind.Explicit)]
struct PropVariant
{
    [FieldOffset(0)] public short vt;
    [FieldOffset(8)] public IntPtr ptr;
}

public class AudioProbe
{
    static string GetName(IMMDevice dev)
    {
        IPropertyStore props;
        dev.OpenPropertyStore(0, out props);
        var key = new PropertyKey { fmtid = new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"), pid = 14 };
        PropVariant v;
        props.GetValue(ref key, out v);
        return v.vt == 31 ? Marshal.PtrToStringUni(v.ptr) : "?";
    }

    static string GetId(IMMDevice dev)
    {
        IntPtr p;
        dev.GetId(out p);
        string s = Marshal.PtrToStringUni(p);
        Marshal.FreeCoTaskMem(p);
        return s;
    }

    public static void Main()
    {
        var en = (IMMDeviceEnumerator)(new MMDeviceEnumeratorComObject());
        IMMDevice def;
        en.GetDefaultAudioEndpoint(0, 0, out def);  // eRender, eConsole
        Console.WriteLine("DEFAULT=" + GetName(def));
        Console.WriteLine("DEFAULT_ID=" + GetId(def));
        // Route logic needs nothing more: the Bluetooth speaker endpoint can
        // only BE the default while it is present and active. If Bluetooth is
        // off or the speaker is gone, Windows fails the default over to
        // something else and the bridge routes local instead.
    }
}
