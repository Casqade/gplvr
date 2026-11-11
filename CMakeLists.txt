cmake_minimum_required(VERSION 3.21)
project(gplvr VERSION 1.0 LANGUAGES CXX RC)

# There are two builds here, and they are different bitnesses on purpose.
#
#   default (32-bit)                    -- gplvr.dll, gplvr.exe (loader)
#   -DGPLVR_PRESENTER_ONLY=ON (64-bit)  -- gplvr-xr.exe
#
# gplc67.exe is a Win32 process, so anything injected into it must be 32-bit. OpenXR is
# the exception: runtimes are 64-bit only in practice, which is why this ended up with an
# x64 companion process, as BFVR did.
#
# A 32-bit build of the survey tool used to be produced here so that "does a 32-bit OpenXR
# runtime exist at all" was answered by the machine rather than assumed. It answered no,
# the cross-process design is built on that answer, and nothing since has been in doubt --
# so it is no longer built. Re-add a `gplvr_add_survey(gplvr_xr32 "gplvr-xr32")` line if a
# runtime ever ships 32-bit support and the question becomes live again.
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

set(GPLVR_STAMP_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_target(gplvr_build_stamp
    COMMAND ${CMAKE_COMMAND} -DOUT=${GPLVR_STAMP_DIR}/GplvrBuildStamp.h
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR} -DVERSION=${PROJECT_VERSION}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/BuildStamp.cmake
    COMMENT "gplvr build stamp")

