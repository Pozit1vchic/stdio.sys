param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [string]$Tool,
    [string]$LabDll,
    [switch]$LabPassThrough,
    [switch]$CheckCancellation
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
    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate uint InstallLab(bool stockOnly, bool pauseFirst);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int CountLabHits();

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate void ResumeLab();

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate uint WaitForLabWorker();

    public sealed class Lab
    {
        public CountLabHits Hits;
        public CountLabHits Requests;
        public ResumeLab Resume;
        public WaitForLabWorker WaitForWorker;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadLibrary(string path);

    [DllImport("kernel32.dll", ExactSpelling = true, SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string name);

    public static Lab StartLab(string path, bool stockOnly, bool pauseFirst)
    {
        var module = LoadLibrary(path);
        if (module == IntPtr.Zero)
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        var installAddress = GetProcAddress(module, "ShellStreamLabInstall");
        var countAddress = GetProcAddress(module, "ShellStreamLabHits");
        var requestsAddress = GetProcAddress(module, "ShellStreamLabRequests");
        var resumeAddress = GetProcAddress(module, "ShellStreamLabResume");
        var waitAddress = GetProcAddress(module, "ShellStreamLabWaitForWorker");
        if (installAddress == IntPtr.Zero || countAddress == IntPtr.Zero ||
            requestsAddress == IntPtr.Zero || resumeAddress == IntPtr.Zero || waitAddress == IntPtr.Zero)
            throw new InvalidOperationException("Missing Shell experiment exports.");
        var install = Marshal.GetDelegateForFunctionPointer<InstallLab>(installAddress);
        var error = install(stockOnly, pauseFirst);
        if (error != 0)
            throw new System.ComponentModel.Win32Exception((int)error);
        return new Lab {
            Hits = Marshal.GetDelegateForFunctionPointer<CountLabHits>(countAddress),
            Requests = Marshal.GetDelegateForFunctionPointer<CountLabHits>(requestsAddress),
            Resume = Marshal.GetDelegateForFunctionPointer<ResumeLab>(resumeAddress),
            WaitForWorker = Marshal.GetDelegateForFunctionPointer<WaitForLabWorker>(waitAddress)
        };
    }

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

function Read-Properties([string]$Path, [int]$FixtureFiles = 0) {
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
        if ($CheckCancellation) {
            while (!$lab.Requests.Invoke() -and $watch.ElapsedMilliseconds -lt 5000) {
                Start-Sleep -Milliseconds 10
            }
            if ($lab.Requests.Invoke() -ne 1) { throw 'The first file request did not pause.' }
            $closeTime = [Diagnostics.Stopwatch]::StartNew()
            if (![FolderPropertiesProbe]::PostMessage($handle, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)) {
                throw 'Could not close the property sheet.'
            }
            if (!$thread.Join(2000)) { throw 'Closing the window waited for the paused file request.' }
            $closeMilliseconds = $closeTime.Elapsed.TotalMilliseconds
            $handle = [IntPtr]::Zero
            $lab.Resume.Invoke()
            if ($lab.WaitForWorker.Invoke() -ne 0) { throw 'The size worker did not exit after closing the window.' }
            $total = $lab.Requests.Invoke()
            if ($total -lt 1 -or $total -ge $FixtureFiles) {
                throw "Expected cancellation before the end of the traversal, observed $total of $FixtureFiles requests."
            }
            return [pscustomobject]@{
                FileCount = $FixtureFiles
                RequestsAfterClose = $total - 1
                CloseMilliseconds = [Math]::Round($closeMilliseconds, 2)
                Optimized = !$LabPassThrough
            }
        }
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
        if ($CheckCancellation) { $lab.Resume.Invoke() }
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
$lab = $null
if ($CheckCancellation -and !$LabDll) { throw 'Cancellation instrumentation requires -LabDll.' }
if ($LabDll) {
    $lab = [FolderPropertiesProbe]::StartLab((Resolve-Path -LiteralPath $LabDll).ProviderPath,
        $LabPassThrough, $CheckCancellation)
}
if ($CheckCancellation) {
    $fixture = Join-Path $parent ('stdio-cancel-' + [guid]::NewGuid().ToString('N'))
    try {
        [void][IO.Directory]::CreateDirectory($fixture)
        $fileCount = 1024
        for ($i = 0; $i -lt $fileCount; $i++) {
            [IO.File]::WriteAllBytes((Join-Path $fixture "$i.bin"), [byte[]]::new(8192))
        }
        Read-Properties $fixture $fileCount
    } finally {
        if ([IO.Directory]::Exists($fixture)) { [IO.Directory]::Delete($fixture, $true) }
    }
    return
}
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
        $hitsBefore = if ($lab) { $lab.Hits.Invoke() } else { 0 }
        $fields = Read-Properties $path
        $hits = if ($lab) { $lab.Hits.Invoke() - $hitsBefore } else { 0 }
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
        $expectedHits = if ($lab -and !$LabPassThrough) { $expectedFiles } else { 0 }
        if ($hits -ne $expectedHits) {
            throw "Unexpected number of optimized requests for ${case}: $hits"
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
            OptimizedRequests = $hits
        }
    } finally {
        if ($originalAcl) { Set-Acl -LiteralPath $nested -AclObject $originalAcl }
        if (Test-Path -LiteralPath $junction) { [IO.Directory]::Delete($junction) }
        if (Test-Path -LiteralPath $path) { [IO.Directory]::Delete($path, $true) }
    }
}
