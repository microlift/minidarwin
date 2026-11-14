/* libxcselect: finds the active developer directory and runs tools from it.
 * Behaviour follows macOS 27.2's libxcselect (version 2418); see
 * docs/xcselect.md. Links only libSystem. */
#include "xcselect-private.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/un.h>
#include <unistd.h>

#define PATH_LEN 1024
#define CLT_SDKS CLT_DIR "/SDKs"
#define MAX_SDKS 64
#define SDK_PROBE "/usr/include/sys/cdefs.h"
#define APP_DEVELOPER ".app/Contents/Developer"
#define CONTENTS_DEVELOPER "/Contents/Developer"
#define SHIM_SEGMENT "__DATA"
#define SHIM_SECTION "__xcrun_shim"
#define INSTALL_REPLY_LIMIT 65536
#define INSTALL_TIMEOUT_SECONDS 10

static const char *const default_dirs[] = {
    "/Applications/Xcode.app/Contents/Developer",
    CLT_DIR,
    "/Applications/Xcode-beta.app/Contents/Developer",
};

/* Tried, in order, for a tool when there is no developer directory. */
static const char *const fallback_dirs[] = {"/usr/libexec/DeveloperTools",
                                            "/usr/local/bin"};

/* The bundle IDs, after "com.apple.", that count as developer tools. */
static const char bundle_prefix[] = "com.apple.";
static const char *const developer_tool_bundle_ids[] = {
    "dt.Xcode",       "dt.SourceEdit",          "iphonesimulator",
    "dt.Instruments", "FileMerge",              "itunes.connect.ApplicationLoader",
    "AccessibilityInspector", "RealityComposer", "RealityComposerPro",
};

struct xcselect_manpaths {
  unsigned count;
  char **paths;
};

static bool path_exists(const char *path) {
  struct stat st;
  return !stat(path, &st);
}

static bool path_is_dir(const char *path) {
  struct stat st;
  return !stat(path, &st) && S_ISDIR(st.st_mode);
}

static void path_join(char *out, size_t len, const char *a, const char *b) {
  size_t n = strlen(a);
  snprintf(out, len, "%s%s%s", a, n && a[n - 1] == '/' ? "" : "/", b);
}

static void strip_trailing_slashes(char *path) {
  for (size_t n = strlen(path); n > 1 && path[n - 1] == '/'; n--)
    path[n - 1] = 0;
}

static bool ends_with(const char *s, const char *suffix) {
  size_t n = strlen(s), m = strlen(suffix);
  return n >= m && !strcmp(s + n - m, suffix);
}

static bool last_component_is(const char *path, const char *name) {
  const char *slash = strrchr(path, '/');
  return slash && !strcmp(slash + 1, name);
}

static bool path_contains_xcrun(const char *dir) {
  char path[PATH_LEN];
  path_join(path, sizeof(path), dir, XCRUN_LIBRARY);
  if (!access(path, F_OK))
    return true;
  path_join(path, sizeof(path), dir, XCRUN_BINARY);
  return !access(path, F_OK);
}

/* macOS has an App Sandbox check here. MiniDarwin has no App Sandbox. */
static bool sandboxed(void) {
  /* FIXME (ACCURACY): always false; macOS refuses sandboxed callers. */
  return false;
}

XCSELECT_EXPORT const char *xcselect_get_version(void) {
  return XCSELECT_VERSION;
}

