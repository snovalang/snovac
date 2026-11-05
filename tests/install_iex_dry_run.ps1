# Dry run of install.ps1 as `irm ... | iex`.
#
# Invoke-Expression leaves $PSScriptRoot and MyCommand.Path empty, and sets
# MyCommand.Definition to the command text. That text must not become the
# source directory. The source directory has to be a real filesystem path.

$ErrorActionPreference = 'Stop'

$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$Installer = Join-Path $RepoRoot 'install.ps1'
if (-not (Test-Path -LiteralPath $Installer)) {
    throw "install.ps1 not found at $Installer"
}

$PwshCmd = Get-Command pwsh -ErrorAction SilentlyContinue
if ($PwshCmd) {
    $Pwsh = $PwshCmd.Source
} else {
    throw 'pwsh is required to run tests/install_iex_dry_run.ps1'
}

function Test-IsUnderDirectory {
    param(
        [string]$Parent,
        [string]$Child
    )
    $prefix = [System.IO.Path]::GetFullPath($Parent)
    $separator = [System.IO.Path]::DirectorySeparatorChar
    if (-not $prefix.EndsWith($separator)) {
        $prefix += $separator
    }
    $full = [System.IO.Path]::GetFullPath($Child)
    return $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
}

function Invoke-Installer {
    param(
        [ValidateSet('iex', 'file')]
        [string]$Mode,
        [switch]$Update
    )

    $root = Join-Path ([System.IO.Path]::GetTempPath()) ('snova-iex-test-' + [guid]::NewGuid().ToString('N'))
    $homeDir = Join-Path $root 'home'
    $configDir = Join-Path $homeDir '.config'
    $profileDir = Join-Path $configDir 'powershell'
    $userProfile = Join-Path $root 'userprofile'
    $installDir = Join-Path $root 'install-prefix'
    $workDir = Join-Path $root 'workdir'
    $emptyPath = Join-Path $root 'empty-path'
    New-Item -ItemType Directory -Path $profileDir, $userProfile, $installDir, $workDir, $emptyPath | Out-Null

    $profile = @'
if (Test-Path -LiteralPath 'Alias:irm') {
    Remove-Item -LiteralPath 'Alias:irm' -Force
}
function global:irm {
    param(
        [Parameter(Position = 0)]
        [string]$Uri
    )
    Get-Content -Raw -LiteralPath $env:SNOVA_TEST_INSTALLER
}
'@
    Set-Content -LiteralPath (Join-Path $profileDir 'Microsoft.PowerShell_profile.ps1') -Value $profile -Encoding utf8

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Pwsh
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.WorkingDirectory = $workDir
    $psi.ArgumentList.Add('-NoLogo')
    $psi.ArgumentList.Add('-ExecutionPolicy')
    $psi.ArgumentList.Add('Bypass')
    if ($Mode -eq 'file') {
        $psi.ArgumentList.Add('-NoProfile')
        $psi.ArgumentList.Add('-File')
        $psi.ArgumentList.Add($Installer)
        if ($Update) {
            $psi.ArgumentList.Add('-Update')
        }
    } else {
        # The command text itself is `irm https://... | iex`, which is what
        # MyCommand.Definition contains for the reported failure.
        $psi.ArgumentList.Add('-Command')
        $psi.ArgumentList.Add('irm https://raw.githubusercontent.com/snovalang/snovac/master/install.ps1 | iex')
    }

    $psi.Environment['HOME'] = $homeDir
    $psi.Environment['XDG_CONFIG_HOME'] = $configDir
    $psi.Environment['USERPROFILE'] = $userProfile
    $psi.Environment['SNOVA_INSTALL_DIR'] = $installDir
    $psi.Environment['SNOVA_INSTALL_DRY_RUN'] = '1'
    $psi.Environment['SNOVA_TEST_INSTALLER'] = $Installer
    $psi.Environment['PATH'] = $emptyPath
    $psi.Environment['PROCESSOR_ARCHITECTURE'] = 'AMD64'
    if ($Update -and $Mode -eq 'iex') {
        $psi.Environment['SNOVA_UPDATE'] = '1'
    } elseif ($psi.Environment.ContainsKey('SNOVA_UPDATE')) {
        [void]$psi.Environment.Remove('SNOVA_UPDATE')
    }

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    [void]$proc.Start()
    $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
    $stderrTask = $proc.StandardError.ReadToEndAsync()
    $timedOut = -not $proc.WaitForExit(20000)
    if ($timedOut) {
        try { $proc.Kill($true) } catch { $proc.Kill() }
    }
    $proc.WaitForExit()
    $stdout = $stdoutTask.Result
    $stderr = $stderrTask.Result
    $output = ($stdout + "`n" + $stderr).Trim()
    if ($timedOut) {
        throw "installer timed out ($Mode update=$Update). Output:`n$output"
    }

    return [pscustomobject]@{
        ExitCode = $proc.ExitCode
        Output   = $output
        WorkDir  = $workDir
        Root     = $root
    }
}

