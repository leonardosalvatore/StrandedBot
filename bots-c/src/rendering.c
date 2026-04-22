#include "rendering.h"
#include "message_log.h"
#include "particles.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>   /* readlink */
#include <limits.h>   /* PATH_MAX */

/* ── Bot sprite sheet (bots.png) ─────────────────────────────────────────── */
static Texture2D bot_sprite = {0};
static bool      bot_sprite_loaded = false;
#define BOT_SPRITE_SIZE 150

static const int bot_state_col[] = {
    [BOT_WAITING]   = 0, [BOT_THINKING]  = 1, [BOT_MOVING]    = 2,
    [BOT_LOOKCLOSE] = 0, [BOT_LOOKFAR]   = 1, [BOT_CHARGING]  = 2,
    [BOT_DESTROYED] = 0,
};
static const int bot_state_row[] = {
    [BOT_WAITING]   = 0, [BOT_THINKING]  = 0, [BOT_MOVING]    = 0,
    [BOT_LOOKCLOSE] = 1, [BOT_LOOKFAR]   = 1, [BOT_CHARGING]  = 1,
    [BOT_DESTROYED] = 0,
};
#define BOT_STATE_SPRITES ((int)(sizeof(bot_state_col) / sizeof(bot_state_col[0])))

/* ── Tile atlas (tiles.png) ──────────────────────────────────────────────── */
/* 4x2 grid of 150x150 cells. TileType → (col,row) below. */
static Texture2D tiles_atlas = {0};
static bool      tiles_atlas_loaded = false;
#define TILES_CELL_PX 150

static const int tile_col[TILE_TYPE_COUNT] = {
    [TILE_GRAVEL] = 0, [TILE_SAND] = 1, [TILE_WATER] = 2, [TILE_ROCKS] = 3,
    [TILE_HABITAT] = 0, [TILE_BATTERY] = 1,
    [TILE_SOLAR_PANEL] = 2, [TILE_BROKEN_HABITAT] = 3,
};
static const int tile_row[TILE_TYPE_COUNT] = {
    [TILE_GRAVEL] = 0, [TILE_SAND] = 0, [TILE_WATER] = 0, [TILE_ROCKS] = 0,
    [TILE_HABITAT] = 1, [TILE_BATTERY] = 1,
    [TILE_SOLAR_PANEL] = 1, [TILE_BROKEN_HABITAT] = 1,
};

/* ── Camera state (mouse pan & zoom) ─────────────────────────────────────── */
static float   cam_pan_x = 0.0f;   /* world-space offset from bot */
static float   cam_pan_y = 0.0f;
static float   cam_zoom  = 1.0f;   /* multiplier applied on top of base scale */
static bool    cam_panning = false;
static Vector2 cam_pan_anchor = {0};

#define CAM_ZOOM_MIN  0.25f
#define CAM_ZOOM_MAX  8.0f
#define CAM_ZOOM_STEP 1.2f

/* ── Texture loading helpers ─────────────────────────────────────────────── */
/* Assets live next to the binary — CMake copies bots-c/resources/*.png into
 * $<TARGET_FILE_DIR:bots>/ at build time. Resolving relative to the exe (not
 * CWD) makes the binary work no matter where it's launched from. */
static Texture2D load_exe_relative(const char *filename, const char *label) {
    /* Hold the exe path in a buffer smaller than `full` so the concatenation
     * below can't exceed PATH_MAX and gcc's -Wformat-truncation stays quiet. */
    char exe_dir[PATH_MAX / 2];
    ssize_t n = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
    if (n <= 0) {
        msg_log("  [%s] readlink(/proc/self/exe) failed; cannot locate %s",
                label, filename);
        return (Texture2D){0};
    }
    exe_dir[n] = '\0';

    char *slash = strrchr(exe_dir, '/');
    if (slash) *slash = '\0';
    else       exe_dir[0] = '\0';

    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s",
             exe_dir[0] ? exe_dir : ".", filename);

    if (!FileExists(full)) {
        msg_log("  [%s] spritesheet not found next to binary: %s",
                label, full);
        return (Texture2D){0};
    }
    Texture2D tex = LoadTexture(full);
    if (tex.id == 0) {
        msg_log("  [%s] LoadTexture failed for %s", label, full);
    } else {
        msg_log("  [%s] Loaded spritesheet from %s", label, full);
    }
    return tex;
}

void rendering_init(void) {
    bot_sprite = load_exe_relative("bots.png", "Bots");
    bot_sprite_loaded = bot_sprite.id != 0;
    if (bot_sprite_loaded) SetTextureFilter(bot_sprite, TEXTURE_FILTER_BILINEAR);

    tiles_atlas = load_exe_relative("tiles.png", "Tiles");
    tiles_atlas_loaded = tiles_atlas.id != 0;
    /* Point filter on the tile atlas: bilinear sampling at the border of each
     * 150x150 cell bleeds pixels from the neighbouring cell, which shows up as
     * wrong-colour seams between tiles (most visible on water/sand edges).
     * Point filtering stops the cross-cell interpolation cold. */
    if (tiles_atlas_loaded) SetTextureFilter(tiles_atlas, TEXTURE_FILTER_POINT);
}

