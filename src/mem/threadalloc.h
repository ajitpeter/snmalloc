#pragma once

#include "../ds/helpers.h"
#include "globalalloc.h"
#if defined(SNMALLOC_USE_THREAD_DESTRUCTOR) && \
  defined(SNMALLOC_USE_THREAD_CLEANUP)
#error At most one out of SNMALLOC_USE_THREAD_CLEANUP and SNMALLOC_USE_THREAD_DESTRUCTOR may be defined.
#endif

#if !defined(_WIN32) && !defined(FreeBSD_KERNEL)
#  include "pthread.h"
#endif

namespace snmalloc
{
  extern "C" void _malloc_thread_cleanup(void);

  /**
   * A global fake allocator object.  This never allocates memory and, as a
   * result, never owns any slabs.  On the slow paths, where it would fetch
   * slabs to allocate from, it will discover that it is the placeholder and
   * replace itself with the thread-local allocator, allocating one if
   * required.  This avoids a branch on the fast path.
   */
  HEADER_GLOBAL Alloc GlobalPlaceHolder(
    default_memory_provider, SNMALLOC_DEFAULT_PAGEMAP(), nullptr, true);

#ifdef SNMALLOC_EXTERNAL_THREAD_ALLOC
  /**
   * Version of the `ThreadAlloc` interface that does no management of thread
   * local state, and just assumes that "ThreadAllocUntyped::get" has been
   * declared before including snmalloc.h.  As it is included before, it cannot
   * know the allocator type, hence the casting.
   *
   * This class is used only when snmalloc is compiled as part of a runtime,
   * which has its own management of the thread local allocator pointer.
   */
  class ThreadAllocUntypedWrapper
  {
  public:
    static SNMALLOC_FAST_PATH Alloc*& get()
    {
      return (Alloc*&)ThreadAllocUntyped::get();
    }
    static void register_cleanup() {}
  };
#endif

  /**
   * Version of the `ThreadAlloc` interface that uses a hook provided by libc
   * to destroy thread-local state.  This is the ideal option, because it
   * enforces ordering of destruction such that the malloc state is destroyed
   * after anything that can allocate memory.
   *
   * This class is used only when snmalloc is compiled as part of a compatible
   * libc (for example, FreeBSD libc).
   */
  class ThreadAllocLibcCleanup
  {
    /**
     * Libc will call `_malloc_thread_cleanup` just before a thread terminates.
     * This function must be allowed to call back into this class to destroy
     * the state.
     */
    friend void _malloc_thread_cleanup(void);

    /**
     * Function called when the thread exits.  This is guaranteed to be called
     * precisely once per thread and releases the current allocator.
     */
    static inline void exit()
    {
      auto* per_thread = get();
      if ((per_thread != &GlobalPlaceHolder) && (per_thread != nullptr))
      {
        current_alloc_pool()->release(per_thread);
        per_thread = nullptr;
      }
    }

  public:
    /**
     * Returns a pointer to the allocator associated with this thread.  If
     * `create` is true, it will create an allocator if one does not exist,
     * otherwise it will return `nullptr` in this case.  This should be called
     * with `create == false` only during thread teardown.
     *
     * The non-create case exists so that the `per_thread` variable can be a
     * local static and not a global, allowing ODR to deduplicate it.
     */
    static SNMALLOC_FAST_PATH Alloc*& get()
    {
      static thread_local Alloc* per_thread = &GlobalPlaceHolder;
      return per_thread;
    }
    static void register_cleanup() {}
  };
  /**
   * Version of the `ThreadAlloc` interface that uses C++ `thread_local`
   * destructors for cleanup.  If a per-thread allocator is used during the
   * destruction of other per-thread data, this class will create a new
   * instance and register its destructor, so should eventually result in
   * cleanup, but may result in allocators being returned to the global pool
   * and then reacquired multiple times.
   *
   * This implementation depends on nothing outside of a working C++
   * environment and so should be the simplest for initial bringup on an
   * unsupported platform.  It is currently used in the FreeBSD kernel version.
   */
  class ThreadAllocThreadDestructor
  {
    /**
     * A pointer to the allocator owned by this thread.
     */
    Alloc* alloc;

    /**
     * Constructor.  Acquires a new allocator and associates it with this
     * object.  There should be only one instance of this class per thread.
     */
    ThreadAllocThreadDestructor() : alloc(&GlobalPlaceHolder) {}

    /**
     * Destructor.  Releases the allocator owned by this thread.
     */
    ~ThreadAllocThreadDestructor()
    {
      current_alloc_pool()->release(alloc);
    }

  public:
    /**
     * Public interface, returns the allocator for this thread, constructing
     * one if necessary.
     */
    static inline Alloc*& get()
    {
      static thread_local ThreadAllocThreadDestructor per_thread;
      return per_thread.alloc;
    }
    static void register_cleanup() {}
  };
  // When targeting the FreeBSD kernel, the pthread header exists, but the
  // pthread symbols do not, so don't compile this because it will fail to
  // link.
#ifndef FreeBSD_KERNEL
  /**
   * Version of the `ThreadAlloc` interface that uses thread-specific (POSIX
   * threads) or Fiber-local (Windows) storage with an explicit destructor.
   * Neither of the underlying mechanisms guarantee ordering, so the cleanup
   * may be called before other cleanup functions or thread-local destructors.
   *
   * This implementation is used when using snmalloc as a library
   * implementation of malloc, but not embedding it in C standard library.
   * Using this implementation removes the dependency on a C++ runtime library.
   */
  class ThreadAllocExplicitTLSCleanup
  {
    /**
     * Cleanup function.  This is registered with the operating system's
     * thread- or fibre-local storage subsystem to clean up the per-thread
     * allocator.
     */
    static inline void
#  ifdef _WIN32
      NTAPI
#  endif
      thread_alloc_release(void* p)
    {
      // Keep pthreads happy
      void** pp = reinterpret_cast<void**>(p);
      *pp = nullptr;
      // Actually destroy the allocator and reset TLS
      Alloc*& alloc = ThreadAllocExplicitTLSCleanup::get();
      current_alloc_pool()->release(alloc);
      alloc = &GlobalPlaceHolder;
    }

#  ifdef _WIN32
    /**
     * Key type used to identify fibre-local storage.
     */
    using tls_key_t = DWORD;

