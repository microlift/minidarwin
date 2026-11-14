/* dscl: the Directory Service command line, for the local node only.
 *
 *   dscl [-q] DATASOURCE -read|-list|-search|-create|-append|-delete|-change
 *        PATH [ARGS...]
 *
 * DATASOURCE is the local node (".", "/Local/Default") or "/Search", which
 * here searches the same records. PATH is /Users[/name] or /Groups[/name]. A
 * record the node does not hold is answered from the system's account
 * database, and copied into the node when it is changed. */
#include "dslocal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static Kind kind;
static const char *record_name; /* NULL for the record type itself */

static void usage(void) {
  fputs("usage: dscl [-q] DATASOURCE -read|-list|-search|-create|-append|"
        "-delete|-change PATH [ARGS...]\n",
        stderr);
  exit(2);
}

static void parse_path(const char *path) {
  char *copy = xstrdup(path);
  char *slash = copy[0] == '/' ? strchr(copy + 1, '/') : NULL;
  if (slash && slash[1]) {
    *slash = 0;
    record_name = xstrdup(slash + 1);
  }
  int k = ds_kind(copy);
  if (k < 0 || (record_name && !ds_valid_name(record_name)))
    ds_fail("dscl", DS_RECORD_NOT_FOUND);
  kind = (Kind)k;
  free(copy);
}

static Record *need_record(int *stored) {
  if (!record_name)
    usage();
  Record *r = ds_find(kind, record_name, stored);
  if (!r)
    ds_fail("dscl", DS_RECORD_NOT_FOUND);
  return r;
}

static int has_space(Attr *a) {
  for (size_t i = 0; i < a->count; i++)
    if (strchr(a->values[i], ' '))
      return 1;
  return 0;
}

/* As dscl prints an attribute: on one line, or one value per indented line
 * when a value has a space in it. */
static void print_attr(const char *name, Attr *a) {
  printf("%s:", name);
  int lines = has_space(a);
  for (size_t i = 0; i < a->count; i++)
    printf(lines ? "\n %s" : " %s", a->values[i]);
  putchar('\n');
}

static int native_first(const void *x, const void *y) {
  const char *a = *(char *const *)x, *b = *(char *const *)y;
  int na = !strncmp(a, "dsAttrTypeNative:", 17),
      nb = !strncmp(b, "dsAttrTypeNative:", 17);
  return na != nb ? nb - na : strcmp(a, b);
}

static int print_record(Record *r, char **keys, int nkeys) {
  if (nkeys) {
    int missing = 0;
    for (int i = 0; i < nkeys; i++) {
      char *key = attr_key(keys[i]), *name = attr_name(key);
      Attr *a = rec_get(r, key);
      if (a)
        print_attr(name, a);
      else {
        fprintf(stderr, "No such key: %s\n", keys[i]);
        missing = 1;
      }
      free(key);
      free(name);
    }
    return missing;
  }
  char **names = xcalloc(r->count, sizeof *names);
  for (size_t i = 0; i < r->count; i++)
    names[i] = attr_name(r->attrs[i].key);
  qsort(names, r->count, sizeof *names, native_first);
  for (size_t i = 0; i < r->count; i++) {
    char *key = attr_key(names[i]);
    print_attr(names[i], rec_get(r, key));
    free(key);
    free(names[i]);
  }
  free(names);
  return 0;
}

static int cmd_read(char **keys, int nkeys) {
  int stored;
  Record *r = need_record(&stored);
  int rc = print_record(r, keys, nkeys);
  ds_free(r);
  return rc;
}

static int cmd_readall(char **keys, int nkeys) {
  if (record_name)
    return cmd_read(keys, nkeys);
  size_t count;
  char **names = ds_list(kind, &count);
  for (size_t i = 0; i < count; i++) {
    int stored;
    Record *r = ds_find(kind, names[i], &stored);
    if (i)
      puts("-");
    if (r)
      print_record(r, keys, nkeys);
    ds_free(r);
  }
  return 0;
}

/* Of a record type, its records' names, with KEY's values beside them; of a
 * record, nothing, once it is known to exist. */
static int cmd_list(char **keys, int nkeys) {
  if (record_name) {
    int stored;
    ds_free(need_record(&stored));
    return 0;
  }
  size_t count;
  char **names = ds_list(kind, &count);
  char *key = nkeys ? attr_key(keys[0]) : NULL;
  for (size_t i = 0; i < count; i++) {
    if (!key) {
      puts(names[i]);
      continue;
    }
    int stored;
    Record *r = ds_find(kind, names[i], &stored);
    Attr *a = r ? rec_get(r, key) : NULL;
    printf("%-24s", names[i]);
    for (size_t j = 0; a && j < a->count; j++)
      printf("%s%s", j ? " " : "", a->values[j]);
    putchar('\n');
    ds_free(r);
  }
  free(key);
  return 0;
}

static void print_match(Record *r, const char *name, const char *value) {
  printf("%s\t\t%s = (\n    %s\n)\n", rec_first(r, "name"), name, value);
}

