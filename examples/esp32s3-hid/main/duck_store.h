#ifndef DUCK_STORE_H
#define DUCK_STORE_H

/*
 * Single-owner script upload store: a committed slot and a staging slot.
 *
 * DUCK:UPLoad:STARt reserves ownership and the exact expected length; the
 * SCPI binary block (DATA:WRITE) streams through the begin/data/end callbacks
 * into staging; a validated script is published to the committed slot only on
 * a clean end. Abort, overflow, length mismatch, a foreign owner or a failed
 * validation all leave the previously committed script intact and discard
 * staging. Portable and allocation-free; no callback input pointer is retained.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "duckscript.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DUCK_UP_NONE = 0,     /* no upload in progress */
    DUCK_UP_RECEIVING,    /* reserved and/or receiving block data */
    DUCK_UP_READY,        /* last upload committed successfully */
    DUCK_UP_ERROR         /* last upload failed; committed slot unchanged */
} duck_upload_state_t;

typedef struct {
    char   committed[DUCK_SCRIPT_MAX];
    size_t committed_len;
    bool   have_committed;

    char   staging[DUCK_SCRIPT_MAX];
    size_t staging_len;

    size_t expected;      /* length reserved by STARt */
    int    owner;         /* owning transport token; 0 = unreserved */
    bool   reserved;
    int    state;         /* duck_upload_state_t */
} duck_store_t;

void duck_store_init(duck_store_t *s);

/* STARt: reserve ownership and the expected block length. Rejects a length
 * over DUCK_SCRIPT_MAX, or a reservation held by a different owner. */
int duck_store_reserve(duck_store_t *s, int owner, size_t expected);

/* ABORt: release a reservation held by `owner` and discard staging. */
int duck_store_abort(duck_store_t *s, int owner);

/* Block callbacks, driven by the core on the owning transport's context.
 * begin requires the reservation and an exact length match. */
int duck_store_block_begin(duck_store_t *s, int owner, size_t total);
int duck_store_block_data(duck_store_t *s, const uint8_t *data, size_t len);
int duck_store_block_end(duck_store_t *s, size_t total);

duck_upload_state_t duck_store_upload_state(const duck_store_t *s);

/* Committed script, or NULL/0 when none has been committed. */
const char *duck_store_committed(const duck_store_t *s, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* DUCK_STORE_H */
