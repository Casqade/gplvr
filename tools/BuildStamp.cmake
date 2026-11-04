# Writes the build stamp header every gplvr binary logs at start (src/BuildStamp.cpp) and the
# version resource reads (src/Version.rc): the version, the commit and the time of this build. Run
# by the gplvr_build_stamp target on every build: __DATE__/__TIME__ in dllmain.cpp only changed
# when that file was recompiled, so a log could name an older build than the one loaded (report
# 105 section 4.34.1).
#   cmake -DOUT=<header> -DSOURCE=<repo> -DVERSION=<major.minor> -P BuildStamp.cmake
string(TIMESTAMP stamp "%Y-%m-%d %H:%M:%S")

string(REPLACE "." ";" parts "${VERSION}")
list(LENGTH parts count)
list(GET parts 0 major)
set(minor 0)
if(count GREATER 1)
    list(GET parts 1 minor)
endif()

# The commit, and "+" when tracked files differ from it: a build from an edited tree is not the
# commit it names.
set(commit "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE}" rev-parse --short HEAD
                    OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE headResult ERROR_QUIET)
    if(headResult EQUAL 0 AND head)
        set(commit "${head}")
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE}" status --porcelain
                                --untracked-files=no
                        OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE
                        RESULT_VARIABLE dirtyResult ERROR_QUIET)
        if(dirtyResult EQUAL 0 AND dirty)
            set(commit "${commit}+")
        endif()
    endif()
endif()

set(content "#pragma once\n")
string(APPEND content "#define GPLVR_BUILD_STAMP \"${stamp}\"\n")
string(APPEND content "#define GPLVR_VERSION \"${major}.${minor}\"\n")
string(APPEND content "#define GPLVR_VERSION_MAJOR ${major}\n")
string(APPEND content "#define GPLVR_VERSION_MINOR ${minor}\n")
string(APPEND content "#define GPLVR_BUILD_COMMIT \"${commit}\"\n")
# One literal for the resource compiler, which does not join adjacent strings the way C does.
string(APPEND content
       "#define GPLVR_VERSION_TEXT \"${major}.${minor} (${commit}, ${stamp})\"\n")
file(WRITE "${OUT}" "${content}")
