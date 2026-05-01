#include "game_logic.h"
#include "message_log.h"
#include "particles.h"
#include "raylib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ── Global state ────────────────────────────────────────────────────────── */
Tile            gl_tile_matrix[GRID_WIDTH][GRID_HEIGHT];
pthread_mutex_t gl_tiles_lock = PTHREAD_MUTEX_INITIALIZER;

/* Bot starts at the centre of the grid. World pixel coords = grid cell *
 * TILE_SIZE; concrete numbers (e.g. 700, 412) fall out of GRID_WIDTH/2 *
 * TILE_SIZE so the spawn auto-adjusts if the map dimensions change. */
float   gl_bot_x = (GRID_WIDTH  / 2) * (float)TILE_SIZE,
        gl_bot_y = (GRID_HEIGHT / 2) * (float)TILE_SIZE;
float   gl_bot_target_x = (GRID_WIDTH  / 2) * (float)TILE_SIZE,
        gl_bot_target_y = (GRID_HEIGHT / 2) * (float)TILE_SIZE;
int     gl_bot_energy = STARTING_BOT_ENERGY;
int     gl_bot_inventory_rocks = STARTING_INVENTORY_ROCKS;
BotState gl_bot_state = BOT_WAITING;
char    gl_bot_last_speech[4096] = "";
int     gl_bot_lookfar_distance = 40;
int     gl_bot_hour_count = 0;

int     gl_hours_solar_flare_every = STARTING_HOURS_SOLAR_FLARE_EVERY;
int     gl_hours_to_solar_flare    = STARTING_HOURS_SOLAR_FLARE_EVERY;

bool    gl_solar_flare_animation_active = false;
double  gl_solar_flare_animation_start_time = 0.0;
double  gl_charging_animation_until = 0.0;
static int solar_flare_last_hour = -1;

bool    gl_enable_fog_of_war = true;
int     gl_world_rocks_target = STARTING_WORLD_ROCKS_TARGET;
int     gl_initial_town_size  = STARTING_INITIAL_TOWN_SIZE;

BuiltRecord    gl_bot_built[MAX_BUILT_TILES];
int            gl_bot_built_count = 0;
pthread_mutex_t gl_built_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── Color / name tables ─────────────────────────────────────────────────── */
static const TileColor tile_colors[TILE_TYPE_COUNT] = {
    [TILE_GRAVEL]         = {140, 120, 100},
    [TILE_SAND]           = {220, 200, 120},
    [TILE_WATER]          = {120, 210, 255},
    [TILE_ROCKS]          = {130, 130, 130},
    [TILE_HABITAT]        = {180, 255, 100},
    [TILE_BATTERY]        = {245, 215,  90},
    [TILE_SOLAR_PANEL]    = { 90, 150, 215},
    [TILE_BROKEN_HABITAT] = { 40,  70,  40},
};

static const char *tile_names[TILE_TYPE_COUNT] = {
    "gravel", "sand", "water", "rocks",
    "habitat", "battery", "solar_panel", "broken_habitat",
};

static const char *tile_descs[TILE_TYPE_COUNT] = {
    "Loose red gravel and dust.",
    "Warm, loose sand.",
    "Clear, shimmering water.",
    "Jagged rocks and boulders.",
    "A sealed habitat module.",
    "Battery segment connecting habitats and solar panels.",
    "A solar panel array that powers the settlement.",
    "A damaged, breached habitat module.",
};

TileColor tile_color(TileType t) {
    if (t < 0 || t >= TILE_TYPE_COUNT) return tile_colors[TILE_GRAVEL];
    return tile_colors[t];
}

const char *tile_type_name(TileType t) {
    if (t < 0 || t >= TILE_TYPE_COUNT) return "gravel";
    return tile_names[t];
}

const char *tile_description(TileType t) {
    if (t < 0 || t >= TILE_TYPE_COUNT) return tile_descs[TILE_GRAVEL];
    return tile_descs[t];
}

TileType tile_type_from_name(const char *name) {
    if (!name) return TILE_GRAVEL;
    for (int i = 0; i < TILE_TYPE_COUNT; i++) {
        if (strcmp(name, tile_names[i]) == 0) return (TileType)i;
    }
    return TILE_GRAVEL;
}

bool tile_is_buildable(TileType t) {
    return t == TILE_HABITAT || t == TILE_BATTERY || t == TILE_SOLAR_PANEL;
}

int tile_build_cost(TileType t) {
    switch (t) {
        case TILE_HABITAT:     return ROCKS_REQUIRED_FOR_HABITAT;
        case TILE_BATTERY:     return ROCKS_REQUIRED_FOR_BATTERY;
        case TILE_SOLAR_PANEL: return ROCKS_REQUIRED_FOR_SOLAR_PANEL;
        default:               return 999;
    }
}

/* ── Direction / distance helpers ────────────────────────────────────────── */
static const char *dir_long_names[DIR_COUNT] = {
    [DIR_NONE] = "none",
    [DIR_N]    = "north",
    [DIR_NE]   = "north-east",
    [DIR_E]    = "east",
    [DIR_SE]   = "south-east",
    [DIR_S]    = "south",
    [DIR_SW]   = "south-west",
    [DIR_W]    = "west",
    [DIR_NW]   = "north-west",
};

static const char *dir_short_names[DIR_COUNT] = {
    [DIR_NONE] = "",
    [DIR_N]    = "N",
    [DIR_NE]   = "NE",
    [DIR_E]    = "E",
    [DIR_SE]   = "SE",
    [DIR_S]    = "S",
    [DIR_SW]   = "SW",
    [DIR_W]    = "W",
    [DIR_NW]   = "NW",
};

const char *direction_name(Direction d) {
    if (d < 0 || d >= DIR_COUNT) return "none";
    return dir_long_names[d];
}

/* y grows downward in the grid, so "north" corresponds to dy<0. The
 * classifier uses 8 bins of 45° each centred on the cardinal directions. */
Direction direction_from_delta(int dx, int dy) {
    if (dx == 0 && dy == 0) return DIR_NONE;
    double angle = atan2((double)-dy, (double)dx); /* flip dy for y-down */
    /* bins (in radians):
     *   E  = [-pi/8, pi/8)
     *   NE = [pi/8, 3pi/8)
     *   N  = [3pi/8, 5pi/8)
     *   NW = [5pi/8, 7pi/8)
     *   W  = [7pi/8, pi] ∪ [-pi, -7pi/8)
     *   SW = [-7pi/8, -5pi/8)
     *   S  = [-5pi/8, -3pi/8)
     *   SE = [-3pi/8, -pi/8) */
    const double P = 3.14159265358979323846;
    if (angle >= -P/8 && angle <  P/8) return DIR_E;
    if (angle >=  P/8 && angle < 3*P/8) return DIR_NE;
    if (angle >= 3*P/8 && angle < 5*P/8) return DIR_N;
    if (angle >= 5*P/8 && angle < 7*P/8) return DIR_NW;
    if (angle >= 7*P/8 || angle < -7*P/8) return DIR_W;
    if (angle >= -7*P/8 && angle < -5*P/8) return DIR_SW;
    if (angle >= -5*P/8 && angle < -3*P/8) return DIR_S;
    return DIR_SE;
}

void direction_unit(Direction d, int *dx, int *dy) {
    int ux = 0, uy = 0;
    switch (d) {
        case DIR_N:  uy = -1; break;
        case DIR_NE: ux =  1; uy = -1; break;
        case DIR_E:  ux =  1; break;
        case DIR_SE: ux =  1; uy =  1; break;
        case DIR_S:  uy =  1; break;
        case DIR_SW: ux = -1; uy =  1; break;
        case DIR_W:  ux = -1; break;
        case DIR_NW: ux = -1; uy = -1; break;
        default: break;
    }
    if (dx) *dx = ux;
    if (dy) *dy = uy;
}

/* Accepts "n"/"N"/"north", "ne"/"north-east"/"northeast"/"north east", etc.
 * Case-insensitive; '-' and ' ' are treated the same. */
Direction direction_from_name(const char *s) {
    if (!s || !s[0]) return DIR_NONE;
    char norm[32] = {0};
    size_t w = 0;
    for (size_t i = 0; s[i] && w < sizeof(norm) - 1; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == '-' || c == '_' || c == ' ') continue;
        norm[w++] = c;
    }
    norm[w] = '\0';
    if (!strcmp(norm, "n") || !strcmp(norm, "north")) return DIR_N;
    if (!strcmp(norm, "ne") || !strcmp(norm, "northeast")) return DIR_NE;
    if (!strcmp(norm, "e") || !strcmp(norm, "east")) return DIR_E;
    if (!strcmp(norm, "se") || !strcmp(norm, "southeast")) return DIR_SE;
    if (!strcmp(norm, "s") || !strcmp(norm, "south")) return DIR_S;
    if (!strcmp(norm, "sw") || !strcmp(norm, "southwest")) return DIR_SW;
    if (!strcmp(norm, "w") || !strcmp(norm, "west")) return DIR_W;
    if (!strcmp(norm, "nw") || !strcmp(norm, "northwest")) return DIR_NW;
    return DIR_NONE;
}

