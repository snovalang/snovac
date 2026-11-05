# ==============================================================================
# Snovalang toolchain Windows installer. Installs or updates snl.exe.
#
# With no arguments, installs snl when it is absent and updates it when the
# command is already installed. Both paths download the latest release, or
# clone this repository and build it, so an update does not need a manual
# git pull and rebuild.
#
# `irm | iex` has no script file. A missing release archive is cloned into a
# temporary directory and built there. The command text is not a path.
#
#   irm https://raw.githubusercontent.com/snovalang/snovac/master/install.ps1 | iex
#   $env:SNOVA_UPDATE = '1'; irm https://raw.githubusercontent.com/snovalang/snovac/master/install.ps1 | iex
#   powershell -ExecutionPolicy Bypass -File install.ps1
#   powershell -ExecutionPolicy Bypass -File install.ps1 -Update
#   powershell -ExecutionPolicy Bypass -File install.ps1 update
# ==============================================================================

$ErrorActionPreference = "Stop"

$Repo = "snovalang/snovac"
$Version = "0.0.1-p3"
$ExplicitUpdate = $false
$RunningAsFile = -not [string]::IsNullOrEmpty($MyInvocation.MyCommand.Path)
# Only -File sets MyCommand.Path. Under `irm | iex` it is empty.
# MyCommand.Definition and MyInvocation.Line are the command text
# (`irm https://...`). Join-Path treats the words before ":" as a drive
# name ("irm https"), so those strings are never used as a filesystem path.
$ScriptFile = [string]$MyInvocation.MyCommand.Path
$DryRun = $env:SNOVA_INSTALL_DRY_RUN -match '^(1|true|yes)$'

if ($env:SNOVA_UPDATE -match '^(1|true|yes)$') {
    $ExplicitUpdate = $true
}

if ($RunningAsFile) {
    foreach ($arg in @($args)) {
        if ($arg -eq 'update' -or $arg -eq '-Update' -or $arg -eq '--update' -or $arg -eq '-update') {
            $ExplicitUpdate = $true
        } elseif ($arg -eq '-Help' -or $arg -eq '--help' -or $arg -eq '-help' -or $arg -eq '-h' -or $arg -eq 'help' -or $arg -eq '-?') {
            Write-Host "Usage: install.ps1 [update|-Update]"
            Write-Host "With no arguments, installs snl when it is absent and updates it when it is already installed."
            Write-Host "Pass -Update, or set SNOVA_UPDATE=1 before irm | iex, to update explicitly."
            return
        } else {
            Write-Error "Unknown argument: $arg"
        }
    }
}

$InstallPrefix = if (-not [string]::IsNullOrEmpty($env:SNOVA_INSTALL_DIR)) {
    $env:SNOVA_INSTALL_DIR
} else {
    Join-Path $env:USERPROFILE ".snova"
}
$InstallPrefix = [System.IO.Path]::GetFullPath($InstallPrefix)
$BinDir = [System.IO.Path]::GetFullPath((Join-Path $InstallPrefix "bin"))
$LibDir = [System.IO.Path]::GetFullPath((Join-Path $InstallPrefix "lib"))
$IncDir = [System.IO.Path]::GetFullPath((Join-Path $InstallPrefix "include"))
$SnExe = Join-Path $BinDir "snl.exe"
$OnPath = $null -ne (Get-Command snl -ErrorAction SilentlyContinue)
$AlreadyInstalled = (Test-Path -LiteralPath $SnExe) -or $OnPath
$Updating = $ExplicitUpdate -or $AlreadyInstalled

