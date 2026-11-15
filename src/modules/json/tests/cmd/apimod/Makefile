# Builds apimod.so, the test module for the C functions of json_api.h, with the
# flags tests/modules/Makefile uses. The five functions stay undefined in the
# library and are resolved when a server loads it.

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

all: apimod.so

apimod.xo: apimod.c $(SRC_DIR)/valkeymodule.h
	$(CC) -I$(SRC_DIR) $(CFLAGS) $(SHOBJ_CFLAGS) -fPIC -c $< -o $@

apimod.so: apimod.xo
	$(CC) $(CFLAGS) -o $@ $^ $(SHOBJ_LDFLAGS) $(LDFLAGS) $(LIBS)

clean:
	rm -f apimod.xo apimod.so

.PHONY: all clean
