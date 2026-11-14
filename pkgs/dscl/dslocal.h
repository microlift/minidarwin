/* The local directory node, as macOS keeps it: one XML property list per
 * record under /private/var/db/dslocal/nodes/Default/{users,groups}, each a
 * dictionary of attribute names to arrays of strings. */
#ifndef DSLOCAL_H
#define DSLOCAL_H

#include <stddef.h>

typedef enum { KIND_USERS, KIND_GROUPS } Kind;

typedef struct {
  char *key; /* dslocal's own name: "uid", "shell", ... */
  char **values;
  size_t count;
} Attr;

typedef struct {
  Attr *attrs;
  size_t count;
} Record;

/* Errors, by the names and numbers Directory Services reports them. */
typedef enum {
  DS_OK = 0,
  DS_RECORD_NOT_FOUND = -14136,
  DS_PERMISSION_ERROR = -14120,
  DS_UNKNOWN_NODE_NAME = -14009,
} DsError;

_Noreturn void ds_fail(const char *prog, DsError error);
_Noreturn void die(const char *fmt, ...);
/* The name die() and ds_fail() report as. */
void ds_set_prog(const char *name);
void *xcalloc(size_t count, size_t size);
char *xstrdup(const char *s);

/* "/Users" or "/Groups" (any case), or -1. */
int ds_kind(const char *type);
const char *ds_type_name(Kind kind);

/* The record named NAME: the node's own, or else the one the system's
 * account database answers for (read-only until saved). NULL if neither has
 * it. *stored says which. */
Record *ds_find(Kind kind, const char *name, int *stored);
/* The record whose id attribute is ID, by the same two sources. */
Record *ds_find_id(Kind kind, unsigned id, int *stored);
/* Names of the node's own records, sorted. */
char **ds_list(Kind kind, size_t *count);
Record *ds_new(const char *name);
void ds_save(Kind kind, Record *r);
void ds_remove(Kind kind, const char *name);
int ds_valid_name(const char *name);
void ds_free(Record *r);

Attr *rec_get(Record *r, const char *key);
const char *rec_first(Record *r, const char *key);
void rec_set(Record *r, const char *key, char *const *values, size_t count);
void rec_append(Record *r, const char *key, char *const *values, size_t count);
void rec_delete(Record *r, const char *key);
void rec_delete_values(Record *r, const char *key, char *const *values,
                       size_t count);

/* An attribute as a user names it (RecordName, dsAttrTypeNative:x, ...) to
 * dslocal's key, and back to the name dscl prints. */
char *attr_key(const char *name);
char *attr_name(const char *key);

/* Whether the caller may change the node. */
int ds_may_write(void);

#endif
