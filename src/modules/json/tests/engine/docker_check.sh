#!/bin/sh
# Runs this directory's suites for another target. The C programs are built
# and run in a container of that target; the Python drivers and the model
# run on this machine, since they do not depend on the target and under
# emulation they would take most of the time.
#
#   docker_check.sh <image> <platform> <name> [quick|full]
#
# e.g. docker_check.sh json-port:s390x linux/s390x s390x quick. Objects go to
# build/<name>, so targets never mix. The container has no network. SAN, if
# set, replaces the sanitizer flags of the _san builds: AddressSanitizer does
# not start under qemu user emulation of s390x, UBSan alone does.
set -eu
image=$1 platform=$2 name=$3 mode=${4:-quick}
here=$(cd "$(dirname "$0")" && pwd)
json=$(cd "$here/../.." && pwd)
ctr=json-engine-check-$name-$$
B=build/$name
run=build/$name-run
docker run -d --rm --name "$ctr" --network none --platform "$platform" -v "$json:/work/json" -w /work/json/tests/engine "$image" sleep infinity >/dev/null
trap 'docker stop -t 1 "$ctr" >/dev/null 2>&1 || true' EXIT INT TERM
docker exec "$ctr" make CC=gcc B="$B" ${SAN:+"SAN=$SAN"} "$B/engine_test" "$B/engine_test_san" "$B/layout_test" "$B/layout_test_san"
mkdir -p "$here/$run"
for prog in engine_test engine_test_san layout_test layout_test_san; do
    printf '#!/bin/sh\nexec docker exec -i %s /work/json/tests/engine/%s/%s "$@"\n' "$ctr" "$B" "$prog" >"$here/$run/$prog"
    chmod +x "$here/$run/$prog"
done
cd "$here"
PYTHONDONTWRITEBYTECODE=1 python3 check_layout.py "$run"
PYTHONDONTWRITEBYTECODE=1 python3 check_engine.py "$run" "$mode"
PYTHONDONTWRITEBYTECODE=1 python3 check_mutate.py "$run" "$mode"
