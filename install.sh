#!/bin/sh
# POSIX sh. Linux and macOS.
set -e

# ==============================================================================
# Snovalang toolchain installer. Installs or updates the `snl` command.
#
# With no arguments, installs snl when it is absent and updates it when the
# command is already installed. Both paths download the latest release, or
# clone this repository and build it, so an update does not need a manual
# git pull and rebuild.
#
#   curl -fsSL https://raw.githubusercontent.com/snovalang/snovac/master/install.sh | sh
#   curl -fsSL https://raw.githubusercontent.com/snovalang/snovac/master/install.sh | sh -s -- --update
#   sh install.sh
#   sh install.sh --update
#   sh install.sh update
# ==============================================================================

REPO="snovalang/snovac"
INSTALL_DIR="${SNOVA_INSTALL_DIR:-$HOME/.snova/bin}"
EXPLICIT=0

usage() {
    cat <<'EOF'
Usage: install.sh [update|--update]

With no arguments, installs snl when it is not already installed and updates
it when it is. Pass update or --update to fetch the latest snl and replace
the installed command.
EOF
}

for arg in "$@"; do
    case "$arg" in
        update|--update)
            EXPLICIT=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $arg" >&2
            usage >&2
            exit 1
            ;;
    esac
done

# Text formatting
BOLD="$(tput bold 2>/dev/null || echo '')"
GREEN="$(tput setaf 2 2>/dev/null || echo '')"
CYAN="$(tput setaf 6 2>/dev/null || echo '')"
YELLOW="$(tput setaf 3 2>/dev/null || echo '')"
RED="$(tput setaf 1 2>/dev/null || echo '')"
RESET="$(tput sgr0 2>/dev/null || echo '')"

echo "${CYAN}${BOLD}"
cat << "BANNER"
  ____                                         
 / ___| _ __   _____   ____ _ _ __   ___ _ __  
 \___ \| '_ \ / _ \ \ / / _` | '_ \ / _ \ '__| 
  ___) | | | | (_) \ V / (_| | | | |  __/ |    
 |____/|_| |_|\___/ \_/ \__,_|_| |_|\___|_|    
           Snovalang toolchain (snl)
BANNER
echo "${RESET}"

ALREADY=0
if [ -x "${INSTALL_DIR}/snl" ] || [ -x "${INSTALL_DIR}/snl.exe" ] || command -v snl >/dev/null 2>&1; then
    ALREADY=1
fi

if [ "$ALREADY" -eq 1 ]; then
    echo "${BOLD}==>${RESET} ${GREEN}snl${RESET} is already installed; updating it."
else
    echo "${BOLD}==>${RESET} Installing ${GREEN}snl${RESET}."
fi

# 1. Detect Operating System
OS="$(uname -s)"
case "$OS" in
    Linux*)     PLATFORM="linux" ;;
    Darwin*)    PLATFORM="darwin" ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT*)
        PLATFORM="windows"
        # `command -v` only checks PATH. MSYS can see powershell.exe and
        # still fail to exec it (EACCES). Probe before handing off so
        # `set -e` does not abort the shell installer.
        PSH=""
        if command -v powershell.exe >/dev/null 2>&1; then
            PSH=$(command -v powershell.exe)
        elif command -v pwsh >/dev/null 2>&1; then
            PSH=$(command -v pwsh)
        fi
        if [ -n "$PSH" ] && [ -x "$PSH" ] && "$PSH" -NoProfile -NonInteractive -Command "exit 0" >/dev/null 2>&1; then
            echo "${CYAN}==> Windows environment detected. Invoking native Windows installer...${RESET}"
            SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || SCRIPT_DIR="."
            if [ -f "$SCRIPT_DIR/install.ps1" ]; then
                if [ "$EXPLICIT" -eq 1 ]; then
                    "$PSH" -ExecutionPolicy Bypass -File "$SCRIPT_DIR/install.ps1" -Update && exit 0
                else
                    "$PSH" -ExecutionPolicy Bypass -File "$SCRIPT_DIR/install.ps1" && exit 0
                fi
                exit $?
            elif [ -f "$SCRIPT_DIR/scripts/install_windows.ps1" ]; then
                "$PSH" -ExecutionPolicy Bypass -File "$SCRIPT_DIR/scripts/install_windows.ps1" && exit 0
                exit $?
            else
                if [ "$EXPLICIT" -eq 1 ]; then
                    "$PSH" -ExecutionPolicy Bypass -Command "\$env:SNOVA_UPDATE='1'; irm https://raw.githubusercontent.com/${REPO}/master/install.ps1 | iex" && exit 0
                else
                    "$PSH" -ExecutionPolicy Bypass -Command "irm https://raw.githubusercontent.com/${REPO}/master/install.ps1 | iex" && exit 0
                fi
                exit $?
            fi
        fi
        echo "${YELLOW}PowerShell cannot be started from this shell. Continuing with the shell installer.${RESET}"
        ;;
    *)
        echo "${RED}Error: Unsupported Operating System: $OS${RESET}" >&2
        exit 1
        ;;
