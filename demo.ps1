[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('setup', 'insert', 'select')]
    [string] $Action = 'select',
    [ValidateRange(1, 10000)]
    [int] $Rows = 10000,
    [string] $Exe = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$total = [Diagnostics.Stopwatch]::StartNew()
$root = [IO.Path]::GetFullPath($PSScriptRoot)
$demo = Join-Path $root '.cache/presentation-demo'
$utf8 = New-Object Text.UTF8Encoding($false)
$lock = $null

function Write-JsonFile($Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 8), $utf8)
}

function Initialize-TimingHelper($Executable) {
    $build = Split-Path -Parent $Executable
    $helper = Join-Path $build 'demo_timing.exe'
    $source = Join-Path $root 'tools/demo_timing.c'
    if ((Test-Path -LiteralPath $helper -PathType Leaf) -and
        (Get-Item -LiteralPath $helper).LastWriteTimeUtc -ge (Get-Item -LiteralPath $source).LastWriteTimeUtc) { return }
    if (-not $build.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath (Join-Path $build 'CMakeCache.txt'))) {
        throw 'Build the demo_timing CMake target beside minidb.exe, then run setup again.'
    }
    $cmake = Join-Path $root '.cache/tooling/cmake/4.5.0/cmake-4.5.0-windows-x86_64/bin/cmake.exe'
    $messages = @(& $cmake --build $build --target demo_timing --parallel 2 2>&1)
    $code = $LASTEXITCODE
    [IO.File]::WriteAllText((Join-Path $demo 'timing-build.log'), ($messages -join "`n"), $utf8)
    if ($code -ne 0 -or -not (Test-Path -LiteralPath $helper)) {
        throw 'Cannot build demo_timing. See .cache/presentation-demo/timing-build.log'
    }
}

function Invoke-Db($Executable, $Schema, $Data, $SqlPath, $LogPrefix, $Init, $Measure = $false) {
    $program = $Executable
    $arguments = if ($Measure) {
        $program = Join-Path (Split-Path -Parent $Executable) 'demo_timing.exe'
        if (-not (Test-Path -LiteralPath $program -PathType Leaf)) {
            throw 'SQL timing helper is missing. Run .\demo.ps1 setup again.'
        }
        @($Schema, $Data, $SqlPath)
    } elseif ($Init) {
        @('init', '--schema-dir', $Schema, '--data-dir', $Data, '--format', 'jsonl')
    } else {
        @('run', '--schema-dir', $Schema, '--data-dir', $Data, '--file', $SqlPath, '--format', 'jsonl')
    }
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $lines = @(& $program @arguments 2> ($LogPrefix + '.stderr'))
    $code = $LASTEXITCODE
    $watch.Stop()
    $raw = $lines -join "`n"
    [IO.File]::WriteAllText(($LogPrefix + '.stdout'), $raw, $utf8)
    if ($code -ne 0) {
        throw "DB exit=$code. See $LogPrefix.stderr"
    }
    $value = $raw | ConvertFrom-Json
    if ($Measure) {
        if ($value.frequency -le 0 -or $value.preparation_ticks -lt 0 -or $value.sql_ticks -lt 0 -or
            [double]::IsNaN($value.preparation_ms) -or [double]::IsInfinity($value.preparation_ms) -or
            [double]::IsNaN($value.sql_ms) -or [double]::IsInfinity($value.sql_ms) -or
            $value.preparation_ms -lt 0 -or $value.sql_ms -lt 0) { throw 'Invalid DB timing.' }
        return [pscustomobject]@{ Value = $value.event; Milliseconds = $value.sql_ms;
            PreparationMilliseconds = $value.preparation_ms; ProcessMilliseconds = $watch.Elapsed.TotalMilliseconds }
    }
    return [pscustomobject]@{ Value = $value; Milliseconds = $watch.Elapsed.TotalMilliseconds;
        PreparationMilliseconds = 0; ProcessMilliseconds = $watch.Elapsed.TotalMilliseconds }
}