XCSELECT_EXPORT bool xcselect_find_developer_contents_from_path(
    const char *path, char *dst, int len, bool *is_cltools) {
  char dir[PATH_LEN], candidate[PATH_LEN];
  if (*path == '/')
    strlcpy(dir, path, sizeof(dir));
  else {
    if (!getcwd(dir, sizeof(dir)))
      return false;
    path_join(candidate, sizeof(candidate), dir, path);
    strlcpy(dir, candidate, sizeof(dir));
  }
  strip_trailing_slashes(dir);

  /* A volume root (/, or one with a kernel) means its Command Line Tools. */
  path_join(candidate, sizeof(candidate), dir, "mach_kernel");
  bool volume = !strcmp(dir, "/") || path_exists(candidate);
  if (volume || last_component_is(dir, "CommandLineTools")) {
    if (volume)
      path_join(dst, (size_t)len, dir, "Library/Developer/CommandLineTools");
    else
      strlcpy(dst, dir, (size_t)len);
    if (!path_contains_xcrun(dst))
      return false;
    *is_cltools = true;
    return true;
  }
  path_join(dst, (size_t)len, dir, "Contents/Developer");
  if (!path_is_dir(dst)) {
    if (path_contains_xcrun(dir))
      strlcpy(dst, dir, (size_t)len);
    else {
      path_join(dst, (size_t)len, dir, "Xcode.app/Contents/Developer");
      if (!path_is_dir(dst))
        return false;
    }
  }
  *is_cltools = false;
  return true;
}

/* 1 when resolution ends at LINK (found or failed), 0 to try the next. */
static int developer_dir_from_link(const char *link, char *buf, int len,
                                   bool *found) {
  char at[PATH_LEN];
  link = xcs_path(link, at, sizeof(at));
  ssize_t n = readlink(link, buf, (size_t)len - 1);
  if (n >= 1) {
    buf[n] = 0;
    *found = path_is_dir(buf);
    return *found;
  }
  if (errno == ENOENT)
    return 0;
  *found = false;
  if (errno == EACCES)
    fprintf(stderr, "xcode-select: error: invalid permissions for data link "
                    "at '%s'\n", link);
  else
    fprintf(stderr, "xcode-select: error: unable to read data link at '%s', "
                    "expected symbolic link (%s)\n", link, strerror(errno));
  return 1;
}

static int developer_dir_from_file(char *buf, int len, bool *found) {
  char at[PATH_LEN];
  int fd = open(xcs_path(SHARE_FILE, at, sizeof(at)), O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT)
      return 0;
    *found = false;
    if (errno == EACCES)
      fputs("xcode-select: error: invalid permissions for data file\n", stderr);
    else
      fprintf(stderr, "xcode-select: error: unable to open data file (%s)\n",
              strerror(errno));
    return 1;
  }
  ssize_t n = read(fd, buf, (size_t)len - 1);
  if (n < 0) {
    fprintf(stderr, "xcode-select: error: unable to read data file (%s)\n",
            strerror(errno));
    close(fd);
    *found = false;
    return 1;
  }
  close(fd);
  if (n && buf[n - 1] == '\n')
    n--;
  buf[n] = 0;
  if (!n || !path_is_dir(buf))
    return 0;
  *found = true;
  return 1;
}

XCSELECT_EXPORT bool xcselect_get_developer_dir_path(char *buf, int len,
                                                     bool *from_env,
                                                     bool *is_cltools,
                                                     bool *is_default) {
  *is_cltools = false;
  *is_default = false;
  const char *env = getenv(DEVELOPER_DIR_ENV);
  if (env && *env) {
    /* An unusable DEVELOPER_DIR is still the answer; xcrun reports it. */
    if (!xcselect_find_developer_contents_from_path(env, buf, len, is_cltools))
      strlcpy(buf, env, (size_t)len);
    else if (strcmp(buf, env))
      setenv(DEVELOPER_DIR_ENV, buf, 1);
    *from_env = true;
    return true;
  }
  *from_env = false;

  bool found = false;
  const char *const links[] = {SELECT_LINK, DB_LINK, SHARE_LINK};
  int done = 0;
  for (size_t i = 0; i < sizeof(links) / sizeof(*links) && !done; i++)
    done = developer_dir_from_link(links[i], buf, len, &found);
  if (!done)
    done = developer_dir_from_file(buf, len, &found);
  if (done) {
    if (found)
      *is_cltools = last_component_is(buf, "CommandLineTools");
    return found;
  }

  *is_default = true;
  for (size_t i = 0; i < sizeof(default_dirs) / sizeof(*default_dirs); i++) {
    char dir[PATH_LEN], library[PATH_LEN];
    const char *at = xcs_path(default_dirs[i], dir, sizeof(dir));
    path_join(library, sizeof(library), at, XCRUN_LIBRARY);
    if (path_exists(library)) {
      strlcpy(buf, at, (size_t)len);
      *is_cltools = !strcmp(default_dirs[i], CLT_DIR);
      return true;
    }
  }
  return false;
}

