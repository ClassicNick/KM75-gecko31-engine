/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Utility_h
#define js_Utility_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Compiler.h"
#include "mozilla/Move.h"
#include "mozilla/NullPtr.h"
#include "mozilla/Scoped.h"
#include "mozilla/TemplateLib.h"

#include <stdlib.h>
#include <string.h>

#ifdef JS_OOM_DO_BACKTRACES
#include <execinfo.h>
#include <stdio.h>
#endif

#include "jstypes.h"

/* The public JS engine namespace. */
namespace JS {}

/* The mozilla-shared reusable template/utility namespace. */
namespace mozilla {}

/* The private JS engine namespace. */
namespace js {}

/*
 * Patterns used by SpiderMonkey to overwrite unused memory. If you are
 * accessing an object with one of these pattern, you probably have a dangling
 * pointer.
 */
#define JS_FRESH_NURSERY_PATTERN 0x2F
#define JS_SWEPT_NURSERY_PATTERN 0x2B
#define JS_ALLOCATED_NURSERY_PATTERN 0x2D
#define JS_FRESH_TENURED_PATTERN 0x4F
#define JS_SWEPT_TENURED_PATTERN 0x4B
#define JS_ALLOCATED_TENURED_PATTERN 0x4D
#define JS_SWEPT_CODE_PATTERN 0x3b
#define JS_SWEPT_FRAME_PATTERN 0x5b

#define JS_ASSERT(expr)           MOZ_ASSERT(expr)
#define JS_ASSERT_IF(cond, expr)  MOZ_ASSERT_IF(cond, expr)

#define JS_STATIC_ASSERT(cond)           static_assert(cond, "JS_STATIC_ASSERT")
#define JS_STATIC_ASSERT_IF(cond, expr)  MOZ_STATIC_ASSERT_IF(cond, expr, "JS_STATIC_ASSERT_IF")

extern MOZ_NORETURN JS_PUBLIC_API(void)
JS_Assert(const char* s, const char* file, int ln);

/*
 * Abort the process in a non-graceful manner. This will cause a core file,
 * call to the debugger or other moral equivalent as well as causing the
 * entire process to stop.
 */
extern JS_PUBLIC_API(void) JS_Abort(void);

/*
 * Custom allocator support for SpiderMonkey
 */
#if defined JS_USE_CUSTOM_ALLOCATOR
# include "jscustomallocator.h"
#else
# if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
/*
 * In order to test OOM conditions, when the testing function
 * oomAfterAllocations COUNT is passed, we fail continuously after the NUM'th
 * allocation from now.
 */
extern JS_PUBLIC_DATA(uint32_t) OOM_maxAllocations; /* set in builtin/TestingFunctions.cpp */
extern JS_PUBLIC_DATA(uint32_t) OOM_counter; /* data race, who cares. */

#ifdef JS_OOM_BREAKPOINT
static MOZ_NEVER_INLINE void js_failedAllocBreakpoint() { asm(""); }
#define JS_OOM_CALL_BP_FUNC() js_failedAllocBreakpoint()
#else
#define JS_OOM_CALL_BP_FUNC() do {} while(0)
#endif

#  define JS_OOM_POSSIBLY_FAIL() \
    do \
    { \
        if (++OOM_counter > OOM_maxAllocations) { \
            JS_OOM_CALL_BP_FUNC();\
            return nullptr; \
        } \
    } while (0)

# else
#  define JS_OOM_POSSIBLY_FAIL() do {} while(0)
# endif /* DEBUG || JS_OOM_BREAKPOINT */

static inline void* js_malloc(size_t bytes)
{
    JS_OOM_POSSIBLY_FAIL();
    return malloc(bytes);
}

static inline void* js_calloc(size_t bytes)
{
    JS_OOM_POSSIBLY_FAIL();
    return calloc(bytes, 1);
}

static inline void* js_calloc(size_t nmemb, size_t size)
{
    JS_OOM_POSSIBLY_FAIL();
    return calloc(nmemb, size);
}

static inline void* js_realloc(void* p, size_t bytes)
{
    JS_OOM_POSSIBLY_FAIL();
    return realloc(p, bytes);
}

static inline void js_free(void* p)
{
    free(p);
}
#endif/* JS_USE_CUSTOM_ALLOCATOR */

#include <new>

