#ifndef DUCK_KEYMAP_H
#define DUCK_KEYMAP_H

/*
 * US-layout character and token -> HID keyboard usage mapping.
 *
 * Portable: no RTOS, USB stack or board headers, so it is exercised on the
 * host by tests/test_duckscript.c. Keycodes are USB HID Usage Table IDs for
 * the Keyboard/Keypad page (0x07); modifier bits are the standard boot-report
 * modifier byte (bit0 LeftCtrl, bit1 LeftShift, bit2 LeftAlt, bit3 LeftGUI).
 *
 * This is the explicit US-layout table the integration plan requires:
 * tud_hid_keyboard_report() does not translate characters, so shifted
 * punctuation and uppercase carry their modifier here.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Boot-report modifier bits (left-hand variants only; one layout, one set). */
#define DUCK_MOD_CTRL   0x01u
#define DUCK_MOD_SHIFT  0x02u
#define DUCK_MOD_ALT    0x04u
#define DUCK_MOD_GUI    0x08u

typedef struct {
    uint8_t modifier;  /* OR of DUCK_MOD_* */
    uint8_t keycode;   /* HID usage id, 0 = no key (modifier-only chord) */
} duck_key_t;

/* Map one printable ASCII character to a keycode (+ shift where needed).
 * Returns true on success; false for any non-printable or unmapped byte
 * (including '\0', '\n', '\r' and bytes >= 0x80). */
bool duck_keymap_ascii(char c, duck_key_t *out);

/* Resolve a whitespace-delimited script token of length `len`.
 *   - A modifier name (CTRL/CONTROL, SHIFT, ALT, GUI/WINDOWS/WIN) sets
 *     *is_modifier = true and fills out->modifier (keycode 0).
 *   - A named key (ENTER, TAB, ESCAPE, SPACE, arrows, F1..F12, ...) or a
 *     single printable character sets *is_modifier = false and out->keycode.
 * Returns false for an unknown token. `is_modifier` may be NULL. */
bool duck_keymap_token(const char *tok, size_t len, duck_key_t *out,
                       bool *is_modifier);

#ifdef __cplusplus
}
#endif

#endif /* DUCK_KEYMAP_H */
