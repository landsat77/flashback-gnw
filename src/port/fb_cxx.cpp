/*
 * C++ runtime of the homebrew (replaces sdk/src/gw_core_cxx_support.cpp,
 * filtered out of the link by the Makefile): operator new / delete use the
 * game heap (fb_alloc.c). The SDK version is a bump allocator without free,
 * and REminiscence allocates and frees objects all the time (a File object
 * for every resource it loads).
 */
#include <cstddef>
#include <cstdlib>
#include "fb_platform.h"

static void *alloc(size_t s)
{
    void *p = fb_malloc(s ? s : 1);
#ifndef FB_DEVICE
    // harness: the C++ runtime allocates before the game heap exists
    if (!p && fb_heap_size() == 0)
        p = (std::malloc)(s ? s : 1); // () : not the malloc macro of the glue
#endif
    return p;
}

void *operator new(size_t s) { return alloc(s); }
void *operator new[](size_t s) { return alloc(s); }
void operator delete(void *p) noexcept { fb_free(p); }
void operator delete[](void *p) noexcept { fb_free(p); }
void operator delete(void *p, size_t) noexcept { fb_free(p); }
void operator delete[](void *p, size_t) noexcept { fb_free(p); }

#ifdef FB_DEVICE
extern "C" void abort(void);

extern "C" void __cxa_pure_virtual()
{
    fb_plat_fatal("pure virtual call");
}

extern "C" {
void *__dso_handle = (void *)&__dso_handle;
}

extern "C" int __cxa_atexit(void (*)(void *), void *, void *)
{
    return 0;
}

extern "C" void *__cxa_allocate_exception(size_t) { abort(); return nullptr; }
extern "C" void  __cxa_throw(void *, void *, void (*)(void *)) { abort(); }
extern "C" int __gxx_personality_v0(int, int, unsigned long long, void *, void *)
{
    abort();
    return 0;
}

namespace __gnu_cxx {
void __verbose_terminate_handler() { abort(); }
}
#endif
