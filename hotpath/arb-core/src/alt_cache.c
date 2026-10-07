#include "alt_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(alt_meta_t) == 136, "ALT2 metadata ABI");

static const alt_ent_t *
entry(const alt_cache_t *c, const uint8_t pk[32])
{
    uint32_t i, x;
    if (!c || !c->ent || !c->cap || !pk) return NULL;
    memcpy(&x, pk, 4);
    for (i = 0; i < c->cap; i++) {
        const alt_ent_t *e = &c->ent[(x % c->cap + i) % c->cap];
        if (!e->used) return NULL;
        if (!memcmp(e->pk, pk, 32)) return e;
    }
    return NULL;
}

static int
conflict(const alt_ent_t *old, const alt_ent_t *next)
{
    uint16_t common = old->n < next->n ? old->n : next->n;
    if (old->flags & ALT_QUARANTINED) return 1;
    if (memcmp(old->addr, next->addr, (size_t)common * 32)) return 1;
    if (!(old->flags & next->flags & ALT_HAS_META)) return 0;
    return next->n < old->n
        || next->meta.last_extended_slot < old->meta.last_extended_slot
        || (old->meta.deactivation_slot != UINT64_MAX
            && old->meta.deactivation_slot != next->meta.deactivation_slot)
        || (next->meta.has_authority && (!old->meta.has_authority
            || memcmp(old->meta.authority, next->meta.authority, 32)))
        || (old->meta.observed_slot == next->meta.observed_slot
            && (memcmp(old->meta.account_hash, next->meta.account_hash, 32)
                || ((old->flags & next->flags & ALT_HAS_HASH)
                    && memcmp(old->meta.observed_bank_hash, next->meta.observed_bank_hash, 32))))
        || (old->meta.last_extended_slot == next->meta.last_extended_slot
            && old->meta.start_index != next->meta.start_index);
}

static uint32_t
h32(const uint8_t pk[32], uint32_t cap)
{
    uint32_t x;
    memcpy(&x, pk, 4);
    return x % cap;
}

int
alt_cache_init(alt_cache_t *c)
{
    if (c == NULL) {
        return -1;
    }
    memset(c, 0, sizeof(*c));
    c->cap = ALT_CACHE_CAP;
    c->ent = calloc(c->cap, sizeof(*c->ent));
    return c->ent == NULL ? -1 : 0;
}

void
alt_cache_free(alt_cache_t *c)
{
    if (c == NULL) {
        return;
    }
    free(c->ent);
    memset(c, 0, sizeof(*c));
}

int
alt_cache_put(alt_cache_t *c, const uint8_t pk[32],
              const uint8_t *addrs, uint16_t n)
{
    uint32_t i, s;
    if (c == NULL || !c->ent || !c->cap || pk == NULL || (n > 0 && addrs == NULL) || n > ALT_ADDR_MAX) {
        return -1;
    }
    s = h32(pk, c->cap);
    for (i = 0; i < c->cap; i++) {
        alt_ent_t *e = &c->ent[(s + i) % c->cap];
        if (e->used && memcmp(e->pk, pk, 32) == 0) {
            memset(&e->meta, 0, sizeof(e->meta));
            e->flags = 0;
            e->n = n;
            if (n > 0) {
                memcpy(e->addr, addrs, (size_t)n * 32u);
            }
            return 0;
        }
        if (!e->used) {
            e->used = 1;
            memcpy(e->pk, pk, 32);
            e->n = n;
            if (n > 0) {
                memcpy(e->addr, addrs, (size_t)n * 32u);
            }
            c->n++;
            c->put++;
            return 0;
        }
    }
    return -1;
}

int
alt_cache_put_meta(alt_cache_t *c, const uint8_t pk[32],
                   const uint8_t *addrs, uint16_t n,
                   const alt_meta_t *meta, uint16_t flags)
{
    alt_ent_t old;
    const alt_ent_t *prior = entry(c, pk);
    int existed = prior != NULL;
    if (!meta || flags & ~15u || meta->start_index > n
        || meta->has_authority > 1
        || ((flags & ALT_HAS_META) && (meta->last_extended_slot > meta->observed_slot
            || (meta->deactivation_slot != UINT64_MAX && meta->deactivation_slot > meta->observed_slot)))) return -1;
    if (existed) old = *prior;
    if (alt_cache_put(c, pk, addrs, n)) return -1;
    alt_ent_t *e = (alt_ent_t *)entry(c, pk);
    e->meta = *meta;
    e->flags = flags;
    if (existed && conflict(&old, e)) e->flags |= ALT_QUARANTINED;
    return 0;
}

