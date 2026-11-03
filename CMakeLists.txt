cmake_minimum_required(VERSION 3.20)
project(LaLuneDoree VERSION 1.01 LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # helps editors with code navigation

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)  # an interpreter is ~10x slower without optimisation
endif()

# Homebrew installs SDL2's CMake package here on Apple Silicon.
list(APPEND CMAKE_PREFIX_PATH /opt/homebrew/opt/sdl2 /usr/local/opt/sdl2)
find_package(SDL2 REQUIRED CONFIG)

# The emulator itself: no SDL or macOS code in here.
set(CORE_SOURCES
  src/apu.cpp
  src/backup.cpp
  src/bios.cpp
  src/bus.cpp
  src/cpu.cpp
  src/cpu_arm.cpp
  src/cpu_thumb.cpp
  src/dma.cpp
  src/gba.cpp
  src/ppu.cpp
  src/timers.cpp
)

# The app around it: window, input, audio and the game library UI.
set(FRONTEND_SOURCES
  src/main.cpp
  src/controls.cpp
  src/library.cpp
  src/settings.cpp
  src/video_filters.cpp
  src/mac_ui.mm
)
set_source_files_properties(src/mac_ui.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

# File name of the .app (kept ASCII so it's easy to type in a terminal).
# The name people see is "LaLune Dorée Emulator" (kAppName in main.cpp).
# Saves and settings stay in ~/Library/Application Support/LaLuneDoree.
set(APP_NAME "LaLuneDoree")

# App icon: a pixel-art golden crescent moon (regenerate with tools/make_icon.py).
set(APP_ICON "${CMAKE_SOURCE_DIR}/assets/AppIcon.icns")
set_source_files_properties(${APP_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

# Default games folder: games/ next to this file. Drop .gba files in it.
# Use another folder with -DGAMES_DIR=/some/folder, or change it in the app's Settings.
# An empty value (used for the downloadable app, see tools/make_dmg.sh) means
# ~/Library/Application Support/LaLuneDoree/Games, which exists on any Mac.
set(GAMES_DIR "${CMAKE_SOURCE_DIR}/games" CACHE PATH "Default folder the app reads games from")

add_executable(LaLuneDoree MACOSX_BUNDLE ${FRONTEND_SOURCES} ${CORE_SOURCES} ${APP_ICON})
if(GAMES_DIR)
  target_compile_definitions(LaLuneDoree PRIVATE LALUNE_GAMES_DIR="${GAMES_DIR}")
endif()
# Homebrew provides SDL2 as a shared library. The downloadable app links a
# static SDL2 instead (built by tools/make_dmg.sh), so it runs on Macs without it.
if(TARGET SDL2::SDL2)
  set(SDL_TARGET SDL2::SDL2)
else()
  set(SDL_TARGET SDL2::SDL2-static)
endif()

target_link_libraries(LaLuneDoree PRIVATE
  ${SDL_TARGET}
  "-framework AppKit"
  "-framework CoreText"
  "-framework GameController"
  "-framework UniformTypeIdentifiers"
)
target_compile_options(LaLuneDoree PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Hardening, because the app runs code from ROM files downloaded off the internet:
# - libc++ "fast" hardening mode makes std::array/std::vector indexing and
#   similar operations stop the app instead of reading or writing out of bounds.
# - Stack canaries on every function with arrays or address-taken locals.
target_compile_definitions(LaLuneDoree PRIVATE _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST)
target_compile_options(LaLuneDoree PRIVATE -fstack-protector-strong)

set_target_properties(LaLuneDoree PROPERTIES
  OUTPUT_NAME "${APP_NAME}"
  MACOSX_BUNDLE_BUNDLE_NAME "LaLune Dorée Emulator"
  MACOSX_BUNDLE_ICON_FILE "AppIcon.icns"
  MACOSX_BUNDLE_GUI_IDENTIFIER "dev.lalunedoree.emulator"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
)
