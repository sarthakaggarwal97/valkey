#!/bin/sh
# Prints the runtest arguments that select every JSON Tcl test file.
# Run from the top of the source tree.
for f in tests/unit/type/json*.tcl tests/integration/json-*.tcl; do
    n=${f#tests/}
    printf ' --single %s' "${n%.tcl}"
done
echo