XCSELECT_EXPORT bool xcselect_developer_dir_matches_path(const char *dev_dir,
                                                         const char *path) {
  char dir[PATH_LEN];
  bool from_env, is_cltools, is_default;
  if (dev_dir)
    strlcpy(dir, dev_dir, sizeof(dir));
  else if (!xcselect_get_developer_dir_path(dir, sizeof(dir), &from_env,
                                            &is_cltools, &is_default))
    return false;
  strip_trailing_slashes(dir);
  /* An Xcode's developer directory stands for the whole application. */
  if (ends_with(dir, APP_DEVELOPER))
    dir[strlen(dir) - strlen(CONTENTS_DEVELOPER)] = 0;
  size_t n = strlen(dir);
  return path && strlen(path) >= n && !strncmp(dir, path, n);
}

XCSELECT_EXPORT bool xcselect_bundle_is_developer_tool(const char *bundle_id) {
  size_t prefix = sizeof(bundle_prefix) - 1;
  if (!bundle_id || strlen(bundle_id) < prefix ||
      strncmp(bundle_id, bundle_prefix, prefix))
    return false;
  for (size_t i = 0; i < sizeof(developer_tool_bundle_ids) /
                             sizeof(*developer_tool_bundle_ids); i++)
    if (!strcmp(bundle_id + prefix, developer_tool_bundle_ids[i]))
      return true;
  return false;
}

/* The host's "major" version as macOS's libxcselect computes it: the number
 * after the first three characters of kern.osproductversion, which is the
 * minor of a 10.x release. */
static long host_version(void) {
  char version[32] = {0};
  size_t len = sizeof(version) - 1;
  if (sysctlbyname("kern.osproductversion", version, &len, NULL, 0))
    return -1;
  return strlen(version) > 3 ? strtol(version + 3, NULL, 10) : -1;
}

/* SDKs are the entries of DIR with usr/include/sys/cdefs.h, in readdir
 * order, which is what "latest" falls back to among unversioned names. */
static size_t sdks_at_path(const char *dir, char **out, size_t max) {
  DIR *d = opendir(dir);
  if (!d)
    return 0;
  size_t n = 0;
  for (struct dirent *e; n < max && (e = readdir(d));) {
    char *probe;
    if (asprintf(&probe, "%s/%s" SDK_PROBE, dir, e->d_name) < 0)
      break;
    if (path_exists(probe)) {
      probe[strlen(probe) - strlen(SDK_PROBE)] = 0;
      out[n++] = probe;
    } else
      free(probe);
  }
  closedir(d);
  return n;
}

/* Only MacOSX10.N.sdk names carry a version, N; any other name is 0. */
static long sdk_version(const char *path) {
  const char *name = strrchr(path, '/') + 1;
  if (strncmp(name, "MacOSX10.", 9))
    return 0;
  char *copy = strdup(name + 9), *dot = strchr(copy, '.');
  long v = dot && !strcmp(dot, ".sdk") ? (*dot = 0, strtol(copy, NULL, 10)) : 0;
  free(copy);
  return v;
}

