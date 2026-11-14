/* /usr/bin/xcrun and the tool shims (cc, make, git, xcodebuild, ...), one
 * source built three ways: XCRUN, or TOOL_FLAGS 0 (public tools) or
 * XCSELECT_FLAG_REQUIRE_XCODE (tools only Xcode has). Each name in /usr/bin
 * is a symlink to its shim, and the name run is the tool run. */
#include "xcselect-private.h"

#include <stdint.h>
#include <strings.h>

/* Marks this file as a shim; libxcselect refuses to run one as a tool. */
__attribute__((used, section("__DATA,__xcrun_shim"))) static uint32_t xcrun_shim = 1;

int main(int argc, char **argv) {
  const char *name = getprogname();
#ifdef XCRUN
  xcselect_invoke_xcrun(strcasecmp(name, "xcrun") ? name : NULL, argc - 1,
                        argv + 1, 0);
#else
  /* FIXME (ACCURACY): macOS takes the name from the file system's own
   * spelling of the executable's path, which matters only on a
   * case-insensitive volume. */
  if (!strcmp(name, "llvm-gcc"))
    name = "clang";
  else if (!strcmp(name, "llvm-g++"))
    name = "clang++";
  xcselect_invoke_xcrun(name, argc - 1, argv + 1, TOOL_FLAGS);
#endif
}
