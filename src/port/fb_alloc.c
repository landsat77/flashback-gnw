/*
 * Small general purpose heap for the Flashback homebrew, placed in the
 * RAM_EMU area left free after the binary's BSS (the retro-go allocators are
 * bump allocators without free()).
 *
 * Boundary-tag allocator: every block has a 4 byte header and a 4 byte footer
 * holding its size (multiple of 8) with bit 0 = "in use". Free blocks are kept
 * in a doubly linked list (first fit) and are coalesced with their neighbours
 * on free(). fb_free() silently ignores pointers that do not belong to the heap
 * (resources used in place from the memory-mapped pack).
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "fb_platform.h"

#define ALIGN      8u
#define HDR        4u
#define MIN_BLOCK  24u   /* header + 2 links + footer, rounded to ALIGN */
#define USED       1u

typedef struct free_node {
    struct free_node *prev;
    struct free_node *next;
} free_node_t;

static uint8_t *heap_lo;     /* first block header */
static uint8_t *heap_hi;     /* end of the last block (sentinel header lives here) */
static free_node_t *free_list;
static size_t heap_used, heap_peak, heap_total;

static inline uint32_t blk_size(uint8_t *b)            { return *(uint32_t *)b & ~7u; }
static inline int      blk_used(uint8_t *b)            { return *(uint32_t *)b & USED; }
static inline void     blk_set(uint8_t *b, uint32_t s, int used)
{
    *(uint32_t *)b = s | (used ? USED : 0);
    *(uint32_t *)(b + s - HDR) = s | (used ? USED : 0);
}
static inline free_node_t *blk_node(uint8_t *b)        { return (free_node_t *)(b + HDR); }
static inline uint8_t *node_blk(free_node_t *n)        { return (uint8_t *)n - HDR; }

static void list_remove(free_node_t *n)
{
    if (n->prev) n->prev->next = n->next; else free_list = n->next;
    if (n->next) n->next->prev = n->prev;
}

static void list_push(free_node_t *n)
{
    n->prev = NULL;
    n->next = free_list;
    if (free_list) free_list->prev = n;
    free_list = n;
}

#ifdef FB_HOST_SYSTEM_MALLOC
/* Host debugging: use the system allocator so that AddressSanitizer can see
 * every block. Pointers inside [ignore_lo, ignore_hi) (the asset pack) are
 * not freed, like on the device. */
#include <stdlib.h>
static const uint8_t *ignore_lo, *ignore_hi;
void fb_heap_ignore_range(const void *lo, const void *hi) { ignore_lo = lo; ignore_hi = hi; }
void fb_heap_init(void *base, size_t size) { (void)base; heap_total = size; }
void *fb_malloc(size_t size) { return malloc(size ? size : 1); }
void fb_free(void *ptr)
{
    if ((const uint8_t *)ptr >= ignore_lo && (const uint8_t *)ptr < ignore_hi)
        return;
    free(ptr);
}
void *fb_calloc(size_t n, size_t size) { return calloc(n ? n : 1, size ? size : 1); }
void *fb_realloc(void *ptr, size_t size)
{
    if ((const uint8_t *)ptr >= ignore_lo && (const uint8_t *)ptr < ignore_hi) {
        void *n = malloc(size);
        memcpy(n, ptr, size);
        return n;
    }
    return realloc(ptr, size);
}
size_t fb_heap_used(void) { return 0; }
size_t fb_heap_peak(void) { return 0; }
size_t fb_heap_size(void) { return heap_total; }
#else
void fb_heap_ignore_range(const void *lo, const void *hi) { (void)lo; (void)hi; }

void fb_heap_init(void *base, size_t size)
{
    uintptr_t lo = ((uintptr_t)base + ALIGN - 1) & ~(uintptr_t)(ALIGN - 1);
    /* Payloads must be 8 aligned: headers sit 4 bytes before an 8 boundary. */
    lo += ALIGN - HDR;
    uintptr_t hi = ((uintptr_t)base + size) & ~(uintptr_t)(ALIGN - 1);
    hi -= HDR;                       /* room for the end sentinel */
    uint32_t total = (uint32_t)((hi - lo) & ~(uintptr_t)(ALIGN - 1));
    heap_lo = (uint8_t *)lo;
    heap_hi = heap_lo + total;
    *(uint32_t *)heap_hi = USED;     /* sentinel: size 0, used */
    free_list = NULL;
    blk_set(heap_lo, total, 0);
    list_push(blk_node(heap_lo));
    heap_used = 0;
    heap_peak = 0;
    heap_total = total;
}

