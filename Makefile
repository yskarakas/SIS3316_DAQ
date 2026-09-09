# ===========================================================================
#  SIS3316 DAQ & Analysis Software — convenience Makefile wrapper around CMake.
#
#  Usage:
#     make          # configure + build (Release)
#     make run      # build then launch the GUI
#     make selftest # build + run the hardware-free self-test
#     make clean    # remove the build directory
#
#  Requirements: CMake >= 3.20, a C++17 compiler, Qt6 (Widgets) and CERN ROOT.
#  If Qt6/ROOT are not found automatically, pass their locations:
#     make PREFIX_PATH="/path/to/qt6;/path/to/root"
# ===========================================================================
BUILD_DIR ?= build
JOBS      ?= 8

# Try to auto-detect Homebrew Qt6 and ROOT prefixes (macOS). Harmless elsewhere.
BREW_QT   := $(shell brew --prefix qt6 2>/dev/null || brew --prefix qt 2>/dev/null)
BREW_ROOT := $(shell brew --prefix root 2>/dev/null)
AUTO_PREFIX := $(BREW_QT);$(BREW_ROOT)
PREFIX_PATH ?= $(AUTO_PREFIX)

CMAKE_ARGS := -DCMAKE_BUILD_TYPE=Release
ifneq ($(strip $(PREFIX_PATH)),;)
CMAKE_ARGS += -DCMAKE_PREFIX_PATH="$(PREFIX_PATH)"
endif

APP := $(BUILD_DIR)/SIS3316_Studio.app/Contents/MacOS/SIS3316_Studio
APP_LINUX := $(BUILD_DIR)/SIS3316_Studio

.PHONY: all configure build run selftest clean

all: build

configure:
	cmake -S . -B $(BUILD_DIR) $(CMAKE_ARGS)

build: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)
	@echo ""
	@echo "Build complete. Launch with:  make run"

run: build
	@if [ -x "$(APP)" ]; then "$(APP)"; \
	elif [ -x "$(APP_LINUX)" ]; then "$(APP_LINUX)"; \
	else echo "Executable not found"; fi

selftest: configure
	cmake --build $(BUILD_DIR) -j$(JOBS) --target studio_selftest
	@if [ -x "$(BUILD_DIR)/studio_selftest" ]; then "$(BUILD_DIR)/studio_selftest"; fi

clean:
	rm -rf $(BUILD_DIR)