int
alt_cache_check(const alt_cache_t *c, const uint8_t pk[32],
                const alt_bank_t *bank, uint16_t *active_n)
{
    const alt_ent_t *e = entry(c, pk);
    int ancestor = 0, deactivation_present = 0;
    if (active_n) *active_n = 0;
    if (!e) return ALT_MISSING;
    if (e->flags & ALT_QUARANTINED) return ALT_CONFLICT;
    if ((e->flags & 7u) != 7u) return ALT_META_UNKNOWN;
    if (!bank || !bank->slot_hashes_complete
        || (bank->n_slot_hashes && !bank->slot_hashes)) return ALT_BANK_UNPROVEN;
    if (e->meta.observed_slot > bank->slot) return ALT_FUTURE_OBSERVATION;
    if (e->meta.observed_slot == bank->slot)
        ancestor = !memcmp(e->meta.observed_bank_hash, bank->bank_hash, 32);
    for (uint16_t i = 0; i < bank->n_slot_hashes; i++) {
        const alt_slot_hash_t *s = &bank->slot_hashes[i];
        if (s->slot >= bank->slot || (i && bank->slot_hashes[i-1].slot <= s->slot))
            return ALT_BANK_UNPROVEN;
        if (s->slot == e->meta.observed_slot
            && !memcmp(s->hash, e->meta.observed_bank_hash, 32)) ancestor = 1;
        if (s->slot == e->meta.deactivation_slot) deactivation_present = 1;
    }
    if (!ancestor) return ALT_BANK_UNPROVEN;
    /* For an observed active table, any later deactivation is newer than this
     * still-recent ancestor and cannot have aged out of the target SlotHashes.
     * The address prefix is append-only along that proven descendant branch. */
    if (e->meta.deactivation_slot != UINT64_MAX
        && e->meta.deactivation_slot != bank->slot && !deactivation_present)
        return ALT_DEACTIVATED;
    if (active_n) *active_n = bank->slot > e->meta.last_extended_slot ? e->n : e->meta.start_index;
    return ALT_VALID;
}

int
alt_cache_live_active_n(const alt_cache_t *c, const uint8_t pk[32],
                        uint64_t tx_slot, uint16_t *active_n)
{
    const alt_ent_t *e = entry(c, pk);
    if (active_n) *active_n = 0;
    if (!e) return ALT_MISSING;
    if (e->flags & ALT_QUARANTINED) return ALT_CONFLICT;
    if (e->n == 0) return ALT_MISSING;
    if ((e->flags & ALT_HAS_META)
        && e->meta.deactivation_slot != UINT64_MAX
        && (!tx_slot || tx_slot > e->meta.deactivation_slot))
        return ALT_DEACTIVATED;
    if ((e->flags & ALT_HAS_META) && tx_slot
        && e->meta.last_extended_slot >= tx_slot) {
        if (active_n) *active_n = e->meta.start_index;
    } else if (active_n) {
        *active_n = e->n;
    }
    return 0;
}

int
alt_cache_get(const alt_cache_t *c, const uint8_t pk[32],
              const uint8_t **addrs, uint16_t *n)
{
    uint32_t i, s;
    if (c == NULL || !c->ent || !c->cap || pk == NULL) {
        return 1;
    }
    s = h32(pk, c->cap);
    for (i = 0; i < c->cap; i++) {
        const alt_ent_t *e = &c->ent[(s + i) % c->cap];
        if (!e->used) {
            ((alt_cache_t *)c)->miss++;
            return 1;
        }
        if (memcmp(e->pk, pk, 32) == 0) {
            if (addrs != NULL) {
                *addrs = e->addr;
            }
            if (n != NULL) {
                *n = e->n;
            }
            ((alt_cache_t *)c)->hit++;
            return 0;
        }
    }
    ((alt_cache_t *)c)->miss++;
    return 1;
}

