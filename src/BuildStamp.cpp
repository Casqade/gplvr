#include "BuildStamp.h"

#include "GplvrBuildStamp.h"  // generated in the build directory by tools/BuildStamp.cmake

namespace gplvr {

const char* buildStamp() { return GPLVR_BUILD_STAMP; }

const char* buildVersion() { return GPLVR_VERSION; }

const char* buildCommit() { return GPLVR_BUILD_COMMIT; }

}  // namespace gplvr
