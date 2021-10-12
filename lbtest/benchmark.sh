#!/bin/bash

set -eu

start=$1
stop=$2
step=$3
proxies=$4

query() {
        weight=$1
        weights_sum=$2
        conns=$3
        port_start=$4
        port_end=$4

        inflight_sum=0
        for (( i=0; i < 10; i++ )); do
                port=$(seq $port_start $port_end)
                server_addr="localhost:$port"
                inflight=$(curl -s ${server_addr}/inflight)
                inflight_sum=$(($inflight_sum + $inflight))
                sleep 0.1
        done

        awk -f <(cat <<EOF
BEGIN {
        perfect=($weight/$weights_sum)*($conns*1.0)
        inflight=($inflight_sum)/10.0
        inflight_over_perfect=inflight/perfect

        printf("%2.2f", inflight_over_perfect)
}
EOF
) /dev/null
}

for conns in $(seq $start $step $stop); do
        ./start_wrks.sh $conns $proxies > /dev/null &
        wrk=$!

        sleep 5

        a=$(query 5 225 $conns 12000 12004)
        b=$(query 7 225 $conns 12005 12009)
        c=$(query 9 225 $conns 12010 12014)
        d=$(query 11 225 $conns 12015 12019)
        e=$(query 13 225 $conns 12020 12024)

        echo $conns $a $b $c $d $e

        kill -TERM $wrk
done

