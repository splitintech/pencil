cmake_minimum_required(VERSION 3.21...3.31)
if(APPLE AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES x86_64;arm64 CACHE STRING "macOS target architectures")
endif()

# Set project name and version
project(Pencil2D
    VERSION 0.7.0
    DESCRIPTION "Pencil2D Animation Software"
    LANGUAGES CXX
)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(STANLEE_AUTOMATION "Build optional authenticated local automation" OFF)
if(STANLEE_AUTOMATION)
    add_compile_definitions(STANLEE_AUTOMATION)
endif()

# CMake settings
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# Set version
if(NOT DEFINED APP_VERSION)
    set(APP_VERSION "0.0.0.0")
endif()

message(STATUS "App Version: ${APP_VERSION}")

# Find Qt
find_package(QT NAMES Qt6 REQUIRED COMPONENTS Core)
find_package(Qt6 REQUIRED COMPONENTS
    Core
    Widgets
    Gui
    Xml
    Multimedia
    Svg
    Network
    LinguistTools
)

# Global definitions
add_compile_definitions(
    APP_VERSION="${APP_VERSION}"
    MINIZ_NO_ZLIB_APIS
    QT_DEPRECATED_WARNINGS
    QT_DISABLE_DEPRECATED_UP_TO=0x050F00
)

# MSVC specific flags
if(MSVC)
    add_compile_options(/MP)  # Multi-processor compilation
endif()

# Include subdirectories to build as single executable
include(core_lib/core_lib.cmake)
include(app/app.cmake)
include(tests/tests.cmake)
if(STANLEE_AUTOMATION)
    target_sources(pencil2d PRIVATE app/src/automationbridge.cpp app/src/automationbridge.h app/data/automation.qrc)
    set_target_properties(pencil2d PROPERTIES OUTPUT_NAME pencil2d-stanlee)
    if(APPLE)
        set_target_properties(pencil2d PROPERTIES
            MACOSX_BUNDLE_BUNDLE_NAME "Pencil2D-Stanlee"
            MACOSX_BUNDLE_GUI_IDENTIFIER "net.splitin.Pencil2D-Stanlee"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/app/data/Info.stanlee.plist")
    endif()
endif()
