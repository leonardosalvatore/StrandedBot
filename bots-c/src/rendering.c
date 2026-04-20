#include "rendering.h"
#include "message_log.h"
#include <math.h>
#include <string.h>

/* ── Sprite sheet ────────────────────────────────────────────────────────── */
static Texture2D sprite_sheet = {0};
static bool      sprite_loaded = false;
static bool      sprite_failed = false;
#define SPRITE_SIZE 150

static const int state_sprite_col[] = {
    [BOT_WAITING]   = 0, [BOT_THINKING]  = 1, [BOT_MOVING]    = 2,
    [BOT_LOOKCLOSE] = 0, [BOT_LOOKFAR]   = 1, [BOT_CHARGING]  = 2,
};
static const int state_sprite_row[] = {
    [BOT_WAITING]   = 0, [BOT_THINKING]  = 0, [BOT_MOVING]    = 0,
    [BOT_LOOKCLOSE] = 1, [BOT_LOOKFAR]   = 1, [BOT_CHARGING]  = 1,
};

void rendering_init(void) {
    /* Try several candidate paths for the sprite sheet */
    static const char *candidates[] = {
        "bots.png",
        "../resources/bots.png",
        "resources/bots.png",
        "../bots.png",
        "../../resources/bots.png",
        NULL,
    };
    for (const char **p = candidates; *p; p++) {
        if (FileExists(*p)) {
            sprite_sheet = LoadTexture(*p);
            sprite_loaded = sprite_sheet.id != 0;
            if (sprite_loaded) {
                msg_log("  [Sprite] Loaded spritesheet from %s", *p);
                break;
            }
        }
    }
    if (!sprite_loaded) {
        msg_log("  [Sprite] bots.png not found; using circle fallback");
        sprite_failed = true;
    }
}

void rendering_unload(void) {
    if (sprite_loaded) UnloadTexture(sprite_sheet);
}

/* ── Darken helper ───────────────────────────────────────────────────────── */
static Color darken(Color c, float factor) {
    return (Color){
        (unsigned char)(c.r * factor),
        (unsigned char)(c.g * factor),
        (unsigned char)(c.b * factor),
        c.a
    };
}

static Color tile_to_color(Tile *t) {
    if (t->fog)
        return (Color){t->r / 3, t->g / 3, t->b / 3, 255};
    return (Color){t->r, t->g, t->b, 255};
}

/* ── Tile overlay drawing ────────────────────────────────────────────────── */
static void draw_rock_triangle(Rectangle r, Color color) {
    if (r.width < 6 || r.height < 6) return;
    Color mark = darken(color, 0.62f);
    float cx = r.x + r.width / 2;
    float top_y = r.y + 2;
    float base_y = r.y + r.height / 3;
    if (base_y > r.y + r.height - 2) base_y = r.y + r.height - 2;
    float hb = r.width / 5;
    if (hb < 1) hb = 1;
    Vector2 v1 = {cx, top_y};
    Vector2 v2 = {cx - hb, base_y};
    Vector2 v3 = {cx + hb, base_y};
    DrawTriangle(v1, v3, v2, mark);
}

static void draw_sand_dots(Rectangle r, Color color, int tx, int ty, int count) {
    if (r.width < 6 || r.height < 6) return;
    Color mark = darken(color, 0.7f);
    unsigned int seed = ((unsigned int)(tx * 73856093) ^ (unsigned int)(ty * 19349663));
    for (int i = 0; i < count; i++) {
        int px = (int)r.x + 2 + (int)((seed >> (i * 5)) % (int)(r.width - 4));
        int py = (int)r.y + 2 + (int)((seed >> (i * 7 + 2)) % (int)(r.height - 4));
        DrawPixel(px, py, mark);
    }
}

static void draw_habitat_circle(Rectangle r, Color color, bool fill_blue) {
    if (r.width < 6 || r.height < 6) return;
    float rad = fminf(r.width, r.height) / 5;
    if (rad < 1) rad = 1;
    float cx = r.x + r.width / 2, cy = r.y + r.height / 2;
    if (fill_blue) DrawCircle((int)cx, (int)cy, rad, (Color){72, 130, 220, 255});
    DrawCircleLines((int)cx, (int)cy, rad, darken(color, 0.6f));
}

static void draw_battery_marker(Rectangle r, Color color) {
    if (r.width < 6 || r.height < 6) return;
    Color bat = {205, 210, 215, 255};
    Color edge = darken(bat, 0.78f);
    float pad = fminf(r.width, r.height) / 14;
    if (pad < 1) pad = 1;
    Rectangle body = {r.x + pad, r.y + r.height / 6 + pad,
                      r.width - 2 * pad, r.height - r.height / 6 - 2 * pad};
    DrawRectangleRounded(body, 0.15f, 4, bat);
    DrawRectangleRoundedLines(body, 0.15f, 4, edge);
    /* Terminal nub */
    float tw = body.width * 0.45f, th = body.height * 0.15f;
    DrawRectangle((int)(body.x + body.width / 2 - tw / 2), (int)(r.y + pad),
                  (int)tw, (int)th, bat);
    /* Lightning bolt */
    Color bolt = {245, 248, 250, 255};
    float bx = body.x + 2, by = body.y + 2;
    float bw = body.width - 4, bh = body.height - 4;
    Vector2 pts[6] = {
        {bx + bw, by}, {bx, by + bh / 2 - 1}, {bx + bw / 2 + 2, by + bh / 2 - 1},
        {bx, by + bh}, {bx + bw - 2, by + bh / 2}, {bx + bw / 2, by + bh / 2},
    };
    DrawTriangle(pts[0], pts[1], pts[2], bolt);
    DrawTriangle(pts[2], pts[3], pts[4], bolt);
}