/* Centralised bucket thresholds. Tuning here updates both LookFar labels
 * and MoveTo step sizes. */
const char *dist_bucket_name(DistBucket b) {
    switch (b) {
        case DIST_ADJACENT: return "adjacent";
        case DIST_CLOSE:    return "close";
        case DIST_MEDIUM:   return "medium";
        case DIST_FAR:      return "far";
        default:            return "none";
    }
}

DistBucket dist_bucket_from_tiles(int tiles) {
    if (tiles <= 0) return DIST_NONE;
    if (tiles <= 1) return DIST_ADJACENT;
    if (tiles <= 5) return DIST_CLOSE;
    if (tiles <= 15) return DIST_MEDIUM;
    return DIST_FAR;
}

int dist_bucket_walk_tiles(DistBucket b) {
    switch (b) {
        case DIST_ADJACENT: return 1;
        case DIST_CLOSE:    return 3;
        case DIST_MEDIUM:   return 8;
        case DIST_FAR:      return MOVE_MAX_TILES;
        default:            return 0;
    }
}

DistBucket dist_bucket_from_name(const char *s) {
    if (!s || !s[0]) return DIST_NONE;
    char norm[16] = {0};
    size_t w = 0;
    for (size_t i = 0; s[i] && w < sizeof(norm) - 1; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        norm[w++] = c;
    }
    norm[w] = '\0';
    if (!strcmp(norm, "adjacent") || !strcmp(norm, "adj") ||
        !strcmp(norm, "next")) return DIST_ADJACENT;
    if (!strcmp(norm, "close") || !strcmp(norm, "near")) return DIST_CLOSE;
    if (!strcmp(norm, "medium") || !strcmp(norm, "mid")) return DIST_MEDIUM;
    if (!strcmp(norm, "far")) return DIST_FAR;
    return DIST_NONE;
}

/* ── Feature memory (last LookFar cache) ─────────────────────────────────── */
typedef struct {
    bool       valid;
    int        x, y;
    Direction  dir;
    DistBucket dist;
    int        game_hour;
} KnownFeature;

static KnownFeature gl_last_lookfar[TILE_TYPE_COUNT];

static void known_features_reset(void) {
    for (int i = 0; i < TILE_TYPE_COUNT; i++) gl_last_lookfar[i].valid = false;
}

/* Invalidate any cache entry whose recorded (x,y) no longer matches that
 * tile's actual type. Called opportunistically from read paths. */
