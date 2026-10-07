#include "alt_cache.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    alt_cache_t c, empty;
    uint8_t pk[32] = {1}, addr[64] = {2};
    alt_meta_t meta = {.observed_slot=100, .last_extended_slot=100,
                       .deactivation_slot=UINT64_MAX, .start_index=1, .has_authority=1};
    alt_slot_hash_t hashes[2] = {{.slot=100}, {.slot=90}};
    alt_bank_t bank = {.slot=100, .slot_hashes_complete=1, .slot_hashes=hashes};
    uint16_t n = 99;
    const uint8_t *a;
    char path[128];
    snprintf(path, sizeof(path), "/tmp/alt_lifecycle_%ld.bin", (long)getpid());
    memset(meta.observed_bank_hash, 0x11, 32);
    memset(bank.bank_hash, 0x11, 32);
    memset(hashes[0].hash, 0x11, 32);
    assert(!alt_cache_init(&c));
    assert(!alt_cache_init(&empty));
    assert(!alt_cache_put_meta(&c, pk, addr, 2, &meta, 7));
    assert(alt_cache_check(&c, pk, NULL, &n) == ALT_BANK_UNPROVEN && n == 0);
    assert(alt_cache_live_active_n(&c, pk, 100, &n) == 0 && n == 1);
    assert(alt_cache_check(&c, pk, &bank, &n) == ALT_VALID && n == 1);
    bank.slot = 101; bank.n_slot_hashes = 1;
    assert(alt_cache_check(&c, pk, &bank, &n) == ALT_VALID && n == 2);
    hashes[0].hash[0] ^= 1;
    assert(alt_cache_check(&c, pk, &bank, &n) == ALT_BANK_UNPROVEN);
    hashes[0].hash[0] ^= 1;
    bank.slot = 99;
    assert(alt_cache_check(&c, pk, &bank, &n) == ALT_FUTURE_OBSERVATION);
    bank.slot = 1000; bank.n_slot_hashes = 2;
    meta.deactivation_slot = 90;
    assert(!alt_cache_put_meta(&c, pk, addr, 2, &meta, 7));
    /* Skipped slots: presence, not numeric distance, governs cooldown. */
    assert(alt_cache_check(&c, pk, &bank, &n) == ALT_VALID);
    bank.n_slot_hashes = 1;
    assert(alt_cache_check(&c, pk, &bank, &n) == ALT_DEACTIVATED);
    assert(!alt_cache_save(&c, path));
    assert(!alt_cache_load(&empty, path));
    assert(alt_cache_check(&empty, pk, &bank, &n) == ALT_DEACTIVATED);
    FILE *f = fopen(path, "wb"); assert(f); fputs("ALT2", f); fclose(f);
    assert(alt_cache_load(&empty, path) == -1);
    assert(!alt_cache_get(&empty, pk, &a, &n) && n == 2); /* failed reload is atomic */
    addr[0] ^= 1;
    assert(!alt_cache_put_meta(&c, pk, addr, 2, &meta, 7));
    assert(alt_cache_check(&c, pk, &bank, &n) == ALT_CONFLICT);
    alt_cache_free(&empty); assert(!alt_cache_init(&empty));
    assert(!alt_cache_save(&empty, path));
    assert(!alt_cache_load(&c, path));
    assert(alt_cache_check(&c, pk, &bank, &n) == ALT_MISSING);
    assert(!alt_cache_put(&c, pk, addr, 2));
    assert(alt_cache_check(&c, pk, &bank, &n) == ALT_META_UNKNOWN);
    f = fopen(path, "wb"); assert(f);
    uint32_t magic = ALT_MAGIC, count = 1;
    uint16_t old_n = 2;
    assert(fwrite(&magic, 4, 1, f) == 1 && fwrite(&count, 4, 1, f) == 1);
    assert(fwrite(pk, 32, 1, f) == 1 && fwrite(&old_n, 2, 1, f) == 1);
    assert(fwrite(addr, 64, 1, f) == 1); fclose(f);
    assert(!alt_cache_load(&c, path));
    assert(alt_cache_check(&c, pk, &bank, &n) == ALT_META_UNKNOWN);
    f = fopen(path, "ab"); assert(f); fputc(1, f); fclose(f);
    assert(alt_cache_load(&c, path) == -1);
    if (argc == 2) {
        assert(!alt_cache_load(&empty, argv[1]));
        const alt_ent_t *e = NULL;
        for (uint32_t i = 0; i < empty.cap; i++) if (empty.ent[i].used) e = &empty.ent[i];
        assert(e && e->meta.observed_slot == 100 && e->meta.start_index == 1);
        bank.slot = 101; bank.n_slot_hashes = 1;
        assert(alt_cache_check(&empty, e->pk, &bank, &n) == ALT_VALID && n == 2);
    }
    remove(path);
    alt_cache_free(&c); alt_cache_free(&empty);
    puts("ALT lifecycle: activation, cooldown, fork hash, conflict, atomic reload, deletion, legacy passed");
    return 0;
}
