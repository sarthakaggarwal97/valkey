# Tests of the path compiler and evaluator (../../json_path.c).
#
#   make compile-test   compile-only tests, no engine needed
#   make test           all tests against the engine: compile, unit, golden
#   make asan           the same under AddressSanitizer and UBSan
#   make bench          timing loops: evaluation alone, then compile + evaluate + free
#   make golden         re-record golden.gz from the reference server
#                       (./run_ref.sh start first, ./run_ref.sh stop after)
#   make cases          re-record compile_cases.txt from the current compiler

CC ?= cc
SRC = ../..
WARN = -std=gnu11 -W -Wall -Wextra -Wshadow -Wstrict-prototypes
CFLAGS_T = $(WARN) -g -DJSON_PATH_DEBUG -I$(SRC)
ENGINE = $(SRC)/json_engine.c $(SRC)/json_num.c
SAN = -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
MODULE_TESTS ?= $(HOME)/workspace/valkey-json-pr95/tst

B = build

all: test

$(B):
	mkdir -p $(B)

$(B)/path_test_noengine: path_test.c $(SRC)/json_path.c $(SRC)/json_path.h | $(B)
	$(CC) $(CFLAGS_T) -O1 -DJSON_PATH_NO_ENGINE path_test.c $(SRC)/json_path.c -w $(SRC)/json_num.c -o $@

$(B)/path_test: path_test.c $(SRC)/json_path.c $(SRC)/json_path.h $(ENGINE) | $(B)
	$(CC) $(CFLAGS_T) -O2 path_test.c $(SRC)/json_path.c -w $(ENGINE) -o $@

$(B)/path_test_asan: path_test.c $(SRC)/json_path.c $(SRC)/json_path.h $(ENGINE) | $(B)
	$(CC) $(CFLAGS_T) -O1 $(SAN) path_test.c $(SRC)/json_path.c -w $(ENGINE) -o $@

compile-test: $(B)/path_test_noengine
	$(B)/path_test_noengine compile compile_cases.txt static_cases.txt

test: compile-test $(B)/path_test
	$(B)/path_test compile compile_cases.txt static_cases.txt
	$(B)/path_test unit
	gzip -dc golden.gz | $(B)/path_test golden

asan: $(B)/path_test_asan
	$(B)/path_test_asan compile compile_cases.txt static_cases.txt
	$(B)/path_test_asan unit
	gzip -dc golden.gz | $(B)/path_test_asan golden

bench: $(B)/path_test
	$(B)/path_test bench
	$(B)/path_test use

golden:
	python3 gen_corpus.py run/ref/x.sock $(MODULE_TESTS) golden.gz

cases: $(B)/path_test_noengine
	cut -f1 static_cases.txt | $(B)/path_test_noengine dump > compile_cases.new && mv compile_cases.new compile_cases.txt

clean:
	rm -rf $(B)

.PHONY: all compile-test test asan bench golden cases clean
