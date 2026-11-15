#!/bin/sh
# Starts or stops the private reference server the golden data is recorded
# from: a Valkey server with the external valkey-json module, on a unix socket
# in a mode-700 directory, with no TCP port.
#
#   ./run_ref.sh start | stop
#
# REF_SERVER and REF_MODULE override the binaries.
set -e
here=$(cd "$(dirname "$0")" && pwd)
dir="$here/run/ref"
server=${REF_SERVER:?set REF_SERVER to a valkey-server built with BUILD_JSON=no}
module=${REF_MODULE:?set REF_MODULE to the external valkey-json shared library}
case "$1" in
start)
    mkdir -p "$dir"
    chmod 700 "$here/run" "$dir"
    cd "$dir"
    "$server" --port 0 --unixsocket ./x.sock --unixsocketperm 700 --loadmodule "$module" \
        --save "" --appendonly no --daemonize yes --pidfile ./pid --logfile ./log --dir .
    ;;
stop)
    [ -f "$dir/pid" ] && kill "$(cat "$dir/pid")"
    ;;
*)
    echo "usage: $0 start|stop" >&2
    exit 2
    ;;
esac