void rendering_unload(void) {
    if (bot_sprite_loaded)   UnloadTexture(bot_sprite);
    if (tiles_atlas_loaded)  UnloadTexture(tiles_atlas);
}

/* ── Camera input handling ───────────────────────────────────────────────── */
/* Updates cam_pan_x/y/zoom from input. No Camera2D is needed here; the caller
 * builds the Camera2D from the final state after this returns. */
static void update_camera_input(float base_scale, float bot_cx, float bot_cy,
                                Vector2 offset) {
    /* Reset. */
    if (IsKeyPressed(KEY_HOME) || IsKeyPressed(KEY_R)) {
        cam_pan_x = cam_pan_y = 0.0f;
        cam_zoom = 1.0f;
        cam_panning = false;
        return;
    }

    /* Wheel zoom, anchored under the cursor. */
    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) {
        Vector2 m = GetMousePosition();
        float old_zoom = base_scale * cam_zoom;
        /* Inverse of Camera2D projection (rotation = 0):
         *   world = (screen - offset) / zoom + target
         * where target = (bot_cx + cam_pan_x, bot_cy + cam_pan_y). */
        float world_before_x = (m.x - offset.x) / old_zoom + bot_cx + cam_pan_x;
        float world_before_y = (m.y - offset.y) / old_zoom + bot_cy + cam_pan_y;

        float new_mul = (wheel > 0) ? cam_zoom * CAM_ZOOM_STEP
                                    : cam_zoom / CAM_ZOOM_STEP;
        if (new_mul < CAM_ZOOM_MIN) new_mul = CAM_ZOOM_MIN;
        if (new_mul > CAM_ZOOM_MAX) new_mul = CAM_ZOOM_MAX;
        cam_zoom = new_mul;

        /* Solve for new pan so the pre-zoom world point stays under the cursor. */
        float new_zoom = base_scale * cam_zoom;
        cam_pan_x = world_before_x - (m.x - offset.x) / new_zoom - bot_cx;
        cam_pan_y = world_before_y - (m.y - offset.y) / new_zoom - bot_cy;

        /* If a drag is in progress, reset its anchor so the next delta is
         * computed relative to the post-zoom mouse position. */
        if (cam_panning) cam_pan_anchor = m;
    }

    /* Right/middle drag to pan. */
    bool drag_held = IsMouseButtonDown(MOUSE_BUTTON_RIGHT) ||
                     IsMouseButtonDown(MOUSE_BUTTON_MIDDLE);
    bool drag_pressed = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) ||
                        IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE);
    if (drag_pressed) {
        cam_panning = true;
        cam_pan_anchor = GetMousePosition();
    }
    if (cam_panning && drag_held) {
        Vector2 m = GetMousePosition();
        float zoom = base_scale * cam_zoom;
        cam_pan_x -= (m.x - cam_pan_anchor.x) / zoom;
        cam_pan_y -= (m.y - cam_pan_anchor.y) / zoom;
        cam_pan_anchor = m;
    }
    if (!drag_held) cam_panning = false;
}

