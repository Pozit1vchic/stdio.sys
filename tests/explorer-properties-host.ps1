param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [switch]$RequireExplorerPatcher
)

# Run with Windows PowerShell -STA on an interactive desktop.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @"
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class ExplorerProcessProbe
{
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

function Wait-For([scriptblock]$Read, [string]$ErrorMessage) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $value = & $Read
        if ($value) { return $value }
        Start-Sleep -Milliseconds 50
    } while ($watch.ElapsedMilliseconds -lt 7000)
    throw $ErrorMessage
}

$parent = (Resolve-Path -LiteralPath $BuildDirectory).ProviderPath
$root = Join-Path $parent ('stdio-host-' + [guid]::NewGuid().ToString('N'))
$name = 'stdio-properties-' + [guid]::NewGuid().ToString('N')
$child = Join-Path $root $name
$payload = Join-Path $child 'payload.bin'
$before = @(Get-Process explorer | Select-Object -ExpandProperty Id)
$desktop = [Windows.Automation.AutomationElement]::RootElement
$shell = New-Object -ComObject Shell.Application
$any = [Windows.Automation.Condition]::TrueCondition
$view = $null
$dialogHandle = [IntPtr]::Zero

try {
    [void][IO.Directory]::CreateDirectory($child)
    [IO.File]::WriteAllBytes($payload, [byte[]]::new(16384))
    Start-Process (Join-Path $env:windir 'explorer.exe') -ArgumentList "/separate,`"$root`""
    $view = Wait-For {
        $shell.Windows() | Where-Object { $_.Document.Folder.Self.Path -eq $root } | Select-Object -First 1
    } 'The isolated Explorer window did not open.'
    [uint32]$owner = 0
    [void][ExplorerProcessProbe]::GetWindowThreadProcessId([IntPtr]$view.HWND, [ref]$owner)
    if (!$owner -or $owner -in $before) {
        throw 'The test window belongs to an existing Explorer process.'
    }
    $explorer = Get-Process -Id $owner
    if ($explorer.Path -ne (Join-Path $env:windir 'explorer.exe')) {
        throw 'The test window does not belong to Windows Explorer.'
    }

    $item = $view.Document.Folder.ParseName($name)
    if (!$item) { throw 'The test folder is missing from the Explorer view.' }
    $item.InvokeVerb('properties')
    $dialog = Wait-For {
        $desktop.FindAll('Children', $any) | Where-Object { $_.Current.Name.Contains($name) } |
            Select-Object -First 1
    } 'Explorer did not open the properties window.'
    $dialogHandle = [IntPtr]$dialog.Current.NativeWindowHandle
    if ($dialog.Current.ProcessId -ne $owner) {
        throw 'Properties ran in a different process from the isolated Explorer window.'
    }

    $fields = Wait-For {
        $values = @{}
        foreach ($element in $dialog.FindAll('Descendants', $any)) {
            $current = $element.Current
            if ($current.AutomationId -in '13064', '13106', '13087' -and $current.Name -match '[0-9]') {
                $values[$current.AutomationId] = $current.Name
            }
        }
        if ($values.Count -eq 3 -and $values['13064'] -match '16[^0-9]384') { $values }
    } 'Explorer did not publish the expected 16384-byte size.'

    $explorer.Refresh()
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
    $expectedCall = 'E8-D0-AF-1A-00'
    $actualCall = [ExplorerProcessProbe]::ReadCode($owner,
        [IntPtr]::Add($image.BaseAddress, 0x35BF7), 5)
    if ([BitConverter]::ToString($actualCall) -ne $expectedCall) {
        throw 'The Explorer call no longer targets the expected helper.'
    }
    foreach ($range in @(@{ Name='WalkerCall'; Offset=0x35BF7; Length=12 },
                         @{ Name='StreamHelperEntry'; Offset=0x1E0BCC; Length=32 })) {
        $stock = [ExplorerProcessProbe]::ReadCode($PID,
            [IntPtr]::Add($local.BaseAddress, $range.Offset), $range.Length)
        $actual = [ExplorerProcessProbe]::ReadCode($owner,
            [IntPtr]::Add($image.BaseAddress, $range.Offset), $range.Length)
        if ([BitConverter]::ToString($stock) -ne [BitConverter]::ToString($actual)) {
            throw "$($range.Name) differs in the Explorer process."
        }
    }

    [pscustomobject]@{
        ExplorerPID = $owner
        ShellVersion = $image.FileVersionInfo.FileVersion
        ExplorerPatcher = $(if ($patcher) { $patcher.FileVersionInfo.FileVersion })
        Size = $fields['13064']
        SizeOnDisk = $fields['13106']
        Contains = $fields['13087']
        ComparedCodeBytes = 44
    }
} finally {
    if ($dialogHandle -ne [IntPtr]::Zero) {
        [void][ExplorerProcessProbe]::PostMessage($dialogHandle, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
    }
    if ($view) { $view.Quit() }
    [IO.File]::Delete($payload)
    if ([IO.Directory]::Exists($child)) { [IO.Directory]::Delete($child) }
    if ([IO.Directory]::Exists($root)) { [IO.Directory]::Delete($root) }
}
