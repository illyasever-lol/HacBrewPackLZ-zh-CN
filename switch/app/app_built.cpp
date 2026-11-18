// When the app was built, for the log. A file of its own, which the Makefile compiles afresh for every build (build.cmd compiles everything every
// time), so that the time is the build's own: written where the log line is, it only changed when that file did, and two builds could say the same.
#include "app_internal.h"

namespace hbp::app {

const char* builtAt() { return __DATE__ " " __TIME__; }

}  // namespace hbp::app
