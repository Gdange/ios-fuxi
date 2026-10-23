extern int fuxi_gpu_scanout_owned(void);
void fuxi_display_kick(void)
{
    /* GPU-SCANOUT owns manual START until reset; console must stay muted. */
    if (fuxi_gpu_scanout_owned()) return;
    if (fuxi_intf1 == NULL || (fuxi_intf1[0x2b4 / 4] & 0x80000000u)) {
        __asm__ volatile("dsb sy" ::: "memory");
        return;
    }
    if (fuxi_ctl0 != NULL && !(fuxi_ctl0[0x1c / 4] & 1)) {
        __asm__ volatile("dsb sy" ::: "memory");
        fuxi_ctl0[0x1c / 4] = 1;
        __asm__ volatile("dsb sy" ::: "memory");
    }
}