int
alt_cache_note_miss(alt_cache_t *c, const uint8_t pk[32])
{
    uint32_t i;
    const uint8_t *addrs = NULL;
    uint16_t n = 0;
    if (c == NULL || pk == NULL) {
        return -1;
    }
    if (alt_cache_get(c, pk, &addrs, &n) == 0) {
        return 1;
    }
    for (i = 0; i < c->n_pend; i++) {
        if (memcmp(c->pend[i], pk, 32) == 0) {
            return 1;
        }
    }
    if (c->n_pend >= ALT_PEND_MAX) {
        return -1;
    }
    memcpy(c->pend[c->n_pend], pk, 32);
    c->n_pend++;
    c->unique++;
    return 0;
}

int
alt_cache_save(const alt_cache_t *c, const char *path)
{
    FILE *f;
    uint32_t magic = ALT_MAGIC_V2, i, n = 0;
    if (c == NULL || path == NULL || c->ent == NULL) {
        return -1;
    }
    for (i = 0; i < c->cap; i++) {
        if (c->ent[i].used) {
            n++;
        }
    }
    f = fopen(path, "wb");
    if (f == NULL) {
        return -1;
    }
    if (fwrite(&magic, 4, 1, f) != 1 || fwrite(&n, 4, 1, f) != 1) {
        fclose(f);
        return -1;
    }
    for (i = 0; i < c->cap; i++) {
        const alt_ent_t *e = &c->ent[i];
        if (!e->used) {
            continue;
        }
        if (fwrite(e->pk, 32, 1, f) != 1
            || fwrite(&e->n, 2, 1, f) != 1
            || fwrite(&e->flags, 2, 1, f) != 1
            || fwrite(&e->meta, sizeof(e->meta), 1, f) != 1
            || (e->n > 0 && fwrite(e->addr, 32u * (uint32_t)e->n, 1, f) != 1)) {
            fclose(f);
            return -1;
        }
    }
    return fclose(f) == 0 ? 0 : -1;
}

int
alt_cache_load(alt_cache_t *c, const char *path)
{
    FILE *f;
    uint32_t magic = 0, n = 0, i;
    alt_cache_t next;
    if (c == NULL || !c->ent || path == NULL) {
        return -1;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    if (fread(&magic, 4, 1, f) != 1 || (magic != ALT_MAGIC && magic != ALT_MAGIC_V2)
        || fread(&n, 4, 1, f) != 1 || n > ALT_CACHE_CAP || alt_cache_init(&next)) {
        fclose(f);
        return -1;
    }
    for (i = 0; i < n; i++) {
        uint8_t pk[32];
        uint16_t an = 0;
        uint16_t flags = 0;
        alt_meta_t meta = {0};
        uint8_t addr[ALT_ADDR_MAX * 32u];
        if (fread(pk, 32, 1, f) != 1 || fread(&an, 2, 1, f) != 1
            || an > ALT_ADDR_MAX) {
            goto bad;
        }
        if (magic == ALT_MAGIC_V2
            && (fread(&flags, 2, 1, f) != 1 || fread(&meta, sizeof(meta), 1, f) != 1)) goto bad;
        if (an > 0 && fread(addr, 32u * (uint32_t)an, 1, f) != 1) {
            goto bad;
        }
        if (entry(&next, pk) || alt_cache_put_meta(&next, pk, addr, an, &meta, flags)) goto bad;
        const alt_ent_t *old = entry(c, pk);
        alt_ent_t *e = (alt_ent_t *)entry(&next, pk);
        if (old && conflict(old, e)) e->flags |= ALT_QUARANTINED;
    }
    if (fgetc(f) != EOF || ferror(f)) goto bad;
    fclose(f);
    next.hit = c->hit;
    next.miss = c->miss;
    alt_cache_free(c);
    *c = next;  /* Replace only a complete validated snapshot; deletions persist. */
    return 0;
bad:
    fclose(f);
    alt_cache_free(&next);
    return -1;
}