static void draw_solar_grid(Rectangle r, Color color) {
    if (r.width < 10 || r.height < 10) return;
    Color grid = darken(color, 0.5f);
    int inset = 2;
    int lx = (int)r.x + inset, ly = (int)r.y + inset;
    int w = (int)r.width - 2 * inset, h = (int)r.height - 2 * inset;
    DrawRectangleLines(lx, ly, w, h, grid);
    for (int i = 1; i < 9; i++) {
        int px = lx + i * w / 9;
        int py = ly + i * h / 9;
        DrawLine(px, ly, px, ly + h - 1, grid);
        DrawLine(lx, py, lx + w - 1, py, grid);
    }
}

/* ── Main draw ───────────────────────────────────────────────────────────── */
void draw_game(void) {
    ClearBackground(BLACK);

    int gx, gy;
    gl_bot_grid_pos(&gx, &gy);
    float cam_world_x = gl_bot_x + TILE_SIZE / 2.0f;
    float cam_world_y = gl_bot_y + TILE_SIZE / 2.0f;
    float scale = (float)DRAW_TILE_SIZE / TILE_SIZE;

    int half_w = VIEWPORT_TILES_W / 2;
    int half_h = VIEWPORT_TILES_H / 2;
    int tx_start = gx - half_w; if (tx_start < 0) tx_start = 0;
    int tx_end   = gx + half_w + 1; if (tx_end > GRID_WIDTH) tx_end = GRID_WIDTH;
    int ty_start = gy - half_h; if (ty_start < 0) ty_start = 0;
    int ty_end   = gy + half_h + 1; if (ty_end > GRID_HEIGHT) ty_end = GRID_HEIGHT;

    /* Draw tiles */
    pthread_mutex_lock(&gl_tiles_lock);
    for (int tx = tx_start; tx < tx_end; tx++) {
        for (int ty = ty_start; ty < ty_end; ty++) {
            Tile *t = &gl_tile_matrix[tx][ty];
            Color c = tile_to_color(t);
            float wx = tx * TILE_SIZE;
            float wy = ty * TILE_SIZE;
            float sx = (wx - cam_world_x) * scale + WIN_WIDTH / 2.0f;
            float sy = (wy - cam_world_y) * scale + WIN_HEIGHT / 2.0f;
            if (sx < -DRAW_TILE_SIZE || sx > WIN_WIDTH || sy < -DRAW_TILE_SIZE || sy > WIN_HEIGHT)
                continue;
            Rectangle rect = {sx, sy, DRAW_TILE_SIZE, DRAW_TILE_SIZE};
            DrawRectangleRec(rect, c);

            /* Accent lines */
            if (rect.width >= 4 && rect.height >= 4) {
                Color accent = darken(c, 0.82f);
                DrawLine((int)rect.x + 1, (int)(rect.y + rect.height - 2),
                         (int)(rect.x + rect.width - 2), (int)(rect.y + rect.height - 2), accent);
                DrawLine((int)rect.x + 1, (int)(rect.y + 1),
                         (int)rect.x + 1, (int)(rect.y + rect.height - 2), accent);
            }

            switch (t->type) {
                case TILE_ROCKS:          draw_rock_triangle(rect, c); break;
                case TILE_SAND:           draw_sand_dots(rect, c, tx, ty, 3); break;
                case TILE_GRAVEL:         draw_sand_dots(rect, c, tx, ty, 5); break;
                case TILE_WATER:          draw_sand_dots(rect, c, tx, ty, 3); break;
                case TILE_HABITAT:        draw_habitat_circle(rect, c, true); break;
                case TILE_BROKEN_HABITAT: draw_habitat_circle(rect, c, false); break;
                case TILE_BATTERY:        draw_battery_marker(rect, c); break;
                case TILE_SOLAR_PANEL:    draw_solar_grid(rect, c); break;
                default: break;
            }
        }
    }
    pthread_mutex_unlock(&gl_tiles_lock);

    /* Draw bot */
    float bot_cx = gl_bot_x + TILE_SIZE / 2.0f;
    float bot_cy = gl_bot_y + TILE_SIZE / 2.0f;
    float bsx = (bot_cx - cam_world_x) * scale + WIN_WIDTH / 2.0f;
    float bsy = (bot_cy - cam_world_y) * scale + WIN_HEIGHT / 2.0f;

    if (sprite_loaded && !sprite_failed) {
        int col = state_sprite_col[gl_bot_state < BOT_STATE_COUNT ? gl_bot_state : 0];
        int row = state_sprite_row[gl_bot_state < BOT_STATE_COUNT ? gl_bot_state : 0];
        Rectangle src = {(float)(col * SPRITE_SIZE), (float)(row * SPRITE_SIZE),
                         SPRITE_SIZE, SPRITE_SIZE};
        int draw_sz = (int)(BOT_RADIUS * 6 * scale / 4);
        if (draw_sz < 8) draw_sz = 8;
        Rectangle dst = {bsx - draw_sz / 2.0f, bsy - draw_sz / 2.0f,
                         (float)draw_sz, (float)draw_sz};
        DrawTexturePro(sprite_sheet, src, dst, (Vector2){0, 0}, 0.0f, WHITE);
    } else {
        int r = (int)(BOT_RADIUS * scale);
        DrawCircle((int)bsx, (int)bsy, (float)r, (Color){0, 120, 255, 255});
    }

    /* Solar flare flash */
    if (gl_solar_flare_animation_active) {
        double elapsed = GetTime() - gl_solar_flare_animation_start_time;
        if (elapsed >= 2.0) {
            gl_solar_flare_animation_active = false;
        } else {
            double cycle = fmod(elapsed, 0.2) / 0.2;
            if (cycle < 0.5) {
                DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                              (Color){255, 255, 0, 200});
            }
        }
    }
}
