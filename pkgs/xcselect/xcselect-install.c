/* xcselect-install: installs Apple's Command Line Tools into the running
 * root, the way macOS's install-on-demand agent does: the Software Update
 * catalog names the products, the newest one whose Distribution allows the
 * target's macOS is chosen, its licence is shown and must be accepted, and
 * mdpkg installs it. Each package must be signed by Apple's Software Update
 * certificate (mdpkg -verify-signature apple-software-update); the catalog
 * and Distribution come over HTTPS from Apple's own hosts. */
#include "xcselect-private.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <openssl/evp.h>
#include <spawn.h>
#include <stdarg.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

#define PATH_LEN 1024
#define CURL "/usr/bin/curl"
#define MDPKG "/usr/bin/mdpkg"
#define CATALOG_BASE "https://swscan.apple.com/content/catalogs/others/"
#define CATALOG_TAIL \
  "10.16-10.15-10.14-10.13-10.12-10.11-10.10-10.9-mountainlion-lion-" \
  "snowleopard-leopard.merged-1.sucatalog.gz"
#define CATALOG_LIMIT (256u << 20)
#define DISTRIBUTION_LIMIT (4u << 20)
#define CLT_TAG "com.apple.dt.commandlinetools"
#define CLT_PACKAGE "CLTools_Executables"
#define IN_PROGRESS "/tmp/.com.apple.dt.CommandLineTools.installondemand.in-progress"
#define SYSTEM_VERSION "/System/Library/CoreServices/SystemVersion.plist"
#define LICENSE_AGREEMENT "agree"

extern char **environ;

/* macOS releases with a Software Update catalog, newest first; a catalog
 * names its release and every older one. */
static const int catalog_releases[] = {27, 26, 15, 14, 13, 12};

static const char *const apple_hosts[] = {
    "https://swscan.apple.com/", "https://swdist.apple.com/",
    "https://swcdn.apple.com/"};

typedef struct {
  char *url, *name, *digest;
  unsigned long long size;
} Package;

typedef struct {
  char *id, *distribution_url, *distribution, *version, *ranges;
  Package *packages;
  size_t count;
  int allowed;
} Product;

static char *work_dir;
static int work_dir_is_temporary;

static _Noreturn void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("xcselect-install: ", stderr);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(1);
}

static char *xstrdup(const char *s) {
  char *copy = strdup(s);
  if (!copy)
    die("out of memory");
  return copy;
}

static char *path_in(const char *dir, const char *name) {
  char *path;
  if (asprintf(&path, "%s/%s", dir, name) < 0)
    die("out of memory");
  return path;
}

/* "1.2.3" against "1.10": numeric dotted components, missing ones as 0. */
static int compare_versions(const char *a, const char *b) {
  while (*a || *b) {
    unsigned long x = strtoul(a, (char **)&a, 10), y = strtoul(b, (char **)&b, 10);
    if (x != y)
      return x < y ? -1 : 1;
    a += *a == '.';
    b += *b == '.';
    if ((*a && (*a < '0' || *a > '9')) || (*b && (*b < '0' || *b > '9')))
      return strcmp(a, b);
  }
  return 0;
}

static int valid_version(const char *s) {
  if (!*s)
    return 0;
  for (; *s; s++)
    if ((*s < '0' || *s > '9') && *s != '.')
      return 0;
  return 1;
}

