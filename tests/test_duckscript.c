/*
 * Host tests for the esp32s3-hid EvilDuck integration's portable core:
 * key mapping, script validation, the upload store, and the asynchronous
 * runner (press/release ordering, cancellation, delay deadlines, runtime
 * budget, busy backpressure, delivery failure and detach).
 *
 * No RTOS, USB or board code: HID, clock and readiness are injected fakes.
 */

#include "duck_keymap.h"
#include "duck_runner.h"
#include "duck_store.h"
#include "duckscript.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- fake HID backend + clock ---- */

typedef struct {
    uint32_t now;
    int busy_reports;   /* report this many submits busy before accepting */
    int fail_index;     /* submit index (1-based) that returns -1; 0 = never */
    int submit_count;
    struct { uint8_t mod; uint8_t key; } log[2048];
    size_t log_len;
} fake_t;

static int fake_submit(void *user, uint8_t mod, const uint8_t keys[6]) {
    fake_t *f = (fake_t *)user;
    f->submit_count++;
    if (f->fail_index && f->submit_count == f->fail_index) return -1;
    assert(f->log_len < sizeof(f->log) / sizeof(f->log[0]));
    f->log[f->log_len].mod = mod;
    f->log[f->log_len].key = keys[0];
    f->log_len++;
    return 1;
}

static bool fake_ready(void *user) {
    fake_t *f = (fake_t *)user;
    if (f->busy_reports > 0) { f->busy_reports--; return false; }
    return true;
}

static uint32_t fake_now(void *user) { return ((fake_t *)user)->now; }

static void fake_init(fake_t *f) { memset(f, 0, sizeof(*f)); }

static void runner_init(duck_runner_t *r, fake_t *f, uint32_t budget, bool use_ready) {
    duck_runner_cfg_t cfg = {
        .submit = fake_submit,
        .ready = use_ready ? fake_ready : NULL,
        .now_ms = fake_now,
        .user = f,
        .max_runtime_ms = budget,
    };
    duck_runner_init(r, &cfg);
}

static void pump(duck_runner_t *r) {
    for (int i = 0; i < 500 && duck_runner_state(r) == DUCK_RUNNING; i++)
        duck_runner_poll(r);
}

/* Does the report log contain a press of this keycode? */
static bool typed_key(const fake_t *f, uint8_t key) {
    for (size_t i = 0; i < f->log_len; i++)
        if (f->log[i].key == key) return true;
    return false;
}

/* ---- tests ---- */

static void test_keymap(void) {
    duck_key_t k;
    assert(duck_keymap_ascii('a', &k) && k.modifier == 0 && k.keycode == 0x04);
    assert(duck_keymap_ascii('A', &k) && k.modifier == DUCK_MOD_SHIFT && k.keycode == 0x04);
    assert(duck_keymap_ascii('z', &k) && k.keycode == 0x1D);
    assert(duck_keymap_ascii('1', &k) && k.modifier == 0 && k.keycode == 0x1E);
    assert(duck_keymap_ascii('!', &k) && k.modifier == DUCK_MOD_SHIFT && k.keycode == 0x1E);
    assert(duck_keymap_ascii('0', &k) && k.keycode == 0x27);
    assert(duck_keymap_ascii(' ', &k) && k.keycode == 0x2C);
    assert(duck_keymap_ascii('/', &k) && k.modifier == 0 && k.keycode == 0x38);
    assert(duck_keymap_ascii('?', &k) && k.modifier == DUCK_MOD_SHIFT && k.keycode == 0x38);
    assert(!duck_keymap_ascii('\n', &k));
    assert(!duck_keymap_ascii('\t', &k));
    assert(!duck_keymap_ascii((char)0x01, &k));

    bool is_mod = false;
    assert(duck_keymap_token("GUI", 3, &k, &is_mod) && is_mod && k.modifier == DUCK_MOD_GUI);
    assert(duck_keymap_token("CTRL", 4, &k, &is_mod) && is_mod && k.modifier == DUCK_MOD_CTRL);
    assert(duck_keymap_token("ENTER", 5, &k, &is_mod) && !is_mod && k.keycode == 0x28);
    assert(duck_keymap_token("F5", 2, &k, &is_mod) && k.keycode == 0x3E);
    assert(duck_keymap_token("r", 1, &k, &is_mod) && !is_mod && k.keycode == 0x15);
    assert(!duck_keymap_token("NOPE", 4, &k, &is_mod));
    assert(!duck_keymap_token("", 0, &k, &is_mod));
}

