#pragma once

namespace gplvr {

// When this gplvr.dll was built: written into a generated header on every build
// (tools/BuildStamp.cmake), so it names the link that produced the binary.
const char* buildStamp();

}  // namespace gplvr
