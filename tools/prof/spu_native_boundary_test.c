/* Reuse only native control-flow cases from the SDK suite. Its original main
 * also tests interpreter behavior, which this recomp-only port forbids. */
#define main sdk_suite_main
#include "test_spu_lifted_start.c"
#undef main
int main(void)
{
    test_guest_stack_reset();
    test_taskset_resume_stack();
    test_alternate_link_return();
    test_nonzero_link_return();
    printf("Native SPU boundaries: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
