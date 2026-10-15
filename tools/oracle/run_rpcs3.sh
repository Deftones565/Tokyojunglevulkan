#!/usr/bin/env bash
# Boot Tokyo Jungle in RPCS3 headless for a fixed time and keep its logs, as a
# reference to diff the recomp against. See tools/oracle/README.md.
#
#   tools/oracle/run_rpcs3.sh [-s seconds] [-o outdir] [-w] [EBOOT] [-- rpcs3 args...]
#
#   -s N    run for N seconds, then stop RPCS3 (default 60)
#   -o DIR  where to put the logs (default build-oracle/<timestamp>, gitignored)
#   -w      open a window instead of running headless (to watch it)
#   -p SCR  press buttons with a virtual gamepad (tools/oracle/virtual_pad.py), e.g.
#           -p '40:0x0008,45:0x4000'; same masks as the recomp's PAD_SCRIPT. Uses the
#           RPCS3 input profile TJOracle (input_configs/global/TJOracle.yml)
#   EBOOT   default: the title as setup_game.sh installed it in RPCS3's dev_hdd0
#
# Audio always goes to RPCS3's Null renderer (a per-run --config copy), so a
# reference run is silent.
#
# Writes into the output directory:
#   TTY.log    the guest's own sys_tty_write output
#   RPCS3.log  the emulator log: HLE calls, syscalls, file opens, errors
#   run.txt    the command line, the exit status and the RPCS3 version
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
app=net.rpcs3.RPCS3
cfg="$HOME/.var/app/$app/config/rpcs3"
logs="$HOME/.var/app/$app/cache/rpcs3"
title=NPUA80523
content_id=UP9000-NPUA80523_00-TOKYOJUNGLE00001

secs=60 out="" windowed=0 pad=""
while getopts "s:o:wp:h" opt; do
	case $opt in
	s) secs=$OPTARG ;;
	o) out=$OPTARG ;;
	w) windowed=1 ;;
	p) pad=$OPTARG ;;
	*) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
	esac
done
shift $((OPTIND - 1))
eboot="$cfg/dev_hdd0/game/$title/USRDIR/EBOOT.BIN"
if [[ $# -gt 0 && $1 != -- ]]; then eboot="$1"; shift; fi
[[ ${1:-} == -- ]] && shift

[[ -f $eboot ]] || { echo "No EBOOT at $eboot. Run tools/extract/setup_game.sh first." >&2; exit 1; }
eboot="$(realpath "$eboot")"
[[ -f $cfg/dev_flash/vsh/etc/version.txt ]] || {
	echo "RPCS3 has no firmware installed. See tools/oracle/README.md." >&2; exit 1; }

# An encrypted EBOOT.BIN (SELF) needs the licence in exdata; a decrypted ELF does not.
if [[ $(head -c 3 "$eboot") == SCE && ! -f $cfg/dev_hdd0/home/00000001/exdata/$content_id.rap ]]; then
	echo "warning: $eboot is encrypted and there is no $content_id.rap in RPCS3's exdata;" >&2
	echo "         RPCS3 will refuse to boot it." >&2
fi

out="${out:-$root/build-oracle/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"   # absolute: RPCS3 runs in a flatpak with its own cwd

# The flatpak only sees its own data and what --filesystem grants. Grant the
# EBOOT's directory read-only when it lives outside RPCS3's tree.
fs=()
[[ $eboot == "$cfg"/* ]] || fs+=(--filesystem="$(dirname "$eboot")":ro)
# Per-run settings (a --config copy with null audio, input profiles) live under
# build-oracle/, which is gitignored; the sandbox has to be able to read them.
mkdir -p "$root/build-oracle"
fs+=(--filesystem="$root/build-oracle":ro)

if ((windowed)); then mode=(--no-gui); env=()
else mode=(--headless); env=(QT_QPA_PLATFORM=offscreen); fi

# RPCS3 truncates RPCS3.log and TTY.log when it starts, so the copies taken
# afterwards hold only this run.
# Silent: a copy of the user's config with the audio renderer set to Null. (Log
# levels can't be raised this way: RPCS3 ignores the config's Log section, at
# least in --no-gui/--headless mode, so per-frame media calls stay unlogged.)
sed 's/^  Renderer: Cubeb$/  Renderer: "Null"/' "$cfg/config.yml" >"$out/config-silent.yml"
extra=(--config "$out/config-silent.yml")

padpid=""
if [[ -n $pad ]]; then
	[[ -f $cfg/input_configs/global/TJOracle.yml ]] || {
		echo "Missing $cfg/input_configs/global/TJOracle.yml (see tools/oracle/README.md)." >&2; exit 1; }
	# The pad has to exist before RPCS3 starts; it removes itself after the run.
	python3 "$root/tools/oracle/virtual_pad.py" --script "$pad" --seconds "$((secs + 20))" \
		>"$out/pad.txt" 2>&1 &
	padpid=$!
	sleep 1
	extra+=(--input-config TJOracle)
	# RPCS3 only accepts --input-config with --no-gui, not --headless. Keep it
	# invisible anyway: Qt draws offscreen and the video renderer is Null.
	if ((!windowed)); then
		mode=(--no-gui); env=(QT_QPA_PLATFORM=offscreen)
		sed -i 's/^  Renderer: Vulkan$/  Renderer: "Null"/' "$out/config-silent.yml"
	fi
fi

cmd=(flatpak run "${fs[@]}" "$app" "${mode[@]}" "${extra[@]}" "$@" "$eboot")
echo "running for ${secs}s: ${cmd[*]}"
set +e
env "${env[@]}" timeout --signal=INT --kill-after=15 "$secs" "${cmd[@]}" >"$out/stdout.txt" 2>&1
status=$?
set -e
[[ -n $padpid ]] && { kill "$padpid" 2>/dev/null; wait "$padpid" 2>/dev/null || true; }

cp "$logs/TTY.log" "$out/TTY.log" 2>/dev/null || : >"$out/TTY.log"
cp "$logs/RPCS3.log" "$out/RPCS3.log" 2>/dev/null || : >"$out/RPCS3.log"
{
	echo "command: ${cmd[*]}"
	echo "seconds: $secs"
	case $status in
	124) echo "exit: stopped by the ${secs}s timer (expected)" ;;
	137) echo "exit: killed after ignoring SIGINT for 15s" ;;
	*)   echo "exit: $status (RPCS3 ended on its own before the timer)" ;;
	esac
	grep -m1 -oE 'RPCS3 v[^ ]+[^|]*' "$out/RPCS3.log" || true
	# Why it stopped, when RPCS3 says so (e.g. "Failed to decrypt content").
	grep -m1 '^Reason:' "$out/stdout.txt" || true
} >"$out/run.txt"

cat "$out/run.txt"
# RPCS3 tags each log line with its level: ·F fatal, ·E error, ·W warning.
echo "TTY lines: $(wc -l <"$out/TTY.log")   log errors: $(grep -cE '^·[EF] ' "$out/RPCS3.log" || true)"
echo "-> $out"
