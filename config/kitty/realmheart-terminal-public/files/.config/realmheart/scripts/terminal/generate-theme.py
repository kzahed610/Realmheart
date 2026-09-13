#!/usr/bin/env python3
"""Realmheart terminal theme generator — Relic Grimoire v4.2 "Ribbon Cartouche Prime".

Reads the Realmheart/Matugen palette cache under XDG_STATE_HOME and emits the
live Kitty, Fish and Starship theme fragments.

N1 turns the prompt into a compact navigation instrument:
  * Aether Ribbon: project/scope -> parent cells -> highlighted current folder
  * Depth Compass: five-step peripheral nesting meter, relative to repo/home root
  * Context Cartouche: SSH / Python / Conda / Nix / container chips when relevant

Every ordinary foreground, background, ribbon cell, depth pip and context chip is
computed from the current Matugen palette. The only non-palette UI invariant is
the ceremonial gold root crown. This keeps the terminal visually synchronized
with wallpaper changes instead of freezing N1 into one showcase color scheme.
"""

import colorsys
import os
import re
import struct
import sys
import tempfile
import time
import zlib
from datetime import datetime

HOME = os.path.expanduser("~")
XDG_CONFIG_HOME = os.environ.get("XDG_CONFIG_HOME", os.path.join(HOME, ".config"))
XDG_STATE_HOME = os.environ.get("XDG_STATE_HOME", os.path.join(HOME, ".local", "state"))
STATE_DIR = os.path.join(XDG_STATE_HOME, "realmheart")
PALETTE_TSV = os.path.join(STATE_DIR, "theme-palette.tsv")
OUT_DIR = os.path.join(STATE_DIR, "theme")
OLD_STARSHIP = os.path.join(XDG_CONFIG_HOME, "starship.toml")

# Ceremonial invariant. Everything else is palette-derived.
IVORY = "#F5F2EA"
GOLD = "#FFD66B"

FALLBACK_PALETTE = {
    "accent": "#9ccbfb", "background": "#101418", "blue": "#b9c8da",
    "error": "#ffb4ab", "outline": "#8c9199", "primary": "#9ccbfb",
    "red": "#ffb4ab", "secondary": "#b9c8da", "surface": "#101418",
    "surface_variant": "#1c2024", "tertiary": "#d4bee6",
    "text": "#e2e2e6", "text_muted": "#c2c7cf",
}


# ── color helpers ────────────────────────────────────────────────────────────
def _rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) / 255 for i in (0, 2, 4))


def _hex(rgb):
    return "#{:02x}{:02x}{:02x}".format(
        *(max(0, min(255, round(c * 255))) for c in rgb)
    )


def mix(a, b, t):
    ca, cb = _rgb(a), _rgb(b)
    return _hex(tuple(x + (y - x) * t for x, y in zip(ca, cb)))


def adjust(c, sat=None, light=None, hue_deg=0.0):
    r, g, b = _rgb(c)
    h, l, s = colorsys.rgb_to_hls(r, g, b)
    h = (h + hue_deg / 360.0) % 1.0
    if sat is not None:
        s = min(1.0, max(0.0, s * (1 + sat) if s > 0 else sat))
    if light is not None:
        l = min(1.0, max(0.0, l * (1 + light)))
    return _hex(colorsys.hls_to_rgb(h, l, s))


def boost(c):
    return adjust(c, sat=0.10, light=0.08)


def _hls(hex_color):
    return colorsys.rgb_to_hls(*_rgb(hex_color))


