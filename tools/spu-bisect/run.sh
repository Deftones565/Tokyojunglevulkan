#!/bin/bash
# usage: run.sh <name>  -- 240 s e2e run (32-press script to the tutorial) of $BISECT_BUILD with frame dumps; posts RUN start/done as $BISECT_AGENT
cd /opt/git/tokyoJungleRecomp
B=${BISECT_BUILD:-build-mel}; name=$1; log=build-runs/$name.txt; fd=build-runs/$name/frames; mkdir -p $fd
if [ "$(pgrep -x tokyojungle | wc -l)" -ge 2 ]; then echo "2 slots busy"; exit 2; fi
.agents/talk say ${BISECT_AGENT:?set BISECT_AGENT to your board name} "RUN start: $name (${B}, ${BISECT_DESC:-full e2e, 32-press, gameplay frames, 240 s})" >/dev/null
TJ_FRAME_DUMP=$PWD/$fd TJ_FRAME_DUMP_EVERY=${BISECT_EVERY:-30} TJ_FRAME_DUMP_SKIP=${BISECT_SKIP:-340} TJ_FRAME_DUMP_MAX=${BISECT_MAX:-80} PS3_VERBOSE=0 TJ_HEADLESS=1 SDL_AUDIODRIVER=dummy TJ_RUN_DIR=$PWD/${B}/vfs \
  PAD_SCRIPT="30:0x4000,36:0x4000,42:0x4000,48:0x4000,54:0x4000,60:0x4000,66:0x4000,72:0x4000,100:0x4000,105:0x4000,110:0x4000,115:0x4000,120:0x4000,125:0x4000,130:0x4000,135:0x4000,140:0x4000,145:0x4000,150:0x4000,155:0x4000,160:0x4000,165:0x4000,170:0x4000,175:0x4000,180:0x4000,185:0x4000,190:0x4000,195:0x4000,200:0x4000,205:0x4000,210:0x4000,215:0x4000" \
  ${BISECT_PIN:+taskset -c $BISECT_PIN} ./${B}/tokyojungle input/EBOOT.ELF > $log 2>&1 &
pid=$!
for i in $(seq 1 240); do sleep 1; kill -0 $pid 2>/dev/null || break; done
kill -KILL $pid 2>/dev/null; wait $pid 2>/dev/null
echo "$name: ran ${i}s; effect.efd: $(grep -c effect.efd $log); FATAL: $(grep -c FATAL $log); frames: $(ls $fd | wc -l)"
.agents/talk say ${BISECT_AGENT:?set BISECT_AGENT to your board name} "RUN done: $name" >/dev/null
