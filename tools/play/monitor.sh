#!/bin/bash
# usage: tools/play/monitor.sh <run-name>
# Once a second while the game runs: elapsed time, game frames in that second
# (from the TJ_PERF_FILE telemetry, build-runs/<run>/flips.txt), the 30/60
# governor mode (last [tj-flipgov] line), memory, thread count and the three
# busiest threads by CPU. Written to build-runs/<run>/monitor.txt, so a stall
# the player reports can be matched to what the game was doing at the time.
cd /opt/git/tokyoJungleRecomp
run=$1; d=build-runs/$run; out=$d/monitor.txt
while [ -z "$(pgrep -x tokyojungle)" ]; do sleep 1; done
P=$(pgrep -x tokyojungle | head -1)
hz=$(getconf CLK_TCK)
declare -A prev
last_flips=0
echo "# time  fps  gov  rss_MB  threads  busiest(tid:%cpu)" > $out
while kill -0 $P 2>/dev/null; do
    sleep 1
    flips=$(tail -1 $d/flips.txt 2>/dev/null | awk '{print $2}'); flips=${flips:-0}
    fps=$((flips - last_flips)); last_flips=$flips
    gov=$(grep "tj-flipgov" $d.txt 2>/dev/null | tail -1 | awk '{print $2}')
    rss=$(( $(awk '/VmRSS/{print $2}' /proc/$P/status 2>/dev/null || echo 0) / 1024 ))
    busy=""
    for t in /proc/$P/task/*; do
        id=${t##*/}; ticks=$(awk '{print $14+$15}' $t/stat 2>/dev/null) || continue
        busy+="$((ticks - ${prev[$id]:-$ticks})) $id"$'\n'; prev[$id]=$ticks
    done
    top=$(echo "$busy" | sort -rn | head -3 | awk -v hz=$hz 'NF{printf "%s:%d%% ", $2, $1*100/hz}')
    echo "$(date +%H:%M:%S) $fps ${gov:-?} $rss $(ls /proc/$P/task | wc -l) $top" >> $out
done
echo "# process ended $(date +%H:%M:%S)" >> $out
