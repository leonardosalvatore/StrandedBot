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

float   gl_bot_x = 400.0f, gl_bot_y = 550.0f;
float   gl_bot_target_x = 400.0f, gl_bot_target_y = 550.0f;
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

void gl_initialize_world(bool use_fog, int rocks_target) {
    gl_enable_fog_of_war = use_fog;
    gl_world_rocks_target = rocks_target > 0 ? rocks_target : 1;
    gl_bot_hour_count = 0;
    gl_hours_to_solar_flare = gl_hours_solar_flare_every;
    solar_flare_last_hour = -1;
    gl_solar_flare_animation_active = false;
    gl_bot_state = BOT_WAITING;
    gl_bot_last_speech[0] = '\0';
    gl_bot_x = 400.0f; gl_bot_y = 550.0f;
    gl_bot_target_x = 400.0f; gl_bot_target_y = 550.0f;

    pthread_mutex_lock(&gl_built_lock);
    gl_bot_built_count = 0;
    pthread_mutex_unlock(&gl_built_lock);

    initialize_default_tiles();
    build_scenery_procedural(gl_world_rocks_target);
    if (gl_initial_town_size > 0)
        generate_initial_town(gl_initial_town_size);
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
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "Already at (%d,%d). MoveTo target must differ from current "
                 "position; pick a different tool or a different target.",
                 start_gx, start_gy);
        tool_result_err(&res, buf);
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
    snprintf(res.json, TOOL_RESULT_MAX_LEN,
        "{\"ok\":true,\"target_tile_x\":%d,\"target_tile_y\":%d,"
        "\"hours_taken\":%d,\"move_limit\":%d,"
        "\"tile_x\":%d,\"tile_y\":%d,\"tile_type\":\"%s\","
        "\"energy\":%d,\"hours_to_solar_flare\":%d}",
        target_x, target_y, hours_taken, MOVE_MAX_TILES,
        gx, gy, landed, gl_bot_energy, gl_hours_to_solar_flare);
    return res;
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

    char features[1400] = "[";
    int flen = 1;
    for (int i = 0; i < TILE_TYPE_COUNT; i++) {
        if (best[i].dist > 1e8f) continue;
        flen += snprintf(features + flen, sizeof(features) - flen,
            "%s{\"type\":\"%s\",\"x\":%d,\"y\":%d,\"distance\":%.1f}",
            flen > 1 ? "," : "", tile_type_name(best[i].type),
            best[i].x, best[i].y, best[i].dist);
    }
    strncat(features, "]", sizeof(features) - strlen(features) - 1);

    /* Big cyan scan ring, radius = gl_bot_lookfar_distance tiles. */
    particles_spawn_scan_pulse(gl_bot_x + TILE_SIZE / 2.0f,
                               gl_bot_y + TILE_SIZE / 2.0f,
                               (float)(radius * TILE_SIZE));

    msg_log("  [LookFar] radius %d from (%d,%d)", radius, gx, gy);
    tool_result_ok(&res);
    snprintf(res.json, TOOL_RESULT_MAX_LEN,
        "{\"ok\":true,\"bot_tile_x\":%d,\"bot_tile_y\":%d,"
        "\"features\":%s,\"energy\":%d,\"hours_to_solar_flare\":%d}",
        gx, gy, features, gl_bot_energy, gl_hours_to_solar_flare);
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
        snprintf(err, sizeof(err), "No rocks at (%d,%d). Current: %s.", gx, gy, tile_type_name(t));
        msg_log("  [Dig] %s", err);
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
        "\"tile_x\":%d,\"tile_y\":%d,\"energy\":%d,\"hours_to_solar_flare\":%d}",
        gl_bot_inventory_rocks, gx, gy, gl_bot_energy, gl_hours_to_solar_flare);
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
        snprintf(err, sizeof(err), "Tile (%d,%d) already has %s.", gx, gy, tile_type_name(cur));
        tool_result_err(&res, err);
        return res;
    }
    if (cur == TILE_WATER || cur == TILE_BROKEN_HABITAT) {
        char err[256];
        snprintf(err, sizeof(err), "Cannot build on %s at (%d,%d).", tile_type_name(cur), gx, gy);
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
     * models keep referencing the old LookFar result that called this
     * coordinate "rocks" and try to come back here to Dig. */
    char note[192] = "";
    if (cur == TILE_ROCKS) {
        snprintf(note, sizeof(note),
            ",\"note\":\"This tile was 'rocks' and is now '%s'. It cannot be"
            " dug anymore. Any older LookFar data calling (%d,%d) 'rocks' is"
            " stale.\"",
            tile_type_name(want), gx, gy);
    }
    snprintf(res.json, TOOL_RESULT_MAX_LEN,
        "{\"ok\":true,\"tile_created\":\"%s\",\"tile_x\":%d,\"tile_y\":%d,"
        "\"previous_tile_type\":\"%s\","
        "\"rocks_consumed\":%d,\"rocks_in_inventory\":%d,"
        "\"energy\":%d,\"hours_to_solar_flare\":%d%s}",
        tile_type_name(want), gx, gy, tile_type_name(cur),
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
    char built_arr[1400] = "[";
    int blen = 1;
    pthread_mutex_lock(&gl_built_lock);
    for (int i = 0; i < gl_bot_built_count && blen < 1300; i++) {
        blen += snprintf(built_arr + blen, sizeof(built_arr) - blen,
            "%s{\"x\":%d,\"y\":%d,\"type\":\"%s\",\"game_hour\":%d}",
            blen > 1 ? "," : "",
            gl_bot_built[i].x, gl_bot_built[i].y,
            tile_type_name(gl_bot_built[i].type), gl_bot_built[i].game_hour);
    }
    int n = gl_bot_built_count;
    pthread_mutex_unlock(&gl_built_lock);
    strncat(built_arr, "]", sizeof(built_arr) - strlen(built_arr) - 1);

    msg_log("  [ListBuiltTiles] %d recorded builds.", n);
    tool_result_ok(&res);
    snprintf(res.json, TOOL_RESULT_MAX_LEN,
        "{\"ok\":true,\"built\":%s,\"count\":%d,\"energy\":%d,\"hours_to_solar_flare\":%d}",
        built_arr, n, gl_bot_energy, gl_hours_to_solar_flare);
    return res;
}

void gl_print_hour_status(void) {
    int gx, gy;
    gl_bot_grid_pos(&gx, &gy);
    msg_log("  === STATUS === Energy:%d Pos:(%d,%d) Flare in:%d Rocks:%d",
            gl_bot_energy, gx, gy, gl_hours_to_solar_flare, gl_bot_inventory_rocks);
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
