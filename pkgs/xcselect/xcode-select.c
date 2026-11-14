/* xcode-select: prints or changes the active developer directory. Options,
 * messages and exit statuses are macOS 27.2's (xcode-select version 2418). */
#include "xcselect-private.h"

#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATH_LEN 1024
#define INSTALLER "/usr/libexec/xcselect-install"
#define OPTION_WIDTH 27

enum { PRINT_PATH, SWITCH, INSTALL, VERSION, RESET, MANPATHS, HELP, ACTIONS };

/* The links -s writes and -r removes, newest reader's first. */
static const char *const data_links[] = {SELECT_LINK, DB_LINK};

static _Noreturn void usage(int status) {
  fprintf(stderr, "Usage: %s [options]\n\n", getprogname());
  fputs("Print or change the path to the active developer directory. This "
        "directory\ncontrols which tools are used for the Xcode command line "
        "tools (for example, \nxcodebuild) as well as the BSD development "
        "commands (such as cc and make).\n\n", stderr);
  fputs("Options:\n", stderr);
  const char *const options[][2] = {
      {"-h, --help", "print this help message and exit"},
      {"-p, --print-path", "print the path of the active developer directory"},
      {"-s <path>, --switch <path>", "set the path for the active developer directory"},
      {"--install", "open a dialog for installation of the command line developer tools"},
      {"-v, --version", "print the xcode-select version"},
      {"-r, --reset", "reset to the default command line tools path"},
  };
  for (size_t i = 0; i < sizeof(options) / sizeof(*options); i++)
    fprintf(stderr, "  %-*s %s\n", OPTION_WIDTH, options[i][0], options[i][1]);
  exit(status);
}

static int print_path(void) {
  char dev[PATH_LEN];
  bool from_env, is_cltools, is_default;
  if (!xcselect_get_developer_dir_path(dev, sizeof(dev), &from_env, &is_cltools,
                                       &is_default)) {
    fprintf(stderr, "%s: error: Unable to get active developer directory. Use "
                    "`sudo xcode-select --switch path/to/Xcode.app` to set one "
                    "(or see `man xcode-select`)\n", getprogname());
    return 2;
  }
  printf("%s\n", dev);
  return 0;
}

static int show_manpaths(void) {
  xcselect_manpaths *m = xcselect_get_manpaths(NULL);
  if (!m) {
    fprintf(stderr, "%s: error: unable to get manpaths\n", getprogname());
    return 1;
  }
  for (unsigned i = 0; i < xcselect_manpaths_get_num_paths(m); i++)
    printf("%s\n", xcselect_manpaths_get_path(m, i));
  xcselect_manpaths_free(m);
  return 0;
}

static int switch_to(const char *requested) {
  char dev[PATH_LEN];
  bool is_cltools = false;
  if (!xcselect_find_developer_contents_from_path(requested, dev, sizeof(dev),
                                                  &is_cltools)) {
    fprintf(stderr, "%s: error: invalid developer directory '%s'\n",
            getprogname(), requested);
    return 1;
  }
  if (xcs_euid()) {
    fprintf(stderr, "%s: error: --switch must be run as root (e.g. `sudo %s "
                    "--switch <xcode_folder_path>`).\n", getprogname(),
            getprogname());
    return 1;
  }
  for (size_t i = 0; i < sizeof(data_links) / sizeof(*data_links); i++) {
    char at[PATH_LEN];
    const char *link = xcs_path(data_links[i], at, sizeof(at));
    if (unlink(link) && errno != ENOENT) {
      fprintf(stderr, "%s: error: unable to remove existing data link at '%s' "
                      "(%s).\n", getprogname(), link, strerror(errno));
      return 2;
    }
    mode_t mask = umask(022);
    int failed = symlink(dev, link);
    int error = errno;
    umask(mask);
    if (failed) {
      fprintf(stderr, "%s: error: unable to create data link (%s).\n",
              getprogname(), strerror(error));
      return 2;
    }
  }
  /* FIXME (ACCURACY): macOS also marks the chosen Xcode as used for
   * Spotlight (MDItemMarkAsUsed); MiniDarwin has no metadata store. */
  return 0;
}