Write-Host @"
  ____                                         
 / ___| _ __   _____   ____ _ _ __   ___ _ __  
 \___ \| '_ \ / _ \ \ / / _` | '_ \ / _ \ '__| 
  ___) | | | | (_) \ V / (_| | | | |  __/ |    
 |____/|_| |_|\___/ \_/ \__,_|_| |_|\___|_|    
           Snovalang toolchain (snl)
"@ -ForegroundColor Cyan

if ($AlreadyInstalled) {
    Write-Host "==> snl is already installed; updating it." -ForegroundColor Green
} else {
    Write-Host "==> Installing snl." -ForegroundColor Green
}

# 1. Detect Architecture
$arch = switch ($env:PROCESSOR_ARCHITECTURE) {
    "AMD64" { "x86_64" }
    "ARM64" { "aarch64" }
    default { "x86_64" }
}

$Platform = "windows-$arch"
$ZipName = "snovac-$Platform.zip"
$DownloadUrl = "https://github.com/$Repo/releases/latest/download/$ZipName"

Write-Host "==> Detected target: $Platform" -ForegroundColor Green

$TempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("snovac-install-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $TempDir -Force | Out-Null

function Get-SnovaLocalCheckout {
    param([string]$ScriptFile)

    if ([string]::IsNullOrWhiteSpace($ScriptFile)) {
        return $null
    }
    # Reject command text such as `irm https://... | iex`. A real -File path
    # has neither a URL scheme nor a pipe.
    if ($ScriptFile.Contains('://') -or $ScriptFile.Contains('|') -or $ScriptFile.Contains("`n") -or $ScriptFile.Contains("`r")) {
        return $null
    }

    $dir = $null
    try {
        $dir = [System.IO.Path]::GetDirectoryName($ScriptFile)
    } catch {
        return $null
    }
    if ([string]::IsNullOrWhiteSpace($dir) -or -not [System.IO.Directory]::Exists($dir)) {
        return $null
    }

    $full = [System.IO.Path]::GetFullPath($dir)
    $makefile = [System.IO.Path]::Combine($full, 'Makefile')
    if (-not [System.IO.File]::Exists($makefile)) {
        return $null
    }
    return $full
}

function Install-FromSource([string]$Dir) {
    Push-Location $Dir
    try {
        if (-not (Get-Command make -ErrorAction SilentlyContinue)) {
            Write-Error "make is required to build snl. Install MinGW-w64 make, or use a published release."
        }
        # -s hides the cc recipe lines. OS=Windows_NT still wins if MSYS
        # make stripped the OS environment variable, and -lws2_32 resolves
        # the Winsock imports in socket_abi.c.
        # Windows feature-test macros. Do not pass Darwin or glibc -D flags.
        & make -s OS=Windows_NT EXTRA_LIBS="-lws2_32" CPPFLAGS="-DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601" build/snl.exe build/libsnovart.a
        if ($LASTEXITCODE -ne 0) {
            Write-Error "Building snl failed."
        }
        $built = Join-Path $Dir "build\snl.exe"
        if (-not (Test-Path -LiteralPath $built)) {
            Write-Error "Build finished without $built."
        }
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Dir "scripts\install_windows.ps1") -Prefix "$InstallPrefix" -BinDir "$BinDir" -LibDir "$LibDir" -IncDir "$IncDir" -Bin "$built"
        $installedExe = Join-Path $BinDir "snl.exe"
        if (-not (Test-Path -LiteralPath $installedExe)) {
            Write-Error "snl.exe was not installed into $BinDir."
        }
    } finally {
        Pop-Location
    }
}

$PreserveTemp = $false
try {
    $LocalCheckout = Get-SnovaLocalCheckout -ScriptFile $ScriptFile
    # TempDir comes from GetTempPath(), so this Join-Path is a real location.
    $CloneDir = [System.IO.Path]::GetFullPath((Join-Path $TempDir "snovac-repo"))

    # Test hook. Resolves the directory a missing release would build from
    # and does not download, clone, or install. SNOVA_INSTALL_DRY_RUN=1.
    if ($DryRun) {
        if ($LocalCheckout -and -not $Updating) {
            $SourceDir = $LocalCheckout
            $SourceKind = "local"
        } else {
            New-Item -ItemType Directory -Path $CloneDir -Force | Out-Null
            $SourceDir = $CloneDir
            $SourceKind = "clone"
        }
        if (-not [System.IO.Directory]::Exists($SourceDir)) {
            Write-Error "Source directory is not a real path: $SourceDir"
        }
        Write-Output "SNOVA_SOURCE_KIND=$SourceKind"
        Write-Output "SNOVA_SOURCE_DIR=$SourceDir"
        Write-Output "SNOVA_TEMP_DIR=$TempDir"
        $PreserveTemp = $true
        return
    }

    $Installed = $false
    # 2. Try downloading pre-built zip release
    $ZipPath = Join-Path $TempDir $ZipName
    try {
        # A missing asset throws. Swallow it here so a 404 is not a warning.
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls13
        Invoke-WebRequest -Uri $DownloadUrl -OutFile $ZipPath -UseBasicParsing -ErrorAction Stop
        Write-Host "==> Downloaded $ZipName." -ForegroundColor Green
        Expand-Archive -Path $ZipPath -DestinationPath (Join-Path $TempDir "extracted") -Force

        $candidates = @(
            (Join-Path $TempDir "extracted\snl.exe"),
            (Join-Path $TempDir "extracted\bin\snl.exe"),
            (Join-Path $TempDir "extracted\snovac.exe"),
            (Join-Path $TempDir "extracted\bin\snovac.exe")
        )
        $binSource = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($binSource) {
            New-Item -ItemType Directory -Path $BinDir -Force | Out-Null
            Copy-Item -Path $binSource -Destination (Join-Path $BinDir "snl.exe") -Force
            $Installed = $true
        }
    } catch {
        Remove-Item -LiteralPath $ZipPath -Force -ErrorAction SilentlyContinue
    }

    # 3. Source build fallback. An update clones the latest tree instead of
    # rebuilding whatever checkout happens to sit next to this script.
    # With no script file (`irm | iex`), the clone target is $CloneDir.
    if (-not $Installed) {
        if ($LocalCheckout -and -not $Updating) {
            Write-Host "==> Building snl..." -ForegroundColor Cyan
            Install-FromSource $LocalCheckout
            $Installed = $true
        } elseif (Get-Command git -ErrorAction SilentlyContinue) {
            Write-Host "==> Building snl..." -ForegroundColor Cyan
            & git clone --depth 1 --quiet "https://github.com/$Repo.git" $CloneDir
            if ($LASTEXITCODE -ne 0) {
                Write-Error "git clone of $Repo failed."
            }
            Install-FromSource $CloneDir
            $Installed = $true
        } elseif ($LocalCheckout) {
            Write-Host "==> Building snl..." -ForegroundColor Cyan
            Install-FromSource $LocalCheckout
            $Installed = $true
        } else {
            Write-Error "Neither prebuilt release nor git/make build environment was found."
        }
    }

    if ($Installed) {
        $verb = if ($AlreadyInstalled) { "updated" } else { "installed" }
        Write-Host ""
        Write-Host "Snovalang toolchain (snl) was successfully $verb!" -ForegroundColor Green
        Write-Host "Run 'snl --version' or 'snl --target-info' to get started." -ForegroundColor Cyan
    }
} finally {
    if (-not $PreserveTemp) {
        Remove-Item -Path $TempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
