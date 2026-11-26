param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [ValidateRange(1, 50000)][int]$FileCount = 10000,
    [ValidateRange(1, 30)][int]$Rounds = 5,
    [switch]$Search,
    [switch]$SearchRename,
    [ValidateSet('FileSystem', 'Shell')][string]$RenameMethod = 'FileSystem',
    [string]$LabSearchDll,
    [switch]$CheckSearchFallback
)

# Run with Windows PowerShell -STA from an interactive desktop.
$ErrorActionPreference = 'Stop'
if ($SearchRename) { $Search = $true }
if ($CheckSearchFallback -and !$LabSearchDll) { throw 'Fallback requires the search lab.' }
if ($LabSearchDll) {
    if ($SearchRename) { throw 'Use a separate run for rename and the search timer.' }
    if (!$CheckSearchFallback -and $Rounds % 2) { throw 'Use an even number of paired search rounds.' }
    $Search = $true
    Add-Type -Path (Join-Path $PSScriptRoot 'explorer-process-probe.cs')
}
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @"
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class ExplorerViewOwner
{
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);

    [StructLayout(LayoutKind.Sequential)]
    public struct IoCounters
    {
        public ulong Reads, Writes, Other, ReadBytes, WriteBytes, OtherBytes;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetProcessIoCounters(IntPtr process, out IoCounters counters);

    public static IoCounters ReadIo(IntPtr process)
    {
        IoCounters counters;
        if (!GetProcessIoCounters(process, out counters))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        return counters;
    }
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
$originalName = 'fixture-00000.bin'
$renamedName = 'fixture-00000-renamed.bin'
$firstItem = New-Condition ([Windows.Automation.AutomationElement]::NameProperty) $originalName
$renamedItem = New-Condition ([Windows.Automation.AutomationElement]::NameProperty) $renamedName
$statusGroup = New-Condition ([Windows.Automation.AutomationElement]::AutomationIdProperty) 'System.StatusBarViewItemCount'
$statusValue = New-Condition ([Windows.Automation.AutomationElement]::AutomationIdProperty) 'PropertyValue'
$searchEdit = New-Condition ([Windows.Automation.AutomationElement]::AutomationIdProperty) 'SearchEditBox'
$listPane = New-Condition ([Windows.Automation.AutomationElement]::AutomationIdProperty) 'listview'
$expectedCount = if ($Search) { 1 } else { $FileCount }
$view = $null
$labReady = $false
$searchText = if ($LabSearchDll -and !$CheckSearchFallback) {
    'System.FileName:="fixture-00000.bin"'
} else {
    'fixture-00000'
}

function Open-SearchBox {
    $view.Navigate2($target)
    $box = Wait-TestValue {
        if ($view.Busy -or $view.Document.Folder.Self.Path -ne $target) { return }
        $ui = [Windows.Automation.AutomationElement]::FromHandle($window)
        $group = $ui.FindFirst($scope, $statusGroup)
        if (!$group) { return }
        $value = $group.FindFirst($scope, $statusValue)
        if ($value -and ($value.Current.Name -replace '[^0-9]', '') -eq [string]$directoryCount) {
            $ui.FindFirst($scope, $searchEdit)
        }
    }
    $box.GetCurrentPattern([Windows.Automation.ValuePattern]::Pattern)
}

function Read-RenameResult {
    $ui = [Windows.Automation.AutomationElement]::FromHandle($window)
    try {
        $list = $ui.FindFirst($scope, $listPane)
        if ($list) {
            @{
                Original = [bool]$list.FindFirst($scope, $firstItem)
                Renamed = [bool]$list.FindFirst($scope, $renamedItem)
            }
        }
    } catch [Windows.Automation.ElementNotAvailableException] {
        # The view can replace a result while this sample is in progress.
    }
}

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
    if ($LabSearchDll) {
        if ($explorer.Path -ne (Join-Path $env:windir 'explorer.exe')) {
            throw 'The test view does not belong to Windows Explorer.'
        }
        $labDirectory = Join-Path $root 'lab'
        [void][IO.Directory]::CreateDirectory($labDirectory)
        $labPath = Join-Path $labDirectory 'stdio_shell_lab.dll'
        [IO.File]::Copy((Resolve-Path -LiteralPath $LabSearchDll).ProviderPath, $labPath)
        $shellImage = $explorer.Modules | Where-Object ModuleName -eq shell32.dll
        $notifyCall = [IntPtr]::Add($shellImage.BaseAddress, 0x2ABA7C)
        $timerCall = [IntPtr]::Add($shellImage.BaseAddress, 0x180EB4)
        $originalNotify = [BitConverter]::ToString([ExplorerProcessProbe]::ReadCode($owner, $notifyCall, 5))
        $originalTimer = [BitConverter]::ToString([ExplorerProcessProbe]::ReadCode($owner, $timerCall, 7))
        $install = [ExplorerProcessProbe]::LabCommand($window, $owner, $labPath, 15, 'stdio.lab.search-install')
        if ($install) { throw "The search timer experiment refused to install, Win32 error $install." }
        $labReady = $true
    }
    $patcher = $explorer.Modules | Where-Object ModuleName -eq ExplorerPatcher.amd64.dll
    $directoryCount = $FileCount
    $data = [byte[]]@()
    for ($i = 0; $i -lt $FileCount; $i++) {
        [IO.File]::WriteAllBytes((Join-Path $target ('fixture-{0:d5}.bin' -f $i)), $data)
    }
    if ($labReady -and !$CheckSearchFallback) {
        $nearPath = Join-Path $target ($originalName + '.bak')
        [IO.File]::WriteAllBytes($nearPath, $data)
        ++$directoryCount
    }
    for ($round = 1; $round -le $Rounds; $round++) {
        [void](Wait-TestValue {
            !$view.Busy -and $view.Document.Folder.Self.Path -eq $control
        })
        if ($Search) { $searchValue = Open-SearchBox }
        if ($labReady) {
            $active = $CheckSearchFallback -or ($round % 4 -in 2,3)
            $command = if ($active) { 16 } else { 17 }
            [void][ExplorerProcessProbe]::LabCommand($window, $owner, $labPath, $command, 'stdio.lab.search-mode')
            $hitsBefore = [ExplorerProcessProbe]::LabCommand($window, $owner, $labPath, 18, 'stdio.lab.search-hits')
        }
        $explorer.Refresh()
        $cpuStart = $explorer.TotalProcessorTime.TotalMilliseconds
        $ioStart = [ExplorerViewOwner]::ReadIo($explorer.Handle)
        $navigationStart = [DateTime]::UtcNow
        $clock = [Diagnostics.Stopwatch]::StartNew()
        if ($Search) { $searchValue.SetValue($searchText) } else { $view.Navigate2($target) }
        $sample = @{ First = $null }
        $total = Wait-TestValue {
            if (!$Search -and $view.Document.Folder.Self.Path -ne $target) { return }
            if ($Search -and $view.LocationName -notlike '*fixture-00000*') { return }
            $ui = [Windows.Automation.AutomationElement]::FromHandle($window)
            try {
                $list = $ui.FindFirst($scope, $listPane)
                if ($null -eq $sample.First -and $list -and $list.FindFirst($scope, $firstItem)) {
                    $sample.First = $clock.Elapsed.TotalMilliseconds
                }
                if ($null -eq $sample.First) { return }
                $group = $ui.FindFirst($scope, $statusGroup)
                if (!$group) { return }
                $value = $group.FindFirst($scope, $statusValue)
                if ($value -and ($value.Current.Name -replace '[^0-9]', '') -eq [string]$expectedCount) {
                    return $clock.Elapsed.TotalMilliseconds
                }
            } catch [Windows.Automation.ElementNotAvailableException] {
                # Navigation can replace the active view during this sample.
            }
        }
        $explorer.Refresh()
        $ioEnd = [ExplorerViewOwner]::ReadIo($explorer.Handle)
        $result = [pscustomobject]@{
            Round = $round
            ExplorerPID = $owner
            NavigationStartUtc = $navigationStart.ToString('o')
            ExplorerPatcher = $(if ($patcher) { $patcher.FileVersionInfo.FileVersion })
            Files = $FileCount
            Operation = $(if ($Search) { 'Search' } else { 'Navigation' })
            FirstItemMs = [Math]::Round($sample.First, 2)
            FullCountMs = [Math]::Round($total, 2)
            ProcessCpuMs = [Math]::Round($explorer.TotalProcessorTime.TotalMilliseconds - $cpuStart, 2)
            PeakWorkingSetBytes = $explorer.PeakWorkingSet64
            PrivateBytes = $explorer.PrivateMemorySize64
            ProcessReads = $ioEnd.Reads - $ioStart.Reads
            ProcessWrites = $ioEnd.Writes - $ioStart.Writes
            ProcessOtherIo = $ioEnd.Other - $ioStart.Other
            ProcessReadBytes = $ioEnd.ReadBytes - $ioStart.ReadBytes
        }
        if ($labReady) {
            $hits = [ExplorerProcessProbe]::LabCommand($window, $owner, $labPath, 18, 'stdio.lab.search-hits') -
                    $hitsBefore
            if ($hits -ne [int]($active -and !$CheckSearchFallback)) {
                throw "Unexpected number of search timer adjustments: $hits."
            }
            $result | Add-Member -NotePropertyMembers @{
                SearchTimerMode = $(if ($CheckSearchFallback) { 'Fallback' }
                                    elseif ($active) { 'Adjusted' } else { 'PassThrough' })
                SearchTimerAdjustments = $hits
                SearchQuery = $searchText
            }
        }
        if ($SearchRename) {
            $originalPath = Join-Path $target $originalName
            $renamedPath = Join-Path $target $renamedName
            if ($RenameMethod -eq 'Shell') {
                $renameItem = $shell.NameSpace($target).ParseName($originalName)
                if (!$renameItem) { throw 'The Shell could not resolve the rename fixture.' }
            }
            $renameStart = [DateTime]::UtcNow
            $renameClock = [Diagnostics.Stopwatch]::StartNew()
            if ($RenameMethod -eq 'Shell') { $renameItem.Name = $renamedName }
            else { [IO.File]::Move($originalPath, $renamedPath) }
            if (![IO.File]::Exists($renamedPath) -or [IO.File]::Exists($originalPath)) {
                throw 'The rename did not produce the expected fixture.'
            }
            $updatedMs = $null
            do {
                $visible = Read-RenameResult
                if ($visible -and $visible.Renamed -and !$visible.Original) {
                    $updatedMs = $renameClock.Elapsed.TotalMilliseconds
                    break
                }
                Start-Sleep -Milliseconds 10
            } while ($renameClock.ElapsedMilliseconds -lt 3000)
            $renameClock.Stop()
            $view.Navigate2($control)
            [void](Wait-TestValue { !$view.Busy -and $view.Document.Folder.Self.Path -eq $control })
            $searchValue = Open-SearchBox
            $requeryStart = [DateTime]::UtcNow
            $requeryClock = [Diagnostics.Stopwatch]::StartNew()
            $searchValue.SetValue('fixture-00000')
            $requeryMs = Wait-TestValue {
                if ($view.LocationName -notlike '*fixture-00000*') { return }
                $state = Read-RenameResult
                if ($state -and $state.Renamed -and !$state.Original) {
                    $requeryClock.Elapsed.TotalMilliseconds
                }
            }
            $result | Add-Member -NotePropertyMembers @{
                RenameMethod = $RenameMethod
                RenameStartUtc = $renameStart.ToString('o')
                RenameObservedMs = $(if ($null -ne $updatedMs) { [Math]::Round($updatedMs, 2) })
                RenameObservationMs = [Math]::Round($renameClock.Elapsed.TotalMilliseconds, 2)
                OldNameAtLastSample = $(if ($visible) { $visible.Original })
                NewNameAtLastSample = $(if ($visible) { $visible.Renamed })
                RequeryStartUtc = $requeryStart.ToString('o')
                RequeryObservedMs = [Math]::Round($requeryMs, 2)
            }
        }
        $view.Navigate2($control)
        if ($SearchRename) {
            [void](Wait-TestValue { !$view.Busy -and $view.Document.Folder.Self.Path -eq $control })
            [IO.File]::Move($renamedPath, $originalPath)
        }
        $result
    }
} finally {
    $labCleanupError = $null
    if ($labReady) {
        try {
            $removed = [ExplorerProcessProbe]::LabCommand($window, $owner, $labPath, 19, 'stdio.lab.search-remove')
            if ($removed) { throw "Could not restore the native search calls, Win32 error $removed." }
            if ([BitConverter]::ToString([ExplorerProcessProbe]::ReadCode($owner, $notifyCall, 5)) -ne $originalNotify -or
                [BitConverter]::ToString([ExplorerProcessProbe]::ReadCode($owner, $timerCall, 7)) -ne $originalTimer) {
                throw 'The native search calls were not restored.'
            }
        } catch {
            $labCleanupError = $_
        }
    }
    if ($view) { $view.Quit() }
    if ([IO.Directory]::Exists($target)) {
        if ($nearPath) { [IO.File]::Delete($nearPath) }
        if ($SearchRename) { [IO.File]::Delete((Join-Path $target $renamedName)) }
        for ($i = 0; $i -lt $FileCount; $i++) {
            [IO.File]::Delete((Join-Path $target ('fixture-{0:d5}.bin' -f $i)))
        }
        [IO.Directory]::Delete($target)
    }
    if ([IO.Directory]::Exists($control)) { [IO.Directory]::Delete($control) }
    $labMapped = $false
    if ($labDirectory -and [IO.Directory]::Exists($labDirectory)) {
        try {
            [IO.File]::Delete($labPath)
            [IO.Directory]::Delete($labDirectory)
        } catch [IO.IOException] {
            $labMapped = $true
        } catch [UnauthorizedAccessException] {
            $labMapped = $true
        }
    }
    if (!$labMapped -and [IO.Directory]::Exists($root)) { [IO.Directory]::Delete($root) }
    if ($labCleanupError) { throw $labCleanupError }
}
