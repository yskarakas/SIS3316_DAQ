#!/bin/bash
# ===========================================================================
#  SIS3316 DAQ & Analysis Suite — one-click macOS installer.
#
#  Double-click this file in Finder, or run it from a terminal:
#      ./install_macos.command
#
#  On a clean Mac (Apple Silicon or Intel) it will:
#    1. install the Xcode command-line tools (if missing),
#    2. install Homebrew (if missing),
#    3. brew install cmake, Qt 6 and ROOT,
#    4. build the application, and
#    5. launch it.
#
#  It is safe to run again: anything already installed is skipped.
# ===========================================================================
set -euo pipefail

# Always operate from the folder this script lives in (the repo root).
cd "$(dirname "$0")"

bold=$'\033[1m'; green=$'\033[32m'; yellow=$'\033[33m'; red=$'\033[31m'; dim=$'\033[2m'; rst=$'\033[0m'
step() { printf "\n%s==> %s%s\n" "$bold$green" "$1" "$rst"; }
info() { printf "    %s\n" "$1"; }
warn() { printf "%s!! %s%s\n" "$yellow" "$1" "$rst"; }

on_error() {
  printf "\n%s========================================================%s\n" "$red" "$rst"
  printf "%sInstallation stopped.%s The message just above says why.\n" "$red$bold" "$rst"
  printf "Common fixes:\n"
  printf "  * Re-run this installer — Homebrew sometimes needs a second pass.\n"
  printf "  * Make sure you are online and try again.\n"
  printf "  * Manual build:  brew install cmake qt root && make run\n"
  printf "\nPress any key to close.\n"; read -r -n 1 -s || true
}
trap on_error ERR

printf "%s\n" "$bold"
printf "  SIS3316 DAQ & Analysis Suite — macOS installer\n"
printf "%s\n" "$rst"

# --- 1. Xcode command-line tools ------------------------------------------
step "Checking the Xcode command-line tools"
if xcode-select -p >/dev/null 2>&1; then
  info "Already installed."
else
  warn "Not found — launching Apple's installer."
  info "A system dialog will open; click \"Install\" and accept, then re-run this script."
  xcode-select --install || true
  printf "\nWaiting for the command-line tools to finish installing.\n"
  until xcode-select -p >/dev/null 2>&1; do sleep 5; done
  info "Command-line tools are ready."
fi

# --- 2. Homebrew -----------------------------------------------------------
step "Checking Homebrew"
if command -v brew >/dev/null 2>&1; then
  info "Already installed."
else
  warn "Not found — installing Homebrew from https://brew.sh (its official installer)."
  info "You may be asked for your macOS password by the Homebrew installer."
  NONINTERACTIVE=1 /bin/bash -c \
    "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
fi

# Put brew on PATH for this session (Apple Silicon vs Intel prefix).
if [ -x /opt/homebrew/bin/brew ]; then
  eval "$(/opt/homebrew/bin/brew shellenv)"
elif [ -x /usr/local/bin/brew ]; then
  eval "$(/usr/local/bin/brew shellenv)"
fi
command -v brew >/dev/null 2>&1 || { warn "brew still not on PATH — open a new terminal and re-run."; exit 1; }
info "Using $(brew --version | head -1)"

# --- 3. Dependencies -------------------------------------------------------
step "Installing build dependencies (cmake, qt, root)"
info "ROOT is large; the first install can take several minutes."
for pkg in cmake qt root; do
  if brew list --versions "$pkg" >/dev/null 2>&1; then
    info "$pkg — already installed."
  else
    info "$pkg — installing…"
    brew install "$pkg"
  fi
done

# --- 4. Build --------------------------------------------------------------
step "Building the application"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
make JOBS="$JOBS"

APP="build/SIS3316_Studio.app"
[ -d "$APP" ] || { warn "Build finished but $APP was not produced."; exit 1; }

# --- 5. Launch -------------------------------------------------------------
step "Done!"
info "The app is built at: $(pwd)/$APP"
info "Tip: drag SIS3316_Studio.app into /Applications to keep it."
printf "\nLaunching it now…\n"
open "$APP"

printf "\n%sInstallation complete.%s You can close this window.\n" "$green$bold" "$rst"
printf "Press any key to close.\n"; read -r -n 1 -s || true
