cmake_minimum_required(VERSION 3.21)

project(TDLASHydrogen
    VERSION 0.1.0
    DESCRIPTION "TDLAS hydrogen detection application based on Qt and NI-DAQmx"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TDLAS_ENABLE_DAQMX "Build NI-DAQmx hardware support" ON)
option(TDLAS_BUILD_TESTS "Build unit tests" OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Widgets Charts)

set(TDLAS_HAS_QT_CHARTS ON)

if (TDLAS_ENABLE_DAQMX)
    find_path(DAQMX_INCLUDE_DIR
        NAMES NIDAQmx.h
        HINTS
            "$ENV{NI4882_DIR}/include"
            "$ENV{NIDAQMX_INCLUDE_DIR}"
            "C:/Program Files/National Instruments/NI-DAQ/DAQmx ANSI C Dev/include"
            "C:/Program Files (x86)/National Instruments/NI-DAQ/DAQmx ANSI C Dev/include"
    )

    find_library(DAQMX_LIBRARY
        NAMES nidaqmx NIDAQmx
        HINTS
            "$ENV{NIDAQMX_LIB_DIR}"
            "C:/Program Files/National Instruments/NI-DAQ/DAQmx ANSI C Dev/lib/x64"
            "C:/Program Files (x86)/National Instruments/NI-DAQ/DAQmx ANSI C Dev/lib/x64"
            "C:/Program Files (x86)/National Instruments/NI-DAQ/DAQmx ANSI C Dev/lib/msvc"
            "C:/Program Files (x86)/National Instruments/Shared/ExternalCompilerSupport/C/lib64/msvc"
    )

    if (NOT DAQMX_INCLUDE_DIR OR NOT DAQMX_LIBRARY)
        message(WARNING
            "NI-DAQmx was not found. Hardware support will be disabled for this build. "
            "Set DAQMX_INCLUDE_DIR and DAQMX_LIBRARY, or configure with "
            "-DTDLAS_ENABLE_DAQMX=OFF."
        )
        set(TDLAS_ENABLE_DAQMX OFF)
    endif()
endif()

add_library(tdlas_core STATIC
    src/core/measurement_data.cpp
    src/processing/tdlas_processor.cpp
)

target_include_directories(tdlas_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(tdlas_core
    PUBLIC
        Qt${QT_VERSION_MAJOR}::Core
)

target_compile_definitions(tdlas_core
    PUBLIC
        TDLAS_HAS_QT_CHARTS=$<BOOL:${TDLAS_HAS_QT_CHARTS}>
)

add_library(tdlas_daq STATIC
    include/daq/daqmx_acquisition.h
    src/daq/daqmx_acquisition.cpp
)

target_include_directories(tdlas_daq
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(tdlas_daq
    PUBLIC
        tdlas_core
        Qt${QT_VERSION_MAJOR}::Core
)

if (TDLAS_ENABLE_DAQMX)
    target_include_directories(tdlas_daq PRIVATE "${DAQMX_INCLUDE_DIR}")
    target_link_libraries(tdlas_daq PRIVATE "${DAQMX_LIBRARY}")
    target_compile_definitions(tdlas_daq PUBLIC TDLAS_HAS_DAQMX=1)
else()
    target_compile_definitions(tdlas_daq PUBLIC TDLAS_HAS_DAQMX=0)
endif()

add_library(tdlas_ui STATIC
    include/ui/main_window.h
    src/ui/main_window.cpp
)

target_include_directories(tdlas_ui
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(tdlas_ui
    PUBLIC
        tdlas_core
        tdlas_daq
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Charts
)

add_executable(tdlas_app
    app/main.cpp
)

target_link_libraries(tdlas_app
    PRIVATE
        tdlas_ui
)

set_target_properties(tdlas_app PROPERTIES
    WIN32_EXECUTABLE TRUE
)

if (MSVC)
    target_compile_options(tdlas_core PRIVATE /W4)
    target_compile_options(tdlas_daq PRIVATE /W4)
    target_compile_options(tdlas_ui PRIVATE /W4)
    target_compile_options(tdlas_app PRIVATE /W4)
else()
    target_compile_options(tdlas_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(tdlas_daq PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(tdlas_ui PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(tdlas_app PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)
install(TARGETS tdlas_app
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

if (TDLAS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
