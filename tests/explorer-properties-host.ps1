param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [switch]$RequireExplorerPatcher,
    [string]$LabDll,
    [switch]$LabPassThrough,
    [switch]$LabFullWalk,
    [switch]$Junction,
    [switch]$DeniedDirectory,
    [switch]$CheckWalkCancellation,
    [switch]$CheckWalkProgress,
    [ValidateRange(0, 16)][int]$Depth = 0,
    [ValidateRange(1, 50000)][int]$FileCount = 1
)

# Run with Windows PowerShell -STA on an interactive desktop.
$ErrorActionPreference = 'Stop'
$expectFallback = $Junction -or $DeniedDirectory
if ($CheckWalkCancellation -and (!$LabDll -or !$LabFullWalk -or $LabPassThrough -or $expectFallback)) {
    throw 'Cancellation requires the active full walk with a regular directory.'
}
if ($CheckWalkProgress -and (!$LabDll -or !$LabFullWalk -or $LabPassThrough -or
    $expectFallback -or $CheckWalkCancellation -or $FileCount -lt 2)) {
    throw 'Progress requires the active full walk with at least two files.'
}
if (($Junction -and $DeniedDirectory) -or
    ($expectFallback -and (!$LabDll -or !$LabFullWalk -or $LabPassThrough))) {
    throw 'Select at most one fallback fixture, with the active full-walk experiment.'
}
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @"
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
"@

function Wait-For([scriptblock]$Read, [string]$ErrorMessage, [int]$PollMilliseconds = 50) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $value = & $Read
        if ($value) { return $value }
        Start-Sleep -Milliseconds $PollMilliseconds
    } while ($watch.ElapsedMilliseconds -lt 7000)
    throw $ErrorMessage
}

function Read-ByteCount([string]$Display) {
    if ($Display -match '\((?<exact>[^()]*)\)\s*$') { $Display = $Matches['exact'] }
    return [uint64]($Display -replace '[^0-9]', '')
}

function Test-ContainsFiles([string]$Display, [int]$Expected, [int]$Folders) {
    $numbers = [regex]::Matches($Display, '[0-9]+(?:[ ,\.\u00a0\u202f][0-9]{3})*')
    return $numbers.Count -eq 2 -and
        (Read-ByteCount $numbers[0].Value) -eq $Expected -and
        (Read-ByteCount $numbers[1].Value) -eq $Folders
}

function Find-PropertiesWindow {
    foreach ($element in $desktop.FindAll('Children', $dialogOwner)) {
        try {
            if (([string]$element.Current.Name).Contains($name)) { return $element }
        } catch [Windows.Automation.ElementNotAvailableException] {
            # A window can close between enumeration and the property read.
        }
    }
}

$parent = (Resolve-Path -LiteralPath $BuildDirectory).ProviderPath
$root = Join-Path $parent ('stdio-host-' + [guid]::NewGuid().ToString('N'))
$name = 'stdio-properties-' + [guid]::NewGuid().ToString('N')
$child = Join-Path $root $name
$outside = Join-Path $root 'junction-target'
$link = Join-Path $child 'junction'
$sentinel = Join-Path $outside 'sentinel.bin'
$denied = Join-Path $child 'denied'
$deniedFile = Join-Path $denied 'excluded.bin'
$originalAcl = $null
$expectedFolders = $Depth + [int][bool]$DeniedDirectory
$fixtureDirectories = [Collections.Generic.List[string]]::new()
$fileDirectory = $child
$expectedBytes = [uint64]16384 * $FileCount
$before = @(Get-Process explorer | Select-Object -ExpandProperty Id)
$desktop = [Windows.Automation.AutomationElement]::RootElement
$shell = New-Object -ComObject Shell.Application
$any = [Windows.Automation.Condition]::TrueCondition
$view = $null
$dialogHandle = [IntPtr]::Zero
$dll = $null
$labDirectory = $null

