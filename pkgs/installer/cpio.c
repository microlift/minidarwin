/* Payload and Scripts archives: CPIO in the odc (070707), newc (070701) or
 * crc (070702) formats, raw or compressed (stream.c). The archive is read
 * front to back once; with a spool, file contents go to disk as they are
 * read, so a payload never has to fit in memory. */
#include "mdpkg.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static uint64_t header_field(const unsigned char *p, size_t n, int base) {
  char buf[24];
  if (n >= sizeof(buf))
    die("CPIO header field too long");
  memcpy(buf, p, n);
  buf[n] = 0;
  return parse_uint(buf, base);
}

typedef struct {
  uint64_t mode, uid, gid, nlink, mtime, namesize, filesize, check;
  size_t header_len;
  int aligned; /* newc/crc pad the name and data to 4 bytes */
  int has_crc;
} Header;

static void read_exact(Stream *s, unsigned char *buf, size_t len,
                       const char *what) {
  if (stream_read(s, buf, len) != len)
    die("truncated CPIO %s", what);
}

static void read_header(const unsigned char *h, size_t avail, Header *out) {
  if (avail < 6)
    die("truncated CPIO header");
  int odc = !memcmp(h, "070707", 6), newc = !memcmp(h, "070701", 6),
      crc = !memcmp(h, "070702", 6);
  if (!odc && !newc && !crc)
    die("unsupported payload format or compression");
  *out = (Header){.header_len = odc ? 76 : 110, .aligned = !odc,
                  .has_crc = crc};
  if (avail < out->header_len)
    die("truncated CPIO header");
  if (odc) {
    out->mode = header_field(h + 18, 6, 8);
    out->uid = header_field(h + 24, 6, 8);
    out->gid = header_field(h + 30, 6, 8);
    out->nlink = header_field(h + 36, 6, 8);
    out->mtime = header_field(h + 48, 11, 8);
    out->namesize = header_field(h + 59, 6, 8);
    out->filesize = header_field(h + 65, 11, 8);
  } else {
    out->mode = header_field(h + 14, 8, 16);
    out->uid = header_field(h + 22, 8, 16);
    out->gid = header_field(h + 30, 8, 16);
    out->nlink = header_field(h + 38, 8, 16);
    out->mtime = header_field(h + 46, 8, 16);
    out->filesize = header_field(h + 54, 8, 16);
    out->namesize = header_field(h + 94, 8, 16);
    out->check = header_field(h + 102, 8, 16);
  }
}

/* True if any component is "__MACOSX" or an AppleDouble "._name". */
static int is_appledouble(const char *path) {
  for (const char *c = path; *c; c = strchr(c, '/') ? strchr(c, '/') + 1 : "") {
    size_t n = strcspn(c, "/");
    if (!strncmp(c, "._", 2) || (n == 8 && !strncmp(c, "__MACOSX", 8)))
      return 1;
  }
  return 0;
}

static Entry make_entry(const Header *h, const char *name) {
  char *path = path_normalize(name, 0);
  mode_t type = (mode_t)h->mode & S_IFMT;
  if (is_appledouble(path))
    die("AppleDouble metadata is not supported: %s", path);
  if (type != S_IFREG && type != S_IFDIR && type != S_IFLNK)
    die("unsupported file type: %s", path);
  if (!*path && type != S_IFDIR)
    die("payload root is not a directory");
  if (h->mode > 0177777 || h->uid > UINT32_MAX || h->gid > UINT32_MAX ||
      h->mtime > (uint64_t)INT64_MAX)
    die("CPIO metadata out of range: %s", path);
  if (type == S_IFREG && h->nlink > 1)
    die("hardlinks are not supported: %s", path);
  if (h->mode & (S_ISUID | S_ISGID))
    die("set-id payloads are not supported: %s", path);
  if (type == S_IFDIR && h->filesize)
    die("directory with data: %s", path);
  if (type == S_IFLNK && (!h->filesize || h->filesize >= PATH_MAX))
    die("invalid symlink: %s", path);
  return (Entry){.path = path, .mode = (mode_t)h->mode, .uid = (uid_t)h->uid,
                 .gid = (gid_t)h->gid, .mtime = (time_t)h->mtime,
                 .size = h->filesize};
}

