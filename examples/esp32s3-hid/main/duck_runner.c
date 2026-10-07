#include "duck_runner.h"

#include <string.h>

/* Internal phases. */
enum {
    RS_LOAD = 0,        /* load the next actionable line */
    RS_STRING_PRESS,    /* press report for the current STRING character */
    RS_STRING_RELEASE,  /* neutral release after a STRING character */
    RS_CHORD_PRESS,     /* press report for a chord line */
    RS_CHORD_RELEASE,   /* neutral release after a chord */
    RS_DELAY,           /* waiting for a DELAY deadline */
    RS_FINISH           /* best-effort neutral release, then settle terminal */
};

#define DUCK_DEFAULT_RUNTIME_MS 60000u
#define DUCK_REPORTS_PER_POLL   8   /* bounded work per poll() call */

static void set_err(duck_runner_t *r, const char *msg) {
    size_t n = strlen(msg);
    if (n >= sizeof(r->err)) n = sizeof(r->err) - 1;
    memcpy(r->err, msg, n);
    r->err[n] = '\0';
}

void duck_runner_init(duck_runner_t *r, const duck_runner_cfg_t *cfg) {
    memset(r, 0, sizeof(*r));
    r->cfg = *cfg;
    r->state = DUCK_IDLE;
    r->err[0] = '\0';
}

int duck_runner_start(duck_runner_t *r, const char *script, size_t len) {
    if (r->state == DUCK_RUNNING) return -1;

    uint32_t el = 0;
    char e[DUCK_ERR_MAX];
    if (duckscript_validate(script, len, &el, e, sizeof(e)) != 0) {
        r->line_no = el;
        set_err(r, e[0] ? e : "invalid script");
        r->state = DUCK_ERROR;
        return -1;
    }

    r->script = script;
    r->script_len = len;
    r->stop_requested = false;
    r->detached = false;
    r->cursor = 0;
    r->line_no = 0;
    r->emit_idx = 0;
    r->phase = RS_LOAD;
    r->term_reason = DUCK_DONE;
    r->pend_mod = 0;
    r->pend_key = 0;
    r->err[0] = '\0';
    r->start_ms = r->cfg.now_ms(r->cfg.user);
    r->state = DUCK_RUNNING;
    return 0;
}

void duck_runner_stop(duck_runner_t *r) {
    if (r->state == DUCK_RUNNING) r->stop_requested = true;
}

void duck_runner_detach(duck_runner_t *r) {
    r->detached = true;
    r->stop_requested = true;
}

duck_state_t duck_runner_state(const duck_runner_t *r) {
    return (duck_state_t)r->state;
}

uint32_t duck_runner_line(const duck_runner_t *r) {
    return r->line_no;
}

const char *duck_runner_error(const duck_runner_t *r) {
    return r->err;
}

/* Try to submit one report. 1 = accepted, 0 = busy (retry), <0 = delivery
 * failure. A busy backend is gated before submitting so no report is lost. */
static int submit_report(duck_runner_t *r, uint8_t mod, uint8_t key) {
    if (r->cfg.ready && !r->cfg.ready(r->cfg.user)) return 0;
    uint8_t keys[6] = { 0, 0, 0, 0, 0, 0 };
    keys[0] = key;
    int rc = r->cfg.submit(r->cfg.user, mod, keys);
    if (rc > 0) {
        r->pend_mod = mod;
        r->pend_key = key;
    }
    return rc;
}

/* Enter the finish phase: stop producing presses, then release while able. */
static void begin_finish(duck_runner_t *r, int reason, const char *msg) {
    r->term_reason = reason;
    if (msg && msg[0]) set_err(r, msg);
    r->phase = RS_FINISH;
}

/* Advance to the next actionable line, skipping blanks and comments. Sets the
 * phase for the loaded line. Returns false at end of script. */