XCSELECT_EXPORT errno_t xcselect_host_sdk_path(xcselect_host_sdk_policy_t policy,
                                               char **path) {
  if (!path || policy < XCSELECT_HOST_SDK_POLICY_MATCHING_PREFERRED ||
      policy > XCSELECT_HOST_SDK_POLICY_LATEST)
    return EINVAL;
  char **sdks = calloc(MAX_SDKS, sizeof(char *));
  if (!sdks)
    return ENOMEM;
  long host = host_version();
  char dev[PATH_LEN], at[PATH_LEN];
  bool from_env, is_cltools, is_default;
  size_t n = 0;
  /* Only a chosen Xcode's SDKs; the Command Line Tools' always. */
  if (xcselect_get_developer_dir_path(dev, sizeof(dev), &from_env, &is_cltools,
                                      &is_default) &&
      !is_cltools && !is_default) {
    char *dir;
    if (asprintf(&dir, "%s/Platforms/MacOSX.platform/Developer/SDKs", dev) >= 0) {
      n = sdks_at_path(dir, sdks, MAX_SDKS);
      free(dir);
    }
  }
  n += sdks_at_path(xcs_path(CLT_SDKS, at, sizeof(at)), sdks + n, MAX_SDKS - n);

  char *latest = NULL, *matching = NULL;
  long latest_version = 0;
  for (size_t i = 0; i < n; i++) {
    long v = sdk_version(sdks[i]);
    if (!latest || v > latest_version) {
      latest = sdks[i];
      latest_version = v;
    }
    if (!matching && v == host)
      matching = sdks[i];
  }
  char *chosen = policy == XCSELECT_HOST_SDK_POLICY_LATEST ? latest
                 : policy == XCSELECT_HOST_SDK_POLICY_MATCHING_ONLY
                     ? matching
                     : matching ? matching : latest;
  for (size_t i = 0; i < n; i++)
    if (sdks[i] != chosen)
      free(sdks[i]);
  free(sdks);
  if (!chosen)
    return ENOENT;
  *path = chosen;
  return 0;
}

static void manpaths_append(xcselect_manpaths *m, const char *path) {
  char **grown = realloc(m->paths, (m->count + 1) * sizeof(char *));
  char *copy = strdup(path);
  if (!grown || !copy) {
    free(copy);
    if (grown)
      m->paths = grown;
    return;
  }
  m->paths = grown;
  m->paths[m->count++] = copy;
}

typedef void (*iter_manpaths_fn)(const char *dev, const char *sdk,
                                 void (^)(const char *));

XCSELECT_EXPORT xcselect_manpaths *xcselect_get_manpaths(const char *sdk) {
  char dev[PATH_LEN], library[PATH_LEN], path[PATH_LEN];
  bool from_env, is_cltools, is_default;
  if (sandboxed() || !xcselect_get_developer_dir_path(dev, sizeof(dev), &from_env,
                                                      &is_cltools, &is_default))
    return NULL;
  path_join(library, sizeof(library), dev, XCRUN_LIBRARY);
  if (path_exists(library)) {
    void *handle = dlopen(library, RTLD_LAZY | RTLD_LOCAL);
    if (!handle) {
      fprintf(stderr, "%s: error: unable to load libxcrun (%s).\n",
              getprogname(), dlerror());
      return NULL;
    }
    iter_manpaths_fn iter = (iter_manpaths_fn)dlsym(handle, "xcrun_iter_manpaths");
    if (iter) {
      xcselect_manpaths *m = calloc(1, sizeof(*m));
      if (m)
        iter(dev, sdk, ^(const char *p) {
          manpaths_append(m, p);
        });
      dlclose(handle);
      return m;
    }
    dlclose(handle);
  }
  xcselect_manpaths *m = calloc(1, sizeof(*m));
  if (!m)
    return NULL;
  const char *const standard[] = {
      "usr/share/man", "usr/llvm-gcc-4.2/share/man",
      "Toolchains/XcodeDefault.xctoolchain/usr/share/man"};
  for (size_t i = 0; i < sizeof(standard) / sizeof(*standard); i++) {
    path_join(path, sizeof(path), dev, standard[i]);
    manpaths_append(m, path);
  }
  return m;
}

XCSELECT_EXPORT unsigned xcselect_manpaths_get_num_paths(xcselect_manpaths *m) {
  return m->count;
}

XCSELECT_EXPORT const char *xcselect_manpaths_get_path(xcselect_manpaths *m,
                                                       unsigned index) {
  return index < m->count ? m->paths[index] : NULL;
}

XCSELECT_EXPORT void xcselect_manpaths_free(xcselect_manpaths *m) {
  if (!m)
    return;
  for (unsigned i = 0; i < m->count; i++)
    free(m->paths[i]);
  free(m->paths);
  free(m);
}

