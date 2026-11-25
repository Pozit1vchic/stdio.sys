param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [string]$Tool
)

# Run with Windows PowerShell -STA from an interactive desktop on local NTFS.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class FolderPropertiesProbe
{
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern bool SHObjectProperties(IntPtr owner, uint kind, string path, string page);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    private delegate bool WindowCallback(IntPtr window, IntPtr unused);

    [DllImport("user32.dll")]
    private static extern bool EnumChildWindows(IntPtr parent, WindowCallback callback, IntPtr unused);

    [DllImport("user32.dll")]
    private static extern int GetDlgCtrlID(IntPtr window);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr window, StringBuilder text, int capacity);

    public static Dictionary<int, string> ReadFields(IntPtr window)
    {
        var fields = new Dictionary<int, string>();
        EnumChildWindows(window, (control, unused) => {
            int id = GetDlgCtrlID(control);
            if (id == 13064 || id == 13106 || id == 13087) {
                var text = new StringBuilder(512);
                if (GetWindowText(control, text, text.Capacity) != 0)
                    fields[id] = text.ToString();
            }
            return true;
        }, IntPtr.Zero);
        return fields;
    }

    public static Thread Open(string path)
    {
        var thread = new Thread(() => SHObjectProperties(IntPtr.Zero, 2, path, null));
        thread.SetApartmentState(ApartmentState.STA);
        thread.IsBackground = true;
        thread.Start();
        return thread;
    }
}
"@