static int cmd_search(char **args, int nargs) {
  if (record_name || nargs != 2)
    usage();
  char *key = attr_key(args[0]), *name = attr_name(key);
  const char *value = args[1];
  size_t count;
  char **names = ds_list(kind, &count);
  for (size_t i = 0; i < count; i++) {
    int stored;
    Record *r = ds_find(kind, names[i], &stored);
    Attr *a = r ? rec_get(r, key) : NULL;
    for (size_t j = 0; a && j < a->count; j++)
      if (!strcmp(a->values[j], value)) {
        print_match(r, name, value);
        break;
      }
    ds_free(r);
  }
  /* The system's records are found by what its database looks up by. */
  const char *id_key = kind == KIND_USERS ? "uid" : "gid";
  Record *system = NULL;
  int stored = 1;
  if (!strcmp(key, id_key)) {
    char *end;
    unsigned long id = strtoul(value, &end, 10);
    if (*value && !*end)
      system = ds_find_id(kind, (unsigned)id, &stored);
  } else if (!strcmp(key, "name")) {
    system = ds_find(kind, value, &stored);
  }
  if (system && !stored)
    print_match(system, name, value);
  ds_free(system);
  free(key);
  free(name);
  return 0;
}

/* The record to change: the node's own, or the system's copied into it. */
static Record *writable(int create) {
  if (!record_name)
    usage();
  if (!ds_may_write())
    ds_fail("dscl", DS_PERMISSION_ERROR);
  int stored;
  Record *r = ds_find(kind, record_name, &stored);
  if (!r && create)
    r = ds_new(record_name);
  if (!r)
    ds_fail("dscl", DS_RECORD_NOT_FOUND);
  return r;
}

static char *checked_key(const char *name) {
  char *key = attr_key(name);
  if (!strcmp(key, "name"))
    die("renaming a record is not supported");
  return key;
}

static int cmd_create(char **args, int nargs) {
  Record *r = writable(1);
  if (nargs) {
    char *key = checked_key(args[0]);
    rec_set(r, key, args + 1, (size_t)nargs - 1);
    free(key);
  }
  ds_save(kind, r);
  ds_free(r);
  return 0;
}

static int cmd_append(char **args, int nargs) {
  if (nargs < 2)
    usage();
  Record *r = writable(0);
  char *key = checked_key(args[0]);
  rec_append(r, key, args + 1, (size_t)nargs - 1);
  ds_save(kind, r);
  free(key);
  ds_free(r);
  return 0;
}

static int cmd_delete(char **args, int nargs) {
  if (!nargs) {
    if (!record_name)
      usage();
    if (!ds_may_write())
      ds_fail("dscl", DS_PERMISSION_ERROR);
    int stored;
    Record *r = ds_find(kind, record_name, &stored);
    if (!r)
      ds_fail("dscl", DS_RECORD_NOT_FOUND);
    /* The system's own accounts are not the node's to delete. */
    if (!stored)
      ds_fail("dscl", DS_PERMISSION_ERROR);
    ds_remove(kind, record_name);
    ds_free(r);
    return 0;
  }
  Record *r = writable(0);
  char *key = checked_key(args[0]);
  if (nargs == 1)
    rec_delete(r, key);
  else
    rec_delete_values(r, key, args + 1, (size_t)nargs - 1);
  ds_save(kind, r);
  free(key);
  ds_free(r);
  return 0;
}

static int cmd_change(char **args, int nargs) {
  if (nargs != 3)
    usage();
  Record *r = writable(0);
  char *key = checked_key(args[0]);
  Attr *a = rec_get(r, key);
  size_t i = 0;
  while (a && i < a->count && strcmp(a->values[i], args[1]))
    i++;
  if (!a || i == a->count)
    die("%s has no value %s", args[0], args[1]);
  free(a->values[i]);
  a->values[i] = xstrdup(args[2]);
  ds_save(kind, r);
  free(key);
  ds_free(r);
  return 0;
}

int main(int argc, char **argv) {
  ds_set_prog("dscl");
  int i = 1;
  while (i < argc && !strcmp(argv[i], "-q"))
    i++;
  if (argc - i < 3)
    usage();
  const char *source = argv[i++];
  if (strcmp(source, ".") && strcasecmp(source, "/Local/Default") &&
      strcasecmp(source, "/Search"))
    ds_fail("dscl", DS_UNKNOWN_NODE_NAME);
  const char *command = argv[i++];
  parse_path(argv[i++]);
  char **args = argv + i;
  int nargs = argc - i;
  static const struct {
    const char *name;
    int (*run)(char **, int);
  } commands[] = {{"read", cmd_read},     {"readall", cmd_readall},
                  {"list", cmd_list},     {"search", cmd_search},
                  {"create", cmd_create}, {"append", cmd_append},
                  {"delete", cmd_delete}, {"change", cmd_change}};
  /* Commands are spelled with or without their dash. */
  const char *name = command[0] == '-' ? command + 1 : command;
  for (size_t c = 0; c < sizeof commands / sizeof commands[0]; c++)
    if (!strcasecmp(name, commands[c].name))
      return commands[c].run(args, nargs);
  fprintf(stderr, "dscl: unsupported command %s\n", command);
  return 2;
}
