param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [ValidateRange(1, 4096)][int]$FileCount = 256,
    [ValidateRange(1, 20)][int]$Rounds = 5,
    [switch]$Recycle
)

# Run with Windows PowerShell -STA on an interactive desktop and local NTFS.
$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class TransferProbe
{
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetFileInformationByHandleEx(SafeFileHandle file, int infoClass,
        [Out] byte[] info, uint size);

    public static string Identity(string path)
    {
        using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read,
            FileShare.ReadWrite | FileShare.Delete))
        {
            var identity = new byte[24]; // FILE_ID_INFO: volume serial followed by FILE_ID_128.
            if (!GetFileInformationByHandleEx(stream.SafeFileHandle, 18, identity, 24))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            return BitConverter.ToString(identity);
        }
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

$parent = (Resolve-Path -LiteralPath $BuildDirectory).ProviderPath
$root = Join-Path $parent ('stdio-transfer-' + [guid]::NewGuid().ToString('N'))
$target = Join-Path $root 'destination'
$recycleName = 'stdio-recycle-' + [guid]::NewGuid().ToString('N')
$recyclePath = Join-Path $target $recycleName
$shell = New-Object -ComObject Shell.Application
$view = $null
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
    $patcher = $explorer.Modules | Where-Object ModuleName -eq ExplorerPatcher.amd64.dll
    $destination = $view.Document.Folder
    if ($Recycle) {
        $bin = $shell.NameSpace(10)
        [void][IO.Directory]::CreateDirectory($recyclePath)
        $fixture = Join-Path $recyclePath 'payload.bin'
        [IO.File]::WriteAllBytes($fixture, $bytes)
        Set-Content -LiteralPath $fixture -Stream stdio-test -Value 'native-recycle' -NoNewline
        $identity = [TransferProbe]::Identity($fixture)
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
        if ([TransferProbe]::Identity($fixture) -ne $identity -or
            (Get-FileHash -LiteralPath $fixture).Hash -ne $hash -or
            (Get-Content -LiteralPath $fixture -Stream stdio-test -Raw) -ne 'native-recycle') {
            throw 'The restored fixture differs from the original.'
        }
        [pscustomobject]@{
            Operation = 'RecycleAndRestore'
            ExplorerPID = $owner
            ExplorerPatcher = $(if ($patcher) { $patcher.FileVersionInfo.FileVersion })
            StartedUtc = $started.ToString('o')
            RecycleMs = [Math]::Round($recycleMs, 2)
            RestoreMs = [Math]::Round($restoreMs, 2)
            IdentityPreserved = $true
        }
        return
    }
    $operations = 'Copy', 'Move'
    for ($round = 1; $round -le $Rounds; $round++) {
        foreach ($operation in $operations) {
            $source = Join-Path $root "$operation-$round"
            $output = Join-Path $target "$operation-$round"
            [void][IO.Directory]::CreateDirectory($source)
            $paths = for ($i = 0; $i -lt $FileCount; $i++) {
                $name = '{0:d4}.bin' -f $i
                [IO.File]::WriteAllBytes((Join-Path $source $name), $bytes)
                Join-Path $output $name
            }
            $firstSource = Join-Path $source '0000.bin'
            $sourceId = [TransferProbe]::Identity($firstSource)
            $expectedHash = (Get-FileHash -LiteralPath $firstSource).Hash
            Set-Content -LiteralPath $firstSource -Stream stdio-test -Value 'native-transfer' -NoNewline
            $explorer.Refresh()
            $cpu = $explorer.TotalProcessorTime.TotalMilliseconds
            $started = [DateTime]::UtcNow
            $watch = [Diagnostics.Stopwatch]::StartNew()
            if ($operation -eq 'Copy') { $destination.CopyHere($source) }
            else { $destination.MoveHere($source) }

            $elapsed = Wait-Observed {
                if ($operation -eq 'Move' -and [IO.Directory]::Exists($source)) { return }
                foreach ($path in $paths) {
                    $file = [IO.FileInfo]::new($path)
                    if (!$file.Exists -or $file.Length -ne $payloadSize) { return }
                }
                $watch.Elapsed.TotalMilliseconds
            } "Explorer did not produce the expected $operation tree."
            $explorer.Refresh()
            $cpuMs = $explorer.TotalProcessorTime.TotalMilliseconds - $cpu
            foreach ($path in $paths) {
                if ((Get-FileHash -LiteralPath $path).Hash -ne $expectedHash) {
                    throw "Unexpected output hash for $path"
                }
            }
            $targetId = [TransferProbe]::Identity($paths[0])
            if (($targetId -eq $sourceId) -ne ($operation -eq 'Move')) {
                throw "Unexpected file identity after $operation."
            }
            if ((Get-Content -LiteralPath $paths[0] -Stream stdio-test -Raw) -ne 'native-transfer') {
                throw "Alternate data stream did not survive $operation."
            }
            if ($operation -eq 'Copy' -and [IO.Directory]::GetFiles($source).Count -ne $FileCount) {
                throw 'Copy changed the source tree.'
            }
            [pscustomobject]@{
                Operation = $operation
                Round = $round
                ExplorerPID = $owner
                StartedUtc = $started.ToString('o')
                ExplorerPatcher = $(if ($patcher) { $patcher.FileVersionInfo.FileVersion })
                Files = $FileCount
                FirstFileIdentityPreserved = $targetId -eq $sourceId
                ObservedMs = [Math]::Round($elapsed, 2)
                ExplorerCpuMs = [Math]::Round($cpuMs, 2)
                PeakWorkingSetBytes = $explorer.PeakWorkingSet64
                PrivateBytes = $explorer.PrivateMemorySize64
            }
        }
        [array]::Reverse($operations)
    }
} finally {
    if ($view) { $view.Quit() }
    if ([IO.Directory]::Exists($root)) {
        $resolved = (Resolve-Path -LiteralPath $root).ProviderPath
        if ([IO.Path]::GetDirectoryName($resolved) -ne $parent -or
            ([IO.File]::GetAttributes($resolved) -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Unexpected fixture location: $resolved"
        }
        [IO.Directory]::Delete($resolved, $true)
    }
}
