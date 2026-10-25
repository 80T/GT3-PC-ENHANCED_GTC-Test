# Gets a machine ready to build GT3 PC (run by setup.cmd).  The build needs
# the 32-bit (x86) MSVC tools, CMake + Ninja (Visual Studio's "C++ CMake
# tools for Windows") and a Windows SDK with Direct3D 11.  They come from an
# installed Visual Studio 2022 / 2026 (any edition, Build Tools included; the
# missing parts are added to it), else from a new Visual Studio Build Tools
# 2026 (Microsoft's download, its signature checked).  Then build\ is
# configured, which also downloads SDL3 (cmake/sdl3.cmake).
#   setup.cmd          get what is missing, configure build\
#   setup.cmd -Check   only say what is there and what would be done
param([switch]$Check)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'    # (Invoke-WebRequest is slow with its progress bar)

$root = Split-Path -Parent $PSScriptRoot
$installerDir = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
$vswhere = Join-Path $installerDir 'vswhere.exe'

$compTools = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
$compCMake = 'Microsoft.VisualStudio.Component.VC.CMake.Project'
$compSdk   = 'Microsoft.VisualStudio.Component.Windows11SDK.26100'
$buildToolsUrl = 'https://aka.ms/vs/18/stable/vs_buildtools.exe'

function Say([string]$text) { Write-Host "setup: $text" }
function Fail([string]$text) { Write-Host "setup: $text" -ForegroundColor Red; exit 1 }

# The Visual Studio instances, 2022 (17) or later, newest first.
function Get-VsInstances {
    if (-not (Test-Path $vswhere)) { return @() }
    $list = & $vswhere -all -products * -format json | ConvertFrom-Json
    return @($list | Where-Object { [version]$_.installationVersion -ge [version]'17.0' } |
             Sort-Object { [version]$_.installationVersion } -Descending)
}

# The ids of the instances that have a component.
function Get-InstancesWith([string]$component) {
    if (-not (Test-Path $vswhere)) { return @() }
    return @(& $vswhere -all -products * -requires $component -property instanceId)
}

# The newest instance with both the x86 tools and CMake + Ninja (the one
# tools\msvc.cmd picks), else $null.
function Get-ReadyInstance {
    $tools = Get-InstancesWith $compTools
    $cmake = Get-InstancesWith $compCMake
    foreach ($vs in Get-VsInstances) {
        if (($tools -contains $vs.instanceId) -and ($cmake -contains $vs.instanceId)) { return $vs }
    }
    return $null
}

