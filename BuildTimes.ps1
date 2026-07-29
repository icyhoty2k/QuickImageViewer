#requires -Version 7.0
<#
.SYNOPSIS
    Per-step build timings and build-over-build deltas, read out of Ninja's own log.

.DESCRIPTION
    Ninja writes .ninja_log in the build directory as it works: one tab-separated
    line per edge, appended at the moment that edge FINISHES. The columns are

        start_ms  end_ms  output_mtime  output_path  command_hash

    where start_ms/end_ms are milliseconds since that build started. Nothing has
    to be instrumented and no build has to be re-run — the numbers are already
    there for every build ever done in this directory.

    Splitting the log back into individual builds relies on one property of how
    ninja writes it: because lines are appended in completion order, end_ms is
    non-decreasing WITHIN a build, and resets toward zero when the next build
    starts. A drop in end_ms is therefore a build boundary.

    Caveats worth knowing before reading the numbers:

      * The link edge includes the POST_BUILD block from CMakeLists.txt, which
        spawns powershell.exe. Expect a few hundred ms of that 'link' time to be
        process startup, not linking.
      * Durations overlap. With -j 30 many compiles run at once, so summing them
        gives CPU time, not elapsed time. The summary reports both, plus the
        ratio, which is the effective parallelism actually achieved.
      * A cancelled or failed build still leaves its finished edges in the log.

.PARAMETER BuildDir
    Directory holding .ninja_log.

.PARAMETER Top
    How many of the slowest steps to list. Ignored when -All is given.

.PARAMETER All
    List every step instead of the slowest -Top.

.PARAMETER History
    Show wall-clock totals for the last N builds in the log, oldest to newest.

.EXAMPLE
    ./BuildTimes.ps1
    Slowest steps of the most recent build, plus a delta against the one before it.

.EXAMPLE
    ./BuildTimes.ps1 -BuildDir Z:\QIV\fast -History 20
    Same, for the fast-iterate directory, with a 20-build trend.
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'Z:\QIV\release',
    [int]$Top = 15,
    [switch]$All,
    [int]$History = 10
)

$ErrorActionPreference = 'Stop'

$logPath = Join-Path $BuildDir '.ninja_log'
if (-not (Test-Path -LiteralPath $logPath)) {
    throw "No .ninja_log in '$BuildDir'. Wrong directory, or the generator is not Ninja."
}

# --- Parse -----------------------------------------------------------------
# Tolerate the leading '# ninja log vN' banner and any malformed trailing line
# (the log can be cut mid-write if a build is killed).
$edges = foreach ($line in Get-Content -LiteralPath $logPath) {
    if ($line.StartsWith('#') -or $line.Length -eq 0) { continue }
    $f = $line.Split("`t")
    if ($f.Count -lt 5) { continue }

    $start = 0; $end = 0
    if (-not [int]::TryParse($f[0], [ref]$start)) { continue }
    if (-not [int]::TryParse($f[1], [ref]$end)) { continue }

    [pscustomobject]@{
        Start = $start
        End   = $end
        Ms    = $end - $start
        # CMakeFiles/QuickImageViewer.dir/src/UI/Foo.cpp.obj -> src/UI/Foo.cpp
        Name  = $f[3] -replace '^CMakeFiles/[^/]+\.dir/', '' -replace '\.obj$', ''
    }
}

if (-not $edges) { throw "No usable entries in '$logPath'." }

# Ninja records a custom target's byproducts under BOTH their relative and their
# absolute path, so QIV_IncreaseBuildNumber and BuildNumber.h each show up twice
# with identical timings. Normalise the absolute form away so the two collapse.
$buildDirSlash = ($BuildDir -replace '\\', '/').TrimEnd('/') + '/'
foreach ($e in $edges) {
    if ($e.Name.StartsWith($buildDirSlash, [StringComparison]::OrdinalIgnoreCase)) {
        $e.Name = $e.Name.Substring($buildDirSlash.Length)
    }
}

# --- Split into builds on the end_ms reset ---------------------------------
$builds = [System.Collections.Generic.List[object]]::new()
$current = [System.Collections.Generic.List[object]]::new()
$prevEnd = -1

foreach ($e in $edges) {
    if ($e.End -lt $prevEnd -and $current.Count -gt 0) {
        $builds.Add($current)
        $current = [System.Collections.Generic.List[object]]::new()
    }
    $current.Add($e)
    $prevEnd = $e.End
}
if ($current.Count -gt 0) { $builds.Add($current) }

# Collapse the now-identical duplicate names within each build (see the
# byproduct note above). Done per build, not globally — the same file legitimately
# appears once in every build it was rebuilt in.
for ($i = 0; $i -lt $builds.Count; $i++) {
    $seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $unique = [System.Collections.Generic.List[object]]::new()
    foreach ($e in $builds[$i]) {
        if ($seen.Add($e.Name)) { $unique.Add($e) }
    }
    $builds[$i] = $unique
}

function Get-BuildSummary {
    param([object]$Build)

    $wall = ($Build | Measure-Object -Property End -Maximum).Maximum -
            ($Build | Measure-Object -Property Start -Minimum).Minimum
    $cpu = ($Build | Measure-Object -Property Ms -Sum).Sum

    [pscustomobject]@{
        Edges = $Build.Count
        WallMs = $wall
        CpuMs  = $cpu
        # >1 means work genuinely overlapped; ~1 means the build was serialised
        # (which is exactly what a link-dominated incremental build looks like).
        Parallelism = if ($wall -gt 0) { [math]::Round($cpu / $wall, 2) } else { 0 }
    }
}

