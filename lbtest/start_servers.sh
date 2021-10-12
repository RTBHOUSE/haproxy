#!/bin/bash

set -eu

server_pids=()
cleanup() {
        for server_pid in ${server_pids[@]}; do
                kill -TERM $server_pid
        done
}
trap cleanup EXIT

for (( instance=0; instance < 25; instance++ )); do
        server_port=$((12000+$instance))
        python3 server.py $server_port &
        server_pids+=( $! )
done


for server_pid in ${server_pids[@]}; do
        wait $server_pid
done
