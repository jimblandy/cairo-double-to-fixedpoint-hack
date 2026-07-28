# Is Cairo's "magic number" double-to-fixed-point trick still worth it?

\[I found this hack in Cairo's code, and was impressed and amazed, but
also skeptical that it would still be effective on modern hardware. So
I asked Claude to benchmark it for me. This summary and the code is
all Claude. -JimB\]

## Motivation

Cairo's rendering code needs to convert `double` values into `cairo_fixed_16_16_t`,
a 32-bit fixed-point format with 16 fractional bits. The obvious implementation is:

```c
int32_t fixed = (int32_t)(d * 65536.0);
```

Instead, Cairo uses a much less obvious trick: add a carefully chosen constant to
the double so that IEEE-754's mantissa layout does the conversion for you, then
reach into the raw bits of the resulting double and pull out the low 32 bits as
the answer. The source comment claims this "provides a very large speedup ... on
a wide array of systems."

That claim dates to an era of compilers and CPUs quite different from today's.
**Is it still true?** Specifically: is it still faster than just multiplying by
65536 and letting the compiler do the conversion?

## The hack itself

Quoting `cairo-excerpt.c` directly, since the comment already explains the technique
better than a paraphrase would:

> The basic idea is to add a large enough number to the double that the literal
> floating point is moved up to the extent that it forces the double's value to
> be shifted down to the bottom of the mantissa (to make room for the large
> number being added in). Since the mantissa is, at a given moment in time, a
> fixed point integer itself, one can convert a float to various fixed point
> representations by moving around the point of a floating point number through
> arithmetic operations. This behavior is reliable on most modern platforms as
> it is mandated by the IEEE-754 standard for floating point arithmetic.
>
> For our purposes, a "magic number" must be carefully selected that is both
> large enough to produce the desired point-shifting effect, and also has no
> lower bits in its representation that would interfere with our value at the
> bottom of the mantissa. The magic number is calculated as follows:
>
> ```
>          (2 ^ (MANTISSA_SIZE - FRACTIONAL_SIZE)) * 1.5
> ```
>
> where in our case:
>  - MANTISSA_SIZE for 64-bit doubles is 52
>  - FRACTIONAL_SIZE for 16.16 fixed point is 16

```c
#define CAIRO_MAGIC_NUMBER_FIXED_16_16 (103079215104.0)

static inline cairo_fixed_16_16_t
_cairo_fixed_16_16_from_double (double d)
{
    union {
        double d;
        int32_t i[2];
    } u;

    u.d = d + CAIRO_MAGIC_NUMBER_FIXED_16_16;
    return u.i[0]; /* u.i[1] on big-endian */
}
```

Correctness was not in question — the technique is sound within the domain a
16.16 fixed-point value can represent (roughly `[-32768.0, 32767.0]`), which is
the only domain the benchmarks below use. The only question was speed.

## The lede

**On modern hardware, the "obvious" approach is faster for the way Cairo actually
uses this function — converting many coordinates in a loop — and only marginally
slower in a narrow, artificial case (a single isolated conversion with nothing
else to overlap it with).** The speedup the 2003-era comment describes has not
just eroded, it has reversed for the realistic case, because modern compilers can
autovectorize the naive version and cannot vectorize the magic-number version at
all. This holds on plain SSE2, not just on exotic instruction sets — it is not an
artifact of AVX-512 or any particular CPU generation.

## Method

Two C benchmarks, `naive` (`(int32_t)(d * 65536.0)`) vs `magic` (the code above),
over random doubles restricted to `[-32768.0, 32767.0]`:

- **Throughput**: convert a 1M-element array of independent doubles, letting the
  compiler inline/vectorize each approach exactly as it would for a real call
  site to one of these `static inline` functions. This models Cairo's actual
  usage — converting many independent coordinates.
- **Latency**: a serial dependency chain, where each conversion's result feeds
  the next conversion's input. This isolates pure instruction latency from
  pipelining/vectorization, since nothing can overlap.

Both were measured first by wall-clock time (`clock_gettime`, best-of-N trials,
`-O0`/`-O2`/`-O3`, and several `-march` targets), then cross-checked with exact
hardware performance counters via `perf stat` (cycles, instructions), and finally
explained by reading the compiler-generated assembly. All work is in this
directory: `bench.c` (main benchmark), `orig_perf.c` / `latency_isolate.c`
(perf-stat harnesses), run on an AMD Threadripper PRO 7975WX (Zen4) with GCC 15.2.

## Results

### Throughput (the realistic case)

Wall-clock, `-O2`/`-O3`, consistent across `-march=native` (AVX-512),
`x86-64-v3` (AVX2), `x86-64-v2` (SSE4.2), and the plain baseline:

| | ns/call |
|---|---|
| magic | ~0.21 |
| naive | ~0.10 |

`perf stat` over 209,715,200 conversions confirmed this with exact counters:

| | cycles/element | instructions/element |
|---|---|---|
| naive | 0.555 | 0.72 |
| magic | 1.061 | 5.10 |