static void known_features_revalidate(void) {
    pthread_mutex_lock(&gl_tiles_lock);
    for (int i = 0; i < TILE_TYPE_COUNT; i++) {
        if (!gl_last_lookfar[i].valid) continue;
        int x = gl_last_lookfar[i].x, y = gl_last_lookfar[i].y;
        if (x < 0 || x >= GRID_WIDTH || y < 0 || y >= GRID_HEIGHT) {
            gl_last_lookfar[i].valid = false;
            continue;
        }
        if (gl_tile_matrix[x][y].type != (TileType)i) {
            gl_last_lookfar[i].valid = false;
        }
    }
    pthread_mutex_unlock(&gl_tiles_lock);
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */
void gl_bot_grid_pos(int *gx, int *gy) {
    int x = (int)gl_bot_target_x / TILE_SIZE;
    int y = (int)gl_bot_target_y / TILE_SIZE;
    if (x < 0) x = 0; if (x >= GRID_WIDTH)  x = GRID_WIDTH - 1;
    if (y < 0) y = 0; if (y >= GRID_HEIGHT) y = GRID_HEIGHT - 1;
    *gx = x; *gy = y;
}

static void consume_energy(int amount) {
    gl_bot_energy -= amount;
    if (gl_bot_energy < 0) gl_bot_energy = 0;
}

/* ── CreateTile (internal) ───────────────────────────────────────────────── */
static bool create_tile(int x, int y, TileType type) {
    if (type < 0 || type >= TILE_TYPE_COUNT) return false;
    if (x < 0 || x >= GRID_WIDTH || y < 0 || y >= GRID_HEIGHT) return false;
    TileColor c = tile_colors[type];
    pthread_mutex_lock(&gl_tiles_lock);
    bool old_fog = gl_tile_matrix[x][y].fog;
    gl_tile_matrix[x][y].type = type;
    gl_tile_matrix[x][y].r = c.r;
    gl_tile_matrix[x][y].g = c.g;
    gl_tile_matrix[x][y].b = c.b;
    gl_tile_matrix[x][y].fog = old_fog;
    if (type == TILE_HABITAT) gl_tile_matrix[x][y].habitat_damage = 0.0f;
    pthread_mutex_unlock(&gl_tiles_lock);
    return true;
}

/* ── Procedural map generation ───────────────────────────────────────────── */
static int place_rock_field(int cx, int cy, int radius, float density) {
    int target = (int)(radius * radius * 2.6f * density);
    if (target < 24) target = 24;
    int max_attempts = target * 40;
    int nwalkers = 3 + rand() % 4;
    int wx[8], wy[8];
    for (int i = 0; i < nwalkers; i++) { wx[i] = cx; wy[i] = cy; }
    int count = 0, attempts = 0;
    while (count < target && attempts < max_attempts) {
        attempts++;
        int i = rand() % nwalkers;
        if ((rand() % 100) < 18) {
            if (wx[i] < cx) wx[i]++;
            else if (wx[i] > cx) wx[i]--;
            if (wy[i] < cy) wy[i]++;
            else if (wy[i] > cy) wy[i]--;
        }
        wx[i] += (rand() % 3) - 1;
        wy[i] += (rand() % 3) - 1;
        if (wx[i] < 1 || wx[i] >= GRID_WIDTH - 1) wx[i] = cx;
        if (wy[i] < 1 || wy[i] >= GRID_HEIGHT - 1) wy[i] = cy;
        int dx = wx[i] - cx, dy = wy[i] - cy;
        if (dx*dx + dy*dy > (int)((radius * 1.45f) * (radius * 1.45f))) continue;
        if (create_tile(wx[i], wy[i], TILE_ROCKS)) count++;
    }
    return count;
}

static int carve_stream(int sx, int sy, int length, int width) {
    int count = 0, x = sx, y = sy;
    int dx = (rand() % 2) ? 1 : -1;
    int dy = (rand() % 3) - 1;
    for (int step = 0; step < length; step++) {
        for (int wxi = -width; wxi <= width; wxi++) {
            for (int wyi = -width; wyi <= width; wyi++) {
                int nx = x + wxi, ny = y + wyi;
                if (nx < 0 || nx >= GRID_WIDTH || ny < 0 || ny >= GRID_HEIGHT) continue;
                if (create_tile(nx, ny, TILE_WATER)) count++;
                for (int sxi = -2; sxi <= 2; sxi++) {
                    for (int syi = -2; syi <= 2; syi++) {
                        if (abs(sxi) < 2 && abs(syi) < 2) continue;
                        int bx = nx + sxi, by = ny + syi;
                        if (bx < 0 || bx >= GRID_WIDTH || by < 0 || by >= GRID_HEIGHT) continue;
                        pthread_mutex_lock(&gl_tiles_lock);
                        bool ok = gl_tile_matrix[bx][by].type == TILE_GRAVEL;
                        pthread_mutex_unlock(&gl_tiles_lock);
                        if (ok) create_tile(bx, by, TILE_SAND);
                    }
                }
            }
        }
        if ((rand() % 100) < 25) {
            dx += (rand() % 3) - 1;
            dy += (rand() % 3) - 1;
            if (dx < -1) dx = -1; if (dx > 1) dx = 1;
            if (dy < -1) dy = -1; if (dy > 1) dy = 1;
            if (dx == 0 && dy == 0) dx = (rand() % 2) ? 1 : -1;
        }
        x += dx; y += dy;
        if (x < 2 || x >= GRID_WIDTH - 2) { dx *= -1; x += dx; }
        if (y < 2 || y >= GRID_HEIGHT - 2) { dy *= -1; y += dy; }
    }
    return count;
}

static int place_sand_patch(int cx, int cy, int rx, int ry) {
    int count = 0;
    for (int x = cx - rx - 2; x <= cx + rx + 2; x++) {
        for (int y = cy - ry - 2; y <= cy + ry + 2; y++) {
            if (x < 0 || x >= GRID_WIDTH || y < 0 || y >= GRID_HEIGHT) continue;
            float dxf = (float)(x - cx) / (rx > 0 ? rx : 1);
            float dyf = (float)(y - cy) / (ry > 0 ? ry : 1);
            float dist = dxf*dxf + dyf*dyf;
            float noise = ((rand() % 50) - 25) / 100.0f;
            if (dist + noise < 1.0f) {
                pthread_mutex_lock(&gl_tiles_lock);
                bool ok = gl_tile_matrix[x][y].type == TILE_GRAVEL;
                pthread_mutex_unlock(&gl_tiles_lock);
                if (ok && create_tile(x, y, TILE_SAND)) count++;
            }
        }
    }
    return count;
}

static void build_scenery_procedural(int rocks_target) {
    msg_log("Generating map procedurally...");
    float terrain_scale = 8.0f / TILE_SIZE;
    float area_scale = terrain_scale * terrain_scale;
    int total = 0;

    for (int i = 0; i < rocks_target; i++) {
        int cx = 6 + rand() % (GRID_WIDTH - 13);
        int cy = 6 + rand() % (GRID_HEIGHT - 13);
        int r = 1 + (int)(rand() % (int)(2 * terrain_scale));
        if (r < 1) r = 1;
        float d = 0.85f + (rand() % 26) / 100.0f;
        total += place_rock_field(cx, cy, r, d);
    }
    msg_log("  Rocks: target=%d, placed=%d", rocks_target, total);

    int stream_count = (int)(6 * area_scale);
    if (stream_count < 6) stream_count = 6;
    for (int i = 0; i < stream_count; i++) {
        int sx = 4 + rand() % (GRID_WIDTH - 9);
        int sy = 4 + rand() % (GRID_HEIGHT - 9);
        int len = (int)(26 * terrain_scale) + rand() % (int)(26 * terrain_scale + 1);
        carve_stream(sx, sy, len, 1);
    }
    msg_log("  Streams: %d carved", stream_count);

    int sand_patches = (int)(22 * area_scale);
    if (sand_patches < 18) sand_patches = 18;
    for (int i = 0; i < sand_patches; i++) {
        int cx = 3 + rand() % (GRID_WIDTH - 7);
        int cy = 3 + rand() % (GRID_HEIGHT - 7);
        int rx = (int)(3 * terrain_scale) + rand() % (int)(4 * terrain_scale + 1);
        int ry = (int)(3 * terrain_scale) + rand() % (int)(4 * terrain_scale + 1);
        place_sand_patch(cx, cy, rx, ry);
    }
    msg_log("  Sand patches: %d clusters", sand_patches);
    msg_log("Procedural map complete.");
}

/* ── Starter town ────────────────────────────────────────────────────────── */
static bool terrain_ok_for_town(int x, int y) {
    if (x < 0 || x >= GRID_WIDTH || y < 0 || y >= GRID_HEIGHT) return false;
    pthread_mutex_lock(&gl_tiles_lock);
    TileType t = gl_tile_matrix[x][y].type;
    pthread_mutex_unlock(&gl_tiles_lock);
    return t == TILE_GRAVEL || t == TILE_SAND || t == TILE_ROCKS;
}

static void generate_initial_town(int num_groups) {
    if (num_groups <= 0) return;
    int groups = num_groups > 12 ? 12 : num_groups;
    int bx, by;
    gl_bot_grid_pos(&bx, &by);

    int sx = -1, sy = -1;
    for (int attempt = 0; attempt < 250; attempt++) {
        sx = 15 + rand() % (GRID_WIDTH - 35);
        sy = 10 + rand() % (GRID_HEIGHT - 22);
        bool ok = true;
        for (int dx = 0; dx < 4 && ok; dx++)
            for (int dy = 0; dy < 4 && ok; dy++)
                if (!terrain_ok_for_town(sx + dx, sy + dy)) ok = false;
        if (ok) break;
        sx = -1;
    }
    if (sx < 0) { msg_log("  [InitialTown] No valid anchor; skipping."); return; }

    for (int dx = 0; dx < 4; dx++)
        for (int dy = 0; dy < 4; dy++)
            create_tile(sx + dx, sy + dy, TILE_SOLAR_PANEL);

    int placed_h = 0, placed_b = 0;
    int col = 0, row = 0;
    for (int g = 0; g < groups; g++) {
        int px = sx - 2 - col;
        int py = sy + row;
        if (px >= 0 && py >= 0 && px < GRID_WIDTH && py < GRID_HEIGHT) {
            create_tile(px, py, TILE_BATTERY);
            placed_b++;
        }
        row++;
        if (row >= 4) { row = 0; col++; }

        int nh = 2 + rand() % 2;
        for (int h = 0; h < nh; h++) {
            px = sx - 2 - col;
            py = sy + row;
            if (px >= 0 && py >= 0 && px < GRID_WIDTH && py < GRID_HEIGHT) {
                create_tile(px, py, TILE_HABITAT);
                pthread_mutex_lock(&gl_tiles_lock);
                gl_tile_matrix[px][py].habitat_damage = 0.0f;
                pthread_mutex_unlock(&gl_tiles_lock);
                placed_h++;
            }
            row++;
            if (row >= 4) { row = 0; col++; }
        }
    }
    msg_log("  [InitialTown] Placed %d habitat(s), %d battery(ies), 16 solar panels", placed_h, placed_b);
}

/* ── Initialize default tiles ────────────────────────────────────────────── */
static void initialize_default_tiles(void) {
    pthread_mutex_lock(&gl_tiles_lock);
    for (int x = 0; x < GRID_WIDTH; x++) {
        for (int y = 0; y < GRID_HEIGHT; y++) {
            TileColor c = tile_colors[TILE_GRAVEL];
            gl_tile_matrix[x][y] = (Tile){
                .x = x, .y = y, .type = TILE_GRAVEL,
                .r = c.r, .g = c.g, .b = c.b,
                .fog = gl_enable_fog_of_war,
                .habitat_damage = 0.0f,
            };
        }
    }
    pthread_mutex_unlock(&gl_tiles_lock);
}

static int gl_spawn_gx_cache = GRID_WIDTH  / 2,
           gl_spawn_gy_cache = GRID_HEIGHT / 2;

void gl_bot_spawn_grid_pos(int *gx, int *gy) {
    if (gx) *gx = gl_spawn_gx_cache;
    if (gy) *gy = gl_spawn_gy_cache;
}

void gl_initialize_world(bool use_fog, int rocks_target) {
    gl_enable_fog_of_war = use_fog;
    gl_world_rocks_target = rocks_target > 0 ? rocks_target : 1;
    gl_bot_hour_count = 0;
    gl_hours_to_solar_flare = gl_hours_solar_flare_every;
    solar_flare_last_hour = -1;
    gl_solar_flare_animation_active = false;
    gl_bot_state = BOT_WAITING;
    gl_bot_last_speech[0] = '\0';
    gl_bot_x = (GRID_WIDTH  / 2) * (float)TILE_SIZE;
    gl_bot_y = (GRID_HEIGHT / 2) * (float)TILE_SIZE;
    gl_bot_target_x = gl_bot_x;
    gl_bot_target_y = gl_bot_y;
    gl_spawn_gx_cache = (int)gl_bot_x / TILE_SIZE;
    gl_spawn_gy_cache = (int)gl_bot_y / TILE_SIZE;

    pthread_mutex_lock(&gl_built_lock);
    gl_bot_built_count = 0;
    pthread_mutex_unlock(&gl_built_lock);

    initialize_default_tiles();
    build_scenery_procedural(gl_world_rocks_target);
    if (gl_initial_town_size > 0)
        generate_initial_town(gl_initial_town_size);

    known_features_reset();
}

/* ── Neighbours line + known-features summary ───────────────────────────── */
/* Order matches the compass order N, NE, E, SE, S, SW, W, NW — same as the
 * Direction enum but without DIR_NONE. */
void gl_build_neighbours_line(char *out, size_t cap) {
    if (cap == 0) return;
    int gx, gy;
    gl_bot_grid_pos(&gx, &gy);
    const Direction order[8] = {
        DIR_N, DIR_NE, DIR_E, DIR_SE, DIR_S, DIR_SW, DIR_W, DIR_NW
    };
    const char *names[8] = {0};
    TileType    types[8] = {0};
    pthread_mutex_lock(&gl_tiles_lock);
    /* The robot is physically present on the centre tile and can see its
     * eight touching neighbours directly, so reveal their fog as we read
     * them. LookFar stays the only tool that reveals beyond this ring. */
    gl_tile_matrix[gx][gy].fog = false;
    for (int i = 0; i < 8; i++) {
        int dx = 0, dy = 0;
        direction_unit(order[i], &dx, &dy);
        int x = gx + dx, y = gy + dy;
        if (x < 0 || x >= GRID_WIDTH || y < 0 || y >= GRID_HEIGHT) {
            names[i] = "off_map";
            types[i] = TILE_TYPE_COUNT; /* sentinel: not a real type */
        } else {
            gl_tile_matrix[x][y].fog = false;
            types[i] = gl_tile_matrix[x][y].type;
            names[i] = tile_type_name(types[i]);
        }
    }
    const char *cur = tile_type_name(gl_tile_matrix[gx][gy].type);

    /* Feed the 8-neighbour ring into gl_last_lookfar so MoveTo(target=...)
     * works without a stale-cache miss. Two passes: orthogonal first, then
     * diagonal. Orth wins ties so the resulting MoveTo lands on an
     * |dx|+|dy|=1 cell (i.e. a valid Create-neighbour). Stale entries for
     * tile types NOT seen in the ring are left untouched — the old LookFar
     * memory for distant features still stands. */
    const int orth_idx[4] = { 0, 2, 4, 6 };   /* N, E, S, W   */
    const int diag_idx[4] = { 1, 3, 5, 7 };   /* NE, SE, SW, NW */
    for (int pass = 0; pass < 2; pass++) {
        const int *idxs = pass == 0 ? orth_idx : diag_idx;
        for (int k = 0; k < 4; k++) {
            int i = idxs[k];
            if (types[i] >= TILE_TYPE_COUNT) continue;
            int dx = 0, dy = 0;
            direction_unit(order[i], &dx, &dy);
            int x = gx + dx, y = gy + dy;
            TileType t = types[i];
            /* Pass 1 (diagonals): do not clobber an orthogonal adjacent
             * entry just written by pass 0. */
            if (pass == 1 && gl_last_lookfar[t].valid &&
                gl_last_lookfar[t].dist == DIST_ADJACENT &&
                gl_last_lookfar[t].game_hour == gl_bot_hour_count) {
                continue;
            }
            gl_last_lookfar[t] = (KnownFeature){
                .valid = true, .x = x, .y = y,
                .dir = order[i], .dist = DIST_ADJACENT,
                .game_hour = gl_bot_hour_count,
            };
        }
    }
    pthread_mutex_unlock(&gl_tiles_lock);

    snprintf(out, cap,
        "Neighbours: N=%s, NE=%s, E=%s, SE=%s, S=%s, SW=%s, W=%s, NW=%s. "
        "Current=%s.",
        names[0], names[1], names[2], names[3],
        names[4], names[5], names[6], names[7], cur);
}

void gl_build_known_features_summary(char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    known_features_revalidate();

    /* Direction/distance of every cached feature is only meaningful
     * relative to where the bot *is now*, not where it was when the
     * feature was seen. Recompute both from the stored absolute (x,y)
     * against the current bot position. Drop entries beyond the LookFar
     * horizon so stale long-range memory stops steering MoveTo. */
    int bgx, bgy;
    gl_bot_grid_pos(&bgx, &bgy);
    int horizon = gl_bot_lookfar_distance;
    if (horizon < 1) horizon = 20;

    size_t w = 0;
    bool any = false;
    for (int i = 0; i < TILE_TYPE_COUNT; i++) {
        if (!gl_last_lookfar[i].valid) continue;
        int fx = gl_last_lookfar[i].x;
        int fy = gl_last_lookfar[i].y;
        int ddx = fx - bgx, ddy = fy - bgy;
        int adx = ddx < 0 ? -ddx : ddx;
        int ady = ddy < 0 ? -ddy : ddy;
        int cheb = adx > ady ? adx : ady;
        if (cheb == 0) {
            /* Feature is under the bot; Neighbours + Current already cover it. */
            continue;
        }
        if (cheb > horizon) {
            /* Out-of-range: drop it so it stops showing up as a mirage. */
            gl_last_lookfar[i].valid = false;
            continue;
        }
        Direction  dir  = direction_from_delta(ddx, ddy);
        DistBucket dist = dist_bucket_from_tiles(cheb);
        /* Refresh the cache so MoveTo(target=...) sees the same direction
         * the LLM just read in the summary. */
        gl_last_lookfar[i].dir  = dir;
        gl_last_lookfar[i].dist = dist;

        /* Capitalise the first letter of the tile name for prose. */
        const char *name = tile_type_name((TileType)i);
        char Name[32];
        snprintf(Name, sizeof(Name), "%s", name);
        if (Name[0] >= 'a' && Name[0] <= 'z') Name[0] = (char)(Name[0] - 'a' + 'A');
        int n = snprintf(out + w, cap - w,
            "%s%s %s (%s).",
            any ? " " : "",
            Name,
            direction_name(dir),
            dist_bucket_name(dist));
        if (n < 0 || (size_t)n >= cap - w) { out[cap-1] = '\0'; return; }
        w += (size_t)n;
        any = true;
    }
}

/* ── Power network check (BFS) ───────────────────────────────────────────── */
/* BFS the connected habitat+battery+solar_panel cluster containing (gx,gy)
 * and report which of those three roles are present. Caller must ensure the
 * out-pointers are non-NULL. */
static void habitat_network_flags(int gx, int gy,
                                  bool *out_on_habitat,
                                  bool *out_has_battery,
                                  bool *out_has_solar) {
    *out_on_habitat = false;
    *out_has_battery = false;
    *out_has_solar = false;

    pthread_mutex_lock(&gl_tiles_lock);
    if (gx < 0 || gx >= GRID_WIDTH || gy < 0 || gy >= GRID_HEIGHT ||
        gl_tile_matrix[gx][gy].type != TILE_HABITAT) {
        pthread_mutex_unlock(&gl_tiles_lock);
        return;
    }
    *out_on_habitat = true;
    static int qx[GRID_WIDTH * GRID_HEIGHT], qy[GRID_WIDTH * GRID_HEIGHT];
    static bool visited[GRID_WIDTH][GRID_HEIGHT];
    memset(visited, 0, sizeof(visited));
    int head = 0, tail = 0;
    qx[tail] = gx; qy[tail] = gy; tail++;
    visited[gx][gy] = true;
    static const int dx4[] = {1, -1, 0, 0};
    static const int dy4[] = {0, 0, 1, -1};
    while (head < tail) {
        int cx = qx[head], cy = qy[head]; head++;
        TileType tt = gl_tile_matrix[cx][cy].type;
        if (tt == TILE_BATTERY)     *out_has_battery = true;
        if (tt == TILE_SOLAR_PANEL) *out_has_solar   = true;
        for (int d = 0; d < 4; d++) {
            int nx = cx + dx4[d], ny = cy + dy4[d];
            if (nx < 0 || nx >= GRID_WIDTH || ny < 0 || ny >= GRID_HEIGHT) continue;
            if (visited[nx][ny]) continue;
            TileType nt = gl_tile_matrix[nx][ny].type;
            if (nt == TILE_HABITAT || nt == TILE_BATTERY || nt == TILE_SOLAR_PANEL) {
                visited[nx][ny] = true;
                qx[tail] = nx; qy[tail] = ny; tail++;
            }
        }
    }
    pthread_mutex_unlock(&gl_tiles_lock);
}

static bool habitat_on_solar_network(int gx, int gy) {
    bool on_habitat, has_battery, has_solar;
    habitat_network_flags(gx, gy, &on_habitat, &has_battery, &has_solar);
    return on_habitat && has_battery && has_solar;
}

bool gl_bot_habitat_network_charge_active(void) {
    int gx, gy;
    gl_bot_grid_pos(&gx, &gy);
    return habitat_on_solar_network(gx, gy);
}

void gl_bot_charge_network_status(bool *on_habitat,
                                  bool *has_battery,
                                  bool *has_solar) {
    bool oh, hb, hs;
    int gx, gy;
    gl_bot_grid_pos(&gx, &gy);
    habitat_network_flags(gx, gy, &oh, &hb, &hs);
    if (on_habitat)  *on_habitat  = oh;
    if (has_battery) *has_battery = hb;
    if (has_solar)   *has_solar   = hs;
}

static void apply_habitat_network_charge(void) {
    int gx, gy;
    gl_bot_grid_pos(&gx, &gy);
    pthread_mutex_lock(&gl_tiles_lock);
    TileType t = gl_tile_matrix[gx][gy].type;
    pthread_mutex_unlock(&gl_tiles_lock);
    if (t != TILE_HABITAT || !habitat_on_solar_network(gx, gy)) return;
    int before = gl_bot_energy;
    gl_bot_energy += HABITAT_SOLAR_CHARGE;
    gl_bot_state = BOT_CHARGING;
    gl_charging_animation_until = (double)clock() / CLOCKS_PER_SEC + 0.8;
    msg_log("  [Power] Habitat at (%d,%d): +%d energy (%d -> %d)",
            gx, gy, HABITAT_SOLAR_CHARGE, before, gl_bot_energy);
}

/* ── Solar flare ─────────────────────────────────────────────────────────── */
void gl_apply_solar_flare_interval(int hours) {
    int h = hours > 1 ? hours : 1;
    gl_hours_solar_flare_every = h;
    gl_hours_to_solar_flare = h;
    solar_flare_last_hour = -1;
}

void gl_apply_initial_town_size(int n) {
    gl_initial_town_size = n < 0 ? 0 : (n > 12 ? 12 : n);
}

bool gl_advance_solar_flare_hour(int current_hour) {
    int hour = current_hour >= 0 ? current_hour : gl_bot_hour_count;
    if (hour > 0 && hour == solar_flare_last_hour) return true;
    if (hour > 0) solar_flare_last_hour = hour;

    apply_habitat_network_charge();

    gl_hours_to_solar_flare--;
    if (gl_hours_to_solar_flare > 0) return true;

    msg_log("  [SolarFlare] === SOLAR FLARE EVENT ===");
    int gx, gy;
    gl_bot_grid_pos(&gx, &gy);
    pthread_mutex_lock(&gl_tiles_lock);
    TileType bt = gl_tile_matrix[gx][gy].type;
    pthread_mutex_unlock(&gl_tiles_lock);
    if (bt == TILE_HABITAT) {
        msg_log("  [SolarFlare] Bot is inside habitat - protected.");
    } else {
        int before = gl_bot_energy;
        gl_bot_energy -= 100;
        if (gl_bot_energy < 0) gl_bot_energy = 0;
        msg_log("  [SolarFlare] Drained 100 energy (%d -> %d)", before, gl_bot_energy);
    }

    gl_solar_flare_animation_active = true;
    gl_solar_flare_animation_start_time = GetTime();
    gl_hours_to_solar_flare = gl_hours_solar_flare_every;

    /* Crackle around the bot for the flash duration. */
    float bx = gl_bot_x + TILE_SIZE / 2.0f;
    float by = gl_bot_y + TILE_SIZE / 2.0f;
    particles_spawn_flare_zap(bx, by);

    if (gl_bot_energy <= 0) {
        gl_bot_state = BOT_DESTROYED;
        msg_log("  [SolarFlare] Bot destroyed - out of energy!");
        particles_spawn_destruction(bx, by);
        return false;
    }
    return true;
}

/* ── Tools ───────────────────────────────────────────────────────────────── */
static void tool_result_ok(ToolResult *r) { r->ok = true; }
static void tool_result_err(ToolResult *r, const char *err) {
    r->ok = false;
    snprintf(r->json, TOOL_RESULT_MAX_LEN, "{\"ok\":false,\"error\":\"%s\"}", err);
}

ToolResult gl_move_to(int target_x, int target_y) {
    ToolResult res = {0};
    if (!gl_advance_solar_flare_hour(-1)) {
        tool_result_err(&res, "Destroyed by solar flare.");
        return res;
    }
    if (target_x < 0) target_x = 0;
    if (target_x >= GRID_WIDTH) target_x = GRID_WIDTH - 1;
    if (target_y < 0) target_y = 0;
    if (target_y >= GRID_HEIGHT) target_y = GRID_HEIGHT - 1;

    int start_gx, start_gy;
    gl_bot_grid_pos(&start_gx, &start_gy);

    /* Reject no-op moves loudly. Small local models otherwise latch onto
     * MoveTo(current_pos) -> ok:true as a "successful" action and loop on
     * it indefinitely (observed: 17 consecutive self-moves on the habitat).
     * Returning ok:false here forces the LLM to pick a different tool. */
    if (start_gx == target_x && start_gy == target_y) {
        tool_result_err(&res,
            "MoveTo with zero displacement. Pick a different direction or "
            "a different tool.");
        msg_log("  [MoveTo] rejected no-op: already at (%d,%d)",
                start_gx, start_gy);
        return res;
    }

    int dx = 0, dy = 0;
    if (start_gx != target_x) dx = (target_x > start_gx) ? 1 : -1;
    if (start_gy != target_y) dy = (target_y > start_gy) ? 1 : -1;

    float new_x = gl_bot_target_x, new_y = gl_bot_target_y;
    int hours_taken = 0;
    for (int step = 0; step < MOVE_MAX_TILES; step++) {
        consume_energy(1);
        float nx = new_x + dx * TILE_SIZE;
        float ny = new_y + dy * TILE_SIZE;
        if (nx < BOT_RADIUS) nx = BOT_RADIUS;
        if (nx > MAP_WIDTH - BOT_RADIUS) nx = MAP_WIDTH - BOT_RADIUS;
        if (ny < BOT_RADIUS) ny = BOT_RADIUS;
        if (ny > MAP_HEIGHT - BOT_RADIUS) ny = MAP_HEIGHT - BOT_RADIUS;
        int ngx = (int)nx / TILE_SIZE;
        int ngy = (int)ny / TILE_SIZE;
        if (ngx < 0) ngx = 0; if (ngx >= GRID_WIDTH) ngx = GRID_WIDTH - 1;
        if (ngy < 0) ngy = 0; if (ngy >= GRID_HEIGHT) ngy = GRID_HEIGHT - 1;
        pthread_mutex_lock(&gl_tiles_lock);
        gl_tile_matrix[ngx][ngy].fog = false;
        pthread_mutex_unlock(&gl_tiles_lock);
        new_x = nx; new_y = ny;
        hours_taken++;
        if (ngx == target_x && ngy == target_y) break;
        if (ngx == target_x) dx = 0;
        if (ngy == target_y) dy = 0;
    }
    gl_bot_target_x = new_x;
    gl_bot_target_y = new_y;

    int gx, gy;
    gl_bot_grid_pos(&gx, &gy);
    pthread_mutex_lock(&gl_tiles_lock);
    const char *landed = tile_type_name(gl_tile_matrix[gx][gy].type);
    pthread_mutex_unlock(&gl_tiles_lock);

    msg_log("  [MoveTo] (%d,%d)->(%d,%d) %d hrs -> (%d,%d)=%s",
            start_gx, start_gy, target_x, target_y, hours_taken, gx, gy, landed);

    tool_result_ok(&res);
    /* No raw coordinates in the LLM-facing payload: the bot now reasons
     * purely in compass terms. Hours, landed tile type, and energy are
     * still useful. */
    snprintf(res.json, TOOL_RESULT_MAX_LEN,
        "{\"ok\":true,\"hours_taken\":%d,\"move_limit\":%d,"
        "\"landed_tile\":\"%s\","
        "\"energy\":%d,\"hours_to_solar_flare\":%d}",
        hours_taken, MOVE_MAX_TILES,
        landed, gl_bot_energy, gl_hours_to_solar_flare);
    return res;
}

/* MoveTo(direction, distance_bucket): walk up to the bucket's step count
 * along the 8-way compass vector. Clamps to world bounds; gl_move_to
 * handles the actual tile-by-tile traversal. */
ToolResult gl_move_direction_bucket(Direction dir, DistBucket dist) {
    ToolResult res = {0};
    if (dir == DIR_NONE) {
        tool_result_err(&res,
            "MoveTo needs a direction (north, north-east, east, south-east, "
            "south, south-west, west, or north-west).");
        return res;
    }
    if (dist == DIST_NONE) {
        tool_result_err(&res,
            "MoveTo needs either 'distance' (adjacent|close|medium|far) "
            "or 'target' (a tile type seen in the last LookFar).");
        return res;
    }
    int ux = 0, uy = 0;
    direction_unit(dir, &ux, &uy);
    int steps = dist_bucket_walk_tiles(dist);
    int start_gx, start_gy;
    gl_bot_grid_pos(&start_gx, &start_gy);
    int tx = start_gx + ux * steps;
    int ty = start_gy + uy * steps;
    return gl_move_to(tx, ty);
}

/* MoveTo(direction, target_type): requires a prior LookFar to have cached
 * the feature. Also requires the cached direction to be in the same
 * octant as the requested one, so bad asks get a clear, targeted error
 * instead of silently redirecting the bot somewhere wrong. */
ToolResult gl_move_direction_target(Direction dir, TileType target_type) {
    ToolResult res = {0};
    if (dir == DIR_NONE) {
        tool_result_err(&res,
            "MoveTo needs a direction (north, north-east, east, south-east, "
            "south, south-west, west, or north-west).");
        return res;
    }
    if (target_type < 0 || target_type >= TILE_TYPE_COUNT) {
        tool_result_err(&res, "Unknown target tile type.");
        return res;
    }
    known_features_revalidate();
    if (!gl_last_lookfar[target_type].valid) {
        char err[256];
        snprintf(err, sizeof(err),
            "No %s known yet. Call LookFar before targeting it.",
            tile_type_name(target_type));
        tool_result_err(&res, err);
        return res;
    }
    if (gl_last_lookfar[target_type].dir != dir) {
        char err[256];
        snprintf(err, sizeof(err),
            "The known %s is %s (%s), not %s. Use MoveTo(direction=%s, "
            "target=%s) or call LookFar again.",
            tile_type_name(target_type),
            direction_name(gl_last_lookfar[target_type].dir),
            dist_bucket_name(gl_last_lookfar[target_type].dist),
            direction_name(dir),
            direction_name(gl_last_lookfar[target_type].dir),
            tile_type_name(target_type));
        tool_result_err(&res, err);
        return res;
    }
    int fx = gl_last_lookfar[target_type].x;
    int fy = gl_last_lookfar[target_type].y;
    return gl_move_to(fx, fy);
}

ToolResult gl_look_close(void) {
    ToolResult res = {0};
    if (!gl_advance_solar_flare_hour(-1)) {
        tool_result_err(&res, "Destroyed by solar flare.");
        return res;
    }
    consume_energy(1);
    int gx, gy;
    gl_bot_grid_pos(&gx, &gy);

    char surrounding[1600] = "[";
    int slen = 1;
    pthread_mutex_lock(&gl_tiles_lock);
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            int nx = gx + dx, ny = gy + dy;
            if (nx < 0 || nx >= GRID_WIDTH || ny < 0 || ny >= GRID_HEIGHT) continue;
            gl_tile_matrix[nx][ny].fog = false;
            Tile *t = &gl_tile_matrix[nx][ny];
            const char *pos = (dx == 0 && dy == 0) ? "center" : "";
            slen += snprintf(surrounding + slen, sizeof(surrounding) - slen,
                "%s{\"x\":%d,\"y\":%d,\"type\":\"%s\",\"position\":\"%s\"}",
                slen > 1 ? "," : "", nx, ny, tile_type_name(t->type), pos);
        }
    }
    pthread_mutex_unlock(&gl_tiles_lock);
    strncat(surrounding, "]", sizeof(surrounding) - strlen(surrounding) - 1);

    /* Cyan scan ring, radius = 1.5 tiles in world units. */
    particles_spawn_scan_pulse(gl_bot_x + TILE_SIZE / 2.0f,
                               gl_bot_y + TILE_SIZE / 2.0f,
                               (float)TILE_SIZE * 1.8f);

    msg_log("  [LookClose] Bot at (%d,%d)", gx, gy);
    tool_result_ok(&res);
    snprintf(res.json, TOOL_RESULT_MAX_LEN,
        "{\"ok\":true,\"bot_tile_x\":%d,\"bot_tile_y\":%d,"
        "\"surrounding\":%s,\"energy\":%d,\"hours_to_solar_flare\":%d}",
        gx, gy, surrounding, gl_bot_energy, gl_hours_to_solar_flare);
    return res;
}