static int run(char *const argv[]) {
  pid_t pid;
  int error = posix_spawn(&pid, argv[0], NULL, NULL, argv, environ);
  if (error)
    die("cannot run %s: %s", argv[0], strerror(error));
  int status;
  while (waitpid(pid, &status, 0) < 0)
    if (errno != EINTR)
      die("waitpid: %s", strerror(errno));
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static void check_host(const char *url) {
  for (size_t i = 0; i < sizeof(apple_hosts) / sizeof(*apple_hosts); i++)
    if (!strncmp(url, apple_hosts[i], strlen(apple_hosts[i])))
      return;
#ifdef XCSELECT_TESTING
  if (!strncmp(url, "file://", 7))
    return;
#endif
  die("refusing a URL that is not on Apple's Software Update hosts: %s", url);
}

/* Downloads with MiniDarwin's curl, HTTPS only, redirects included. */
static void download(const char *url, const char *dest, unsigned long long limit,
                     int progress) {
  check_host(url);
  char max[32];
  snprintf(max, sizeof(max), "%llu", limit);
#ifdef XCSELECT_TESTING
  const char *protocols = "=https,file";
  const char *curl = getenv("XCSELECT_TEST_CURL") ? getenv("XCSELECT_TEST_CURL") : CURL;
#else
  const char *protocols = "=https";
  const char *curl = CURL;
#endif
  char *const argv[] = {(char *)curl, "--fail", "--location", "--proto",
                        (char *)protocols, "--proto-redir", "=https",
                        "--max-filesize", max, "--silent", "--show-error",
                        progress ? "--progress-bar" : "--no-progress-meter",
                        "--output", (char *)dest, (char *)url, NULL};
  if (run(argv))
    die("could not download %s", url);
}

static unsigned char *read_file(const char *path, size_t limit, size_t *len) {
  int fd = open(path, O_RDONLY);
  struct stat st;
  if (fd < 0 || fstat(fd, &st))
    die("cannot read %s: %s", path, strerror(errno));
  if ((unsigned long long)st.st_size > limit)
    die("%s is larger than expected", path);
  unsigned char *data = malloc((size_t)st.st_size + 1);
  if (!data)
    die("out of memory");
  size_t done = 0;
  while (done < (size_t)st.st_size) {
    ssize_t n = read(fd, data + done, (size_t)st.st_size - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      die("cannot read %s", path);
    done += (size_t)n;
  }
  close(fd);
  data[done] = 0;
  *len = done;
  return data;
}

static unsigned char *gunzip_file(const char *path, size_t limit, size_t *len) {
  gzFile gz = gzopen(path, "rb");
  if (!gz)
    die("cannot open %s", path);
  size_t cap = 1 << 20, n = 0;
  unsigned char *out = malloc(cap + 1);
  for (;;) {
    if (!out)
      die("out of memory");
    int got = gzread(gz, out + n, (unsigned)(cap - n));
    if (got < 0)
      die("corrupt catalog %s", path);
    n += (size_t)got;
    if (n < cap)
      break;
    if (cap >= limit)
      die("catalog is larger than expected");
    cap *= 2;
    out = realloc(out, cap + 1);
  }
  gzclose(gz);
  out[n] = 0;
  *len = n;
  return out;
}

#define XAR_HEADER_SIZE 28

/* The catalog's Digest is a package's XAR TOC checksum: the SHA-1 of its
 * compressed table of contents, which is what Apple's signature signs. */
static char *toc_digest(const char *path) {
  int fd = open(path, O_RDONLY);
  unsigned char header[XAR_HEADER_SIZE];
  if (fd < 0 || pread(fd, header, sizeof(header), 0) != (ssize_t)sizeof(header) ||
      memcmp(header, "xar!", 4)) {
    if (fd >= 0)
      close(fd);
    return NULL;
  }
  uint64_t size = (uint64_t)header[4] << 8 | header[5], packed = 0;
  for (int i = 8; i < 16; i++)
    packed = packed << 8 | header[i];
  if (packed > (64u << 20)) {
    close(fd);
    return NULL;
  }
  unsigned char *toc = malloc((size_t)packed + 1);
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned len = 0;
  int ok = toc && pread(fd, toc, (size_t)packed, (off_t)size) == (ssize_t)packed &&
           EVP_Digest(toc, (size_t)packed, md, &len, EVP_sha1(), NULL);
  free(toc);
  close(fd);
  if (!ok)
    return NULL;
  char *hex = malloc(len * 2 + 1);
  for (unsigned i = 0; i < len; i++)
    snprintf(hex + i * 2, 3, "%02x", md[i]);
  return hex;
}

static int package_is_current(const Package *p, const char *path) {
  struct stat st;
  if (stat(path, &st) || (unsigned long long)st.st_size != p->size)
    return 0;
  if (!p->digest)
    return 1;
  char *got = toc_digest(path);
  int same = got && !strcasecmp(got, p->digest);
  free(got);
  return same;
}

/* Property lists. */
static xmlNode *element(xmlNode *n) {
  while (n && n->type != XML_ELEMENT_NODE)
    n = n->next;
  return n;
}

static int named(xmlNode *n, const char *name) {
  return n && !xmlStrcmp(n->name, BAD_CAST name);
}

static xmlNode *plist_get(xmlNode *dict, const char *key) {
  if (!named(dict, "dict"))
    return NULL;
  for (xmlNode *k = element(dict->children); k; k = element(k->next)) {
    xmlNode *value = element(k->next);
    if (!value)
      return NULL;
    xmlChar *text = xmlNodeGetContent(k);
    int match = named(k, "key") && !xmlStrcmp(text, BAD_CAST key);
    xmlFree(text);
    if (match)
      return value;
    k = value;
  }
  return NULL;
}

static char *plist_string(xmlNode *dict, const char *key) {
  xmlNode *v = plist_get(dict, key);
  if (!named(v, "string") && !named(v, "integer"))
    return NULL;
  xmlChar *text = xmlNodeGetContent(v);
  char *s = xstrdup((const char *)text);
  xmlFree(text);
  return s;
}

static xmlDoc *parse_xml(const unsigned char *data, size_t len, const char *what) {
  if (len > INT32_MAX)
    die("%s is too large", what);
  xmlDoc *doc = xmlReadMemory((const char *)data, (int)len, what, NULL,
                              XML_PARSE_NONET);
  if (!doc)
    die("%s is not valid XML", what);
  return doc;
}

/* Target. */
static char *target_version(const char *root) {
  char *path = path_in(root, &SYSTEM_VERSION[1]);
  size_t len;
  unsigned char *data = read_file(path, 1 << 20, &len);
  xmlDoc *doc = parse_xml(data, len, path);
  char *version = plist_string(element(xmlDocGetRootElement(doc)->children),
                               "ProductVersion");
  if (!version || !valid_version(version))
    die("%s has no ProductVersion", path);
  xmlFreeDoc(doc);
  free(data);
  free(path);
  return version;
}

static char *catalog_url(const char *version) {
  int major = atoi(version);
  size_t first = 0, count = sizeof(catalog_releases) / sizeof(*catalog_releases);
  while (first < count && catalog_releases[first] != major)
    first++;
  if (first == count)
    die("no Software Update catalog is known for macOS %s", version);
  char *url = xstrdup(CATALOG_BASE "index");
  for (size_t i = first; i < count; i++) {
    char *next;
    if (asprintf(&next, "%s-%d", url, catalog_releases[i]) < 0)
      die("out of memory");
    free(url);
    url = next;
  }
  char *full;
  if (asprintf(&full, "%s-" CATALOG_TAIL, url) < 0)
    die("out of memory");
  free(url);
  return full;
}

/* Catalog: products with a CLTools_Executables package and an English
 * Distribution. */
static Product *catalog_products(xmlDoc *catalog, size_t *count) {
  xmlNode *products = plist_get(element(xmlDocGetRootElement(catalog)->children),
                                "Products");
  Product *out = NULL;
  *count = 0;
  for (xmlNode *k = element(products ? products->children : NULL); k;
       k = element(k->next)) {
    xmlNode *product = element(k->next);
    if (!product)
      break;
    xmlNode *packages = plist_get(product, "Packages");
    xmlNode *distributions = plist_get(product, "Distributions");
    char *english = distributions ? plist_string(distributions, "English") : NULL;
    if (!english)
      english = distributions ? plist_string(distributions, "en") : NULL;
    Product p = {0};
    int clt = 0;
    for (xmlNode *pkg = element(packages ? packages->children : NULL); pkg;
         pkg = element(pkg->next)) {
      char *url = plist_string(pkg, "URL"), *size = plist_string(pkg, "Size");
      if (!url || !size)
        die("catalog package without a URL or Size");
      const char *name = strrchr(url, '/') ? strrchr(url, '/') + 1 : url;
      if (!strncmp(name, CLT_PACKAGE, strlen(CLT_PACKAGE)))
        clt = 1;
      p.packages = realloc(p.packages, (p.count + 1) * sizeof(Package));
      p.packages[p.count++] = (Package){url, xstrdup(name),
                                        plist_string(pkg, "Digest"),
                                        strtoull(size, NULL, 10)};
      free(size);
    }
    if (clt && english) {
      xmlChar *id = xmlNodeGetContent(k);
      p.id = xstrdup((const char *)id);
      xmlFree(id);
      p.distribution_url = english;
      out = realloc(out, (*count + 1) * sizeof(Product));
      out[(*count)++] = p;
    } else {
      for (size_t i = 0; i < p.count; i++) {
        free(p.packages[i].url);
        free(p.packages[i].name);
        free(p.packages[i].digest);
      }
      free(p.packages);
      free(english);
    }
    k = product;
  }
  return out;
}

static xmlNode *child(xmlNode *parent, const char *name) {
  for (xmlNode *n = element(parent ? parent->children : NULL); n; n = element(n->next))
    if (named(n, name))
      return n;
  return NULL;
}

/* SU_VERS from the English localization: "KEY" = "VALUE"; */
static char *localized(xmlNode *dist, const char *key) {
  for (xmlNode *s = element(child(dist, "localization") ?
                            child(dist, "localization")->children : NULL);
       s; s = element(s->next)) {
    xmlChar *language = xmlGetProp(s, BAD_CAST "language");
    int english = language && (!xmlStrcmp(language, BAD_CAST "English") ||
                               !xmlStrcmp(language, BAD_CAST "en"));
    xmlFree(language);
    if (!english)
      continue;
    xmlChar *text = xmlNodeGetContent(s);
    char quoted[128];
    snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    const char *at = strstr((const char *)text, quoted);
    char *value = NULL;
    if (at && (at = strchr(at + strlen(quoted), '"'))) {
      const char *end = strchr(at + 1, '"');
      if (end)
        value = strndup(at + 1, (size_t)(end - at - 1));
    }
    xmlFree(text);
    return value;
  }
  return NULL;
}

static void read_distribution(Product *p, const char *target) {
  size_t len;
  unsigned char *data = read_file(p->distribution, DISTRIBUTION_LIMIT, &len);
  xmlDoc *doc = parse_xml(data, len, p->distribution);
  xmlNode *dist = xmlDocGetRootElement(doc);
  int tagged = 0;
  for (xmlNode *t = element(child(dist, "tags") ? child(dist, "tags")->children : NULL);
       t; t = element(t->next)) {
    xmlChar *text = xmlNodeGetContent(t);
    tagged |= !xmlStrcmp(text, BAD_CAST CLT_TAG);
    xmlFree(text);
  }
  p->version = localized(dist, "SU_VERS");
  if (!tagged || !p->version || !valid_version(p->version)) {
    free(p->version);
    p->version = NULL;
  }
  p->ranges = xstrdup("");
  xmlNode *versions = child(child(dist, "volume-check"), "allowed-os-versions");
  for (xmlNode *v = element(versions ? versions->children : NULL); v; v = element(v->next)) {
    xmlChar *min = xmlGetProp(v, BAD_CAST "min"), *before = xmlGetProp(v, BAD_CAST "before");
    if ((!min || compare_versions(target, (char *)min) >= 0) &&
        (!before || compare_versions(target, (char *)before) < 0))
      p->allowed = 1;
    char *ranges;
    if (asprintf(&ranges, "%s%s[%s, %s)", p->ranges, *p->ranges ? ", " : "",
                 min ? (char *)min : "", before ? (char *)before : "") < 0)
      die("out of memory");
    free(p->ranges);
    p->ranges = ranges;
    xmlFree(min);
    xmlFree(before);
  }
  xmlFreeDoc(doc);
  free(data);
}

/* The licence is RTF; this is its text: control words dropped, \par and
 * line-ending backslashes as newlines, \'hh as Windows-1252, and groups
 * that are destinations (\*, font and colour tables) skipped. */
static void put_cp1252(FILE *out, unsigned c) {
  static const unsigned short high[32] = {
      0x20ac, 0, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030,
      0x0160, 0x2039, 0x0152, 0, 0x017d, 0, 0, 0x2018, 0x2019, 0x201c, 0x201d,
      0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e,
      0x0178};
  unsigned u = c >= 0x80 && c < 0xa0 ? high[c - 0x80] : c;
  if (!u)
    return;
  if (u < 0x80)
    fputc((int)u, out);
  else if (u < 0x800)
    fprintf(out, "%c%c", 0xc0 | (u >> 6), 0x80 | (u & 0x3f));
  else
    fprintf(out, "%c%c%c", 0xe0 | (u >> 12), 0x80 | ((u >> 6) & 0x3f),
            0x80 | (u & 0x3f));
}

static void print_rtf(const char *rtf, FILE *out) {
  enum { DEPTH = 64 };
  int skip[DEPTH] = {0}, depth = 0;
  for (const char *p = rtf; *p; p++) {
    if (*p == '{') {
      if (depth + 1 < DEPTH) {
        depth++;
        skip[depth] = skip[depth - 1];
      }
    } else if (*p == '}') {
      if (depth)
        depth--;
    } else if (*p == '\\') {
      p++;
      if (*p == '\n' || *p == '\r') {
        if (!skip[depth])
          fputc('\n', out);
      } else if (*p == '\'' && p[1] && p[2]) {
        char hex[3] = {p[1], p[2], 0};
        if (!skip[depth])
          put_cp1252(out, (unsigned)strtoul(hex, NULL, 16));
        p += 2;
      } else if (*p == '*') {
        skip[depth] = 1;
      } else if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
        const char *word = p;
        while ((p[1] >= 'a' && p[1] <= 'z') || (p[1] >= 'A' && p[1] <= 'Z'))
          p++;
        size_t n = (size_t)(p - word + 1);
        while (p[1] == '-' || (p[1] >= '0' && p[1] <= '9'))
          p++;
        if (p[1] == ' ')
          p++;
        if ((n == 3 && !strncmp(word, "par", 3)) || (n == 4 && !strncmp(word, "line", 4))) {
          if (!skip[depth])
            fputc('\n', out);
        } else if (n == 3 && !strncmp(word, "tab", 3)) {
          if (!skip[depth])
            fputc('\t', out);
        } else if ((n == 7 && !strncmp(word, "fonttbl", 7)) ||
                   (n == 8 && !strncmp(word, "colortbl", 8)) ||
                   (n == 10 && !strncmp(word, "stylesheet", 10)) ||
                   (n == 4 && !strncmp(word, "info", 4))) {
          skip[depth] = 1;
        }
      } else if (*p && !skip[depth]) {
        fputc(*p, out); /* \{ \} \\ */
      } else if (!*p)
        break;
    } else if (*p != '\n' && *p != '\r' && !skip[depth]) {
      fputc(*p, out);
    }
  }
  fputc('\n', out);
}

static void accept_license(const Product *p, int accepted) {
  size_t len;
  unsigned char *data = read_file(p->distribution, DISTRIBUTION_LIMIT, &len);
  xmlDoc *doc = parse_xml(data, len, p->distribution);
  xmlNode *license = child(xmlDocGetRootElement(doc), "license");
  if (!license)
    die("product %s has no licence", p->id);
  xmlChar *text = xmlNodeGetContent(license);
  if (accepted) {
    printf("The licence of Command Line Tools %s was accepted with "
           "-accept-license.\n", p->version);
  } else {
    print_rtf((const char *)text, stdout);
    fflush(stdout);
    FILE *tty = fopen("/dev/tty", "r+");
    if (!tty)
      die("the licence must be accepted on a terminal, or with -accept-license");
    fprintf(tty, "\nType '%s' to accept the licence above, anything else to "
                 "cancel: ", LICENSE_AGREEMENT);
    fflush(tty);
    char answer[64] = "";
    if (!fgets(answer, sizeof(answer), tty))
      answer[0] = 0;
    answer[strcspn(answer, "\r\n")] = 0;
    fclose(tty);
    if (strcmp(answer, LICENSE_AGREEMENT))
      die("the licence was not accepted; nothing was installed");
  }
  xmlFree(text);
  xmlFreeDoc(doc);
  free(data);
}

/* The temporary directory holds only files this program put there. */
static void cleanup(void) {
  char at[PATH_LEN];
  unlink(xcs_path(IN_PROGRESS, at, sizeof(at)));
  if (!work_dir_is_temporary || !work_dir)
    return;
  DIR *d = opendir(work_dir);
  for (struct dirent *e; d && (e = readdir(d));)
    if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
      char *path = path_in(work_dir, e->d_name);
      unlink(path);
      free(path);
    }
  if (d)
    closedir(d);
  rmdir(work_dir);
}

