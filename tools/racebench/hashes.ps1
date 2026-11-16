# PC (2026-11-06): the benchmark's picture hashes over every renderer
# configuration - the four reference pictures (docs/renderer.md 10,
# tools/racebench) in each environment set: the thread configurations
# (no submit workers, no submit thread), the alpha mirror synced at every
# frame (as the game's double-buffered frames have it; with no workers too),
# the stage's walk and the registers from the record off, the run textures
# off, the lazy mirror off, the sync from a copy, the DATE pair as two draws,
# the mirror let go at every pass (a stress test), the fast clears off, the
# doubled index stream off, no merged draws.  Any change to the submission
# order or the threads must hold every row.
#   tools\racebench\hashes.ps1              every set, the four configurations
#   tools\racebench\hashes.ps1 -quick       the two main configurations
#   tools\racebench\hashes.ps1 -only normal,resync
# Pictures go to build\tools\racebench\px\ (-px for another folder).
param(
    [string]$exe = "build\tools\racebench\racebench.exe",
    [string[]]$only = @(),      # env-set names to run (empty: all)
    [switch]$quick,              # the two main configurations only
    [string]$px = "build\tools\racebench\px"
)
Set-Location (Join-Path $PSScriptRoot "..\..")
New-Item -ItemType Directory -Force $px | Out-Null
$configs = @(
    @{ n = "default";     o = @("--frames", "400");                                               ref = "6D69ECA7731214D9" },
    @{ n = "states";      o = @("--states", "--frames", "400");                                   ref = "6BAD526528AD934D" },
    @{ n = "far-newpal";  o = @("--far", "--states", "--newpal", "--frames", "300");              ref = "3799AA43562E00B7" },
    @{ n = "hires-date8"; o = @("--far", "--states", "--date", "8", "--hires", "--frames", "400"); ref = "1201609D198079D8" }
)
if ($quick) { $configs = $configs[0..1] }
$envs = @(
    @{ n = "normal";     e = @{} },
    @{ n = "resync";     e = @{ GT3PC_PGL_MIRROR_RESYNC = "1" } },
    @{ n = "workers0";   e = @{ GT3PC_PGL_WORKERS = "0" } },
    @{ n = "nosubmit";   e = @{ GT3PC_PGL_SUBMIT_THREAD = "0" } },
    @{ n = "walk0";      e = @{ GT3PC_PGL_RUN_WALK = "0" } },
    @{ n = "regs0";      e = @{ GT3PC_PGL_RUN_REGS = "0" } },
    @{ n = "runtex0";    e = @{ GT3PC_PGL_RUN_TEX = "0" } },
    @{ n = "resync-w0";  e = @{ GT3PC_PGL_MIRROR_RESYNC = "1"; GT3PC_PGL_WORKERS = "0" } },
    @{ n = "lazy0";      e = @{ GT3PC_PGL_MIRROR_LAZY = "0" } },
    @{ n = "synccopy";   e = @{ GT3PC_PGL_MIRROR_SYNC_COPY = "1"; GT3PC_PGL_MIRROR_RESYNC = "1" } },
    @{ n = "pair0";      e = @{ GT3PC_PGL_DATE_PAIR = "0" } },
    @{ n = "dropall";    e = @{ GT3PC_PGL_MIRROR_DROP_PX = "1" } },
    @{ n = "dropall-w0"; e = @{ GT3PC_PGL_MIRROR_DROP_PX = "1"; GT3PC_PGL_WORKERS = "0"; GT3PC_PGL_MIRROR_RESYNC = "1" } },
    @{ n = "fast0";      e = @{ GT3PC_PGL_FAST_CLEAR = "0" } },
    @{ n = "double0";    e = @{ GT3PC_PGL_AD_DOUBLE = "0" } },
    @{ n = "merge0";     e = @{ GT3PC_PGL_MERGE = "0" } }
)
if ($only.Count -gt 0) { $envs = $envs | Where-Object { $only -contains $_.n } }
$bad = 0
foreach ($ev in $envs) {
    foreach ($c in $configs) {
        $saved = @{}
        foreach ($k in $ev.e.Keys) {
            $saved[$k] = [Environment]::GetEnvironmentVariable($k)
            [Environment]::SetEnvironmentVariable($k, $ev.e[$k])
        }
        $file = Join-Path $px ("px_" + $ev.n + "_" + $c.n + ".rgba")
        $text = (& $exe @($c.o) --pixels $file 2>&1 | Out-String)
        foreach ($k in $ev.e.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
        $hash = "?"
        if ($text -match '([0-9A-F]{16})') { $hash = $Matches[1] }
        $ms = "?"
        if ($text -match '([\d.]+) ms a frame') { $ms = $Matches[1] }
        $ok = if ($hash -eq $c.ref) { "ok " } else { $bad++; "BAD" }
        "{0,-10} {1,-12} {2} {3}  {4} ms" -f $ev.n, $c.n, $ok, $hash, $ms
    }
}
"bad: $bad"
