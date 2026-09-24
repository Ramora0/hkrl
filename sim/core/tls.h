#pragma once
/* Thread-local slots and a mutex, so different instances can be stepped on different threads.
 *
 * Not `__thread` / `_Thread_local`: on this toolchain either makes the DLL depend on libgcc_s_seh-1.dll.
 * Keys are allocated from a constructor (run on DLL_PROCESS_ATTACH), before any export can be called.
 *
 * One instance is single-threaded and unlocked.  hksim_vocab is shared and NOT locked: the caller
 * interns from one thread.
 */
#include <stddef.h>

#if defined(_WIN32)
#  include <windows.h>
typedef DWORD hks_tls_key;
typedef CRITICAL_SECTION hks_mutex;
#  define HKS_TLS_NEW()        TlsAlloc()
#  define HKS_TLS_GET(k)       TlsGetValue(k)
#  define HKS_TLS_SET(k, v)    TlsSetValue((k), (v))
#  define HKS_MUTEX_INIT(m)    InitializeCriticalSection(m)
#  define HKS_LOCK(m)          EnterCriticalSection(m)
#  define HKS_UNLOCK(m)        LeaveCriticalSection(m)
#else
#  include <pthread.h>
typedef pthread_key_t hks_tls_key;
typedef pthread_mutex_t hks_mutex;
#  define HKS_TLS_NEW()        ({ pthread_key_t _k; pthread_key_create(&_k, NULL); _k; })
#  define HKS_TLS_GET(k)       pthread_getspecific(k)
#  define HKS_TLS_SET(k, v)    pthread_setspecific((k), (v))
#  define HKS_MUTEX_INIT(m)    pthread_mutex_init((m), NULL)
#  define HKS_LOCK(m)          pthread_mutex_lock(m)
#  define HKS_UNLOCK(m)        pthread_mutex_unlock(m)
#endif

/* Runs on DLL load, before any export can be reached. */
#define HKS_CTOR __attribute__((constructor)) static void