/*
 * Low-level memory management in SpiderMonkey:
 *
 *  ** Do not use the standard malloc/free/realloc: SpiderMonkey allows these
 *     to be redefined (via JS_USE_CUSTOM_ALLOCATOR) and Gecko even #define's
 *     these symbols.
 *
 *  ** Do not use the builtin C++ operator new and delete: these throw on
 *     error and we cannot override them not to.
 *
 * Allocation:
 *
 * - If the lifetime of the allocation is tied to the lifetime of a GC-thing
 *   (that is, finalizing the GC-thing will free the allocation), call one of
 *   the following functions:
 *
 *     JSContext::{malloc_,realloc_,calloc_,new_}
 *     JSRuntime::{malloc_,realloc_,calloc_,new_}
 *
 *   These functions accumulate the number of bytes allocated which is used as
 *   part of the GC-triggering heuristic.
 *
 *   The difference between the JSContext and JSRuntime versions is that the
 *   cx version reports an out-of-memory error on OOM. (This follows from the
 *   general SpiderMonkey idiom that a JSContext-taking function reports its
 *   own errors.)
 *
 * - Otherwise, use js_malloc/js_realloc/js_calloc/js_free/js_new
 *
 * Deallocation:
 *
 * - Ordinarily, use js_free/js_delete.
 *
 * - For deallocations during GC finalization, use one of the following
 *   operations on the FreeOp provided to the finalizer:
 *
 *     FreeOp::{free_,delete_}
 *
 *   The advantage of these operations is that the memory is batched and freed
 *   on another thread.
 */

#define JS_NEW_BODY(allocator, t, parms)                                       \
    void* memory = allocator(sizeof(t));                                       \
    return memory ? new(memory) t parms : nullptr;

/*
 * Given a class which should provide 'new' methods, add
 * JS_DECLARE_NEW_METHODS (see JSContext for a usage example). This
 * adds news with up to 12 parameters. Add more versions of new below if
 * you need more than 12 parameters.
 *
 * Note: Do not add a ; at the end of a use of JS_DECLARE_NEW_METHODS,
 * or the build will break.
 */
#define JS_DECLARE_NEW_METHODS(NEWNAME, ALLOCATOR, QUALIFIERS)\
    template <class T>\
    QUALIFIERS T* NEWNAME() MOZ_HEAP_ALLOCATOR {\
        JS_NEW_BODY(ALLOCATOR, T, ())\
    }\
\
    template <class T, class P1>\
    QUALIFIERS T* NEWNAME(P1&& p1) MOZ_HEAP_ALLOCATOR {\
        JS_NEW_BODY(ALLOCATOR, T,\
                    (mozilla::Forward<P1>(p1)))\
    }\
\
    template <class T, class P1, class P2>\
    QUALIFIERS T* NEWNAME(P1&& p1, P2&& p2) MOZ_HEAP_ALLOCATOR {\
        JS_NEW_BODY(ALLOCATOR, T,\
                    (mozilla::Forward<P1>(p1),\
                     mozilla::Forward<P2>(p2)))\
    }\
\
    template <class T, class P1, class P2, class P3>\
    QUALIFIERS T* NEWNAME(P1&& p1, P2&& p2, P3&& p3) MOZ_HEAP_ALLOCATOR {\
        JS_NEW_BODY(ALLOCATOR, T,\
                    (mozilla::Forward<P1>(p1),\
                     mozilla::Forward<P2>(p2),\
                     mozilla::Forward<P3>(p3)))\
    }\
\
    template <class T, class P1, class P2, class P3, class P4>\
    QUALIFIERS T* NEWNAME(P1&& p1, P2&& p2, P3&& p3, P4&& p4) MOZ_HEAP_ALLOCATOR {\
        JS_NEW_BODY(ALLOCATOR, T,\
                    (mozilla::Forward<P1>(p1),\
                     mozilla::Forward<P2>(p2),\
                     mozilla::Forward<P3>(p3),\
                     mozilla::Forward<P4>(p4)))\
    }\
\
    template <class T, class P1, class P2, class P3, class P4, class P5>\
    QUALIFIERS T* NEWNAME(P1&& p1, P2&& p2, P3&& p3, P4&& p4, P5&& p5) MOZ_HEAP_ALLOCATOR {\
        JS_NEW_BODY(ALLOCATOR, T,\
                    (mozilla::Forward<P1>(p1),\
                     mozilla::Forward<P2>(p2),\
                     mozilla::Forward<P3>(p3),\
                     mozilla::Forward<P4>(p4),\
                     mozilla::Forward<P5>(p5)))\
    }\
\
    template <class T, class P1, class P2, class P3, class P4, class P5, class P6>\
    QUALIFIERS T* NEWNAME(P1&& p1, P2&& p2, P3&& p3, P4&& p4, P5&& p5, P6&& p6) MOZ_HEAP_ALLOCATOR {\
        JS_NEW_BODY(ALLOCATOR, T,\
                    (mozilla::Forward<P1>(p1),\
                     mozilla::Forward<P2>(p2),\
                     mozilla::Forward<P3>(p3),\
                     mozilla::Forward<P4>(p4),\
                     mozilla::Forward<P5>(p5),\
                     mozilla::Forward<P6>(p6)))\
    }\
