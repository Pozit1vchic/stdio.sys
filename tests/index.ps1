param([string]$Tool, [string]$BuildDirectory)

$ErrorActionPreference = 'Stop'

function Assert-Count([string]$Query, [int]$Expected) {
    $writer.WriteLine("count $Query")
    $line = $process.StandardOutput.ReadLine()
    if ($line -notmatch "^matches=$Expected search_seconds=") {
        throw "Query '$Query' expected $Expected matches, received '$line'."
    }
}

function Assert-Response([string]$Command, [string]$Pattern) {
    $writer.WriteLine($Command)
    $line = $process.StandardOutput.ReadLine()
    if ($line -notmatch $Pattern) {
        throw "Command '$Command' expected '$Pattern', received '$line'."
    }
}

$parent = (Resolve-Path -LiteralPath $BuildDirectory).ProviderPath
$fixture = Join-Path $parent ('index-test-' + [guid]::NewGuid().ToString('N'))
$process = $null
try {
    [IO.Directory]::CreateDirectory($fixture) | Out-Null
    $left = [IO.Directory]::CreateDirectory((Join-Path $fixture 'left')).FullName
    $right = [IO.Directory]::CreateDirectory((Join-Path $fixture 'right')).FullName
    $name = [string]([char]0x0442) + [char]0x0435 + [char]0x0441 + [char]0x0442 + '.txt'
    $upper = $name.ToUpperInvariant()
    $original = Join-Path $left $name
    $alias = Join-Path $right $upper
    [IO.File]::WriteAllText($original, 'payload')
    [IO.File]::WriteAllText((Join-Path $right ("extra-$name")), 'other payload')
    New-Item -ItemType HardLink -Path $alias -Target $original | Out-Null

    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $Tool
    $start.Arguments = 'index-dir "' + $fixture + '"'
    $start.UseShellExecute = $false
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.StandardOutputEncoding = [Text.Encoding]::UTF8
    $process = [Diagnostics.Process]::Start($start)
    if ($process.StandardOutput.ReadLine() -notmatch '^entries=5 build_seconds=') {
        throw 'The initial directory snapshot did not include all three file names.'
    }
    # Windows PowerShell cannot set ProcessStartInfo.StandardInputEncoding.
    $writer = [IO.StreamWriter]::new($process.StandardInput.BaseStream, [Text.UTF8Encoding]::new($false))
    $writer.AutoFlush = $true
    Assert-Response 'snapshot-stats' '^full_refreshes=1 refreshed_directories=0$'
    Assert-Response 'folder-size' '^files=3 directories=3 bytes=27$'
    Assert-Count "name-exact $upper" 2
    Assert-Count "name $upper" 3
    Assert-Count 'name-exact missing.txt' 0
    $renamed = Join-Path $right 'renamed.txt'
    [IO.File]::Move($alias, $renamed)
    Assert-Count 'name-exact RENAMED.TXT' 1
    Assert-Count "name-exact $name" 1
    Assert-Response 'snapshot-stats' '^full_refreshes=1 refreshed_directories=1$'
    [IO.File]::WriteAllText($renamed, ('x' * 20))
    Assert-Count 'size 20:20' 2
    Assert-Response 'folder-size' '^files=3 directories=3 bytes=53$'
    Assert-Response 'snapshot-stats' '^full_refreshes=1 refreshed_directories=(?:[3-9]|[1-9][0-9]+)$'
    [IO.File]::Delete($renamed)
    Assert-Count 'name-exact renamed.txt' 0
    Assert-Response 'folder-size' '^files=2 directories=3 bytes=33$'
    [IO.Directory]::Move($left, (Join-Path $fixture 'relocated'))
    Assert-Count 'path relocated' 1
    Assert-Count 'path \left\' 0
    Assert-Response 'snapshot-stats' '^full_refreshes=2 refreshed_directories='
    for ($i = 0; $i -lt 1500; $i++) {
        [IO.File]::WriteAllBytes((Join-Path $right ("burst-$i.bin")), [byte[]]@())
    }
    Assert-Count 'name burst-' 1500
    Assert-Response 'snapshot-stats' '^full_refreshes=[3-9] refreshed_directories='
    $writer.WriteLine('quit')
    if (!$process.WaitForExit(5000) -or $process.ExitCode) {
        throw 'The index process did not exit successfully.'
    }
} finally {
    if ($process) {
        if (!$process.HasExited) { $process.Kill() }
        $process.Dispose()
    }
    if ([IO.Directory]::Exists($fixture)) {
        $resolved = (Resolve-Path -LiteralPath $fixture).ProviderPath
        if ([IO.Path]::GetDirectoryName($resolved) -ne $parent) {
            throw "Test fixture is outside the build directory ($resolved)."
        }
        [IO.Directory]::Delete($resolved, $true)
    }
}
