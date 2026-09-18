#include <stdio.h>

extern char __libc_version[];

int main(void)
{
    printf("musl libc version: %s\n", __libc_version);
    return 0;
}