#!/usr/bin/env python3
"""Closed-loop gameplay perf run: boot -> Survival tutorial street, then measure.

usage: POSTER=<MAGI name> drive_gameplay.py <binary> <run-name> [--measure 60]
         [--settle 15] [--no-prof] [--keep-frames] [VAR=value ...]

A fixed wall-clock PAD_SCRIPT misses gameplay whenever the machine is loaded
(mg5-census sat on the intro movie for the whole run and read as 37 "flips/s").
This drives the game with PAD_FILE instead and looks at what is on screen:

  1. until the Survival map starts loading (effect.efd in the log): CROSS and
     START alternately every --press-every s (logos, title "press START", main menu with
     SURVIVAL highlighted);
  2. then CROSS every --press-every s (intro movie, ~13 tutorial prompts); START
     once if the HUD has not shown --skip-after s later (skips the movie; if it opened the
     tutorial's pause dialog instead, a CROSS once the HUD shows picks No);
  3. once the HUD shows (LIFE bar blue, STAMINA bar red at their screen
     positions): no more input. After --settle s the window opens: the
     wall-clock profile gate (tools/prof/sigprof.c PROF_WALL_HZ=200,
     PROF_GATE) and the flips/s count, for --measure s. Then the run ends.

The Pomeranian stands on the tutorial street with wolves walking about: the
same scene the 11-press script measured, reached reliably.

Output: build-runs/<run>.txt (log), build-runs/<run>/prof.bin (+ the binary that ran,
build-runs/<run>/tokyojungle, to symbolise it with), and
build-runs/<run>/result.txt (flips/s in the window, waits, FATAL count). Frame
dumps are deleted at the end except the first HUD frame and the last frame,
unless --keep-frames. Extra VAR=value arguments go into the game's environment.
"""
import argparse, os, re, shutil, signal, subprocess, sys, time

R = '/opt/git/tokyoJungleRecomp'
CROSS, START = '0x4000', '0x0008'


def say(msg):
    if os.environ.get('POSTER'):
        subprocess.run([R + '/.agents/talk', 'say', os.environ['POSTER'], msg], stdout=subprocess.DEVNULL)
    else:
        print(msg, flush=True)


def frames(d):
    out = []
    for f in os.listdir(d):
        m = re.match(r'frame_\d+_flip(\d+)\.ppm$', f)
        if m: out.append((int(m.group(1)), f))
    return sorted(out)