/* ── Main draw ───────────────────────────────────────────────────────────── */
void draw_game(void) {
    ClearBackground(BLACK);

    /* Snapshot globals that the agent/logic threads can mutate, so the whole
     * frame sees a consistent view. gl_tiles_lock is the documented guard for
     * all shared world state (see game_logic.h). */
    pthread_mutex_lock(&gl_tiles_lock);
    float    snap_bot_x        = gl_bot_x;
    float    snap_bot_y        = gl_bot_y;
    BotState snap_state        = gl_bot_state;
    bool     snap_flare_active = gl_solar_flare_animation_active;
    double   snap_flare_start  = gl_solar_flare_animation_start_time;
    pthread_mutex_unlock(&gl_tiles_lock);

    float bot_cx = snap_bot_x + TILE_SIZE / 2.0f;
    float bot_cy = snap_bot_y + TILE_SIZE / 2.0f;
    float base_scale = (float)DRAW_TILE_SIZE / TILE_SIZE;
    Vector2 cam_offset = { WIN_WIDTH * 0.5f, WIN_HEIGHT * 0.5f };

    /* Update pan/zoom state from input before building the Camera2D. */
    update_camera_input(base_scale, bot_cx, bot_cy, cam_offset);

    Camera2D camera = {
        .offset   = cam_offset,
        .target   = { bot_cx + cam_pan_x, bot_cy + cam_pan_y },
        .rotation = 0.0f,
        .zoom     = base_scale * cam_zoom,
    };

    BeginMode2D(camera);

    /* Compute visible tile range in world coords via inverse projection. */
    Vector2 tl = GetScreenToWorld2D((Vector2){0, 0}, camera);
    Vector2 br = GetScreenToWorld2D((Vector2){WIN_WIDTH, WIN_HEIGHT}, camera);
    int tx_start = (int)floorf(tl.x / TILE_SIZE) - 1;
    int ty_start = (int)floorf(tl.y / TILE_SIZE) - 1;
    int tx_end   = (int)floorf(br.x / TILE_SIZE) + 2;
    int ty_end   = (int)floorf(br.y / TILE_SIZE) + 2;
    if (tx_start < 0)          tx_start = 0;
    if (ty_start < 0)          ty_start = 0;
    if (tx_end   > GRID_WIDTH) tx_end   = GRID_WIDTH;
    if (ty_end   > GRID_HEIGHT)ty_end   = GRID_HEIGHT;

    /* All tiles share a single texture → raylib batches them into one
     * GPU draw call. CMake copies tiles.png next to the binary, so this
     * branch always runs in a proper build. */
    if (tiles_atlas_loaded) {
        /* Two anti-seam tricks for atlases drawn through a Camera2D:
         *   1. src inset by 0.5 px: even with POINT filtering, sampling right
         *      at x = col*150 can nudge into the previous atlas column at
         *      certain zoom levels. A half-pixel margin is invisible for
         *      150-px art and eliminates the bleed.
         *   2. dst bleed by 1 world unit: adjacent tiles share an edge that
         *      rasterises differently on either side at non-integer zoom,
         *      leaving 1-pixel gaps. Inflating each dst by +1 unit makes
         *      neighbours overlap by 1 world pixel so there's no gap. */
        const float SRC_INSET = 0.5f;
        const float DST_BLEED = 1.0f;
        pthread_mutex_lock(&gl_tiles_lock);
        for (int tx = tx_start; tx < tx_end; tx++) {
            for (int ty = ty_start; ty < ty_end; ty++) {
                Tile *t = &gl_tile_matrix[tx][ty];
                TileType tt = (t->type < TILE_TYPE_COUNT) ? t->type : TILE_GRAVEL;
                Rectangle src = {
                    (float)(tile_col[tt] * TILES_CELL_PX) + SRC_INSET,
                    (float)(tile_row[tt] * TILES_CELL_PX) + SRC_INSET,
                    (float)TILES_CELL_PX - 2.0f * SRC_INSET,
                    (float)TILES_CELL_PX - 2.0f * SRC_INSET,
                };
                Rectangle dst = {
                    (float)(tx * TILE_SIZE),
                    (float)(ty * TILE_SIZE),
                    (float)TILE_SIZE + DST_BLEED,
                    (float)TILE_SIZE + DST_BLEED,
                };
                Color tint = t->fog ? (Color){85, 85, 85, 255} : WHITE;
                DrawTexturePro(tiles_atlas, src, dst, (Vector2){0, 0},
                               0.0f, tint);
            }
        }
        pthread_mutex_unlock(&gl_tiles_lock);
    }

    /* Bot — drawn in world space, size in world units so Camera2D scales. */
    if (bot_sprite_loaded) {
        float bot_draw_world = (float)BOT_RADIUS * 1.5f;
        int idx = ((int)snap_state >= 0 && (int)snap_state < BOT_STATE_SPRITES)
                      ? (int)snap_state : 0;
        int col = bot_state_col[idx];
        int row = bot_state_row[idx];
        /* 1-px inset avoids bilinear sampling from the neighbouring sprite
         * cell (was showing up as a faint halo around the bot). */
        const float BOT_SRC_INSET = 1.0f;
        Rectangle src = {
            (float)(col * BOT_SPRITE_SIZE) + BOT_SRC_INSET,
            (float)(row * BOT_SPRITE_SIZE) + BOT_SRC_INSET,
            (float)BOT_SPRITE_SIZE - 2.0f * BOT_SRC_INSET,
            (float)BOT_SPRITE_SIZE - 2.0f * BOT_SRC_INSET,
        };
        Rectangle dst = {
            bot_cx - bot_draw_world / 2.0f,
            bot_cy - bot_draw_world / 2.0f,
            bot_draw_world, bot_draw_world,
        };
        DrawTexturePro(bot_sprite, src, dst, (Vector2){0, 0}, 0.0f, WHITE);
    }

    /* Particle effects drawn on top of tiles & bot, still in world space. */
    particles_draw_world();

    EndMode2D();

    /* Solar flare flash — screen space, outside the camera transform. */
    if (snap_flare_active) {
        double elapsed = GetTime() - snap_flare_start;
        if (elapsed >= 2.0) {
            pthread_mutex_lock(&gl_tiles_lock);
            gl_solar_flare_animation_active = false;
            pthread_mutex_unlock(&gl_tiles_lock);
        } else {
            double cycle = fmod(elapsed, 0.2) / 0.2;
            if (cycle < 0.5) {
                DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                              (Color){255, 255, 0, 200});
            }
        }
    }
}