try {
    [void][IO.Directory]::CreateDirectory($child)
    for ($level = 0; $level -lt $Depth; $level++) {
        $fileDirectory = Join-Path $fileDirectory "level-$level"
        [void][IO.Directory]::CreateDirectory($fileDirectory)
        $fixtureDirectories.Add($fileDirectory)
    }
    $payload = [byte[]]::new(16384)
    for ($i = 0; $i -lt $FileCount; $i++) {
        [IO.File]::WriteAllBytes((Join-Path $fileDirectory "$i.bin"), $payload)
    }
    if ($Junction) {
        [void][IO.Directory]::CreateDirectory($outside)
        [IO.File]::WriteAllBytes($sentinel, $payload)
        New-Item -ItemType Junction -Path $link -Target $outside | Out-Null
    }
    if ($DeniedDirectory) {
        [void][IO.Directory]::CreateDirectory($denied)
        [IO.File]::WriteAllBytes($deniedFile, $payload)
        $originalAcl = Get-Acl -LiteralPath $denied
        $acl = Get-Acl -LiteralPath $denied
        $identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
            $identity, 'ListDirectory', 'Deny'))
        Set-Acl -LiteralPath $denied -AclObject $acl
    }
    Start-Process (Join-Path $env:windir 'explorer.exe') -ArgumentList "/separate,`"$root`""
    $view = Wait-For {
        $shell.Windows() | Where-Object { $_.Document.Folder.Self.Path -eq $root } | Select-Object -First 1
    } 'The isolated Explorer window did not open.'
    [uint32]$owner = 0
    [void][ExplorerProcessProbe]::GetWindowThreadProcessId([IntPtr]$view.HWND, [ref]$owner)
    if (!$owner -or $owner -in $before) {
        throw 'The test window belongs to an existing Explorer process.'
    }
    $dialogOwner = New-Object Windows.Automation.PropertyCondition(
        [Windows.Automation.AutomationElement]::ProcessIdProperty, [int]$owner)
    $explorer = Get-Process -Id $owner
    if ($explorer.Path -ne (Join-Path $env:windir 'explorer.exe')) {
        throw 'The test window does not belong to Windows Explorer.'
    }

    $window = [IntPtr]$view.HWND
    if ($LabDll) {
        $source = (Resolve-Path -LiteralPath $LabDll).ProviderPath
        # A window host can outlive its last window. Keep the pinned test image
        # separate from the build output so that it does not block relinking.
        $labDirectory = Join-Path $parent ('stdio-lab-image-' + [guid]::NewGuid().ToString('N'))
        [void][IO.Directory]::CreateDirectory($labDirectory)
        $dll = Join-Path $labDirectory 'stdio_shell_lab.dll'
        [IO.File]::Copy($source, $dll)
        $command = if ($LabFullWalk) { 5 } else { 1 }
        if ($LabPassThrough) { $command++ }
        if ($CheckWalkCancellation) { $command = 8 }
        if ($CheckWalkProgress) { $command = 12 }
        $result = [ExplorerProcessProbe]::LabCommand($window, $owner, $dll, $command, 'stdio.lab.install')
        if ($result) { throw "The Explorer experiment refused to install, Win32 error $result." }
    }

    $item = $view.Document.Folder.ParseName($name)
    if (!$item) { throw 'The test folder is missing from the Explorer view.' }
    $baselineCpu = $explorer.TotalProcessorTime.TotalMilliseconds
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $item.InvokeVerb('properties')
    $dialog = Wait-For { Find-PropertiesWindow } 'Explorer did not open the properties window.' 10
    $dialogHandle = [IntPtr]$dialog.Current.NativeWindowHandle
    if ($dialog.Current.ProcessId -ne $owner) {
        throw 'Properties ran in a different process from the isolated Explorer window.'
    }

    if ($CheckWalkCancellation) {
        [void](Wait-For {
            [ExplorerProcessProbe]::LabCommand($window, $owner, $dll, 9, 'stdio.lab.paused') -eq 1
        } 'The recursive walk did not reach its first entry.')
        $closeClock = [Diagnostics.Stopwatch]::StartNew()
        if (![ExplorerProcessProbe]::PostMessage($dialogHandle, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)) {
            throw 'Could not close the paused properties dialog.'
        }
        [void](Wait-For { !(Find-PropertiesWindow) } 'The dialog did not close before resuming the walk.' 10)
        $closeMs = $closeClock.Elapsed.TotalMilliseconds
        $dialogHandle = [IntPtr]::Zero
        [void][ExplorerProcessProbe]::LabCommand($window, $owner, $dll, 10, 'stdio.lab.resumed')
        [void](Wait-For {
            [ExplorerProcessProbe]::LabCommand($window, $owner, $dll, 11, 'stdio.lab.cancelled') -eq 1
        } 'The walk did not report cancellation.' 10)
        $hits = [ExplorerProcessProbe]::LabCommand($window, $owner, $dll, 3, 'stdio.lab.hits')
        $fallbacks = [ExplorerProcessProbe]::LabCommand($window, $owner, $dll, 7, 'stdio.lab.walk-fallbacks')
        if ($hits -or $fallbacks) { throw 'The cancelled walk committed results or called the stock walk.' }
        [pscustomobject]@{
            ExplorerPID = $owner
            Files = $FileCount
            CloseMilliseconds = [Math]::Round($closeMs, 2)
            Cancelled = 1
            OptimizedRequests = $hits
            WalkFallbacks = $fallbacks
        }
        return
    }

    $partialBytes = $null
    if ($CheckWalkProgress) {
        [void](Wait-For {
            [ExplorerProcessProbe]::LabCommand($window, $owner, $dll, 9, 'stdio.lab.paused') -eq 1
        } 'The walk did not pause after publishing progress.')
        $partialBytes = Wait-For {
            foreach ($element in $dialog.FindAll('Descendants', $any)) {
                $current = $element.Current
                if ($current.AutomationId -eq '13064' -and $current.Name -match '[0-9]') {
                    $bytes = Read-ByteCount $current.Name
                    if ($bytes -gt 0 -and $bytes -lt $expectedBytes) { return $bytes }
                }
            }
        } 'The native window did not display a partial result.' 10
        [void][ExplorerProcessProbe]::LabCommand($window, $owner, $dll, 10, 'stdio.lab.resumed')
    }
    $fields = Wait-For {
        $values = @{}
        foreach ($element in $dialog.FindAll('Descendants', $any)) {
            $current = $element.Current
            if ($current.AutomationId -in '13064', '13106', '13087' -and $current.Name -match '[0-9]') {
                $values[$current.AutomationId] = $current.Name
            }
        }
        if ($values.Count -eq 3 -and
            (Read-ByteCount $values['13064']) -eq $expectedBytes -and
            (Read-ByteCount $values['13106']) -eq $expectedBytes -and
            (Test-ContainsFiles $values['13087'] $FileCount $expectedFolders)) { $values }
    } "Explorer did not publish the expected totals for $FileCount files." 10
    $elapsed = $watch.Elapsed.TotalMilliseconds

    $explorer.Refresh()
    $cpu = $explorer.TotalProcessorTime.TotalMilliseconds - $baselineCpu
    $peakWorkingSet = $explorer.PeakWorkingSet64
    $privateBytes = $explorer.PrivateMemorySize64
    $image = $explorer.Modules | Where-Object { $_.ModuleName -eq 'shell32.dll' } | Select-Object -First 1
    $patcher = $explorer.Modules | Where-Object { $_.ModuleName -eq 'ExplorerPatcher.amd64.dll' } |
        Select-Object -First 1
    if (!$image -or ($RequireExplorerPatcher -and !$patcher)) {
        throw 'The expected Shell or ExplorerPatcher module was not loaded.'
    }

    # These offsets apply only to the same Shell image as the isolated in-process lab.
    $digest = (Get-FileHash -Algorithm SHA256 -LiteralPath $image.FileName).Hash
    if ($digest -ne 'C83CC7EE08E1B3380E53F8A7B00A0EA84ACD2BBDDE240223F84BD32AE868A09F') {
        throw "Unsupported Shell image $($image.FileVersionInfo.FileVersion)"
    }
    $local = (Get-Process -Id $PID).Modules | Where-Object { $_.ModuleName -eq 'shell32.dll' } |
        Select-Object -First 1
    if (!$local -or $local.FileName -ne $image.FileName) {
        throw 'The control and Explorer processes have different Shell images.'
    }
    $expectedCall = if ($LabFullWalk) { 'E8-1D-D6-0F-00' } else { 'E8-D0-AF-1A-00' }
    $callOffset = if ($LabFullWalk) { 0x34D816 } else { 0x35BF7 }
    $helperOffset = if ($LabFullWalk) { 0x44AE38 } else { 0x1E0BCC }
    $actualCall = [ExplorerProcessProbe]::ReadCode($owner,
        [IntPtr]::Add($image.BaseAddress, $callOffset), 5)
    if (!$dll -and [BitConverter]::ToString($actualCall) -ne $expectedCall) {
        throw 'The Explorer call no longer targets the expected helper.'
    }
    if ($dll -and ($actualCall[0] -ne 0xE8 -or [BitConverter]::ToString($actualCall) -eq $expectedCall)) {
        throw 'The requested Shell call was not redirected.'
    }
    foreach ($range in @(@{ Name='WalkerCall'; Offset=$callOffset; Length=12 },
                         @{ Name='OriginalHelperEntry'; Offset=$helperOffset; Length=32 })) {
        if ($dll -and $range.Name -eq 'WalkerCall') { continue }
        $stock = [ExplorerProcessProbe]::ReadCode($PID,
            [IntPtr]::Add($local.BaseAddress, $range.Offset), $range.Length)
        $actual = [ExplorerProcessProbe]::ReadCode($owner,
            [IntPtr]::Add($image.BaseAddress, $range.Offset), $range.Length)
        if ([BitConverter]::ToString($stock) -ne [BitConverter]::ToString($actual)) {
            throw "$($range.Name) differs in the Explorer process."
        }
    }

    $hits = if ($dll) { [ExplorerProcessProbe]::LabCommand($window, $owner, $dll, 3, 'stdio.lab.hits') } else { 0 }
    $expectedHits = if ($dll -and !$LabPassThrough) {
        if ($expectFallback) { 0 } elseif ($LabFullWalk) { 1 } else { $FileCount }
    } else { 0 }
    if ($hits -ne $expectedHits) { throw "Expected $expectedHits optimized requests, found $hits." }
    $fallbacks = if ($dll -and $LabFullWalk) {
        [ExplorerProcessProbe]::LabCommand($window, $owner, $dll, 7, 'stdio.lab.walk-fallbacks')
    } else { 0 }
    if ($fallbacks -ne [int]$expectFallback) {
        throw "Unexpected full-walk fallback count: $fallbacks."
    }
    if ($dll) {
        [void][ExplorerProcessProbe]::LabCommand($window, $owner, $dll, 4, 'stdio.lab.disabled')
        if (![ExplorerProcessProbe]::PostMessage($dialogHandle, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)) {
            throw 'Could not close the first properties dialog.'
        }
        [void](Wait-For {
            !(Find-PropertiesWindow)
        } 'The first properties dialog did not close.')
        $dialogHandle = [IntPtr]::Zero
        $item.InvokeVerb('properties')
        $again = Wait-For { Find-PropertiesWindow } 'Explorer did not reopen the properties dialog.'
        $dialogHandle = [IntPtr]$again.Current.NativeWindowHandle
        if ($again.Current.ProcessId -ne $owner) {
            throw 'The reopened dialog belongs to a different process.'
        }
        [void](Wait-For {
            $matching = @($again.FindAll('Descendants', $any) | Where-Object {
                $current = $_.Current
                $fields.ContainsKey($current.AutomationId) -and
                    $fields[$current.AutomationId] -eq $current.Name
            })
            $matching.Count -eq 3
        } 'The reopened properties dialog did not return the original values.')
        $after = [ExplorerProcessProbe]::LabCommand($window, $owner, $dll, 3, 'stdio.lab.hits')
        if ($after -ne $hits) { throw 'The disabled path still used the stdio implementation.' }
    }

    [pscustomobject]@{
        ExplorerPID = $owner
        ShellVersion = $image.FileVersionInfo.FileVersion
        ExplorerPatcher = $(if ($patcher) { $patcher.FileVersionInfo.FileVersion })
        Size = $fields['13064']
        SizeOnDisk = $fields['13106']
        Contains = $fields['13087']
        ComparedCodeBytes = $(if ($dll) { 32 } else { 44 })
        OptimizedRequests = $hits
        WalkFallbacks = $fallbacks
        PartialBytes = $partialBytes
        VerifiedDisable = [bool]$dll
        FileCount = $FileCount
        Depth = $Depth
        FinalFieldsMilliseconds = [Math]::Round($elapsed, 2)
        ProcessCpuMilliseconds = [Math]::Round($cpu, 2)
        ProcessPeakWorkingSetBytes = $peakWorkingSet
        ProcessPrivateBytes = $privateBytes
    }
} finally {
    if ($dll -and $view) {
        try {
            [void][ExplorerProcessProbe]::LabCommand($window, $owner, $dll, 4, 'stdio.lab.disabled')
            if ($CheckWalkCancellation -or $CheckWalkProgress) {
                [void][ExplorerProcessProbe]::LabCommand($window, $owner, $dll, 10, 'stdio.lab.resumed')
            }
        }
        catch { Write-Warning "Could not disable the experimental path: $_" }
    }
    if ($dialogHandle -ne [IntPtr]::Zero) {
        [void][ExplorerProcessProbe]::PostMessage($dialogHandle, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
    }
    if ($view) { $view.Quit() }
    if ($originalAcl) { Set-Acl -LiteralPath $denied -AclObject $originalAcl }
    if ([IO.Directory]::Exists($denied)) {
        [IO.File]::Delete($deniedFile)
        [IO.Directory]::Delete($denied)
    }
    if ([IO.Directory]::Exists($link)) { [IO.Directory]::Delete($link) }
    if ([IO.Directory]::Exists($outside)) {
        [IO.File]::Delete($sentinel)
        [IO.Directory]::Delete($outside)
    }
    for ($i = 0; $i -lt $FileCount; $i++) {
        [IO.File]::Delete((Join-Path $fileDirectory "$i.bin"))
    }
    for ($level = $fixtureDirectories.Count - 1; $level -ge 0; $level--) {
        [IO.Directory]::Delete($fixtureDirectories[$level])
    }
    if ([IO.Directory]::Exists($child)) { [IO.Directory]::Delete($child) }
    if ([IO.Directory]::Exists($root)) { [IO.Directory]::Delete($root) }
    if ($labDirectory) {
        try {
            [IO.File]::Delete($dll)
            [IO.Directory]::Delete($labDirectory)
        } catch [IO.IOException] {
            Write-Verbose "Test image is still mapped by the isolated Explorer host: $dll"
        } catch [UnauthorizedAccessException] {
            Write-Verbose "Test image is still mapped by the isolated Explorer host: $dll"
        }
    }
}
