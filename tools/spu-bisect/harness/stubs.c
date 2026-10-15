#include <stdio.h>
#define STUB(n) void n(void){ fprintf(stderr,"stub " #n " called\n"); }
STUB(spu_drain_call) STUB(spu_halt) STUB(spu_img_restore) STUB(spu_indirect_branch)
STUB(spu_ls_watch_slow) STUB(spu_rdch) STUB(spu_register_function) STUB(spu_stop) STUB(spu_wrch)
