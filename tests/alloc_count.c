/* LD_PRELOAD shim for the T4.2 check: counts heap allocation calls (malloc,
 * calloc, realloc, aligned_alloc, posix_memalign, memalign) made by a process
 * and prints "alloc_count: N regexec: M" on stderr at exit, where M of the N
 * calls were made inside glibc's regexec (which allocates and frees scratch
 * memory within some calls; that is the C library's business, not growth of
 * the pipeline). glibc's own entry points (__libc_*) do the real work. Used by
 * tests/test_alloc.sh; plain builds only (the sanitizers replace malloc). */
#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void *__libc_memalign(size_t, size_t);

static unsigned long long n_allocs, n_in_regexec;
static __thread int in_regexec;

static void count(void)
{
    __atomic_fetch_add(&n_allocs, 1, __ATOMIC_RELAXED);
    if (in_regexec)
        __atomic_fetch_add(&n_in_regexec, 1, __ATOMIC_RELAXED);
}

/* regex.h is not included: its prototype uses a VLA parameter; the ABI is
 * all pointers and integers. */
typedef int (*regexec_fn)(const void *, const char *, size_t, void *, int);

int regexec(const void *re, const char *s, size_t n, void *m, int flags)
{
    static regexec_fn real;
    regexec_fn f = __atomic_load_n(&real, __ATOMIC_ACQUIRE);
    if (!f) {
        *(void **)&f = dlsym(RTLD_NEXT, "regexec");     /* POSIX idiom for function pointers */
        __atomic_store_n(&real, f, __ATOMIC_RELEASE);
    }
    in_regexec++;
    int rc = f(re, s, n, m, flags);
    in_regexec--;
    return rc;
}

void *malloc(size_t n)                 { count(); return __libc_malloc(n); }
void *calloc(size_t a, size_t b)       { count(); return __libc_calloc(a, b); }
void *realloc(void *p, size_t n)       { count(); return __libc_realloc(p, n); }
void *aligned_alloc(size_t a, size_t n) { count(); return __libc_memalign(a, n); }
void *memalign(size_t a, size_t n)     { count(); return __libc_memalign(a, n); }

int posix_memalign(void **out, size_t a, size_t n)
{
    count();
    void *p = __libc_memalign(a, n);
    if (!p)
        return ENOMEM;
    *out = p;
    return 0;
}

__attribute__((destructor)) static void report(void)
{
    char buf[96];
    int len = snprintf(buf, sizeof buf, "alloc_count: %llu regexec: %llu\n",
                       __atomic_load_n(&n_allocs, __ATOMIC_RELAXED),
                       __atomic_load_n(&n_in_regexec, __ATOMIC_RELAXED));
    if (len > 0 && write(STDERR_FILENO, buf, (size_t)len) < 0)
        return;
}
