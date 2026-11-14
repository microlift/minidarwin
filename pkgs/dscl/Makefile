# Used by bootstrap.nix (host world) and dscl.nix (target world).
CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Werror -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE
LDLIBS += -lxml2

all: dscl dseditgroup

dscl: dscl.o dslocal.o
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

dseditgroup: dseditgroup.o dslocal.o
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

dscl.o dseditgroup.o dslocal.o: dslocal.h

.PHONY: all clean
clean:
	rm -f dscl dseditgroup *.o
