#!/bin/bash
# usage: tools/rsx/run_nv3089_census.sh <build-dir> <run-name> [gate-open-s] [gate-close-s]
# One scripted run (scripts/run_e2e.sh) with NV3089_GATE open from gate-open-s
# to gate-close-s of wall time (default 180-190 = tutorial gameplay), then
# tools/rsx/nv3089_census.py on its log.
cd "$(dirname "$0")/../.."
B=$1; name=$2; t0=${3:-180}; t1=${4:-190}
gate=$PWD/build-runs/$name.gate; rm -f "$gate"
( sleep "$t0"; touch "$gate"; sleep $((t1 - t0)); rm -f "$gate" ) &
NV3089_GATE=$gate scripts/run_e2e.sh "$B" "$name" "NV3089 census, gate ${t0}-${t1} s"
rm -f "$gate"
python3 tools/rsx/nv3089_census.py build-runs/$name.txt