static void test_validate(void) {
    const char *ok =
        "REM a harmless demo\n"
        "DELAY 100\n"
        "STRING Hello, World! 123\n"
        "ENTER\n"
        "GUI r\n"
        "CTRL ALT DELETE\n"
        "\n";
    assert(duckscript_validate(ok, strlen(ok), NULL, NULL, 0) == 0);

    uint32_t line = 0;
    char err[64];

    const char *unknown = "STRING ok\nBOGUS token\n";
    assert(duckscript_validate(unknown, strlen(unknown), &line, err, sizeof(err)) != 0);
    assert(line == 2);

    const char *ctrl = "STRING ab\x01";  /* control byte inside STRING */
    assert(duckscript_validate(ctrl, strlen(ctrl), &line, err, sizeof(err)) != 0);
    assert(line == 1);

    const char *two_keys = "GUI r t\n";  /* two non-modifier keys in one chord */
    assert(duckscript_validate(two_keys, strlen(two_keys), &line, err, sizeof(err)) != 0);

    const char *bad_delay = "DELAY 99999\n";  /* over DUCK_DELAY_MAX */
    assert(duckscript_validate(bad_delay, strlen(bad_delay), &line, err, sizeof(err)) != 0);

    char nul[] = { 'A', '\0', 'B' };
    assert(duckscript_validate(nul, sizeof(nul), &line, err, sizeof(err)) != 0);

    /* CRLF and an unterminated final line both validate. */
    const char *crlf = "ENTER\r\nSTRING hi";
    assert(duckscript_validate(crlf, strlen(crlf), NULL, NULL, 0) == 0);
}

static void test_runner_string(void) {
    fake_t f; fake_init(&f);
    duck_runner_t r; runner_init(&r, &f, 0, false);
    const char *s = "STRING ab\n";
    assert(duck_runner_start(&r, s, strlen(s)) == 0);
    pump(&r);
    assert(duck_runner_state(&r) == DUCK_DONE);
    /* press a, release, press b, release, final neutral release */
    assert(f.log_len == 5);
    assert(f.log[0].key == 0x04 && f.log[0].mod == 0);
    assert(f.log[1].key == 0x00);
    assert(f.log[2].key == 0x05);
    assert(f.log[3].key == 0x00);
    assert(f.log[4].key == 0x00);
    assert(duck_runner_line(&r) == 1);
}

static void test_runner_chord(void) {
    fake_t f; fake_init(&f);
    duck_runner_t r; runner_init(&r, &f, 0, false);
    const char *s = "GUI r\n";
    assert(duck_runner_start(&r, s, strlen(s)) == 0);
    pump(&r);
    assert(duck_runner_state(&r) == DUCK_DONE);
    assert(f.log[0].mod == DUCK_MOD_GUI && f.log[0].key == 0x15);  /* press GUI+r */
    assert(f.log[1].mod == 0 && f.log[1].key == 0x00);             /* release */
}

static void test_runner_cancel_during_delay(void) {
    fake_t f; fake_init(&f);
    duck_runner_t r; runner_init(&r, &f, 0, false);
    const char *s = "DELAY 5000\nSTRING x\n";
    assert(duck_runner_start(&r, s, strlen(s)) == 0);
    duck_runner_poll(&r);                 /* enters the delay */
    assert(duck_runner_state(&r) == DUCK_RUNNING);
    duck_runner_stop(&r);
    pump(&r);
    assert(duck_runner_state(&r) == DUCK_CANCELLED);
    assert(!typed_key(&f, 0x1B));         /* 'x' was never typed */
}

static void test_runner_invalid_no_keystrokes(void) {
    fake_t f; fake_init(&f);
    duck_runner_t r; runner_init(&r, &f, 0, false);
    const char *s = "BOGUS line\n";
    assert(duck_runner_start(&r, s, strlen(s)) == -1);
    assert(duck_runner_state(&r) == DUCK_ERROR);
    assert(duck_runner_line(&r) == 1);
    assert(f.log_len == 0);               /* no report ever submitted */
}

static void test_runner_runtime_budget(void) {
    fake_t f; fake_init(&f);
    duck_runner_t r; runner_init(&r, &f, 10, false);   /* 10 ms budget */
    const char *s = "DELAY 5000\n";
    assert(duck_runner_start(&r, s, strlen(s)) == 0);
    duck_runner_poll(&r);                 /* enters the 5 s delay */
    assert(duck_runner_state(&r) == DUCK_RUNNING);
    f.now = 50;                           /* past the 10 ms budget */
    pump(&r);
    assert(duck_runner_state(&r) == DUCK_ERROR);
    assert(strstr(duck_runner_error(&r), "runtime") != NULL);
}

