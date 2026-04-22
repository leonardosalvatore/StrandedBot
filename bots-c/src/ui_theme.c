#include "ui_theme.h"
#include "raygui.h"

#include <math.h>
#include <stdio.h>
#include <unistd.h>

/* ── Palette ──────────────────────────────────────────────────────────────── */
const Color UI_BG        = (Color){  5,  12,   8, 230 };
const Color UI_FG        = (Color){  0, 255, 140, 255 };
const Color UI_FG_DIM    = (Color){  0, 110,  60, 255 };
const Color UI_FG_BRIGHT = (Color){170, 255, 200, 255 };

/* Helper: pack an RGBA Color into the 0xRRGGBBAA int raygui expects. */
static int pack_rgba(Color c) {
    return ((int)c.r << 24) | ((int)c.g << 16) | ((int)c.b << 8) | (int)c.a;
}

/* ── Font loading ─────────────────────────────────────────────────────────── */
/* TTF size the glyph atlas is baked at. Rendering at the same size is pixel
 * perfect; going larger/smaller applies bilinear. 18 gives us a clearly
 * readable HUD without wasting space. */
#define UI_FONT_SIZE 18

static Font ui_font = {0};
static bool ui_font_loaded = false;

/* Try a list of common monospace TTFs on the system. First one that exists
 * and loads wins. Keeps the dependency opt-in: if none of these exist, we
 * stay on raylib's default bitmap font — the app still runs. */
static const char *k_font_candidates[] = {
    "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
    "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    NULL,
};

static void load_ui_font(void) {
    for (int i = 0; k_font_candidates[i]; i++) {
        if (access(k_font_candidates[i], R_OK) != 0) continue;
        /* Load the full ASCII range (codepoints NULL → default 95 glyphs). */
        ui_font = LoadFontEx(k_font_candidates[i], UI_FONT_SIZE, NULL, 0);
        if (ui_font.texture.id != 0 && ui_font.glyphCount > 0) {
            /* Bilinear keeps the TTF crisp if raygui ends up scaling it. */
            SetTextureFilter(ui_font.texture, TEXTURE_FILTER_BILINEAR);
            ui_font_loaded = true;
            TraceLog(LOG_INFO, "UI: loaded font %s", k_font_candidates[i]);
            return;
        }
    }
    TraceLog(LOG_INFO, "UI: no monospace TTF found, using default bitmap font");
}

Font ui_theme_font(void) {
    return ui_font_loaded ? ui_font : GetFontDefault();
}

int ui_theme_font_size(void) {
    return UI_FONT_SIZE;
}

void ui_theme_unload(void) {
    if (ui_font_loaded) {
        UnloadFont(ui_font);
        ui_font_loaded = false;
    }
}

/* ── Theme setup ──────────────────────────────────────────────────────────── */
void ui_theme_apply(void) {
    /* Base raylib text rendering: keep the bitmap default font sharp when
     * scaled up (no bilinear smoothing → chunky CRT pixels). This only
     * matters if the TTF load below fails and we fall back to default. */
    Font def = GetFontDefault();
    if (def.texture.id != 0) {
        SetTextureFilter(def.texture, TEXTURE_FILTER_POINT);
    }

    load_ui_font();
    if (ui_font_loaded) {
        /* Hand the real font to raygui so every control renders with it. */
        GuiSetFont(ui_font);
        GuiSetStyle(DEFAULT, TEXT_SIZE, UI_FONT_SIZE);
        GuiSetStyle(DEFAULT, TEXT_SPACING, 0);
        GuiSetStyle(DEFAULT, TEXT_LINE_SPACING, UI_FONT_SIZE + 4);
    } else {
        /* Default bitmap font fallback: the old chunky look. */
        GuiSetStyle(DEFAULT, TEXT_SIZE, 15);
        GuiSetStyle(DEFAULT, TEXT_SPACING, 1);
        GuiSetStyle(DEFAULT, TEXT_LINE_SPACING, 18);
    }
    GuiSetStyle(DEFAULT, BORDER_WIDTH, 1);

    /* Default colors (every control inherits unless overridden). */
    int fg       = pack_rgba(UI_FG);
    int fg_dim   = pack_rgba(UI_FG_DIM);
    int fg_hi    = pack_rgba(UI_FG_BRIGHT);
    int bg       = pack_rgba(UI_BG);
    int bg_solid = pack_rgba((Color){ 2, 8, 4, 255 });

    GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL,   fg);
    GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL,     bg);
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL,     fg);
    GuiSetStyle(DEFAULT, BORDER_COLOR_FOCUSED,  fg_hi);
    GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED,    bg);
    GuiSetStyle(DEFAULT, TEXT_COLOR_FOCUSED,    fg_hi);
    GuiSetStyle(DEFAULT, BORDER_COLOR_PRESSED,  fg_hi);
    GuiSetStyle(DEFAULT, BASE_COLOR_PRESSED,    pack_rgba((Color){0, 60, 30, 255}));
    GuiSetStyle(DEFAULT, TEXT_COLOR_PRESSED,    pack_rgba((Color){220, 255, 230, 255}));
    GuiSetStyle(DEFAULT, BORDER_COLOR_DISABLED, fg_dim);
    GuiSetStyle(DEFAULT, BASE_COLOR_DISABLED,   bg_solid);
    GuiSetStyle(DEFAULT, TEXT_COLOR_DISABLED,   fg_dim);
    GuiSetStyle(DEFAULT, LINE_COLOR,            fg);
    GuiSetStyle(DEFAULT, BACKGROUND_COLOR,      bg);

    /* Tighter control-level tweaks. */
    GuiSetStyle(BUTTON,   BORDER_WIDTH, 2);
    GuiSetStyle(TEXTBOX,  TEXT_PADDING, 4);
    GuiSetStyle(LABEL,    TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
}

