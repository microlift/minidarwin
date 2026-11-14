# Used by bootstrap.nix (host world). installer.nix compiles the same
# sources with the target toolchain and does not use this file.
CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Werror
CPPFLAGS += $(shell pkg-config --cflags libxml-2.0 libcrypto zlib liblzma)
LDLIBS += $(shell pkg-config --libs libxml-2.0 libcrypto zlib liblzma)
# Lets the tests name their own signature anchor (signature.c).
CPPFLAGS += -DMDPKG_TESTING
CPPFLAGS += -isystem $(QUICKJS)/include/quickjs
LDLIBS += $(QUICKJS)/lib/libquickjs.a -lm -lpthread

SOURCES = main.c util.c xml.c xar.c signature.c stream.c cpio.c package.c install.c receipts.c installer-js.c js-plist.c
OBJECTS = $(SOURCES:.c=.o)

mdpkg: $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) $(LDLIBS) -o $@

$(OBJECTS): mdpkg.h installer-js.h

# APPLE_ROOT_CA: Apple Root CA in DER, from security_certificates.
apple-root-ca.h: $(APPLE_ROOT_CA) der-to-header.sh
	sh der-to-header.sh $(APPLE_ROOT_CA) apple_root_ca_der > $@

signature.o: apple-root-ca.h

.PHONY: clean
clean:
	rm -f mdpkg $(OBJECTS) apple-root-ca.h
