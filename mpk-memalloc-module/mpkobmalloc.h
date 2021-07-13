#include "Python.h"
#include "hashmap.h"
#include <stdbool.h>
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/time.h>

#define WITH_PYMALLOC 1
#define HAVE_MMAP 1

#define SYSTEM_PAGE_SIZE        (4 * 1024)
#define SYSTEM_PAGE_SIZE_MASK   (SYSTEM_PAGE_SIZE - 1)

/* Defined in tracemalloc.c */
//extern void _PyMeme_DumpTraceback(int fd, const void *ptr);


/* Python's malloc wrappers (see pymem.h) */

#undef  uint
#define uint    unsigned int    /* assuming >= 16 bits */

/* Forward declaration */
/* static void* _PyMeme_DebugRawMalloc(void *ctx, size_t size); */
/* static void* _PyMeme_DebugRawCalloc(void *ctx, size_t nelem, size_t elsize); */
/* static void* _PyMeme_DebugRawRealloc(void *ctx, void *ptr, size_t size); */
/* static void _PyMeme_DebugRawFree(void *ctx, void *ptr); */

/* static void* _PyMeme_DebugMalloc(void *ctx, size_t size); */
/* static void* _PyMeme_DebugCalloc(void *ctx, size_t nelem, size_t elsize); */
/* static void* _PyMeme_DebugRealloc(void *ctx, void *ptr, size_t size); */
/* static void _PyMeme_DebugFree(void *ctx, void *p); */

/* static void _PyObject_DebugDumpAddress(const void *p); */
/* static void _PyMeme_DebugCheckAddress(char api_id, const void *p); */

/* static void _PyMeme_SetupDebugHooksDomain(PyMemAllocatorDomain domain); */

void init_usedpools();
/* void pymem_pkey_set(int key); */
void init_arena_components();

/* static void pkey_pool_protect(uintptr_t pool, int pkey); */
static void pkey_pool_protect(int *pool, int key);
static void pkey_raw_protect(int *pool, int size);
int get_thread_pkey(bool checkForSharing);

#if defined(__has_feature)  /* Clang */
#  if __has_feature(address_sanitizer) /* is ASAN enabled? */
#    define _Py_NO_ADDRESS_SAFETY_ANALYSIS \
        __attribute__((no_address_safety_analysis))
#  endif
#  if __has_feature(thread_sanitizer)  /* is TSAN enabled? */
#    define _Py_NO_SANITIZE_THREAD __attribute__((no_sanitize_thread))
#  endif
#  if __has_feature(memory_sanitizer)  /* is MSAN enabled? */
#    define _Py_NO_SANITIZE_MEMORY __attribute__((no_sanitize_memory))
#  endif
#elif defined(__GNUC__)
#  if defined(__SANITIZE_ADDRESS__)    /* GCC 4.8+, is ASAN enabled? */
#    define _Py_NO_ADDRESS_SAFETY_ANALYSIS \
        __attribute__((no_address_safety_analysis))
#  endif
   // TSAN is supported since GCC 5.1, but __SANITIZE_THREAD__ macro
   // is provided only since GCC 7.
#  if __GNUC__ > 5 || (__GNUC__ == 5 && __GNUC_MINOR__ >= 1)
#    define _Py_NO_SANITIZE_THREAD __attribute__((no_sanitize_thread))
#  endif
#endif

#ifndef _Py_NO_ADDRESS_SAFETY_ANALYSIS
#  define _Py_NO_ADDRESS_SAFETY_ANALYSIS
#endif
#ifndef _Py_NO_SANITIZE_THREAD
#  define _Py_NO_SANITIZE_THREAD
#endif
#ifndef _Py_NO_SANITIZE_MEMORY
#  define _Py_NO_SANITIZE_MEMORY
#endif

#ifdef WITH_PYMALLOC

#ifdef MS_WINDOWS
#  include <windows.h>
#elif defined(HAVE_MMAP)
#  include <sys/mman.h>
#  ifdef MAP_ANONYMOUS
#    define ARENAS_USE_MMAP
#  endif
#endif

#ifndef DEBUG_VAR
#define DEBUG_VAR 1  
int DEBUG = 0;
#endif

#ifndef MPK_ENABLED
#define MPK_ENABLED 1
#endif

#define PYMEM_CLEANBYTE      0xCD
#define PYMEM_DEADBYTE       0xDD
#define PYMEM_FORBIDDENBYTE  0xFD

#define SYS_pkey_mprotect 329
#define SYS_pkey_alloc 330
#define SYS_pkey_free 331

#define ENABLE_READ  0x1
#define ENABLE_WRITE 0x2
#define ENABLE_BOTH  (ENABLE_READ | ENABLE_WRITE)
#define PKRU_MASK    0xFFFFFFFF 

/* Forward declaration */
static void* _PyObject_Malloc(void *ctx, size_t size);
static void* _PyObject_Calloc(void *ctx, size_t nelem, size_t elsize);
static void _PyObject_Free(void *ctx, void *p);
static void* _PyObject_Realloc(void *ctx, void *ptr, size_t size);
#endif

int tssKey;
bool privateOrSharedDomain;
//memory locations to hold TSS values for each pkey
int tssValue[16] = {0};
/* int pkey=0; */
bool collectStats = 0;
PyMemAllocatorEx default_raw_alloc, default_mem_alloc;
hashtable_t *allocated_ptrs;

/* buffer env to recover from a faulty realloc */
jmp_buf _PyMeme_RawRealloc_env;

struct mmap_node{
    struct mmap_node *prev;
    struct mmap_node *next;
    void *ptr;
    size_t size;
    int pkey;
};

struct stats{
    uint64_t numRequests;
    uint64_t hashMapCalls;
    size_t servedSize;
    double timeSpent;
    double hashTime;
};

struct stats mallocStats  = {.numRequests = 0, .servedSize = 0, .timeSpent = 0};
struct stats callocStats  = {.numRequests = 0, .servedSize = 0, .timeSpent = 0};
struct stats reallocStats = {.numRequests = 0, .servedSize = 0, .timeSpent = 0};
struct stats freeStats    = {.numRequests = 0, .servedSize = 0, .timeSpent = 0};

void printStats(struct stats a, char *name)
{
    printf("|%s\t|%" PRIu64 "\t|%zu\t|%f\n",
            name, a.numRequests, a.servedSize, a.timeSpent);
}

void printAllStats(){
    printf("|Name\t|NumRequests\t|ServedSize\t|TimeSpent\n");
    printStats(mallocStats, "Malloc");
    printStats(callocStats, "Calloc");
    printStats(reallocStats, "Realloc");
    printStats(freeStats, "Free");
}

struct mmap_node *mmap_list_head;

size_t minimum(size_t a, size_t b){
    size_t m = (a<b) ? a: b;
    return m;
}

void traverse_mmap_list()
{
    struct mmap_node *node;
    node = mmap_list_head;
    if(DEBUG) printf("[Mmap List]");
    while(node != NULL)
    {
        if(DEBUG) printf("(%x,%d, %d) -> ", node->ptr, node->size, node->pkey);
        node = node->next;
    }
    if(DEBUG) printf("\n");

}
void insert_mmap_list(void *ptr, size_t size)
{
    int pkey = get_thread_pkey(1);
    struct mmap_node *node;

    node = (struct mmap_node *)malloc(sizeof(struct mmap_node));
    node->ptr  = ptr;
    node->size = size;
    node->pkey = pkey;
    node->prev = NULL;//inserting at the beginning of the list

    if(mmap_list_head == NULL)
        node->next = NULL;
    else{
        node->next = mmap_list_head;
        mmap_list_head->prev = node;
    }

    mmap_list_head = node;
    traverse_mmap_list();
}

static inline void
wrpkru(unsigned int pkru)
{
    unsigned int eax = pkru;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    asm volatile(".byte 0x0f,0x01,0xef\n\t"
                 : : "a" (eax), "c" (ecx), "d" (edx));
}

int
pkru_set(int pkey, unsigned long rights)
{
    unsigned int pkru = 0;
    unsigned int all_rights = 0;

    //Always give access to default pkey
    all_rights |= (ENABLE_BOTH);

    if(pkey == -1){
        //Hard reset PKRU, should be called only from memory manager
        for(int i=0; i<16;i++)
            all_rights |= (rights) << (2*i);
    } else {
        //Give access to current thread's pkey 
        all_rights |= rights << (2*pkey);
    }

    pkru = (PKRU_MASK ^ all_rights);
    wrpkru(pkru);
}

int
allocate_pkey()
{
    static int pkey=0;
    if(MPK_ENABLED)
        return syscall(SYS_pkey_alloc, 0, 0);
    else{
        pkey += 1;
        return pkey;
    }
}

static void 
pkey_raw_protect(int *pool, int size)
{
    int pkey = get_thread_pkey(1);
    int out = syscall(329, pool, size, 3, pkey);
}

int round_page_size(int size)
{
    return 4096*((size/4096) + 1);
}

double diff(struct timespec start, struct timespec end)
{
    struct timespec temp;
    double t = 0;

    if ((end.tv_nsec-start.tv_nsec)<0) {
        temp.tv_sec = end.tv_sec-start.tv_sec-1;
        temp.tv_nsec = 1000000000+end.tv_nsec-start.tv_nsec;
    } else {
        temp.tv_sec = end.tv_sec-start.tv_sec;
        temp.tv_nsec = end.tv_nsec-start.tv_nsec;
    }

    t = 1000.0*(double)temp.tv_sec;
    t += (double)temp.tv_nsec/(double)1000000;

    return t;
}

static void *
_PyMeme_RawMalloc(void *ctx, size_t size)
{
    struct timespec start, end;
    double t;
    clock_gettime(CLOCK_REALTIME, &start);

    if (!size) size = 1;

    void *ptr;
    //Round off to nearest page size
    int rawSize = round_page_size(size);

    ptr = mmap(NULL, rawSize, PROT_READ|PROT_WRITE,
               MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);

    if (ptr == MAP_FAILED)
        return NULL;
    assert(ptr != NULL);

    if(MPK_ENABLED) pkey_raw_protect(ptr, rawSize);

    char sptr[12];
    sprintf(sptr, "%x", ptr);
    if(!ht_get(allocated_ptrs, (uint64_t)ptr)) ht_set(allocated_ptrs, (uint64_t) ptr, rawSize);
    clock_gettime(CLOCK_REALTIME, &end);

    //Collect Stats
    if(collectStats)
    {
        t = diff(start, end);
        mallocStats.numRequests++;
        mallocStats.servedSize += rawSize;
        mallocStats.timeSpent += t;
    }

    if(DEBUG) printf("[RawMmap] Ptr: %p, size: %d, pkey: %d, Elapsed time: %f\n", ptr, size, get_thread_pkey(1), t);

    return ptr;
}