ToolResult gl_look_far(void) {
    ToolResult res = {0};
    if (!gl_advance_solar_flare_hour(-1)) {
        tool_result_err(&res, "Destroyed by solar flare.");
        return res;
    }
    consume_energy(1);
    int gx, gy;
    gl_bot_grid_pos(&gx, &gy);
    int radius = gl_bot_lookfar_distance;

    /* Snapshot tile types */
    static TileType snap[GRID_WIDTH][GRID_HEIGHT];
    pthread_mutex_lock(&gl_tiles_lock);
    for (int x = 0; x < GRID_WIDTH; x++)
        for (int y = 0; y < GRID_HEIGHT; y++)
            snap[x][y] = gl_tile_matrix[x][y].type;
    pthread_mutex_unlock(&gl_tiles_lock);

    /* Track best (nearest) feature per tile type */
    typedef struct { TileType type; int x, y; float dist; } Feature;
    Feature best[TILE_TYPE_COUNT];
    for (int i = 0; i < TILE_TYPE_COUNT; i++) best[i].dist = 1e9f;

    for (int tx = gx - radius; tx <= gx + radius; tx++) {
        for (int ty = gy - radius; ty <= gy + radius; ty++) {
            if (tx < 0 || tx >= GRID_WIDTH || ty < 0 || ty >= GRID_HEIGHT) continue;
            int ddx = tx - gx, ddy = ty - gy;
            float d = sqrtf((float)(ddx*ddx + ddy*ddy));
            if (d > radius) continue;

            /* Simple LOS: check if rocks block the path */
            bool blocked = false;
            int steps = (int)(d * 2) + 1;
            for (int s = 1; s < steps && !blocked; s++) {
                float t = (float)s / steps;
                int ix = gx + (int)((tx - gx) * t);
                int iy = gy + (int)((ty - gy) * t);
                if (ix >= 0 && ix < GRID_WIDTH && iy >= 0 && iy < GRID_HEIGHT) {
                    if (snap[ix][iy] == TILE_ROCKS && !(ix == tx && iy == ty))
                        blocked = true;
                }
            }
            if (blocked) continue;

            /* Reveal fog */
            pthread_mutex_lock(&gl_tiles_lock);
            gl_tile_matrix[tx][ty].fog = false;
            pthread_mutex_unlock(&gl_tiles_lock);

            TileType tt = snap[tx][ty];
            if (d < best[tt].dist) {
                best[tt] = (Feature){tt, tx, ty, d};
            }
        }
    }

    /* Invalidate every type, then repopulate only what we saw. Any type
     * not represented in `best` this turn was either not in range or
     * fully occluded, so stale cache entries would mislead the LLM. */
    known_features_reset();

    /* Keep these tight enough that features + summary + JSON envelope fits
     * inside TOOL_RESULT_MAX_LEN (2048). */
    char features[900] = "[";
    int flen = 1;
    char summary[420] = "";
    size_t slen = 0;
    bool any_feature = false;
    for (int i = 0; i < TILE_TYPE_COUNT; i++) {
        if (best[i].dist > 1e8f) continue;
        int ddx = best[i].x - gx, ddy = best[i].y - gy;
        Direction  dir  = direction_from_delta(ddx, ddy);
        int        tiles = (int)(best[i].dist + 0.5f);
        if (tiles < 1) tiles = 1;
        DistBucket dist = dist_bucket_from_tiles(tiles);
        const char *type_name = tile_type_name(best[i].type);

        gl_last_lookfar[i] = (KnownFeature){
            .valid = true,
            .x = best[i].x, .y = best[i].y,
            .dir = dir, .dist = dist,
            .game_hour = gl_bot_hour_count,
        };

        flen += snprintf(features + flen, sizeof(features) - flen,
            "%s{\"type\":\"%s\",\"direction\":\"%s\",\"distance\":\"%s\"}",
            flen > 1 ? "," : "",
            type_name,
            direction_name(dir),
            dist_bucket_name(dist));

        char Name[32];
        snprintf(Name, sizeof(Name), "%s", type_name);
        if (Name[0] >= 'a' && Name[0] <= 'z') Name[0] = (char)(Name[0] - 'a' + 'A');
        int n = snprintf(summary + slen, sizeof(summary) - slen,
            "%s%s %s (%s).",
            any_feature ? " " : "",
            Name, direction_name(dir), dist_bucket_name(dist));
        if (n > 0 && (size_t)n < sizeof(summary) - slen) slen += (size_t)n;
        any_feature = true;
    }
    strncat(features, "]", sizeof(features) - strlen(features) - 1);
    if (!any_feature) snprintf(summary, sizeof(summary), "Nothing notable in view.");

    /* Big cyan scan ring, radius = gl_bot_lookfar_distance tiles. */
    particles_spawn_scan_pulse(gl_bot_x + TILE_SIZE / 2.0f,
                               gl_bot_y + TILE_SIZE / 2.0f,
                               (float)(radius * TILE_SIZE));

    msg_log("  [LookFar] %s", summary);
    tool_result_ok(&res);
    /* Escape backslashes/quotes in the summary would be nice, but the
     * generator only uses ASCII letters, digits, spaces, dots and
     * parentheses, so direct interpolation is safe. */
    snprintf(res.json, TOOL_RESULT_MAX_LEN,
        "{\"ok\":true,\"features\":%s,\"summary\":\"%s\","
        "\"energy\":%d,\"hours_to_solar_flare\":%d}",
        features, summary, gl_bot_energy, gl_hours_to_solar_flare);
    return res;
}