    /**
     * On Windows, construct a new fibre-local storage allocation.  This
     * function must not be called more than once.
     */
    static inline tls_key_t tls_key_create() noexcept
    {
      return FlsAlloc(thread_alloc_release);
    }

    /**
     * On Windows, store a pointer to a `thread_local` pointer to an allocator
     * into fibre-local storage.  This function takes a pointer to the
     * `thread_local` allocation, rather than to the pointee, so that the
     * cleanup function can zero the pointer.
     *
     * This must not be called until after `tls_key_create` has returned.
     */
    static inline void tls_set_value(tls_key_t key, Alloc* value)
    {
      FlsSetValue(key, value);
    }
#  else
    /**
     * Key type used for thread-specific storage.
     */
    using tls_key_t = pthread_key_t;

    /**
     * On POSIX systems, construct a new thread-specific storage allocation.
     * This function must not be called more than once.
     */
    static inline tls_key_t tls_key_create() noexcept
    {
      tls_key_t key;
      pthread_key_create(&key, thread_alloc_release);
      return key;
    }

    /**
     * On POSIX systems, store a pointer to a `thread_local` pointer to an
     * allocator into fibre-local storage.  This function takes a pointer to
     * the `thread_local` allocation, rather than to the pointee, so that the
     * cleanup function can zero the pointer.
     *
     * This must not be called until after `tls_key_create` has returned.
     */
    static inline void tls_set_value(tls_key_t key, Alloc* value)
    {
      pthread_setspecific(key, value);
    }
#  endif

    /**
     * Private accessor to the per thread allocator
     * Provides no checking for initialization
     */
    static SNMALLOC_FAST_PATH Alloc*& inner_get()
    {
      static thread_local Alloc* per_thread = &GlobalPlaceHolder;
      return per_thread;
    }

#  ifdef USE_SNMALLOC_STATS
    static void print_stats()
    {
      Stats s;
      current_alloc_pool()->aggregate_stats(s);
      s.print<Alloc>(std::cout);
    }
#  endif

  public:
    /**
     * Public interface, returns the allocator for the current thread,
     * constructing it if necessary.
     */
    static SNMALLOC_FAST_PATH Alloc*& get()
    {
      return inner_get();
    }
    static void register_cleanup()
    {
      // Register the allocator destructor.
      bool first = false;
      tls_key_t key = Singleton<tls_key_t, tls_key_create>::get(&first);
      // Associate the new allocator with the destructor.
      tls_set_value(key, &GlobalPlaceHolder);

#  ifdef USE_SNMALLOC_STATS
      // Allocator is up and running now, safe to call atexit.
      if (first)
      {
        atexit(print_stats);
      }
#  else
      UNUSED(first);
#  endif
    }
  };
#endif

#ifdef SNMALLOC_USE_THREAD_CLEANUP
  /**
   * Entry point the allows libc to call into the allocator for per-thread
   * cleanup.
   */
  extern "C" void _malloc_thread_cleanup(void)
  {
    ThreadAllocLibcCleanup::exit();
  }
  using ThreadAlloc = ThreadAllocLibcCleanup;
#elif defined(SNMALLOC_USE_THREAD_DESTRUCTOR)
  using ThreadAlloc = ThreadAllocThreadDestructor;
#elif defined(SNMALLOC_EXTERNAL_THREAD_ALLOC)
  using ThreadAlloc = ThreadAllocUntypedWrapper;
#else
  using ThreadAlloc = ThreadAllocExplicitTLSCleanup;
#endif

  /**
   * Slow path for the placeholder replacement.  The simple check that this is
   * the global placeholder is inlined, the rest of it is only hit in a very
   * unusual case and so should go off the fast path.
   */
  SNMALLOC_SLOW_PATH inline void* lazy_replacement_slow()
  {
    auto*& local_alloc = ThreadAlloc::get();
    if ((local_alloc != nullptr) && (local_alloc != &GlobalPlaceHolder))
    {
      return local_alloc;
    }
    local_alloc = current_alloc_pool()->acquire();
    ThreadAlloc::register_cleanup();
    return local_alloc;
  }

  /**
   * Function passed as a template parameter to `Allocator` to allow lazy
   * replacement.  This is called on all of the slow paths in `Allocator`.  If
   * the caller is the global placeholder allocator then this function will
   * check if we've already allocated a per-thread allocator, returning it if
   * so.  If we have not allocated a per-thread allocator yet, then this
   * function will allocate one.
   */
  ALWAYSINLINE void* lazy_replacement(void* existing)
  {
    if (existing != &GlobalPlaceHolder)
    {
      return nullptr;
    }
    return lazy_replacement_slow();
  }

} // namespace snmalloc
