#include "dslocal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define NODE "/private/var/db/dslocal/nodes/Default"
#define NATIVE "dsAttrTypeNative:"
#define STANDARD "dsAttrTypeStandard:"

static const char *prog_name = "dscl";

_Noreturn void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "%s: ", prog_name);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(1);
}

_Noreturn void ds_fail(const char *prog, DsError error) {
  const char *name = error == DS_RECORD_NOT_FOUND    ? "eDSRecordNotFound"
                     : error == DS_PERMISSION_ERROR  ? "eDSPermissionError"
                                                     : "eDSUnknownNodeName";
  fprintf(stderr, "<%s_cmd> DS Error: %d (%s)\n", prog, (int)error, name);
  exit(1);
}

void *xcalloc(size_t count, size_t size) {
  void *p = calloc(count ? count : 1, size ? size : 1);
  if (!p)
    die("out of memory");
  return p;
}

char *xstrdup(const char *s) {
  char *p = strdup(s);
  if (!p)
    die("out of memory");
  return p;
}

static char *xasprintf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *s = NULL;
  if (vasprintf(&s, fmt, ap) < 0)
    die("out of memory");
  va_end(ap);
  return s;
}

/* DSCL_TEST_NODE stands in for the node directory, and lets a test that is
 * not root change it. It is not for any other use. */
static const char *node_dir(void) {
  const char *test = getenv("DSCL_TEST_NODE");
  return test && *test ? test : NODE;
}

int ds_may_write(void) {
  const char *test = getenv("DSCL_TEST_NODE");
  return geteuid() == 0 || (test && *test);
}

int ds_kind(const char *type) {
  if (!strcasecmp(type, "/Users") || !strcasecmp(type, "/Users/"))
    return KIND_USERS;
  if (!strcasecmp(type, "/Groups") || !strcasecmp(type, "/Groups/"))
    return KIND_GROUPS;
  return -1;
}

const char *ds_type_name(Kind kind) {
  return kind == KIND_USERS ? "Users" : "Groups";
}

static char *kind_dir(Kind kind) {
  return xasprintf("%s/%s", node_dir(), kind == KIND_USERS ? "users" : "groups");
}

int ds_valid_name(const char *name) {
  return *name && *name != '.' && !strchr(name, '/') &&
         strlen(name) < NAME_MAX - sizeof(".plist");
}

/* Attribute names: the standard ones dslocal stores under a native key of
 * its own; any other name is a native key itself. */
static const char *const names[][2] = {
    {"RecordName", "name"},
    {"UniqueID", "uid"},
    {"PrimaryGroupID", "gid"},
    {"NFSHomeDirectory", "home"},
    {"UserShell", "shell"},
    {"RealName", "realname"},
    {"Password", "passwd"},
    {"GeneratedUID", "generateduid"},
    {"AuthenticationAuthority", "authentication_authority"},
    {"PasswordPolicyOptions", "passwordpolicyoptions"},
    {"GroupMembership", "users"},
    {"GroupMembers", "groupmembers"},
    {"NestedGroups", "nestedgroups"},
    {"Comment", "comment"},
    {"AuthenticationHint", "hint"},
    {"Picture", "picture"},
};
enum { NNAMES = sizeof names / sizeof names[0] };

char *attr_key(const char *name) {
  if (!strncmp(name, NATIVE, strlen(NATIVE)))
    return xstrdup(name + strlen(NATIVE));
  if (!strncmp(name, STANDARD, strlen(STANDARD)))
    name += strlen(STANDARD);
  for (size_t i = 0; i < NNAMES; i++)
    if (!strcmp(name, names[i][0]))
      return xstrdup(names[i][1]);
  return xstrdup(name);
}

char *attr_name(const char *key) {
  for (size_t i = 0; i < NNAMES; i++)
    if (!strcmp(key, names[i][1]))
      return xstrdup(names[i][0]);
  return xasprintf(NATIVE "%s", key);
}