static int reset(void) {
  if (xcs_euid()) {
    fprintf(stderr, "%s: error: --reset must be run as root (e.g. `sudo %s "
                    "--reset`).\n", getprogname(), getprogname());
    return 1;
  }
  const char *const files[] = {SELECT_LINK, DB_LINK, SHARE_LINK, SHARE_FILE};
  for (size_t i = 0; i < sizeof(files) / sizeof(*files); i++) {
    char at[PATH_LEN];
    const char *path = xcs_path(files[i], at, sizeof(at));
    if (unlink(path) && errno != ENOENT) {
      fprintf(stderr, "%s: error: unable to remove file at '%s' (%s).\n",
              getprogname(), path, strerror(errno));
      return 2;
    }
  }
  return 0;
}

static int install(void) {
  char at[PATH_LEN];
  if (!access(xcs_path(CLT_LIBXCRUN, at, sizeof(at)), F_OK)) {
    fprintf(stderr, "%s: note: Command line tools are already installed. Use "
                    "\"Software Update\" in System Settings or the "
                    "softwareupdate command line interface to install "
                    "updates\n", getprogname());
    return 1;
  }
  if (xcselect_trigger_install_request("xcode-select")) {
    fputs("xcode-select: note: install requested for command line developer "
          "tools\n", stderr);
    return 0;
  }
  /* macOS reports that no install could be requested. With no installer
   * agent and no GUI, MiniDarwin runs its own installer here instead. */
  const char *installer = xcs_path(INSTALLER, at, sizeof(at));
  if (!access(installer, X_OK)) {
    execl(installer, installer, (char *)NULL);
    fprintf(stderr, "%s: error: cannot run %s (%s)\n", getprogname(), installer,
            strerror(errno));
    return 1;
  }
  fputs("xcode-select: error: no developer tools were found, and no install "
        "could be requested (perhaps no UI is present), please install "
        "manually from 'developer.apple.com'.\n", stderr);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fputs("xcode-select: error: no command option given\n", stderr);
    usage(1);
  }
  bool actions[ACTIONS] = {false};
  const char *switch_path = NULL;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    int action = !strcmp(a, "-h") || !strcmp(a, "--help")         ? HELP
                 : !strcmp(a, "-v") || !strcmp(a, "-version") ||
                         !strcmp(a, "--version")                  ? VERSION
                 : !strcmp(a, "-p") || !strcmp(a, "-print-path") ||
                         !strcmp(a, "--print-path")               ? PRINT_PATH
                 : !strcmp(a, "--show-manpaths")                  ? MANPATHS
                 : !strcmp(a, "--install")                        ? INSTALL
                 : !strcmp(a, "-r") || !strcmp(a, "--reset")      ? RESET
                 : !strcmp(a, "-s") || !strcmp(a, "-switch") ||
                         !strcmp(a, "--switch")                   ? SWITCH
                                                                  : -1;
    if (action < 0) {
      fprintf(stderr, "%s: error: invalid argument '%s'\n", getprogname(), a);
      usage(1);
    }
    if (action == SWITCH) {
      if (i + 1 >= argc) {
        fprintf(stderr, "%s: error: missing argument to '%s'\n", getprogname(), a);
        usage(1);
      }
      switch_path = argv[++i];
    }
    actions[action] = true;
  }
  int count = 0;
  for (int a = 0; a < ACTIONS; a++)
    count += actions[a];
  if (count > 1) {
    fputs("xcode-select: error: cannot execute multiple actions\n", stderr);
    usage(1);
  }
  if (actions[HELP])
    usage(0);
  if (actions[VERSION]) {
    printf("%s version %s.\n", getprogname(), xcselect_get_version());
    return 0;
  }
  if (actions[PRINT_PATH])
    return print_path();
  if (actions[MANPATHS])
    return show_manpaths();
  if (actions[INSTALL])
    return install();
  if (actions[RESET])
    return reset();
  return switch_to(switch_path);
}
