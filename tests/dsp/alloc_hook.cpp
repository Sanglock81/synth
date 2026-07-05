#include "alloc_hook.h"
#include <cstdlib>
#include <new>

// Global operator new/delete overrides. Counting is armed PER THREAD via the
// thread_local alloc_hook::armed flag, so only the arming (audio) thread's
// allocations register. Forwards to malloc/free otherwise. Test-only TU.

namespace alloc_hook
{
    std::atomic<std::size_t> newCount    { 0 };
    std::atomic<std::size_t> deleteCount { 0 };
}

// Under AddressSanitizer, do NOT override global new/delete — ASan provides its
// own and needs them for allocation tracking / leak detection. The RT-alloc
// counters then stay zero (AllocGuard sees no bumps), which is fine: the alloc
// guarantee is verified in normal builds; sanitizer builds verify leaks/UB.
#if ! defined(__SANITIZE_ADDRESS__)

namespace
{
    inline void bumpNew()
    {
        if (alloc_hook::armed)
            alloc_hook::newCount.fetch_add (1, std::memory_order_relaxed);
    }
    inline void bumpDelete()
    {
        if (alloc_hook::armed)
            alloc_hook::deleteCount.fetch_add (1, std::memory_order_relaxed);
    }
}

void* operator new (std::size_t sz)
{
    bumpNew();
    if (sz == 0) sz = 1;
    if (void* p = std::malloc (sz)) return p;
    throw std::bad_alloc();
}

void* operator new[] (std::size_t sz)
{
    bumpNew();
    if (sz == 0) sz = 1;
    if (void* p = std::malloc (sz)) return p;
    throw std::bad_alloc();
}

void operator delete (void* p) noexcept                { bumpDelete(); std::free (p); }
void operator delete[] (void* p) noexcept              { bumpDelete(); std::free (p); }
void operator delete (void* p, std::size_t) noexcept   { bumpDelete(); std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { bumpDelete(); std::free (p); }

#endif // ! __SANITIZE_ADDRESS__
