#ifndef KROSSHAIR_TEST_H
#define KROSSHAIR_TEST_H

/*
 * Minimal self-contained C test harness. Each tests/test_*.c file
 * includes this header and defines its entry point via TESTS_MAIN.
 * A test binary exits non-zero if any CHECK failed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_passed = 0;
static int test_failed = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (cond) {                                                    \
            test_passed++;                                             \
        } else {                                                       \
            test_failed++;                                             \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                    #cond);                                            \
        }                                                              \
    } while (0)

#define CHECK_EQ(actual, expected)                                     \
    do {                                                               \
        long long a_ = (long long)(actual);                            \
        long long e_ = (long long)(expected);                          \
        if (a_ == e_) {                                                \
            test_passed++;                                             \
        } else {                                                       \
            test_failed++;                                             \
            fprintf(stderr,                                            \
                    "  FAIL %s:%d: %s == %s (got %lld, want %lld)\n",  \
                    __FILE__, __LINE__, #actual, #expected, a_, e_);   \
        }                                                              \
    } while (0)

#define CHECK_PTR(actual, expected)                                    \
    do {                                                               \
        const void* a_ = (const void*)(actual);                        \
        const void* e_ = (const void*)(expected);                      \
        if (a_ == e_) {                                                \
            test_passed++;                                             \
        } else {                                                       \
            test_failed++;                                             \
            fprintf(stderr,                                            \
                    "  FAIL %s:%d: %s == %s (got %p, want %p)\n",      \
                    __FILE__, __LINE__, #actual, #expected, a_, e_);   \
        }                                                              \
    } while (0)

#define CHECK_STR(actual, expected)                                    \
    do {                                                               \
        const char* a_ = (actual);                                     \
        const char* e_ = (expected);                                   \
        if (a_ && e_ && strcmp(a_, e_) == 0) {                        \
            test_passed++;                                             \
        } else {                                                       \
            test_failed++;                                             \
            fprintf(stderr,                                            \
                    "  FAIL %s:%d: %s == %s (got \"%s\", want \"%s\")\n", \
                    __FILE__, __LINE__, #actual, #expected,            \
                    a_ ? a_ : "(null)", e_ ? e_ : "(null)");           \
        }                                                              \
    } while (0)

#define RUN_TEST(fn) do { test_failed = 0; fn(); } while (0)

#define TESTS_MAIN(fn)                                                \
    int main(void)                                                    \
    {                                                                 \
        fprintf(stderr, "# %s\n", #fn);                              \
        RUN_TEST(fn);                                                 \
        fprintf(stderr, "# %s: %d passed, %d failed\n", #fn,         \
                test_passed, test_failed);                            \
        return test_failed ? 1 : 0;                                   \
    }

#endif /* KROSSHAIR_TEST_H */