/* Reads an entry's data: a symlink's target, or a file's contents into the
 * spool (or memory, without one). */
static void read_data(Stream *s, const Header *h, Entry *e, Spool *spool) {
  uint32_t sum = 0;
  if (S_ISLNK(e->mode)) {
    char *target = xcalloc((size_t)h->filesize + 1, 1);
    read_exact(s, (unsigned char *)target, (size_t)h->filesize, "data");
    for (size_t i = 0; i < h->filesize; i++)
      sum += (unsigned char)target[i];
    if (memchr(target, 0, (size_t)h->filesize))
      die("invalid symlink: %s", e->path);
    e->link = target;
    symlink_check_lexical(e->path, e->link);
  } else if (S_ISREG(e->mode)) {
    if (spool) {
      if (h->filesize > LIMIT_SPOOLED - spool->used)
        die("payload exceeds the spool limit");
      e->spool = spool;
      e->spool_offset = spool->used;
    } else {
      if (h->filesize > LIMIT_MEMBER)
        die("CPIO file too large to hold in memory: %s", e->path);
      decoded_account((size_t)h->filesize);
      e->data = (Bytes){xcalloc((size_t)h->filesize, 1), (size_t)h->filesize};
    }
    unsigned char buf[1 << 16];
    for (uint64_t done = 0; done < h->filesize;) {
      size_t n = h->filesize - done > sizeof(buf) ? sizeof(buf)
                                                  : (size_t)(h->filesize - done);
      read_exact(s, buf, n, "data");
      for (size_t i = 0; i < n; i++)
        sum += buf[i];
      if (spool)
        spool_write(spool, buf, n);
      else
        memcpy(e->data.data + done, buf, n);
      done += n;
    }
  }
  if (h->has_crc && sum != h->check)
    die("CPIO checksum mismatch: %s", e->path);
}

static void skip_padding(Stream *s, uint64_t consumed, int aligned) {
  unsigned char pad[4];
  size_t n = aligned ? (size_t)((4 - consumed % 4) % 4) : 0;
  read_exact(s, pad, n, "padding");
}

void cpio_parse(Bytes archive, EntryList *out, Spool *spool) {
  Stream *s = stream_open(archive);
  unsigned char header[110];
  char *name = xcalloc(PATH_MAX, 1);
  for (;;) {
    size_t got = stream_read(s, header, 6);
    if (got < 6)
      die(got ? "truncated CPIO header" : "missing CPIO trailer");
    if (memcmp(header, "070707", 6) && memcmp(header, "070701", 6) &&
        memcmp(header, "070702", 6))
      die("unsupported payload format or compression");
    size_t len = !memcmp(header, "070707", 6) ? 76 : 110;
    read_exact(s, header + 6, len - 6, "header");
    Header h;
    read_header(header, len, &h);
    if (!h.namesize || h.namesize >= PATH_MAX)
      die("CPIO name out of bounds");
    read_exact(s, (unsigned char *)name, (size_t)h.namesize, "name");
    if (name[h.namesize - 1] || memchr(name, 0, (size_t)h.namesize - 1))
      die("invalid CPIO name");
    skip_padding(s, h.header_len + h.namesize, h.aligned);
    if (!strcmp(name, "TRAILER!!!")) {
      if (h.filesize)
        die("invalid CPIO trailer");
      break;
    }
    Entry e = make_entry(&h, name);
    read_data(s, &h, &e, spool);
    skip_padding(s, h.filesize, h.aligned);
    entries_append(out, e);
  }
  unsigned char rest[4096];
  for (size_t n; (n = stream_read(s, rest, sizeof(rest)));)
    for (size_t i = 0; i < n; i++)
      if (rest[i])
        die("data after CPIO trailer");
  stream_close(s);
  free(name);
  entries_check_paths(out, 0);
}