def semantic_ansi(hue_deg, dominant, surface, bright=False):
    """Return a wallpaper-harmonized ANSI color without losing its meaning.

    v2 rotated the dominant hue to synthesize ANSI colors. On an orange
    wallpaper that made ANSI green become pink. v3 instead pins ANSI families
    to recognizable hue anchors, borrowing only saturation/lightness character
    from the wallpaper and a very small dominant tint.
    """
    _, dom_l, dom_s = _hls(dominant)
    _, surf_l, _ = _hls(surface)
    dark_bg = surf_l < 0.50
    sat = max(0.48, min(0.72, dom_s if dom_s > 0.12 else 0.58))
    if dark_bg:
        light = 0.76 if bright else max(0.62, min(0.70, dom_l + 0.12))
    else:
        light = 0.30 if bright else max(0.34, min(0.44, dom_l - 0.12))
    anchor = _hex(colorsys.hls_to_rgb((hue_deg % 360) / 360.0, light, sat))
    # 8% tint is enough to belong to the wallpaper without hue corruption.
    return mix(anchor, dominant, 0.08)


def stamp(pal, dominant):
    wall = ""
    try:
        with open(os.path.join(STATE_DIR, "wallpaper", "path.txt"), encoding="utf-8") as f:
            wall = os.path.basename(f.read().strip())
    except OSError:
        pass
    return (
        f"# palette stamp: dominant {dominant} | error {pal.get('error', '?')} "
        f"| wallpaper {wall or 'unknown'} | {datetime.now():%Y-%m-%d %H:%M}"
    )


# ── ghost-rail PNG (kitty background_image, clamped = anchored top-left) ────
def emit_rail_png(path, dom_hex, alpha=48, width=2, height=2160):
    """2px vertical rail in dominant color at ~19% alpha."""
    r, g, b = _rgb(dom_hex)
    rail = bytes((round(r * 255), round(g * 255), round(b * 255), alpha))
    row = rail * width

    def chunk(typ, data):
        return (
            struct.pack(">I", len(data)) + typ + data
            + struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF)
        )

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    idat = zlib.compress(b"".join(b"\x00" + row for _ in range(height)), 9)
    with open(path, "wb") as f:
        f.write(
            b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", idat) + chunk(b"IEND", b"")
        )


# ── palette input ────────────────────────────────────────────────────────────
def load_palette():
    """Read a complete Matugen cache, retrying briefly across non-atomic writes.

    A path unit can fire while a producer is still rewriting a file in place.
    If that happens, do *not* overwrite a beautiful live theme with a
    fallback/half-Matugen chimera: retry a handful of times and fail closed.
    Missing cache on first install still gets the built-in fallback palette.
    """
    if not os.path.exists(PALETTE_TSV):
        return dict(FALLBACK_PALETTE)

    for attempt in range(6):
        parsed = {}
        try:
            with open(PALETTE_TSV, encoding="utf-8") as f:
                for line in f:
                    parts = line.rstrip("\n").split("\t")
                    if len(parts) == 2 and re.fullmatch(
                        r"#[0-9a-fA-F]{6}", parts[1].strip()
                    ):
                        parsed[parts[0].strip()] = parts[1].strip().lower()
        except OSError:
            parsed = {}

        has_surface = "surface" in parsed or "background" in parsed
        has_primary = "primary" in parsed or "accent" in parsed
        has_text = "text" in parsed
        if has_surface and has_primary and has_text:
            pal = dict(FALLBACK_PALETTE)
            pal.update(parsed)
            return pal

        if attempt < 5:
            time.sleep(0.04)

    raise RuntimeError(
        "Matugen palette cache exists but is incomplete; preserving the current "
        "generated terminal theme instead of emitting fallback-mixed colors"
    )


