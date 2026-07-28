/*
 * Benchmark: Cairo's "magic number" double -> 16.16 fixed-point trick
 * vs. the straightforward "multiply by 65536 and cast" approach.
 *
 * Only doubles in the range representable by a signed 16.16 fixed-point
 * value are used ([-32768.0, 32767.0]), since that's the domain where
 * both approaches are meant to work and where Cairo actually uses this.
 *
 * Two kinds of benchmark are run for each approach:
 *
 *   - THROUGHPUT: convert a whole array of independent doubles. The
 *     compiler is free to pipeline / vectorize this, same as it would
 *     for real calls to these `static inline` functions in a loop.
 *
 *   - LATENCY: each conversion's result feeds into the input of the
 *     next one, forming a serial dependency chain. This defeats
 *     pipelining and exposes the true instruction latency of each
 *     approach -- notably the magic-number trick's double-store /
 *     int32-load through a union, which is a classic store-forwarding
 *     stall on x86.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef int32_t cairo_fixed_16_16_t;

#define CAIRO_MAGIC_NUMBER_FIXED_16_16 (103079215104.0)

static inline cairo_fixed_16_16_t
magic_fixed_16_16_from_double(double d)
{
    union {
        double d;
        int32_t i[2];
    } u;

    u.d = d + CAIRO_MAGIC_NUMBER_FIXED_16_16;
#ifdef FLOAT_WORDS_BIGENDIAN
    return u.i[1];
#else
    return u.i[0];
#endif
}

static inline cairo_fixed_16_16_t
naive_fixed_16_16_from_double(double d)
{
    return (cairo_fixed_16_16_t)(d * 65536.0);
}

/* ---- utilities ---------------------------------------------------- */

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Small fast PRNG (xorshift64*) so results are reproducible and we
 * don't pay rand()'s overhead while building the input arrays. */
static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

static uint64_t xorshift64star(void)
{
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* Uniform double in [-32768.0, 32767.0], well within 16.16 range. */
static double random_input(void)
{
    double u = (double)(xorshift64star() >> 11) * (1.0 / 9007199254740992.0); /* [0,1) */
    return -32768.0 + u * 65535.0;
}

/* ---- throughput benchmark ------------------------------------------ */

#define N_ELEMS (1 << 20) /* 1M doubles, ~8MB: exceeds L2, fits L3 */
#define N_REPEATS 30

static double inputs[N_ELEMS];
static int32_t outputs[N_ELEMS];

/* Each approach gets its own convert-the-whole-array function, called
 * directly (never through a function pointer), so the compiler can
 * inline and auto-vectorize exactly as it would for a real call site
 * to one of these `static inline` conversion functions. */

__attribute__((noinline)) static void
convert_array_magic(const double *in, int32_t *out, int n)
{
    for (int i = 0; i < n; i++)
        out[i] = magic_fixed_16_16_from_double(in[i]);
}

__attribute__((noinline)) static void
convert_array_naive(const double *in, int32_t *out, int n)
{
    for (int i = 0; i < n; i++)
        out[i] = naive_fixed_16_16_from_double(in[i]);
}

typedef void (*convert_array_fn)(const double *, int32_t *, int);

static double bench_throughput(convert_array_fn fn, const char *name)
{
    double best = 1e300;

    for (int rep = 0; rep < N_REPEATS; rep++) {
        double t0 = now_sec();
        fn(inputs, outputs, N_ELEMS);
        double t1 = now_sec();
        if (t1 - t0 < best)
            best = t1 - t0;

        /* Prevent the compiler from proving the loop is dead across
         * reps by making the next rep's work observably depend on it. */
        __asm__ __volatile__("" : : "g"(outputs) : "memory");
    }

    /* checksum so the whole computation can't be optimized away */
    uint64_t checksum = 0;
    for (int i = 0; i < N_ELEMS; i++)
        checksum += (uint32_t)outputs[i];

    double ns_per_call = best * 1e9 / N_ELEMS;
    printf("%-32s best=%8.3f ms  %6.3f ns/call  (checksum=%llu)\n",
           name, best * 1e3, ns_per_call, (unsigned long long)checksum);
    return ns_per_call;
}

/* ---- latency benchmark --------------------------------------------- */

#define N_CHAIN (1 << 24) /* 16M serially-dependent conversions */

__attribute__((noinline)) static double
chain_magic(double acc, int n)
{
    for (int i = 0; i < n; i++) {
        int32_t v = magic_fixed_16_16_from_double(acc);
        /* Fold the integer result back into a small double so the
         * next iteration's input truly depends on this one's output,
         * while staying within [-32768, 32767]. */
        acc = (double)(int16_t)(v ^ i);
    }
    return acc;
}

__attribute__((noinline)) static double
chain_naive(double acc, int n)
{
    for (int i = 0; i < n; i++) {
        int32_t v = naive_fixed_16_16_from_double(acc);
        acc = (double)(int16_t)(v ^ i);
    }
    return acc;
}

typedef double (*chain_fn)(double, int);

static double bench_latency(chain_fn fn, const char *name)
{
    double best = 1e300;
    double result = 0;

    for (int rep = 0; rep < 5; rep++) {
        double t0 = now_sec();
        result = fn(0.5 /* arbitrary seed in-range */, N_CHAIN);
        double t1 = now_sec();
        if (t1 - t0 < best)
            best = t1 - t0;
        __asm__ __volatile__("" : : "g"(&result) : "memory");
    }

    double ns_per_call = best * 1e9 / N_CHAIN;
    printf("%-32s best=%8.3f ms  %6.3f ns/call  (result=%g)\n", name, best * 1e3,
           ns_per_call, result);
    return ns_per_call;
}

int main(void)
{
    for (int i = 0; i < N_ELEMS; i++)
        inputs[i] = random_input();

    printf("=== Throughput (independent conversions, array of %d) ===\n", N_ELEMS);
    double t_magic = bench_throughput(convert_array_magic, "magic number");
    double t_naive = bench_throughput(convert_array_naive, "naive multiply+cast");
    printf("magic/naive ratio: %.2fx\n\n", t_magic / t_naive);

    printf("=== Latency (serial dependency chain, %d conversions) ===\n", N_CHAIN);
    double l_magic = bench_latency(chain_magic, "magic number");
    double l_naive = bench_latency(chain_naive, "naive multiply+cast");
    printf("magic/naive ratio: %.2fx\n", l_magic / l_naive);

    return 0;
}