/* Install requests go to whatever serves the install socket (see
 * docs/xcselect.md): one frame each way, a big-endian uint32 length and an
 * XML property list, as in MiniDarwin's open protocol. */
static bool write_all(int fd, const void *p, size_t len) {
  for (const char *c = p; len;) {
    ssize_t n = write(fd, c, len);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    c += n;
    len -= (size_t)n;
  }
  return true;
}

static bool read_all(int fd, void *p, size_t len) {
  for (char *c = p; len;) {
    ssize_t n = read(fd, c, len);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    c += n;
    len -= (size_t)n;
  }
  return true;
}

static void append_escaped(char *out, size_t len, const char *s) {
  size_t n = strlen(out);
  for (; *s && n + 7 < len; s++) {
    const char *e = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : NULL;
    if (e) {
      memcpy(out + n, e, strlen(e));
      n += strlen(e);
    } else
      out[n++] = *s;
  }
  out[n] = 0;
}

/* The reply's OK: the dictionary's <key>OK</key> must be followed by <true/>. */
static bool reply_ok(const char *reply) {
  const char *key = strstr(reply, "<key>OK</key>");
  if (!key)
    return false;
  key += strlen("<key>OK</key>");
  while (*key == ' ' || *key == '\t' || *key == '\n' || *key == '\r')
    key++;
  return !strncmp(key, "<true/>", 7);
}

XCSELECT_EXPORT bool xcselect_trigger_install_request(const char *tool_name) {
  char at[PATH_LEN];
  const char *socket_path = getenv(INSTALL_SOCKET_ENV);
  if (!socket_path || !*socket_path)
    socket_path = xcs_path(INSTALL_SOCKET, at, sizeof(at));
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  if (strlen(socket_path) >= sizeof(addr.sun_path))
    return false;
  strlcpy(addr.sun_path, socket_path, sizeof(addr.sun_path));
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return false;
  struct timeval timeout = {INSTALL_TIMEOUT_SECONDS, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  bool ok = false;
  if (!connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
    char request[2048] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<plist version=\"1.0\"><dict><key>Version</key><integer>1</integer>";
    if (tool_name) {
      strlcat(request, "<key>ToolName</key><string>", sizeof(request));
      append_escaped(request, sizeof(request) - 32, tool_name);
      strlcat(request, "</string>", sizeof(request));
    }
    strlcat(request, "</dict></plist>\n", sizeof(request));
    uint32_t length = htonl((uint32_t)strlen(request)), reply_length;
    if (write_all(fd, &length, sizeof(length)) &&
        write_all(fd, request, strlen(request)) &&
        read_all(fd, &reply_length, sizeof(reply_length))) {
      reply_length = ntohl(reply_length);
      char *reply = reply_length && reply_length <= INSTALL_REPLY_LIMIT
                        ? calloc(reply_length + 1, 1) : NULL;
      ok = reply && read_all(fd, reply, reply_length) && reply_ok(reply);
      free(reply);
    }
  }
  close(fd);
  return ok;
}

/* True if PATH is a Mach-O (thin or fat) with a __DATA,__xcrun_shim section:
 * the marker every shim carries, so that a shim never runs another. */
static bool slice_is_shim(int fd, off_t base) {
  struct mach_header_64 header;
  if (pread(fd, &header, sizeof(header), base) != (ssize_t)sizeof(header) ||
      header.magic != MH_MAGIC_64 || header.sizeofcmds > (1u << 20))
    return false;
  char *commands = malloc(header.sizeofcmds);
  bool found = false;
  if (commands && pread(fd, commands, header.sizeofcmds,
                        base + (off_t)sizeof(header)) == (ssize_t)header.sizeofcmds) {
    uint32_t offset = 0;
    for (uint32_t i = 0; i < header.ncmds && !found; i++) {
      if (header.sizeofcmds - offset < sizeof(struct load_command))
        break;
      struct load_command *lc = (struct load_command *)(commands + offset);
      if (lc->cmdsize < sizeof(*lc) || lc->cmdsize > header.sizeofcmds - offset)
        break;
      if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(struct segment_command_64)) {
        struct segment_command_64 *seg = (struct segment_command_64 *)lc;
        struct section_64 *sect = (struct section_64 *)(seg + 1);
        uint64_t room = (lc->cmdsize - sizeof(*seg)) / sizeof(*sect);
        for (uint32_t s = 0; s < seg->nsects && s < room; s++)
          if (!strncmp(sect[s].segname, SHIM_SEGMENT, sizeof(sect[s].segname)) &&
              !strncmp(sect[s].sectname, SHIM_SECTION, sizeof(sect[s].sectname)))
            found = true;
      }
      offset += lc->cmdsize;
    }
  }
  free(commands);
  return found;
}