\
    template <class T, class P1, class P2, class P3, class P4, class P5, class P6, class P7>\
    QUALIFIERS T* NEWNAME(P1&& p1, P2&& p2, P3&& p3, P4&& p4, P5&& p5, P6&& p6, P7&& p7) MOZ_HEAP_ALLOCATOR {\
        JS_NEW_BODY(ALLOCATOR, T,\
                    (mozilla::Forward<P1>(p1),\
                     mozilla::Forward<P2>(p2),\
                     mozilla::Forward<P3>(p3),\
                     mozilla::Forward<P4>(p4),\
                     mozilla::Forward<P5>(p5),\
                     mozilla::Forward<P6>(p6),\
                     mozilla::Forward<P7>(p7)))\
    }\
\
    template <class T, class P1, class P2, class P3, class P4, class P5, class P6, class P7, class P8>\
    QUALIFIERS T* NEWNAME(P1&& p1, P2&& p2, P3&& p3, P4&& p4, P5&& p5, P6&& p6, P7&& p7, P8&& p8) MOZ_HEAP_ALLOCATOR {\
        JS_NEW_BODY(ALLOCATOR, T,\
                    (mozilla::Forward<P1>(p1),\
                     mozilla::Forward<P2>(p2),\
                     mozilla::Forward<P3>(p3),\
                     mozilla::Forward<P4>(p4),\
                     mozilla::Forward<P5>(p5),\
                     mozilla::Forward<P6>(p6),\
                     mozilla::Forward<P7>(p7),\
                     mozilla::Forward<P8>(p8)))\
    }\
\
    template <class T, class P1, class P2, class P3, class P4, class P5, class P6, class P7, class P8, class P9>\
    QUALIFIERS T* NEWNAME(P1&& p1, P2&& p2, P3&& p3, P4&& p4, P5&& p5, P6&& p6, P7&& p7, P8&& p8, P9&& p9) MOZ_HEAP_ALLOCATOR {\
        JS_NEW_BODY(ALLOCATOR, T,\
                    (mozilla::Forward<P1>(p1),\
                     mozilla::Forward<P2>(p2),\
                     mozilla::Forward<P3>(p3),\
                     mozilla::Forward<P4>(p4),\
                     mozilla::Forward<P5>(p5),\
                     mozilla::Forward<P6>(p6),\
                     mozilla::Forward<P7>(p7),\
                     mozilla::Forward<P8>(p8),\
                     mozilla::Forward<P9>(p9)))\
    }\
\
    template <class T, class P1, class P2, class P3, class P4, class P5, class P6, class P7, class P8, class P9, class P10>\
    QUALIFIERS T* NEWNAME(P1&& p1, P2&& p2, P3&& p3, P4&& p4, P5&& p5, P6&& p6, P7&& p7, P8&& p8, P9&& p9, P10&& p10) MOZ_HEAP_ALLOCATOR {\
        JS_NEW_BODY(ALLOCATOR, T,\
                    (mozilla::Forward<P1>(p1),\
                     mozilla::Forward<P2>(p2),\
                     mozilla::Forward<P3>(p3),\
                     mozilla::Forward<P4>(p4),\
                     mozilla::Forward<P5>(p5),\
                     mozilla::Forward<P6>(p6),\
                     mozilla::Forward<P7>(p7),\
                     mozilla::Forward<P8>(p8),\
                     mozilla::Forward<P9>(p9),\
                     mozilla::Forward<P10>(p10)))\
    }\
\
    template <class T, class P1, class P2, class P3, class P4, class P5, class P6, class P7, class P8, class P9, class P10, class P11>\
    QUALIFIERS T* NEWNAME(P1&& p1, P2&& p2, P3&& p3, P4&& p4, P5&& p5, P6&& p6, P7&& p7, P8&& p8, P9&& p9, P10&& p10, P11&& p11) MOZ_HEAP_ALLOCATOR {\
        JS_NEW_BODY(ALLOCATOR, T,\
                    (mozilla::Forward<P1>(p1),\
                     mozilla::Forward<P2>(p2),\
                     mozilla::Forward<P3>(p3),\
                     mozilla::Forward<P4>(p4),\
                     mozilla::Forward<P5>(p5),\
                     mozilla::Forward<P6>(p6),\
                     mozilla::Forward<P7>(p7),\
                     mozilla::Forward<P8>(p8),\
                     mozilla::Forward<P9>(p9),\
                     mozilla::Forward<P10>(p10),\
                     mozilla::Forward<P11>(p11)))\
    }\
