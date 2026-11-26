param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [ValidateRange(1, 4096)][int]$FileCount = 256,
    [ValidateRange(1, 20)][int]$Rounds = 5,
    [switch]$Recycle,
    [switch]$ClipboardVerbs,
    [switch]$CheckCollision,
    [switch]$FlatSelection,
    [string]$LabCopyDll,
    [ValidateSet('Both', 'Copy', 'Move')][string]$Operation = 'Both'
)

# Run with Windows PowerShell -STA on an interactive desktop and local NTFS.
$ErrorActionPreference = 'Stop'
if ($Recycle -and $ClipboardVerbs) { throw 'Choose recycle or clipboard transfer.' }
if ($Recycle -and $Operation -ne 'Both') { throw 'The operation filter applies to transfers only.' }
if ($Recycle -and !$PSBoundParameters.ContainsKey('FileCount')) { $FileCount = 1 }
if ($FlatSelection -and (!$ClipboardVerbs -or $Operation -ne 'Copy')) {
    throw 'Flat selection requires clipboard copy.'
}
if ($CheckCollision -and ($Recycle -or $ClipboardVerbs -or $Operation -ne 'Both')) {
    throw 'The collision control uses clipboard copy and paste.'
}
if ($LabCopyDll) {
    if (!$CheckCollision -and (!$ClipboardVerbs -or $Operation -ne 'Copy')) {
        throw 'Copy accounting requires clipboard copy or the collision control.'
    }
    Add-Type -Path (Join-Path $PSScriptRoot 'explorer-process-probe.cs')
}
if ($ClipboardVerbs -or $CheckCollision) { Add-Type -AssemblyName System.Windows.Forms }
Add-Type @"
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class TransferProbe
{
    [DllImport("ole32.dll")]
    private static extern int OleInitialize(IntPtr reserved);
    [DllImport("ole32.dll")]
    private static extern void OleUninitialize();
    [DllImport("ole32.dll")]
    private static extern int OleGetClipboard(out IntPtr data);
    [DllImport("ole32.dll")]
    private static extern int OleSetClipboard(IntPtr data);
    [DllImport("ole32.dll")]
    private static extern int OleIsCurrentClipboard(IntPtr data);
    [DllImport("user32.dll")]
    public static extern uint GetClipboardSequenceNumber();

    public sealed class ClipboardBackup : IDisposable
    {
        private IntPtr saved;
        public ClipboardBackup()
        {
            Marshal.ThrowExceptionForHR(OleInitialize(IntPtr.Zero));
            try { Marshal.ThrowExceptionForHR(OleGetClipboard(out saved)); }
            catch { OleUninitialize(); throw; }
        }
        public bool Restore(uint expectedSequence)
        {
            if (GetClipboardSequenceNumber() != expectedSequence) return false;
            int result = OleSetClipboard(saved);
            if (result == unchecked((int)0x800401D4) && OleIsCurrentClipboard(saved) == 0)
                return true;
            Marshal.ThrowExceptionForHR(result);
            return true;
        }
        public void Dispose()
        {
            if (saved != IntPtr.Zero) Marshal.Release(saved);
            saved = IntPtr.Zero;
            OleUninitialize();
        }
    }
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetFileInformationByHandleEx(SafeFileHandle file, int infoClass,
        [Out] byte[] info, uint size);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFile(string path, uint access, uint sharing,
        IntPtr security, uint disposition, uint flags, IntPtr template);

    public static string DirectoryIdentity(string path)
    {
        const uint ReadAttributes = 0x80, ShareAll = 7, OpenExisting = 3, BackupSemantics = 0x02000000;
        using (var handle = CreateFile(path, ReadAttributes, ShareAll, IntPtr.Zero,
            OpenExisting, BackupSemantics, IntPtr.Zero))
        {
            if (handle.IsInvalid)
                throw new Win32Exception(Marshal.GetLastWin32Error());
            return Identity(handle);
        }
    }

    public static string Identity(string path)
    {
        using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read,
            FileShare.ReadWrite | FileShare.Delete))
            return Identity(stream.SafeFileHandle);
    }

    private static string Identity(SafeFileHandle handle)
    {
        var identity = new byte[24]; // FILE_ID_INFO: volume serial followed by FILE_ID_128.
        if (!GetFileInformationByHandleEx(handle, 18, identity, 24))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        return BitConverter.ToString(identity);
    }
}
"@

