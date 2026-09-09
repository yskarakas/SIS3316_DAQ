cmake_minimum_required(VERSION 3.20)
project(SIS3316_DAQ_Analysis LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
#  Dependencies: Qt6 (GUI) and ROOT (scientific data backend).
#  If CMake cannot find them automatically, pass their prefixes, e.g.:
#    cmake -DCMAKE_PREFIX_PATH="/opt/homebrew/opt/qt6;/opt/homebrew/opt/root" ..
#  or set the environment variables Qt6_DIR / ROOT_DIR.
# ---------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Widgets)
# RIO/Tree/Hist = data + histograms; Gpad/Graf/Graf3d/ASImage = the embedded ROOT
# Inspector's TCanvas + PNG rendering; Gui not needed (we render in batch mode).
find_package(ROOT REQUIRED COMPONENTS RIO Tree Hist Gpad Graf Graf3d ASImage)

set(APP_SOURCES
    src/main_studio.cpp
    # hardware / DAQ / analysis core
    src/hw/Sis3316Daq.cpp
    src/hw/Sis3316Config.cpp
    src/daq/Event.cpp
    src/analysis/Analysis.cpp
    # ROOT I/O backend
    src/io/RootWriter.cpp
    # GUI (pure Qt6)
    src/gui/PlotWidget.cpp
    src/gui/Heatmap2D.cpp
    src/gui/LiveWorker.cpp
    src/gui/LiveDaqWidget.cpp
    src/gui/ConfigEditor.cpp
    src/gui/RootAnalyzer.cpp
    src/gui/StudioWindow.cpp
    # headers with Q_OBJECT (for AUTOMOC)
    src/gui/PlotWidget.h
    src/gui/Heatmap2D.hpp
    src/gui/RateBars.hpp
    src/gui/LiveWorker.hpp
    src/gui/LiveDaqWidget.hpp
    src/gui/ConfigEditor.hpp
    src/gui/RootAnalyzer.hpp
    src/gui/StudioWindow.hpp
)

qt_add_executable(SIS3316_Studio MACOSX_BUNDLE ${APP_SOURCES})

target_include_directories(SIS3316_Studio PRIVATE
    src src/gui ${ROOT_INCLUDE_DIRS})

target_compile_definitions(SIS3316_Studio PRIVATE
    SIS_STUDIO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(SIS3316_Studio PRIVATE
    Qt6::Widgets ROOT::RIO ROOT::Tree ROOT::Hist ROOT::Gpad ROOT::Graf ROOT::ASImage)

set_target_properties(SIS3316_Studio PROPERTIES
    MACOSX_BUNDLE TRUE
    # NOTE: no raw "&" here — CMake copies this string verbatim into the XML
    # Info.plist, and a bare ampersand makes the plist invalid XML. macOS then
    # cannot read the bundle identifier at all, so LaunchServices may open a
    # DIFFERENT app that claims a similar id. Keep this plain ASCII text.
    MACOSX_BUNDLE_BUNDLE_NAME "SIS3316 DAQ Analysis"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.sis3316.daq"
    INSTALL_RPATH "${ROOT_LIBRARY_DIR}"
    BUILD_WITH_INSTALL_RPATH TRUE)

# Bundle the application icon (guarded so the build still works without it).
set(APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/AppIcon.icns")
if(APPLE AND EXISTS "${APP_ICON}")
    set_source_files_properties("${APP_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(SIS3316_Studio PRIVATE "${APP_ICON}")
    set_target_properties(SIS3316_Studio PROPERTIES MACOSX_BUNDLE_ICON_FILE "AppIcon.icns")
endif()

# ---------------------------------------------------------------------------
#  Optional hardware-free self-test (parser + PSD + ROOT round-trip).
#    cmake --build build --target studio_selftest && ./build/studio_selftest
# ---------------------------------------------------------------------------
add_executable(studio_selftest EXCLUDE_FROM_ALL
    src/tools/studio_selftest.cpp
    src/daq/Event.cpp
    src/analysis/Analysis.cpp
    src/io/RootWriter.cpp)
target_include_directories(studio_selftest PRIVATE src ${ROOT_INCLUDE_DIRS})
target_link_libraries(studio_selftest PRIVATE ROOT::RIO ROOT::Tree ROOT::Hist)
set_target_properties(studio_selftest PROPERTIES
    INSTALL_RPATH "${ROOT_LIBRARY_DIR}" BUILD_WITH_INSTALL_RPATH TRUE)

# ---------------------------------------------------------------------------
#  Headless data-taking tool (no Qt): configure + arm + stream to ROOT.
#    cmake --build build --target studio_acquire
#    ./build/studio_acquire --seconds 10 --out .
# ---------------------------------------------------------------------------
add_executable(studio_acquire EXCLUDE_FROM_ALL
    src/tools/studio_acquire.cpp
    src/hw/Sis3316Daq.cpp
    src/hw/Sis3316Config.cpp
    src/daq/Event.cpp
    src/analysis/Analysis.cpp
    src/io/RootWriter.cpp)
target_include_directories(studio_acquire PRIVATE src ${ROOT_INCLUDE_DIRS})
target_link_libraries(studio_acquire PRIVATE ROOT::RIO ROOT::Tree ROOT::Hist)
set_target_properties(studio_acquire PROPERTIES
    INSTALL_RPATH "${ROOT_LIBRARY_DIR}" BUILD_WITH_INSTALL_RPATH TRUE)

# ---------------------------------------------------------------------------
#  Headless PlotWidget render regression test (Qt only, no ROOT/hardware).
#    cmake --build build --target plot_render_test
#    QT_QPA_PLATFORM=offscreen ./build/plot_render_test
# ---------------------------------------------------------------------------
add_executable(plot_render_test EXCLUDE_FROM_ALL
    src/tools/plot_render_test.cpp
    src/gui/PlotWidget.cpp
    src/gui/PlotWidget.h)
set_target_properties(plot_render_test PROPERTIES AUTOMOC ON)
target_include_directories(plot_render_test PRIVATE src src/gui)
target_link_libraries(plot_render_test PRIVATE Qt6::Widgets)