Attr *rec_get(Record *r, const char *key) {
  for (size_t i = 0; i < r->count; i++)
    if (!strcmp(r->attrs[i].key, key))
      return &r->attrs[i];
  return NULL;
}

const char *rec_first(Record *r, const char *key) {
  Attr *a = rec_get(r, key);
  return a && a->count ? a->values[0] : NULL;
}

static Attr *rec_make(Record *r, const char *key) {
  Attr *a = rec_get(r, key);
  if (a)
    return a;
  r->attrs = realloc(r->attrs, (r->count + 1) * sizeof *r->attrs);
  if (!r->attrs)
    die("out of memory");
  a = &r->attrs[r->count++];
  *a = (Attr){xstrdup(key), NULL, 0};
  return a;
}

void rec_append(Record *r, const char *key, char *const *values,
                size_t count) {
  Attr *a = rec_make(r, key);
  a->values = realloc(a->values, (a->count + count + 1) * sizeof *a->values);
  if (!a->values)
    die("out of memory");
  for (size_t i = 0; i < count; i++)
    a->values[a->count++] = xstrdup(values[i]);
}

static void attr_clear(Attr *a) {
  for (size_t i = 0; i < a->count; i++)
    free(a->values[i]);
  free(a->values);
  a->values = NULL;
  a->count = 0;
}

void rec_set(Record *r, const char *key, char *const *values, size_t count) {
  attr_clear(rec_make(r, key));
  rec_append(r, key, values, count);
}

void rec_delete(Record *r, const char *key) {
  Attr *a = rec_get(r, key);
  if (!a)
    return;
  attr_clear(a);
  free(a->key);
  *a = r->attrs[--r->count];
}

void rec_delete_values(Record *r, const char *key, char *const *values,
                       size_t count) {
  Attr *a = rec_get(r, key);
  if (!a)
    return;
  size_t kept = 0;
  for (size_t i = 0; i < a->count; i++) {
    int drop = 0;
    for (size_t j = 0; j < count && !drop; j++)
      drop = !strcmp(a->values[i], values[j]);
    if (drop)
      free(a->values[i]);
    else
      a->values[kept++] = a->values[i];
  }
  a->count = kept;
}

void ds_free(Record *r) {
  if (!r)
    return;
  for (size_t i = 0; i < r->count; i++) {
    attr_clear(&r->attrs[i]);
    free(r->attrs[i].key);
  }
  free(r->attrs);
  free(r);
}

static void set_one(Record *r, const char *key, const char *value) {
  char *v = (char *)value;
  rec_set(r, key, &v, 1);
}

/* A random (version 4) UUID, as Directory Services gives every record. */
static char *new_uuid(void) {
  unsigned char b[16];
  arc4random_buf(b, sizeof b);
  b[6] = (b[6] & 0x0f) | 0x40;
  b[8] = (b[8] & 0x3f) | 0x80;
  return xasprintf("%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-"
                   "%02X%02X%02X%02X%02X%02X",
                   b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
                   b[10], b[11], b[12], b[13], b[14], b[15]);
}

Record *ds_new(const char *name) {
  Record *r = xcalloc(1, sizeof *r);
  set_one(r, "name", name);
  char *uuid = new_uuid();
  set_one(r, "generateduid", uuid);
  free(uuid);
  return r;
}