static const char usage[] =
    "usage: xcselect-install [-list] [-release VERSION] [-pkg-dir DIRECTORY]\n"
    "                        [-accept-license]\n";

int main(int argc, char **argv) {
  int list = 0, accepted = 0;
  const char *release = NULL, *pkg_dir = NULL, *catalog_override = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-list"))
      list = 1;
    else if (!strcmp(argv[i], "-accept-license"))
      accepted = 1;
    else if (!strcmp(argv[i], "-release") && i + 1 < argc)
      release = argv[++i];
    else if (!strcmp(argv[i], "-pkg-dir") && i + 1 < argc)
      pkg_dir = argv[++i];
#ifdef XCSELECT_TESTING
    else if (!strcmp(argv[i], "-catalog") && i + 1 < argc)
      catalog_override = argv[++i];
#endif
    else {
      fputs(usage, stderr);
      return 2;
    }
  }
  char at[PATH_LEN], root_at[PATH_LEN];
  const char *root = xcs_path("/", root_at, sizeof(root_at));
#ifdef XCSELECT_TESTING
  int tested = getenv("MDPKG_TEST_LIVE_ROOT") != NULL;
#else
  int tested = 0;
#endif
  if (!list && geteuid() && !tested)
    die("installing into the running root requires root (try sudo)");

  char *target = target_version(root);
  char *url = catalog_override ? xstrdup(catalog_override) : catalog_url(target);
  if (pkg_dir) {
    if (mkdir(pkg_dir, 0755) && errno != EEXIST)
      die("cannot create %s: %s", pkg_dir, strerror(errno));
    work_dir = xstrdup(pkg_dir);
  } else {
    work_dir = xstrdup("/private/var/tmp/xcselect-install.XXXXXX");
    if (!mkdtemp(work_dir))
      die("cannot create a temporary directory: %s", strerror(errno));
    work_dir_is_temporary = 1;
  }
  atexit(cleanup);

  char *catalog_path = path_in(work_dir, "catalog.sucatalog.gz");
  download(url, catalog_path, CATALOG_LIMIT, 0);
  size_t len;
  unsigned char *catalog_data = gunzip_file(catalog_path, CATALOG_LIMIT, &len);
  xmlDoc *catalog = parse_xml(catalog_data, len, url);
  size_t count;
  Product *products = catalog_products(catalog, &count);

  Product *chosen = NULL;
  for (size_t i = 0; i < count; i++) {
    Product *p = &products[i];
    char *name;
    if (asprintf(&name, "%s.dist", p->id) < 0)
      die("out of memory");
    p->distribution = path_in(work_dir, name);
    free(name);
    download(p->distribution_url, p->distribution, DISTRIBUTION_LIMIT, 0);
    read_distribution(p, target);
    if (!p->version)
      continue;
    if (list)
      printf("%s %s, for macOS %s%s\n", p->version, p->id, p->ranges,
             p->allowed ? "" : " (not this target)");
    int wanted = release ? !strcmp(p->version, release) : 1;
    if (p->allowed && wanted &&
        (!chosen || compare_versions(p->version, chosen->version) > 0))
      chosen = p;
  }
  if (list)
    return 0;
  if (!chosen)
    die(release ? "Command Line Tools %s are not offered for macOS %s"
                : "no Command Line Tools are offered for macOS %s%s",
        release ? release : target, release ? target : "");

  printf("Command Line Tools for Xcode %s (%s), for macOS %s\n", chosen->version,
         chosen->id, target);
  accept_license(chosen, accepted);

  for (size_t i = 0; i < chosen->count; i++) {
    Package *p = &chosen->packages[i];
    char *dest = path_in(work_dir, p->name);
    if (!package_is_current(p, dest)) {
      printf("Downloading %s (%llu bytes)\n", p->name, p->size);
      fflush(stdout);
      unlink(dest);
      download(p->url, dest, p->size, isatty(STDERR_FILENO));
      if (!package_is_current(p, dest))
        die("%s does not match the catalog's size and digest", p->name);
    }
    free(dest);
  }

  /* The Distribution shows its choice on a fresh install only while an
   * on-demand install is in progress. */
  int marker = open(xcs_path(IN_PROGRESS, at, sizeof(at)),
                    O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
  if (marker < 0)
    die("cannot create %s: %s", at, strerror(errno));
  close(marker);

#ifdef XCSELECT_TESTING
  const char *mdpkg = getenv("XCSELECT_TEST_MDPKG") ? getenv("XCSELECT_TEST_MDPKG") : MDPKG;
#else
  const char *mdpkg = MDPKG;
#endif
  char *const install[] = {(char *)mdpkg, "-dist", chosen->distribution,
                           "-pkg-dir", work_dir, "-target", (char *)root,
                           "-verify-signature", "apple-software-update", NULL};
  int status = run(install);
  if (status)
    die("mdpkg failed (status %d); see above", status);
  printf("Installed Command Line Tools for Xcode %s\n", chosen->version);
  return 0;
}