static void *
_PyMeme_RawCalloc(void *ctx, size_t nelem, size_t elsize)
{
    struct timespec start, end;
    double t;
    clock_gettime(CLOCK_REALTIME, &start);
    /* PyMem_RawCalloc(0, 0) means calloc(1, 1). Some systems would return NULL
       for calloc(0, 0), which would be treated as an error. Some platforms
       would return a pointer with no memory behind it, which would break
       pymalloc.  To solve these problems, allocate an extra byte. */
    if (nelem == 0 || elsize == 0) {
        nelem = 1;
        elsize = 1;
    }

    void *ptr;
    //Round off to nearest page size
    int rawSize = round_page_size(nelem*elsize);

    ptr = mmap(NULL, rawSize, PROT_READ|PROT_WRITE,
               MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED)
        return NULL;
    assert(ptr != NULL);

    if(MPK_ENABLED) pkey_raw_protect(ptr, rawSize);

    if(!ht_get(allocated_ptrs, (uint64_t)ptr)) ht_set(allocated_ptrs, (uint64_t)ptr, rawSize);

    clock_gettime(CLOCK_REALTIME, &end);

    if(collectStats)
    {
        t = diff(start, end);
        callocStats.numRequests++;
        callocStats.servedSize += rawSize;
        callocStats.timeSpent += t;
    }

    if(DEBUG) printf("[RawCalloc] Ptr: %p, size: %d, pkey: %d, Elapsed time: %f\n", ptr, nelem*elsize, get_thread_pkey(1), t);
    return ptr;
}

static void *
_PyMeme_RawRealloc(void *ctx, void *ptr, size_t size)
{
    struct timespec start, end;
    clock_gettime(CLOCK_REALTIME, &start);
    int copySize = -1;
    void *newPtr;
    size_t byteOffset;
    int rawSize = 0;

    //Malloc and realloc would be the same for the size 0
    if(!size) return _PyMeme_RawMalloc(ctx, size);

    //Remove sprintf, hashtable should store
    if(ptr != NULL)
    {
        copySize = ht_get(allocated_ptrs, (uint64_t)ptr);

        /* Check if overallocation in Rawmalloc is sufficient here*/
        if(size <= copySize)
        {
            newPtr = ptr;
            clock_gettime(CLOCK_REALTIME, &end);
            goto finally;
        }
    }

    //Round off to nearest page size
    rawSize = round_page_size(size);
    newPtr = mmap(NULL, rawSize, PROT_READ|PROT_WRITE,
               MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);


    if (newPtr == MAP_FAILED)
        return NULL;
    assert(newPtr != NULL);

    if(!copySize && ptr != NULL)
    {
        //Copy values to new memory locations in realloc!
        if(size <= 512)
        {
            /* Realloc gave a segfault, so ptr was allocated by arenas */
            /* Copy till the end of the pool or size, whichever is lower */
            if(DEBUG) printf("[RawRealloc] %p was arena allocated\n", ptr);
            byteOffset = (size_t)((uintptr_t)ptr & SYSTEM_PAGE_SIZE_MASK);
            copySize   = minimum(SYSTEM_PAGE_SIZE - byteOffset - 1, size);
        }
        else {
            /* Check if ptr was allocated by malloc */
            if(DEBUG) printf("[RawRealloc] Check if %p was malloc'ed\n", ptr);
            /* void *rePtr = realloc(ptr, size); */
            copySize = size;
            /* ptr = rePtr; */
        }
    }

    if(ptr != NULL)
        memcpy(newPtr, ptr, minimum(copySize, size));

    if(!ht_get(allocated_ptrs, (uint64_t)newPtr)) ht_set(allocated_ptrs, (uint64_t)newPtr, rawSize);

    if(MPK_ENABLED) pkey_raw_protect(newPtr, rawSize);

    if (DEBUG) printf("[RawRealloc] Finshed Ptr: %p, size: %d, pkey:%d\n", newPtr, size, get_thread_pkey(1));
    clock_gettime(CLOCK_REALTIME, &end);
    goto finally;

finally:
    if(collectStats){
        double t = diff(start, end);
        reallocStats.numRequests++;
        reallocStats.servedSize += rawSize;
        reallocStats.timeSpent += t;
    }
    return newPtr;
}

static void
_PyMeme_RawFree(void *ctx, void *ptr)
{
    struct timespec start, end;
    clock_gettime(CLOCK_REALTIME, &start);
    if(DEBUG) printf("[RawFree] Free requested for: %p\n",ptr);
    int allocatedSize = ht_get(allocated_ptrs, (uint64_t)ptr);
    if(!allocatedSize) munmap(ptr, allocatedSize);
    clock_gettime(CLOCK_REALTIME, &end);

    if(collectStats){
        double t = diff(start, end);
        freeStats.numRequests++;
        freeStats.timeSpent += t;
        freeStats.servedSize += allocatedSize;
        freeStats.hashMapCalls++;
    }
}

