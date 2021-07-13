#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void __rdpkru(void)
{
    int pkru, sanityCheck;

    printf("Reading PKRU..\n");

    asm("mov $0x0, %%ecx\n\t"
	    "RDPKRU\n\t"
        "mov %%eax, %0\n\t"
        "mov %%edx, %1\n\t"
        : "=gm" (pkru), "=gm" (sanityCheck));

    if(!sanityCheck)
        printf("PKRU is set to %d\n", pkru);
    else
        printf("EDX not set to 0 by RDPKRU\n");
}

void __resetpkru(int regVal)
{
    printf("Resetting PKRU with value %d..\n", regVal);

    asm("mov $0x0,  %eax\n\t"
        "mov $0x0,  %ecx\n\t"
        "mov $0x0,  %edx\n\t"
        "WRPKRU\n\t" 
        );

     printf("Finished resetting PKRU.\n");
}

void __wrpkru(int regVal)
{
    printf("Writing PKRU with value fffffffc\n");

    asm("mov $0xc,  %eax\n\t"
        "mov $0x0,  %ecx\n\t"
        "mov $0x0,  %edx\n\t"
        "WRPKRU\n\t" 
        );

     printf("Finished writing PKRU.\n");
}

void protectAddr(int untrusted, int size)
{
    int *map, pkey, pkru, dompret;
    void *virtAddr;

    printf("Entered pkeys alloc sys call\n");
    //virtAddr  = (void *)__get_free_page(1);
    /* virtAddr  = malloc(size); */

    virtAddr  = mmap(NULL, size, PROT_READ|PROT_WRITE,
               MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);

    map  = (int *) virtAddr;
    *map = 100;

    printf("Value written to address %p - %d\n", virtAddr, *map);

    pkey = syscall(330,0,0);
    printf("Pkey %d\n", pkey);

    //dompret = do_mprotect_pkey(virtAddr, , 3, pkey);
    dompret = syscall(329, virtAddr, size, 3, pkey);
    printf("mprotect returned %d\n", dompret);
    pkru = (int) 3 << 2*(pkey-1);

    __resetpkru(0); //PKRU_TRUSTED
    //__rdpkru();
    
    //Trusted Code
    printf("Entered trusted with value %d\n", *map);
    *map = 200;
    printf("Changed value to %d\n", *map);

    /* __wrpkru(0); //PKRU_UNTRUSTED */
    __rdpkru();

    //Untrusted Code
    printf("Successfully entered untrusted code\n");

    if(untrusted == 1)
        *map = 200;
    else if(untrusted == 2)
        printf("Entered untrusted with value %d\n", *map);

    printf("Finished pkeys alloc sys call with exit\n" );
}

int main(void)
{
    protectAddr(1, 4096);
    protectAddr(2, 4096);
    __resetpkru(0);
    return(0);
}
