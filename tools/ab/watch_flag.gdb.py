# gdb -x tools/ab/watch_flag.gdb.py: (diag) hardware-watch a guest word, log who writes it.
# env WATCH_EA=<hex guest address>, WATCH_HITS=<n>, WATCH_OUT=<file>
import gdb, os
ea = int(os.environ.get('WATCH_EA', '0'), 16)
hits_max = int(os.environ.get('WATCH_HITS', '200'))
out = open(os.environ.get('WATCH_OUT', '/dev/stderr'), 'w')
state = {'n': 0, 'armed': False}
gdb.execute('set pagination off')
gdb.execute('set confirm off')
gdb.execute('handle SIGSEGV SIGPIPE SIGUSR1 SIGUSR2 SIG34 SIG35 nostop noprint pass')
class Arm(gdb.Breakpoint):
    def stop(self):
        if not state['armed']:
            base = int(gdb.parse_and_eval('(unsigned long)vm_base'))
            W(base + ea)
            state['armed'] = True
            out.write('armed at host 0x%x\n' % (base + ea)); out.flush()
        return False
class W(gdb.Breakpoint):
    def __init__(self, addr):
        super().__init__('*(unsigned int*)0x%x' % addr, gdb.BP_WATCHPOINT, gdb.WP_WRITE, internal=True)
        self.addr = addr
    def stop(self):
        state['n'] += 1
        f = gdb.newest_frame(); names = []
        for _ in range(8):
            if f is None: break
            names.append(f.name() or '??'); f = f.older()
        th = gdb.selected_thread().ptid[1]
        raw = int(gdb.parse_and_eval('*(unsigned int*)0x%x' % self.addr))
        val = int.from_bytes(raw.to_bytes(4, 'little'), 'big')     # guest words are big-endian
        out.write('%d tid=%d val=0x%X %s\n' % (state['n'], th, val, ' < '.join(names))); out.flush()
        if state['n'] >= hits_max:
            gdb.execute('kill'); return True
        return False
Arm('__wrap__Z13func_001536ECP11ppu_context')
gdb.execute('run')