def relative_luminance(color):
    def channel(v):
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
    r, g, b = _rgb(color)
    r, g, b = channel(r), channel(g), channel(b)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast_ratio(a, b):
    la, lb = relative_luminance(a), relative_luminance(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def readable_on(background, preferred, *fallbacks):
    """Pick the most palette-native readable foreground for a derived cell."""
    candidates = (preferred,) + fallbacks
    if contrast_ratio(preferred, background) >= 4.5:
        return preferred
    return max(candidates, key=lambda fg: contrast_ratio(fg, background))


def derive(pal):
    # Core colors come directly from the current Realmheart/Matugen cache.
    surface = pal.get("surface") or pal.get("background") or "#101418"
    surface_var = pal.get("surface_variant") or mix(
        surface, pal.get("text", "#e2e2e6"), 0.06
    )
    dominant = pal.get("primary") or pal.get("accent") or "#9ccbfb"
    error = pal.get("error") or pal.get("red") or "#ffb4ab"
    tertiary = pal.get("tertiary") or pal.get("secondary") or dominant
    secondary = pal.get("secondary") or pal.get("blue") or dominant
    outline = pal.get("outline") or mix(surface, pal.get("text", dominant), 0.45)
    text = pal.get("text") or mix(surface, dominant, 0.78)
    text_muted = pal.get("text_muted") or mix(text, outline, 0.55)

    dom_bright = boost(dominant)
    dom_soft = mix(dominant, surface, 0.35)
    ghost = mix(outline, surface, 0.24)
    pager_alt = mix(surface, surface_var, 0.55)

    # N1: subtle surface materials derived from the wallpaper palette.
    ribbon_scope_bg = mix(surface_var, dominant, 0.13)
    ribbon_parent_a_bg = mix(surface, surface_var, 0.74)
    ribbon_parent_b_bg = mix(surface, surface_var, 0.92)
    ribbon_current_bg = mix(surface_var, dominant, 0.30)

    ribbon_scope_fg = readable_on(
        ribbon_scope_bg, mix(text, dominant, 0.22), text, surface, text_muted
    )
    ribbon_parent_a_fg = readable_on(
        ribbon_parent_a_bg, text_muted, text, surface, dominant
    )
    ribbon_parent_b_fg = readable_on(
        ribbon_parent_b_bg, text_muted, text, surface, dominant
    )
    ribbon_current_fg = readable_on(
        ribbon_current_bg, mix(text, dominant, 0.10), text, surface, text_muted
    )

    # Context chips: remote state is intentionally a bit more salient.
    ctx_bg = mix(surface_var, secondary, 0.12)
    ctx_fg = readable_on(ctx_bg, text_muted, text, surface, secondary)
    ctx_hot_bg = mix(surface_var, tertiary, 0.22)
    ctx_hot_fg = readable_on(ctx_hot_bg, tertiary, text, surface, text_muted)
    ctx_container_bg = mix(surface_var, secondary, 0.18)
    ctx_container_fg = readable_on(
        ctx_container_bg, secondary, text, surface, text_muted
    )

    # Depth Compass: completed depth / current depth / remaining capacity.
    depth_on = mix(dominant, text, 0.20)
    depth_hot = dom_bright
    depth_off = mix(ghost, surface, 0.18)

    return {
        "dominant": dominant,
        "dom_bright": dom_bright,
        "dom_soft": dom_soft,
        "error": error,
        "error_bright": boost(error),
        "tertiary": tertiary,
        "secondary": secondary,
        "surface": surface,
        "surface_var": surface_var,
        "outline": outline,
        "text": text,
        "text_muted": text_muted,
        "ghost": ghost,
        "pager_alt": pager_alt,
        "crown": GOLD,
        "ribbon_scope_bg": ribbon_scope_bg,
        "ribbon_scope_fg": ribbon_scope_fg,
        "ribbon_parent_a_bg": ribbon_parent_a_bg,
        "ribbon_parent_a_fg": ribbon_parent_a_fg,
        "ribbon_parent_b_bg": ribbon_parent_b_bg,
        "ribbon_parent_b_fg": ribbon_parent_b_fg,
        "ribbon_current_bg": ribbon_current_bg,
        "ribbon_current_fg": ribbon_current_fg,
        "depth_on": depth_on,
        "depth_hot": depth_hot,
        "depth_off": depth_off,
        "ctx_bg": ctx_bg,
        "ctx_fg": ctx_fg,
        "ctx_hot_bg": ctx_hot_bg,
        "ctx_hot_fg": ctx_hot_fg,
        "ctx_container_bg": ctx_container_bg,
        "ctx_container_fg": ctx_container_fg,
        # ANSI 16 keeps semantic hue families recognizable while borrowing
        # saturation/lightness character from the wallpaper.
        "term0": surface,
        "term1": error,
        "term2": semantic_ansi(132, dominant, surface),
        "term3": semantic_ansi(46, dominant, surface),
        "term4": semantic_ansi(214, dominant, surface),
        "term5": semantic_ansi(300, dominant, surface),
        "term6": semantic_ansi(184, dominant, surface),
        "term7": mix(text, outline, 0.16),
        "term8": mix(surface, outline, 0.62),
        "term9": boost(error),
        "term10": semantic_ansi(132, dominant, surface, bright=True),
        "term11": semantic_ansi(46, dominant, surface, bright=True),
        "term12": semantic_ansi(214, dominant, surface, bright=True),
        "term13": semantic_ansi(300, dominant, surface, bright=True),
        "term14": semantic_ansi(184, dominant, surface, bright=True),
        # Even ANSI bright white follows the current Matugen text color.
        "term15": mix(text, dominant, 0.04),
    }

# ── emitters ─────────────────────────────────────────────────────────────────
def emit_kitty(c, stamp_line):
    lines = [
        "# ── Realmheart terminal theme (generated) — do not edit by hand ──",
        stamp_line,
        "# ghost-rail: permanent faint spine in the left padding margin",
        f"background_image {OUT_DIR}/rail.png",
        "background_image_layout clamped",
        f"foreground {c['text']}",
        f"background {c['surface']}",
        f"selection_foreground {c['surface']}",
        f"selection_background {c['dom_bright']}",
        f"cursor {c['dom_bright']}",
        f"cursor_text_color {c['surface']}",
        f"url_color {c['dominant']}",
        f"active_border_color {c['dom_bright']}",
        f"inactive_border_color {c['surface_var']}",
        f"bell_border_color {c['error']}",
        f"active_tab_foreground {c['surface']}",
        f"active_tab_background {c['dom_bright']}",
        f"inactive_tab_foreground {c['outline']}",
        f"inactive_tab_background {c['surface']}",
        f"tab_bar_background {mix(c['surface'], '#000000', 0.35)}",
    ]
    lines += [f"color{i:<2} {c[f'term{i}']}" for i in range(16)]
    return "\n".join(lines) + "\n"


def fh(c):
    """Fish colors are bare hex — a leading '#' would start a comment."""
    return c.lstrip("#")


def fish_col(name, *spec):
    return f"set -g {name} {' '.join(spec)}"


def emit_fish(c, stamp_line):
    L = [
        "# ── Realmheart fish theme (generated) — do not edit by hand ──",
        stamp_line,
        # Command-line grammar: verbs > structure > data > comments.
        fish_col("fish_color_normal", fh(c["text"])),
        fish_col("fish_color_command", "--bold", fh(c["dominant"])),
        fish_col("fish_color_builtin", fh(c["dom_bright"])),
        fish_col("fish_color_function", fh(c["dominant"])),
        fish_col("fish_color_keyword", "--bold", fh(c["tertiary"])),
        fish_col("fish_color_param", fh(c["text"])),
        fish_col("fish_color_option", fh(c["secondary"])),
        fish_col("fish_color_quote", fh(c["tertiary"])),
        fish_col("fish_color_operator", fh(c["secondary"])),
        fish_col("fish_color_end", fh(c["secondary"])),
        fish_col("fish_color_redirection", "--bold", fh(c["secondary"])),
        fish_col("fish_color_error", "--bold", fh(c["error"])),
        fish_col("fish_color_exception", "--bold", fh(c["error"])),
        fish_col("fish_color_comment", "--italic", fh(c["outline"])),
        fish_col("fish_color_autosuggestion", fh(c["ghost"])),
        fish_col("fish_color_cwd", fh(c["dom_bright"])),
        fish_col("fish_color_cwd_root", "--bold", fh(c["crown"])),
        fish_col("fish_color_status", fh(c["error"])),
        fish_col("fish_color_user", fh(c["dom_bright"])),
        fish_col("fish_color_host", fh(c["text"])),
        fish_col("fish_color_host_remote", fh(c["tertiary"])),
        fish_col("fish_color_escape", fh(c["tertiary"])),
        fish_col("fish_color_cancel", "-r"),
        fish_col("fish_color_history_current", "--bold"),
        # Modifier-only means a valid path keeps its param/option foreground.
        fish_col("fish_color_valid_path", "--underline"),
        fish_col("fish_color_search_match", "--background=" + fh(c["surface_var"])),
        fish_col("fish_color_selection", "--bold", "--background=" + fh(c["surface_var"])),
        # Completion pager: explicit normal / alternate / selected states.
        fish_col("fish_pager_color_background", "--background=" + fh(c["surface"])),
        fish_col("fish_pager_color_completion", fh(c["text"])),
        fish_col("fish_pager_color_description", fh(c["text_muted"])),
        fish_col("fish_pager_color_prefix", fh(c["dominant"]), "--bold"),
        fish_col("fish_pager_color_progress", fh(c["surface"]), "--background=" + fh(c["dom_bright"])),
        fish_col("fish_pager_color_secondary_background", "--background=" + fh(c["pager_alt"])),
        fish_col("fish_pager_color_secondary_prefix", fh(c["dominant"]), "--bold"),
        fish_col("fish_pager_color_secondary_completion", fh(c["text"])),
        fish_col("fish_pager_color_secondary_description", fh(c["text_muted"])),
        fish_col("fish_pager_color_selected_background", "--background=" + fh(c["surface_var"])),
        fish_col("fish_pager_color_selected_prefix", fh(c["dom_bright"]), "--bold"),
        fish_col("fish_pager_color_selected_completion", fh(c["text"]), "--bold"),
        fish_col("fish_pager_color_selected_description", fh(c["tertiary"])),
        # Hooks consumed by config.fish transient rendering.
        fish_col("__rh_error", fh(c["error"])),
        fish_col("__rh_ghost", fh(c["ghost"])),
    ]
    return "\n".join(L) + "\n"


STARSHIP_TEMPLATE = r'''# ═══════════════════════════════════════════════════════════════════
#  REALMHEART · RELIC GRIMOIRE v4.2 "RIBBON CARTOUCHE PRIME" — generated
#
#  N1 anatomy:
#    1. AETHER RIBBON      — scope/repo > parents > highlighted current folder
#    2. DEPTH COMPASS      — five-step nesting meter relative to project/home
#    3. CONTEXT CARTOUCHE  — SSH/runtime/container chips only when relevant
#    4. RIGHT PERIPHERY    — git + duration; location keeps visual priority
#
#  Every ordinary UI color below is generated from the current Matugen palette.
#  Do not edit by hand — edit STARSHIP_TEMPLATE in generate-theme.py.
# ═══════════════════════════════════════════════════════════════════
@@STAMP@@

add_newline = false
palette = "realmheart"

format = "$status${env_var.RH_ROOT_SIGIL}${env_var.RH_USER_SIGIL}${env_var.RH_RIBBON_ROOT_ONLY}${env_var.RH_RIBBON_SCOPE}${env_var.RH_RIBBON_P1}${env_var.RH_RIBBON_P2}${env_var.RH_RIBBON_P3}${env_var.RH_RIBBON_P4}${env_var.RH_RIBBON_CURRENT_FROM_SCOPE}${env_var.RH_RIBBON_CURRENT_FROM_A}${env_var.RH_RIBBON_CURRENT_FROM_B}${env_var.RH_DEPTH_ON}${env_var.RH_DEPTH_HOT}${env_var.RH_DEPTH_OFF}${env_var.RH_CTX_REMOTE}${env_var.RH_CTX_ENV}${env_var.RH_CTX_CONTAINER}$fill$git_branch$git_status$git_state${env_var.RH_DURATION_OK}${env_var.RH_DURATION_ERR}$line_break$character"

[palettes.realmheart]
dom = "@@DOM@@"
dom_bright = "@@DOM_BRIGHT@@"
dom_soft = "@@DOM_SOFT@@"
error = "@@ERROR@@"
tertiary = "@@TERTIARY@@"
secondary = "@@SECONDARY@@"
text = "@@TEXT@@"
text_muted = "@@TEXT_MUTED@@"
ghost = "@@GHOST@@"
crown = "@@CROWN@@"
surface = "@@SURFACE@@"
surface_var = "@@SURFACE_VAR@@"
ribbon_scope_bg = "@@RIBBON_SCOPE_BG@@"
ribbon_scope_fg = "@@RIBBON_SCOPE_FG@@"
ribbon_parent_a_bg = "@@RIBBON_PARENT_A_BG@@"
ribbon_parent_a_fg = "@@RIBBON_PARENT_A_FG@@"
ribbon_parent_b_bg = "@@RIBBON_PARENT_B_BG@@"
ribbon_parent_b_fg = "@@RIBBON_PARENT_B_FG@@"
ribbon_current_bg = "@@RIBBON_CURRENT_BG@@"
ribbon_current_fg = "@@RIBBON_CURRENT_FG@@"
depth_on = "@@DEPTH_ON@@"
depth_hot = "@@DEPTH_HOT@@"
depth_off = "@@DEPTH_OFF@@"
ctx_bg = "@@CTX_BG@@"
ctx_fg = "@@CTX_FG@@"
ctx_hot_bg = "@@CTX_HOT_BG@@"
ctx_hot_fg = "@@CTX_HOT_FG@@"
ctx_container_bg = "@@CTX_CONTAINER_BG@@"
ctx_container_fg = "@@CTX_CONTAINER_FG@@"

# ── verdict spine ───────────────────────────────────────────────────────────
[status]
disabled = false
format = "$symbol"
success_symbol = "[│](bold fg:dom_bright) "
symbol = "[│](bold fg:error)[ ✗ $status](bold fg:error) "
not_executable_symbol = "[│](bold fg:error)[ ✗ perms](bold fg:error) "
not_found_symbol = "[│](bold fg:error)[ ✗ cmd](bold fg:error) "
sigint_symbol = "[│](bold fg:error)[ ✗ ^C](bold fg:error) "
signal_symbol = "[│](bold fg:error)[ ✗ $status](bold fg:error) "
recognize_signal_code = false
map_symbol = false

# ── identity sigil ──────────────────────────────────────────────────────────
[env_var.RH_USER_SIGIL]
format = "[$env_value](bold fg:dom_bright) "

[env_var.RH_ROOT_SIGIL]
format = "[$env_value](bold fg:crown) "

# ── Aether Ribbon ───────────────────────────────────────────────────────────
[env_var.RH_RIBBON_ROOT_ONLY]
format = "[](fg:ribbon_current_bg)[$env_value](bold fg:ribbon_current_fg bg:ribbon_current_bg)[](fg:ribbon_current_bg) "

[env_var.RH_RIBBON_SCOPE]
format = "[](fg:ribbon_scope_bg)[$env_value](bold fg:ribbon_scope_fg bg:ribbon_scope_bg)"

[env_var.RH_RIBBON_P1]
format = "[](fg:ribbon_scope_bg bg:ribbon_parent_a_bg)[$env_value](fg:ribbon_parent_a_fg bg:ribbon_parent_a_bg)"

[env_var.RH_RIBBON_P2]
format = "[](fg:ribbon_parent_a_bg bg:ribbon_parent_b_bg)[$env_value](fg:ribbon_parent_b_fg bg:ribbon_parent_b_bg)"

[env_var.RH_RIBBON_P3]
format = "[](fg:ribbon_parent_b_bg bg:ribbon_parent_a_bg)[$env_value](fg:ribbon_parent_a_fg bg:ribbon_parent_a_bg)"

[env_var.RH_RIBBON_P4]
format = "[](fg:ribbon_parent_a_bg bg:ribbon_parent_b_bg)[$env_value](fg:ribbon_parent_b_fg bg:ribbon_parent_b_bg)"

# Fish exports exactly one current-cell module based on the preceding cell.
[env_var.RH_RIBBON_CURRENT_FROM_SCOPE]
format = "[](fg:ribbon_scope_bg bg:ribbon_current_bg)[$env_value](bold fg:ribbon_current_fg bg:ribbon_current_bg)[](fg:ribbon_current_bg) "

[env_var.RH_RIBBON_CURRENT_FROM_A]
format = "[](fg:ribbon_parent_a_bg bg:ribbon_current_bg)[$env_value](bold fg:ribbon_current_fg bg:ribbon_current_bg)[](fg:ribbon_current_bg) "

[env_var.RH_RIBBON_CURRENT_FROM_B]
format = "[](fg:ribbon_parent_b_bg bg:ribbon_current_bg)[$env_value](bold fg:ribbon_current_fg bg:ribbon_current_bg)[](fg:ribbon_current_bg) "

[directory]
disabled = true

# ── Depth Compass ───────────────────────────────────────────────────────────
[env_var.RH_DEPTH_ON]
format = "[$env_value](fg:depth_on)"

[env_var.RH_DEPTH_HOT]
format = "[$env_value](bold fg:depth_hot)"

[env_var.RH_DEPTH_OFF]
format = "[$env_value](fg:depth_off) "

# ── Context Cartouche ───────────────────────────────────────────────────────
[env_var.RH_CTX_REMOTE]
format = "[](fg:ctx_hot_bg)[ $env_value ](bold fg:ctx_hot_fg bg:ctx_hot_bg)[](fg:ctx_hot_bg) "

[env_var.RH_CTX_ENV]
format = "[](fg:ctx_bg)[ $env_value ](fg:ctx_fg bg:ctx_bg)[](fg:ctx_bg) "

[env_var.RH_CTX_CONTAINER]
format = "[](fg:ctx_container_bg)[ $env_value ](fg:ctx_container_fg bg:ctx_container_bg)[](fg:ctx_container_bg) "

# Elastic space: path/context left, git/timing right.
[fill]
symbol = " "
style = "fg:surface"

# ── peripheral metadata ─────────────────────────────────────────────────────
[git_branch]
symbol = "󰘬 "
style = "fg:tertiary"
format = "[$symbol$branch(:$remote_branch)]($style) "
truncation_length = 18
truncation_symbol = "…"

[git_status]
format = "[$ahead_behind](fg:tertiary) "
conflicted = ""
untracked = ""
stashed = ""
modified = ""
staged = ""
renamed = ""
deleted = ""
typechanged = ""
ahead = "⇡${count}"
behind = "⇣${count}"
diverged = "⇡${ahead_count}⇣${behind_count}"
up_to_date = ""

[git_state]
format = "[· $state( $progress_current/$progress_total)](bold fg:tertiary) "

[env_var.RH_DURATION_OK]
format = "[· $env_value](fg:dom_soft) "

[env_var.RH_DURATION_ERR]
format = "[· $env_value](bold fg:error) "

[cmd_duration]
disabled = true

# ── quest entry point ───────────────────────────────────────────────────────
[character]
success_symbol = "[│](bold fg:dom_bright) [╰─󰁔](bold fg:dom_bright) "
error_symbol = "[│](bold fg:error) [╰─󰁔](bold fg:error) "
vimcmd_symbol = "[│](bold fg:tertiary) [╰─󰁔](bold fg:tertiary) "

[package]
disabled = true
'''


def extract_substitutions():
    """Carry over the user's directory.substitutions icon block verbatim."""
    try:
        with open(OLD_STARSHIP, encoding="utf-8") as f:
            text = f.read()
        m = re.search(r"^\[directory\.substitutions\]\s*$((?:.+\n)+?)(?=^\[|\Z)", text, re.M)
        if m:
            return "[directory.substitutions]\n" + m.group(1).rstrip("\n") + "\n"
    except OSError:
        pass
    return ""


def emit_starship(c, stamp_line):
    out = STARSHIP_TEMPLATE
    for key, val in {
        "STAMP": stamp_line,
        "DOM": c["dominant"],
        "DOM_BRIGHT": c["dom_bright"],
        "DOM_SOFT": c["dom_soft"],
        "ERROR": c["error"],
        "TERTIARY": c["tertiary"],
        "SECONDARY": c["secondary"],
        "TEXT": c["text"],
        "TEXT_MUTED": c["text_muted"],
        "GHOST": c["ghost"],
        "CROWN": c["crown"],
        "SURFACE": c["surface"],
        "SURFACE_VAR": c["surface_var"],
        "RIBBON_SCOPE_BG": c["ribbon_scope_bg"],
        "RIBBON_SCOPE_FG": c["ribbon_scope_fg"],
        "RIBBON_PARENT_A_BG": c["ribbon_parent_a_bg"],
        "RIBBON_PARENT_A_FG": c["ribbon_parent_a_fg"],
        "RIBBON_PARENT_B_BG": c["ribbon_parent_b_bg"],
        "RIBBON_PARENT_B_FG": c["ribbon_parent_b_fg"],
        "RIBBON_CURRENT_BG": c["ribbon_current_bg"],
        "RIBBON_CURRENT_FG": c["ribbon_current_fg"],
        "DEPTH_ON": c["depth_on"],
        "DEPTH_HOT": c["depth_hot"],
        "DEPTH_OFF": c["depth_off"],
        "CTX_BG": c["ctx_bg"],
        "CTX_FG": c["ctx_fg"],
        "CTX_HOT_BG": c["ctx_hot_bg"],
        "CTX_HOT_FG": c["ctx_hot_fg"],
        "CTX_CONTAINER_BG": c["ctx_container_bg"],
        "CTX_CONTAINER_FG": c["ctx_container_fg"],
    }.items():
        out = out.replace(f"@@{key}@@", val)
    subs = extract_substitutions()
    if subs:
        out = out.replace("[fill]", subs + "\n[fill]", 1)
    return out


# ── main ─────────────────────────────────────────────────────────────────────
def atomic_write(path, content):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), prefix=".tmp-rh-theme-")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(content)
        os.replace(tmp, path)
    except BaseException:
        os.unlink(tmp)
        raise


def main():
    try:
        pal = load_palette()
    except RuntimeError as exc:
        print(f"[realmheart-theme] {exc}", file=sys.stderr)
        return 2
    c = derive(pal)
    stamp_line = stamp(pal, c["dominant"])
    atomic_write(os.path.join(OUT_DIR, "kitty-theme.conf"), emit_kitty(c, stamp_line))
    atomic_write(os.path.join(OUT_DIR, "fish-theme.fish"), emit_fish(c, stamp_line))
    atomic_write(os.path.join(OUT_DIR, "starship.toml"), emit_starship(c, stamp_line))
    emit_rail_png(os.path.join(OUT_DIR, "rail.png"), c["dom_bright"])
    print(
        f"[realmheart-theme] v4.2 Ribbon Cartouche Prime emitted 3 files + rail.png → {OUT_DIR} | "
        f"dominant {c['dominant']} (bright {c['dom_bright']}), error {c['error']}, "
        f"surface {c['surface']}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
