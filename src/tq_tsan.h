#ifndef TQ_TSAN_H
#define TQ_TSAN_H

#include <glib.h>

// GLib implements GMutex directly on futexes rather than on pthread
// primitives, and ThreadSanitizer only intercepts the pthread ones.  It
// therefore cannot see the happens-before edge a g_mutex_lock/unlock pair
// establishes, and reports every correctly-locked handoff -- the DBR cache
// and the intern table both publish heap objects this way -- as a data race.
//
// tq_mutex_lock/unlock annotate the edge explicitly so TSan can model it.
// Verified 2026-08-13: without them the prefetch self-test reports 2 races
// per run; with them, none, and swapping the GMutex for a pthread_mutex_t
// gives the same clean result.  Suppressing instead would have meant
// blanket-ignoring arz_record_get_var and the glib containers, which is
// exactly where a real race would show up.
//
// Outside a TSan build these expand to the bare GLib calls -- no cost, no
// behaviour change, and nothing to keep in sync.

#if defined(__SANITIZE_THREAD__)
#  define TQ_TSAN_ENABLED 1
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define TQ_TSAN_ENABLED 1
#  endif
#endif

#ifdef TQ_TSAN_ENABLED

void __tsan_acquire(void *addr);
void __tsan_release(void *addr);

#define tq_mutex_lock(m)                                                      \
  do                                                                          \
  {                                                                           \
    g_mutex_lock(m);                                                          \
    __tsan_acquire(m);                                                        \
  } while(0)

#define tq_mutex_unlock(m)                                                    \
  do                                                                          \
  {                                                                           \
    __tsan_release(m);                                                        \
    g_mutex_unlock(m);                                                        \
  } while(0)

#else

#define tq_mutex_lock(m)   g_mutex_lock(m)
#define tq_mutex_unlock(m) g_mutex_unlock(m)

#endif

#endif