ToolResult gl_dig(void) {
    ToolResult res = {0};
    if (!gl_advance_solar_flare_hour(-1)) {
        tool_result_err(&res, "Destroyed by solar flare.");
        return res;
    }
    consume_energy(1);
    int gx, gy;
    gl_bot_grid_pos(&gx, &gy);

    pthread_mutex_lock(&gl_tiles_lock);
    TileType t = gl_tile_matrix[gx][gy].type;
    pthread_mutex_unlock(&gl_tiles_lock);

    if (t != TILE_ROCKS) {
        char err[256];
        snprintf(err, sizeof(err),
            "No rocks on the current tile (it is %s). Dig requires the bot "
            "to stand on a 'rocks' tile.", tile_type_name(t));
        msg_log("  [Dig] rejected at (%d,%d): current=%s", gx, gy, tile_type_name(t));
        tool_result_err(&res, err);
        return res;
    }
    create_tile(gx, gy, TILE_GRAVEL);
    gl_bot_inventory_rocks++;
    particles_spawn_dig_dust((float)gx * TILE_SIZE + TILE_SIZE / 2.0f,
                             (float)gy * TILE_SIZE + TILE_SIZE / 2.0f);
    msg_log("  [Dig] Rock at (%d,%d)! Inventory: %d", gx, gy, gl_bot_inventory_rocks);
    tool_result_ok(&res);
    snprintf(res.json, TOOL_RESULT_MAX_LEN,
        "{\"ok\":true,\"rock_obtained\":true,\"rocks_in_inventory\":%d,"
        "\"current_tile\":\"gravel\","
        "\"energy\":%d,\"hours_to_solar_flare\":%d}",
        gl_bot_inventory_rocks, gl_bot_energy, gl_hours_to_solar_flare);
    return res;
}