\
    template <class T, class P1, class P2, class P3, class P4, class P5, class P6, class P7, class P8, class P9, class P10, class P11, class P12>\
    QUALIFIERS T* NEWNAME(P1&& p1, P2&& p2, P3&& p3, P4&& p4, P5&& p5, P6&& p6, P7&& p7, P8&& p8, P9&& p9, P10&& p10, P11&& p11, P12&& p12) MOZ_HEAP_ALLOCATOR {\
        JS_NEW_BODY(ALLOCATOR, T,\
                    (mozilla::Forward<P1>(p1),\
                     mozilla::Forward<P2>(p2),\
                     mozilla::Forward<P3>(p3),\
                     mozilla::Forward<P4>(p4),\
                     mozilla::Forward<P5>(p5),\
                     mozilla::Forward<P6>(p6),\
                     mozilla::Forward<P7>(p7),\
                     mozilla::Forward<P8>(p8),\
                     mozilla::Forward<P9>(p9),\
                     mozilla::Forward<P10>(p10),\
                     mozilla::Forward<P11>(p11),\
                     mozilla::Forward<P12>(p12)))\
    }\

JS_DECLARE_NEW_METHODS(js_new, js_malloc, static MOZ_ALWAYS_INLINE)

template <class T>
static MOZ_ALWAYS_INLINE void
js_delete(T* p)
{
    if (p) {
        p->~T();
        js_free(p);
    }
}

template<class T>
static MOZ_ALWAYS_INLINE void
js_delete_poison(T* p)
{
    if (p) {
        p->~T();
        memset(p, 0x3B, sizeof(T));
        js_free(p);
    }
}

template <class T>
static MOZ_ALWAYS_INLINE T*
js_pod_malloc()
{
    return (T*)js_malloc(sizeof(T));
}

template <class T>
static MOZ_ALWAYS_INLINE T*
js_pod_calloc()
{
    return (T*)js_calloc(sizeof(T));
}

template <class T>
static MOZ_ALWAYS_INLINE T*
js_pod_malloc(size_t numElems)
{
    if (numElems & mozilla::tl::MulOverflowMask<sizeof(T)>::value)
        return nullptr;
    return (T*)js_malloc(numElems * sizeof(T));
}

template <class T>
static MOZ_ALWAYS_INLINE T*
js_pod_calloc(size_t numElems)
{
    if (numElems & mozilla::tl::MulOverflowMask<sizeof(T)>::value)
        return nullptr;
    return (T*)js_calloc(numElems * sizeof(T));
}

namespace js {

template<typename T>
struct ScopedFreePtrTraits
{
    typedef T* type;
    static T* empty() { return nullptr; }
    static void release(T* ptr) { js_free(ptr); }
};
SCOPED_TEMPLATE(ScopedJSFreePtr, ScopedFreePtrTraits)

template <typename T>
struct ScopedDeletePtrTraits : public ScopedFreePtrTraits<T>
{
    static void release(T* ptr) { js_delete(ptr); }
};
SCOPED_TEMPLATE(ScopedJSDeletePtr, ScopedDeletePtrTraits)

template <typename T>
struct ScopedReleasePtrTraits : public ScopedFreePtrTraits<T>
{
    static void release(T* ptr) { if (ptr) ptr->release(); }
};
SCOPED_TEMPLATE(ScopedReleasePtr, ScopedReleasePtrTraits)

} /* namespace js */

namespace js {

/* Integral types for all hash functions. */
typedef uint32_t HashNumber;
const unsigned HashNumberSizeBits = 32;

namespace detail {

/*
 * Given a raw hash code, h, return a number that can be used to select a hash
 * bucket.
 *
 * This function aims to produce as uniform an output distribution as possible,
 * especially in the most significant (leftmost) bits, even though the input
 * distribution may be highly nonrandom, given the constraints that this must
 * be deterministic and quick to compute.
 *
 * Since the leftmost bits of the result are best, the hash bucket index is
 * computed by doing ScrambleHashCode(h) / (2^32/N) or the equivalent
 * right-shift, not ScrambleHashCode(h) % N or the equivalent bit-mask.
 *
 * FIXME: OrderedHashTable uses a bit-mask; see bug 775896.
 */
inline HashNumber
ScrambleHashCode(HashNumber h)
{
    /*
     * Simply returning h would not cause any hash tables to produce wrong
     * answers. But it can produce pathologically bad performance: The caller
     * right-shifts the result, keeping only the highest bits. The high bits of
     * hash codes are very often completely entropy-free. (So are the lowest
     * bits.)
     *
     * So we use Fibonacci hashing, as described in Knuth, The Art of Computer
     * Programming, 6.4. This mixes all the bits of the input hash code h.
     * 
     * The value of goldenRatio is taken from the hex
     * expansion of the golden ratio, which starts 1.9E3779B9....
     * This value is especially good if values with consecutive hash codes
     * are stored in a hash table; see Knuth for details.
     */
    static const HashNumber goldenRatio = 0x9E3779B9U;
    return h * goldenRatio;
}

} /* namespace detail */

} /* namespace js */

