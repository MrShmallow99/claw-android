/* Guest address space: a 4 GiB host reservation; guest address A lives at g_mem + A.
 *
 * Layout:
 *   0x00000000-0x0000ffff  no access (null page catch)
 *   0x00010000-0x003fffff  TEBs, command line, misc runtime data ("low arena")
 *   0x00400000-0x00c4a000  game exe image (see IMAGE_BASE/IMAGE_SIZE in gen/image_info.h)
 *   0x01000000-0x7fffffff  regions (VirtualAlloc, heap chunks, thread stacks)
 *   0xF0000000-...         host thunk addresses (never accessed as data)
 */
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "runtime.h"

uint8_t *g_mem;

#define REGION_BASE 0x01000000u
#define REGION_END  0x80000000u
#define LOW_BASE    0x00010000u
#define LOW_END     0x00400000u

static pthread_mutex_t g_mem_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_region_top = REGION_BASE;
static uint32_t g_low_top = LOW_BASE;

/* Simple region free list (addr,size), first-fit. */
typedef struct { uint32_t addr, size; } Span;
static Span g_free_spans[4096];
static int g_free_count;

void mem_init(void) {
    void *p = mmap(NULL, 0x100000000ull, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) rt_fatal("cannot reserve 4 GiB guest address space");
    g_mem = p;
    /* everything from 64K up to the region end is read/write; pages are only
     * backed by RAM once touched. */
    if (mprotect(g_mem + LOW_BASE, REGION_END - LOW_BASE, PROT_READ | PROT_WRITE))
        rt_fatal("mprotect guest memory failed");
}

uint32_t mem_low_alloc(uint32_t size) {
    pthread_mutex_lock(&g_mem_lock);
    uint32_t a = (g_low_top + 15) & ~15u;
    g_low_top = a + size;
    if (g_low_top > LOW_END) rt_fatal("low arena exhausted");
    pthread_mutex_unlock(&g_mem_lock);
    memset(GPTR(a), 0, size);
    return a;
}

/* Reserve a fresh, zeroed region. */
uint32_t mem_reserve(uint32_t size, uint32_t align) {
    if (align < 0x1000) align = 0x1000;
    size = (size + 0xfff) & ~0xfffu;
    pthread_mutex_lock(&g_mem_lock);
    for (int i = 0; i < g_free_count; i++) {
        uint32_t a = (g_free_spans[i].addr + align - 1) & ~(align - 1);
        uint32_t end = g_free_spans[i].addr + g_free_spans[i].size;
        if (a + size <= end && a + size > a) {
            uint32_t s0 = g_free_spans[i].addr;
            /* split: keep leading and trailing parts */
            g_free_spans[i] = g_free_spans[--g_free_count];
            if (a > s0 && g_free_count < 4096) g_free_spans[g_free_count++] = (Span){s0, a - s0};
            if (end > a + size && g_free_count < 4096) g_free_spans[g_free_count++] = (Span){a + size, end - a - size};
            pthread_mutex_unlock(&g_mem_lock);
            memset(GPTR(a), 0, size);
            return a;
        }
    }
    uint32_t a = (g_region_top + align - 1) & ~(align - 1);
    if ((uint64_t)a + size > REGION_END) {
        pthread_mutex_unlock(&g_mem_lock);
        return 0;
    }
    g_region_top = a + size;
    pthread_mutex_unlock(&g_mem_lock);
    return a; /* fresh pages are already zero */
}

void mem_release(uint32_t a, uint32_t size) {
    size = (size + 0xfff) & ~0xfffu;
    madvise(g_mem + a, size, MADV_DONTNEED); /* give RAM back; re-reads as zero */
    pthread_mutex_lock(&g_mem_lock);
    if (g_free_count < 4096) g_free_spans[g_free_count++] = (Span){a, size};
    pthread_mutex_unlock(&g_mem_lock);
}

void mem_decommit(uint32_t a, uint32_t size) {
    uint32_t start = a & ~0xfffu;
    uint32_t end = (a + size + 0xfff) & ~0xfffu;
    madvise(g_mem + start, end - start, MADV_DONTNEED);
}

/* ---------------------------------------------------------------------------
 * Heap: size classes (powers of two, 16 B .. 64 KiB) carved from 1 MiB chunks,
 * larger blocks get their own region.  Every block has a 16-byte header:
 *   +0 size requested, +4 class index (or 0xffff for large), +8 region size.
 */
#define HDR 16u
#define NCLASS 13 /* 16 << 12 = 64 KiB */
static uint32_t g_free_list[NCLASS];
static uint32_t g_chunk_ptr, g_chunk_end;

static int size_class(uint32_t n) {
    uint32_t s = 16;
    for (int c = 0; c < NCLASS; c++, s <<= 1)
        if (n + HDR <= s) return c;
    return -1;
}

uint32_t heap_alloc(uint32_t size, int zero) {
    int c = size_class(size);
    uint32_t blk;
    if (c < 0) {
        uint32_t total = (size + HDR + 0xfff) & ~0xfffu;
        blk = mem_reserve(total, 0x1000);
        if (!blk) return 0;
        WR32(blk, size); WR32(blk + 4, 0xffff); WR32(blk + 8, total);
        return blk + HDR;
    }
    uint32_t bs = 16u << c;
    pthread_mutex_lock(&g_mem_lock);
    blk = g_free_list[c];
    if (blk) {
        g_free_list[c] = RD32(blk + 8);
    } else {
        if (g_chunk_ptr + bs > g_chunk_end) {
            pthread_mutex_unlock(&g_mem_lock);
            uint32_t ch = mem_reserve(0x100000, 0x1000);
            if (!ch) return 0;
            pthread_mutex_lock(&g_mem_lock);
            g_chunk_ptr = ch;
            g_chunk_end = ch + 0x100000;
        }
        blk = g_chunk_ptr;
        g_chunk_ptr += bs;
    }
    pthread_mutex_unlock(&g_mem_lock);
    WR32(blk, size); WR32(blk + 4, (uint32_t)c); WR32(blk + 8, 0);
    if (zero) memset(GPTR(blk + HDR), 0, size);
    return blk + HDR;
}

void heap_free(uint32_t p) {
    if (!p) return;
    uint32_t blk = p - HDR;
    uint32_t c = RD32(blk + 4);
    if (c == 0xffff) {
        mem_release(blk, RD32(blk + 8));
        return;
    }
    if (c >= NCLASS) rt_fatal("heap_free: bad block %08x", p);
    pthread_mutex_lock(&g_mem_lock);
    WR32(blk + 8, g_free_list[c]);
    g_free_list[c] = blk;
    pthread_mutex_unlock(&g_mem_lock);
}

uint32_t heap_size(uint32_t p) {
    return p ? RD32(p - HDR) : 0;
}

uint32_t heap_realloc(uint32_t p, uint32_t size) {
    if (!p) return heap_alloc(size, 0);
    uint32_t old = heap_size(p);
    uint32_t c = RD32(p - HDR + 4);
    if (c < NCLASS && size + HDR <= (16u << c)) {
        WR32(p - HDR, size);
        return p;
    }
    uint32_t n = heap_alloc(size, 0);
    if (!n) return 0;
    memcpy(GPTR(n), GPTR(p), old < size ? old : size);
    heap_free(p);
    return n;
}

uint32_t guest_strdup(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t a = heap_alloc(n, 0);
    memcpy(GPTR(a), s, n);
    return a;
}
