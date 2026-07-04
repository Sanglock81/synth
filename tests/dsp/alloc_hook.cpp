#include "alloc_hook.h"
#include <cstdlib>
#include <new>

// Global operator new/delete overrides. When counting is armed, every heap
// allocation/free bumps a counter. We forward to malloc/free so behaviour is
// otherwise unchanged. This is a test-only translation unit.

namespace alloc_hook
{
    std::atomic<bool>        counting     { false };
    std::atomic<std::size_t> newCount     { 0 };
    std::atomic<std::size_t> deleteCount  { 0 };
}

namespace
{
    inline void bumpNew()
    {
        if (alloc_hook::counting.load (std::memory_order_acquire))
            alloc_hook::newCount.fetch_add (1, std::memory_order_relaxed);
    }
    inline void bumpDelete()
    {
        if (alloc_hook::counting.load (std::memory_order_acquire))
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

void operator delete (void* p) noexcept              { bumpDelete(); std::free (p); }
void operator delete[] (void* p) noexcept            { bumpDelete(); std::free (p); }
void operator delete (void* p, std::size_t) noexcept   { bumpDelete(); std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { bumpDelete(); std::free (p); }
