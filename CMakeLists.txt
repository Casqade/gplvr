cmake_minimum_required(VERSION 3.21)
project(gplvr CXX)

# There are two builds here, and they are different bitnesses on purpose.
#
#   default (32-bit)                    -- gplvr.dll, gplvr.exe (loader), gplvr-xr32.exe
#   -DGPLVR_PRESENTER_ONLY=ON (64-bit)  -- gplvr-xr.exe
#
# gplc67.exe is a Win32 process, so anything injected into it must be 32-bit. OpenXR is
# the exception: runtimes are increasingly 64-bit only, which is why BFVR ended up with
# an x64 companion process. gplvr-xr32 and gplvr-xr are the same survey tool built both
# ways, so "does this runtime exist for 32-bit at all" is a question the user's own
# machine answers rather than one we guess.
#
#   cmake -S . -B build     -G "Visual Studio 17 2022" -A Win32
#   cmake -S . -B build-x64 -G "Visual Studio 17 2022" -A x64 -DGPLVR_PRESENTER_ONLY=ON
option(GPLVR_PRESENTER_ONLY "Build only the x64 OpenXR companion tools." OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GPLVR_OPENXR_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openxr-1.1.61/include")
if(NOT EXISTS "${GPLVR_OPENXR_INCLUDE}/openxr/openxr.h")
    message(FATAL_ERROR
        "The pinned OpenXR 1.1.61 headers are missing from third_party/. "
        "See third_party/openxr-1.1.61/README.md for what belongs there.")
endif()

# The survey tool, built for whichever bitness this configuration is. It links no OpenXR
# import library: runtime/openxr/<bitness>/openxr_loader.dll is opened by path at run
# time, so a missing or broken loader is a logged failure rather than a process that
# will not start.
function(gplvr_add_survey target output)
    add_executable(${target}
        src/Log.cpp
        src/openxr/Xr.cpp
        src/openxr/GplvrXr.cpp
        src/presenter/Consumer.cpp
    )
    set_target_properties(${target} PROPERTIES OUTPUT_NAME "${output}")
    target_include_directories(${target} PRIVATE src "${GPLVR_OPENXR_INCLUDE}")
    target_link_libraries(${target} PRIVATE d3d11 dxgi d3dcompiler user32)

    # The loader is opened by a path relative to the executable, so stage it there
    # rather than making "run the tool" a two-step job with a copy in the middle.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
            "$<TARGET_FILE_DIR:${target}>/runtime"
        COMMENT "Staging the OpenXR loader next to ${output}")
endfunction()

if(GPLVR_PRESENTER_ONLY)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR
            "GPLVR_PRESENTER_ONLY builds the x64 companion -- configure with -A x64. "
            "Current pointer size: ${CMAKE_SIZEOF_VOID_P} bytes.")
    endif()

    gplvr_add_survey(gplvr_xr "gplvr-xr")
    set(GPLVR_TARGETS gplvr_xr)
else()
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR
            "gplvr must be built as 32-bit -- configure with -A Win32 (Visual Studio) "
            "or a 32-bit toolchain. Current pointer size: ${CMAKE_SIZEOF_VOID_P} bytes. "
            "For the x64 OpenXR companion, add -DGPLVR_PRESENTER_ONLY=ON.")
    endif()

    # -- the mod itself ----------------------------------------------------------
    add_library(gplvr SHARED
        src/dllmain.cpp
        src/Discovery.cpp
        src/GlCapture.cpp
        src/GlInterop.cpp
        src/Hooks.cpp
        src/Log.cpp
        src/Pe.cpp
        src/Stereo.cpp
        src/VrCamera.cpp
        src/presenter/Producer.cpp
    )
    set_target_properties(gplvr PROPERTIES OUTPUT_NAME "gplvr" PREFIX "")
    target_include_directories(gplvr PRIVATE src)
    # No d3d11 or opengl32 import here on purpose. GlCapture.cpp resolves both by name,
    # so neither is loaded into the game unless glProbe is switched on -- and with the
    # D3D7 rasterizer selected there is no GL at all, which has to stay a logged note
    # rather than a load failure.
    target_link_libraries(gplvr PRIVATE user32)

    # -- the loader --------------------------------------------------------------
    # Starts the game suspended, injects gplvr.dll, resumes. See the comment at the top
    # of GplvrLoader.cpp for why this rather than a proxy DLL.
    add_executable(gplvr_loader WIN32 src/loader/GplvrLoader.cpp)
    set_target_properties(gplvr_loader PROPERTIES OUTPUT_NAME "gplvr")
    target_link_libraries(gplvr_loader PRIVATE user32)

    gplvr_add_survey(gplvr_xr32 "gplvr-xr32")
    set(GPLVR_TARGETS gplvr gplvr_loader gplvr_xr32)
endif()

foreach(target ${GPLVR_TARGETS})
    target_compile_definitions(${target} PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    endif()
endforeach()