ToolResult gl_create(const char *tile_type_str) {
    ToolResult res = {0};
    if (!gl_advance_solar_flare_hour(-1)) {
        tool_result_err(&res, "Destroyed by solar flare.");
        return res;
    }
    consume_energy(1);
    int gx, gy;
    gl_bot_grid_pos(&gx, &gy);

    TileType want = tile_type_from_name(tile_type_str);
    if (!tile_is_buildable(want)) {
        char err[256];
        snprintf(err, sizeof(err), "Cannot build '%s'. Allowed: habitat, battery, solar_panel.", tile_type_str);
        tool_result_err(&res, err);
        return res;
    }

    pthread_mutex_lock(&gl_tiles_lock);
    TileType cur = gl_tile_matrix[gx][gy].type;
    pthread_mutex_unlock(&gl_tiles_lock);

    if (tile_is_buildable(cur)) {
        char err[256];
        snprintf(err, sizeof(err),
            "Current tile already has %s; move to an empty buildable tile first.",
            tile_type_name(cur));
        tool_result_err(&res, err);
        return res;
    }
    if (cur == TILE_WATER || cur == TILE_BROKEN_HABITAT) {
        char err[256];
        snprintf(err, sizeof(err),
            "Cannot build on %s; move to gravel, sand, or rocks first.",
            tile_type_name(cur));
        tool_result_err(&res, err);
        return res;
    }

    int cost = tile_build_cost(want);
    if (gl_bot_inventory_rocks < cost) {
        char err[256];
        snprintf(err, sizeof(err), "Need %d rocks, have %d.", cost, gl_bot_inventory_rocks);
        tool_result_err(&res, err);
        return res;
    }

    gl_bot_inventory_rocks -= cost;
    create_tile(gx, gy, want);
    particles_spawn_build_sparkle((float)gx * TILE_SIZE + TILE_SIZE / 2.0f,
                                  (float)gy * TILE_SIZE + TILE_SIZE / 2.0f);

    pthread_mutex_lock(&gl_built_lock);
    if (gl_bot_built_count < MAX_BUILT_TILES) {
        gl_bot_built[gl_bot_built_count++] = (BuiltRecord){
            .x = gx, .y = gy, .type = want, .game_hour = gl_bot_hour_count
        };
    }
    pthread_mutex_unlock(&gl_built_lock);

    msg_log("  [Create] Built %s at (%d,%d). Rocks left: %d",
            tile_type_name(want), gx, gy, gl_bot_inventory_rocks);
    tool_result_ok(&res);
    /* Warn loudly if the bot just consumed a rocks tile: without this, small
     * models keep referencing the old LookFar 'rocks' entry and try to come
     * back here to Dig. The cache is already invalidated by
     * known_features_revalidate, but telling the LLM directly is cheaper
     * than making it re-read STATUS. */
    char note[192] = "";
    if (cur == TILE_ROCKS) {
        snprintf(note, sizeof(note),
            ",\"note\":\"This tile was 'rocks' and is now '%s'. Any older "
            "LookFar entry for 'rocks' may be stale; call LookFar to refresh.\"",
            tile_type_name(want));
    }
    snprintf(res.json, TOOL_RESULT_MAX_LEN,
        "{\"ok\":true,\"tile_created\":\"%s\","
        "\"previous_tile_type\":\"%s\","
        "\"rocks_consumed\":%d,\"rocks_in_inventory\":%d,"
        "\"energy\":%d,\"hours_to_solar_flare\":%d%s}",
        tile_type_name(want), tile_type_name(cur),
        cost, gl_bot_inventory_rocks,
        gl_bot_energy, gl_hours_to_solar_flare, note);
    return res;
}

