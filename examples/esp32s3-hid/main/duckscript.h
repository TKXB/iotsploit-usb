#ifndef DUCKSCRIPT_H
#define DUCKSCRIPT_H

/*
 * Bounded DuckyScript-subset parser (milestone 2 language).
 *
 * Portable and allocation-free: it classifies one line at a time and validates
 * a whole script without expanding it into a keystroke list. The runner reuses
 * the same classifier while executing, so "valid" and "runnable" cannot drift.
 *
 * Supported lines (US layout, printable ASCII):
 *   (blank)              no-op
 *   REM <text>           comment, no-op
 *   STRING <text>        type the literal remainder of the line
 *   DELAY <ms>           wait, bounded by the runtime budget
 *   <MOD>... [KEY]       a chord: zero+ modifiers and an optional key/char
 *
 * Only a documented subset is accepted; everything else is rejected with the
 * offending 1-based line number. This is a support matrix, not a claim of full
 * DuckyScript or EvilDuck compatibility.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "duck_keymap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Documented limits. Named constants, measured before freezing (see plan). */
#define DUCK_SCRIPT_MAX   4096u  /* bytes of script text */
#define DUCK_LINE_MAX      512u  /* bytes per line, excluding newline */
#define DUCK_DELAY_MAX   60000u  /* ms accepted by a single DELAY */

typedef enum {
    DUCK_LINE_BLANK = 0,
    DUCK_LINE_REM,
    DUCK_LINE_STRING,
    DUCK_LINE_DELAY,
    DUCK_LINE_CHORD
} duck_line_kind_t;

typedef struct {
    duck_line_kind_t kind;
    const char *arg;     /* STRING: literal text; points into the script */
    size_t      arg_len;
    uint32_t    delay_ms;/* DELAY only */
    duck_key_t  chord;   /* CHORD only: combined modifiers + optional key */
} duck_line_t;

/* Classify a single line (without its newline). Returns true if the line is a
 * valid, supported line and fills *out; false otherwise. */
bool duckscript_classify(const char *line, size_t len, duck_line_t *out);

/* Validate an entire script. Returns 0 if every line is supported and all
 * limits hold. On failure returns non-zero, sets *err_line to the 1-based
 * failing line, and writes a short reason into err (NUL-terminated, bounded by
 * err_cap). err/err_line may be NULL. Rejects NUL bytes and over-long lines. */
int duckscript_validate(const char *script, size_t len,
                        uint32_t *err_line, char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* DUCKSCRIPT_H */
