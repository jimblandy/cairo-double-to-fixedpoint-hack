/* perf-stat-friendly harness for the *original* throughput and latency
 * benchmarks from bench.c (array conversion + xor/int16-folded chain),
 * run one at a time so `perf stat` cycle counts aren't shared between
 * the two approaches. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAIRO_MAGIC_NUMBER_FIXED_16_16 (103079215104.0)

static inline int32_t
magic_fixed_16_16_from_double(double d)
{
    union {
        double d;
        int32_t i[2];
    } u;
    u.d = d + CAIRO_MAGIC_NUMBER_FIXED_16_16;
    return u.i[0];
}

static inline int32_t
naive_fixed_16_16_from_double(double d)
{
    return (int32_t)(d * 65536.0);
}

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

__attribute__((noinline)) static double
chain_magic(double acc, long n)
{
    for (long i = 0; i < n; i++) {
        int32_t v = magic_fixed_16_16_from_double(acc);
        acc = (double)(int16_t)(v ^ (int)i);
    }
    return acc;
}

__attribute__((noinline)) static double
chain_naive(double acc, long n)
{
    for (long i = 0; i < n; i++) {
        int32_t v = naive_fixed_16_16_from_double(acc);
        acc = (double)(int16_t)(v ^ (int)i);
    }
    return acc;
}

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

static double random_input(void)
{
    double u = (double)(xorshift64star() >> 11) * (1.0 / 9007199254740992.0);
    return -32768.0 + u * 65535.0;
}

#define N_ELEMS (1 << 20)
#define N_CHAIN (1 << 24)

static double inputs[N_ELEMS];
static int32_t outputs[N_ELEMS];

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "tp-naive";

    int repeats = argc > 2 ? atoi(argv[2]) : 200;

    if (strcmp(mode, "tp-magic") == 0 || strcmp(mode, "tp-naive") == 0) {
        for (int i = 0; i < N_ELEMS; i++)
            inputs[i] = random_input();
        for (int rep = 0; rep < repeats; rep++) {
            if (mode[3] == 'm')
                convert_array_magic(inputs, outputs, N_ELEMS);
            else
                convert_array_naive(inputs, outputs, N_ELEMS);
        }
        uint64_t checksum = 0;
        for (int i = 0; i < N_ELEMS; i++)
            checksum += (uint32_t)outputs[i];
        printf("checksum=%llu\n", (unsigned long long)checksum);
    } else {
        double result = (mode[4] == 'm') ? chain_magic(0.5, N_CHAIN) : chain_naive(0.5, N_CHAIN);
        printf("result=%g\n", result);
    }
    return 0;
}