function Read-Properties([string]$Path) {
    $name = Split-Path -Leaf $Path
    $process = New-Object Windows.Automation.PropertyCondition(
        [Windows.Automation.AutomationElement]::ProcessIdProperty, $PID)
    $desktop = [Windows.Automation.AutomationElement]::RootElement
    $thread = [FolderPropertiesProbe]::Open($Path)
    $window = $null
    $handle = [IntPtr]::Zero
    try {
        $watch = [Diagnostics.Stopwatch]::StartNew()
        do {
            $window = $desktop.FindAll('Children', $process) |
                Where-Object { $_.Current.Name.Contains($name) } | Select-Object -First 1
            if (!$window) { Start-Sleep -Milliseconds 50 }
        } until ($window -or $watch.ElapsedMilliseconds -ge 5000)
        if (!$window) { throw "No native property sheet appeared for $Path" }
        $handle = [IntPtr]$window.Current.NativeWindowHandle
        $ids = 13064, 13106, 13087
        $before = $null
        $stable = 0
        do {
            $values = [FolderPropertiesProbe]::ReadFields($handle)
            $sample = ($ids | ForEach-Object { $values[$_] }) -join '|'
            if ($values.Count -eq $ids.Count -and $sample -eq $before) { $stable++ } else { $stable = 0 }
            if ($stable -ge 4) { return $values }
            $before = $sample
            Start-Sleep -Milliseconds 75
        } while ($watch.ElapsedMilliseconds -lt 5000)
        throw "Native property fields did not settle for $Path"
    } finally {
        if ($handle -ne [IntPtr]::Zero) {
            [void][FolderPropertiesProbe]::PostMessage($handle, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
        }
        if (!$thread.Join(5000)) { throw "Property sheet did not exit for $Path" }
    }
}

function Read-ByteCount([string]$Text) {
    # Explorer displays the exact byte count in parentheses when the leading value is rounded.
    if ($Text -match '\((?<exact>[^()]*)\)\s*$') { $Text = $Matches['exact'] }
    return [uint64]($Text -replace '[^0-9]', '')
}

$parent = (Resolve-Path -LiteralPath $BuildDirectory).ProviderPath
$cases = 'plain', 'hardlink', 'sparse', 'junction', 'fileStream', 'directoryStream', 'accessDenied'
$ordinaryAllocation = $null
foreach ($case in $cases) {
    $name = 'stdio-props-' + [guid]::NewGuid().ToString('N')
    $path = Join-Path $parent $name
    $nested = Join-Path $path 'nested'
    $junction = Join-Path $path 'link'
    $first = Join-Path $path 'first.bin'
    $second = Join-Path $nested 'second.bin'
    $originalAcl = $null
    try {
        [void][IO.Directory]::CreateDirectory($nested)
        [IO.File]::WriteAllBytes($first, [byte[]]::new(17))
        [IO.File]::WriteAllBytes($second, [byte[]]::new(20480))
        switch ($case) {
            hardlink { New-Item -ItemType HardLink -Path (Join-Path $path 'alias.bin') -Target $second | Out-Null }
            sparse {
                $sparse = Join-Path $path 'hole.bin'
                & fsutil.exe file createnew $sparse 8388608 | Out-Null
                if ($LASTEXITCODE) { throw 'Could not create sparse fixture.' }
                & fsutil.exe sparse setflag $sparse | Out-Null
                if ($LASTEXITCODE) { throw 'Could not set sparse flag.' }
                & fsutil.exe sparse setrange $sparse 0 8388608 | Out-Null
                if ($LASTEXITCODE) { throw 'Could not punch sparse range.' }
            }
            junction { New-Item -ItemType Junction -Path $junction -Target $nested | Out-Null }
            fileStream { Set-Content -LiteralPath $first -Stream payload -Value ('z' * 8000) -NoNewline }
            directoryStream { Set-Content -LiteralPath $nested -Stream payload -Value ('y' * 12000) -NoNewline }
            accessDenied {
                $identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
                $originalAcl = Get-Acl -LiteralPath $nested
                $acl = Get-Acl -LiteralPath $nested
                $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
                    $identity, 'ListDirectory', 'Deny'))
                Set-Acl -LiteralPath $nested -AclObject $acl
                try {
                    [void][IO.Directory]::GetFiles($nested)
                    throw 'Test ACL did not deny listing the nested directory.'
                } catch {
                    if ($_.Exception.InnerException -isnot [UnauthorizedAccessException]) { throw }
                }
            }
        }
        $fields = Read-Properties $path
        $logical = Read-ByteCount $fields[13064]
        $allocated = Read-ByteCount $fields[13106]
        $expectedBytes = switch ($case) {
            hardlink { 40977 }
            sparse { 8409105 }
            accessDenied { 17 }
            default { 20497 }
        }
        $expectedFiles = switch ($case) {
            hardlink { 3 }
            sparse { 3 }
            accessDenied { 1 }
            default { 2 }
        }
        $counts = @([regex]::Matches($fields[13087], '[0-9]+') | ForEach-Object { [int]$_.Value })
        if ($logical -ne $expectedBytes -or ($counts -join ',') -ne "$expectedFiles,1") {
            throw "Unexpected native totals for ${case}: $logical bytes, $($fields[13087])"
        }
        switch ($case) {
            plain { $ordinaryAllocation = $allocated }
            fileStream {
                if ($allocated -le $ordinaryAllocation) { throw 'File stream did not increase allocation.' }
            }
            hardlink {
                if ($allocated -le $ordinaryAllocation) { throw 'Hardlink did not increase allocation.' }
            }
            accessDenied {
                if ($allocated -ge $ordinaryAllocation) { throw 'Shell included the denied file allocation.' }
            }
            default {
                if ($allocated -ne $ordinaryAllocation) { throw "Unexpected native allocation for $case" }
            }
        }
        $engine = $null
        if ($Tool) {
            $engine = & $Tool size $path
            # The CLI exit status is 1 for partial traversal, with the Win32 error on stderr.
            $expectedExit = if ($case -eq 'accessDenied') { 1 } else { 0 }
            if ($LASTEXITCODE -ne $expectedExit) { throw "Unexpected engine exit code for $case" }
        }
        [pscustomobject]@{
            Case = $case
            ShellSize = $fields[13064]
            ShellSizeOnDisk = $fields[13106]
            ShellContains = $fields[13087]
            Engine = $engine
        }
    } finally {
        if ($originalAcl) { Set-Acl -LiteralPath $nested -AclObject $originalAcl }
        if (Test-Path -LiteralPath $junction) { [IO.Directory]::Delete($junction) }
        if (Test-Path -LiteralPath $path) { [IO.Directory]::Delete($path, $true) }
    }
}