function Wait-Observed([scriptblock]$Read, [string]$Message) {
    $deadline = [Diagnostics.Stopwatch]::StartNew()
    do {
        $value = & $Read
        if ($value) { return $value }
        Start-Sleep -Milliseconds 10
    } while ($deadline.ElapsedMilliseconds -lt 15000)
    throw $Message
}

function Set-TransferClipboard($Item, [string[]]$Paths, [string]$Verb, [switch]$Collection) {
    if (!$Item) { throw 'The source fixture did not resolve in Explorer.' }
    $priorSequence = [TransferProbe]::GetClipboardSequenceNumber()
    $expected = [Collections.Generic.HashSet[string]]::new($Paths, [StringComparer]::OrdinalIgnoreCase)
    if ($Collection) { $Item.InvokeVerbEx($Verb) } else { $Item.InvokeVerb($Verb) }
    Wait-Observed {
        $sequence = [TransferProbe]::GetClipboardSequenceNumber()
        if ($sequence -eq $priorSequence) { return }
        $pathsOnClipboard = [Windows.Forms.Clipboard]::GetFileDropList()
        if ($pathsOnClipboard.Count -eq $expected.Count -and $expected.SetEquals([string[]]@($pathsOnClipboard))) {
            [TransferProbe]::GetClipboardSequenceNumber()
        }
    } "Explorer did not set the clipboard for $Verb."
}

function Wait-CopyAccounting([long]$Before, [int]$Minimum) {
    if (!$labReady) { return }
    Wait-Observed {
        $now = [ExplorerProcessProbe]::LabCommand([IntPtr]$view.HWND, $owner, $labPath, 21,
                                                'stdio.lab.copy-accounted') - $Before
        if ($now -ge $Minimum) { $now }
    } 'Explorer did not account for the expected native copies.'
}

$parent = (Resolve-Path -LiteralPath $BuildDirectory).ProviderPath
$root = Join-Path $parent ('stdio-transfer-' + [guid]::NewGuid().ToString('N'))
$target = Join-Path $root 'destination'
$recycleName = 'stdio-recycle-' + [guid]::NewGuid().ToString('N')
$recyclePath = Join-Path $target $recycleName
$shell = New-Object -ComObject Shell.Application
$view = $null
$clipboard = $null
$clipboardSequence = $null
$labReady = $false
$payloadSize = 65536
$bytes = [byte[]]::new($payloadSize)
[Random]::new(12345).NextBytes($bytes)