function Get-Field {
    param(
        [string]$Output,
        [string]$Name
    )
    $match = [regex]::Match($Output, "(?m)^$Name=(.+)$")
    if (-not $match.Success) {
        throw "missing ${Name} in installer output:`n$Output"
    }
    return $match.Groups[1].Value.Trim()
}

function Assert-RealSource {
    param(
        [string]$Label,
        $Result,
        [ValidateSet('local', 'clone')]
        [string]$Kind
    )

    if ($Result.ExitCode -ne 0) {
        throw "$Label exited $($Result.ExitCode). Output:`n$($Result.Output)"
    }
    if ($Result.Output -match 'Cannot find drive') {
        throw "$Label treated the irm command text as a drive.`n$($Result.Output)"
    }

    $source = Get-Field -Output $Result.Output -Name 'SNOVA_SOURCE_DIR'
    $actualKind = Get-Field -Output $Result.Output -Name 'SNOVA_SOURCE_KIND'
    $tempDir = Get-Field -Output $Result.Output -Name 'SNOVA_TEMP_DIR'

    if ($actualKind -ne $Kind) {
        throw "$Label kind was $actualKind, expected $Kind. Source: $source"
    }
    if ($source -match '(?i)irm\s+https' -or $source.Contains('://') -or $source.Contains('|')) {
        throw "$Label source is command text, not a path: $source"
    }
    if (-not [System.IO.Path]::IsPathRooted($source)) {
        throw "$Label source is not rooted: $source"
    }
    if (-not [System.IO.Directory]::Exists($source)) {
        throw "$Label source directory does not exist: $source"
    }
    # The failure mode throws inside Join-Path. This must succeed.
    $makefile = Join-Path -Path $source -ChildPath 'Makefile'
    if ([string]::IsNullOrWhiteSpace($makefile)) {
        throw "$Label Join-Path returned an empty path for $source"
    }
    if ($source -eq $Result.WorkDir) {
        throw "$Label used the working directory as the source tree: $source"
    }

    if ($Kind -eq 'clone') {
        $leaf = [System.IO.Path]::GetFileName($source.TrimEnd('\', '/'))
        if ($leaf -ne 'snovac-repo') {
            throw "$Label clone directory was $source"
        }
        if (-not (Test-IsUnderDirectory -Parent $tempDir -Child $source)) {
            throw "$Label clone directory $source is not under $tempDir"
        }
        $repoFull = [System.IO.Path]::GetFullPath($RepoRoot)
        if ([System.IO.Path]::GetFullPath($source) -eq $repoFull) {
            throw "$Label reused the checkout instead of a temp clone"
        }
    } else {
        $repoFull = [System.IO.Path]::GetFullPath($RepoRoot)
        if ([System.IO.Path]::GetFullPath($source) -ne $repoFull) {
            throw "$Label local source was $source, expected $repoFull"
        }
    }

    Write-Host "OK $Label -> $source"
}

$runs = @(
    @{ Label = 'iex with no PSScriptRoot'; Mode = 'iex'; Update = $false; Kind = 'clone' }
    @{ Label = 'iex with SNOVA_UPDATE=1'; Mode = 'iex'; Update = $true; Kind = 'clone' }
    @{ Label = 'file install'; Mode = 'file'; Update = $false; Kind = 'local' }
    @{ Label = 'file -Update'; Mode = 'file'; Update = $true; Kind = 'clone' }
)

$failures = New-Object System.Collections.Generic.List[string]
foreach ($run in $runs) {
    $result = $null
    try {
        $update = [bool]$run.Update
        if ($update) {
            $result = Invoke-Installer -Mode $run.Mode -Update
        } else {
            $result = Invoke-Installer -Mode $run.Mode
        }
        Assert-RealSource -Label $run.Label -Result $result -Kind $run.Kind
    } catch {
        $failures.Add($_.Exception.Message)
    } finally {
        if ($result) {
            $tempMatch = [regex]::Match($result.Output, '(?m)^SNOVA_TEMP_DIR=(.+)$')
            if ($tempMatch.Success) {
                $temp = $tempMatch.Groups[1].Value.Trim()
                if (Test-Path -LiteralPath $temp) {
                    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
            if (Test-Path -LiteralPath $result.Root) {
                Remove-Item -LiteralPath $result.Root -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
    }
}

if ($failures.Count -gt 0) {
    throw ($failures -join "`n")
}