/* ── Scanlines ────────────────────────────────────────────────────────────── */
void ui_theme_draw_scanlines(Rectangle r) {
    /* Draw a darkening 1px line every 3px — cheap and subtle. */
    Color line = (Color){ 0, 0, 0, 60 };
    for (int y = (int)r.y; y < (int)(r.y + r.height); y += 3) {
        DrawRectangle((int)r.x, y, (int)r.width, 1, line);
    }
}

/* ── Custom hacker frame ──────────────────────────────────────────────────── */
void ui_theme_draw_frame(Rectangle r, const char *title) {
    ui_theme_draw_frame_ex(r, title, UI_BG);
}

void ui_theme_draw_frame_ex(Rectangle r, const char *title, Color bg) {
    /* Translucent fill. */
    DrawRectangleRec(r, bg);

    /* Double border (bright outer + dim inner offset). */
    DrawRectangleLinesEx(r, 2.0f, UI_FG);
    Rectangle inner = { r.x + 4, r.y + 4, r.width - 8, r.height - 8 };
    DrawRectangleLinesEx(inner, 1.0f, UI_FG_DIM);

    /* Corner brackets. */
    float L = 10.0f;
    float t = 2.0f;
    /* Top-left */
    DrawRectangle((int)r.x,           (int)r.y,           (int)L, (int)t, UI_FG_BRIGHT);
    DrawRectangle((int)r.x,           (int)r.y,           (int)t, (int)L, UI_FG_BRIGHT);
    /* Top-right */
    DrawRectangle((int)(r.x + r.width - L), (int)r.y,           (int)L, (int)t, UI_FG_BRIGHT);
    DrawRectangle((int)(r.x + r.width - t), (int)r.y,           (int)t, (int)L, UI_FG_BRIGHT);
    /* Bottom-left */
    DrawRectangle((int)r.x,           (int)(r.y + r.height - t), (int)L, (int)t, UI_FG_BRIGHT);
    DrawRectangle((int)r.x,           (int)(r.y + r.height - L), (int)t, (int)L, UI_FG_BRIGHT);
    /* Bottom-right */
    DrawRectangle((int)(r.x + r.width - L), (int)(r.y + r.height - t), (int)L, (int)t, UI_FG_BRIGHT);
    DrawRectangle((int)(r.x + r.width - t), (int)(r.y + r.height - L), (int)t, (int)L, UI_FG_BRIGHT);

    /* Title plate: "[ title ]" in the top-left of the frame, on a solid bg
     * so it cuts cleanly through the border. */
    if (title && title[0]) {
        int fs = GuiGetStyle(DEFAULT, TEXT_SIZE);
        int sp = GuiGetStyle(DEFAULT, TEXT_SPACING);
        Font f = ui_theme_font();
        char buf[128];
        snprintf(buf, sizeof(buf), " [ %s ] ", title);
        Vector2 sz = MeasureTextEx(f, buf, (float)fs, (float)sp);
        float tx = r.x + 16;
        float ty = r.y - sz.y * 0.5f + 2;
        DrawRectangle((int)tx - 2, (int)ty, (int)sz.x + 4, (int)sz.y, (Color){2, 8, 4, 255});
        DrawTextEx(f, buf, (Vector2){tx, ty}, (float)fs, (float)sp, UI_FG_BRIGHT);
    }

    ui_theme_draw_scanlines(r);
}
