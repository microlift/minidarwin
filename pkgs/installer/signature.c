/* XAR signatures. The heap holds an RSA PKCS#1 v1.5 signature of the TOC
 * checksum, and the TOC holds the signing chain. Since the TOC lists every
 * member's checksums, a valid signature covers the whole package.
 *
 * A policy names the only anchor it trusts and the extended key usage the
 * signing certificate must carry, as macOS's Security policies do. Chain
 * certificates other than the anchor come from the package and are
 * untrusted until they verify. */
#include "mdpkg.h"
#include "apple-root-ca.h"

#include <libxml/tree.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Apple Software Update Signing, the leaf usage of Apple's SU packages. */
#define OID_APPLE_SOFTWARE_UPDATE_SIGNING "1.2.840.113635.100.4.1"
#define LIMIT_CHAIN 8

typedef struct {
  const char *name;
  const unsigned char *anchor;
  size_t anchor_len;
  const char *leaf_usage;
} Policy;

static const Policy policies[] = {
    {"apple-software-update", apple_root_ca_der, sizeof(apple_root_ca_der),
     OID_APPLE_SOFTWARE_UPDATE_SIGNING},
};

static _Noreturn void refuse(const char *what) {
  unsigned long e = ERR_get_error();
  die("package signature: %s%s%s", what, e ? ": " : "",
      e ? ERR_reason_error_string(e) : "");
}

static X509 *certificate(const unsigned char *der, size_t len) {
  const unsigned char *p = der;
  X509 *x = d2i_X509(NULL, &p, (long)len);
  if (!x || p != der + len)
    refuse("malformed certificate");
  return x;
}

/* Base64 with the line breaks XML-DSig allows. */
static Bytes decode_base64(const char *text) {
  size_t len = strlen(text);
  char *compact = xcalloc(len + 1, 1);
  size_t n = 0;
  for (const char *p = text; *p; p++)
    if (!strchr(" \t\r\n", *p))
      compact[n++] = *p;
  Bytes out = {xcalloc(n / 4 * 3 + 3, 1), 0};
  int got = n ? EVP_DecodeBlock(out.data, (unsigned char *)compact, (int)n) : -1;
  if (got < 0 || n % 4)
    die("package signature: invalid base64 certificate");
  out.len = (size_t)got - (n >= 1 && compact[n - 1] == '=') -
            (n >= 2 && compact[n - 2] == '=');
  free(compact);
  return out;
}

static int has_usage(X509 *x, const char *oid) {
  EXTENDED_KEY_USAGE *usage = X509_get_ext_d2i(x, NID_ext_key_usage, NULL, NULL);
  int found = 0;
  for (int i = 0; usage && i < sk_ASN1_OBJECT_num(usage); i++) {
    char text[128];
    if (OBJ_obj2txt(text, sizeof(text), sk_ASN1_OBJECT_value(usage, i), 1) > 0 &&
        !strcmp(text, oid))
      found = 1;
  }
  EXTENDED_KEY_USAGE_free(usage);
  return found;
}

static const EVP_MD *signed_digest(const char *style) {
  return !strcasecmp(style, "sha1")     ? EVP_sha1()
         : !strcasecmp(style, "sha256") ? EVP_sha256()
         : !strcasecmp(style, "sha512") ? EVP_sha512()
                                        : NULL;
}

char *xar_verify_signature(xmlNode *toc, Bytes heap, Bytes toc_hash,
                           const char *hash_style, const char *policy_name) {
  const Policy *policy = NULL;
  for (size_t i = 0; i < sizeof(policies) / sizeof(*policies); i++)
    if (!strcmp(policies[i].name, policy_name))
      policy = &policies[i];
  if (!policy)
    die("unknown signature policy: %s", policy_name);

  xmlNode *signature = NULL;
  for (xmlNode *n = xml_elements(toc); n; n = xml_next_element(n))
    if (xml_is(n, "signature")) {
      if (signature)
        die("package signature: more than one signature");
      signature = n;
    }
  if (!signature)
    die("package is not signed, and -verify-signature %s requires it",
        policy_name);
  char *style = xml_attr(signature, "style", NULL);
  if (strcmp(style, "RSA"))
    die("package signature: unsupported style %s", style);
  free(style);
  const EVP_MD *md = signed_digest(hash_style);
  if (!md)
    die("package signature: a %s TOC checksum cannot be signed", hash_style);

  uint64_t offset = xml_child_uint(signature, "offset"),
           size = xml_child_uint(signature, "size");
  if (offset > heap.len || size > heap.len - offset || !size)
    die("package signature out of bounds");

  xmlNode *data = xml_child(xml_child(signature, "KeyInfo"), "X509Data");
  STACK_OF(X509) *chain = sk_X509_new_null();
  X509 *leaf = NULL;
  for (xmlNode *n = xml_elements(data); n; n = xml_next_element(n)) {
    if (!xml_is(n, "X509Certificate"))
      continue;
    if (sk_X509_num(chain) + !!leaf >= LIMIT_CHAIN)
      die("package signature: certificate limit");
    char *text = xml_text(n);
    Bytes der = decode_base64(text);
    X509 *x = certificate(der.data, der.len);
    free(der.data);
    free(text);
    /* XAR puts the signing certificate first. */
    if (!leaf)
      leaf = x;
    else if (!sk_X509_push(chain, x))
      refuse("out of memory");
  }
  if (!leaf)
    die("package signature without a certificate");

  Bytes anchor_der = {(unsigned char *)policy->anchor, policy->anchor_len};
#ifdef MDPKG_TESTING
  /* Only the host build that the tests run honours this: nothing but Apple
   * can sign a fixture for Apple's anchor. */
  const char *test_anchor = getenv("MDPKG_TEST_SIGNATURE_ANCHOR");
  if (test_anchor && *test_anchor)
    anchor_der = file_read(test_anchor, LIMIT_XML);
#endif
  X509 *anchor = certificate(anchor_der.data, anchor_der.len);
  X509_STORE *store = X509_STORE_new();
  X509_STORE_CTX *ctx = X509_STORE_CTX_new();
  if (!store || !ctx || !X509_STORE_add_cert(store, anchor) ||
      !X509_STORE_CTX_init(ctx, store, leaf, chain))
    refuse("cannot set up verification");
  if (X509_verify_cert(ctx) != 1)
    die("package signature: certificate chain does not verify to the %s "
        "anchor: %s", policy_name,
        X509_verify_cert_error_string(X509_STORE_CTX_get_error(ctx)));
  if (!has_usage(leaf, policy->leaf_usage))
    die("package signature: signing certificate lacks the %s usage %s",
        policy_name, policy->leaf_usage);

  EVP_PKEY *key = X509_get_pubkey(leaf);
  EVP_PKEY_CTX *verify = key ? EVP_PKEY_CTX_new(key, NULL) : NULL;
  if (!verify || EVP_PKEY_verify_init(verify) <= 0 ||
      EVP_PKEY_CTX_set_signature_md(verify, md) <= 0)
    refuse("cannot set up signature check");
  if (EVP_PKEY_verify(verify, heap.data + offset, (size_t)size, toc_hash.data,
                      toc_hash.len) != 1)
    die("package signature does not match its table of contents");

  char subject[512];
  X509_NAME_oneline(X509_get_subject_name(leaf), subject, sizeof(subject));
  EVP_PKEY_CTX_free(verify);
  EVP_PKEY_free(key);
  X509_STORE_CTX_free(ctx);
  X509_STORE_free(store);
  X509_free(anchor);
  X509_free(leaf);
  sk_X509_pop_free(chain, X509_free);
  return xstrdup(subject);
}
