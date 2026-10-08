#include "duck_keymap.h"

#include <string.h>

/* HID usage ids used below (Keyboard/Keypad page 0x07). */
#define KEY_A       0x04u
#define KEY_1       0x1Eu
#define KEY_0       0x27u
#define KEY_ENTER   0x28u
#define KEY_ESC     0x29u
#define KEY_BKSP    0x2Au
#define KEY_TAB     0x2Bu
#define KEY_SPACE   0x2Cu
#define KEY_MINUS   0x2Du
#define KEY_EQUAL   0x2Eu
#define KEY_LBRACK  0x2Fu
#define KEY_RBRACK  0x30u
#define KEY_BSLASH  0x31u
#define KEY_SEMI    0x33u
#define KEY_QUOTE   0x34u
#define KEY_GRAVE   0x35u
#define KEY_COMMA   0x36u
#define KEY_DOT     0x37u
#define KEY_SLASH   0x38u

static bool ascii_letter(char c, duck_key_t *out) {
    if (c >= 'a' && c <= 'z') {
        out->modifier = 0;
        out->keycode = (uint8_t)(KEY_A + (c - 'a'));
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        out->modifier = DUCK_MOD_SHIFT;
        out->keycode = (uint8_t)(KEY_A + (c - 'A'));
        return true;
    }
    return false;
}

static bool ascii_digit(char c, duck_key_t *out) {
    if (c == '0') { out->modifier = 0; out->keycode = KEY_0; return true; }
    if (c >= '1' && c <= '9') {
        out->modifier = 0;
        out->keycode = (uint8_t)(KEY_1 + (c - '1'));
        return true;
    }
    return false;
}

/* Punctuation and the handful of control-ish printables we accept. Each entry
 * is the ASCII byte, its keycode, and whether Shift is required. */
typedef struct { char ch; uint8_t key; uint8_t shift; } punct_t;

static const punct_t k_punct[] = {
    { ' ',  KEY_SPACE,  0 },
    { '-',  KEY_MINUS,  0 }, { '_', KEY_MINUS,  1 },
    { '=',  KEY_EQUAL,  0 }, { '+', KEY_EQUAL,  1 },
    { '[',  KEY_LBRACK, 0 }, { '{', KEY_LBRACK, 1 },
    { ']',  KEY_RBRACK, 0 }, { '}', KEY_RBRACK, 1 },
    { '\\', KEY_BSLASH, 0 }, { '|', KEY_BSLASH, 1 },
    { ';',  KEY_SEMI,   0 }, { ':', KEY_SEMI,   1 },
    { '\'', KEY_QUOTE,  0 }, { '"', KEY_QUOTE,  1 },
    { '`',  KEY_GRAVE,  0 }, { '~', KEY_GRAVE,  1 },
    { ',',  KEY_COMMA,  0 }, { '<', KEY_COMMA,  1 },
    { '.',  KEY_DOT,    0 }, { '>', KEY_DOT,    1 },
    { '/',  KEY_SLASH,  0 }, { '?', KEY_SLASH,  1 },
    { '!',  KEY_1,      1 }, { '@', 0x1Fu,      1 }, { '#', 0x20u, 1 },
    { '$',  0x21u,      1 }, { '%', 0x22u,      1 }, { '^', 0x23u, 1 },
    { '&',  0x24u,      1 }, { '*', 0x25u,      1 }, { '(', 0x26u, 1 },
    { ')',  KEY_0,      1 },
};

bool duck_keymap_ascii(char c, duck_key_t *out) {
    if (ascii_letter(c, out)) return true;
    if (ascii_digit(c, out))  return true;
    for (size_t i = 0; i < sizeof(k_punct) / sizeof(k_punct[0]); i++) {
        if (k_punct[i].ch == c) {
            out->modifier = k_punct[i].shift ? DUCK_MOD_SHIFT : 0u;
            out->keycode = k_punct[i].key;
            return true;
        }
    }
    return false;
}

typedef struct { const char *name; uint8_t key; } named_t;

/* Named keys recognised in chord lines. Case-sensitive upper, as DuckyScript. */
static const named_t k_named[] = {
    { "ENTER", KEY_ENTER }, { "RETURN", KEY_ENTER },
    { "ESC", KEY_ESC }, { "ESCAPE", KEY_ESC },
    { "TAB", KEY_TAB }, { "SPACE", KEY_SPACE },
    { "BACKSPACE", KEY_BKSP }, { "DELETE", 0x4Cu }, { "DEL", 0x4Cu },
    { "UP", 0x52u }, { "DOWN", 0x51u }, { "LEFT", 0x50u }, { "RIGHT", 0x4Fu },
    { "HOME", 0x4Au }, { "END", 0x4Du }, { "INSERT", 0x49u },
    { "PAGEUP", 0x4Bu }, { "PAGEDOWN", 0x4Eu },
    { "CAPSLOCK", 0x39u }, { "MENU", 0x65u }, { "APP", 0x65u },
    { "F1", 0x3Au }, { "F2", 0x3Bu }, { "F3", 0x3Cu }, { "F4", 0x3Du },
    { "F5", 0x3Eu }, { "F6", 0x3Fu }, { "F7", 0x40u }, { "F8", 0x41u },
    { "F9", 0x42u }, { "F10", 0x43u }, { "F11", 0x44u }, { "F12", 0x45u },
};

typedef struct { const char *name; uint8_t bit; } modname_t;

static const modname_t k_mods[] = {
    { "CTRL", DUCK_MOD_CTRL }, { "CONTROL", DUCK_MOD_CTRL },
    { "SHIFT", DUCK_MOD_SHIFT },
    { "ALT", DUCK_MOD_ALT },
    { "GUI", DUCK_MOD_GUI }, { "WINDOWS", DUCK_MOD_GUI }, { "WIN", DUCK_MOD_GUI },
    { "META", DUCK_MOD_GUI }, { "COMMAND", DUCK_MOD_GUI },
};

static bool tok_eq(const char *tok, size_t len, const char *name) {
    return strlen(name) == len && memcmp(tok, name, len) == 0;
}

bool duck_keymap_token(const char *tok, size_t len, duck_key_t *out,
                       bool *is_modifier) {
    out->modifier = 0;
    out->keycode = 0;
    if (is_modifier) *is_modifier = false;
    if (len == 0) return false;

    for (size_t i = 0; i < sizeof(k_mods) / sizeof(k_mods[0]); i++) {
        if (tok_eq(tok, len, k_mods[i].name)) {
            out->modifier = k_mods[i].bit;
            if (is_modifier) *is_modifier = true;
            return true;
        }
    }
    for (size_t i = 0; i < sizeof(k_named) / sizeof(k_named[0]); i++) {
        if (tok_eq(tok, len, k_named[i].name)) {
            out->keycode = k_named[i].key;
            return true;
        }
    }
    if (len == 1) {
        return duck_keymap_ascii(tok[0], out);
    }
    return false;
}
