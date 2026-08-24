#!/bin/bash
# 高并发 HTTP 延迟测试（curl 并发，真实客户端行为）
# 用法: ./bench_curl_latency.sh <socks端口> <名称> <并发> <请求数>
set -u
PORT=$1; NAME=$2; CONN=$3; N=$4
TMP=$(mktemp -d)
for i in $(seq 1 $CONN); do
  for j in $(seq 1 $((N / CONN))); do
    (timeout 10 curl -s -o /dev/null -w '%{time_total}\n' -x socks5h://127.0.0.1:$PORT http://10.206.0.4/ >> $TMP/lat_$i.txt) &
  done
done
wait
cat $TMP/lat_*.txt | sort -n > $TMP/all.txt
TOTAL=$(wc -l < $TMP/all.txt)
if [ "$TOTAL" -eq 0 ]; then echo "$NAME: 0 requests"; rm -rf $TMP; exit 1; fi
awk -v n="$TOTAL" -v name="$NAME" '
BEGIN{sum=0}
{ v[NR]=$1*1000; sum+=$1*1000 }
END{
  printf "%s: total=%d req avg=%.2fms p50=%.2f p90=%.2f p95=%.2f p99=%.2f max=%.2f\n",
    name, n, sum/n, v[int(n*0.50)], v[int(n*0.90)], v[int(n*0.95)], v[int(n*0.99)], v[n]
}' $TMP/all.txt
rm -rf $TMP
