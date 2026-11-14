/* mdpkg: a CoreFoundation-free installer for flat .pkg files into offline
 * MiniDarwin roots. See README.md for the supported subset and the
 * transaction model. */
#ifndef MDPKG_H
#define MDPKG_H

#define _DARWIN_C_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#define MDPKG_VERSION "1.0.0"

/* Hostile-input bounds. Every size read from a package is checked against
 * these before anything is allocated. */
#define LIMIT_MEMBER (512u * 1024u * 1024u) /* one decoded XAR member */
#define LIMIT_DECODED (1024u * 1024u * 1024u) /* all bytes decoded to memory */
#define LIMIT_PACKAGE (UINT64_C(16) << 30)  /* one package file */
#define LIMIT_SPOOLED (UINT64_C(64) << 30)  /* payload contents, held on disk */
#define LIMIT_PBZX_CHUNK (64u * 1024u * 1024u)
#define LIMIT_XZ_MEMORY (256u * 1024u * 1024u)
#define LIMIT_XML (16u * 1024u * 1024u)
/* Entries are metadata only (contents are spooled); Apple's Command Line
 * Tools, with two macOS SDKs, are well over 100,000. */
#define LIMIT_ENTRIES 1000000
#define LIMIT_DEPTH 128
#define LIMIT_SYMLINK_HOPS 32

/* Paths the installer owns inside a root; payloads may not write there. */
#define RECEIPTS_DIR "private/var/db/receipts"
#define INVENTORY_DIR "private/var/db/mdpkg"
#define SCRIPTS_DIR ".mdpkg-scripts"

typedef struct {
  unsigned char *data;
  size_t len;
} Bytes;

/* An unlinked temporary file that holds a component's payload contents, so
 * that no payload has to fit in memory. */
typedef struct {
  int fd;
  uint64_t used;
} Spool;

/* One filesystem object: a CPIO record, or a snapshot of an installed one.
 * `path` is root-relative and normalized ("" is the root itself). For
 * snapshots, `data` holds the SHA-256 of a regular file, not its contents. */
typedef struct {
  char *path;
  char *package_path; /* as packaged, when it differs from `path` */
  const char *change; /* script inventory: "created", "modified", "removed" */
  char *link;
  mode_t mode;
  uid_t uid;
  gid_t gid;
  time_t mtime;
  Bytes data;
  /* Set instead of `data` for a payload file: SIZE bytes at SPOOL_OFFSET. */
  const Spool *spool;
  uint64_t spool_offset, size;
} Entry;

typedef struct {
  Entry *items;
  size_t count;
} EntryList;

/* A member is decoded and verified when first asked for. */
typedef struct {
  char *name;
  uint64_t offset, length, size;
  char *encoding, *archived_style, *archived, *extracted_style, *extracted;
  int read;
  Bytes data;
} XarMember;

typedef struct {
  char *path;
  unsigned char *map; /* the whole file, mapped read-only */
  size_t map_len;
  Bytes heap;
  XarMember *members;
  size_t count;
  char *signer; /* subject of the verified signing certificate, or NULL */
} Xar;

enum { HOOK_PREFLIGHT, HOOK_PREINSTALL, HOOK_POSTINSTALL, HOOK_POSTFLIGHT,
       HOOK_COUNT };
extern const char *const hook_names[HOOK_COUNT];

typedef struct {
  Xar *xar;      /* the archive holding this component */
  char *xar_dir; /* "" for a bare component, else e.g. "mcinstall.pkg" */
  char *package_path; /* the .pkg file, for scripts and receipts */
  char *identifier;
  char *version;
  char *location; /* normalized, root-relative install-location */
  Bytes bom;
  EntryList payload; /* paths include `location` */
  Spool *spool; /* shared by the payload's entries, so never moved */
  EntryList scripts;
  char *hooks[HOOK_COUNT]; /* script paths inside `scripts`, or NULL */
  char **groups; /* PackageInfo's package groups, NULL-terminated */
  int metadata_only; /* target-free inspection of a JS candidate */
} Component;

typedef struct {
  const char *path; /* absolute; the Distribution itself with -dist */
  Xar xar;
  const char *pkg_dir; /* -dist: where its pkg-refs are, else NULL */
  Component *components;
  size_t count;
  char *host_architectures; /* informational; NULL if absent */
  char *allowed_os_versions; /* informational, e.g. "15 <= v < 16.0"; NULL if absent */
  struct _xmlDoc *distribution; /* retained until target selection */
  char **strings; /* Distribution localization: key, value, ..., NULL */
  int needs_js, unresolved;
} Package;

