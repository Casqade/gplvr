#pragma once

namespace gplvr {

// When this binary was built: written into a generated header on every build
// (tools/BuildStamp.cmake), so it names the link that produced the binary.
const char* buildStamp();

// The release version, from `project(gplvr VERSION ...)` in CMakeLists.txt -- "1.0".
const char* buildVersion();

// The commit the build was made from, short form, with "+" when the working tree had changes to
// tracked files; "unknown" without git. Together with buildStamp() it names the exact source.
const char* buildCommit();

}  // namespace gplvr
