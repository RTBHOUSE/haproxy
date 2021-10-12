#!/bin/bash

set -eu

# Inputs
lb_algo=$1

# Constants
haproxy_port_range_start=10000
haproxy_stats_port_range_start=11000
haproxy_instances=5

haproxy_pids=()
cleanup() {
        for haproxy_pid in ${haproxy_pids[@]}; do
                kill -TERM $haproxy_pid
        done
}
trap cleanup EXIT

for (( instance=0; instance < ${haproxy_instances}; instance++ )); do
        haproxy_port=$((${haproxy_port_range_start}+$instance))
        haproxy_cfg=haproxy_${haproxy_port}.cfg
        haproxy_stats_port=$((${haproxy_stats_port_range_start}+$instance))

        echo $instance
        echo $haproxy_port

        read -r -d '' CFG <<EOF || true
global
        stats socket /tmp/haproxy_${haproxy_port}.sock mode 660 level admin

defaults
        mode http

        option httplog
        log stdout format raw local0 info

        default_backend workers

        timeout client 60000
        timeout connect 20000
        timeout server 20000

frontend http
        bind localhost:${haproxy_port}

frontend stats
        bind localhost:${haproxy_stats_port}
        stats enable
        stats uri /
        stats refresh 5s

backend workers
        balance ${lb_algo}

        option httpchk

        http-check connect linger
        http-check send uri /health meth GET

        default-server check inter 5000

        server worker_12000 127.0.0.1:12000 weight 5
        server worker_12001 127.0.0.1:12001 weight 5
        server worker_12002 127.0.0.1:12002 weight 5
        server worker_12003 127.0.0.1:12003 weight 5
        server worker_12004 127.0.0.1:12004 weight 5
        server worker_12005 127.0.0.1:12005 weight 7
        server worker_12006 127.0.0.1:12006 weight 7
        server worker_12007 127.0.0.1:12007 weight 7
        server worker_12008 127.0.0.1:12008 weight 7
        server worker_12009 127.0.0.1:12009 weight 7
        server worker_12010 127.0.0.1:12010 weight 9
        server worker_12011 127.0.0.1:12011 weight 9
        server worker_12012 127.0.0.1:12012 weight 9
        server worker_12013 127.0.0.1:12013 weight 9
        server worker_12014 127.0.0.1:12014 weight 9
        server worker_12015 127.0.0.1:12015 weight 11
        server worker_12016 127.0.0.1:12016 weight 11
        server worker_12017 127.0.0.1:12017 weight 11
        server worker_12018 127.0.0.1:12018 weight 11
        server worker_12019 127.0.0.1:12019 weight 11
        server worker_12020 127.0.0.1:12020 weight 13
        server worker_12021 127.0.0.1:12021 weight 13
        server worker_12022 127.0.0.1:12022 weight 13
        server worker_12023 127.0.0.1:12023 weight 13
        server worker_12024 127.0.0.1:12024 weight 13
EOF

        echo -e "${CFG}" > ${haproxy_cfg}

        ../haproxy -f ${haproxy_cfg} &

        haproxy_pids+=( $! )
done


for haproxy_pid in ${haproxy_pids[@]}; do
        wait $haproxy_pid
done