# The survey tool, built for whichever bitness this configuration is. It links no OpenXR
# import library: runtime/openxr/<bitness>/openxr_loader.dll is opened by path at run
# time, so a missing or broken loader is a logged failure rather than a process that
# will not start.
function(gplvr_add_survey target output)
    add_executable(${target}
        src/BuildStamp.cpp
        src/Log.cpp
        src/LogDir.cpp
        src/Paths.cpp
        src/Version.rc
        src/openxr/Beam.cpp
        src/openxr/Xr.cpp
        src/openxr/GplvrXr.cpp
        src/openxr/XrInput.cpp
        src/presenter/Consumer.cpp
    )
    set_target_properties(${target} PROPERTIES OUTPUT_NAME "${output}")
    target_include_directories(${target} PRIVATE src "${GPLVR_OPENXR_INCLUDE}" "${GPLVR_STAMP_DIR}")
    target_compile_definitions(${target} PRIVATE GPLVR_RC_PRESENTER)
    add_dependencies(${target} gplvr_build_stamp)
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
        src/AddressSpace.cpp
        src/BuildStamp.cpp
        src/Billboard.cpp
        src/CrashGuard.cpp
        src/CursorOverlay.cpp
        src/DataRebase.cpp
        src/Discovery.cpp
        src/DrawnMarks.cpp
        src/FlagmanReplay.cpp
        src/FlatFrames.cpp
        src/FlatTarget.cpp
        src/FrameBudget.cpp
        src/FrameHold.cpp
        src/VrMenuKeys.cpp
        src/GameExit.cpp
        src/GlApi.cpp
        src/GlInterop.cpp
        src/GplMenuPanel.cpp
        src/GpuTimer.cpp
        src/Hooks.cpp
        src/Hud.cpp
        src/Interpolation.cpp
        src/Letterbox.cpp
        src/Log.cpp
        src/LogDir.cpp
        src/MenuScale.cpp
        src/MirrorCache.cpp
        src/MirrorConfig.cpp
        src/MirrorDriverArms.cpp
        src/MirrorGeometry.cpp
        src/MirrorShared.cpp
        src/MirrorTarget.cpp
        src/MirrorUv.cpp
        src/MouseLook.cpp
        src/NoSignal.cpp
        src/FrameTrace.cpp
        src/GlTrace.cpp
        src/HeadAudio.cpp
        src/Paths.cpp
        src/Pe.cpp
        src/PostProfile.cpp
        src/Pribluda.cpp
        src/QuickMenu.cpp
        src/Radial.cpp
        src/RenderPacing.cpp
        src/RenderProfile.cpp
        src/RenderTarget.cpp
        src/ReplayPanel.cpp
        src/ReplayQuickMenu.cpp
        src/SegmentSearch.cpp
        src/SkidFlood.cpp
        src/Stereo.cpp
        src/StereoEuler.cpp
        src/SwapControl.cpp
        src/TaskRate.cpp
        src/TweakApi.cpp
        src/VrCamera.cpp
        src/VrInput.cpp
        src/VrMenu.cpp
        src/VrText.cpp
        src/Version.rc
        src/Window.cpp
        src/XrRuntimeProbe.cpp
        src/presenter/Producer.cpp
        src/presenter/HeadsetMemory.cpp
        src/fast_text/FastText.cpp
        src/fast_text/TextDraw.cpp
        src/baked/Assets.cpp
        src/baked/BakedLog.cpp
        src/baked/CarRenderer.cpp
        src/baked/CarRowStore.cpp
        src/baked/CarScene.cpp
        src/baked/Mip.cpp
        src/baked/Piff.cpp
        src/baked/Renderer.cpp
        src/baked/RendererMerged.cpp
        src/baked/Scene.cpp
        src/baked/SceneMemory.cpp
        src/baked/SegmentReach.cpp
        src/baked/Srb.cpp
        src/baked/TwoSided.cpp
        src/baked_gpl/BakedMirror.cpp
        src/baked_gpl/BakedPass.cpp
        src/baked_gpl/CarBake.cpp
        src/baked_gpl/CarBuildWorker.cpp
        src/baked_gpl/CarConfig.cpp
        src/baked_gpl/CarMemo.cpp
        src/baked_gpl/CarOrder.cpp
        src/baked_gpl/CarPassView.cpp
        src/baked_gpl/CarPoseWrapper.cpp
        src/baked_gpl/CarProgram.cpp
        src/baked_gpl/CarShare.cpp
        src/baked_gpl/CarUnion.cpp
        src/baked_gpl/CarWalk.cpp
        src/baked_gpl/CockpitDraw.cpp
        src/baked_gpl/CockpitGlass.cpp
        src/baked_gpl/Flagmen.cpp
        src/baked_gpl/ProgramArena.cpp
        src/baked_gpl/ResidualDepth.cpp
        src/baked_gpl/TrackBake.cpp
    )
    set_target_properties(gplvr PROPERTIES OUTPUT_NAME "gplvr" PREFIX "")
    target_include_directories(gplvr PRIVATE src "${GPLVR_STAMP_DIR}")
    target_compile_definitions(gplvr PRIVATE GPLVR_RC_DLL)
    add_dependencies(gplvr gplvr_build_stamp)
    # No d3d11 or opengl32 import here on purpose. GlInterop.cpp resolves both by name,
    # so neither is loaded into the game until VR is switched on -- and with the D3D7
    # rasterizer selected there is no GL at all, which has to stay a logged note rather
    # than a load failure.
    # gdi32 for the cursor overlay: GetIconInfo hands back HBITMAPs and GetDIBits is the
    # only way to read them. Unlike d3d11 and opengl32 this costs nothing to import --
    # gdi32 is already loaded in any GUI process, opengl32 itself depends on it, and it
    # is a KnownDLL so the import cannot be redirected by anything on disk.
    target_link_libraries(gplvr PRIVATE user32 gdi32)

    # -- the loader --------------------------------------------------------------
    # Starts the game suspended, injects gplvr.dll, resumes. See the comment at the top
    # of GplvrLoader.cpp for why this rather than a proxy DLL.
    add_executable(gplvr_loader WIN32
        src/loader/GplvrLoader.cpp
        src/BuildStamp.cpp
        src/LogDir.cpp
        src/Paths.cpp
        src/Version.rc
    )
    target_include_directories(gplvr_loader PRIVATE src "${GPLVR_STAMP_DIR}")
    target_compile_definitions(gplvr_loader PRIVATE GPLVR_RC_LOADER)
    add_dependencies(gplvr_loader gplvr_build_stamp)
    # Its own PDB name: with OUTPUT_NAME "gplvr" the loader's link would write gplvr.pdb over
    # the DLL's.
    set_target_properties(gplvr_loader PROPERTIES OUTPUT_NAME "gplvr" PDB_NAME "gplvr-loader")
    # shell32 for CommandLineToArgvW, which splits the command line so --exe can be taken out of
    # it by the same rules the game would have parsed the rest by.
    target_link_libraries(gplvr_loader PRIVATE user32 shell32)

    set(GPLVR_TARGETS gplvr gplvr_loader)
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
