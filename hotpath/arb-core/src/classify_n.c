#include "classify_n.h"
#include "classify.h"
#include "proto.h"

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

typedef struct {
    const uint8_t *id;
    uint8_t disc0;
    uint8_t disc7;
    uint32_t bit;
} needle_t;

static const needle_t NEEDLE[CLASSIFY_N_MAX] = {
    { PROG_DLMM, 0x04, 0x26, REL_N_DLMM },
    { PROG_PUMP, 0x0c, 0x76, REL_N_PUMP },
    { PROG_CLMM, 0xa5, 0xb5, REL_N_CLMM },
    { PROG_CPMM, 0xa9, 0x52, REL_N_CPMM },
    { PROG_DAMM, 0x09, 0x9c, REL_N_DAMM },
    { PROG_ORCA, 0x0e, 0x53, REL_N_ORCA },
};

static uint32_t
want_mask(uint32_t n_ids)
{
    uint32_t m = 0;
    uint32_t i;

    if (n_ids > CLASSIFY_N_MAX) {
        n_ids = CLASSIFY_N_MAX;
    }
    for (i = 0; i < n_ids; i++) {
        m |= NEEDLE[i].bit;
    }
    return m;
}

static uint32_t
classify_n_range(const uint8_t *p, uint32_t from, uint32_t last,
                 uint32_t n_ids, uint32_t hit)
{
    uint32_t i;
    uint32_t want = want_mask(n_ids);

    for (i = from; i <= last; i++) {
        uint32_t n;
        for (n = 0; n < n_ids; n++) {
            if ((hit & NEEDLE[n].bit) == 0 && id_eq32(p + i, NEEDLE[n].id)) {
                hit |= NEEDLE[n].bit;
            }
        }
        if (hit == want) {
            return hit;
        }
    }
    return hit;
}

#if defined(__x86_64__) || defined(__i386__)

__attribute__((target("avx2")))
static uint32_t
classify_n_avx2(const uint8_t *payload, size_t len, uint32_t n_ids)
{
    __m256i b0[CLASSIFY_N_MAX];
    __m256i b7[CLASSIFY_N_MAX];
    uint32_t hit = REL_N_NONE;
    uint32_t want;
    uint32_t last;
    uint32_t i = 0;
    uint32_t n;

    if (payload == NULL || len < 32 || n_ids == 0) {
        return REL_N_NONE;
    }
    if (n_ids > CLASSIFY_N_MAX) {
        n_ids = CLASSIFY_N_MAX;
    }
    want = want_mask(n_ids);
    last = (uint32_t)(len - 32);
    for (n = 0; n < n_ids; n++) {
        b0[n] = _mm256_set1_epi8((char)NEEDLE[n].disc0);
        b7[n] = _mm256_set1_epi8((char)NEEDLE[n].disc7);
    }

    while (i + 63u <= (uint32_t)len) {
        __m256i v0 = _mm256_loadu_si256((const __m256i *)(payload + i));
        __m256i v7 = _mm256_loadu_si256(
            (const __m256i *)(payload + i + CLASSIFY_N_DISC1));
        unsigned masks[CLASSIFY_N_MAX];
        unsigned any = 0;

        for (n = 0; n < n_ids; n++) {
            masks[n] = (unsigned)_mm256_movemask_epi8(
                _mm256_and_si256(_mm256_cmpeq_epi8(v0, b0[n]),
                                 _mm256_cmpeq_epi8(v7, b7[n])));
            any |= masks[n];
        }
        while (any != 0) {
            unsigned c = (unsigned)__builtin_ctz(any);
            const uint8_t *cand = payload + i + c;
            for (n = 0; n < n_ids; n++) {
                if ((masks[n] & (1u << c)) != 0 && (hit & NEEDLE[n].bit) == 0
                    && id_eq32(cand, NEEDLE[n].id)) {
                    hit |= NEEDLE[n].bit;
                }
            }
            if (hit == want) {
                return hit;
            }
            any &= any - 1u;
        }
        i += 32u;
    }
    return classify_n_range(payload, i, last, n_ids, hit);
}

#endif

uint32_t
classify_n(const uint8_t *payload, size_t len, uint32_t n_ids)
{
    if (payload == NULL || len < 32 || n_ids == 0) {
        return REL_N_NONE;
    }
    if (n_ids > CLASSIFY_N_MAX) {
        n_ids = CLASSIFY_N_MAX;
    }
#if defined(__x86_64__) || defined(__i386__)
    if (classify_have_avx2()) {
        return classify_n_avx2(payload, len, n_ids);
    }
#endif
    return classify_n_range(payload, 0, (uint32_t)(len - 32), n_ids, REL_N_NONE);
}