function Write-Sql($Run, $Name, $Sql) {
    $path = Join-Path $Run ($Name + '.sql')
    [IO.File]::WriteAllText($path, $Sql, $utf8)
    return $path
}

function Assert-Query($Query, $ExpectedRows) {
    if ($Query.type -ne 'query_result' -or $Query.statement_index -ne '1' -or
        [long]$Query.row_count -ne $ExpectedRows -or $Query.columns.Count -ne 3) {
        throw 'Unexpected DB query result.'
    }
    $names = @('id', 'name', 'age')
    $types = @('INT64', 'TEXT', 'INT64')
    for ($i = 0; $i -lt 3; $i++) {
        if ($Query.columns[$i].name -cne $names[$i] -or $Query.columns[$i].type -cne $types[$i]) {
            throw 'Unexpected DB result columns.'
        }
    }
}

function Read-Pair($Run, $State, $Id, $Prefix) {
    $sql = "SELECT id,name,age FROM users WHERE id=$Id;"
    $sqlPath = Write-Sql $Run $Prefix $sql
    $native = Invoke-Db $State.exe (Join-Path $Run 'schema') (Join-Path $Run 'data') $sqlPath (Join-Path $Run $Prefix) $false $true
    Assert-Query $native.Value 1
    $csv = Join-Path $Run 'users.csv'
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $matches = @(Select-String -LiteralPath $csv -Pattern "^$Id," -Encoding UTF8)
    $watch.Stop()
    if ($matches.Count -ne 1) { throw 'CSV must contain exactly one matching ID.' }
    $line = $native.Value.rows[0] -join ','
    if ($line -cne $matches[0].Line) { throw 'DB and CSV results differ.' }
    return [pscustomobject]@{ Line = $line; DbMs = $native.Milliseconds; DbPreparationMs = $native.PreparationMilliseconds;
        DbProcessMs = $native.ProcessMilliseconds; PsMs = $watch.Elapsed.TotalMilliseconds; Sql = $sql }
}

