# Builds searchmod.so, a test module that uses JSON the way valkey-search does,
# with the flags tests/modules/Makefile uses. It has no undefined symbol of the
# JSON module: it finds the module when it is loaded.

SRC_DIR = ../../../../..
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')

warning_cflags = -W -Wall -Wno-missing-field-initializers
ifeq ($(uname_S),Darwin)
	SHOBJ_CFLAGS ?= $(warning_cflags) -dynamic -fno-common -g -ggdb -std=gnu11 -O2
	SHOBJ_LDFLAGS ?= -bundle -undefined dynamic_lookup
else
	SHOBJ_CFLAGS ?= $(warning_cflags) -fno-common -g -ggdb -std=gnu11 -O2
	SHOBJ_LDFLAGS ?= -shared
endif

all: searchmod.so

searchmod.xo: searchmod.c $(SRC_DIR)/valkeymodule.h
	$(CC) -I$(SRC_DIR) $(CFLAGS) $(SHOBJ_CFLAGS) -fPIC -c $< -o $@

searchmod.so: searchmod.xo
	$(CC) $(CFLAGS) -o $@ $^ $(SHOBJ_LDFLAGS) $(LDFLAGS) $(LIBS)

clean:
	rm -f searchmod.xo searchmod.so

.PHONY: all clean