static void test_runner_busy_backpressure(void) {
    fake_t f; fake_init(&f);
    f.busy_reports = 3;                   /* backend busy for 3 attempts */
    duck_runner_t r; runner_init(&r, &f, 0, true);
    const char *s = "STRING a\n";
    assert(duck_runner_start(&r, s, strlen(s)) == 0);
    pump(&r);
    assert(duck_runner_state(&r) == DUCK_DONE);
    assert(f.log[0].key == 0x04);         /* nothing lost to the busy gate */
}

static void test_runner_delivery_failure(void) {
    fake_t f; fake_init(&f);
    f.fail_index = 1;                     /* first submit fails */
    duck_runner_t r; runner_init(&r, &f, 0, false);
    const char *s = "STRING a\n";
    assert(duck_runner_start(&r, s, strlen(s)) == 0);
    pump(&r);
    assert(duck_runner_state(&r) == DUCK_ERROR);
    assert(strstr(duck_runner_error(&r), "delivery") != NULL);
}

static void test_runner_detach(void) {
    fake_t f; fake_init(&f);
    duck_runner_t r; runner_init(&r, &f, 0, false);
    const char *s = "DELAY 5000\n";
    assert(duck_runner_start(&r, s, strlen(s)) == 0);
    duck_runner_poll(&r);
    duck_runner_detach(&r);
    duck_runner_poll(&r);
    assert(duck_runner_state(&r) == DUCK_ERROR);
    assert(strstr(duck_runner_error(&r), "detach") != NULL);
}

static void test_runner_reject_second_run(void) {
    fake_t f; fake_init(&f);
    duck_runner_t r; runner_init(&r, &f, 0, false);
    const char *s = "DELAY 5000\n";
    assert(duck_runner_start(&r, s, strlen(s)) == 0);
    duck_runner_poll(&r);                 /* now RUNNING in the delay */
    assert(duck_runner_start(&r, s, strlen(s)) == -1);  /* rejected while active */
}

static void test_store(void) {
    duck_store_t st; duck_store_init(&st);
    size_t len = 0;
    assert(duck_store_committed(&st, &len) == NULL && len == 0);

    /* Happy path: reserve, stream, commit a valid script. */
    const char *s1 = "ENTER";  /* 5 bytes */
    assert(duck_store_reserve(&st, 1, 5) == 0);
    assert(duck_store_block_begin(&st, 1, 5) == 0);
    assert(duck_store_block_data(&st, (const uint8_t *)s1, 5) == 0);
    assert(duck_store_block_end(&st, 5) == 0);
    assert(duck_store_upload_state(&st) == DUCK_UP_READY);
    const char *c = duck_store_committed(&st, &len);
    assert(c && len == 5 && memcmp(c, "ENTER", 5) == 0);

    /* Foreign owner cannot drive the block. */
    assert(duck_store_reserve(&st, 1, 5) == 0);
    assert(duck_store_block_begin(&st, 2, 5) == -1);
    assert(duck_store_upload_state(&st) == DUCK_UP_ERROR);

    /* Length mismatch at begin. */
    assert(duck_store_reserve(&st, 1, 5) == 0);
    assert(duck_store_block_begin(&st, 1, 6) == -1);

    /* Oversize data overflows staging and fails. */
    assert(duck_store_reserve(&st, 1, 3) == 0);
    assert(duck_store_block_begin(&st, 1, 3) == 0);
    assert(duck_store_block_data(&st, (const uint8_t *)"ABCD", 4) == -1);

    /* A failed validation preserves the previously committed script. */
    assert(duck_store_reserve(&st, 1, 4) == 0);
    assert(duck_store_block_begin(&st, 1, 4) == 0);
    assert(duck_store_block_data(&st, (const uint8_t *)"NOPE", 4) == 0);
    assert(duck_store_block_end(&st, 4) == -1);
    assert(duck_store_upload_state(&st) == DUCK_UP_ERROR);
    c = duck_store_committed(&st, &len);
    assert(c && len == 5 && memcmp(c, "ENTER", 5) == 0);  /* unchanged */

    /* Clean abort releases the reservation. */
    assert(duck_store_reserve(&st, 1, 5) == 0);
    assert(duck_store_abort(&st, 1) == 0);
    assert(duck_store_upload_state(&st) == DUCK_UP_NONE);
    assert(duck_store_abort(&st, 1) == -1);  /* nothing to abort */
}