ToolResult gl_list_built_tiles(void) {
    ToolResult res = {0};
    if (!gl_advance_solar_flare_hour(-1)) {
        tool_result_err(&res, "Destroyed by solar flare.");
        return res;
    }
    consume_energy(1);

    int gx, gy;
    gl_bot_grid_pos(&gx, &gy);

    int counts[TILE_TYPE_COUNT] = {0};
    char built_arr[1400] = "[";
    int blen = 1;
    pthread_mutex_lock(&gl_built_lock);
    int n = gl_bot_built_count;
    for (int i = 0; i < n && blen < 1300; i++) {
        int ddx = gl_bot_built[i].x - gx;
        int ddy = gl_bot_built[i].y - gy;
        Direction  dir = direction_from_delta(ddx, ddy);
        int        tiles = (int)(sqrtf((float)(ddx*ddx + ddy*ddy)) + 0.5f);
        DistBucket dist = (ddx == 0 && ddy == 0) ? DIST_CLOSE
                                                 : dist_bucket_from_tiles(tiles);
        counts[gl_bot_built[i].type]++;
        blen += snprintf(built_arr + blen, sizeof(built_arr) - blen,
            "%s{\"type\":\"%s\",\"direction\":\"%s\",\"distance\":\"%s\","
            "\"game_hour\":%d}",
            blen > 1 ? "," : "",
            tile_type_name(gl_bot_built[i].type),
            (dir == DIR_NONE) ? "here" : direction_name(dir),
            dist_bucket_name(dist),
            gl_bot_built[i].game_hour);
    }
    pthread_mutex_unlock(&gl_built_lock);
    strncat(built_arr, "]", sizeof(built_arr) - strlen(built_arr) - 1);

    char counts_str[192];
    snprintf(counts_str, sizeof(counts_str),
        "{\"habitat\":%d,\"battery\":%d,\"solar_panel\":%d}",
        counts[TILE_HABITAT], counts[TILE_BATTERY], counts[TILE_SOLAR_PANEL]);

    char summary[192];
    snprintf(summary, sizeof(summary),
        "Built so far: %d habitat, %d battery, %d solar_panel.",
        counts[TILE_HABITAT], counts[TILE_BATTERY], counts[TILE_SOLAR_PANEL]);

    msg_log("  [ListBuiltTiles] %s", summary);
    tool_result_ok(&res);
    snprintf(res.json, TOOL_RESULT_MAX_LEN,
        "{\"ok\":true,\"built\":%s,\"count\":%d,\"counts\":%s,"
        "\"summary\":\"%s\","
        "\"energy\":%d,\"hours_to_solar_flare\":%d}",
        built_arr, n, counts_str, summary,
        gl_bot_energy, gl_hours_to_solar_flare);
    return res;
}

