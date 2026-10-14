#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

name='socket-criu-test'

if [ "$(id -u)" -ne 0 ]; then
    printf 'Run with sudo.\n' >&2
    exit 1
fi
gcc -O2 -Wall -Wextra -Wpedantic -pthread socket_churn.c -o socket_churn

./socket_churn --host &
host_pid=$!
trap 'kill "$host_pid" 2>/dev/null || true
      wait "$host_pid" 2>/dev/null || true
    podman stop --time 1 "$name" || true' 0

podman run --detach --replace --name "$name" \
    --network=host --user 0 --volume "$PWD/socket_churn:/socket_churn:ro" \
    --entrypoint /socket_churn docker.io/library/debian:trixie-slim
static_dir=$(podman inspect --format '{{.StaticDir}}' "$name")

while :; do
    kill -0 "$host_pid"
    printf 'Checkpointing...\n'
    podman container checkpoint "$name" || {
        status=$?
        if [ -f "$static_dir/dump.log" ]; then
            cat "$static_dir/dump.log"
        fi
        exit "$status"
    }
    printf 'Restoring...\n'
    podman container restore "$name"
done
