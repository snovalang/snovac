# ==============================================================================
# Snovalang toolchain Windows installer. Installs or updates snl.exe.
#
# With no arguments, installs snl when it is absent and updates it when the
# command is already installed. Both paths download the latest release, or
# clone this repository and build it, so an update does not need a manual
# git pull and rebuild.
#
#   irm https://raw.githubusercontent.com/snovalang/snovac/master/install.ps1 | iex
#   $env:SNOVA_UPDATE = '1'; irm https://raw.githubusercontent.com/snovalang/snovac/master/install.ps1 | iex
#   powershell -ExecutionPolicy Bypass -File install.ps1
#   powershell -ExecutionPolicy Bypass -File install.ps1 -Update
#   powershell -ExecutionPolicy Bypass -File install.ps1 update
# ==============================================================================

$ErrorActionPreference = "Stop"

$Repo = "snovalang/snovac"
$Version = "0.0.1-p1"
$ExplicitUpdate = $false
$RunningAsFile = -not [string]::IsNullOrEmpty($MyInvocation.MyCommand.Path)

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

function Install-FromSource([string]$Dir) {
    Push-Location $Dir
    try {
        if (Get-Command make -ErrorAction SilentlyContinue) {
            make
        } elseif (Get-Command gcc -ErrorAction SilentlyContinue) {
            New-Item -ItemType Directory -Path build -Force | Out-Null
            gcc -std=c11 -O2 -g -pthread -o build/snl.exe *.c
        } else {
            Write-Error "GCC or Make is required to compile snl from source on Windows. Install MinGW-w64 or use a pre-built binary."
        }

        & powershell.exe -ExecutionPolicy Bypass -File (Join-Path $Dir "scripts\install_windows.ps1") -Prefix "$InstallPrefix" -BinDir "$BinDir" -LibDir "$LibDir" -IncDir "$IncDir"
    } finally {
        Pop-Location
    }
}

try {
    $Installed = $false
    # 2. Try downloading pre-built zip release
    Write-Host "==> Checking release package $ZipName..." -ForegroundColor Cyan
    $ZipPath = Join-Path $TempDir $ZipName
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls13
        Invoke-WebRequest -Uri $DownloadUrl -OutFile $ZipPath -UseBasicParsing -ErrorAction Stop
        Write-Host "[OK] Downloaded release archive." -ForegroundColor Green
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
        Write-Host "Release archive not yet published on GitHub Releases. Falling back to a source build..." -ForegroundColor Yellow
    }

    # 3. Source build fallback. An update clones the latest tree instead of
    # rebuilding whatever checkout happens to sit next to this script.
    if (-not $Installed) {
        $ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
        $SourceDir = $ScriptDir
        if ([string]::IsNullOrEmpty($SourceDir) -or -not (Test-Path (Join-Path $SourceDir "Makefile"))) {
            $SourceDir = (Get-Location).Path
        }
        $LocalMakefile = Test-Path (Join-Path $SourceDir "Makefile")

        if ($LocalMakefile -and -not $Updating) {
            Write-Host "==> Compiling snl from source..." -ForegroundColor Cyan
            Install-FromSource $SourceDir
            $Installed = $true
        } elseif (Get-Command git -ErrorAction SilentlyContinue) {
            Write-Host "==> Cloning $Repo repository..." -ForegroundColor Cyan
            $CloneDir = Join-Path $TempDir "snovac-repo"
            git clone --depth 1 "https://github.com/$Repo.git" $CloneDir
            Install-FromSource $CloneDir
            $Installed = $true
        } elseif ($LocalMakefile) {
            Write-Host "Could not reach GitHub. Building the local tree..." -ForegroundColor Yellow
            Install-FromSource $SourceDir
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
    Remove-Item -Path $TempDir -Recurse -Force -ErrorAction SilentlyContinue
}