try {
    # 시연 파일만 소유한다. 이전 run을 지우거나 전역 설정을 바꾸지 않는다.
    [IO.Directory]::CreateDirectory($demo) | Out-Null
    $runs = Join-Path $demo 'runs'
    [IO.Directory]::CreateDirectory($runs) | Out-Null
    foreach ($path in @($demo, $runs)) {
        if ((Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'Demo directories must not be reparse points.'
        }
    }
    $lock = [IO.File]::Open((Join-Path $demo 'demo.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $currentPath = Join-Path $demo 'current.json'
    if ($Action -ne 'setup' -and ($PSBoundParameters.ContainsKey('Exe') -or $PSBoundParameters.ContainsKey('Rows'))) {
        throw '-Exe and -Rows are setup options only.'
    }

    if ($Action -eq 'setup') {
        if (-not $Exe) {
            $Exe = Join-Path $root '.cache/agent-harness/m14-native-20261119/artifacts/VERIFY/b/817458ad21/minidb.exe'
        }
        $Exe = [IO.Path]::GetFullPath($Exe)
        if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
            throw 'minidb.exe is missing. Build first, then use .\demo.ps1 setup -Exe <path>.'
        }
        Initialize-TimingHelper $Exe
        $runId = [guid]::NewGuid().ToString('N')
        $run = Join-Path $runs $runId
        $schema = Join-Path $run 'schema'
        $data = Join-Path $run 'data'
        [IO.Directory]::CreateDirectory($schema) | Out-Null
        [IO.File]::WriteAllText((Join-Path $schema 'users.json'), '{"version":1,"name":"users","columns":[{"name":"id","type":"INT64"},{"name":"name","type":"TEXT"},{"name":"age","type":"INT64"}]}', $utf8)
        $values = New-Object 'Collections.Generic.List[string]'
        $csvLines = New-Object 'Collections.Generic.List[string]'
        for ($id = 1; $id -le $Rows; $id++) {
            $name = 'User{0:D5}' -f $id
            $age = 20 + ($id % 50)
            $values.Add("('$name',$age)")
            $csvLines.Add("$id,$name,$age")
        }
        $seedPath = Write-Sql $run 'seed' ('INSERT INTO users(name,age) VALUES' + ($values -join ',') + ';')
        $null = Invoke-Db $Exe $schema $data '' (Join-Path $run 'init') $true
        $seed = Invoke-Db $Exe $schema $data $seedPath (Join-Path $run 'seed') $false
        if ($seed.Value.type -ne 'statement_ok' -or $seed.Value.command -ne 'INSERT' -or
            [long]$seed.Value.affected_rows -ne $Rows -or $seed.Value.committed -ne $true) {
            throw 'Seed INSERT was not confirmed.'
        }
        [IO.File]::WriteAllLines((Join-Path $run 'users.csv'), $csvLines, $utf8)
        $allPath = Write-Sql $run 'verify-all' 'SELECT id,name,age FROM users ORDER BY id;'
        $all = Invoke-Db $Exe $schema $data $allPath (Join-Path $run 'verify-all') $false
        Assert-Query $all.Value $Rows
        $actualCsv = [IO.File]::ReadAllLines((Join-Path $run 'users.csv'), $utf8)
        if ($actualCsv.Count -ne $Rows) { throw 'CSV fixture row count mismatch.' }
        for ($i = 0; $i -lt $Rows; $i++) {
            if (($all.Value.rows[$i] -join ',') -cne $csvLines[$i] -or $actualCsv[$i] -cne $csvLines[$i]) {
                throw "Fixture mismatch at row $($i + 1)."
            }
        }
        $state = [pscustomobject]@{ version = 1; exe = $Exe; seed_rows = $Rows; target = $Rows; inserts = 0; pending = $false }
        Write-JsonFile (Join-Path $run 'state.json') $state
        Write-JsonFile $currentPath @{ version = 1; run = $runId }
        $total.Stop()
        Write-JsonFile (Join-Path $run 'setup-report.json') @{ action = 'setup'; rows_verified = $Rows; executable = $Exe; powershell = $PSVersionTable.PSVersion.ToString(); total_ms = $total.Elapsed.TotalMilliseconds; valid = $true }
        Write-Host "READY: $Rows rows; DB and CSV all fields MATCH. Preparation: $($total.Elapsed.TotalMilliseconds.ToString('F1')) ms"
        Write-Host 'Live: .\demo.ps1 insert   then   .\demo.ps1 select'
    } else {
        if (-not (Test-Path -LiteralPath $currentPath -PathType Leaf)) { throw 'Run .\demo.ps1 setup first.' }
        $pointer = [IO.File]::ReadAllText($currentPath, $utf8) | ConvertFrom-Json
        if ($pointer.version -ne 1 -or $pointer.run -cnotmatch '^[0-9a-f]{32}$') { throw 'Invalid demo pointer. Run setup again.' }
        $run = Join-Path $runs $pointer.run
        if ((Get-Item -LiteralPath $run).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Demo run must not be a reparse point.' }
        $statePath = Join-Path $run 'state.json'
        $state = [IO.File]::ReadAllText($statePath, $utf8) | ConvertFrom-Json
        if ($state.version -ne 1 -or $state.pending -ne $false -or [long]$state.target -lt 1 -or
            [long]$state.target -ne ([long]$state.seed_rows + [long]$state.inserts)) {
            throw 'Incomplete or invalid demo pair. Run .\demo.ps1 setup again.'
        }
        if (-not (Test-Path -LiteralPath $state.exe -PathType Leaf)) { throw 'Recorded minidb.exe is missing. Run setup again.' }
        $tag = $Action + '-' + [guid]::NewGuid().ToString('N')
        $csv = Join-Path $run 'users.csv'
        if ($Action -eq 'insert') {
            $id = [long]$state.target + 1
            $line = "$id,Presenter,25"
            $sql = "INSERT INTO users(name,age) VALUES('Presenter',25);"
            Write-Host "SQL> $sql"
            Write-Host "PS > Add-Content -LiteralPath '$csv' -Value '$line' -Encoding UTF8"
            # 두 저장소는 하나의 트랜잭션이 아니다. 중간 실패 후 재실행을 막는다.
            $state.pending = $true
            Write-JsonFile $statePath $state
            $sqlPath = Write-Sql $run $tag $sql
            $native = Invoke-Db $state.exe (Join-Path $run 'schema') (Join-Path $run 'data') $sqlPath (Join-Path $run $tag) $false $true
            if ($native.Value.type -ne 'statement_ok' -or $native.Value.command -ne 'INSERT' -or
                $native.Value.affected_rows -ne '1' -or $native.Value.committed -ne $true) {
                throw 'INSERT was not confirmed.'
            }
            $watch = [Diagnostics.Stopwatch]::StartNew()
            Add-Content -LiteralPath $csv -Value $line -Encoding UTF8
            $watch.Stop()
            $dbMs = $native.Milliseconds
            $dbPreparationMs = $native.PreparationMilliseconds
            $dbProcessMs = $native.ProcessMilliseconds
            $psMs = $watch.Elapsed.TotalMilliseconds
            $pair = Read-Pair $run $state $id ($tag + '-verify')
            if ($pair.Line -cne $line) { throw 'New automatic ID or values differ from the expected record.' }
            $state.target = $id
            $state.inserts = [long]$state.inserts + 1
            $state.pending = $false
            Write-JsonFile $statePath $state
        } else {
            $id = [long]$state.target
            $sql = "SELECT id,name,age FROM users WHERE id=$id;"
            Write-Host "SQL> $sql"
            Write-Host "PS > Select-String -LiteralPath '$csv' -Pattern '^$id,' -Encoding UTF8"
            $pair = Read-Pair $run $state $id $tag
            $line = $pair.Line
            $dbMs = $pair.DbMs
            $dbPreparationMs = $pair.DbPreparationMs
            $dbProcessMs = $pair.DbProcessMs
            $psMs = $pair.PsMs
        }
        $difference = $dbMs - $psMs
        $total.Stop()
        Write-JsonFile (Join-Path $run ($tag + '-report.json')) @{ action = $Action; id = $id; row = $line; logical_rows = [long]$state.target; db_ms = $dbMs; db_preparation_ms = $dbPreparationMs; db_process_ms = $dbProcessMs; powershell_ms = $psMs; db_minus_powershell_ms = $difference; total_ms = $total.Elapsed.TotalMilliseconds; valid = $true; db_timing = 'request open + SQL input/parsing/execution + EOF + request close, measured directly inside native helper'; db_preparation_timing = 'SQL file open + DB schema/data validation/loading + index construction; excluded from SQL comparison'; file_timing = 'current PowerShell host Add-Content or full-file Select-String; different validation/durability work' }
        Write-Host "MATCH: $line"
        Write-Host ('DB preparation {0:F3} ms (file open / load / index; excluded below)' -f $dbPreparationMs)
        Write-Host ('SQL {0:F3} ms | PowerShell {1:F3} ms | SQL-PS {2:+0.000;-0.000;0.000} ms' -f $dbMs, $psMs, $difference)
        Write-Host ('DB process {0:F1} ms | Wrapper work {1:F1} ms (before report/output)' -f $dbProcessMs, $total.Elapsed.TotalMilliseconds)
    }
} catch {
    Write-Error -Message $_.Exception.Message -ErrorAction Continue
    exit 1
} finally {
    if ($null -ne $lock) { $lock.Dispose() }
}
