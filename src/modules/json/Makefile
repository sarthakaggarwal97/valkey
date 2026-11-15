# Built-in JSON module. Built as a static library and linked into
# valkey-server (see BUILD_JSON in ../../Makefile).

# Strip LTO flags, as the Lua module does: LTO bitcode objects in a .a cause
# linker failures when the system linker cannot read them. 'override' is
# needed because OPTIMIZATION is passed on the command line by the parent.
override OPTIMIZATION:=$(subst -flto,,$(subst -ffat-lto-objects,,$(subst -flto=auto,,$(OPTIMIZATION))))

JSON_TARGET=libvalkeyjson.a
JSON_CFLAGS=-I. -fPIC -W -Wall -fno-common $(OPTIMIZATION) -std=gnu11 -D_GNU_SOURCE $(CFLAGS) $(CPPFLAGS)

SRCS=json_module.c json_cmd.c json_path.c json_engine.c json_num.c
OBJS=$(SRCS:.c=.o)

CC_COLOR="\033[32m"
LINKCOLOR_MOD="\033[36;1m"
SRC_COLOR="\033[33m"
SCOPE_COLOR="\033[38:5:250m"
BINCOLOR_MOD="\033[35m"
RESET="\033[0m"

ifndef V
QUIET_CC_MOD = @printf '    %b %b(JSON Module)%b %b\n' $(CC_COLOR)CC$(RESET) $(SCOPE_COLOR) $(RESET) $(SRC_COLOR)$<$(RESET) 1>&2;
QUIET_LINK_MOD = @printf '    %b %b\n' $(LINKCOLOR_MOD)LINK$(RESET) $(BINCOLOR_MOD)$@$(RESET) 1>&2;
endif

all: $(JSON_TARGET)

$(JSON_TARGET): $(OBJS)
	$(QUIET_LINK_MOD)rm -f $@ && $(AR) rcs $@ $(OBJS)

%.o: %.c
	$(QUIET_CC_MOD)$(CC) $(JSON_CFLAGS) -MMD -c $< -o $@

-include $(OBJS:.o=.d)

clean:
	rm -f *.a *.d *.o