$last = $builds[-1]
$lastSummary = Get-BuildSummary -Build $last

# --- Slowest steps ---------------------------------------------------------
$sorted = $last | Sort-Object -Property Ms -Descending
$show = if ($All) { $sorted } else { $sorted | Select-Object -First $Top }

Write-Host ''
Write-Host "Last build in $BuildDir" -ForegroundColor Cyan
Write-Host ('-' * 72)

$show | Format-Table -AutoSize @(
    @{ Label = 'ms'; Expression = { $_.Ms }; Align = 'right' }
    @{ Label = '%wall'; Expression = {
            if ($lastSummary.WallMs -gt 0) { [math]::Round(100 * $_.Ms / $lastSummary.WallMs, 1) } else { 0 } }
        Align = 'right' }
    @{ Label = 'step'; Expression = { $_.Name } }
)

Write-Host ("edges {0}   wall {1:N0} ms   cpu {2:N0} ms   parallelism {3}x" -f `
        $lastSummary.Edges, $lastSummary.WallMs, $lastSummary.CpuMs, $lastSummary.Parallelism)

# --- Compile vs link split -------------------------------------------------
# The single most useful number here: if 'link' dominates, PCH and -j cannot
# help and the LTCG form is what to change.
$linkEdges = $last | Where-Object { $_.Name -match '\.(exe|dll)$' }
if ($linkEdges) {
    $linkMs = ($linkEdges | Measure-Object -Property Ms -Sum).Sum
    $pct = if ($lastSummary.WallMs -gt 0) { [math]::Round(100 * $linkMs / $lastSummary.WallMs, 1) } else { 0 }
    Write-Host ("link {0:N0} ms = {1}% of wall clock (includes the POST_BUILD powershell call)" -f $linkMs, $pct) `
        -ForegroundColor Yellow
}

# --- Delta against the previous build --------------------------------------
if ($builds.Count -ge 2) {
    $prev = $builds[-2]
    $prevSummary = Get-BuildSummary -Build $prev

    Write-Host ''
    Write-Host 'Delta vs previous build' -ForegroundColor Cyan
    Write-Host ('-' * 72)

    $wallDelta = $lastSummary.WallMs - $prevSummary.WallMs
    $sign = if ($wallDelta -gt 0) { '+' } else { '' }
    $colour = if ($wallDelta -gt 0) { 'Red' } else { 'Green' }
    Write-Host ("wall {0:N0} ms -> {1:N0} ms  ({2}{3:N0} ms)   edges {4} -> {5}" -f `
            $prevSummary.WallMs, $lastSummary.WallMs, $sign, $wallDelta, $prevSummary.Edges, $lastSummary.Edges) `
        -ForegroundColor $colour

    # Per-step deltas are only meaningful for steps that ran in BOTH builds;
    # an incremental build compiles a handful of files, a full one compiles all
    # of them, and pretending those are comparable produces nonsense.
    $prevByName = @{}
    foreach ($e in $prev) { $prevByName[$e.Name] = $e.Ms }

    $common = foreach ($e in $last) {
        if ($prevByName.ContainsKey($e.Name)) {
            [pscustomobject]@{
                Name   = $e.Name
                Was    = $prevByName[$e.Name]
                Now    = $e.Ms
                Change = $e.Ms - $prevByName[$e.Name]
            }
        }
    }

    if ($common) {
        $common |
            Sort-Object -Property { [math]::Abs($_.Change) } -Descending |
            Select-Object -First $Top |
            Format-Table -AutoSize @(
                @{ Label = 'was'; Expression = { $_.Was }; Align = 'right' }
                @{ Label = 'now'; Expression = { $_.Now }; Align = 'right' }
                @{ Label = 'delta'; Expression = { '{0}{1}' -f $(if ($_.Change -gt 0) { '+' } else { '' }), $_.Change }; Align = 'right' }
                @{ Label = 'step'; Expression = { $_.Name } }
            )
    }
    else {
        Write-Host 'No steps in common — different build shapes, per-step delta skipped.' -ForegroundColor DarkGray
    }
}

# --- Trend -----------------------------------------------------------------
if ($History -gt 0 -and $builds.Count -gt 1) {
    Write-Host ''
    Write-Host "Wall clock, last $History builds (oldest first)" -ForegroundColor Cyan
    Write-Host ('-' * 72)

    $take = [math]::Min($History, $builds.Count)
    $builds[($builds.Count - $take)..($builds.Count - 1)] |
        ForEach-Object { Get-BuildSummary -Build $_ } |
        Format-Table -AutoSize @(
            @{ Label = 'edges'; Expression = { $_.Edges }; Align = 'right' }
            @{ Label = 'wall ms'; Expression = { $_.WallMs }; Align = 'right' }
            @{ Label = 'cpu ms'; Expression = { $_.CpuMs }; Align = 'right' }
            @{ Label = 'par'; Expression = { '{0}x' -f $_.Parallelism }; Align = 'right' }
        )
}