#ifdef MS_WINDOWS
static void *
_PyObject_ArenaVirtualAlloc(void *ctx, size_t size)
{
    return VirtualAlloc(NULL, size,
                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

static void
_PyObject_ArenaVirtualFree(void *ctx, void *ptr, size_t size)
{
    VirtualFree(ptr, 0, MEM_RELEASE);
}

#elif defined(ARENAS_USE_MMAP)
static void *
_PyObject_ArenaMmap(void *ctx, size_t size)
{
    void *ptr;
    ptr = mmap(NULL, size, PROT_READ|PROT_WRITE,
               MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED)
        return NULL;
    assert(ptr != NULL);

    if(DEBUG) printf("[ArenaMmap] Ptr: %p, size: %d\n", ptr, size);
    insert_mmap_list(ptr,size);
    return ptr;
}

static void
_PyObject_ArenaMunmap(void *ctx, void *ptr, size_t size)
{
    if(DEBUG) printf("[ArenaMunmap] Ptr: %x, size: %d\n", ptr, size);
    munmap(ptr, size);
}

#else
static void *
_PyObject_ArenaMalloc(void *ctx, size_t size)
{
    /* if(DEBUG) printf("Arena malloc\n"); */
    return malloc(size);
}

static void
_PyObject_ArenaFree(void *ctx, void *ptr, size_t size)
{
    if(DEBUG) printf("ArenaFree: Ptr - %x\n", ptr);
    free(ptr);
}
#endif

#define MALLOC_ALLOC {NULL, _PyMeme_RawMalloc, _PyMeme_RawCalloc, _PyMeme_RawRealloc, _PyMeme_RawFree}
#ifdef WITH_PYMALLOC
#  define PYMALLOC_ALLOC {NULL, _PyObject_Malloc, _PyObject_Calloc, _PyObject_Realloc, _PyObject_Free}
#endif

#define PYRAW_ALLOC MALLOC_ALLOC
#ifdef WITH_PYMALLOC
#  define PYOBJ_ALLOC PYMALLOC_ALLOC
#else
#  define PYOBJ_ALLOC MALLOC_ALLOC
#endif
#define PYMEM_ALLOC PYOBJ_ALLOC

typedef struct {
    /* We tag each block with an API ID in order to tag API violations */
    char api_id;
    PyMemAllocatorEx alloc;
} debug_alloc_api_t;
static struct {
    debug_alloc_api_t raw;
    debug_alloc_api_t mem;
    debug_alloc_api_t obj;
} _PyMeme_Debug = {
    {'r', PYRAW_ALLOC},
    {'m', PYMEM_ALLOC},
    {'o', PYOBJ_ALLOC}
    };

#define PYDBGRAW_ALLOC \
    {&_PyMeme_Debug.raw, _PyMeme_DebugRawMalloc, _PyMeme_DebugRawCalloc, _PyMeme_DebugRawRealloc, _PyMeme_DebugRawFree}
#define PYDBGMEM_ALLOC \
    {&_PyMeme_Debug.mem, _PyMeme_DebugMalloc, _PyMeme_DebugCalloc, _PyMeme_DebugRealloc, _PyMeme_DebugFree}
#define PYDBGOBJ_ALLOC \
    {&_PyMeme_Debug.obj, _PyMeme_DebugMalloc, _PyMeme_DebugCalloc, _PyMeme_DebugRealloc, _PyMeme_DebugFree}

#ifdef Py_DEBUG
static PyMemAllocatorEx _PyMeme_Raw = PYDBGRAW_ALLOC;
static PyMemAllocatorEx _PyMem = PYDBGMEM_ALLOC;
static PyMemAllocatorEx _PyObject = PYDBGOBJ_ALLOC;
#else
static PyMemAllocatorEx _PyMeme_Raw = PYRAW_ALLOC;
static PyMemAllocatorEx _PyMem = PYMEM_ALLOC;
static PyMemAllocatorEx _PyObject = PYOBJ_ALLOC;
#endif


/* static int */
/* pymem_set_default_allocator(PyMemAllocatorDomain domain, int debug, */
/*                             PyMemAllocatorEx *old_alloc) */
/* { */
/*     if (old_alloc != NULL) { */
/*         PyMem_GetAllocator(domain, old_alloc); */
/*     } */


/*     PyMemAllocatorEx new_alloc; */
/*     switch(domain) */
/*     { */
/*     case PYMEM_DOMAIN_RAW: */
/*         new_alloc = (PyMemAllocatorEx)PYRAW_ALLOC; */
/*         break; */
/*     case PYMEM_DOMAIN_MEM: */
/*         new_alloc = (PyMemAllocatorEx)PYMEM_ALLOC; */
/*         break; */
/*     case PYMEM_DOMAIN_OBJ: */
/*         new_alloc = (PyMemAllocatorEx)PYOBJ_ALLOC; */
/*         break; */
/*     default: */
/*         /1* unknown domain *1/ */
/*         return -1; */
/*     } */
/*     PyMem_SetAllocator(domain, &new_alloc); */
/*     if (debug) { */
/*         _PyMeme_SetupDebugHooksDomain(domain); */
/*     } */
/*     return 0; */
/* } */

//int
//_PyMeme_SetDefaultAllocator(PyMemAllocatorDomain domain,
//                           PyMemAllocatorEx *old_alloc)
//{
//#ifdef Py_DEBUG
//    const int debug = 1;
//#else
//    const int debug = 0;
//#endif
//    return pymem_set_default_allocator(domain, debug, old_alloc);
//}


//int
//_PyMeme_GetAllocatorName(const char *name, PyMemAllocatorName *allocator)
//{
//    if (name == NULL || *name == '\0') {
//        /* PYTHONMALLOC is empty or is not set or ignored (-E/-I command line
//           nameions): use default memory allocators */
//        *allocator = PYMEM_ALLOCATOR_DEFAULT;
//    }
//    else if (strcmp(name, "default") == 0) {
//        *allocator = PYMEM_ALLOCATOR_DEFAULT;
//    }
//    else if (strcmp(name, "debug") == 0) {
//        *allocator = PYMEM_ALLOCATOR_DEBUG;
//    }
//#ifdef WITH_PYMALLOC
//    else if (strcmp(name, "pymalloc") == 0) {
//        *allocator = PYMEM_ALLOCATOR_PYMALLOC;
//    }
//    else if (strcmp(name, "pymalloc_debug") == 0) {
//        *allocator = PYMEM_ALLOCATOR_PYMALLOC_DEBUG;
//    }
//#endif
//    else if (strcmp(name, "malloc") == 0) {
//        *allocator = PYMEM_ALLOCATOR_MALLOC;
//    }
//    else if (strcmp(name, "malloc_debug") == 0) {
//        *allocator = PYMEM_ALLOCATOR_MALLOC_DEBUG;
//    }
//    else {
//        /* unknown allocator */
//        return -1;
//    }
//    return 0;
//}

int
pymeme_setup_default_allocators()
{
    tssKey = PyThread_create_key();
    int c = PyThread_set_key_value(tssKey, (void *)(tssValue));
    /* privateOrSharedDomain = PyThread_create_key(); */
    /* int d = PyThread_set_key_value(privateOrSharedDomain, (void *)(sharedValue)); */

    PyMem_GetAllocator(PYMEM_DOMAIN_RAW, &default_raw_alloc);
    PyMem_GetAllocator(PYMEM_DOMAIN_MEM, &default_mem_alloc);

    init_arena_components();
    init_usedpools();

    allocated_ptrs = ht_create();
    PyMemAllocatorEx malloc_alloc = MALLOC_ALLOC;
    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &malloc_alloc);

    PyMemAllocatorEx pymalloc = PYMALLOC_ALLOC;
    PyMem_SetAllocator(PYMEM_DOMAIN_MEM, &pymalloc);

    /* PyMemAllocatorEx pyobjalloc = PYOBJ_ALLOC; */
    /* PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &pyobjalloc); */

    /* signal(SIGSEGV, segfault_handler); */

    /* Setup segfault handler */
    /* struct sigaction action; */
    /* action.sa_handler = segfault_handler; */
    /* action.sa_flags   = SA_NODEFER; */
    /* sigaction(SIGSEGV, &action, NULL); */

    return 0;
}

int pymem_cleanup_arenas(){
    if(DEBUG) printf("[Arenas Cleanup]Initialized\n");
    void *cleanup_ctx = NULL;

    struct mmap_node *cleaner = mmap_list_head;
    while(cleaner != NULL)
    {
        /* Cleanup all non-default arenas */
        if(cleaner->pkey)
        {
            struct mmap_node *tmp = cleaner;
            if(DEBUG) printf("[Arenas Cleanup]Ptr: %x, size: %d\n", cleaner->ptr, cleaner->size);
            _PyObject_ArenaMunmap(cleanup_ctx, cleaner->ptr, cleaner->size);

            if(cleaner->next == NULL)
                cleaner->prev->next = NULL;
            if(cleaner->prev == NULL)
                cleaner->next->prev = NULL;
            else if(cleaner->next && cleaner->prev)
            {
                cleaner->prev->next = cleaner->next;
                cleaner->next->prev = cleaner->prev;
            }
            cleaner = cleaner->next;
            free(tmp);
        }
        else
            cleaner = cleaner->next;
    }

    if(DEBUG) printf("[Arenas Cleanup]Finished\n");
}

int pymem_reset_allocators()
{
    if(collectStats) printAllStats();
    int retVal = ht_delete(allocated_ptrs);
    if(DEBUG) printf("Hashtable delete retVal: %d\n", retVal);
    /* pymem_cleanup_arenas(); */
    /* PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &default_raw_alloc); */
    /* PyMem_SetAllocator(PYMEM_DOMAIN_MEM, &default_mem_alloc); */
    return retVal;
}

static int
pymemallocator_eq(PyMemAllocatorEx *a, PyMemAllocatorEx *b)
{
    return (memcmp(a, b, sizeof(PyMemAllocatorEx)) == 0);
}

#undef MALLOC_ALLOC
#undef PYMALLOC_ALLOC
#undef PYRAW_ALLOC
#undef PYMEM_ALLOC
#undef PYOBJ_ALLOC
#undef PYDBGRAW_ALLOC
#undef PYDBGMEM_ALLOC
#undef PYDBGOBJ_ALLOC


static PyObjectArenaAllocator _PyObject_Arena = {NULL,
#ifdef MS_WINDOWS
    _PyObject_ArenaVirtualAlloc, _PyObject_ArenaVirtualFree
#elif defined(ARENAS_USE_MMAP)
    _PyObject_ArenaMmap, _PyObject_ArenaMunmap
#else
    _PyObject_ArenaMalloc, _PyObject_ArenaFree
#endif
    };

void *
PyMem_RawMalloc(size_t size)
{
    /*
     * Limit ourselves to PY_SSIZE_T_MAX bytes to prevent security holes.
     * Most python internals blindly use a signed Py_ssize_t to track
     * things without checking for overflows or negatives.
     * As size_t is unsigned, checking for size < 0 is not required.
     */
    if (size > (size_t)PY_SSIZE_T_MAX)
        return NULL;
    return _PyMeme_Raw.malloc(_PyMeme_Raw.ctx, size);
}

void *
PyMem_RawCalloc(size_t nelem, size_t elsize)
{
    /* see PyMem_RawMalloc() */
    if (elsize != 0 && nelem > (size_t)PY_SSIZE_T_MAX / elsize)
        return NULL;
    return _PyMeme_Raw.calloc(_PyMeme_Raw.ctx, nelem, elsize);
}

void*
PyMem_RawRealloc(void *ptr, size_t new_size)
{
    /* see PyMem_RawMalloc() */
    if (new_size > (size_t)PY_SSIZE_T_MAX)
        return NULL;
    return _PyMeme_Raw.realloc(_PyMeme_Raw.ctx, ptr, new_size);
}

void PyMem_RawFree(void *ptr)
{
    _PyMeme_Raw.free(_PyMeme_Raw.ctx, ptr);
}


void *
PyMem_Malloc(size_t size)
{
    /* see PyMem_RawMalloc() */
    if (size > (size_t)PY_SSIZE_T_MAX)
        return NULL;
    return _PyMem.malloc(_PyMem.ctx, size);
}

void *
PyMem_Calloc(size_t nelem, size_t elsize)
{
    /* see PyMem_RawMalloc() */
    if (elsize != 0 && nelem > (size_t)PY_SSIZE_T_MAX / elsize)
        return NULL;
    return _PyMem.calloc(_PyMem.ctx, nelem, elsize);
}

void *
PyMem_Realloc(void *ptr, size_t new_size)
{
    /* see PyMem_RawMalloc() */
    if (new_size > (size_t)PY_SSIZE_T_MAX)
        return NULL;
    return _PyMem.realloc(_PyMem.ctx, ptr, new_size);
}

void
PyMem_Free(void *ptr)
{
    _PyMem.free(_PyMem.ctx, ptr);
}


/* void * */
/* PyObject_Malloc(size_t size) */
/* { */
/*     /1* see PyMem_RawMalloc() *1/ */
/*     if (size > (size_t)PY_SSIZE_T_MAX) */
/*         return NULL; */
/*     return _PyObject.malloc(_PyObject.ctx, size); */
/* } */

/* void * */
/* PyObject_Calloc(size_t nelem, size_t elsize) */
/* { */
/*     /1* see PyMem_RawMalloc() *1/ */
/*     if (elsize != 0 && nelem > (size_t)PY_SSIZE_T_MAX / elsize) */
/*         return NULL; */
/*     return _PyObject.calloc(_PyObject.ctx, nelem, elsize); */
/* } */

/* void * */
/* PyObject_Realloc(void *ptr, size_t new_size) */
/* { */
/*     /1* see PyMem_RawMalloc() *1/ */
/*     if (new_size > (size_t)PY_SSIZE_T_MAX) */
/*         return NULL; */
/*     return _PyObject.realloc(_PyObject.ctx, ptr, new_size); */
/* } */

/* void */
/* PyObject_Free(void *ptr) */
/* { */
/*     _PyObject.free(_PyObject.ctx, ptr); */
/* } */


/* If we're using GCC, use __builtin_expect() to reduce overhead of
   the valgrind checks */
#if defined(__GNUC__) && (__GNUC__ > 2) && defined(__OPTIMIZE__)
#  define UNLIKELY(value) __builtin_expect((value), 0)
#  define LIKELY(value) __builtin_expect((value), 1)
#else
#  define UNLIKELY(value) (value)
#  define LIKELY(value) (value)
#endif

#ifdef WITH_PYMALLOC

#ifdef WITH_VALGRIND
#include <valgrind/valgrind.h>

/* -1 indicates that we haven't checked that we're running on valgrind yet. */
static int running_on_valgrind = -1;
#endif


/* An object allocator for Python.

   Here is an introduction to the layers of the Python memory architecture,
   showing where the object allocator is actually used (layer +2), It is
   called for every object allocation and deallocation (PyObject_New/Del),
   unless the object-specific allocators implement a proprietary allocation
   scheme (ex.: ints use a simple free list). This is also the place where
   the cyclic garbage collector operates selectively on container objects.


    Object-specific allocators
    _____   ______   ______       ________
   [ int ] [ dict ] [ list ] ... [ string ]       Python core         |
+3 | <----- Object-specific memory -----> | <-- Non-object memory --> |
    _______________________________       |                           |
   [   Python's object allocator   ]      |                           |
+2 | ####### Object memory ####### | <------ Internal buffers ------> |
    ______________________________________________________________    |
   [          Python's raw memory allocator (PyMem_ API)          ]   |
+1 | <----- Python memory (under PyMem manager's control) ------> |   |
    __________________________________________________________________
   [    Underlying general-purpose allocator (ex: C library malloc)   ]
 0 | <------ Virtual memory allocated for the python process -------> |

   =========================================================================
    _______________________________________________________________________
   [                OS-specific Virtual Memory Manager (VMM)               ]
-1 | <--- Kernel dynamic storage allocation & management (page-based) ---> |
    __________________________________   __________________________________
   [                                  ] [                                  ]
-2 | <-- Physical memory: ROM/RAM --> | | <-- Secondary storage (swap) --> |

*/
/*==========================================================================*/

/* A fast, special-purpose memory allocator for small blocks, to be used
   on top of a general-purpose malloc -- heavily based on previous art. */

/* Vladimir Marangozov -- August 2000 */

/*
 * "Memory management is where the rubber meets the road -- if we do the wrong
 * thing at any level, the results will not be good. And if we don't make the
 * levels work well together, we are in serious trouble." (1)
 *
 * (1) Paul R. Wilson, Mark S. Johnstone, Michael Neely, and David Boles,
 *    "Dynamic Storage Allocation: A Survey and Critical Review",
 *    in Proc. 1995 Int'l. Workshop on Memory Management, September 1995.
 */

/* #undef WITH_MEMORY_LIMITS */         /* disable mem limit checks  */

/*==========================================================================*/

/*
 * Allocation strategy abstract:
 *
 * For small requests, the allocator sub-allocates <Big> blocks of memory.
 * Requests greater than SMALL_REQUEST_THRESHOLD bytes are routed to the
 * system's allocator.
 *
 * Small requests are grouped in size classes spaced 8 bytes apart, due
 * to the required valid alignment of the returned address. Requests of
 * a particular size are serviced from memory pools of 4K (one VMM page).
 * Pools are fragmented on demand and contain free lists of blocks of one
 * particular size class. In other words, there is a fixed-size allocator
 * for each size class. Free pools are shared by the different allocators
 * thus minimizing the space reserved for a particular size class.
 *
 * This allocation strategy is a variant of what is known as "simple
 * segregated storage based on array of free lists". The main drawback of
 * simple segregated storage is that we might end up with lot of reserved
 * memory for the different free lists, which degenerate in time. To avoid
 * this, we partition each free list in pools and we share dynamically the
 * reserved space between all free lists. This technique is quite efficient
 * for memory intensive programs which allocate mainly small-sized blocks.
 *
 * For small requests we have the following table:
 *
 * Request in bytes     Size of allocated block      Size class idx
 * ----------------------------------------------------------------
 *        1-8                     8                       0
 *        9-16                   16                       1
 *       17-24                   24                       2
 *       25-32                   32                       3
 *       33-40                   40                       4
 *       41-48                   48                       5
 *       49-56                   56                       6
 *       57-64                   64                       7
 *       65-72                   72                       8
 *        ...                   ...                     ...
 *      497-504                 504                      62
 *      505-512                 512                      63
 *
 *      0, SMALL_REQUEST_THRESHOLD + 1 and up: routed to the underlying
 *      allocator.
 */

/*==========================================================================*/

/*
 * -- Main tunable settings section --
 */

/*
 * Alignment of addresses returned to the user. 8-bytes alignment works
 * on most current architectures (with 32-bit or 64-bit address busses).
 * The alignment value is also used for grouping small requests in size
 * classes spaced ALIGNMENT bytes apart.
 *
 * You shouldn't change this unless you know what you are doing.
 */

#if SIZEOF_VOID_P > 4
#define ALIGNMENT              16               /* must be 2^N */
#define ALIGNMENT_SHIFT         4
#else
#define ALIGNMENT               8               /* must be 2^N */
#define ALIGNMENT_SHIFT         3
#endif

/* Return the number of bytes in size class I, as a uint. */
#define INDEX2SIZE(I) (((uint)(I) + 1) << ALIGNMENT_SHIFT)

/*
 * Max size threshold below which malloc requests are considered to be
 * small enough in order to use preallocated memory pools. You can tune
 * this value according to your application behaviour and memory needs.
 *
 * Note: a size threshold of 512 guarantees that newly created dictionaries
 * will be allocated from preallocated memory pools on 64-bit.
 *
 * The following invariants must hold:
 *      1) ALIGNMENT <= SMALL_REQUEST_THRESHOLD <= 512
 *      2) SMALL_REQUEST_THRESHOLD is evenly divisible by ALIGNMENT
 *
 * Although not required, for better performance and space efficiency,
 * it is recommended that SMALL_REQUEST_THRESHOLD is set to a power of 2.
 */
#define SMALL_REQUEST_THRESHOLD 512
#define NB_SMALL_SIZE_CLASSES   (SMALL_REQUEST_THRESHOLD / ALIGNMENT)

/*
 * The system's VMM page size can be obtained on most unices with a
 * getpagesize() call or deduced from various header files. To make
 * things simpler, we assume that it is 4K, which is OK for most systems.
 * It is probably better if this is the native page size, but it doesn't
 * have to be.  In theory, if SYSTEM_PAGE_SIZE is larger than the native page
 * size, then `POOL_ADDR(p)->arenaindex' could rarely cause a segmentation
 * violation fault.  4K is apparently OK for all the platforms that python
 * currently targets.
 */

/*
 * Maximum amount of memory managed by the allocator for small requests.
 */
#ifdef WITH_MEMORY_LIMITS
#ifndef SMALL_MEMORY_LIMIT
#define SMALL_MEMORY_LIMIT      (64 * 1024 * 1024)      /* 64 MB -- more? */
#endif
#endif

/*
 * The allocator sub-allocates <Big> blocks of memory (called arenas) aligned
 * on a page boundary. This is a reserved virtual address space for the
 * current process (obtained through a malloc()/mmap() call). In no way this
 * means that the memory arenas will be used entirely. A malloc(<Big>) is
 * usually an address range reservation for <Big> bytes, unless all pages within
 * this space are referenced subsequently. So malloc'ing big blocks and not
 * using them does not mean "wasting memory". It's an addressable range
 * wastage...
 *
 * Arenas are allocated with mmap() on systems supporting anonymous memory
 * mappings to reduce heap fragmentation.
 */
#define ARENA_SIZE              (256 << 10)     /* 256KB */

#ifdef WITH_MEMORY_LIMITS
#define MAX_ARENAS              (SMALL_MEMORY_LIMIT / ARENA_SIZE)
#endif

/*
 * Size of the pools used for small blocks. Should be a power of 2,
 * between 1K and SYSTEM_PAGE_SIZE, that is: 1k, 2k, 4k.
 */
#define POOL_SIZE               SYSTEM_PAGE_SIZE        /* must be 2^N */
#define POOL_SIZE_MASK          SYSTEM_PAGE_SIZE_MASK

#define MAX_POOLS_IN_ARENA  (ARENA_SIZE / POOL_SIZE)
#if MAX_POOLS_IN_ARENA * POOL_SIZE != ARENA_SIZE
#   error "arena size not an exact multiple of pool size"
#endif

/*
 * -- End of tunable settings section --
 */

/*==========================================================================*/

/* When you say memory, my mind reasons in terms of (pointers to) blocks */
typedef uint8_t block;

/* Pool for small blocks. */
struct pool_header {
    union { block *_padding;
            uint count; } ref;          /* number of allocated blocks    */
    block *freeblock;                   /* pool's free list head         */
    struct pool_header *nextpool;       /* next pool of this size class  */
    struct pool_header *prevpool;       /* previous pool       ""        */
    uint arenaindex;                    /* index into arenas of base adr */
    uint szidx;                         /* block size class index        */
    uint nextoffset;                    /* bytes to virgin block         */
    uint maxnextoffset;                 /* largest valid nextoffset      */
};

typedef struct pool_header *poolp;

/* Record keeping for arenas. */
struct arena_object {
    /* The address of the arena, as returned by malloc.  Note that 0
     * will never be returned by a successful malloc, and is used
     * here to mark an arena_object that doesn't correspond to an
     * allocated arena.
     */
    uintptr_t address;

    /* Pool-aligned pointer to the next pool to be carved off. */
    block* pool_address;

    /* The number of available pools in the arena:  free pools + never-
     * allocated pools.
     */
    uint nfreepools;

    /* The total number of pools in the arena, whether or not available. */
    uint ntotalpools;

    /* Singly-linked list of available pools. */
    struct pool_header* freepools;

    /* Whenever this arena_object is not associated with an allocated
     * arena, the nextarena member is used to link all unassociated
     * arena_objects in the singly-linked `unused_arena_objects[pkey]` list.
     * The prevarena member is unused in this case.
     *
     * When this arena_object is associated with an allocated arena
     * with at least one available pool, both members are used in the
     * doubly-linked `usable_arenas[pkey]` list, which is maintained in
     * increasing order of `nfreepools` values.
     *
     * Else this arena_object is associated with an allocated arena
     * all of whose pools are in use.  `nextarena` and `prevarena`
     * are both meaningless in this case.
     */
    struct arena_object* nextarena;
    struct arena_object* prevarena;
};

#define POOL_OVERHEAD   _Py_SIZE_ROUND_UP(sizeof(struct pool_header), ALIGNMENT)

#define DUMMY_SIZE_IDX          0xffff  /* size class of newly cached pools */

/* Round pointer P down to the closest pool-aligned address <= P, as a poolp */
#define POOL_ADDR(P) ((poolp)_Py_ALIGN_DOWN((P), POOL_SIZE))

/* Return total number of blocks in pool of size index I, as a uint. */
#define NUMBLOCKS(I) ((uint)(POOL_SIZE - POOL_OVERHEAD) / INDEX2SIZE(I))

/*==========================================================================*/

/*
 * Pool table -- headed, circular, doubly-linked lists of partially used pools.

This is involved.  For an index i, usedpools[i+i] is the header for a list of
all partially used pools holding small blocks with "size class idx" i. So
usedpools[0] corresponds to blocks of size 8, usedpools[2] to blocks of size
16, and so on:  index 2*i <-> blocks of size (i+1)<<ALIGNMENT_SHIFT.

Pools are carved off an arena's highwater mark (an arena_object's pool_address
member) as needed.  Once carved off, a pool is in one of three states forever
after:

used == partially used, neither empty nor full
    At least one block in the pool is currently allocated, and at least one
    block in the pool is not currently allocated (note this implies a pool
    has room for at least two blocks).
    This is a pool's initial state, as a pool is created only when malloc
    needs space.
    The pool holds blocks of a fixed size, and is in the circular list headed
    at usedpools[i] (see above).  It's linked to the other used pools of the
    same size class via the pool_header's nextpool and prevpool members.
    If all but one block is currently allocated, a malloc can cause a
    transition to the full state.  If all but one block is not currently
    allocated, a free can cause a transition to the empty state.

full == all the pool's blocks are currently allocated
    On transition to full, a pool is unlinked from its usedpools[] list.
    It's not linked to from anything then anymore, and its nextpool and
    prevpool members are meaningless until it transitions back to used.
    A free of a block in a full pool puts the pool back in the used state.
    Then it's linked in at the front of the appropriate usedpools[] list, so
    that the next allocation for its size class will reuse the freed block.

empty == all the pool's blocks are currently available for allocation
    On transition to empty, a pool is unlinked from its usedpools[] list,
    and linked to the front of its arena_object's singly-linked freepools list,
    via its nextpool member.  The prevpool member has no meaning in this case.
    Empty pools have no inherent size class:  the next time a malloc finds
    an empty list in usedpools[], it takes the first pool off of freepools.
    If the size class needed happens to be the same as the size class the pool
    last had, some pool initialization can be skipped.


Block Management

Blocks within pools are again carved out as needed.  pool->freeblock points to
the start of a singly-linked list of free blocks within the pool.  When a
block is freed, it's inserted at the front of its pool's freeblock list.  Note
that the available blocks in a pool are *not* linked all together when a pool
is initialized.  Instead only "the first two" (lowest addresses) blocks are
set up, returning the first such block, and setting pool->freeblock to a
one-block list holding the second such block.  This is consistent with that
pymalloc strives at all levels (arena, pool, and block) never to touch a piece
of memory until it's actually needed.

So long as a pool is in the used state, we're certain there *is* a block
available for allocating, and pool->freeblock is not NULL.  If pool->freeblock
points to the end of the free list before we've carved the entire pool into
blocks, that means we simply haven't yet gotten to one of the higher-address
blocks.  The offset from the pool_header to the start of "the next" virgin
block is stored in the pool_header nextoffset member, and the largest value
of nextoffset that makes sense is stored in the maxnextoffset member when a
pool is initialized.  All the blocks in a pool have been passed out at least
once when and only when nextoffset > maxnextoffset.


Major obscurity:  While the usedpools vector is declared to have poolp
entries, it doesn't really.  It really contains two pointers per (conceptual)
poolp entry, the nextpool and prevpool members of a pool_header.  The
excruciating initialization code below fools C so that

    usedpool[i+i]

"acts like" a genuine poolp, but only so long as you only reference its
nextpool and prevpool members.  The "- 2*sizeof(block *)" gibberish is
compensating for that a pool_header's nextpool and prevpool members
immediately follow a pool_header's first two members:

    union { block *_padding;
            uint count; } ref;
    block *freeblock;

each of which consume sizeof(block *) bytes.  So what usedpools[i+i] really
contains is a fudged-up pointer p such that *if* C believes it's a poolp
pointer, then p->nextpool and p->prevpool are both p (meaning that the headed
circular list is empty).

It's unclear why the usedpools setup is so convoluted.  It could be to
minimize the amount of cache required to hold this heavily-referenced table
(which only *needs* the two interpool pointer members of a pool_header). OTOH,
referencing code has to remember to "double the index" and doing so isn't
free, usedpools[0] isn't a strictly legal pointer, and we're crucially relying
on that C doesn't insert any padding anywhere in a pool_header at or before
the prevpool member.
**************************************************************************** */

#define PTA(i,x)  ((poolp )((uint8_t *)&(usedpools[i][2*(x)]) - 2*sizeof(block *)))

static poolp usedpools[16][2 * ((NB_SMALL_SIZE_CLASSES + 7) / 8) * 8];

void init_usedpools()
{
    int num_pool_classes = (NB_SMALL_SIZE_CLASSES + 7) / 8;
    //Each usedpool[i] represents the usedpool for pages belonging to protection key, i.
    for(int i=0; i<16; i++)
    {
        for(int j=0; j<num_pool_classes; j++)
        {
            for(int k=0; k<16; k++)
            {
                usedpools[i][16*j+k] = PTA(i,(16*j+k)/2);
            }
        }
    }
}

//CHANGE: Making 2 usedpools, one for default pkey and other for all other
//static poolp usedpools[2][2 * ((NB_SMALL_SIZE_CLASSES + 7) / 8) * 8] = {{
//    PT(0,0), PT(0,1), PT(0,2), PT(0,3), PT(0,4), PT(0,5), PT(0,6), PT(0,7)
//#if NB_SMALL_SIZE_CLASSES > 8
//    , PT(0,8), PT(0,9), PT(0,10), PT(0,11), PT(0,12), PT(0,13), PT(0,14), PT(0,15)
//#if NB_SMALL_SIZE_CLASSES > 16
//    , PT(0,16), PT(0,17), PT(0,18), PT(0,19), PT(0,20), PT(0,21), PT(0,22), PT(0,23)
//#if NB_SMALL_SIZE_CLASSES > 24
//    , PT(0,24), PT(0,25), PT(0,26), PT(0,27), PT(0,28), PT(0,29), PT(0,30), PT(0,31)
//#if NB_SMALL_SIZE_CLASSES > 32
//    , PT(0,32), PT(0,33), PT(0,34), PT(0,35), PT(0,36), PT(0,37), PT(0,38), PT(0,39)
//#if NB_SMALL_SIZE_CLASSES > 40
//    , PT(0,40), PT(0,41), PT(0,42), PT(0,43), PT(0,44), PT(0,45), PT(0,46), PT(0,47)
//#if NB_SMALL_SIZE_CLASSES > 48
//    , PT(0,48), PT(0,49), PT(0,50), PT(0,51), PT(0,52), PT(0,53), PT(0,54), PT(0,55)
//#if NB_SMALL_SIZE_CLASSES > 56
//    , PT(0,56), PT(0,57), PT(0,58), PT(0,59), PT(0,60), PT(0,61), PT(0,62), PT(0,63)
//#if NB_SMALL_SIZE_CLASSES > 64
//#error "NB_SMALL_SIZE_CLASSES should be less than 64"
//#endif /* NB_SMALL_SIZE_CLASSES > 64 */
//#endif /* NB_SMALL_SIZE_CLASSES > 56 */
//#endif /* NB_SMALL_SIZE_CLASSES > 48 */
//#endif /* NB_SMALL_SIZE_CLASSES > 40 */
//#endif /* NB_SMALL_SIZE_CLASSES > 32 */
//#endif /* NB_SMALL_SIZE_CLASSES > 24 */
//#endif /* NB_SMALL_SIZE_CLASSES > 16 */
//#endif /* NB_SMALL_SIZE_CLASSES >  8 */
//},{
//    PT(1,0), PT(1,1), PT(1,2), PT(1,3), PT(1,4), PT(1,5), PT(1,6), PT(1,7)
//#if NB_SMALL_SIZE_CLASSES > 8
//    , PT(1,8), PT(1,9), PT(1,10), PT(1,11), PT(1,12), PT(1,13), PT(1,14), PT(1,15)
//#if NB_SMALL_SIZE_CLASSES > 16
//    , PT(1,16), PT(1,17), PT(1,18), PT(1,19), PT(1,20), PT(1,21), PT(1,22), PT(1,23)
//#if NB_SMALL_SIZE_CLASSES > 24
//    , PT(1,24), PT(1,25), PT(1,26), PT(1,27), PT(1,28), PT(1,29), PT(1,30), PT(1,31)
//#if NB_SMALL_SIZE_CLASSES > 32
//    , PT(1,32), PT(1,33), PT(1,34), PT(1,35), PT(1,36), PT(1,37), PT(1,38), PT(1,39)
//#if NB_SMALL_SIZE_CLASSES > 40
//    , PT(1,40), PT(1,41), PT(1,42), PT(1,43), PT(1,44), PT(1,45), PT(1,46), PT(1,47)
//#if NB_SMALL_SIZE_CLASSES > 48
//    , PT(1,48), PT(1,49), PT(1,50), PT(1,51), PT(1,52), PT(1,53), PT(1,54), PT(1,55)
//#if NB_SMALL_SIZE_CLASSES > 56
//    , PT(1,56), PT(1,57), PT(1,58), PT(1,59), PT(1,60), PT(1,61), PT(1,62), PT(1,63)
//#if NB_SMALL_SIZE_CLASSES > 64
//#error "NB_SMALL_SIZE_CLASSES should be less than 64"
//#endif /* NB_SMALL_SIZE_CLASSES > 64 */
//#endif /* NB_SMALL_SIZE_CLASSES > 56 */
//#endif /* NB_SMALL_SIZE_CLASSES > 48 */
//#endif /* NB_SMALL_SIZE_CLASSES > 40 */
//#endif /* NB_SMALL_SIZE_CLASSES > 32 */
//#endif /* NB_SMALL_SIZE_CLASSES > 24 */
//#endif /* NB_SMALL_SIZE_CLASSES > 16 */
//#endif /* NB_SMALL_SIZE_CLASSES >  8 */
//}};

/*==========================================================================
Arena management.

`arenas` is a vector of arena_objects.  It contains maxarenas[pkey] entries, some of
which may not be currently used (== they're arena_objects that aren't
currently associated with an allocated arena).  Note that arenas proper are
separately malloc'ed.

Prior to Python 2.5, arenas were never free()'ed.  Starting with Python 2.5,
we do try to free() arenas, and use some mild heuristic strategies to increase
the likelihood that arenas eventually can be freed.

unused_arena_objects[pkey]

    This is a singly-linked list of the arena_objects that are currently not
    being used (no arena is associated with them).  Objects are taken off the
    head of the list in new_arena(), and are pushed on the head of the list in
    PyObject_Free() when the arena is empty.  Key invariant:  an arena_object
    is on this list if and only if its .address member is 0.

usable_arenas[pkey]

    This is a doubly-linked list of the arena_objects associated with arenas
    that have pools available.  These pools are either waiting to be reused,
    or have not been used before.  The list is sorted to have the most-
    allocated arenas first (ascending order based on the nfreepools member).
    This means that the next allocation will come from a heavily used arena,
    which gives the nearly empty arenas a chance to be returned to the system.
    In my unscientific tests this dramatically improved the number of arenas
    that could be freed.

Note that an arena_object associated with an arena all of whose pools are
currently in use isn't on either list.

Changed in Python 3.8:  keeping usable_arenas[pkey] sorted by number of free pools
used to be done by one-at-a-time linear search when an arena's number of
free pools changed.  That could, overall, consume time quadratic in the
number of arenas.  That didn't really matter when there were only a few
hundred arenas (typical!), but could be a timing disaster when there were
hundreds of thousands.  See bpo-37029.

Now we have a vector of "search fingers" to eliminate the need to search:
nfp2lasta[pkey][nfp] returns the last ("rightmost") arena in usable_arenas[pkey]
with nfp free pools.  This is NULL if and only if there is no arena with
nfp free pools in usable_arenas[pkey].
*/

#define INITIAL_ARENA_OBJECTS 16
/* Array of objects used to track chunks of memory (arenas). */
static struct arena_object* arenas[16];
/* Number of slots currently allocated in the `arenas` vector. */
static uint maxarenas[16];

/* The head of the singly-linked, NULL-terminated list of available
 * arena_objects.
 */
static struct arena_object* unused_arena_objects[16];

/* The head of the doubly-linked, NULL-terminated at each end, list of
 * arena_objects associated with arenas that have pools available.
 */
static struct arena_object* usable_arenas[16];

/* nfp2lasta[pkey][nfp] is the last arena in usable_arenas[pkey] with nfp free pools */
static struct arena_object* nfp2lasta[16][MAX_POOLS_IN_ARENA + 1];

/* How many arena_objects do we initially allocate?
 * 16 = can allocate 16 arenas = 16 * ARENA_SIZE = 4MB before growing the
 * `arenas` vector.
     */

/* Number of arenas allocated that haven't been free()'d. */
static size_t narenas_currently_allocated[16];

/* Total number of times malloc() called to allocate an arena. */
static size_t ntimes_arena_allocated[16];
/* High water mark (max value ever seen) for narenas_currently_allocated[pkey]. */
static size_t narenas_highwater[16];

static Py_ssize_t raw_allocated_blocks[16];

void init_arena_components()
{
    for(int i=0; i<16; i++)
    {
        /* Array of objects used to track chunks of memory (arenas). */
        arenas[i] = NULL;
        /* Number of slots currently allocated in the `arenas[pkey]` vector. */
        maxarenas[i] = 0;
        unused_arena_objects[i] = NULL;
        usable_arenas[i] = NULL;
        narenas_currently_allocated[i] = 0;
        ntimes_arena_allocated[i] = 0;
        narenas_highwater[i] = 0;

        for(int j=0; j < MAX_POOLS_IN_ARENA + 1; j++)
            nfp2lasta[i][j] = NULL;

        raw_allocated_blocks[i] = 0;
    }
}

Py_ssize_t
_Py_GetAllocatedBlocks(void)
{
    int pkey = get_thread_pkey(1);
    Py_ssize_t n = raw_allocated_blocks[pkey];
    /* add up allocated blocks for used pools */
    for (uint i = 0; i < maxarenas[pkey]; ++i) {
        /* Skip arenas[pkey] which are not allocated. */
        if (arenas[pkey][i].address == 0) {
            continue;
        }

        uintptr_t base = (uintptr_t)_Py_ALIGN_UP(arenas[pkey][i].address, POOL_SIZE);

        /* visit every pool in the arena */
        assert(base <= (uintptr_t) arenas[pkey][i].pool_address);
        for (; base < (uintptr_t) arenas[pkey][i].pool_address; base += POOL_SIZE) {
            poolp p = (poolp)base;
            n += p->ref.count;
        }
    }
    return n;
}


/* Allocate a new arena.  If we run out of memory, return NULL.  Else
 * allocate a new arena, and return the address of an arena_object
 * describing the new arena.  It's expected that the caller will set
 * `usable_arenas[pkey]` to the return value.
 */
static struct arena_object*
new_arena(void)
{
    int pkey = get_thread_pkey(1);
    struct arena_object* arenaobj;
    uint excess;        /* number of bytes above pool alignment */
    void *address;
    static int debug_stats = -1;

    if (debug_stats == -1) {
        const char *opt = Py_GETENV("PYTHONMALLOCSTATS");
        debug_stats = (opt != NULL && *opt != '\0');
    }
    //TODO: Removed because of error:conflicting types for ‘_PyObject_DebugMallocStats'
    /*if (debug_stats)
        _PyObject_DebugMallocStats(stderr);*/

    if (unused_arena_objects[pkey] == NULL) {
        uint i;
        uint numarenas;
        size_t nbytes;

        /* Double the number of arena objects on each allocation.
         * Note that it's possible for `numarenas` to overflow.
         */
        numarenas = maxarenas[pkey] ? maxarenas[pkey] << 1 : INITIAL_ARENA_OBJECTS;
        if (numarenas <= maxarenas[pkey])
            return NULL;                /* overflow */
#if SIZEOF_SIZE_T <= SIZEOF_INT
        if (numarenas > SIZE_MAX / sizeof(*arenas[pkey]))
            return NULL;                /* overflow */
#endif
        nbytes = numarenas * sizeof(*arenas[pkey]);
        arenaobj = (struct arena_object *)PyMem_RawRealloc(arenas[pkey], nbytes);
        if (arenaobj == NULL)
            return NULL;
        arenas[pkey] = arenaobj;

        /* We might need to fix pointers that were copied.  However,
         * new_arena only gets called when all the pages in the
         * previous arenas[pkey] are full.  Thus, there are *no* pointers
         * into the old array. Thus, we don't have to worry about
         * invalid pointers.  Just to be sure, some asserts:
         */
        assert(usable_arenas[pkey] == NULL);
        assert(unused_arena_objects[pkey] == NULL);

        /* Put the new arenas[pkey] on the unused_arena_objects[pkey] list. */
        for (i = maxarenas[pkey]; i < numarenas; ++i) {
            arenas[pkey][i].address = 0;              /* mark as unassociated */
            arenas[pkey][i].nextarena = i < numarenas - 1 ?
                                   &arenas[pkey][i+1] : NULL;
        }

        /* Update globals. */
        unused_arena_objects[pkey] = &arenas[pkey][maxarenas[pkey]];
        maxarenas[pkey] = numarenas;
    }

    /* Take the next available arena object off the head of the list. */
    assert(unused_arena_objects[pkey] != NULL);
    arenaobj = unused_arena_objects[pkey];
    unused_arena_objects[pkey] = arenaobj->nextarena;
    assert(arenaobj->address == 0);
    address = _PyObject_Arena.alloc(_PyObject_Arena.ctx, ARENA_SIZE);
    if (address == NULL) {
        /* The allocation failed: return NULL after putting the
         * arenaobj back.
         */
        arenaobj->nextarena = unused_arena_objects[pkey];
        unused_arena_objects[pkey] = arenaobj;
        return NULL;
    }
    arenaobj->address = (uintptr_t)address;

    ++narenas_currently_allocated[pkey];
    ++ntimes_arena_allocated[pkey];
    if (narenas_currently_allocated[pkey] > narenas_highwater[pkey])
        narenas_highwater[pkey] = narenas_currently_allocated[pkey];
    arenaobj->freepools = NULL;
    /* pool_address <- first pool-aligned address in the arena
       nfreepools <- number of whole pools that fit after alignment */
    arenaobj->pool_address = (block*)arenaobj->address;
    arenaobj->nfreepools = MAX_POOLS_IN_ARENA;
    excess = (uint)(arenaobj->address & POOL_SIZE_MASK);
    if (excess != 0) {
        --arenaobj->nfreepools;
        arenaobj->pool_address += POOL_SIZE - excess;
    }
    arenaobj->ntotalpools = arenaobj->nfreepools;

    return arenaobj;
}


/*
mpk_address_in_range(P, POOL)

Return true if and only if P is an address that was allocated by pymalloc.
POOL must be the pool address associated with P, i.e., POOL = POOL_ADDR(P)
(the caller is asked to compute this because the macro expands POOL more than
once, and for efficiency it's best for the caller to assign POOL_ADDR(P) to a
variable and pass the latter to the macro; because mpk_address_in_range is
called on every alloc/realloc/free, micro-efficiency is important here).

Tricky:  Let B be the arena base address associated with the pool, B =
arenas[(POOL)->arenaindex].address.  Then P belongs to the arena if and only if

    B <= P < B + ARENA_SIZE

Subtracting B throughout, this is true iff

    0 <= P-B < ARENA_SIZE

By using unsigned arithmetic, the "0 <=" half of the test can be skipped.

Obscure:  A PyMem "free memory" function can call the pymalloc free or realloc
before the first arena has been allocated.  `arenas` is still NULL in that
case.  We're relying on that maxarenas[pkey] is also 0 in that case, so that
(POOL)->arenaindex < maxarenas[pkey]  must be false, saving us from trying to index
into a NULL arenas.

Details:  given P and POOL, the arena_object corresponding to P is AO =
arenas[(POOL)->arenaindex].  Suppose obmalloc controls P.  Then (barring wild
stores, etc), POOL is the correct address of P's pool, AO.address is the
correct base address of the pool's arena, and P must be within ARENA_SIZE of
AO.address.  In addition, AO.address is not 0 (no arena can start at address 0
(NULL)).  Therefore mpk_address_in_range correctly reports that obmalloc
controls P.

Now suppose obmalloc does not control P (e.g., P was obtained via a direct
call to the system malloc() or realloc()).  (POOL)->arenaindex may be anything
in this case -- it may even be uninitialized trash.  If the trash arenaindex
is >= maxarenas[pkey], the macro correctly concludes at once that obmalloc doesn't
control P.

Else arenaindex is < maxarena, and AO is read up.  If AO corresponds to an
allocated arena, obmalloc controls all the memory in slice AO.address :
AO.address+ARENA_SIZE.  By case assumption, P is not controlled by obmalloc,
so P doesn't lie in that slice, so the macro correctly reports that P is not
controlled by obmalloc.

Finally, if P is not controlled by obmalloc and AO corresponds to an unused
arena_object (one not currently associated with an allocated arena),
AO.address is 0, and the second test in the macro reduces to:

    P < ARENA_SIZE

If P >= ARENA_SIZE (extremely likely), the macro again correctly concludes
that P is not controlled by obmalloc.  However, if P < ARENA_SIZE, this part
of the test still passes, and the third clause (AO.address != 0) is necessary
to get the correct result:  AO.address is 0 in this case, so the macro
correctly reports that P is not controlled by obmalloc (despite that P lies in
slice AO.address : AO.address + ARENA_SIZE).

Note:  The third (AO.address != 0) clause was added in Python 2.5.  Before
2.5, arenas were never free()'ed, and an arenaindex < maxarena always
corresponded to a currently-allocated arena, so the "P is not controlled by
obmalloc, AO corresponds to an unused arena_object, and P < ARENA_SIZE" case
was impossible.

Note that the logic is excruciating, and reading up possibly uninitialized
memory when P is not controlled by obmalloc (to get at (POOL)->arenaindex)
creates problems for some memory debuggers.  The overwhelming advantage is
that this test determines whether an arbitrary address is controlled by
obmalloc in a small constant time, independent of the number of arenas
obmalloc controls.  Since this test is needed at every entry point, it's
extremely desirable that it be this fast.
*/

static bool _Py_NO_ADDRESS_SAFETY_ANALYSIS
            _Py_NO_SANITIZE_THREAD
            _Py_NO_SANITIZE_MEMORY
mpk_address_in_range(void *p, poolp pool)
{
    // Since mpk_address_in_range may be reading from memory which was not allocated
    // by Python, it is important that pool->arenaindex is read only once, as
    // another thread may be concurrently modifying the value without holding
    // the GIL. The following dance forces the compiler to read pool->arenaindex
    // only once.
    // TODO: check performance.
    int pkey = get_thread_pkey(1);
    uint arenaindex = *((volatile uint *)&pool->arenaindex);
    return arenaindex < maxarenas[pkey] &&
        (uintptr_t)p - arenas[pkey][arenaindex].address < ARENA_SIZE &&
        arenas[pkey][arenaindex].address != 0;
}

/*
 * We need to search across arenas allocated in all threads when trying to free 
 * a memory address. mpk_address_in_range is very frequently used and is highly optimized 
 * as explained above and for cases other than free(), it suffices to check address in the 
 * current thread's arenas alone. Hence, maintaining two functions to only incur the additional
 * overhead of searching all threads' arenas only when required(when freeing a ptr)
 */

int _Py_NO_ADDRESS_SAFETY_ANALYSIS
            _Py_NO_SANITIZE_THREAD
            _Py_NO_SANITIZE_MEMORY
address_in_any_arena(void *p, poolp pool)
{
    // Since mpk_address_in_range may be reading from memory which was not allocated
    // by Python, it is important that pool->arenaindex is read only once, as
    // another thread may be concurrently modifying the value without holding
    // the GIL. The following dance forces the compiler to read pool->arenaindex
    // only once.
    int pkey = -1;
    uint arenaindex = *((volatile uint *)&pool->arenaindex);
    for(int i=0;i<16;i++)
    {
        if(arenaindex < maxarenas[i] &&
        (uintptr_t)p - arenas[i][arenaindex].address < ARENA_SIZE &&
        arenas[i][arenaindex].address != 0)
        {
            pkey = i; break;
        }
    }
    return pkey;
}

/*==========================================================================*/

// Called when freelist is exhausted.  Extend the freelist if there is
// space for a block.  Otherwise, remove this pool from usedpools.
static void
pymalloc_pool_extend(poolp pool, uint size)
{
    if (UNLIKELY(pool->nextoffset <= pool->maxnextoffset)) {
        /* There is room for another block. */
        pool->freeblock = (block*)pool + pool->nextoffset;
        pool->nextoffset += INDEX2SIZE(size);
        *(block **)(pool->freeblock) = NULL;
        return;
    }

    /* Pool is full, unlink from used pools. */
    poolp next;
    next = pool->nextpool;
    pool = pool->prevpool;
    next->prevpool = pool;
    pool->nextpool = next;
}

/* static void */ 
/* pkey_pool_protect(uintptr_t pool, int pkey) */
/* { */
/*     int out = syscall(329, pool, ARENA_SIZE, 3, pkey); */
/*     printf("Protected pool: %d with pkey: %d, out: %d, errno: %d\n", */ 
/*             pool, pkey, out, errno); */
/* } */

static void 
pkey_pool_protect(int *pool, int pkey)
{
    int out = syscall(329, pool, ARENA_SIZE, 3, pkey);
    /* if(DEBUG) printf("Protected pool: %x, size: %d, pkey: %d, out: %d, errno: %d\n", */ 
            /* pool, ARENA_SIZE, pkey, out, errno); */
}

/* called when pymalloc_alloc can not allocate a block from usedpool.
 * This function takes new pool and allocate a block from it.
 */
static void*
allocate_from_new_pool(uint size)
{
    int pkey = get_thread_pkey(1);
    /* There isn't a pool of the right size class immediately
     * available:  use a free pool.
     */
    if (UNLIKELY(usable_arenas[pkey] == NULL)) {
        /* No arena has a free pool:  allocate a new arena. */
#ifdef WITH_MEMORY_LIMITS
        if (narenas_currently_allocated[pkey] >= MAX_ARENAS) {
            return NULL;
        }
#endif
        usable_arenas[pkey] = new_arena();
        //Protect Here!
        if(MPK_ENABLED) pkey_pool_protect(usable_arenas[pkey]->address, pkey);

        if (usable_arenas[pkey] == NULL) {
            return NULL;
        }
        usable_arenas[pkey]->nextarena = usable_arenas[pkey]->prevarena = NULL;
        assert(nfp2lasta[pkey][usable_arenas[pkey]->nfreepools] == NULL);
        nfp2lasta[pkey][usable_arenas[pkey]->nfreepools] = usable_arenas[pkey];
    }
    assert(usable_arenas[pkey]->address != 0);

    /* This arena already had the smallest nfreepools value, so decreasing
     * nfreepools doesn't change that, and we don't need to rearrange the
     * usable_arenas[pkey] list.  However, if the arena becomes wholly allocated,
     * we need to remove its arena_object from usable_arenas[pkey].
     */
    assert(usable_arenas[pkey]->nfreepools > 0);
    if (nfp2lasta[pkey][usable_arenas[pkey]->nfreepools] == usable_arenas[pkey]) {
        /* It's the last of this size, so there won't be any. */
        nfp2lasta[pkey][usable_arenas[pkey]->nfreepools] = NULL;
    }
    /* If any free pools will remain, it will be the new smallest. */
    if (usable_arenas[pkey]->nfreepools > 1) {
        assert(nfp2lasta[pkey][usable_arenas[pkey]->nfreepools - 1] == NULL);
        nfp2lasta[pkey][usable_arenas[pkey]->nfreepools - 1] = usable_arenas[pkey];
    }

    /* Try to get a cached free pool. */
    poolp pool = usable_arenas[pkey]->freepools;
    if (LIKELY(pool != NULL)) {
        /* Unlink from cached pools. */
        usable_arenas[pkey]->freepools = pool->nextpool;
        usable_arenas[pkey]->nfreepools--;
        if (UNLIKELY(usable_arenas[pkey]->nfreepools == 0)) {
            /* Wholly allocated:  remove. */
            assert(usable_arenas[pkey]->freepools == NULL);
            assert(usable_arenas[pkey]->nextarena == NULL ||
                   usable_arenas[pkey]->nextarena->prevarena ==
                   usable_arenas[pkey]);
            usable_arenas[pkey] = usable_arenas[pkey]->nextarena;
            if (usable_arenas[pkey] != NULL) {
                usable_arenas[pkey]->prevarena = NULL;
                assert(usable_arenas[pkey]->address != 0);
            }
        }
        else {
            /* nfreepools > 0:  it must be that freepools
             * isn't NULL, or that we haven't yet carved
             * off all the arena's pools for the first
             * time.
             */
            assert(usable_arenas[pkey]->freepools != NULL ||
                   usable_arenas[pkey]->pool_address <=
                   (block*)usable_arenas[pkey]->address +
                       ARENA_SIZE - POOL_SIZE);
        }
    }
    else {
        /* Carve off a new pool. */
        assert(usable_arenas[pkey]->nfreepools > 0);
        assert(usable_arenas[pkey]->freepools == NULL);
        pool = (poolp)usable_arenas[pkey]->pool_address;
        assert((block*)pool <= (block*)usable_arenas[pkey]->address +
                                 ARENA_SIZE - POOL_SIZE);
        pool->arenaindex = (uint)(usable_arenas[pkey] - arenas[pkey]);
        assert(&arenas[pkey][pool->arenaindex] == usable_arenas[pkey]);
        pool->szidx = DUMMY_SIZE_IDX;
        usable_arenas[pkey]->pool_address += POOL_SIZE;
        --usable_arenas[pkey]->nfreepools;

        if (usable_arenas[pkey]->nfreepools == 0) {
            assert(usable_arenas[pkey]->nextarena == NULL ||
                   usable_arenas[pkey]->nextarena->prevarena ==
                   usable_arenas[pkey]);
            /* Unlink the arena:  it is completely allocated. */
            usable_arenas[pkey] = usable_arenas[pkey]->nextarena;
            if (usable_arenas[pkey] != NULL) {
                usable_arenas[pkey]->prevarena = NULL;
                assert(usable_arenas[pkey]->address != 0);
            }
        }
    }

    /* Frontlink to used pools. */
    block *bp;
    /* pkey_pool_protect((int *)pool, pkey); */
    poolp next = usedpools[pkey][size + size]; /* == prev */
    pool->nextpool = next;
    pool->prevpool = next;
    next->nextpool = pool;
    next->prevpool = pool;
    pool->ref.count = 1;
    if (pool->szidx == size) {
        /* Luckily, this pool last contained blocks
         * of the same size class, so its header
         * and free list are already initialized.
         */
        bp = pool->freeblock;
        assert(bp != NULL);
        pool->freeblock = *(block **)bp;
        return bp;
    }
    /*
     * Initialize the pool header, set up the free list to
     * contain just the second block, and return the first
     * block.
     */
    pool->szidx = size;
    size = INDEX2SIZE(size);
    bp = (block *)pool + POOL_OVERHEAD;
    pool->nextoffset = POOL_OVERHEAD + (size << 1);
    pool->maxnextoffset = POOL_SIZE - size;
    pool->freeblock = bp + size;
    *(block **)(pool->freeblock) = NULL;
    return bp;
}

/* pymalloc allocator

   Return a pointer to newly allocated memory if pymalloc allocated memory.

   Return NULL if pymalloc failed to allocate the memory block: on bigger
   requests, on error in the code below (as a last chance to serve the request)
   or when the max memory limit has been reached.
*/
static inline void*
pymalloc_alloc(void *ctx, size_t nbytes)
{
    int pkey = get_thread_pkey(1);
    /* printf("[Pymalloc] Requirement:%d, pkey is:%d!\n", nbytes, pkey); */
#ifdef WITH_VALGRIND
    if (UNLIKELY(running_on_valgrind == -1)) {
        running_on_valgrind = RUNNING_ON_VALGRIND;
    }
    if (UNLIKELY(running_on_valgrind)) {
        return NULL;
    }
#endif

    if (UNLIKELY(nbytes == 0)) {
        return NULL;
    }
    if (UNLIKELY(nbytes > SMALL_REQUEST_THRESHOLD)) {
        return NULL;
    }

    uint size = (uint)(nbytes - 1) >> ALIGNMENT_SHIFT;
    poolp pool = usedpools[pkey][size + size];
    block *bp;

    /* printf("[Pymalloc] Size class: %d, Next offset:%d\n", size, pool->nextoffset); */
    if (LIKELY(pool != pool->nextpool)) {
        /* printf("[Pymalloc] Used pool present\n"); */
        /*
         * There is a used pool for this size class.
         * Pick up the head block of its free list.
         */
        ++pool->ref.count;
        bp = pool->freeblock;
        assert(bp != NULL);

        if (UNLIKELY((pool->freeblock = *(block **)bp) == NULL)) {
            // Reached the end of the free list, try to extend it.
            pymalloc_pool_extend(pool, size);
        }
    }
    else {
        /* printf("[Pymalloc] Allocating new pool\n"); */
        /* There isn't a pool of the right size class immediately
         * available:  use a free pool.
         */
        bp = allocate_from_new_pool(size);
    }

    /* printf("[Pymalloc] Pointer allocated :%p!\n", bp); */
    return (void *)bp;
}

int get_thread_pkey(bool checkForSharing)
{
    int sharing = 0;
    if(checkForSharing && privateOrSharedDomain) return 0;

    int *p = (int *)PyThread_get_key_value(tssKey);
    if (p) 
        return *p;
    else
        return 0; //default domain is shared domain
}

static void *
_PyObject_Malloc(void *ctx, size_t nbytes)
{
    int pkey = get_thread_pkey(1);
    void* ptr = pymalloc_alloc(ctx, nbytes);
    if (LIKELY(ptr != NULL)) {
        return ptr;
    }

    ptr = PyMem_RawMalloc(nbytes);
    if (ptr != NULL) {
        raw_allocated_blocks[pkey]++;
    }
    return ptr;
}

static void *
_PyObject_Calloc(void *ctx, size_t nelem, size_t elsize)
{
    int pkey = get_thread_pkey(1);
    assert(elsize == 0 || nelem <= (size_t)PY_SSIZE_T_MAX / elsize);
    size_t nbytes = nelem * elsize;

    void* ptr = pymalloc_alloc(ctx, nbytes);
    if (LIKELY(ptr != NULL)) {
        memset(ptr, 0, nbytes);
        return ptr;
    }

    ptr = PyMem_RawCalloc(nelem, elsize);
    if (ptr != NULL) {
        raw_allocated_blocks[pkey]++;
    }
    return ptr;
}


static void
insert_to_usedpool(poolp pool, int arenapkey)
{
    assert(pool->ref.count > 0);            /* else the pool is empty */

    uint size = pool->szidx;
    poolp next = usedpools[arenapkey][size + size];
    poolp prev = next->prevpool;

    /* insert pool before next:   prev <-> pool <-> next */
    pool->nextpool = next;
    pool->prevpool = prev;
    next->prevpool = pool;
    prev->nextpool = pool;
}

static void
insert_to_freepool(poolp pool, int arenapkey)
{
    poolp next = pool->nextpool;
    poolp prev = pool->prevpool;
    next->prevpool = prev;
    prev->nextpool = next;

    /* Link the pool to freepools.  This is a singly-linked
     * list, and pool->prevpool isn't used there.
     */
    struct arena_object *ao = &arenas[arenapkey][pool->arenaindex];
    pool->nextpool = ao->freepools;
    ao->freepools = pool;
    uint nf = ao->nfreepools;
    /* If this is the rightmost arena with this number of free pools,
     * nfp2lasta[arenapkey][nf] needs to change.  Caution:  if nf is 0, there
     * are no arenas in usable_arenas[arenapkey] with that value.
     */
    struct arena_object* lastnf = nfp2lasta[arenapkey][nf];
    assert((nf == 0 && lastnf == NULL) ||
           (nf > 0 &&
            lastnf != NULL &&
            lastnf->nfreepools == nf &&
            (lastnf->nextarena == NULL ||
             nf < lastnf->nextarena->nfreepools)));
    if (lastnf == ao) {  /* it is the rightmost */
        struct arena_object* p = ao->prevarena;
        nfp2lasta[arenapkey][nf] = (p != NULL && p->nfreepools == nf) ? p : NULL;
    }
    ao->nfreepools = ++nf;

    /* All the rest is arena management.  We just freed
     * a pool, and there are 4 cases for arena mgmt:
     * 1. If all the pools are free, return the arena to
     *    the system free().  Except if this is the last
     *    arena in the list, keep it to avoid thrashing:
     *    keeping one wholly free arena in the list avoids
     *    pathological cases where a simple loop would
     *    otherwise provoke needing to allocate and free an
     *    arena on every iteration.  See bpo-37257.
     * 2. If this is the only free pool in the arena,
     *    add the arena back to the `usable_arenas[arenapkey]` list.
     * 3. If the "next" arena has a smaller count of free
     *    pools, we have to "slide this arena right" to
     *    restore that usable_arenas[arenapkey] is sorted in order of
     *    nfreepools.
     * 4. Else there's nothing more to do.
     */
    if (nf == ao->ntotalpools && ao->nextarena != NULL) {
        /* Case 1.  First unlink ao from usable_arenas[arenapkey].
         */
        assert(ao->prevarena == NULL ||
               ao->prevarena->address != 0);
        assert(ao ->nextarena == NULL ||
               ao->nextarena->address != 0);

        /* Fix the pointer in the prevarena, or the
         * usable_arenas[arenapkey] pointer.
         */
        if (ao->prevarena == NULL) {
            usable_arenas[arenapkey] = ao->nextarena;
            assert(usable_arenas[arenapkey] == NULL ||
                   usable_arenas[arenapkey]->address != 0);
        }
        else {
            assert(ao->prevarena->nextarena == ao);
            ao->prevarena->nextarena =
                ao->nextarena;
        }
        /* Fix the pointer in the nextarena. */
        if (ao->nextarena != NULL) {
            assert(ao->nextarena->prevarena == ao);
            ao->nextarena->prevarena =
                ao->prevarena;
        }
        /* Record that this arena_object slot is
         * available to be reused.
         */
        ao->nextarena = unused_arena_objects[arenapkey];
        unused_arena_objects[arenapkey] = ao;

        /* Free the entire arena. */
        _PyObject_Arena.free(_PyObject_Arena.ctx,
                             (void *)ao->address, ARENA_SIZE);
        ao->address = 0;                        /* mark unassociated */
        --narenas_currently_allocated[arenapkey];

        return;
    }

    if (nf == 1) {
        /* Case 2.  Put ao at the head of
         * usable_arenas[arenapkey].  Note that because
         * ao->nfreepools was 0 before, ao isn't
         * currently on the usable_arenas[arenapkey] list.
         */
        ao->nextarena = usable_arenas[arenapkey];
        ao->prevarena = NULL;
        if (usable_arenas[arenapkey])
            usable_arenas[arenapkey]->prevarena = ao;
        usable_arenas[arenapkey] = ao;
        assert(usable_arenas[arenapkey]->address != 0);
        if (nfp2lasta[arenapkey][1] == NULL) {
            nfp2lasta[arenapkey][1] = ao;
        }

        return;
    }

    /* If this arena is now out of order, we need to keep
     * the list sorted.  The list is kept sorted so that
     * the "most full" arenas are used first, which allows
     * the nearly empty arenas to be completely freed.  In
     * a few un-scientific tests, it seems like this
     * approach allowed a lot more memory to be freed.
     */
    /* If this is the only arena with nf, record that. */
    if (nfp2lasta[arenapkey][nf] == NULL) {
        nfp2lasta[arenapkey][nf] = ao;
    } /* else the rightmost with nf doesn't change */
    /* If this was the rightmost of the old size, it remains in place. */
    if (ao == lastnf) {
        /* Case 4.  Nothing to do. */
        return;
    }
    /* If ao were the only arena in the list, the last block would have
     * gotten us out.
     */
    assert(ao->nextarena != NULL);

    /* Case 3:  We have to move the arena towards the end of the list,
     * because it has more free pools than the arena to its right.  It needs
     * to move to follow lastnf.
     * First unlink ao from usable_arenas[arenapkey].
     */
    if (ao->prevarena != NULL) {
        /* ao isn't at the head of the list */
        assert(ao->prevarena->nextarena == ao);
        ao->prevarena->nextarena = ao->nextarena;
    }
    else {
        /* ao is at the head of the list */
        assert(usable_arenas[arenapkey] == ao);
        usable_arenas[arenapkey] = ao->nextarena;
    }
    ao->nextarena->prevarena = ao->prevarena;
    /* And insert after lastnf. */
    ao->prevarena = lastnf;
    ao->nextarena = lastnf->nextarena;
    if (ao->nextarena != NULL) {
        ao->nextarena->prevarena = ao;
    }
    lastnf->nextarena = ao;
    /* Verify that the swaps worked. */
    assert(ao->nextarena == NULL || nf <= ao->nextarena->nfreepools);
    assert(ao->prevarena == NULL || nf > ao->prevarena->nfreepools);
    assert(ao->nextarena == NULL || ao->nextarena->prevarena == ao);
    assert((usable_arenas[arenapkey] == ao && ao->prevarena == NULL)
           || ao->prevarena->nextarena == ao);
}

/* Free a memory block allocated by pymalloc_alloc().
   Return 1 if it was freed.
   Return 0 if the block was not allocated by pymalloc_alloc(). */
static inline int
pymalloc_free(void *ctx, void *p)
{
    assert(p != NULL);

#ifdef WITH_VALGRIND
    if (UNLIKELY(running_on_valgrind > 0)) {
        return 0;
    }
#endif

    poolp pool = POOL_ADDR(p);
    int arenapkey = address_in_any_arena(p, pool);

    if(arenapkey==-1)
        return 0;

    /* We allocated this address. */

    /* Link p to the start of the pool's freeblock list.  Since
     * the pool had at least the p block outstanding, the pool
     * wasn't empty (so it's already in a usedpools[] list, or
     * was full and is in no list -- it's not in the freeblocks
     * list in any case).
     */
    assert(pool->ref.count > 0);            /* else it was empty */
    block *lastfree = pool->freeblock;
    *(block **)p = lastfree;
    pool->freeblock = (block *)p;
    pool->ref.count--;

    if (UNLIKELY(lastfree == NULL)) {
        /* Pool was full, so doesn't currently live in any list:
         * link it to the front of the appropriate usedpools[] list.
         * This mimics LRU pool usage for new allocations and
         * targets optimal filling when several pools contain
         * blocks of the same size class.
         */
        insert_to_usedpool(pool, arenapkey);
        return 1;
    }

    /* freeblock wasn't NULL, so the pool wasn't full,
     * and the pool is in a usedpools[] list.
     */
    if (LIKELY(pool->ref.count != 0)) {
        /* pool isn't empty:  leave it in usedpools */
        return 1;
    }

    /* Pool is now empty:  unlink from usedpools, and
     * link to the front of freepools.  This ensures that
     * previously freed pools will be allocated later
     * (being not referenced, they are perhaps paged out).
     */
    //TODO: Reset PKRU here
    /* printf("Entered pymalloc_free\n"); */
    /* pkey_pool_protect(pool, 0); */
    insert_to_freepool(pool,arenapkey);
    return 1;
}


static void
_PyObject_Free(void *ctx, void *p)
{
    if (p == NULL) {
        return;
    }

    // Need access to all domains because Free can be issued on memory block 
    // belonging to any domain
    if (MPK_ENABLED) pkru_set(-1, ENABLE_BOTH);
    int pkey = get_thread_pkey(0);

    if (UNLIKELY(!pymalloc_free(ctx, p))) {
        /* pymalloc didn't allocate this address */
        PyMem_RawFree(p);
        raw_allocated_blocks[pkey]--;
    }

    int pkru = pkey ? pkey : -1;
    if (MPK_ENABLED) pkru_set(pkru,ENABLE_BOTH);
    return;
}

/* pymalloc realloc.

   If nbytes==0, then as the Python docs promise, we do not treat this like
   free(p), and return a non-NULL result.

   Return 1 if pymalloc reallocated memory and wrote the new pointer into
   newptr_p.

   Return 0 if pymalloc didn't allocated p. */
static int
pymalloc_realloc(void *ctx, void **newptr_p, void *p, size_t nbytes)
{
    void *bp;
    poolp pool;
    size_t size;

    assert(p != NULL);

#ifdef WITH_VALGRIND
    /* Treat running_on_valgrind == -1 the same as 0 */
    if (UNLIKELY(running_on_valgrind > 0)) {
        return 0;
    }
#endif

    pool = POOL_ADDR(p);

    /* if(address_in_range(p, pool)){ */
    /*     printf("Voila! We know that the address was in a default arena!\n"); */
    /* } */

    if (!mpk_address_in_range(p, pool)) {
        /* pymalloc is not managing this block.

           If nbytes <= SMALL_REQUEST_THRESHOLD, it's tempting to try to take
           over this block.  However, if we do, we need to copy the valid data
           from the C-managed block to one of our blocks, and there's no
           portable way to know how much of the memory space starting at p is
           valid.

           As bug 1185883 pointed out the hard way, it's possible that the
           C-managed block is "at the end" of allocated VM space, so that a
           memory fault can occur if we try to copy nbytes bytes starting at p.
           Instead we punt: let C continue to manage this block. */
        return 0;
    }

    /* pymalloc is in charge of this block */
    size = INDEX2SIZE(pool->szidx);
    if (nbytes <= size) {
        /* The block is staying the same or shrinking.

           If it's shrinking, there's a tradeoff: it costs cycles to copy the
           block to a smaller size class, but it wastes memory not to copy it.

           The compromise here is to copy on shrink only if at least 25% of
           size can be shaved off. */
        if (4 * nbytes > 3 * size) {
            /* It's the same, or shrinking and new/old > 3/4. */
            *newptr_p = p;
            return 1;
        }
        size = nbytes;
    }

    bp = _PyObject_Malloc(ctx, nbytes);
    if (bp != NULL) {
        memcpy(bp, p, size);
        /* if(DEBUG) printf("Pymalloc_realloc: ptr - %x\n", p); */
        _PyObject_Free(ctx, p);
    }
    *newptr_p = bp;
    return 1;
}


static void *
_PyObject_Realloc(void *ctx, void *ptr, size_t nbytes)
{
    void *ptr2;

    if (ptr == NULL) {
        return _PyObject_Malloc(ctx, nbytes);
    }

    if (pymalloc_realloc(ctx, &ptr2, ptr, nbytes)) {
        return ptr2;
    }

    return PyMem_RawRealloc(ptr, nbytes);
}

#else   /* ! WITH_PYMALLOC */

/*==========================================================================*/
/* pymalloc not enabled:  Redirect the entry points to malloc.  These will
 * only be used by extensions that are compiled with pymalloc enabled. */

Py_ssize_t
_Py_GetAllocatedBlocks(void)
{
    return 0;
}

#endif /* WITH_PYMALLOC */
