#!/bin/bash

set -eu

connections=$1
haproxy_instances=$2

# Constants
haproxy_port_range_start=10000

wrk_pids=()
cleanup() {
        for wrk_pid in ${wrk_pids[@]}; do
                kill -TERM $wrk_pid
        done
}
trap cleanup EXIT

for (( instance=0; instance < ${haproxy_instances}; instance++ )); do
        haproxy_port=$((${haproxy_port_range_start}+$instance))
        connections_per_haproxy=$(($connections / $haproxy_instances))

        echo chuj
        echo $connections_per_haproxy

        wrk -c$connections_per_haproxy -d300 http://localhost:${haproxy_port}/ &
        wrk_pids+=( $! )
done


for wrk_pid in ${wrk_pids[@]}; do
        wait $wrk_pid
done