static bool is_path_xcrun_shim(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return false;
  uint32_t magic = 0;
  bool shim = false;
  if (pread(fd, &magic, sizeof(magic), 0) == (ssize_t)sizeof(magic)) {
    if (magic == FAT_CIGAM) {
      struct fat_header fat;
      if (pread(fd, &fat, sizeof(fat), 0) == (ssize_t)sizeof(fat)) {
        uint32_t count = ntohl(fat.nfat_arch);
        for (uint32_t i = 0; i < count && i < 64 && !shim; i++) {
          struct fat_arch arch;
          if (pread(fd, &arch, sizeof(arch),
                    (off_t)(sizeof(fat) + i * sizeof(arch))) != (ssize_t)sizeof(arch))
            break;
          shim = slice_is_shim(fd, (off_t)ntohl(arch.offset));
        }
      }
    } else
      shim = slice_is_shim(fd, 0);
  }
  close(fd);
  return shim;
}

static char **with_program(const char *program, const char *tool, int argc,
                           char **argv) {
  char **out = calloc((size_t)argc + 3, sizeof(char *));
  if (!out) {
    fputs("xcrun: error: out of memory\n", stderr);
    exit(1);
  }
  int n = 0;
  out[n++] = (char *)program;
  if (tool)
    out[n++] = (char *)tool;
  for (int i = 0; i < argc; i++)
    out[n++] = argv[i];
  return out;
}

static const char no_tools_requested[] =
    "xcode-select: note: No developer tools were found, requesting install.\n"
    "If developer tools are located at a non-default location on disk, use "
    "`sudo xcode-select --switch path/to/Xcode.app` to specify the Xcode that "
    "you wish to use for command line developer tools, and cancel the "
    "installation dialog.\n"
    "See `man xcode-select` for more details.\n";
static const char no_tools_unrequested[] =
    "xcode-select: error: No developer tools were found and no install could "
    "be requested (possibly because there is no active GUI session).\n"
    "If developer tools are located at a non-default location on disk, use "
    "`sudo xcode-select --switch path/to/Xcode.app` to specify the Xcode that "
    "you wish to use for command line developer tools.\n"
    "Use `xcode-select --install` to install the standalone command line "
    "developer tools, or visit http://adc.apple.com to download Xcode or the "
    "standalone command line tools installation package.\n"
    "See `man xcode-select` for more details.\n";

static _Noreturn void request_install(const char *tool) {
  fputs(xcselect_trigger_install_request(tool) ? no_tools_requested
                                               : no_tools_unrequested, stderr);
  exit(1);
}

typedef void (*xcrun_main_fn)(const char *tool, int argc, char **argv,
                              const char *dev);
typedef void (*unknown_utility_fn)(void (^)(const char *));

