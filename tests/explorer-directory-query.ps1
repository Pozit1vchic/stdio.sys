param([Parameter(Mandatory=$true)][string]$BuildDirectory,
      [ValidateRange(1,50000)][int]$FileCount=40000,
      [ValidateRange(1,30)][int]$Rounds=12)

# Compare the NTFS query class observed in Explorer, outside the live view.
$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class DirectoryQueryProbe
{
    [StructLayout(LayoutKind.Sequential)]
    private struct IoStatus { public IntPtr Status; public UIntPtr Bytes; }

    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    private static extern SafeFileHandle CreateFile(string path, uint access, uint sharing,
        IntPtr security, uint disposition, uint flags, IntPtr template);

    [DllImport("ntdll.dll")]
    private static extern int NtQueryDirectoryFile(SafeFileHandle directory, IntPtr evt,
        IntPtr apc, IntPtr context, out IoStatus status, IntPtr buffer, int length, int infoClass,
        byte single, IntPtr filter, byte restart);

    public sealed class Result
    {
        public int BufferBytes, Queries, Entries;
        public ulong NameHash;
        public double Milliseconds;
    }

    public static Result Run(string path, int capacity)
    {
        const int FileIdBothDirectoryInformation = 37;
        const int NoMoreFiles = unchecked((int)0x80000006);
        using(var directory = CreateFile(path, 1, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero))
        {
            if (directory.IsInvalid)
                throw new Win32Exception(Marshal.GetLastWin32Error());
            IntPtr buffer = Marshal.AllocHGlobal(capacity);
            try {
                var result = new Result { BufferBytes = capacity };
                var clock = new Stopwatch();
                var digest = 14695981039346656037UL;
                for (;;) {
                    IoStatus io;
                    clock.Start();
                    int status = NtQueryDirectoryFile(directory, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero,
                        out io, buffer, capacity, FileIdBothDirectoryInformation, 0, IntPtr.Zero, 0);
                    clock.Stop();
                    result.Queries++;
                    if (status == NoMoreFiles)
                        break;
                    ulong length = io.Bytes.ToUInt64();
                    if (status != 0 || length < 104 || length > (ulong)capacity)
                        throw new InvalidOperationException("Directory query returned " + status.ToString("X"));
                    for (int offset = 0; ; ) {
                        if ((ulong)offset + 104 > length)
                            throw new InvalidOperationException("Invalid directory entry offset.");
                        int next = Marshal.ReadInt32(buffer, offset);
                        int nameBytes = Marshal.ReadInt32(buffer, offset + 60);
                        int record = next == 0 ? (int)length - offset : next;
                        if (record < 104 || record > (int)length - offset ||
                            nameBytes <= 0 || nameBytes % 2 != 0 || nameBytes > record - 104)
                            throw new InvalidOperationException("Invalid directory record.");
                        for (int i = 0; i < nameBytes; i++)
                            digest = unchecked((digest ^ Marshal.ReadByte(buffer, offset + 104 + i)) * 1099511628211UL);
                        digest = unchecked(digest * 1099511628211UL);
                        result.Entries++;
                        if (next == 0)
                            break;
                        offset += next;
                    }
                }
                result.Milliseconds = clock.Elapsed.TotalMilliseconds;
                result.NameHash = digest;
                return result;
            } finally {
                Marshal.FreeHGlobal(buffer);
            }
        }
    }
}
"@

$parent = (Resolve-Path -LiteralPath $BuildDirectory).ProviderPath
$root = Join-Path $parent ('stdio-query-' + [guid]::NewGuid().ToString('N'))
try {
    [void][IO.Directory]::CreateDirectory($root)
    $bytes = [byte[]]@()
    for ($i = 0; $i -lt $FileCount; $i++) {
        [IO.File]::WriteAllBytes((Join-Path $root ('fixture-{0:d5}.bin' -f $i)), $bytes)
    }
    $baseline = [DirectoryQueryProbe]::Run($root, 64KB)
    if ($baseline.Entries -ne $FileCount + 2) { throw 'Unexpected directory entry count.' }
    $sizes = 64KB, 128KB, 256KB
    for ($round = 1; $round -le $Rounds; $round++) {
        foreach ($size in $sizes) {
            $result = [DirectoryQueryProbe]::Run($root, $size)
            if ($result.Entries -ne $baseline.Entries -or $result.NameHash -ne $baseline.NameHash) {
                throw "Directory entries differ at buffer size ${size}."
            }
            $result
        }
        [array]::Reverse($sizes)
    }
} finally {
    if ([IO.Directory]::Exists($root)) {
        for ($i = 0; $i -lt $FileCount; $i++) {
            [IO.File]::Delete((Join-Path $root ('fixture-{0:d5}.bin' -f $i)))
        }
        [IO.Directory]::Delete($root)
    }
}
