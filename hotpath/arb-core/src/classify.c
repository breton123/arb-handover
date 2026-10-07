#include "classify.h"

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

int classify_have_avx2(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
#else
    return 0;
#endif
}

int classify_have_avx512(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx512bw");
#else
    return 0;
#endif
}

static uint32_t
classify_range(const uint8_t *p, uint16_t from, uint16_t last, uint32_t hit)
{
    uint16_t i;

    for (i = from; i <= last; i++) {
        if ((hit & REL_DLMM) == 0 && id_eq32(p + i, PROG_DLMM)) {
            hit |= REL_DLMM;
        }
        if ((hit & REL_PUMP) == 0 && id_eq32(p + i, PROG_PUMP)) {
            hit |= REL_PUMP;
        }
        if (hit == REL_BOTH) {
            return hit;
        }
    }
    return hit;
}

uint32_t
classify_scalar(const uint8_t *payload, uint16_t len)
{
    if (payload == NULL || len < 32) {
        return REL_NONE;
    }
    return classify_range(payload, 0, (uint16_t)(len - 32), REL_NONE);
}

#if defined(__x86_64__) || defined(__i386__)

__attribute__((target("avx2")))
uint32_t
classify_avx2(const uint8_t *payload, uint16_t len)
{
    const __m256i b0_d = _mm256_set1_epi8((char)PROG_DLMM[CLASSIFY_DISC0]);
    const __m256i b7_d = _mm256_set1_epi8((char)PROG_DLMM[CLASSIFY_DISC1]);
    const __m256i b0_p = _mm256_set1_epi8((char)PROG_PUMP[CLASSIFY_DISC0]);
    const __m256i b7_p = _mm256_set1_epi8((char)PROG_PUMP[CLASSIFY_DISC1]);
    uint32_t hit = REL_NONE;
    uint16_t last;
    uint16_t i = 0;

    if (payload == NULL || len < 32) {
        return REL_NONE;
    }
    last = (uint16_t)(len - 32);

    /* 32 start offsets. v7 load needs +7, last start in block is i+31. */
    while ((uint32_t)i + 63u <= (uint32_t)len) {
        __m256i v0 = _mm256_loadu_si256((const __m256i *)(payload + i));
        __m256i v7 = _mm256_loadu_si256((const __m256i *)(payload + i + CLASSIFY_DISC1));
        unsigned md = (unsigned)_mm256_movemask_epi8(
            _mm256_and_si256(_mm256_cmpeq_epi8(v0, b0_d),
                             _mm256_cmpeq_epi8(v7, b7_d)));
        unsigned mp = (unsigned)_mm256_movemask_epi8(
            _mm256_and_si256(_mm256_cmpeq_epi8(v0, b0_p),
                             _mm256_cmpeq_epi8(v7, b7_p)));
        unsigned mask = md | mp;

        while (mask != 0) {
            unsigned c = (unsigned)__builtin_ctz(mask);
            const uint8_t *cand = payload + i + c;
            if ((md & (1u << c)) != 0 && (hit & REL_DLMM) == 0
                && id_eq32(cand, PROG_DLMM)) {
                hit |= REL_DLMM;
            }
            if ((mp & (1u << c)) != 0 && (hit & REL_PUMP) == 0
                && id_eq32(cand, PROG_PUMP)) {
                hit |= REL_PUMP;
            }
            if (hit == REL_BOTH) {
                return hit;
            }
            mask &= mask - 1u;
        }
        i = (uint16_t)(i + 32);
    }
    return classify_range(payload, i, last, hit);
}

__attribute__((target("avx512f,avx512bw")))
uint32_t
classify_avx512(const uint8_t *payload, uint16_t len)
{
    const __m512i b0_d = _mm512_set1_epi8((char)PROG_DLMM[CLASSIFY_DISC0]);
    const __m512i b7_d = _mm512_set1_epi8((char)PROG_DLMM[CLASSIFY_DISC1]);
    const __m512i b0_p = _mm512_set1_epi8((char)PROG_PUMP[CLASSIFY_DISC0]);
    const __m512i b7_p = _mm512_set1_epi8((char)PROG_PUMP[CLASSIFY_DISC1]);
    uint32_t hit = REL_NONE;
    uint16_t last;
    uint16_t i = 0;

    if (payload == NULL || len < 32) {
        return REL_NONE;
    }
    last = (uint16_t)(len - 32);

    /* 64 start offsets. v7 load needs +7, last start in block is i+63. */
    while ((uint32_t)i + 95u <= (uint32_t)len) {
        __m512i v0 = _mm512_loadu_si512((const void *)(payload + i));
        __m512i v7 = _mm512_loadu_si512((const void *)(payload + i + CLASSIFY_DISC1));
        __mmask64 m0d = _mm512_cmpeq_epi8_mask(v0, b0_d);
        __mmask64 m7d = _mm512_cmpeq_epi8_mask(v7, b7_d);
        __mmask64 m0p = _mm512_cmpeq_epi8_mask(v0, b0_p);
        __mmask64 m7p = _mm512_cmpeq_epi8_mask(v7, b7_p);
        uint64_t md64 = (uint64_t)(m0d & m7d);
        uint64_t mp64 = (uint64_t)(m0p & m7p);
        uint64_t mask = md64 | mp64;

        while (mask != 0) {
            unsigned c = (unsigned)__builtin_ctzll(mask);
            const uint8_t *cand = payload + i + c;
            if ((md64 & (1ULL << c)) != 0 && (hit & REL_DLMM) == 0
                && id_eq32(cand, PROG_DLMM)) {
                hit |= REL_DLMM;
            }
            if ((mp64 & (1ULL << c)) != 0 && (hit & REL_PUMP) == 0
                && id_eq32(cand, PROG_PUMP)) {
                hit |= REL_PUMP;
            }
            if (hit == REL_BOTH) {
                return hit;
            }
            mask &= mask - 1ULL;
        }
        i = (uint16_t)(i + 64);
    }
    return classify_range(payload, i, last, hit);
}

#else

uint32_t
classify_avx2(const uint8_t *payload, uint16_t len)
{
    return classify_scalar(payload, len);
}

uint32_t
classify_avx512(const uint8_t *payload, uint16_t len)
{
    return classify_scalar(payload, len);
}

#endif
