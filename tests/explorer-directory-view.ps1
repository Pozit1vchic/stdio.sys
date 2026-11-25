param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [ValidateRange(1, 50000)][int]$FileCount = 10000,
    [ValidateRange(1, 30)][int]$Rounds = 5
)

# Run with Windows PowerShell -STA from an interactive desktop.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class ExplorerViewOwner
{
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
}
"@

function Find-TestWindow($Shell, [string]$Path) {
    foreach ($candidate in $Shell.Windows()) {
        try {
            if ($candidate.Document.Folder.Self.Path -eq $Path) { return $candidate }
        } catch [Runtime.InteropServices.COMException] {
            # A Shell window can exit while the collection is being enumerated.
        }
    }
}

function Wait-TestValue([scriptblock]$Read) {
    $timeout = [Diagnostics.Stopwatch]::StartNew()
    do {
        $value = & $Read
        if ($value) { return $value }
        Start-Sleep -Milliseconds 10
    } while ($timeout.ElapsedMilliseconds -lt 15000)
    throw 'The Explorer view did not reach the expected state.'
}

function New-Condition($Property, $Value) {
    New-Object Windows.Automation.PropertyCondition($Property, $Value)
}

$parent = (Resolve-Path -LiteralPath $BuildDirectory).ProviderPath
$root = Join-Path $parent ('stdio-view-' + [Guid]::NewGuid().ToString('N'))
$target = Join-Path $root 'large'
$control = Join-Path $root 'empty'
$shell = New-Object -ComObject Shell.Application
$scope = [Windows.Automation.TreeScope]::Descendants
$firstItem = New-Condition ([Windows.Automation.AutomationElement]::NameProperty) 'fixture-00000.bin'
$statusGroup = New-Condition ([Windows.Automation.AutomationElement]::AutomationIdProperty) 'System.StatusBarViewItemCount'
$statusValue = New-Condition ([Windows.Automation.AutomationElement]::AutomationIdProperty) 'PropertyValue'
$view = $null
try {
    [void][IO.Directory]::CreateDirectory($target)
    [void][IO.Directory]::CreateDirectory($control)
    $openProcesses = @(Get-Process explorer | Select-Object -ExpandProperty Id)
    Start-Process (Join-Path $env:windir 'explorer.exe') -ArgumentList "/separate,`"$control`""
    $view = Wait-TestValue { Find-TestWindow $shell $control }
    $window = [IntPtr]$view.HWND
    [uint32]$owner = 0
    [void][ExplorerViewOwner]::GetWindowThreadProcessId($window, [ref]$owner)
    if (!$owner -or $owner -in $openProcesses) { throw 'The test view was not isolated.' }
    $explorer = Get-Process -Id $owner
    $patcher = $explorer.Modules | Where-Object ModuleName -eq ExplorerPatcher.amd64.dll
    $data = [byte[]]@()
    for ($i = 0; $i -lt $FileCount; $i++) {
        [IO.File]::WriteAllBytes((Join-Path $target ('fixture-{0:d5}.bin' -f $i)), $data)
    }
    for ($round = 1; $round -le $Rounds; $round++) {
        [void](Wait-TestValue {
            !$view.Busy -and $view.Document.Folder.Self.Path -eq $control
        })
        $explorer.Refresh()
        $cpuStart = $explorer.TotalProcessorTime.TotalMilliseconds
        $navigationStart = [DateTime]::UtcNow
        $clock = [Diagnostics.Stopwatch]::StartNew()
        $view.Navigate2($target)
        $sample = @{ First = $null }
        $total = Wait-TestValue {
            if ($view.Document.Folder.Self.Path -ne $target) { return }
            $ui = [Windows.Automation.AutomationElement]::FromHandle($window)
            try {
                if ($null -eq $sample.First -and $ui.FindFirst($scope, $firstItem)) {
                    $sample.First = $clock.Elapsed.TotalMilliseconds
                }
                if ($null -eq $sample.First) { return }
                $group = $ui.FindFirst($scope, $statusGroup)
                if (!$group) { return }
                $value = $group.FindFirst($scope, $statusValue)
                if ($value -and ($value.Current.Name -replace '[^0-9]', '') -eq [string]$FileCount) {
                    return $clock.Elapsed.TotalMilliseconds
                }
            } catch [Windows.Automation.ElementNotAvailableException] {
                # Navigation can replace the active view during this sample.
            }
        }
        $explorer.Refresh()
        [pscustomobject]@{
            Round = $round
            ExplorerPID = $owner
            NavigationStartUtc = $navigationStart.ToString('o')
            ExplorerPatcher = $(if ($patcher) { $patcher.FileVersionInfo.FileVersion })
            Files = $FileCount
            FirstItemMs = [Math]::Round($sample.First, 2)
            FullCountMs = [Math]::Round($total, 2)
            ProcessCpuMs = [Math]::Round($explorer.TotalProcessorTime.TotalMilliseconds - $cpuStart, 2)
            PeakWorkingSetBytes = $explorer.PeakWorkingSet64
            PrivateBytes = $explorer.PrivateMemorySize64
        }
        $view.Navigate2($control)
    }
} finally {
    if ($view) { $view.Quit() }
    if ([IO.Directory]::Exists($target)) {
        for ($i = 0; $i -lt $FileCount; $i++) {
            [IO.File]::Delete((Join-Path $target ('fixture-{0:d5}.bin' -f $i)))
        }
        [IO.Directory]::Delete($target)
    }
    if ([IO.Directory]::Exists($control)) { [IO.Directory]::Delete($control) }
    if ([IO.Directory]::Exists($root)) { [IO.Directory]::Delete($root) }
}
