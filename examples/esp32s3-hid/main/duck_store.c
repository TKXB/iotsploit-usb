#include "duck_store.h"

#include <string.h>

void duck_store_init(duck_store_t *s) {
    memset(s, 0, sizeof(*s));
    s->state = DUCK_UP_NONE;
}

/* Fail an in-progress upload: discard staging, release the reservation, mark
 * ERROR. The committed slot is never touched here. */
static int fail(duck_store_t *s) {
    s->staging_len = 0;
    s->expected = 0;
    s->owner = 0;
    s->reserved = false;
    s->state = DUCK_UP_ERROR;
    return -1;
}

int duck_store_reserve(duck_store_t *s, int owner, size_t expected) {
    if (owner == 0) return -1;
    if (expected > DUCK_SCRIPT_MAX) return -1;
    if (s->reserved && s->owner != owner) return -1;  /* held by another owner */
    s->owner = owner;
    s->expected = expected;
    s->staging_len = 0;
    s->reserved = true;
    s->state = DUCK_UP_RECEIVING;
    return 0;
}

int duck_store_abort(duck_store_t *s, int owner) {
    if (!s->reserved || s->owner != owner) return -1;
    s->staging_len = 0;
    s->expected = 0;
    s->owner = 0;
    s->reserved = false;
    s->state = DUCK_UP_NONE;  /* clean abort, not an error */
    return 0;
}

int duck_store_block_begin(duck_store_t *s, int owner, size_t total) {
    if (!s->reserved || s->owner != owner) return fail(s);
    if (total != s->expected) return fail(s);
    s->staging_len = 0;
    s->state = DUCK_UP_RECEIVING;
    return 0;
}

int duck_store_block_data(duck_store_t *s, const uint8_t *data, size_t len) {
    if (!s->reserved) return fail(s);
    if (len > s->expected - s->staging_len) return fail(s);  /* overflow */
    memcpy(s->staging + s->staging_len, data, len);          /* copy by length */
    s->staging_len += len;
    return 0;
}

int duck_store_block_end(duck_store_t *s, size_t total) {
    if (!s->reserved) return fail(s);
    if (total != s->expected || s->staging_len != s->expected) return fail(s);
    if (duckscript_validate(s->staging, s->staging_len, NULL, NULL, 0) != 0) {
        return fail(s);  /* committed slot preserved */
    }
    /* Atomic publish: copy staging into the committed slot. */
    memcpy(s->committed, s->staging, s->staging_len);
    s->committed_len = s->staging_len;
    s->have_committed = true;
    s->staging_len = 0;
    s->expected = 0;
    s->owner = 0;
    s->reserved = false;
    s->state = DUCK_UP_READY;
    return 0;
}

duck_upload_state_t duck_store_upload_state(const duck_store_t *s) {
    return (duck_upload_state_t)s->state;
}

const char *duck_store_committed(const duck_store_t *s, size_t *len) {
    if (!s->have_committed) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = s->committed_len;
    return s->committed;
}