static inline int in_heap(const void *p)
{
    return (const uint8_t *)p > heap_lo && (const uint8_t *)p < heap_hi;
}

#ifdef FB_HEAP_TRACE
static void *fb_malloc_impl(size_t size);
void *fb_malloc(size_t size)
{
    void *p = fb_malloc_impl(size);
    if (size >= 2048)
        printf("heap: +%zu at %p (used %zu)\n", size, p, heap_used);
    return p;
}
#define fb_malloc fb_malloc_impl
#endif
void *fb_malloc(size_t size)
{
    if (heap_lo == NULL)
        return NULL;
    if (size == 0)
        size = 1;
    uint32_t need = (uint32_t)((size + 2 * HDR + ALIGN - 1) & ~(size_t)(ALIGN - 1));
    if (need < MIN_BLOCK)
        need = MIN_BLOCK;
    for (free_node_t *n = free_list; n; n = n->next) {
        uint8_t *b = node_blk(n);
        uint32_t s = blk_size(b);
        if (s < need)
            continue;
        list_remove(n);
        if (s - need >= MIN_BLOCK) {
            uint8_t *rest = b + need;
            blk_set(rest, s - need, 0);
            list_push(blk_node(rest));
            s = need;
        }
        blk_set(b, s, 1);
        heap_used += s;
#ifdef FB_ALLOC_TRACE
        if (size >= 8192)
            printf("fb_malloc(%u) -> %p used %u\n", (unsigned)size, (void *)(b + HDR), (unsigned)heap_used);
#endif
        if (heap_used > heap_peak)
            heap_peak = heap_used;
        return b + HDR;
    }
    printf("fb_malloc: out of memory (%u bytes requested, %u used / %u)\n",
           (unsigned)size, (unsigned)heap_used, (unsigned)heap_total);
    return NULL;
}

void fb_free(void *ptr)
{
    if (ptr == NULL || !in_heap(ptr))
        return;
    uint8_t *b = (uint8_t *)ptr - HDR;
    if (!blk_used(b)) {
        printf("fb_free: double free %p\n", ptr);
        return;
    }
    uint32_t s = blk_size(b);
    heap_used -= s;
#ifdef FB_ALLOC_TRACE
    if (s >= 8192)
        printf("fb_free(%p) size %u used %u\n", ptr, (unsigned)s, (unsigned)heap_used);
#endif
    /* merge with next */
    uint8_t *next = b + s;
    if (next < heap_hi && !blk_used(next)) {
        list_remove(blk_node(next));
        s += blk_size(next);
    }
    /* merge with previous */
    if (b > heap_lo) {
        uint32_t ptag = *(uint32_t *)(b - HDR);
        if (!(ptag & USED)) {
            uint8_t *prev = b - (ptag & ~7u);
            list_remove(blk_node(prev));
            s += blk_size(prev);
            b = prev;
        }
    }
    blk_set(b, s, 0);
    list_push(blk_node(b));
}

void *fb_calloc(size_t n, size_t size)
{
    size_t total = n * size;
    void *p = fb_malloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

void *fb_realloc(void *ptr, size_t size)
{
    if (ptr == NULL)
        return fb_malloc(size);
    if (size == 0) {
        fb_free(ptr);
        return NULL;
    }
    size_t old = 0;
    if (in_heap(ptr)) {
        old = blk_size((uint8_t *)ptr - HDR) - 2 * HDR;
        if (old >= size)
            return ptr;
    }
    void *np = fb_malloc(size);
    if (np == NULL)
        return NULL;
    /* Pointers outside the heap (pack data): copy what was asked for. */
    memcpy(np, ptr, old ? old : size);
    fb_free(ptr);
    return np;
}

size_t fb_heap_used(void) { return heap_used; }
size_t fb_heap_peak(void) { return heap_peak; }
size_t fb_heap_size(void) { return heap_total; }
#endif /* FB_HOST_SYSTEM_MALLOC */