static Record *parse(const char *path) {
  xmlDoc *doc = xmlReadFile(path, NULL, XML_PARSE_NONET | XML_PARSE_NOBLANKS);
  /* The plist DOCTYPE names a DTD; one that declares anything is refused,
   * so no entity is ever expanded. */
  if (!doc || (doc->intSubset && doc->intSubset->children) || doc->extSubset)
    die("%s: not an XML property list", path);
  xmlNode *plist = xmlDocGetRootElement(doc), *dict = NULL;
  if (plist && !xmlStrcmp(plist->name, BAD_CAST "plist"))
    for (xmlNode *n = plist->children; n && !dict; n = n->next)
      if (n->type == XML_ELEMENT_NODE)
        dict = n;
  if (!dict || xmlStrcmp(dict->name, BAD_CAST "dict"))
    die("%s: not a dictionary", path);
  Record *r = xcalloc(1, sizeof *r);
  for (xmlNode *n = dict->children; n; n = n->next) {
    if (n->type != XML_ELEMENT_NODE)
      continue;
    xmlNode *value = n->next;
    while (value && value->type != XML_ELEMENT_NODE)
      value = value->next;
    if (xmlStrcmp(n->name, BAD_CAST "key") || !value ||
        xmlStrcmp(value->name, BAD_CAST "array"))
      die("%s: every attribute must be a key and an array", path);
    xmlChar *key = xmlNodeGetContent(n);
    if (rec_get(r, (char *)key))
      die("%s: duplicate key %s", path, key);
    rec_make(r, (char *)key);
    for (xmlNode *s = value->children; s; s = s->next) {
      if (s->type != XML_ELEMENT_NODE)
        continue;
      if (xmlStrcmp(s->name, BAD_CAST "string"))
        die("%s: %s: values must be strings", path, key);
      xmlChar *text = xmlNodeGetContent(s);
      char *v = (char *)text;
      rec_append(r, (char *)key, &v, 1);
      xmlFree(text);
    }
    xmlFree(key);
    n = value;
  }
  xmlFreeDoc(doc);
  return r;
}

static Record *load(Kind kind, const char *name) {
  if (!ds_valid_name(name))
    return NULL;
  char *dir = kind_dir(kind), *path = xasprintf("%s/%s.plist", dir, name);
  Record *r = access(path, F_OK) ? NULL : parse(path);
  free(dir);
  free(path);
  return r;
}

static char *number(unsigned long n) { return xasprintf("%lu", n); }

static Record *from_passwd(struct passwd *pw) {
  Record *r = xcalloc(1, sizeof *r);
  char *uid = number(pw->pw_uid), *gid = number(pw->pw_gid);
  set_one(r, "name", pw->pw_name);
  set_one(r, "uid", uid);
  set_one(r, "gid", gid);
  set_one(r, "realname", pw->pw_gecos ? pw->pw_gecos : "");
  set_one(r, "home", pw->pw_dir ? pw->pw_dir : "");
  set_one(r, "shell", pw->pw_shell ? pw->pw_shell : "");
  free(uid);
  free(gid);
  return r;
}

static Record *from_group(struct group *gr) {
  Record *r = xcalloc(1, sizeof *r);
  char *gid = number(gr->gr_gid);
  set_one(r, "name", gr->gr_name);
  set_one(r, "gid", gid);
  size_t n = 0;
  while (gr->gr_mem && gr->gr_mem[n])
    n++;
  rec_set(r, "users", gr->gr_mem, n);
  free(gid);
  return r;
}

Record *ds_find(Kind kind, const char *name, int *stored) {
  Record *r = load(kind, name);
  *stored = r != NULL;
  if (r)
    return r;
  if (kind == KIND_USERS) {
    struct passwd *pw = getpwnam(name);
    return pw ? from_passwd(pw) : NULL;
  }
  struct group *gr = getgrnam(name);
  return gr ? from_group(gr) : NULL;
}

Record *ds_find_id(Kind kind, unsigned id, int *stored) {
  const char *key = kind == KIND_USERS ? "uid" : "gid";
  size_t count;
  char **names = ds_list(kind, &count);
  Record *found = NULL;
  for (size_t i = 0; i < count; i++) {
    Record *r = load(kind, names[i]);
    const char *v = r ? rec_first(r, key) : NULL;
    if (!found && v && strtoul(v, NULL, 10) == id && *v)
      found = r;
    else
      ds_free(r);
    free(names[i]);
  }
  free(names);
  *stored = found != NULL;
  if (found)
    return found;
  Record *system = NULL;
  if (kind == KIND_USERS) {
    struct passwd *pw = getpwuid(id);
    system = pw ? from_passwd(pw) : NULL;
  } else {
    struct group *gr = getgrgid(id);
    system = gr ? from_group(gr) : NULL;
  }
  /* A stored record of the same name replaces the system's. */
  Record *own = system ? load(kind, rec_first(system, "name")) : NULL;
  if (own) {
    ds_free(own);
    ds_free(system);
    return NULL;
  }
  return system;
}

