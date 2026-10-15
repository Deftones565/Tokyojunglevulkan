#!/bin/bash
# usage: tools/play/watch.sh <run-name> <pid> [from-line]
# Blocks until the first sign of trouble in a play session, prints it and exits:
#   a new log line matching FATAL / [LIFT-GAP] / UNIMPLEMENTED / UNRESOLVED /
#   unresolved / stuck, the flip counter not moving across two frame dumps
#   (hang), or the process ending. Run it again with the printed from-line.
cd /opt/git/tokyoJungleRecomp
name=$1; pid=$2; from=${3:-1}; log=build-runs/$name.txt; fd=build-runs/$name/frames
pat='FATAL|\[LIFT-GAP\]|UNIMPLEMENTED|UNRESOLVED|unresolved|stuck|Segmentation|Aborted'
lastflip=; same=0
while true; do
    n=$(wc -l < $log)
    if [ "$n" -ge "$from" ]; then
        hit=$(sed -n "${from},${n}p" $log | grep -nE "$pat" | grep -vE "UNRESOLVED NID 0x(45FE2FCE|BB68D76E)" | head -3)
        if [ -n "$hit" ]; then echo "EVENT log (lines from $from): $hit"; echo "next-from: $((n+1))"; exit 0; fi
        from=$((n+1))
    fi
    if ! kill -0 $pid 2>/dev/null; then echo "EVENT process $pid ended"; tail -5 $log; exit 0; fi
    f=$(ls $fd 2>/dev/null | tail -1); flip=${f##*_flip}; flip=${flip%.ppm}
    if [ -n "$f" ] && [ "$flip" = "$lastflip" ] && [ "$f" != "$lastf" ]; then
        same=$((same+1))
        if [ $same -ge 2 ]; then echo "EVENT hang? flip stuck at $flip across 3 dumps ($f)"; echo "next-from: $from"; exit 0; fi
    elif [ "$f" != "$lastf" ]; then same=0; fi
    lastflip=$flip; lastf=$f
    sleep 3
done