esac

# 2. Detect Machine Architecture
ARCH="$(uname -m)"
case "$ARCH" in
    x86_64|amd64)   ARCH_NAME="x86_64" ;;
    arm64|aarch64) ARCH_NAME="aarch64" ;;
    *)
        echo "${RED}Error: Unsupported CPU Architecture: $ARCH${RESET}" >&2
        exit 1
        ;;
esac

# The release workflow publishes a zip for Windows and a tar.gz elsewhere.
if [ "$PLATFORM" = "windows" ]; then
    ARCHIVE="snovac-${PLATFORM}-${ARCH_NAME}.zip"
else
    ARCHIVE="snovac-${PLATFORM}-${ARCH_NAME}.tar.gz"
fi
DOWNLOAD_URL="https://github.com/${REPO}/releases/latest/download/${ARCHIVE}"

echo "${BOLD}==>${RESET} Detected target: ${GREEN}${PLATFORM}-${ARCH_NAME}${RESET}"
echo "${BOLD}==>${RESET} Downloading ${CYAN}${ARCHIVE}${RESET} from ${REPO}..."

TMP_DIR="$(mktemp -d)"
cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

compile_from_source() {
    echo "${YELLOW}Release archive not found yet on latest release. Attempting source compile fallback...${RESET}"
    if ! command -v git >/dev/null 2>&1 || ! command -v make >/dev/null 2>&1 || ! command -v cc >/dev/null 2>&1; then
        return 1
    fi
    git clone --depth 1 "https://github.com/${REPO}.git" "$TMP_DIR/snovac-src"
    make -C "$TMP_DIR/snovac-src"
    mkdir -p "$TMP_DIR/extracted"
    if [ -f "$TMP_DIR/snovac-src/build/snl.exe" ]; then
        cp "$TMP_DIR/snovac-src/build/snl.exe" "$TMP_DIR/extracted/snl.exe"
    elif [ -f "$TMP_DIR/snovac-src/build/snl" ]; then
        cp "$TMP_DIR/snovac-src/build/snl" "$TMP_DIR/extracted/snl"
    else
        cp "$TMP_DIR/snovac-src/build/snovac" "$TMP_DIR/extracted/snl"
    fi
}

# 3. Download release binary. curl and wget both fall back to a source build.
if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$DOWNLOAD_URL" -o "$TMP_DIR/$ARCHIVE" || {
        rm -f "$TMP_DIR/$ARCHIVE"
        compile_from_source
    } || {
        echo "${RED}Failed to download binary from $DOWNLOAD_URL${RESET}" >&2
        exit 1
    }
elif command -v wget >/dev/null 2>&1; then
    wget -qO "$TMP_DIR/$ARCHIVE" "$DOWNLOAD_URL" || {
        rm -f "$TMP_DIR/$ARCHIVE"
        compile_from_source
    } || {
        echo "${RED}Failed to download binary using wget${RESET}" >&2
        exit 1
    }
else
    echo "${RED}Error: curl or wget is required to install snl.${RESET}" >&2
    exit 1
fi

