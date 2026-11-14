/* XAR, the container of a flat package. Layout: a big-endian header, a
 * zlib-compressed XML table of contents, then a heap. The TOC names each
 * member's heap range and checksums; the TOC's own checksum is in the heap.
 * The file is mapped, and members are decoded only when asked for: a
 * component's Payload can be larger than memory. */
#include "mdpkg.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define XAR_HEADER_MIN 28
#define XAR_HEADER_NAMED 64 /* with the 36-byte checksum name */

static uint64_t big_endian(const unsigned char *p, size_t n) {
  uint64_t v = 0;
  while (n--)
    v = (v << 8) | *p++;
  return v;
}

static void verify_checksum(const char *style, const char *want, Bytes data) {
  Bytes hash = digest(data, style);
  char *got = hex_encode(hash);
  if (strcasecmp(want, got))
    die("XAR checksum mismatch");
  free(got);
  free(hash.data);
}

static void checksum_of(xmlNode *checksum, char **style, char **value) {
  if (!checksum)
    die("missing XAR checksum");
  *style = xml_attr(checksum, "style", NULL);
  *value = xml_text(checksum);
}

static XarMember describe_member(char *name, xmlNode *data, Bytes heap) {
  XarMember m = {.name = name};
  m.offset = xml_child_uint(data, "offset");
  m.length = xml_child_uint(data, "length");
  m.size = xml_child_uint(data, "size");
  if (m.offset > heap.len || m.length > heap.len - m.offset)
    die("XAR member out of bounds");
  m.encoding = xml_attr(xml_child(data, "encoding"), "style", NULL);
  if (strcmp(m.encoding, "application/x-gzip") &&
      strcmp(m.encoding, "application/octet-stream"))
    die("unsupported XAR encoding: %s", m.encoding);
  if (!strcmp(m.encoding, "application/octet-stream") && m.length != m.size)
    die("XAR member size mismatch");
  if (!strcmp(m.encoding, "application/x-gzip") && m.size > LIMIT_MEMBER)
    die("XAR member out of bounds");
  checksum_of(xml_child(data, "archived-checksum"), &m.archived_style,
              &m.archived);
  checksum_of(xml_child(data, "extracted-checksum"), &m.extracted_style,
              &m.extracted);
  return m;
}

/* A stored member is the mapped heap itself, never copied. */
static Bytes read_member(Xar *xar, XarMember *m) {
  Bytes encoded = {xar->heap.data + m->offset, (size_t)m->length};
  verify_checksum(m->archived_style, m->archived, encoded);
  if (!strcmp(m->encoding, "application/octet-stream")) {
    if (strcasecmp(m->archived_style, m->extracted_style) ||
        strcasecmp(m->archived, m->extracted))
      verify_checksum(m->extracted_style, m->extracted, encoded);
    return encoded;
  }
  Bytes out = inflate_limited(encoded, 15, (size_t)m->size, LIMIT_MEMBER);
  verify_checksum(m->extracted_style, m->extracted, out);
  return out;
}

/* Collects every file with data. Directories only contribute a prefix;
 * member metadata (modes, xattrs) is ignored because members are never
 * written to disk as themselves. */
static void walk_toc(Xar *xar, xmlNode *parent, const char *prefix, int depth) {
  if (depth > LIMIT_DEPTH)
    die("XAR nesting limit");
  for (xmlNode *n = xml_elements(parent); n;
       n = xml_next_element(n)) {
    if (!xml_is(n, "file"))
      continue;
    char *name = xml_text(xml_child(n, "name"));
    if (!*name || strchr(name, '/') || !strcmp(name, ".") ||
        !strcmp(name, ".."))
      die("unsafe XAR member name");
    char *joined = path_join(prefix, name);
    char *path = path_normalize(joined, 0);
    free(joined);
    free(name);
    char *type = xml_text(xml_child(n, "type"));
    xmlNode *data = xml_child(n, "data");
    if (!strcmp(type, "directory") && !data)
      walk_toc(xar, n, path, depth + 1);
    else if (!strcmp(type, "file") && data) {
      if (xar->count >= LIMIT_ENTRIES)
        die("XAR member limit");
      xar->members = xrealloc(xar->members, xar->count + 1, sizeof(XarMember));
      xar->members[xar->count++] = describe_member(path, data, xar->heap);
      path = NULL;
    } else
      die("unsupported XAR member: %s (%s)", path, type);
    free(type);
    free(path);
  }
}

