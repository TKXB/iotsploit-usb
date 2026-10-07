#include "duckscript.h"

#include <string.h>

static void set_err(char *err, size_t cap, const char *msg) {
    if (!err || cap == 0) return;
    size_t n = strlen(msg);
    if (n >= cap) n = cap - 1;
    memcpy(err, msg, n);
    err[n] = '\0';
}

/* First whitespace-delimited token in [line, line+len): leading spaces skipped.
 * Returns token start offset in *start and length in *tlen (0 if none). */
static void first_token(const char *line, size_t len, size_t *start, size_t *tlen) {
    size_t i = 0;
    while (i < len && line[i] == ' ') i++;
    size_t j = i;
    while (j < len && line[j] != ' ') j++;
    *start = i;
    *tlen = j - i;
}

static bool tok_is(const char *tok, size_t tlen, const char *name) {
    return strlen(name) == tlen && memcmp(tok, name, tlen) == 0;
}

/* Parse a bounded unsigned decimal. Returns false on empty, non-digit or
 * overflow past DUCK_DELAY_MAX. */
static bool parse_delay(const char *s, size_t len, uint32_t *out) {
    if (len == 0) return false;
    uint32_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10u + (uint32_t)(s[i] - '0');
        if (v > DUCK_DELAY_MAX) return false;
    }
    *out = v;
    return true;
}

static bool classify_chord(const char *line, size_t len, duck_line_t *out) {
    uint8_t mods = 0;
    uint8_t key = 0;
    bool have_key = false;
    size_t i = 0;
    while (i < len) {
        while (i < len && line[i] == ' ') i++;
        if (i >= len) break;
        size_t j = i;
        while (j < len && line[j] != ' ') j++;
        duck_key_t k;
        bool is_mod = false;
        if (!duck_keymap_token(line + i, j - i, &k, &is_mod)) return false;
        if (is_mod) {
            mods |= k.modifier;
        } else {
            if (have_key) return false;  /* at most one key per chord */
            key = k.keycode;
            mods |= k.modifier;          /* e.g. a shifted character token */
            have_key = true;
        }
        i = j;
    }
    out->kind = DUCK_LINE_CHORD;
    out->chord.modifier = mods;
    out->chord.keycode = key;
    return true;
}

bool duckscript_classify(const char *line, size_t len, duck_line_t *out) {
    memset(out, 0, sizeof(*out));

    size_t ts, tl;
    first_token(line, len, &ts, &tl);
    if (tl == 0) { out->kind = DUCK_LINE_BLANK; return true; }

    const char *tok = line + ts;
    if (tok_is(tok, tl, "REM")) {
        out->kind = DUCK_LINE_REM;
        return true;
    }
    if (tok_is(tok, tl, "STRING")) {
        size_t after = ts + tl;
        /* Arg is the verbatim remainder after exactly one delimiter space. */
        size_t astart = (after < len && line[after] == ' ') ? after + 1 : len;
        const char *arg = line + astart;
        size_t arg_len = len - astart;
        for (size_t i = 0; i < arg_len; i++) {
            duck_key_t k;
            if (!duck_keymap_ascii(arg[i], &k)) return false;
        }
        out->kind = DUCK_LINE_STRING;
        out->arg = arg;
        out->arg_len = arg_len;
        return true;
    }
    if (tok_is(tok, tl, "DELAY")) {
        /* Exactly one numeric operand. */
        size_t after = ts + tl;
        while (after < len && line[after] == ' ') after++;
        size_t j = after;
        while (j < len && line[j] != ' ') j++;
        /* Reject trailing junk after the number. */
        size_t k = j;
        while (k < len && line[k] == ' ') k++;
        if (k != len) return false;
        uint32_t ms;
        if (!parse_delay(line + after, j - after, &ms)) return false;
        out->kind = DUCK_LINE_DELAY;
        out->delay_ms = ms;
        return true;
    }
    return classify_chord(line, len, out);
}

int duckscript_validate(const char *script, size_t len,
                        uint32_t *err_line, char *err, size_t err_cap) {
    if (len > DUCK_SCRIPT_MAX) {
        if (err_line) *err_line = 0;
        set_err(err, err_cap, "script too large");
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        if (script[i] == '\0') {
            if (err_line) *err_line = 0;
            set_err(err, err_cap, "NUL byte in script");
            return -1;
        }
    }

    uint32_t line_no = 0;
    size_t i = 0;
    /* Walk lines; accept LF and CRLF; the final line need not be terminated. */
    while (i <= len) {
        if (i == len) break;  /* no trailing empty line after a final newline */
        size_t start = i;
        while (i < len && script[i] != '\n') i++;
        size_t end = i;               /* exclusive, at '\n' or len */
        if (i < len) i++;             /* step past '\n' for next iteration */

        size_t llen = end - start;
        if (llen > 0 && script[start + llen - 1] == '\r') llen--;  /* CRLF */

        line_no++;
        if (llen > DUCK_LINE_MAX) {
            if (err_line) *err_line = line_no;
            set_err(err, err_cap, "line too long");
            return -1;
        }
        duck_line_t parsed;
        if (!duckscript_classify(script + start, llen, &parsed)) {
            if (err_line) *err_line = line_no;
            set_err(err, err_cap, "unsupported or malformed line");
            return -1;
        }
    }
    if (err_line) *err_line = 0;
    set_err(err, err_cap, "");
    return 0;
}