if [ -f "$TMP_DIR/$ARCHIVE" ]; then
    mkdir -p "$TMP_DIR/extracted"
    case "$ARCHIVE" in
        *.zip)
            if command -v unzip >/dev/null 2>&1; then
                unzip -q "$TMP_DIR/$ARCHIVE" -d "$TMP_DIR/extracted"
            else
                tar -xf "$TMP_DIR/$ARCHIVE" -C "$TMP_DIR/extracted"
            fi
            ;;
        *)
            tar -xzf "$TMP_DIR/$ARCHIVE" -C "$TMP_DIR/extracted"
            ;;
    esac
fi

# Release archives named the binary `snovac` before the `snl` command rename.
if [ -f "$TMP_DIR/extracted/snovac" ] && [ ! -f "$TMP_DIR/extracted/snl" ]; then
    mv "$TMP_DIR/extracted/snovac" "$TMP_DIR/extracted/snl"
fi
if [ -f "$TMP_DIR/extracted/snovac.exe" ] && [ ! -f "$TMP_DIR/extracted/snl.exe" ]; then
    mv "$TMP_DIR/extracted/snovac.exe" "$TMP_DIR/extracted/snl.exe"
fi

BIN_SRC=""
if [ -f "$TMP_DIR/extracted/snl.exe" ]; then
    BIN_SRC="$TMP_DIR/extracted/snl.exe"
elif [ -f "$TMP_DIR/extracted/bin/snl.exe" ]; then
    BIN_SRC="$TMP_DIR/extracted/bin/snl.exe"
elif [ -f "$TMP_DIR/extracted/snl" ]; then
    BIN_SRC="$TMP_DIR/extracted/snl"
elif [ -f "$TMP_DIR/extracted/snovac" ]; then
    BIN_SRC="$TMP_DIR/extracted/snovac"
fi

if [ -z "$BIN_SRC" ]; then
    echo "${RED}Error: installer did not produce an snl binary.${RESET}" >&2
    exit 1
fi

# 4. Install binary. Windows needs the .exe suffix so PATHEXT can find it.
mkdir -p "$INSTALL_DIR"
if [ "$PLATFORM" = "windows" ]; then
    DEST="${INSTALL_DIR}/snl.exe"
else
    DEST="${INSTALL_DIR}/snl"
fi
cp -f "$BIN_SRC" "$DEST"
chmod +x "$DEST"

if [ "$ALREADY" -eq 1 ]; then
    echo "${GREEN}${BOLD}Updated snl binary at ${DEST}${RESET}"
else
    echo "${GREEN}${BOLD}Installed snl binary into ${DEST}${RESET}"
fi

# 5. Check and configure PATH
SHELL_CONFIG=""
case "$SHELL" in
    */zsh)  SHELL_CONFIG="$HOME/.zshrc" ;;
    */bash) SHELL_CONFIG="$HOME/.bashrc" ;;
    */fish) SHELL_CONFIG="$HOME/.config/fish/config.fish" ;;
    *)      SHELL_CONFIG="$HOME/.profile" ;;
esac

PATH_STR="export PATH=\"\$PATH:${INSTALL_DIR}\""

case ":${PATH}:" in
    *":${INSTALL_DIR}:"*)
        ;;
    *)
        if [ -f "$SHELL_CONFIG" ]; then
            if ! grep -q "${INSTALL_DIR}" "$SHELL_CONFIG"; then
                echo "" >> "$SHELL_CONFIG"
                echo "# Snovalang Compiler" >> "$SHELL_CONFIG"
                echo "$PATH_STR" >> "$SHELL_CONFIG"
                echo "${YELLOW}Added ${INSTALL_DIR} to PATH in ${SHELL_CONFIG}${RESET}"
            fi
        fi
        ;;
esac

echo ""
if [ "$ALREADY" -eq 1 ]; then
    echo "${GREEN}${BOLD}Snovalang toolchain (snl) was successfully updated!${RESET}"
else
    echo "${GREEN}${BOLD}Snovalang toolchain (snl) was successfully installed!${RESET}"
fi
echo "Run '${CYAN}snl --help${RESET}' to get started."