namespace JS {

/*
 * Methods for poisoning GC heap pointer words and checking for poisoned words.
 * These are in this file for use in Value methods and so forth.
 *
 * If the moving GC hazard analysis is in use and detects a non-rooted stack
 * pointer to a GC thing, one byte of that pointer is poisoned to refer to an
 * invalid location. For both 32 bit and 64 bit systems, the fourth byte of the
 * pointer is overwritten, to reduce the likelihood of accidentally changing
 * a live integer value.
 */

inline void PoisonPtr(void* v)
{
#if defined(JSGC_ROOT_ANALYSIS) && defined(JS_DEBUG)
    uint8_t* ptr = (uint8_t*) v + 3;
    *ptr = JS_FREE_PATTERN;
#endif
}

template <typename T>
inline bool IsPoisonedPtr(T* v)
{
#if defined(JSGC_ROOT_ANALYSIS) && defined(JS_DEBUG)
    uint32_t mask = uintptr_t(v) & 0xff000000;
    return mask == uint32_t(JS_FREE_PATTERN << 24);
#else
    return false;
#endif
}

}

/* sixgill annotation defines */
#ifndef HAVE_STATIC_ANNOTATIONS
# define HAVE_STATIC_ANNOTATIONS
# ifdef XGILL_PLUGIN
#  define STATIC_PRECONDITION(COND)         __attribute__((precondition(#COND)))
#  define STATIC_PRECONDITION_ASSUME(COND)  __attribute__((precondition_assume(#COND)))
#  define STATIC_POSTCONDITION(COND)        __attribute__((postcondition(#COND)))
#  define STATIC_POSTCONDITION_ASSUME(COND) __attribute__((postcondition_assume(#COND)))
#  define STATIC_INVARIANT(COND)            __attribute__((invariant(#COND)))
#  define STATIC_INVARIANT_ASSUME(COND)     __attribute__((invariant_assume(#COND)))
#  define STATIC_PASTE2(X,Y) X ## Y
#  define STATIC_PASTE1(X,Y) STATIC_PASTE2(X,Y)
#  define STATIC_ASSERT(COND)                        \
  JS_BEGIN_MACRO                                     \
    __attribute__((assert_static(#COND), unused))    \
    int STATIC_PASTE1(assert_static_, __COUNTER__);  \
  JS_END_MACRO
#  define STATIC_ASSUME(COND)                        \
  JS_BEGIN_MACRO                                     \
    __attribute__((assume_static(#COND), unused))    \
    int STATIC_PASTE1(assume_static_, __COUNTER__);  \
  JS_END_MACRO
#  define STATIC_ASSERT_RUNTIME(COND)                       \
  JS_BEGIN_MACRO                                            \
    __attribute__((assert_static_runtime(#COND), unused))   \
    int STATIC_PASTE1(assert_static_runtime_, __COUNTER__); \
  JS_END_MACRO
# else /* XGILL_PLUGIN */
#  define STATIC_PRECONDITION(COND)          /* nothing */
#  define STATIC_PRECONDITION_ASSUME(COND)   /* nothing */
#  define STATIC_POSTCONDITION(COND)         /* nothing */
#  define STATIC_POSTCONDITION_ASSUME(COND)  /* nothing */
#  define STATIC_INVARIANT(COND)             /* nothing */
#  define STATIC_INVARIANT_ASSUME(COND)      /* nothing */
#  define STATIC_ASSERT(COND)          JS_BEGIN_MACRO /* nothing */ JS_END_MACRO
#  define STATIC_ASSUME(COND)          JS_BEGIN_MACRO /* nothing */ JS_END_MACRO
#  define STATIC_ASSERT_RUNTIME(COND)  JS_BEGIN_MACRO /* nothing */ JS_END_MACRO
# endif /* XGILL_PLUGIN */
# define STATIC_SKIP_INFERENCE STATIC_INVARIANT(skip_inference())
#endif /* HAVE_STATIC_ANNOTATIONS */

#endif /* js_Utility_h */