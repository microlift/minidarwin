/* dseditgroup: create, delete and edit groups in the local node.
 *
 *   dseditgroup [-q] [-n NODE] -o create [-i GID] [-r REALNAME] GROUP
 *   dseditgroup [-q] [-n NODE] -o delete GROUP
 *   dseditgroup [-q] [-n NODE] -o edit [-a USER | -d USER] -t user GROUP
 *
 * A group created without -i gets the lowest id from 501 that neither the
 * node nor the system's group database uses. */
#include "dslocal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* The first id handed to a group made without one. */
#define FIRST_GID 501u

static void usage(void) {
  fputs("usage: dseditgroup [-q] [-n NODE] -o create|delete|edit "
        "[-i GID] [-r REALNAME] [-a USER | -d USER] [-t user] GROUP\n",
        stderr);
  exit(2);
}

static char *free_gid(void) {
  for (unsigned gid = FIRST_GID;; gid++) {
    int stored;
    Record *r = ds_find_id(KIND_GROUPS, gid, &stored);
    if (!r) {
      char *s = NULL;
      if (asprintf(&s, "%u", gid) < 0)
        die("out of memory");
      return s;
    }
    ds_free(r);
  }
}

int main(int argc, char **argv) {
  ds_set_prog("dseditgroup");
  const char *op = NULL, *gid = NULL, *realname = NULL, *add = NULL,
             *del = NULL, *type = "user", *node = ".";
  int c;
  while ((c = getopt(argc, argv, "qn:o:i:r:a:d:t:u:P:")) != -1)
    switch (c) {
    case 'q': break;
    case 'n': node = optarg; break;
    case 'o': op = optarg; break;
    case 'i': gid = optarg; break;
    case 'r': realname = optarg; break;
    case 'a': add = optarg; break;
    case 'd': del = optarg; break;
    case 't': type = optarg; break;
    /* Credentials for a remote node; the local one needs root instead. */
    case 'u':
    case 'P': break;
    default: usage();
    }
  if (optind != argc - 1 || !op)
    usage();
  const char *name = argv[optind];
  if (strcmp(node, ".") && strcasecmp(node, "/Local/Default"))
    ds_fail("dseditgroup", DS_UNKNOWN_NODE_NAME);
  if (!ds_valid_name(name))
    die("invalid group name %s", name);
  if (!ds_may_write())
    ds_fail("dseditgroup", DS_PERMISSION_ERROR);

  int stored;
  Record *r = ds_find(KIND_GROUPS, name, &stored);
  if (!strcmp(op, "create")) {
    if (r)
      die("group %s already exists", name);
    if (gid) {
      char *end;
      unsigned long id = strtoul(gid, &end, 10);
      if (!*gid || *end)
        die("invalid group id %s", gid);
      Record *taken = ds_find_id(KIND_GROUPS, (unsigned)id, &stored);
      if (taken)
        die("group id %s is in use by %s", gid, rec_first(taken, "name"));
    }
    char *id = gid ? xstrdup(gid) : free_gid();
    char *rn = (char *)(realname ? realname : name);
    r = ds_new(name);
    rec_set(r, "gid", &id, 1);
    rec_set(r, "realname", &rn, 1);
    ds_save(KIND_GROUPS, r);
    free(id);
  } else if (!strcmp(op, "delete")) {
    if (!r)
      ds_fail("dseditgroup", DS_RECORD_NOT_FOUND);
    if (!stored)
      ds_fail("dseditgroup", DS_PERMISSION_ERROR);
    ds_remove(KIND_GROUPS, name);
  } else if (!strcmp(op, "edit")) {
    if (!r)
      ds_fail("dseditgroup", DS_RECORD_NOT_FOUND);
    if (strcmp(type, "user"))
      die("only users can be added to or removed from a group");
    if (!add == !del)
      usage();
    char *member = (char *)(add ? add : del);
    Attr *users = rec_get(r, "users");
    int present = 0;
    for (size_t i = 0; users && i < users->count && !present; i++)
      present = !strcmp(users->values[i], member);
    if (add && !present)
      rec_append(r, "users", &member, 1);
    if (del)
      rec_delete_values(r, "users", &member, 1);
    ds_save(KIND_GROUPS, r);
  } else {
    die("unsupported operation %s", op);
  }
  ds_free(r);
  return 0;
}
