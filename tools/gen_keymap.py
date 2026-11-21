#!/usr/bin/env python3
"""Generate engine/miryoku_keymap.h from a Miryoku ZMK checkout.

Usage: gen_keymap.py <path-to-miryoku_zmk> <out.h> [ALPHAS]   (ALPHAS default QWERTY)

Reads miryoku/miryoku_babel/miryoku_layer_{list,alternatives}.h so the layer
table always matches your fork. Re-run after pulling upstream changes.
"""
import re
import sys
from pathlib import Path

HID = {  # ZMK name -> HID keyboard usage
    **{c: 4 + i for i, c in enumerate("ABCDEFGHIJKLMNOPQRSTUVWXYZ")},
    **{f"N{d}": 30 + (d - 1) % 10 for d in range(1, 10)}, "N0": 39,
    "RET": 0x28, "ESC": 0x29, "BSPC": 0x2A, "TAB": 0x2B, "SPACE": 0x2C,
    "MINUS": 0x2D, "EQUAL": 0x2E, "LBKT": 0x2F, "RBKT": 0x30, "BSLH": 0x31,
    "SEMI": 0x33, "SQT": 0x34, "GRAVE": 0x35, "COMMA": 0x36, "DOT": 0x37, "SLASH": 0x38,
    **{f"F{i}": 0x3A + i - 1 for i in range(1, 13)},
    "PSCRN": 0x46, "SLCK": 0x47, "PAUSE_BREAK": 0x48, "INS": 0x49, "HOME": 0x4A,
    "PG_UP": 0x4B, "DEL": 0x4C, "END": 0x4D, "PG_DN": 0x4E, "RIGHT": 0x4F,
    "LEFT": 0x50, "DOWN": 0x51, "UP": 0x52, "K_APP": 0x65,
    "K_AGAIN": 0x79, "K_UNDO": 0x7A, "K_CUT": 0x7B, "K_COPY": 0x7C, "K_PASTE": 0x7D,
    "LCTRL": 0xE0, "LSHFT": 0xE1, "LALT": 0xE2, "LGUI": 0xE3,
    "RCTRL": 0xE4, "RSHFT": 0xE5, "RALT": 0xE6, "RGUI": 0xE7,
}
MODBIT = {"LC": "MOD_LCTRL", "LS": "MOD_LSHFT", "LA": "MOD_LALT", "LG": "MOD_LGUI"}
SHIFTED = {  # ZMK names that are Shift + another key
    "AMPS": "N7", "ASTRK": "N8", "LPAR": "N9", "RPAR": "N0", "LBRC": "LBKT",
    "RBRC": "RBKT", "COLON": "SEMI", "DLLR": "N4", "PRCNT": "N5", "CARET": "N6",
    "PLUS": "EQUAL", "TILDE": "GRAVE", "EXCL": "N1", "AT": "N2", "HASH": "N3",
    "PIPE": "BSLH", "UNDER": "MINUS",
}
CONSUMER = {"C_PREV": 0xB6, "C_NEXT": 0xB5, "C_VOL_UP": 0xE9, "C_VOL_DN": 0xEA,
            "C_PP": 0xCD, "C_MUTE": 0xE2, "C_STOP": 0xB7}
# Miryoku U_* clipboard defaults (miryoku_clipboard.h, no MIRYOKU_CLIPBOARD_* set)
ALIASES = {"U_RDO": "&kp K_AGAIN", "U_PST": "&kp LS(INS)", "U_CPY": "&kp LC(INS)",
           "U_CUT": "&kp LS(DEL)", "U_UND": "&kp K_UNDO"}