static _Noreturn void invoke_via_library(const char *library, const char *tool,
                                         int argc, char **argv, const char *dev) {
  void *handle = dlopen(library, RTLD_LAZY | RTLD_LOCAL);
  if (!handle) {
    fprintf(stderr, "xcrun: error: unable to load libxcrun (%s).\n", dlerror());
    exit(1);
  }
  xcrun_main_fn xcrun_main = (xcrun_main_fn)dlsym(handle, "xcrun_main");
  if (!xcrun_main) {
    fprintf(stderr, "xcrun: error: unable to resolve xcrun_main (%s).\n",
            dlerror());
    exit(1);
  }
  if (!strcmp(getprogname(), "xcrun")) {
    unknown_utility_fn set_handler = (unknown_utility_fn)dlsym(
        handle, "xcrun_set_unknown_utility_handler");
    if (set_handler)
      set_handler(^(const char *name) {
        if (xcselect_trigger_install_request(name))
          fprintf(stderr, "xcode-select: Failed to locate '%s', requesting "
                          "installation of command line developer tools.\n",
                  name);
        else
          fprintf(stderr, "xcode-select: Failed to locate '%s', and no install "
                          "could be requested (perhaps no UI is present). "
                          "Please install manually from "
                          "'developer.apple.com'.\n", name);
      });
  }
  xcrun_main(tool, argc, argv, dev);
  fputs("xcrun: error: unexpected exit from xcrun_main\n", stderr);
  exit(1);
}

static _Noreturn void invoke_via_binary(const char *xcrun, char **argv,
                                        const char *dev, bool from_env) {
  execv(xcrun, argv);
  if (errno != ENOENT) {
    fprintf(stderr, "xcrun: error: unable to exec Xcode native xcrun (%s).\n",
            strerror(errno));
  } else if (path_is_dir(dev)) {
    fprintf(stderr, "xcrun: error: invalid %s path (%s), missing xcrun at: %s\n",
            from_env ? DEVELOPER_DIR_ENV : "active developer", dev, xcrun);
  } else if (from_env) {
    fprintf(stderr, "xcrun: error: missing DEVELOPER_DIR path: %s\n", dev);
  } else {
    fprintf(stderr,
            "xcrun: error: active developer path (\"%s\") %s\n"
            "Use `sudo xcode-select --switch path/to/Xcode.app` to specify the "
            "Xcode that you wish to use for command line developer tools, or use "
            "`xcode-select --install` to install the standalone command line "
            "developer tools.\n"
            "See `man xcode-select` for more details.\n",
            dev, errno == ENOENT ? "does not exist" : "is invalid");
  }
  exit(1);
}

XCSELECT_EXPORT _Noreturn int xcselect_invoke_xcrun(const char *tool, int argc,
                                                    char **argv, int flags) {
  if (sandboxed()) {
    fputs("xcrun: error: cannot be used within an App Sandbox.\n", stderr);
    exit(1);
  }
  char dev[PATH_LEN], path[PATH_LEN];
  bool from_env, is_cltools, is_default;
  if (!xcselect_get_developer_dir_path(dev, sizeof(dev), &from_env, &is_cltools,
                                       &is_default)) {
    if (!tool)
      request_install("xcrun");
    for (size_t i = 0; i < sizeof(fallback_dirs) / sizeof(*fallback_dirs); i++) {
      char at[PATH_LEN];
      path_join(path, sizeof(path), xcs_path(fallback_dirs[i], at, sizeof(at)), tool);
      if (path_exists(path)) {
        execv(path, with_program(path, NULL, argc, argv));
        fprintf(stderr, "xcrun: error: unable to exec %s (%s).\n", path,
                strerror(errno));
        exit(1);
      }
    }
    request_install(getprogname());
  }
  if ((flags & XCSELECT_FLAG_REQUIRE_XCODE) && is_cltools) {
    fprintf(stderr, "xcode-select: error: tool '%s' requires Xcode, but active "
                    "developer directory '%s' is a command line tools instance\n",
            tool, dev);
    exit(1);
  }
  /* A shim's own xcrun logging would describe the shim, not the tool. */
  if (tool)
    unsetenv("xcrun_log");
  path_join(path, sizeof(path), dev, XCRUN_LIBRARY);
  if (path_exists(path))
    invoke_via_library(path, tool, argc, argv, dev);
  path_join(path, sizeof(path), dev, XCRUN_BINARY);
  if (is_path_xcrun_shim(path)) {
    fprintf(stderr, "xcode-select: error: malformed developer path (\"%s\")\n",
            dev);
    exit(1);
  }
  invoke_via_binary(path, with_program(path, tool, argc, argv), dev, from_env);
}