/* Wait: stay on the current tile for one hour. Follows the same flare-
 * advance + 1-energy-cost contract as every other tool so the bookkeeping
 * matches. The habitat +25 recharge for the *starting* tile was already
 * applied at the top of agent_loop, so calling Wait while standing on a
 * powered habitat nets +23 energy per hour. */
ToolResult gl_wait(void) {
    ToolResult res = {0};
    if (!gl_advance_solar_flare_hour(-1)) {
        tool_result_err(&res, "Destroyed by solar flare.");
        return res;
    }
    consume_energy(1);

    int gx, gy;
    gl_bot_grid_pos(&gx, &gy);
    pthread_mutex_lock(&gl_tiles_lock);
    TileType t = gl_tile_matrix[gx][gy].type;
    pthread_mutex_unlock(&gl_tiles_lock);
    bool on_powered_habitat =
        (t == TILE_HABITAT) && habitat_on_solar_network(gx, gy);

    /* Hold the charging sprite for the whole hour while recharging, instead
     * of the 0.8s flash apply_habitat_network_charge sets at hour start.
     * The next hour's apply_habitat_network_charge will reset the timer to
     * the short flash value, so this doesn't stall the agent loop. */
    if (on_powered_habitat) {
        gl_bot_state = BOT_CHARGING;
        gl_charging_animation_until =
            (double)clock() / CLOCKS_PER_SEC + 10.0;
    } else {
        gl_bot_state = BOT_WAITING;
    }

    msg_log("  [Wait] %s", on_powered_habitat
        ? "recharging on powered habitat."
        : "idle (no recharge; not on a powered habitat).");

    tool_result_ok(&res);
    snprintf(res.json, TOOL_RESULT_MAX_LEN,
        "{\"ok\":true,\"on_powered_habitat\":%s,"
        "\"energy\":%d,\"hours_to_solar_flare\":%d}",
        on_powered_habitat ? "true" : "false",
        gl_bot_energy, gl_hours_to_solar_flare);
    return res;
}

void gl_print_hour_status(void) {
    int gx, gy;
    gl_bot_grid_pos(&gx, &gy);
    msg_log("  === STATUS === Energy:%d Pos:(%d,%d) Flare in:%d Rocks:%d",
            gl_bot_energy, gx, gy, gl_hours_to_solar_flare, gl_bot_inventory_rocks);
}

/* ── Post-mortem summary ────────────────────────────────────────────────── */
/* Flood-fills connected {habitat, battery, solar_panel} groups (orthogonal
 * edges only) starting from each un-visited structure cell, and counts how
 * many groups contain at least one habitat AND one battery AND one
 * solar_panel. Operates on a local visited[] grid so it is safe to call
 * while the game keeps rendering. */
static int count_powered_clusters(const unsigned char *structure) {
    const int W = GRID_WIDTH, H = GRID_HEIGHT;
    unsigned char *visited = (unsigned char *)calloc((size_t)W * H, 1);
    if (!visited) return 0;
    int *stack = (int *)malloc(sizeof(int) * (size_t)W * H);
    if (!stack) { free(visited); return 0; }
    int powered = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int idx = y * W + x;
            if (!structure[idx] || visited[idx]) continue;
            int top = 0;
            stack[top++] = idx;
            visited[idx] = 1;
            int has_hab = 0, has_bat = 0, has_sol = 0;
            while (top > 0) {
                int p = stack[--top];
                int px = p % W, py = p / W;
                if (structure[p] == (unsigned char)TILE_HABITAT)     has_hab = 1;
                else if (structure[p] == (unsigned char)TILE_BATTERY) has_bat = 1;
                else if (structure[p] == (unsigned char)TILE_SOLAR_PANEL) has_sol = 1;
                const int dx[4] = {1, -1, 0, 0};
                const int dy[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; k++) {
                    int nx = px + dx[k], ny = py + dy[k];
                    if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                    int ni = ny * W + nx;
                    if (!structure[ni] || visited[ni]) continue;
                    visited[ni] = 1;
                    stack[top++] = ni;
                }
            }
            if (has_hab && has_bat && has_sol) powered++;
        }
    }
    free(stack);
    free(visited);
    return powered;
}

void gl_compute_postmortem(PostmortemStats *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->hours_survived   = gl_bot_hour_count;
    out->final_energy     = gl_bot_energy;
    out->final_rocks      = gl_bot_inventory_rocks;
    out->destroyed_by_flare = (gl_bot_state == BOT_DESTROYED);
    gl_bot_spawn_grid_pos(&out->spawn_gx, &out->spawn_gy);
    gl_bot_grid_pos(&out->final_gx, &out->final_gy);
    int dx = out->final_gx - out->spawn_gx;
    int dy = out->final_gy - out->spawn_gy;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    out->travel_chebyshev = adx > ady ? adx : ady;
    out->tiles_total = GRID_WIDTH * GRID_HEIGHT;

    const int W = GRID_WIDTH, H = GRID_HEIGHT;
    unsigned char *structure = (unsigned char *)calloc((size_t)W * H, 1);
    out->explore_min_gx = W;
    out->explore_min_gy = H;
    out->explore_max_gx = -1;
    out->explore_max_gy = -1;

    pthread_mutex_lock(&gl_tiles_lock);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            Tile *t = &gl_tile_matrix[x][y];
            if (!t->fog) {
                out->tiles_discovered++;
                if (x < out->explore_min_gx) out->explore_min_gx = x;
                if (x > out->explore_max_gx) out->explore_max_gx = x;
                if (y < out->explore_min_gy) out->explore_min_gy = y;
                if (y > out->explore_max_gy) out->explore_max_gy = y;
            }
            if (structure) {
                if (t->type == TILE_HABITAT || t->type == TILE_BATTERY ||
                    t->type == TILE_SOLAR_PANEL) {
                    structure[y * W + x] = (unsigned char)t->type;
                }
            }
        }
    }
    pthread_mutex_unlock(&gl_tiles_lock);

    if (out->explore_max_gx < 0) {
        /* No fog cleared at all — bot was destroyed before any observation. */
        out->explore_min_gx = out->explore_max_gx = out->spawn_gx;
        out->explore_min_gy = out->explore_max_gy = out->spawn_gy;
    }

    pthread_mutex_lock(&gl_built_lock);
    for (int i = 0; i < gl_bot_built_count; i++) {
        switch (gl_bot_built[i].type) {
            case TILE_HABITAT:     out->built_habitats++;     break;
            case TILE_BATTERY:     out->built_batteries++;    break;
            case TILE_SOLAR_PANEL: out->built_solar_panels++; break;
            default: break;
        }
    }
    out->built_total = gl_bot_built_count;
    pthread_mutex_unlock(&gl_built_lock);

    if (structure) {
        out->powered_clusters = count_powered_clusters(structure);
        free(structure);
    }
}

/* ── Frame update ────────────────────────────────────────────────────────── */
void gl_update(float dt) {
    static float move_dust_accum   = 0.0f;
    static float charge_spark_accum = 0.0f;

    float diff_x = gl_bot_target_x - gl_bot_x;
    float diff_y = gl_bot_target_y - gl_bot_y;
    float dist = sqrtf(diff_x * diff_x + diff_y * diff_y);
    if (dist > 0.5f) {
        gl_bot_state = BOT_MOVING;
        float step = BOT_MOVE_SPEED * dt;
        if (step >= dist) {
            gl_bot_x = gl_bot_target_x;
            gl_bot_y = gl_bot_target_y;
        } else {
            gl_bot_x += (diff_x / dist) * step;
            gl_bot_y += (diff_y / dist) * step;
        }
        /* Drop a small dust puff every ~0.08s while moving. */
        move_dust_accum += dt;
        if (move_dust_accum >= 0.08f) {
            move_dust_accum = 0.0f;
            particles_spawn_move_dust(gl_bot_x + TILE_SIZE / 2.0f,
                                      gl_bot_y + TILE_SIZE / 2.0f);
        }
    } else if (gl_bot_state == BOT_MOVING) {
        gl_bot_state = BOT_WAITING;
        move_dust_accum = 0.0f;
    }

    if (gl_bot_state == BOT_CHARGING) {
        charge_spark_accum += dt;
        if (charge_spark_accum >= 0.12f) {
            charge_spark_accum = 0.0f;
            particles_spawn_charge_spark(gl_bot_x + TILE_SIZE / 2.0f,
                                         gl_bot_y + TILE_SIZE / 2.0f);
        }
        double now = GetTime();
        if (now >= gl_charging_animation_until)
            gl_bot_state = BOT_WAITING;
    } else {
        charge_spark_accum = 0.0f;
    }
}
