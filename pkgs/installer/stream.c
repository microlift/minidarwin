/* Payload and Scripts archives as a byte stream: raw CPIO, gzip, or pbzx.
 *
 * pbzx is the Software Update payload format: "pbzx", a big-endian 64-bit
 * chunk size, then chunks of (64-bit decoded length, 64-bit encoded length,
 * data). A chunk whose two lengths are equal is stored; any other is one
 * complete .xz stream that decodes to exactly the decoded length. */
#include "mdpkg.h"

#include <limits.h>
#include <lzma.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

enum { RAW, GZIP, PBZX };

struct Stream {
  int kind;
  Bytes in;
  size_t pos;
  z_stream z;
  int z_done;
  unsigned char *chunk; /* pbzx: the current decoded chunk */
  size_t chunk_size, chunk_len, chunk_pos;
};

static uint64_t big_endian64(const unsigned char *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++)
    v = (v << 8) | p[i];
  return v;
}

Stream *stream_open(Bytes encoded) {
  Stream *s = xcalloc(1, sizeof(*s));
  s->in = encoded;
  if (encoded.len >= 2 && encoded.data[0] == 0x1f && encoded.data[1] == 0x8b) {
    s->kind = GZIP;
    if (inflateInit2(&s->z, 31) != Z_OK)
      die("invalid compressed stream");
  } else if (encoded.len >= 4 && !memcmp(encoded.data, "pbzx", 4)) {
    s->kind = PBZX;
    if (encoded.len < 12)
      die("truncated pbzx header");
    uint64_t size = big_endian64(encoded.data + 4);
    if (!size || size > LIMIT_PBZX_CHUNK)
      die("unsupported pbzx chunk size");
    s->chunk_size = (size_t)size;
    s->chunk = xcalloc(s->chunk_size, 1);
    s->pos = 12;
  } else
    s->kind = RAW;
  return s;
}

static void xz_chunk(Stream *s, const unsigned char *in, size_t len,
                     size_t decoded) {
  lzma_stream z = LZMA_STREAM_INIT;
  if (lzma_stream_decoder(&z, LIMIT_XZ_MEMORY, 0) != LZMA_OK)
    die("cannot start xz decoder");
  z.next_in = in;
  z.avail_in = len;
  z.next_out = s->chunk;
  z.avail_out = decoded;
  lzma_ret r = lzma_code(&z, LZMA_FINISH);
  /* Ending with output room left would mean a short chunk; ending with no
   * room before LZMA_STREAM_END, a long one. */
  if (r != LZMA_STREAM_END || z.avail_in || z.avail_out)
    die("corrupt pbzx chunk (%s)", r == LZMA_MEMLIMIT_ERROR ? "xz memory limit"
                                   : r == LZMA_STREAM_END   ? "size mismatch"
                                                            : "xz error");
  lzma_end(&z);
}

/* Returns 0 at a clean end of the stream. */
static int next_chunk(Stream *s) {
  if (s->pos == s->in.len)
    return 0;
  if (s->in.len - s->pos < 16)
    die("truncated pbzx chunk header");
  uint64_t decoded = big_endian64(s->in.data + s->pos),
           encoded = big_endian64(s->in.data + s->pos + 8);
  s->pos += 16;
  if (!decoded || decoded > s->chunk_size || encoded > s->in.len - s->pos)
    die("pbzx chunk out of bounds");
  const unsigned char *in = s->in.data + s->pos;
  if (encoded == decoded)
    memcpy(s->chunk, in, (size_t)decoded);
  else
    xz_chunk(s, in, (size_t)encoded, (size_t)decoded);
  s->pos += (size_t)encoded;
  s->chunk_len = (size_t)decoded;
  s->chunk_pos = 0;
  return 1;
}

static size_t read_gzip(Stream *s, unsigned char *buf, size_t len) {
  if (s->z_done)
    return 0;
  if (s->in.len > UINT_MAX)
    die("compressed stream too large");
  s->z.next_in = s->in.data + s->pos;
  s->z.avail_in = (uInt)(s->in.len - s->pos);
  s->z.next_out = buf;
  s->z.avail_out = len > UINT_MAX ? UINT_MAX : (uInt)len;
  size_t room = s->z.avail_out;
  while (s->z.avail_out) {
    int status = inflate(&s->z, Z_NO_FLUSH);
    if (status == Z_STREAM_END) {
      s->z_done = 1;
      break;
    }
    if (status != Z_OK)
      die("corrupt compressed stream");
    if (!s->z.avail_in)
      die("truncated compressed stream");
  }
  s->pos = s->in.len - s->z.avail_in;
  if (s->z_done && s->pos != s->in.len)
    die("data after compressed stream");
  return room - s->z.avail_out;
}

size_t stream_read(Stream *s, unsigned char *buf, size_t len) {
  size_t done = 0;
  while (done < len) {
    size_t n;
    if (s->kind == RAW) {
      n = s->in.len - s->pos;
      if (n > len - done)
        n = len - done;
      memcpy(buf + done, s->in.data + s->pos, n);
      s->pos += n;
    } else if (s->kind == GZIP)
      n = read_gzip(s, buf + done, len - done);
    else {
      if (s->chunk_pos == s->chunk_len && !next_chunk(s))
        break;
      n = s->chunk_len - s->chunk_pos;
      if (n > len - done)
        n = len - done;
      memcpy(buf + done, s->chunk + s->chunk_pos, n);
      s->chunk_pos += n;
    }
    if (!n)
      break;
    done += n;
  }
  return done;
}

void stream_close(Stream *s) {
  if (s->kind == GZIP)
    inflateEnd(&s->z);
  free(s->chunk);
  free(s);
}