NONE = {"U_NP", "U_NA", "U_NU", "&none"}
SPECIAL = {  # firmware-handled actions
    "U_BOOT": "BOOT", "&u_caps_word": "CAPS_WORD",
    "U_BTN1": "MOUSE_BTN1", "U_BTN2": "MOUSE_BTN2", "U_BTN3": "MOUSE_BTN3",
    "U_MS_L": "MOUSE_LEFT", "U_MS_R": "MOUSE_RIGHT", "U_MS_U": "MOUSE_UP", "U_MS_D": "MOUSE_DOWN",
    "U_WH_L": "WHEEL_LEFT", "U_WH_R": "WHEEL_RIGHT", "U_WH_U": "WHEEL_UP", "U_WH_D": "WHEEL_DOWN",
    "U_RGB_TOG": "RGB_TOG", "U_RGB_EFF": "RGB_EFF", "U_RGB_HUI": "RGB_HUI",
    "U_RGB_SAI": "RGB_SAI", "U_RGB_BRI": "RGB_BRI", "U_EP_TOG": "EXT_POWER_TOG",
    "&u_out_tog": "OUTPUT_TOG",
    **{f"&u_bt_sel_{i}": f"BT_SEL_{i}" for i in range(4)},
}
SPECIALS_USED = []

# ---- on-screen labels (short, fit a ~140 px wide key at font size 28) ----
LABEL = {
    **{c: c for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"},
    **{f"N{d}": str(d) for d in range(10)},
    "RET": "Ent", "ESC": "Esc", "BSPC": "Bksp", "TAB": "Tab", "SPACE": "Space",
    "MINUS": "-", "EQUAL": "=", "LBKT": "[", "RBKT": "]", "BSLH": "\\", "SEMI": ";", "SQT": "'",
    "GRAVE": "`", "COMMA": ",", "DOT": ".", "SLASH": "/",
    **{f"F{i}": f"F{i}" for i in range(1, 13)},
    "PSCRN": "PrtSc", "SLCK": "ScrLk", "PAUSE_BREAK": "Pause", "INS": "Ins", "HOME": "Home",
    "PG_UP": "PgUp", "DEL": "Del", "END": "End", "PG_DN": "PgDn", "RIGHT": "Right",
    "LEFT": "Left", "DOWN": "Down", "UP": "Up", "K_APP": "Menu",
    "K_AGAIN": "Redo", "K_UNDO": "Undo", "K_CUT": "Cut", "K_COPY": "Copy", "K_PASTE": "Paste",
    "LCTRL": "Ctrl", "LSHFT": "Shft", "LALT": "Alt", "LGUI": "Gui",
    "RCTRL": "Ctrl", "RSHFT": "Shft", "RALT": "AltGr", "RGUI": "Gui",
    "AMPS": "&", "ASTRK": "*", "LPAR": "(", "RPAR": ")", "LBRC": "{", "RBRC": "}", "COLON": ":",
    "DLLR": "$", "PRCNT": "%", "CARET": "^", "PLUS": "+", "TILDE": "~", "EXCL": "!", "AT": "@",
    "HASH": "#", "PIPE": "|", "UNDER": "_",
    "C_PREV": "Prev", "C_NEXT": "Next", "C_VOL_UP": "Vol+", "C_VOL_DN": "Vol-",
    "C_PP": "Play", "C_MUTE": "Mute", "C_STOP": "Stop",
}
HOLD_LABEL = {"LGUI": "Gui", "LALT": "Alt", "LCTRL": "Ctrl", "LSHFT": "Shft", "RALT": "AltGr",
              "RCTRL": "Ctrl", "RSHFT": "Shft", "RGUI": "Gui"}
ALIAS_LABEL = {"U_RDO": "Redo", "U_PST": "Paste", "U_CPY": "Copy", "U_CUT": "Cut", "U_UND": "Undo"}
SPECIAL_LABEL = {
    "BOOT": "Boot", "CAPS_WORD": "CapsW", "MOUSE_BTN1": "Clk L", "MOUSE_BTN2": "Clk R",
    "MOUSE_BTN3": "Clk M", "MOUSE_LEFT": "Ms L", "MOUSE_RIGHT": "Ms R", "MOUSE_UP": "Ms U",
    "MOUSE_DOWN": "Ms D", "WHEEL_LEFT": "Wh L", "WHEEL_RIGHT": "Wh R", "WHEEL_UP": "Wh U",
    "WHEEL_DOWN": "Wh D", "RGB_TOG": "RGB", "RGB_EFF": "Fx", "RGB_HUI": "Hue", "RGB_SAI": "Sat",
    "RGB_BRI": "Brt", "EXT_POWER_TOG": "Pwr", "OUTPUT_TOG": "Out",
    **{f"BT_SEL_{i}": f"BT {i}" for i in range(4)},
}


def label_of(tok, layer_names):
    """ZMK binding token -> (main, sub) label strings."""
    if tok in ALIAS_LABEL:
        return ALIAS_LABEL[tok], ""
    if tok in NONE:
        return "", ""
    if tok in SPECIAL:
        return SPECIAL_LABEL[SPECIAL[tok]], ""
    m = re.fullmatch(r"&kp\s+(.+)", tok)
    if m:
        return LABEL[m.group(1).strip()], ""
    m = re.fullmatch(r"U_MT\((\w+),\s*(\w+)\)", tok)
    if m:
        return LABEL[m.group(2)], HOLD_LABEL[m.group(1)]
    m = re.fullmatch(r"U_LT\(U_(\w+),\s*(\w+)\)", tok)
    if m:
        return LABEL[m.group(2)], layer_names[m.group(1)]
    m = re.fullmatch(r"&u_to_U_(\w+)", tok)
    if m:
        return layer_names[m.group(1)], "2x tap"
    sys.exit(f"no label for: {tok}")


def cstr(t):
    return '"' + t.replace("\\", "\\\\").replace('"', '\\"') + '"'


def split_top(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def keycode(expr):
    """ZMK key expression -> C++ Keycode expression."""
    expr = expr.strip()
    m = re.fullmatch(r"(L[CSAG])\((.+)\)", expr)
    if m:
        inner = keycode(m.group(2))
        return f"({inner} | (Keycode({MODBIT[m.group(1)]}) << 24))"
    if expr in CONSUMER:
        return f"consumer(0x{CONSUMER[expr]:02X} /*{expr}*/)"
    if expr in SHIFTED:
        return f"({keycode(SHIFTED[expr])} | (Keycode(MOD_LSHFT) << 24))"
    if expr in HID:
        return f"kbd(0x{HID[expr]:02X} /*{expr}*/)"
    sys.exit(f"unknown key name: {expr}")


def binding(tok, layers):
    tok = ALIASES.get(tok, tok)
    if tok in NONE:
        return "NONE()"
    if tok in SPECIAL:
        if SPECIAL[tok] not in SPECIALS_USED:
            SPECIALS_USED.append(SPECIAL[tok])
        return f"SP(Special::{SPECIAL[tok]})"
    m = re.fullmatch(r"&kp\s+(.+)", tok)
    if m:
        return f"K({keycode(m.group(1))})"
    m = re.fullmatch(r"U_MT\((\w+),\s*(\w+)\)", tok)
    if m:
        return f"MT({keycode(m.group(1))}, {keycode(m.group(2))})"
    m = re.fullmatch(r"U_LT\(U_(\w+),\s*(\w+)\)", tok)
    if m:
        return f"LT({layers.index(m.group(1))} /*{m.group(1)}*/, {keycode(m.group(2))})"
    m = re.fullmatch(r"&u_to_U_(\w+)", tok)
    if m:
        return f"TO({layers.index(m.group(1))} /*{m.group(1)}*/)"
    sys.exit(f"unhandled binding: {tok}")


def macro_body(text, name):
    m = re.search(r"#define " + re.escape(name) + r"\s*\\\n(.*?)(?:\n\s*\n|\Z)", text, re.S)
    if not m:
        sys.exit(f"macro not found: {name}")
    return " ".join(line.rstrip("\\").strip() for line in m.group(1).splitlines())


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    fork, out = Path(sys.argv[1]), Path(sys.argv[2])
    alphas = sys.argv[3] if len(sys.argv) > 3 else "QWERTY"
    babel = fork / "miryoku" / "miryoku_babel"
    layers = re.findall(r'MIRYOKU_X\((\w+),\s*"(\w+)"\)', (babel / "miryoku_layer_list.h").read_text())
    layer_ids = [l for l, _ in layers]
    alt = (babel / "miryoku_layer_alternatives.h").read_text()
    # Same defaults as miryoku_layer_selection.h with MIRYOKU_ALPHAS_<alphas>.
    source = {
        "BASE": f"MIRYOKU_ALTERNATIVES_BASE_{alphas}", "EXTRA": "MIRYOKU_ALTERNATIVES_BASE_QWERTY",
        "TAP": f"MIRYOKU_ALTERNATIVES_TAP_{alphas}", "BUTTON": "MIRYOKU_ALTERNATIVES_BUTTON",
        "NAV": "MIRYOKU_ALTERNATIVES_NAV", "MOUSE": "MIRYOKU_ALTERNATIVES_MOUSE",
        "MEDIA": "MIRYOKU_ALTERNATIVES_MEDIA", "NUM": "MIRYOKU_ALTERNATIVES_NUM",
        "SYM": "MIRYOKU_ALTERNATIVES_SYM", "FUN": "MIRYOKU_ALTERNATIVES_FUN",
    }
    rows = []
    for lid in layer_ids:
        toks = split_top(macro_body(alt, source[lid]))
        if len(toks) != 40:
            sys.exit(f"{lid}: expected 40 tokens, got {len(toks)}")
        # Drop the 4 unused thumb slots (U_NP) to get Miryoku's 36 keys.
        keep = toks[:30] + toks[32:38]
        names = dict(layers)
        rows.append((lid, [binding(t, layer_ids) for t in keep], [label_of(t, names) for t in keep]))

    o = ["// GENERATED by tools/gen_keymap.py from " + str(fork.name) + " - do not edit.",
         f"// Alphas: {alphas}. Positions: 0-9 top, 10-19 home, 20-29 bottom, 30-35 thumbs.",
         "#pragma once", '#include "keymap_types.h"', "namespace kb {", "",
         "enum class Special : uint16_t {"]
    o += [f"  {s}," for s in SPECIALS_USED] + ["};", ""]
    o += [f"enum Layer : uint8_t {{ " + ", ".join(f"L_{l} = {i}" for i, l in enumerate(layer_ids)) + " };", ""]
    o.append("inline const Binding kMiryokuBindings[] = {")
    for lid, bs, _ in rows:
        o.append(f"  // ---- {lid} ----")
        for r in range(0, 36, 5):
            o.append("  " + ", ".join(bs[r:r + 5]) + ",")
    o += ["};", "", "inline const KeyLabel kMiryokuLabels[] = {"]
    for lid, _, ls in rows:
        o.append(f"  // ---- {lid} ----")
        for r in range(0, 36, 5):
            o.append("  " + ", ".join("{%s, %s}" % (cstr(a), cstr(b)) for a, b in ls[r:r + 5]) + ",")
    o += ["};", "", "inline const char* const kMiryokuLayerNames[] = {"]
    o += [f'  "{name}",' for _, name in layers] + ["};", ""]
    o.append(f"inline const Keymap kMiryokuKeymap = {{{len(layers)}, kMiryokuBindings, kMiryokuLayerNames, kMiryokuLabels}};")
    o += ["", "}  // namespace kb", ""]
    out.write_text("\n".join(o))
    print(f"wrote {out}: {len(layers)} layers, {len(SPECIALS_USED)} special actions")


main()