static int by_name(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

char **ds_list(Kind kind, size_t *count) {
  char *dir = kind_dir(kind);
  DIR *d = opendir(dir);
  free(dir);
  char **out = xcalloc(1, sizeof *out);
  *count = 0;
  if (!d)
    return out;
  struct dirent *e;
  while ((e = readdir(d))) {
    size_t len = strlen(e->d_name);
    if (e->d_name[0] == '.' || len <= 6 || strcmp(e->d_name + len - 6, ".plist"))
      continue;
    out = realloc(out, (*count + 1) * sizeof *out);
    if (!out)
      die("out of memory");
    out[(*count)++] = strndup(e->d_name, len - 6);
  }
  closedir(d);
  qsort(out, *count, sizeof *out, by_name);
  return out;
}

static void write_escaped(FILE *f, const char *s) {
  for (; *s; s++)
    switch (*s) {
    case '&': fputs("&amp;", f); break;
    case '<': fputs("&lt;", f); break;
    case '>': fputs("&gt;", f); break;
    default: fputc(*s, f);
    }
}

static void mkdirs(const char *path) {
  char *p = xstrdup(path);
  for (char *s = p + 1; *s; s++)
    if (*s == '/') {
      *s = 0;
      if (mkdir(p, 0755) && errno != EEXIST)
        die("mkdir %s: %s", p, strerror(errno));
      *s = '/';
    }
  if (mkdir(p, 0755) && errno != EEXIST)
    die("mkdir %s: %s", p, strerror(errno));
  free(p);
}

/* Written whole beside the old file and renamed over it, so a reader sees
 * one record or the other. Readable by everyone: nothing here stores a
 * password hash (there is no -passwd), and the system's account lookups
 * read these files. */
void ds_save(Kind kind, Record *r) {
  const char *name = rec_first(r, "name");
  if (!name || !ds_valid_name(name))
    die("invalid record name");
  char *dir = kind_dir(kind);
  mkdirs(dir);
  char *tmp = xasprintf("%s/.%s.plist.XXXXXX", dir, name),
       *path = xasprintf("%s/%s.plist", dir, name);
  int fd = mkstemp(tmp);
  FILE *f = fd < 0 ? NULL : fdopen(fd, "w");
  if (!f)
    die("%s: %s", tmp, strerror(errno));
  fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n",
        f);
  for (size_t i = 0; i < r->count; i++) {
    Attr *a = &r->attrs[i];
    fputs("\t<key>", f);
    write_escaped(f, a->key);
    fputs("</key>\n", f);
    if (!a->count) {
      fputs("\t<array/>\n", f);
      continue;
    }
    fputs("\t<array>\n", f);
    for (size_t j = 0; j < a->count; j++) {
      fputs("\t\t<string>", f);
      write_escaped(f, a->values[j]);
      fputs("</string>\n", f);
    }
    fputs("\t</array>\n", f);
  }
  fputs("</dict>\n</plist>\n", f);
  if (fchmod(fd, 0644) || fflush(f) || fsync(fd) || fclose(f) ||
      rename(tmp, path)) {
    int saved = errno;
    unlink(tmp);
    die("%s: %s", path, strerror(saved));
  }
  free(tmp);
  free(path);
  free(dir);
}

void ds_remove(Kind kind, const char *name) {
  char *dir = kind_dir(kind), *path = xasprintf("%s/%s.plist", dir, name);
  if (unlink(path))
    die("%s: %s", path, strerror(errno));
  free(path);
  free(dir);
}

void ds_set_prog(const char *name) { prog_name = name; }
