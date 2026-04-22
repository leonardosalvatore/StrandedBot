#ifndef UI_THEME_H
#define UI_THEME_H

#include "raylib.h"

/* Apply the "hacker terminal" raygui style globally: bright green on near-
 * black with big pixelated text. Must be called after InitWindow(). */
void ui_theme_apply(void);

/* Decorative helpers for the in-game HUD. Call between frame begin/end. */

/* Semi-transparent horizontal scanlines over the given rectangle — cheap
 * CRT vibe that doesn't hurt readability. */
void ui_theme_draw_scanlines(Rectangle r);

/* Draw a hacker-style frame: dim translucent fill, bright double-line border
 * with corner brackets, and a title label in the top-left. */
void ui_theme_draw_frame(Rectangle r, const char *title);

/* Same as ui_theme_draw_frame but with a caller-supplied fill color. Use
 * this for panels that need a different transparency than the shared UI_BG
 * (e.g. the SYSLOG panel, which sits over the map and wants to show more
 * of it through the background). */
void ui_theme_draw_frame_ex(Rectangle r, const char *title, Color bg);

/* The monospace font loaded by ui_theme_apply(). Falls back to
 * GetFontDefault() if no system TTF could be loaded. */
Font ui_theme_font(void);
int  ui_theme_font_size(void);

/* Release any resources allocated by ui_theme_apply(). */
void ui_theme_unload(void);

/* Named colors for any custom widgets that don't go through raygui. */
extern const Color UI_BG;
extern const Color UI_FG;
extern const Color UI_FG_DIM;
extern const Color UI_FG_BRIGHT;

#endif /* UI_THEME_H */