try {
    [void][IO.Directory]::CreateDirectory($target)
    $existing = @(Get-Process explorer | Select-Object -ExpandProperty Id)
    Start-Process (Join-Path $env:windir 'explorer.exe') -ArgumentList "/separate,`"$target`""
    $view = Wait-Observed {
        $shell.Windows() | Where-Object { $_.Document.Folder.Self.Path -eq $target } |
            Select-Object -First 1
    } 'The isolated Explorer window did not open.'
    [uint32]$owner = 0
    [void][TransferProbe]::GetWindowThreadProcessId([IntPtr]$view.HWND, [ref]$owner)
    if (!$owner -or $owner -in $existing) { throw 'The transfer window was not isolated.' }
    $explorer = Get-Process -Id $owner
    if ($LabCopyDll) {
        if ($explorer.Path -ne (Join-Path $env:windir 'explorer.exe')) {
            throw 'The test view does not belong to Windows Explorer.'
        }
        $labDirectory = Join-Path $parent ('stdio-copy-lab-' + [guid]::NewGuid().ToString('N'))
        [void][IO.Directory]::CreateDirectory($labDirectory)
        $labPath = Join-Path $labDirectory 'stdio_shell_lab.dll'
        [IO.File]::Copy((Resolve-Path -LiteralPath $LabCopyDll).ProviderPath, $labPath)
        $image = $explorer.Modules | Where-Object ModuleName -eq windows.storage.dll
        $copyCall = [IntPtr]::Add($image.BaseAddress, 0x34AE60)
        $originalCopy = [BitConverter]::ToString([ExplorerProcessProbe]::ReadCode($owner, $copyCall, 7))
        $install = [ExplorerProcessProbe]::LabCommand([IntPtr]$view.HWND, $owner, $labPath, 20, 'stdio.lab.copy-install')
        if ($install) { throw "Copy accounting refused to install, Win32 error $install." }
        $labReady = $true
    }
    $patcher = $explorer.Modules | Where-Object ModuleName -eq ExplorerPatcher.amd64.dll
    $destination = $view.Document.Folder
    if ($CheckCollision) {
        $clipboard = [TransferProbe+ClipboardBackup]::new()
        for ($round = 1; $round -le $Rounds; $round++) {
            if ($labReady) {
                $before = [ExplorerProcessProbe]::LabCommand([IntPtr]$view.HWND, $owner, $labPath, 21, 'stdio.lab.copy-accounted')
            }
            if ($null -ne $clipboardSequence -and
                [TransferProbe]::GetClipboardSequenceNumber() -ne $clipboardSequence) {
                throw 'The clipboard changed outside the test.'
            }
            $folder = 'stdio-collision-' + [guid]::NewGuid().ToString('N')
            $source = Join-Path $root $folder
            $output = Join-Path $target $folder
            [void][IO.Directory]::CreateDirectory($source)
            [void][IO.Directory]::CreateDirectory($output)
            $original = Join-Path $source '5000-collision.bin'
            $existingFile = Join-Path $output '5000-collision.bin'
            [IO.File]::WriteAllText($original, 'source bytes')
            [IO.File]::WriteAllText($existingFile, 'destination bytes')
            $sourceId = [TransferProbe]::Identity($original)
            $targetId = [TransferProbe]::Identity($existingFile)
            $markers = @('0000-before.bin', '9999-after.bin')
            foreach ($marker in $markers) {
                [IO.File]::WriteAllText((Join-Path $source $marker), $marker)
            }
            $item = $destination.ParentFolder.ParseName($folder)
            $clipboardSequence = Set-TransferClipboard $item $source 'copy'
            $explorer.Refresh()
            $cpu = $explorer.TotalProcessorTime.TotalMilliseconds
            $started = [DateTime]::UtcNow
            $watch = [Diagnostics.Stopwatch]::StartNew()
            if ([TransferProbe]::GetClipboardSequenceNumber() -ne $clipboardSequence) {
                throw 'The clipboard changed before paste.'
            }
            $destination.Self.InvokeVerb('paste')
            $elapsed = Wait-Observed {
                try {
                    foreach ($marker in $markers) {
                        if ([IO.File]::ReadAllText((Join-Path $output $marker)) -cne $marker) { return }
                    }
                } catch [IO.IOException] {
                    # The destination may not exist yet or Explorer may still have it open.
                    return
                }
                $watch.Elapsed.TotalMilliseconds
            } 'Explorer did not produce both noncolliding files.'
            $explorer.Refresh()
            $cpuMs = $explorer.TotalProcessorTime.TotalMilliseconds - $cpu
            foreach ($marker in $markers) {
                if ([IO.File]::ReadAllText((Join-Path $source $marker)) -cne $marker) {
                    throw "Unexpected content for $marker."
                }
            }
            if ([IO.File]::ReadAllText($original) -cne 'source bytes' -or
                [IO.File]::ReadAllText($existingFile) -cne 'destination bytes' -or
                [TransferProbe]::Identity($original) -ne $sourceId -or
                [TransferProbe]::Identity($existingFile) -ne $targetId -or
                [IO.Directory]::GetFiles($output).Count -ne 3) {
                throw 'The sampled paste outcome differed from the skip-collision control.'
            }
            [pscustomobject]@{
                Operation = 'CopyCollision'
                EntryPoint = 'ClipboardVerbs'
                Round = $round
                ExplorerPID = $owner
                ExplorerPatcher = $(if ($patcher) { $patcher.FileVersionInfo.FileVersion })
                StartedUtc = $started.ToString('o')
                ObservedUtc = $started.AddMilliseconds($elapsed).ToString('o')
                SourcePath = $source
                DestinationPath = $output
                ObservedMs = [Math]::Round($elapsed, 2)
                ExplorerCpuMs = [Math]::Round($cpuMs, 2)
                PeakWorkingSetBytes = $explorer.PeakWorkingSet64
                PrivateBytes = $explorer.PrivateMemorySize64
                NoncollidingFilesCopied = $markers.Count
                CollidingFilesPreserved = $true
                AccountedNativeCalls = Wait-CopyAccounting $before $markers.Count
            }
        }
        return
    }
    if ($Recycle) {
        $bin = $shell.NameSpace(10)
        $fileDirectory = if ($FileCount -eq 1) { $recyclePath } else { Join-Path $recyclePath 'nested' }
        [void][IO.Directory]::CreateDirectory($fileDirectory)
        $identities = @{}
        for ($i = 0; $i -lt $FileCount; $i++) {
            $path = Join-Path $fileDirectory ('fixture-{0:d4}.bin' -f $i)
            [IO.File]::WriteAllBytes($path, $bytes)
            $identities[$path] = [TransferProbe]::Identity($path)
        }
        $fixture = Join-Path $fileDirectory 'fixture-0000.bin'
        Set-Content -LiteralPath $fixture -Stream stdio-test -Value 'native-recycle' -NoNewline
        $directoryId = [TransferProbe]::DirectoryIdentity($recyclePath)
        $nestedId = [TransferProbe]::DirectoryIdentity($fileDirectory)
        if ($FileCount -gt 1) {
            $link = Join-Path $fileDirectory 'hardlink.bin'
            New-Item -ItemType HardLink -Path $link -Target $fixture | Out-Null
            $identities[$link] = $identities[$fixture]
        }
        $hash = (Get-FileHash -LiteralPath $fixture).Hash
        $item = $destination.ParseName($recycleName)
        if (!$item) { throw 'The fixture is missing in the Explorer namespace.' }
        $started = [DateTime]::UtcNow
        $timer = [Diagnostics.Stopwatch]::StartNew()
        $item.InvokeVerb('delete')
        $recycled = Wait-Observed {
            if ([IO.Directory]::Exists($recyclePath)) { return }
            $bin.Items() | Where-Object {
                $_.Name -eq $recycleName -and $_.ExtendedProperty('System.Recycle.DeletedFrom') -eq $target
            } | Select-Object -First 1
        } 'The fixture did not appear in the Recycle Bin.'
        $recycleMs = $timer.Elapsed.TotalMilliseconds
        $timer.Restart()
        $recycled.InvokeVerb('undelete')
        $restoreMs = Wait-Observed {
            if ([IO.File]::Exists($fixture)) { $timer.Elapsed.TotalMilliseconds }
        } 'Explorer did not restore the fixture from the Recycle Bin.'
        if ([TransferProbe]::DirectoryIdentity($recyclePath) -ne $directoryId -or
            [TransferProbe]::DirectoryIdentity($fileDirectory) -ne $nestedId -or
            [IO.Directory]::GetFiles($fileDirectory).Length -ne $identities.Count -or
            (Get-Content -LiteralPath $fixture -Stream stdio-test -Raw) -ne 'native-recycle') {
            throw 'The restored tree differs from the original.'
        }
        foreach ($path in $identities.Keys) {
            if ([TransferProbe]::Identity($path) -ne $identities[$path] -or
                (Get-FileHash -LiteralPath $path).Hash -ne $hash) {
                throw "The restored file differs from the original: $path"
            }
        }
        [pscustomobject]@{
            Operation = 'RecycleAndRestore'
            ExplorerPID = $owner
            ExplorerPatcher = $(if ($patcher) { $patcher.FileVersionInfo.FileVersion })
            StartedUtc = $started.ToString('o')
            RecycleMs = [Math]::Round($recycleMs, 2)
            RestoreMs = [Math]::Round($restoreMs, 2)
            IdentityPreserved = $true
            Files = $identities.Count
        }
        return
    }
    $operations = if ($Operation -eq 'Both') { @('Copy', 'Move') } else { @($Operation) }
    $operations = @($operations)
    if ($ClipboardVerbs) { $clipboard = [TransferProbe+ClipboardBackup]::new() }
    for ($round = 1; $round -le $Rounds; $round++) {
        foreach ($transfer in $operations) {
            if ($labReady) {
                $before = [ExplorerProcessProbe]::LabCommand([IntPtr]$view.HWND, $owner, $labPath, 21, 'stdio.lab.copy-accounted')
            }
            $source = Join-Path $root "$transfer-$round"
            $output = Join-Path $target "$transfer-$round"
            [void][IO.Directory]::CreateDirectory($source)
            $pasteFolder = $destination
            if ($FlatSelection) {
                [void][IO.Directory]::CreateDirectory($output)
                $pasteFolder = $destination.ParseName([IO.Path]::GetFileName($output)).GetFolder
                if (!$pasteFolder) { throw 'The flat destination did not resolve in Explorer.' }
            }
            $paths = for ($i = 0; $i -lt $FileCount; $i++) {
                $name = '{0:d4}.bin' -f $i
                [IO.File]::WriteAllBytes((Join-Path $source $name), $bytes)
                Join-Path $output $name
            }
            $firstSource = Join-Path $source '0000.bin'
            $sourceId = [TransferProbe]::Identity($firstSource)
            $expectedHash = (Get-FileHash -LiteralPath $firstSource).Hash
            Set-Content -LiteralPath $firstSource -Stream stdio-test -Value 'native-transfer' -NoNewline
            if ($clipboard) {
                if ($null -ne $clipboardSequence -and
                    [TransferProbe]::GetClipboardSequenceNumber() -ne $clipboardSequence) {
                    throw 'The clipboard changed outside the test.'
                }
                $sourceFolder = $view.Document.Folder.ParentFolder
                $item = $sourceFolder.ParseName([IO.Path]::GetFileName($source))
                $verb = if ($transfer -eq 'Copy') { 'copy' } else { 'cut' }
                if ($FlatSelection) {
                    $clipboardSequence = Set-TransferClipboard ($item.GetFolder.Items()) ([IO.Directory]::GetFiles($source)) $verb -Collection
                } else {
                    $clipboardSequence = Set-TransferClipboard $item $source $verb
                }
            }
            $explorer.Refresh()
            $cpu = $explorer.TotalProcessorTime.TotalMilliseconds
            $started = [DateTime]::UtcNow
            $watch = [Diagnostics.Stopwatch]::StartNew()
            if ($clipboard) {
                if ([TransferProbe]::GetClipboardSequenceNumber() -ne $clipboardSequence) {
                    throw 'The clipboard changed before paste.'
                }
                $pasteFolder.Self.InvokeVerb('paste')
            }
            elseif ($transfer -eq 'Copy') { $destination.CopyHere($source) }
            else { $destination.MoveHere($source) }

            $elapsed = Wait-Observed {
                if ($transfer -eq 'Move' -and [IO.Directory]::Exists($source)) { return }
                foreach ($path in $paths) {
                    $file = [IO.FileInfo]::new($path)
                    if (!$file.Exists -or $file.Length -ne $payloadSize) { return }
                }
                $watch.Elapsed.TotalMilliseconds
            } "Explorer did not produce the expected $transfer tree."
            $observed = $started.AddMilliseconds($elapsed)
            $explorer.Refresh()
            $cpuMs = $explorer.TotalProcessorTime.TotalMilliseconds - $cpu
            foreach ($path in $paths) {
                if ((Get-FileHash -LiteralPath $path).Hash -ne $expectedHash) {
                    throw "Unexpected output hash for $path"
                }
            }
            $targetId = [TransferProbe]::Identity($paths[0])
            if (($targetId -eq $sourceId) -ne ($transfer -eq 'Move')) {
                throw "Unexpected file identity after $transfer."
            }
            if ((Get-Content -LiteralPath $paths[0] -Stream stdio-test -Raw) -ne 'native-transfer') {
                throw "Alternate data stream did not survive $transfer."
            }
            if ($transfer -eq 'Copy' -and [IO.Directory]::GetFiles($source).Count -ne $FileCount) {
                throw 'Copy changed the source tree.'
            }
            if ($clipboard -and $transfer -eq 'Move' -and
                [TransferProbe]::GetClipboardSequenceNumber() -ne $clipboardSequence) {
                $afterPaste = [TransferProbe]::GetClipboardSequenceNumber()
                if ([Windows.Forms.Clipboard]::GetDataObject().GetFormats().Count -ne 0 -or
                    [TransferProbe]::GetClipboardSequenceNumber() -ne $afterPaste) {
                    throw 'The clipboard changed during paste.'
                }
                $clipboardSequence = $afterPaste
            }
            $accounted = Wait-CopyAccounting $before $FileCount
            if ($labReady -and $accounted -ne $FileCount) { throw "Expected $FileCount native copies, saw $accounted." }
            [pscustomobject]@{
                Operation = $transfer
                EntryPoint = $(if ($clipboard) { 'ClipboardVerbs' } else { 'FolderAutomation' })
                FlatSelection = [bool]$FlatSelection
                Round = $round
                ExplorerPID = $owner
                StartedUtc = $started.ToString('o')
                ObservedUtc = $observed.ToString('o')
                SourcePath = $source
                DestinationPath = $output
                ExplorerPatcher = $(if ($patcher) { $patcher.FileVersionInfo.FileVersion })
                Files = $FileCount
                FirstFileIdentityPreserved = $targetId -eq $sourceId
                ObservedMs = [Math]::Round($elapsed, 2)
                ExplorerCpuMs = [Math]::Round($cpuMs, 2)
                PeakWorkingSetBytes = $explorer.PeakWorkingSet64
                PrivateBytes = $explorer.PrivateMemorySize64
                AccountedNativeCalls = $accounted
            }
        }
        [array]::Reverse($operations)
    }
} finally {
    $labCleanupError = $null
    if ($labReady) {
        try {
            $restored = [ExplorerProcessProbe]::LabCommand([IntPtr]$view.HWND, $owner, $labPath, 22, 'stdio.lab.copy-remove')
            if ($restored -or [BitConverter]::ToString([ExplorerProcessProbe]::ReadCode($owner, $copyCall, 7)) -ne $originalCopy) {
                throw "Could not restore native copy call, Win32 error $restored."
            }
        } catch { $labCleanupError = $_ }
    }
    $clipboardError = $null
    if ($clipboard) {
        try {
            if ($null -ne $clipboardSequence -and !$clipboard.Restore($clipboardSequence)) {
                Write-Verbose 'Clipboard changed after the test copy/cut and was left alone.'
            }
        } catch {
            $clipboardError = $_
        } finally { $clipboard.Dispose() }
    }
    if ($view) { $view.Quit() }
    if ($labPath -and [IO.Directory]::Exists($labDirectory)) {
        try {
            [IO.File]::Delete($labPath)
            [IO.Directory]::Delete($labDirectory)
        } catch [IO.IOException] {
            Write-Verbose "Isolated Explorer still has the lab image mapped: $labPath"
        } catch [UnauthorizedAccessException] {
            Write-Verbose "Isolated Explorer still has the lab image mapped: $labPath"
        }
    }
    if ([IO.Directory]::Exists($root)) {
        $resolved = (Resolve-Path -LiteralPath $root).ProviderPath
        if ([IO.Path]::GetDirectoryName($resolved) -ne $parent -or
            ([IO.File]::GetAttributes($resolved) -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Unexpected fixture location: $resolved"
        }
        [IO.Directory]::Delete($resolved, $true)
    }
    if ($clipboardError) { throw $clipboardError }
    if ($labCleanupError) { throw $labCleanupError }
}