def is_hud(path):
    import numpy as np
    from PIL import Image
    try:
        a = np.asarray(Image.open(path).convert('RGB')).astype(int)
    except Exception:
        return False                       # still being written
    if a.shape[0] != 720 or a.shape[1] != 1280: return False
    life = a[78:92, 190:400].reshape(-1, 3).mean(0)      # blue bar
    stam = a[112:124, 190:400].reshape(-1, 3).mean(0)    # red bar
    return life[2] - life[0] > 35 and stam[0] - stam[1] > 35


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('binary'); ap.add_argument('name')
    ap.add_argument('--measure', type=float, default=60)
    ap.add_argument('--settle', type=float, default=15)
    ap.add_argument('--timeout', type=float, default=420, help='give up if no HUD by then')
    # Input pacing to reach gameplay. Was a 20 s start, a press every 3 s and
    # the movie skip 60 s after loading began: ~3 minutes to the HUD.
    ap.add_argument('--first-press', type=float, default=5, help='s before the first press')
    ap.add_argument('--press-every', type=float, default=1.0, help='s between presses')
    ap.add_argument('--skip-after', type=float, default=5,
                    help='s after the map starts loading before START skips the movie')
    ap.add_argument('--no-prof', action='store_true')
    ap.add_argument('--clean-timing', action='store_true',
                    help='use native flip telemetry; pause dumps and disable sampling during timing')
    ap.add_argument('--keep-frames', action='store_true')
    ap.add_argument('env', nargs='*')
    a = ap.parse_args()
    if a.clean_timing:
        a.no_prof = True
    # SLOTS=<n> (default 2): how many game instances may already be running.
    slots = int(os.environ.get('SLOTS', '2'))
    if int(subprocess.run('pgrep -x tokyojungle | wc -l', shell=True, capture_output=True, text=True).stdout) >= slots:
        sys.exit('%d slots busy' % slots)
    out = f'{R}/build-runs/{a.name}'; log = out + '.txt'
    shutil.rmtree(out, ignore_errors=True)
    fd, cwd, pad, gate = out + '/frames', out + '/cwd', out + '/pad.txt', out + '/gate'
    telemetry, dump_pause = out + '/flips.txt', out + '/pause-dumps'
    os.makedirs(fd); os.makedirs(cwd); open(pad, 'w').close()
    so = R + '/build-runs/sigprof.so'
    if not os.path.exists(so) or os.path.getmtime(so) < os.path.getmtime(R + '/tools/prof/sigprof.c'):
        subprocess.check_call(['cc', '-O2', '-shared', '-fPIC', '-o', so, R + '/tools/prof/sigprof.c', '-lpthread'])
    env = dict(os.environ, PS3_VERBOSE='0', TJ_HEADLESS='1', SDL_AUDIODRIVER='dummy', TJ_TICK_STATS='1',
               GCM_WAIT_STATS='1', TJ_RUN_DIR=out + '/vfs', PAD_FILE=pad, TJ_FRAME_DUMP=fd,
               TJ_FRAME_DUMP_EVERY='15', TJ_FRAME_DUMP_MAX='100000')
    if a.no_prof:
        env.pop('LD_PRELOAD', None)
        for k in list(env):
            if k.startswith('PROF_'): del env[k]
    if a.clean_timing:
        env.update(TJ_PERF_FILE=telemetry, TJ_FRAME_DUMP_PAUSE_FILE=dump_pause)
    if not a.no_prof:
        env.update(PROF_OUT=out + '/prof.bin', PROF_WALL_HZ='200', PROF_GATE=gate, LD_PRELOAD=so)
    for kv in a.env:
        k, v = kv.split('=', 1); env[k] = v
    if a.clean_timing:
        # Explicit VAR=value arguments must not silently undo clean timing.
        env.pop('LD_PRELOAD', None)
        for k in list(env):
            if k.startswith('PROF_'): del env[k]
        env.update(TJ_PERF_FILE=telemetry, TJ_FRAME_DUMP_PAUSE_FILE=dump_pause)
    # The profile is symbolised against the binary: keep the exact one that ran
    # (a rebuilt build dir would otherwise give wrong names).
    shutil.copy2(os.path.realpath(a.binary), out + '/tokyojungle')
    say(f'RUN start: {a.name} ({a.binary}, closed-loop gameplay perf, scratch CWD) {" ".join(a.env)}')
    logf = open(log, 'wb')
    p = subprocess.Popen([os.path.realpath(a.binary), R + '/gamefiles/games/NPUA80523/USRDIR/EBOOT.ELF'], cwd=cwd, env=env,
                         stdout=logf, stderr=subprocess.STDOUT)
    t0 = time.time(); last_press = 0; n_press = 0; loading_t = None; hud_t = None; hud_frame = None
    started_skip = False; result = 'no HUD'
    clean_started = None; clean_first = None

    def latest_flip():
        try:
            data = open(telemetry).read()
            lines = data[:data.rfind('\n')].splitlines()
            t, f = lines[-1].split()
            return float(t), int(f)
        except (OSError, ValueError, IndexError):
            return None

    def press(m):
        nonlocal last_press, n_press
        # Held 8 polls (~0.13 s), so presses --press-every apart stay separate.
        with open(pad, 'w') as f: f.write(m + ' 8\n')
        last_press = time.time(); n_press += 1

    try:
        while p.poll() is None:
            time.sleep(0.5)
            now = time.time()
            if now - t0 > a.timeout and hud_t is None: break
            if loading_t is None:
                with open(log, 'rb') as f:
                    if b'effect.efd' in f.read(): loading_t = now
            fs = frames(fd)
            if hud_t is None:
                if fs and len(fs) > 1 and is_hud(os.path.join(fd, fs[-2][1])):
                    hud_t = now; hud_frame = fs[-2]
                    print(f'HUD at t={now - t0:.0f}s flip {hud_frame[0]}', flush=True)
                    # A START that skipped the movie may instead have opened the
                    # tutorial's pause dialog ("Exit the tutorial?", No
                    # highlighted) over the HUD (mg5-post77 measured a paused
                    # game). CROSS picks No; in plain gameplay it is harmless.
                    if started_skip:
                        time.sleep(1.0); press(CROSS)
                    continue
                if now - t0 < a.first_press or now - last_press < a.press_every: continue
                if loading_t is None:
                    press(CROSS if n_press % 2 == 0 else START)
                elif not started_skip and now - loading_t > a.skip_after:
                    press(START); started_skip = True
                else:
                    press(CROSS)
                continue
            if now - hud_t < a.settle: continue
            if a.clean_timing:
                if now - hud_t > a.settle + a.measure + 30:
                    raise RuntimeError('clean measurement stalled after reaching gameplay')
                if clean_started is None:
                    open(dump_pause, 'w').close()
                    clean_started = time.monotonic()
                sample = latest_flip()
                # Allow any in-progress screenshot to finish before timing.
                if sample is None or sample[0] < clean_started + 2:
                    if time.monotonic() - clean_started > 10:
                        raise RuntimeError('native flip telemetry missing: use a build with TJ_PERF_FILE support')
                    continue
                if clean_first is None:
                    clean_first = sample
                    clean_frame_count = len(fs)
                if sample[0] - clean_first[0] >= a.measure:
                    if len(fs) != clean_frame_count:
                        raise RuntimeError('screenshots were dumped during the clean timing window')
                    dt = sample[0] - clean_first[0]
                    result = f'{(sample[1] - clean_first[1]) / dt:.2f} flips/s over flips {clean_first[1]}..{sample[1]} ({dt:.3f} s; native telemetry, no dumps/profiler)'
                    os.unlink(dump_pause)
                    # Capture a post-window validation frame, outside timing.
                    time.sleep(2)
                    break
                continue
            if not os.path.exists(gate):
                open(gate, 'w').close(); w0 = (now, fs[-1][0] if fs else 0)
            if now - w0[0] >= a.measure:
                os.unlink(gate); w1 = (now, fs[-1][0] if fs else 0)
                result = f'{(w1[1] - w0[1]) / (w1[0] - w0[0]):.2f} flips/s over flips {w0[1]}..{w1[1]} ({w1[0] - w0[0]:.0f} s)'
                break
    finally:
        if p.poll() is None:
            p.send_signal(signal.SIGKILL); p.wait()
        logf.close()
    txt = open(log, errors='replace').read()
    waits = re.findall(r'\[gcm-wait\].*', txt)
    res = [f'{a.name}: {result}', f'HUD at flip {hud_frame[0] if hud_frame else "-"}',
           f'FATAL {txt.count("FATAL")}, LIFT-GAP {txt.count("LIFT-GAP")}, RSX cap hits {txt.count("RSX still busy")}']
    res += waits[-2:]
    open(out + '/result.txt', 'w').write('\n'.join(res) + '\n')
    print('\n'.join(res))
    if not a.keep_frames:
        keep = {hud_frame[1] if hud_frame else None}
        fs = frames(fd)
        if fs: keep.add(fs[-1][1])
        for _, f in fs:
            if f not in keep: os.unlink(os.path.join(fd, f))
    shutil.rmtree(cwd, ignore_errors=True); shutil.rmtree(out + '/vfs', ignore_errors=True)
    say(f'RUN done: {a.name}: {result}')


if __name__ == '__main__':
    main()
