using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class ExplorerProcessProbe
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadLibrary(string path);
    [DllImport("kernel32.dll", ExactSpelling = true, SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string name);
    [DllImport("kernel32.dll")]
    private static extern bool FreeLibrary(IntPtr module);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr SetWindowsHookEx(int kind, IntPtr callback, IntPtr module, uint threadId);
    [DllImport("user32.dll")]
    private static extern bool UnhookWindowsHookEx(IntPtr hook);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern uint RegisterWindowMessage(string name);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr RemoveProp(IntPtr window, string key);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr wParam,
        IntPtr lParam, uint flags, uint timeout, out IntPtr result);

    public static long LabCommand(IntPtr window, uint owner, string path, int command, string reply)
    {
        uint actual;
        uint thread = GetWindowThreadProcessId(window, out actual);
        if (thread == 0 || actual != owner)
            throw new InvalidOperationException("Test window changed owner.");
        IntPtr module = LoadLibrary(path);
        if (module == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error());
        IntPtr hook = IntPtr.Zero;
        try {
            IntPtr callback = GetProcAddress(module, "ShellStreamLabWindowHook");
            if (callback == IntPtr.Zero)
                throw new Win32Exception(Marshal.GetLastWin32Error());
            uint message = RegisterWindowMessage("stdio.shell.stream.lab.2026");
            RemoveProp(window, reply);
            hook = SetWindowsHookEx(4, callback, module, thread);
            if (hook == IntPtr.Zero)
                throw new Win32Exception(Marshal.GetLastWin32Error());
            IntPtr ignored;
            if (SendMessageTimeout(window, message, (IntPtr)owner, (IntPtr)command, 2, 5000,
                out ignored) == IntPtr.Zero)
                throw new Win32Exception(Marshal.GetLastWin32Error());
            IntPtr value = RemoveProp(window, reply);
            if (value == IntPtr.Zero)
                throw new InvalidOperationException("The test hook did not return a result.");
            return value.ToInt64() - 1;
        } finally {
            if (hook != IntPtr.Zero)
                UnhookWindowsHookEx(hook);
            FreeLibrary(module);
        }
    }

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint access, bool inherit, uint processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] buffer,
        UIntPtr length, out UIntPtr read);

    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr handle);

    public static byte[] ReadCode(uint processId, IntPtr address, int count)
    {
        const uint VM_READ = 0x10;
        IntPtr process = OpenProcess(VM_READ, false, processId);
        if (process == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error());
        try {
            var data = new byte[count];
            UIntPtr read;
            if (!ReadProcessMemory(process, address, data, (UIntPtr)count, out read))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            if (read.ToUInt64() != (ulong)count)
                throw new InvalidOperationException("Incomplete image read.");
            return data;
        } finally {
            CloseHandle(process);
        }
    }
}
