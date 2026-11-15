# Standalone tests and benchmark for src/modules/json/json_num.c.
#
#   make             build everything into build/
#   make check       table check, selftests (plain, ASan+UBSan, portable
#                    fallback) and the differential test against Python
#   make check-quick the same with a 1/20 size differential test
#   make bench       micro-benchmark
#   make clean

CC ?= cc
PYTHON ?= python3
OPT ?= -O2
WARN = -W -Wall -Wextra -Wshadow -Wstrict-prototypes
CFLAGS_ALL = -std=gnu11 $(OPT) -g $(WARN) $(CFLAGS)
SAN = -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer

JSON_DIR = ../..
FPCONV_DIR = ../../../../../deps/fpconv
B = build

SRC = $(JSON_DIR)/json_num.c
HDR = $(JSON_DIR)/json_num.h

all: $(B)/json_num_test $(B)/json_num_test_san $(B)/json_num_test_portable $(B)/json_num_bench

$(B):
	mkdir -p $(B)

$(B)/json_num_test: json_num_test.c $(SRC) $(HDR) | $(B)
	$(CC) $(CFLAGS_ALL) -o $@ json_num_test.c $(SRC) $(LDFLAGS)

$(B)/json_num_test_san: json_num_test.c $(SRC) $(HDR) | $(B)
	$(CC) $(CFLAGS_ALL) $(SAN) -o $@ json_num_test.c $(SRC) $(LDFLAGS)

# The plain C11 fallbacks: no __int128, no builtins, no eight-digit steps.
$(B)/json_num_test_portable: json_num_test.c $(SRC) $(HDR) | $(B)
	$(CC) $(CFLAGS_ALL) -DJSON_NUM_PORTABLE -o $@ json_num_test.c $(SRC) $(LDFLAGS)

# fpconv is third party code: build it without our warning flags.
$(B)/fpconv_dtoa.o: $(FPCONV_DIR)/fpconv_dtoa.c | $(B)
	$(CC) -std=gnu11 $(OPT) $(CFLAGS) -c -o $@ $<

$(B)/json_num_bench: json_num_bench.c $(SRC) $(HDR) $(B)/fpconv_dtoa.o | $(B)
	$(CC) $(CFLAGS_ALL) -I$(FPCONV_DIR) -o $@ json_num_bench.c $(SRC) $(B)/fpconv_dtoa.o $(LDFLAGS)

check-table:
	$(PYTHON) gen_pow10.py --check $(SRC)

selftest: $(B)/json_num_test $(B)/json_num_test_san $(B)/json_num_test_portable
	$(B)/json_num_test selftest
	$(B)/json_num_test_san selftest
	$(B)/json_num_test_portable selftest

check: check-table selftest
	$(PYTHON) check_num.py $(B)/json_num_test
	$(PYTHON) check_num.py --scale 0.02 $(B)/json_num_test_san
	$(PYTHON) check_num.py --scale 0.02 $(B)/json_num_test_portable

check-quick: check-table selftest
	$(PYTHON) check_num.py --scale 0.05 $(B)/json_num_test
	$(PYTHON) check_num.py --scale 0.01 $(B)/json_num_test_san

bench: $(B)/json_num_bench
	$(B)/json_num_bench

clean:
	rm -rf $(B)

.PHONY: all check-table selftest check check-quick bench clean