# A Windows 10 / 11 SDK with the Direct3D 11 header and its x86 library.
function Test-WindowsSdk {
    $kits = $null
    try {
        $kits = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots' -Name KitsRoot10).KitsRoot10
    } catch { }
    if (-not $kits) { $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10' }
    foreach ($inc in @(Get-ChildItem (Join-Path $kits 'Include') -Directory -ErrorAction SilentlyContinue)) {
        if ((Test-Path (Join-Path $inc.FullName 'um\d3d11.h')) -and
            (Test-Path (Join-Path $kits "Lib\$($inc.Name)\um\x86\d3d11.lib"))) {
            return $true
        }
    }
    return $false
}

# Runs a Visual Studio installer and says how it ended (0 done, 3010 done
# but Windows wants a restart; anything else: stopped, e.g. 1602 cancelled).
function Invoke-VsInstaller([string]$exe, [string[]]$arguments) {
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -Wait -PassThru
    if ($p.ExitCode -eq 3010) {
        Say 'the Visual Studio installer finished; Windows wants a restart (the build works before it)'
    } elseif ($p.ExitCode -ne 0) {
        Say "the Visual Studio installer stopped with code $($p.ExitCode)"
    }
}

# ---- what is there ----------------------------------------------------------
$instances = Get-VsInstances
$withTools = Get-InstancesWith $compTools
$withCMake = Get-InstancesWith $compCMake
$sdk = Test-WindowsSdk

if ($instances.Count -eq 0) { Say 'no Visual Studio 2022 / 2026 found' }
foreach ($vs in $instances) {
    $parts = @()
    if ($withTools -contains $vs.instanceId) { $parts += 'the x86 C++ tools' }
    if ($withCMake -contains $vs.instanceId) { $parts += 'CMake + Ninja' }
    if ($parts.Count -eq 0) { $parts += 'no C++ build tools' }
    Say ("found {0} {1} ({2}): {3}" -f $vs.displayName, $vs.installationVersion, $vs.installationPath, ($parts -join ', '))
}
if ($sdk) { Say 'found a Windows SDK with Direct3D 11' } else { Say 'no Windows SDK with Direct3D 11 found' }

# ---- what is missing --------------------------------------------------------
$ready = Get-ReadyInstance
if (-not ($ready -and $sdk)) {
    # Added to the instance that has the most already; a new Build Tools 2026
    # when there is none.
    $target = $ready
    if (-not $target) {
        foreach ($vs in $instances) {
            if ($withTools -contains $vs.instanceId) { $target = $vs; break }
        }
    }
    if (-not $target -and $instances.Count -gt 0) { $target = $instances[0] }

    $add = @()
    if (-not $target -or -not ($withTools -contains $target.instanceId)) { $add += $compTools }
    if (-not $target -or -not ($withCMake -contains $target.instanceId)) { $add += $compCMake }
    if (-not $sdk) { $add += $compSdk }
    $addArgs = @($add | ForEach-Object { '--add'; $_ })

    if ($target) {
        Say ("adding to {0}: {1}" -f $target.displayName, ($add -join ', '))
        if ($Check) { Say '(-Check: nothing done)'; exit 1 }
        Invoke-VsInstaller (Join-Path $installerDir 'setup.exe') `
            (@('modify', '--installPath', "`"$($target.installationPath)`"", '--passive', '--norestart') + $addArgs)
    } else {
        Say ("installing Visual Studio Build Tools 2026 with: {0}" -f ($add -join ', '))
        if ($Check) { Say '(-Check: nothing done)'; exit 1 }
        $dir = Join-Path $env:TEMP 'gt3pc-setup'
        New-Item -ItemType Directory -Force $dir | Out-Null
        $exe = Join-Path $dir 'vs_BuildTools.exe'
        Say "downloading $buildToolsUrl"
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -Uri $buildToolsUrl -OutFile $exe -UseBasicParsing
        $sig = Get-AuthenticodeSignature $exe
        if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
            Remove-Item $exe
            Fail "the download is not signed by Microsoft ($($sig.Status)); nothing installed"
        }
        Invoke-VsInstaller $exe (@('--passive', '--wait', '--norestart', '--add', 'Microsoft.VisualStudio.Workload.VCTools') + $addArgs)
        Remove-Item $exe -ErrorAction SilentlyContinue
    }

    $ready = Get-ReadyInstance
    $sdk = Test-WindowsSdk
    $missing = @()
    if (-not $ready) { $missing += 'the x86 C++ tools / CMake + Ninja' }
    if (-not $sdk) { $missing += 'the Windows SDK' }
    if ($missing.Count -gt 0) {
        Fail (('still missing: {0} (the Visual Studio Installer adds them: "MSVC Build Tools for x64/x86", ' +
               '"C++ CMake tools for Windows", "Windows 11 SDK")') -f ($missing -join ', '))
    }
}
Say "using $($ready.displayName) $($ready.installationVersion)"
if ($Check) { Say 'ready to build'; exit 0 }

# ---- build\ -----------------------------------------------------------------
# A build\ configured elsewhere (another folder or machine, a compiler that
# is gone, another generator) is configured afresh (cmake --fresh: a new
# cache; the objects are rebuilt by the next build).
$build = Join-Path $root 'build'
$cache = Join-Path $build 'CMakeCache.txt'
$cmakeArgs = @('cmake', '-S', $root, '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=RelWithDebInfo')
function Get-CacheValue([string]$name) {
    $m = Select-String -Path $cache -Pattern "^$name`:[A-Z]+=(.*)$" | Select-Object -First 1
    if ($m) { return $m.Matches[0].Groups[1].Value }
    return ''
}
if (Test-Path $cache) {
    $src = (Get-CacheValue 'CMAKE_HOME_DIRECTORY') -replace '/', '\'
    $cxx = Get-CacheValue 'CMAKE_CXX_COMPILER'
    $gen = Get-CacheValue 'CMAKE_GENERATOR'
    if ($src.TrimEnd('\') -ne $root.TrimEnd('\') -or -not $cxx -or -not (Test-Path $cxx) -or $gen -ne 'Ninja') {
        Say "build\ was configured elsewhere ($src, $gen, $cxx): configuring it afresh"
        $cmakeArgs += '--fresh'
    }
}
& (Join-Path $root 'tools\msvc.cmd') @cmakeArgs
if ($LASTEXITCODE -ne 0) { Fail 'configuring build\ failed (above)' }

Say 'ready. build.cmd builds everything into build\bin\ (build.cmd --target gt3pc: the game alone).'
Say 'To play: build\bin\gt3launcher.exe, Install game files... with your disc image.'