static void test_end_to_end_upload_then_run(void) {
    duck_store_t st; duck_store_init(&st);
    const char *script = "STRING hi\nENTER\n";
    size_t n = strlen(script);
    assert(duck_store_reserve(&st, 7, n) == 0);
    assert(duck_store_block_begin(&st, 7, n) == 0);
    assert(duck_store_block_data(&st, (const uint8_t *)script, n) == 0);
    assert(duck_store_block_end(&st, n) == 0);

    size_t clen = 0;
    const char *committed = duck_store_committed(&st, &clen);
    assert(committed);

    fake_t f; fake_init(&f);
    duck_runner_t r; runner_init(&r, &f, 0, false);
    assert(duck_runner_start(&r, committed, clen) == 0);
    pump(&r);
    assert(duck_runner_state(&r) == DUCK_DONE);
    assert(typed_key(&f, 0x0B));          /* 'h' */
    assert(typed_key(&f, 0x28));          /* ENTER */
}

/* A submitted release is not yet a delivered release. */
static bool fake_complete(void *user) {
    return ((fake_t *)user)->busy_reports == 0;
}

static void test_finish_completion_and_deadline(void) {
    fake_t f; fake_init(&f);
    duck_runner_t r; runner_init(&r, &f, 10, false);
    r.cfg.complete = fake_complete;
    f.busy_reports = 1;
    assert(duck_runner_start(&r, "", 0) == 0);
    duck_runner_poll(&r);
    assert(duck_runner_state(&r) == DUCK_RUNNING);
    assert(f.log_len == 1); /* only the final neutral transfer */
    f.busy_reports = 0;
    duck_runner_poll(&r);
    assert(duck_runner_state(&r) == DUCK_DONE);

    runner_init(&r, &f, 10, true);
    f.now = 0; f.busy_reports = 10000;
    assert(duck_runner_start(&r, "ENTER", 5) == 0);
    duck_runner_poll(&r);
    f.now = 10; duck_runner_poll(&r); /* runtime expires */
    f.now = 110; duck_runner_poll(&r); /* release deadline expires */
    assert(duck_runner_state(&r) == DUCK_ERROR);
    assert(strstr(duck_runner_error(&r), "release timeout"));
    f.busy_reports = 0;
    assert(duck_runner_start(&r, "", 0) == 0); /* not permanently busy */
    pump(&r);
    assert(duck_runner_state(&r) == DUCK_DONE);

    fake_init(&f); runner_init(&r, &f, 10, false);
    f.fail_index = 1; /* final neutral report fails */
    assert(duck_runner_start(&r, "", 0) == 0);
    pump(&r);
    assert(duck_runner_state(&r) == DUCK_ERROR);
    assert(strstr(duck_runner_error(&r), "release failed"));

    fake_init(&f); runner_init(&r, &f, 10, false);
    r.cfg.complete = fake_complete;
    f.busy_reports = 1;
    assert(duck_runner_start(&r, "", 0) == 0);
    duck_runner_poll(&r);
    f.now = 100; duck_runner_poll(&r);
    assert(duck_runner_state(&r) == DUCK_ERROR); /* accepted but never completed */
}

static void test_accept_then_immediate_stop(void) {
    fake_t f; fake_init(&f);
    duck_runner_t r; runner_init(&r, &f, 0, false);
    assert(duck_runner_start(&r, "", 0) == 0);
    pump(&r);
    assert(duck_runner_state(&r) == DUCK_DONE);
    assert(duck_runner_start(&r, "ENTER", 5) == 0);
    assert(duck_runner_state(&r) == DUCK_RUNNING); /* no stale DONE */
    duck_runner_stop(&r); /* before any poll */
    pump(&r);
    assert(duck_runner_state(&r) == DUCK_CANCELLED);
    assert(!typed_key(&f, 0x28));
}

int main(void) {
    test_finish_completion_and_deadline();
    test_accept_then_immediate_stop();
    test_keymap();
    test_validate();
    test_runner_string();
    test_runner_chord();
    test_runner_cancel_during_delay();
    test_runner_invalid_no_keystrokes();
    test_runner_runtime_budget();
    test_runner_busy_backpressure();
    test_runner_delivery_failure();
    test_runner_detach();
    test_runner_reject_second_run();
    test_store();
    test_end_to_end_upload_then_run();
    puts("duckscript tests passed");
    return 0;
}
