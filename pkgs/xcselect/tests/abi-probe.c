/* Calls every libxcselect export from the library named on the command line
 * and prints the results, so that macOS's library and this one can be
 * compared (scripts/test_xcselect_abi.py). Paths to try come after it. */
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOAD(name) name##_fn name = (name##_fn)dlsym(lib, #name)

typedef const char *(*xcselect_get_version_fn)(void);
typedef bool (*xcselect_get_developer_dir_path_fn)(char *, int, bool *, bool *, bool *);
typedef bool (*xcselect_find_developer_contents_from_path_fn)(const char *, char *, int, bool *);
typedef bool (*xcselect_developer_dir_matches_path_fn)(const char *, const char *);
typedef bool (*xcselect_bundle_is_developer_tool_fn)(const char *);
typedef int (*xcselect_host_sdk_path_fn)(unsigned, char **);
typedef void *(*xcselect_get_manpaths_fn)(const char *);
typedef unsigned (*xcselect_manpaths_get_num_paths_fn)(void *);
typedef const char *(*xcselect_manpaths_get_path_fn)(void *, unsigned);
typedef void (*xcselect_manpaths_free_fn)(void *);

int main(int argc, char **argv) {
  void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    fprintf(stderr, "%s\n", dlerror());
    return 1;
  }
  LOAD(xcselect_get_version);
  LOAD(xcselect_get_developer_dir_path);
  LOAD(xcselect_find_developer_contents_from_path);
  LOAD(xcselect_developer_dir_matches_path);
  LOAD(xcselect_bundle_is_developer_tool);
  LOAD(xcselect_host_sdk_path);
  LOAD(xcselect_get_manpaths);
  LOAD(xcselect_manpaths_get_num_paths);
  LOAD(xcselect_manpaths_get_path);
  LOAD(xcselect_manpaths_free);
  printf("version %s\n", xcselect_get_version());
  char buf[1024] = "";
  bool a = true, b = true, c = true;
  bool r = xcselect_get_developer_dir_path(buf, sizeof(buf), &a, &b, &c);
  printf("dev %d [%s] env=%d clt=%d default=%d DEVELOPER_DIR=%s\n", r, r ? buf : "",
         a, b, c, getenv("DEVELOPER_DIR") ? getenv("DEVELOPER_DIR") : "(unset)");
  for (int i = 2; i < argc; i++) {
    bool clt = false;
    buf[0] = 0;
    r = xcselect_find_developer_contents_from_path(argv[i], buf, sizeof(buf), &clt);
    printf("find %s -> %d [%s] clt=%d\n", argv[i], r, r ? buf : "", clt);
    printf("matches(NULL, %s) %d\n", argv[i], xcselect_developer_dir_matches_path(NULL, argv[i]));
    printf("matches(%s, /Applications/Xcode.app/Contents) %d\n", argv[i],
           xcselect_developer_dir_matches_path(argv[i], "/Applications/Xcode.app/Contents"));
    printf("bundle %s %d\n", argv[i], xcselect_bundle_is_developer_tool(argv[i]));
  }
  for (unsigned p = 0; p <= 4; p++) {
    char *path = NULL;
    int e = xcselect_host_sdk_path(p, &path);
    printf("sdk %u -> %d %s\n", p, e, e ? "" : path);
  }
  void *m = xcselect_get_manpaths(NULL);
  printf("manpaths %s\n", m ? "" : "(null)");
  for (unsigned i = 0; m && i < xcselect_manpaths_get_num_paths(m); i++)
    printf("  %s\n", xcselect_manpaths_get_path(m, i));
  if (m)
    xcselect_manpaths_free(m);
  return 0;
}
