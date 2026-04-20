#include "rendering.h"
#include "message_log.h"
#include <math.h>
#include <string.h>

/* ── Bot sprite sheet (bots.png) ─────────────────────────────────────────── */
static Texture2D bot_sprite = {0};
static bool      bot_sprite_loaded = false;
#define BOT_SPRITE_SIZE 150

static const int bot_state_col[] = {
    [BOT_WAITING]   = 0, [BOT_THINKING]  = 1, [BOT_MOVING]    = 2,
    [BOT_LOOKCLOSE] = 0, [BOT_LOOKFAR]   = 1, [BOT_CHARGING]  = 2,
};
static const int bot_state_row[] = {
    [BOT_WAITING]   = 0, [BOT_THINKING]  = 0, [BOT_MOVING]    = 0,
    [BOT_LOOKCLOSE] = 1, [BOT_LOOKFAR]   = 1, [BOT_CHARGING]  = 1,
};

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
static Texture2D try_load(const char *const *candidates, const char *label) {
    Texture2D tex = {0};
    for (const char *const *p = candidates; *p; p++) {
        if (FileExists(*p)) {
            tex = LoadTexture(*p);
            if (tex.id != 0) {
                msg_log("  [%s] Loaded spritesheet from %s", label, *p);
                return tex;
            }
        }
    }
    msg_log("  [%s] spritesheet not found; using fallback", label);
    return tex;
}

void rendering_init(void) {
    static const char *bot_paths[] = {
        "bots.png", "../resources/bots.png", "resources/bots.png",
        "../bots.png", "../../resources/bots.png", NULL,
    };
    bot_sprite = try_load(bot_paths, "Bots");
    bot_sprite_loaded = bot_sprite.id != 0;
    if (bot_sprite_loaded) SetTextureFilter(bot_sprite, TEXTURE_FILTER_BILINEAR);

    static const char *tile_paths[] = {
        "tiles.png", "../resources/tiles.png", "resources/tiles.png",
        "../tiles.png", "../../resources/tiles.png", NULL,
    };
    tiles_atlas = try_load(tile_paths, "Tiles");
    tiles_atlas_loaded = tiles_atlas.id != 0;
    if (tiles_atlas_loaded) SetTextureFilter(tiles_atlas, TEXTURE_FILTER_BILINEAR);
}

void rendering_unload(void) {
    if (bot_sprite_loaded)   UnloadTexture(bot_sprite);
    if (tiles_atlas_loaded)  UnloadTexture(tiles_atlas);
}

/* ── Camera input handling ───────────────────────────────────────────────── */
static void update_camera_input(Camera2D *camera, float base_scale,
                                float bot_cx, float bot_cy) {
    /* Reset. */
    if (IsKeyPressed(KEY_HOME) || IsKeyPressed(KEY_R)) {
        cam_pan_x = cam_pan_y = 0.0f;
        cam_zoom = 1.0f;
        cam_panning = false;
        camera->target = (Vector2){bot_cx, bot_cy};
        camera->zoom = base_scale;
        return;
    }

    /* Wheel zoom, anchored under the cursor. */
    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) {
        Vector2 m = GetMousePosition();
        Vector2 world_before = GetScreenToWorld2D(m, *camera);

        float new_mul = (wheel > 0) ? cam_zoom * CAM_ZOOM_STEP
                                    : cam_zoom / CAM_ZOOM_STEP;
        if (new_mul < CAM_ZOOM_MIN) new_mul = CAM_ZOOM_MIN;
        if (new_mul > CAM_ZOOM_MAX) new_mul = CAM_ZOOM_MAX;
        cam_zoom = new_mul;
        camera->zoom = base_scale * cam_zoom;

        /* Move target so the pre-zoom world point stays under the cursor. */
        Vector2 tgt;
        tgt.x = world_before.x - (m.x - camera->offset.x) / camera->zoom;
        tgt.y = world_before.y - (m.y - camera->offset.y) / camera->zoom;
        cam_pan_x = tgt.x - bot_cx;
        cam_pan_y = tgt.y - bot_cy;
        camera->target = tgt;
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
        float dx = (m.x - cam_pan_anchor.x) / camera->zoom;
        float dy = (m.y - cam_pan_anchor.y) / camera->zoom;
        cam_pan_x -= dx;
        cam_pan_y -= dy;
        cam_pan_anchor = m;
        camera->target = (Vector2){bot_cx + cam_pan_x, bot_cy + cam_pan_y};
    }
    if (!drag_held) cam_panning = false;
}

/* ── Main draw ───────────────────────────────────────────────────────────── */
void draw_game(void) {
    ClearBackground(BLACK);

    float bot_cx = gl_bot_x + TILE_SIZE / 2.0f;
    float bot_cy = gl_bot_y + TILE_SIZE / 2.0f;
    float base_scale = (float)DRAW_TILE_SIZE / TILE_SIZE;

    Camera2D camera = {
        .offset   = { WIN_WIDTH * 0.5f, WIN_HEIGHT * 0.5f },
        .target   = { bot_cx + cam_pan_x, bot_cy + cam_pan_y },
        .rotation = 0.0f,
        .zoom     = base_scale * cam_zoom,
    };
    update_camera_input(&camera, base_scale, bot_cx, bot_cy);

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
     * GPU draw call. If tiles.png failed to load, nothing is drawn here. */
    if (tiles_atlas_loaded) {
        pthread_mutex_lock(&gl_tiles_lock);
        for (int tx = tx_start; tx < tx_end; tx++) {
            for (int ty = ty_start; ty < ty_end; ty++) {
                Tile *t = &gl_tile_matrix[tx][ty];
                TileType tt = (t->type < TILE_TYPE_COUNT) ? t->type : TILE_GRAVEL;
                Rectangle src = {
                    (float)(tile_col[tt] * TILES_CELL_PX),
                    (float)(tile_row[tt] * TILES_CELL_PX),
                    (float)TILES_CELL_PX,
                    (float)TILES_CELL_PX,
                };
                Rectangle dst = {
                    (float)(tx * TILE_SIZE), (float)(ty * TILE_SIZE),
                    (float)TILE_SIZE, (float)TILE_SIZE,
                };
                Color tint = t->fog ? (Color){85, 85, 85, 255} : WHITE;
                DrawTexturePro(tiles_atlas, src, dst, (Vector2){0, 0},
                               0.0f, tint);
            }
        }
        pthread_mutex_unlock(&gl_tiles_lock);
    }

    /* Bot — drawn in world space, size in world units so Camera2D scales.
     * If bots.png failed to load, nothing is drawn here. */
    if (bot_sprite_loaded) {
        float bot_draw_world = (float)BOT_RADIUS * 1.5f;
        int col = bot_state_col[gl_bot_state < BOT_STATE_COUNT ? gl_bot_state : 0];
        int row = bot_state_row[gl_bot_state < BOT_STATE_COUNT ? gl_bot_state : 0];
        Rectangle src = {
            (float)(col * BOT_SPRITE_SIZE), (float)(row * BOT_SPRITE_SIZE),
            (float)BOT_SPRITE_SIZE, (float)BOT_SPRITE_SIZE,
        };
        Rectangle dst = {
            bot_cx - bot_draw_world / 2.0f,
            bot_cy - bot_draw_world / 2.0f,
            bot_draw_world, bot_draw_world,
        };
        DrawTexturePro(bot_sprite, src, dst, (Vector2){0, 0}, 0.0f, WHITE);
    }

    EndMode2D();

    /* Solar flare flash — screen space, outside the camera transform. */
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