**naive wins by ~1.9–2x.** The instruction counts explain why: naive compiles to
`vmulpd` + `vcvttpd2dq` operating on 8 doubles at a time (AVX-512), while magic
compiles to a genuinely scalar loop (`vaddsd`, `vmovd`, one element per
iteration) — the union-based type-punning defeats the autovectorizer entirely.
Disassembly confirmed this directly.

### Latency (the narrow case where the old trick still wins)

| | ns/call | cycles/iteration |
|---|---|---|
| magic | ~2.6–2.7 | 14.01 |
| naive | ~3.2 | 17.02 |

**magic wins here, by a fixed ~3 cycles (~15–20%).** Disassembling the two loop
bodies showed they are *identical* except for one instruction pair in the
dependency chain:

```
naive:  vmulsd  (multiply by 65536)   ->  vcvttsd2si  (convert double to int32)
magic:  vaddsd  (add magic constant)  ->  vmovq       (copy raw bits to a GPR)
```

Both instruction counts (8/iteration) are identical; only the cycle counts
differ. The reason: `cvttsd2si` is a real numeric conversion — it decodes the
IEEE-754 sign/exponent/mantissa, truncates toward zero, and checks for
overflow/NaN — while `vmovq` is a raw 64-bit bit copy with no interpretation of
the value at all. Domain-crossing "real" conversions have consistently carried a
few cycles more latency than plain bit-transfer instructions across x86
microarchitectures, and that gap is directly measurable here. (Note also: `addsd`
and `mulsd` have similar latency to each other — a few cycles each, not one —
so the popular "FP arithmetic is single-cycle" intuition conflates throughput
with latency; it's the *throughput* of these units that approaches one op per
cycle, not the latency of any individual instruction.)

### Does this depend on AVX-512 specifically?

No — and this was checked directly, motivated by the historical fact that early
Intel AVX-512 (Skylake-X, ~2017) really did throttle the clock under sustained
heavy 512-bit instructions (separate "frequency license" levels, plus a real
transition penalty when switching between them). That behavior improved on later
Intel generations and was avoided by AMD's Zen4 by design: Zen4 executes every
512-bit AVX-512 instruction as two sequential 256-bit micro-ops on its existing
256-bit datapath, specifically so it never needs a separate power/frequency state
for it.

Measured directly (long-running loop so process-startup overhead is negligible,
clock speed derived from `cycles / task-clock`, with `magic` — which never
vectorizes regardless of build flags — as a built-in no-vectorization control):

| build | naive cycles/element | naive clock | magic cycles/element (control) | magic clock |
|---|---|---|---|---|
| AVX-512 | 0.487 | 5.00 GHz | 1.015 | 4.97 GHz |
| AVX2 | 0.517 | 4.95 GHz | 1.016 | 4.94 GHz |
| SSE2 | 0.516 | 4.88 GHz | 1.020 | 4.95 GHz |

No downclock is visible at any vector width on this chip — clock speed is flat
within measurement noise across all three builds, for both the vectorized and
scalar code paths. AVX-512 also only beats AVX2/SSE2 by about 6%, not the ~2x a
naive doubling-of-width argument would suggest, which is exactly what
double-pumped 256-bit execution predicts.

More importantly for the original question: **the `naive`/`magic` throughput gap
is already ~2x at plain SSE2** (present on every x86-64 chip since 2003) and
barely changes going to AVX2 or AVX-512. The advantage comes from naive being
vectorizable *at all* — the magic-number trick's union-based bit reinterpretation
blocks vectorization regardless of what instruction set is available — not from
exploiting any particular register width. So the throughput result is not an
AVX-512-era artifact and would very likely hold even on hardware that pays a real
AVX-512 tax, or that has no AVX-512 at all.

## Verdict

The comment's claim was almost certainly true when it was written — pre-SSE2-era
FPUs and compilers without autovectorization made the union trick a genuine win.
On a modern compiler and CPU, that has inverted for Cairo's actual use pattern
(converting many independent coordinates): the "obvious" multiply-and-cast is
about twice as fast, because it vectorizes and the magic-number trick cannot. The
old trick still holds a small, real edge (~3 cycles) in the narrow case of a
single conversion sitting in a serial dependency chain with nothing else to
overlap it with — but that's not how Cairo uses this function.

## Caveats

- Measured on one CPU (AMD Threadripper PRO 7975WX / Zen4) and one compiler
  (GCC 15.2). Other compilers' autovectorizers, or CPUs without efficient packed
  `cvttpd2dq`, could shift the numbers, though the SSE2-level result suggests
  this generalizes fairly broadly across x86-64.
- The two approaches round differently (`magic` uses banker's rounding, `naive`
  truncates toward zero); this was explicitly out of scope, per the original
  question, which was about speed, not correctness.
- Only tested within the domain a 16.16 fixed-point value can represent
  (`[-32768.0, 32767.0]`), which is the domain in which both approaches are
  claimed to work correctly.
