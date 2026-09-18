/*
 * CrossNook toolchain smoke test.
 *
 * Tiny static ARM EABI soft-float program; its ONLY job is to prove that
 * a binary produced by the pinned musl cross toolchain runs on the Nook
 * Simple Touch (Linux 2.6.29, ARMv7). Print + return 0.
 */
#include <stdio.h>

int main(void)
{
    printf("CrossNook toolchain OK\n");
    return 0;
}