/* libxcselect's private interface, shared by the library and its tools.
 * macOS ships no header for these; the signatures are those its callers
 * use (see docs/xcselect.md). Not installed. */
#ifndef XCSELECT_PRIVATE_H
#define XCSELECT_PRIVATE_H

#include "xcselect.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define XCSELECT_EXPORT __attribute__((visibility("default")))

/* The macOS release whose behaviour this copies. */
#define XCSELECT_VERSION "2418"

#define XCSELECT_FLAG_REQUIRE_XCODE 1

#define DEVELOPER_DIR_ENV "DEVELOPER_DIR"
#define SELECT_LINK "/var/select/developer_dir"
#define DB_LINK "/var/db/xcode_select_link"
#define SHARE_LINK "/usr/share/xcode-select/xcode_dir_link"
#define SHARE_FILE "/usr/share/xcode-select/xcode_dir_path"
#define CLT_DIR "/Library/Developer/CommandLineTools"
#define CLT_LIBXCRUN CLT_DIR "/usr/lib/libxcrun.dylib"
#define XCRUN_LIBRARY "usr/lib/libxcrun.dylib"
#define XCRUN_BINARY "usr/bin/xcrun"
#define INSTALL_SOCKET "/private/var/run/org.minidarwin.xcselect.install.sock"
#define INSTALL_SOCKET_ENV "MINIDARWIN_XCSELECT_INSTALL_SOCKET"

typedef struct xcselect_manpaths xcselect_manpaths;

XCSELECT_EXPORT const char *xcselect_get_version(void);
XCSELECT_EXPORT bool xcselect_get_developer_dir_path(char *buf, int len,
                                                     bool *from_env,
                                                     bool *is_cltools,
                                                     bool *is_default);
XCSELECT_EXPORT bool xcselect_find_developer_contents_from_path(
    const char *path, char *dst, int len, bool *is_cltools);
XCSELECT_EXPORT bool xcselect_developer_dir_matches_path(const char *dev_dir,
                                                         const char *path);
XCSELECT_EXPORT _Noreturn int xcselect_invoke_xcrun(const char *tool, int argc,
                                                    char **argv, int flags);
XCSELECT_EXPORT bool xcselect_trigger_install_request(const char *tool_name);
XCSELECT_EXPORT bool xcselect_bundle_is_developer_tool(const char *bundle_id);
XCSELECT_EXPORT xcselect_manpaths *xcselect_get_manpaths(const char *sdk);
XCSELECT_EXPORT unsigned xcselect_manpaths_get_num_paths(xcselect_manpaths *m);
XCSELECT_EXPORT const char *xcselect_manpaths_get_path(xcselect_manpaths *m,
                                                       unsigned index);
XCSELECT_EXPORT void xcselect_manpaths_free(xcselect_manpaths *m);

/* System paths. The test build (XCSELECT_TESTING) puts them under
 * $XCSELECT_TEST_ROOT, so no test touches the real /var or /Library. */
static inline const char *xcs_path(const char *absolute, char *buf,
                                   size_t len) {
#ifdef XCSELECT_TESTING
  const char *root = getenv("XCSELECT_TEST_ROOT");
  if (root && *root) {
    snprintf(buf, len, "%s%s", root, absolute);
    return buf;
  }
#endif
  (void)buf;
  (void)len;
  return absolute;
}

/* The test build lets $XCSELECT_TEST_UID stand in for the effective UID, so
 * root-only actions can be tested unprivileged. */
static inline unsigned xcs_euid(void) {
#ifdef XCSELECT_TESTING
  const char *uid = getenv("XCSELECT_TEST_UID");
  if (uid && *uid)
    return (unsigned)strtoul(uid, NULL, 10);
#endif
  return (unsigned)geteuid();
}

#endif