/* Command-line policy (main.c sets these, defined in util.c). */
extern const char *opt_system_version; /* -system-version: system.version.ProductVersion */
extern int opt_skip_scripts;           /* -skip-scripts: install payloads only */
extern const char *opt_signature_policy; /* -verify-signature POLICY */

/* util.c */
_Noreturn void die(const char *fmt, ...);
extern void (*die_hook)(void);
/* Called with every directory and file util.c creates, so a live install
 * can undo them. */
extern void (*create_hook)(const char *path);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *p, size_t count, size_t size);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *path_join(const char *a, const char *b);
char *path_parent(const char *path);
int path_has_prefix(const char *path, const char *prefix);
char *path_normalize(const char *path, int absolute);
int path_exists(const char *path);
void path_parents(const char *base, const char *rel, int create);
uint64_t parse_uint(const char *s, int base);
Bytes file_read(const char *path, size_t limit);
void file_write(const char *path, Bytes contents, mode_t mode);
void file_copy(const char *from, const char *to, mode_t mode);
Bytes inflate_limited(Bytes in, int window_bits, size_t expected, size_t limit);
void file_write_spooled(const char *path, const Entry *e, mode_t mode);
void spool_open(Spool *spool);
void spool_write(Spool *spool, const unsigned char *p, size_t len);
void decoded_account(size_t n);
Bytes digest(Bytes in, const char *algorithm);
Bytes file_sha256(const char *path);
char *hex_encode(Bytes b);
void entries_append(EntryList *list, Entry e);
void entries_free(EntryList *list);
Entry *entries_find(EntryList *list, const char *path);
void entries_check_paths(EntryList *list, int across_components);
int entries_identical(const Entry *a, const Entry *b);
void symlink_check_lexical(const char *path, const char *target);

/* xml.c -- thin, failing wrappers over libxml2. */
typedef struct _xmlDoc xmlDoc;
typedef struct _xmlNode xmlNode;
xmlDoc *xml_parse(Bytes b);
void xml_free(xmlDoc *doc);
xmlNode *xml_root(xmlDoc *doc, const char *expected_name);
int xml_is(xmlNode *n, const char *name);
xmlNode *xml_child(xmlNode *parent, const char *name);
xmlNode *xml_first_element(xmlNode *n);
xmlNode *xml_elements(xmlNode *parent);
xmlNode *xml_next_element(xmlNode *n);
const char *xml_name(xmlNode *n);
char *xml_text(xmlNode *n);
char *xml_attr(xmlNode *n, const char *key, const char *fallback);
uint64_t xml_child_uint(xmlNode *parent, const char *name);

/* xar.c */
void xar_open(Xar *xar, const char *path);
Bytes xar_member(Xar *xar, const char *name, int required);

/* signature.c: dies unless the TOC's signature chains to the policy's anchor. */
char *xar_verify_signature(xmlNode *toc, Bytes heap, Bytes toc_hash,
                           const char *hash_style, const char *policy);

/* stream.c: a payload, raw or gzip/pbzx-compressed, read front to back. */
typedef struct Stream Stream;
Stream *stream_open(Bytes encoded);
size_t stream_read(Stream *s, unsigned char *buf, size_t len);
void stream_close(Stream *s);

/* cpio.c: with a spool, file contents go there instead of into memory. */
void cpio_parse(Bytes archive, EntryList *out, Spool *spool);

/* package.c */
void package_load(Package *pkg, const char *path);
void package_load_distribution(Package *pkg, const char *path,
                               const char *pkg_dir);
void package_resolve(Package *pkg, const char *root, int live);
void package_inspect_candidates(Package *pkg);

/* install.c */
void install_package(Package *pkg, const char *root, const char *runner);
char *resolve_target_path(const char *base, const char *rel,
                          int follow_last, int live);
int is_running_root(const char *root);
char *inspect_target(const char *requested);

/* receipts.c */
void snapshot_tree(EntryList *out, const char *base);
Entry snapshot_entry(const char *base, const char *rel);
void receipts_write(const char *stage, const Component *c,
                    EntryList *script_changes);
void script_changes(EntryList *out, EntryList *before, EntryList *after);

#endif
