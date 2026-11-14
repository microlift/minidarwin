# Host build for the tests (bootstrap.nix): every system path goes under
# $XCSELECT_TEST_ROOT (XCSELECT_TESTING), and the library is found next to
# the tools rather than as the host's own /usr/lib/libxcselect.dylib.
# xcselect.nix builds the same sources for the rootfs and does not use this.
CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=gnu11 -fblocks -Wall -Wextra -Werror -fvisibility=hidden
CPPFLAGS += -DXCSELECT_TESTING
INSTALL_CFLAGS = $(shell pkg-config --cflags libxml-2.0 libcrypto zlib)
INSTALL_LIBS = $(shell pkg-config --libs libxml-2.0 libcrypto zlib)
LIB = -L. -lxcselect -Wl,-rpath,@executable_path/../lib

all: libxcselect.dylib xcode-select xcrun tool-shim xtool-shim xcselect-install

libxcselect.dylib: libxcselect.c xcselect-private.h xcselect.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -dynamiclib -install_name @rpath/libxcselect.dylib \
	  libxcselect.c -o $@

xcode-select: xcode-select.c libxcselect.dylib
	$(CC) $(CFLAGS) $(CPPFLAGS) xcode-select.c $(LIB) -o $@

xcrun: shim.c libxcselect.dylib
	$(CC) $(CFLAGS) $(CPPFLAGS) -DXCRUN shim.c $(LIB) -o $@

tool-shim: shim.c libxcselect.dylib
	$(CC) $(CFLAGS) $(CPPFLAGS) -DTOOL_FLAGS=0 shim.c $(LIB) -o $@

xtool-shim: shim.c libxcselect.dylib
	$(CC) $(CFLAGS) $(CPPFLAGS) -DTOOL_FLAGS=XCSELECT_FLAG_REQUIRE_XCODE shim.c $(LIB) -o $@

xcselect-install: xcselect-install.c xcselect-private.h
	$(CC) $(CFLAGS) $(CPPFLAGS) $(INSTALL_CFLAGS) xcselect-install.c $(INSTALL_LIBS) -o $@

.PHONY: all clean
clean:
	rm -f libxcselect.dylib xcode-select xcrun tool-shim xtool-shim xcselect-install
