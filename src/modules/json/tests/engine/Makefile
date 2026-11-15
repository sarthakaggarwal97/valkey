# Standalone tests and benchmark for src/modules/json/json_engine.c.
#
#   make             build everything into build/
#   make check       unit tests, the differential tests against Python and the
#                    model-based tests of changes and defrag, plain and under
#                    ASan+UBSan
#   make check-quick the same on a smaller corpus
#   make bench       parse / serialise / free / lookup timings, and timings of
#                    every kind of change
#   make clean
#
# NUM=stub links num_stub.c in place of ../../json_num.c. Give it a build
# directory of its own: make check-quick NUM=stub B=build/stub

CC = clang
PYTHON ?= python3
OPT ?= -O2
WARN = -W -Wall -Wextra -Wshadow -Wstrict-prototypes
CFLAGS_ALL = -std=gnu11 -g $(WARN) $(CFLAGS)
SAN = -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -DJSON_ENGINE_DEBUG

JSON_DIR = ../..
B = build
NUM ?= real
ifeq ($(NUM),stub)
NUM_SRC = num_stub.c
else
NUM_SRC = $(JSON_DIR)/json_num.c
endif

SRC = $(JSON_DIR)/json_engine.c $(NUM_SRC)
HDR = $(JSON_DIR)/json_engine.h $(JSON_DIR)/json_engine_format.h $(JSON_DIR)/json_engine_stats.h $(JSON_DIR)/json_num.h

all: $(B)/engine_test $(B)/engine_test_san $(B)/engine_bench

$(B):
	mkdir -p $(B)

$(B)/engine_test: engine_test.c $(SRC) $(HDR) | $(B)
	$(CC) $(CFLAGS_ALL) $(OPT) -o $@ engine_test.c $(SRC) $(LDFLAGS) -lpthread -lm

$(B)/engine_test_san: engine_test.c $(SRC) $(HDR) | $(B)
	$(CC) $(CFLAGS_ALL) $(SAN) -o $@ engine_test.c $(SRC) $(LDFLAGS) -lpthread -lm

$(B)/engine_bench: engine_bench.c $(SRC) $(HDR) | $(B)
	$(CC) $(CFLAGS_ALL) -O3 -mcpu=native -o $@ engine_bench.c $(SRC) $(LDFLAGS) -lpthread -lm

check: $(B)/engine_test $(B)/engine_test_san
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) check_engine.py $(B) full
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) check_mutate.py $(B) full

check-quick: $(B)/engine_test $(B)/engine_test_san
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) check_engine.py $(B) quick
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) check_mutate.py $(B) quick

bench: $(B)/engine_bench
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) gen_docs.py corpus $(B)/data 100000
	$(B)/engine_bench $(B)/data

clean:
	rm -rf $(B)

.PHONY: all check check-quick bench clean