static bool load_next(duck_runner_t *r, uint32_t now) {
    while (r->cursor < r->script_len) {
        size_t start = r->cursor;
        size_t i = start;
        while (i < r->script_len && r->script[i] != '\n') i++;
        size_t end = i;
        r->cursor = (i < r->script_len) ? i + 1 : r->script_len;

        size_t llen = end - start;
        if (llen > 0 && r->script[start + llen - 1] == '\r') llen--;
        r->line_no++;

        duck_line_t p;
        if (!duckscript_classify(r->script + start, llen, &p)) {
            begin_finish(r, DUCK_ERROR, "line parse error");
            return true;
        }
        switch (p.kind) {
        case DUCK_LINE_BLANK:
        case DUCK_LINE_REM:
            continue;
        case DUCK_LINE_STRING:
            if (p.arg_len == 0) continue;
            r->cur = p;
            r->emit_idx = 0;
            r->phase = RS_STRING_PRESS;
            return true;
        case DUCK_LINE_DELAY:
            r->delay_deadline = now + p.delay_ms;
            r->phase = RS_DELAY;
            return true;
        case DUCK_LINE_CHORD:
            r->cur = p;
            r->phase = RS_CHORD_PRESS;
            return true;
        }
    }
    return false;
}

void duck_runner_poll(duck_runner_t *r) {
    if (r->state != DUCK_RUNNING) return;

    /* A detach cannot deliver a release, so settle to an error immediately. */
    if (r->detached) {
        set_err(r, "usb detached");
        r->state = DUCK_ERROR;
        return;
    }

    uint32_t limit = r->cfg.max_runtime_ms ? r->cfg.max_runtime_ms
                                           : DUCK_DEFAULT_RUNTIME_MS;

    for (int budget = DUCK_REPORTS_PER_POLL; budget > 0; budget--) {
        if (r->state != DUCK_RUNNING) return;
        uint32_t now = r->cfg.now_ms(r->cfg.user);

        /* Checked between every report and during every wait. */
        if (r->phase != RS_FINISH) {
            if ((uint32_t)(now - r->start_ms) >= limit) {
                begin_finish(r, DUCK_ERROR, "runtime limit");
            } else if (r->stop_requested) {
                begin_finish(r, DUCK_CANCELLED, "");
            }
        }

        switch (r->phase) {
        case RS_LOAD:
            if (!load_next(r, now)) begin_finish(r, DUCK_DONE, "");
            break;

        case RS_STRING_PRESS: {
            duck_key_t k;
            (void)duck_keymap_ascii(r->cur.arg[r->emit_idx], &k);
            int rc = submit_report(r, k.modifier, k.keycode);
            if (rc == 0) return;
            if (rc < 0) { begin_finish(r, DUCK_ERROR, "hid delivery failed"); break; }
            r->phase = RS_STRING_RELEASE;
            break;
        }
        case RS_STRING_RELEASE: {
            int rc = submit_report(r, 0, 0);
            if (rc == 0) return;
            if (rc < 0) { begin_finish(r, DUCK_ERROR, "hid delivery failed"); break; }
            r->emit_idx++;
            r->phase = (r->emit_idx >= r->cur.arg_len) ? RS_LOAD : RS_STRING_PRESS;
            break;
        }
        case RS_CHORD_PRESS: {
            int rc = submit_report(r, r->cur.chord.modifier, r->cur.chord.keycode);
            if (rc == 0) return;
            if (rc < 0) { begin_finish(r, DUCK_ERROR, "hid delivery failed"); break; }
            r->phase = RS_CHORD_RELEASE;
            break;
        }
        case RS_CHORD_RELEASE: {
            int rc = submit_report(r, 0, 0);
            if (rc == 0) return;
            if (rc < 0) { begin_finish(r, DUCK_ERROR, "hid delivery failed"); break; }
            r->phase = RS_LOAD;
            break;
        }
        case RS_DELAY:
            if ((int32_t)(now - r->delay_deadline) >= 0) r->phase = RS_LOAD;
            else return;  /* keep waiting; cancellation handled at loop top */
            break;

        case RS_FINISH: {
            int rc = submit_report(r, 0, 0);  /* neutral release, best effort */
            if (rc == 0) return;              /* busy: retry next poll */
            r->state = r->term_reason;        /* accepted or failed: settle */
            return;
        }
        default:
            return;
        }
    }
}