static int member_compare(const void *a, const void *b) {
  return strcmp(((const XarMember *)a)->name, ((const XarMember *)b)->name);
}

static void map_file(Xar *xar, const char *path) {
  int fd = open(path, O_RDONLY | O_NOFOLLOW);
  struct stat st;
  if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode))
    die("cannot read regular file %s", path);
  if ((uint64_t)st.st_size > LIMIT_PACKAGE || (uint64_t)st.st_size > SIZE_MAX)
    die("file too large: %s", path);
  xar->map_len = (size_t)st.st_size;
  if (xar->map_len) {
    xar->map = mmap(NULL, xar->map_len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (xar->map == MAP_FAILED)
      die("mmap %s: %s", path, strerror(errno));
  }
  close(fd);
}

void xar_open(Xar *xar, const char *path) {
  *xar = (Xar){.path = xstrdup(path)};
  map_file(xar, path);
  Bytes file = {xar->map, xar->map_len};
  if (file.len < XAR_HEADER_MIN || memcmp(file.data, "xar!", 4) ||
      big_endian(file.data + 6, 2) != 1)
    die("not a flat package (XAR version 1)");
  uint64_t header = big_endian(file.data + 4, 2),
           toc_packed = big_endian(file.data + 8, 8),
           toc_size = big_endian(file.data + 16, 8),
           algorithm = big_endian(file.data + 24, 4);
  if (header < XAR_HEADER_MIN || header > file.len ||
      toc_packed > file.len - header || !toc_size || toc_size > LIMIT_XML)
    die("invalid XAR header");
  Bytes packed = {file.data + header, (size_t)toc_packed};
  xar->heap = (Bytes){packed.data + packed.len, file.len - header - toc_packed};
  Bytes toc_xml = inflate_limited(packed, 15, (size_t)toc_size, LIMIT_XML);
  xmlDoc *doc = xml_parse(toc_xml);
  xmlNode *toc = xml_child(xml_root(doc, "xar"), "toc");
  xmlNode *checksum = xml_child(toc, "checksum");
  if (!toc || !checksum)
    die("XAR without a TOC checksum");

  /* 1 and 2 are fixed; 3 ("other") names the algorithm in the header when it
   * is long enough to hold one, and otherwise leaves it to the TOC. */
  char *style = xml_attr(checksum, "style", NULL);
  const char *expected = algorithm == 1 ? "sha1" : algorithm == 2 ? "md5" : NULL;
  char named[37] = {0};
  if (algorithm == 3 && header >= XAR_HEADER_NAMED) {
    memcpy(named, file.data + 28, 36);
    expected = named;
  } else if (algorithm == 3)
    expected = style;
  if (!expected || strcmp(style, expected))
    die("unsupported or inconsistent XAR TOC checksum");
  uint64_t offset = xml_child_uint(checksum, "offset"),
           size = xml_child_uint(checksum, "size");
  Bytes hash = digest(packed, style);
  if (offset > xar->heap.len || size > xar->heap.len - offset ||
      size != hash.len || memcmp(hash.data, xar->heap.data + offset, hash.len))
    die("XAR TOC checksum mismatch");
  if (opt_signature_policy)
    xar->signer = xar_verify_signature(toc, xar->heap, hash, style,
                                       opt_signature_policy);
  free(hash.data);
  free(style);

  walk_toc(xar, toc, "", 0);
  qsort(xar->members, xar->count, sizeof(XarMember), member_compare);
  for (size_t i = 1; i < xar->count; i++)
    if (!strcmp(xar->members[i - 1].name, xar->members[i].name))
      die("duplicate XAR member: %s", xar->members[i].name);
  xml_free(doc);
  free(toc_xml.data);
}

Bytes xar_member(Xar *xar, const char *name, int required) {
  XarMember key = {.name = (char *)name};
  XarMember *m = bsearch(&key, xar->members, xar->count, sizeof(XarMember),
                         member_compare);
  if (!m) {
    if (required)
      die("missing package member: %s", name);
    return (Bytes){NULL, 0};
  }
  if (!m->read) {
    m->data = read_member(xar, m);
    m->read = 1;
  }
  return m->data;
}
