# Writes the build stamp header gplvr logs at attach (src/BuildStamp.cpp), with the time of this
# build. Run by the gplvr_build_stamp target on every build: __DATE__/__TIME__ in dllmain.cpp only
# changed when that file was recompiled, so a log could name an older build than the one loaded
# (report 105 section 4.34.1).
#   cmake -DOUT=<header> -P BuildStamp.cmake
string(TIMESTAMP stamp "%Y-%m-%d %H:%M:%S")
file(WRITE "${OUT}" "#pragma once\n#define GPLVR_BUILD_STAMP \"${stamp}\"\n")
