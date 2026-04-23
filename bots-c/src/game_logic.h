#ifndef GAME_LOGIC_H
#define GAME_LOGIC_H

#include <stdbool.h>
#include <pthread.h>

/* ── Window / grid constants ─────────────────────────────────────────────── */
#define MAP_WIDTH       1400
#define SIDEBAR_WIDTH   520
#define WIN_WIDTH       (MAP_WIDTH + SIDEBAR_WIDTH)
#define MAP_HEIGHT      830
#define PANEL_HEIGHT    500
#define WIN_HEIGHT      (MAP_HEIGHT + PANEL_HEIGHT)

#define TILE_SIZE       4
#define GRID_WIDTH      (MAP_WIDTH / TILE_SIZE)
#define GRID_HEIGHT     (MAP_HEIGHT / TILE_SIZE)

#define VIEWPORT_TILES_W 70
#define VIEWPORT_TILES_H 70
#define DRAW_TILE_SIZE   (MAP_WIDTH / VIEWPORT_TILES_W)

#define BOT_RADIUS      10
#define BOT_SPEED       220
#define BOT_MOVE_SPEED  (TILE_SIZE * 2)
#define MOVE_MAX_TILES  20

/* ── Default new-game values ─────────────────────────────────────────────── */
#define STARTING_BOT_ENERGY              2000
#define STARTING_INVENTORY_ROCKS         100
#define STARTING_WORLD_ROCKS_TARGET      200
#define STARTING_INITIAL_TOWN_SIZE       4
#define STARTING_HOURS_SOLAR_FLARE_EVERY 2000

#define ROCKS_REQUIRED_FOR_HABITAT     1
#define ROCKS_REQUIRED_FOR_BATTERY     1
#define ROCKS_REQUIRED_FOR_SOLAR_PANEL 1
#define HABITAT_SOLAR_CHARGE           25

/* ── Tile types (enum) ───────────────────────────────────────────────────── */
typedef enum {
    TILE_GRAVEL = 0,
    TILE_SAND,
    TILE_WATER,
    TILE_ROCKS,
    TILE_HABITAT,
    TILE_BATTERY,
    TILE_SOLAR_PANEL,
    TILE_BROKEN_HABITAT,
    TILE_TYPE_COUNT
} TileType;

/* ── Bot states ──────────────────────────────────────────────────────────── */
typedef enum {
    BOT_WAITING = 0,
    BOT_THINKING,
    BOT_MOVING,
    BOT_LOOKCLOSE,
    BOT_LOOKFAR,
    BOT_CHARGING,
    BOT_DESTROYED,
    BOT_STATE_COUNT
} BotState;

/* ── Tile structure ──────────────────────────────────────────────────────── */
typedef struct {
    int       x, y;
    TileType  type;
    unsigned char r, g, b;
    bool      fog;
    float     habitat_damage;   /* 0..100; only meaningful for TILE_HABITAT */
} Tile;

/* ── Tool result (simple key-value bag returned as JSON) ─────────────────── */
#define TOOL_RESULT_MAX_LEN 2048
typedef struct {
    bool ok;
    char json[TOOL_RESULT_MAX_LEN];
} ToolResult;

/* ── Direction / distance vocabulary ─────────────────────────────────────── */
/* 8-way compass directions used in all LLM-facing strings. The game grid
 * has y growing downward, so DIR_N corresponds to dy<0. */
typedef enum {
    DIR_NONE = 0,
    DIR_N, DIR_NE, DIR_E, DIR_SE,
    DIR_S, DIR_SW, DIR_W, DIR_NW,
    DIR_COUNT
} Direction;

/* Distance buckets replace numeric tile counts in every LLM-facing string.
 *   adjacent : exactly 1 tile (orthogonal N/E/S/W or diagonal NE/NW/SE/SW
 *              neighbour of the bot; used for cluster extension)
 *   close    : 2-5 tiles
 *   medium   : 6-15 tiles
 *   far      : 16+ tiles (clamped by MOVE_MAX_TILES in moves) */
typedef enum {
    DIST_NONE = 0,
    DIST_ADJACENT,
    DIST_CLOSE,
    DIST_MEDIUM,
    DIST_FAR,
    DIST_COUNT
} DistBucket;

const char *direction_name(Direction d);            /* e.g. "north-east" */
Direction   direction_from_delta(int dx, int dy);   /* 8-bin atan2 */
Direction   direction_from_name(const char *s);     /* parses many forms */
void        direction_unit(Direction d, int *dx, int *dy);

const char *dist_bucket_name(DistBucket b);         /* "close" */
DistBucket  dist_bucket_from_tiles(int tiles);      /* thresholds */
int         dist_bucket_walk_tiles(DistBucket b);   /* suggested step size */
DistBucket  dist_bucket_from_name(const char *s);

/* ── Global world state (accessed from multiple threads; use gl_tiles_lock) ─ */
extern Tile         gl_tile_matrix[GRID_WIDTH][GRID_HEIGHT];
extern pthread_mutex_t gl_tiles_lock;

extern float  gl_bot_x, gl_bot_y;
extern float  gl_bot_target_x, gl_bot_target_y;
extern int    gl_bot_energy;
extern int    gl_bot_inventory_rocks;
extern BotState gl_bot_state;
extern char   gl_bot_last_speech[4096];
extern int    gl_bot_lookfar_distance;
extern int    gl_bot_hour_count;

extern int    gl_hours_solar_flare_every;
extern int    gl_hours_to_solar_flare;

extern bool   gl_solar_flare_animation_active;
extern double gl_solar_flare_animation_start_time;
extern double gl_charging_animation_until;

extern bool   gl_enable_fog_of_war;
extern int    gl_world_rocks_target;
extern int    gl_initial_town_size;

/* ── Build knowledge (for ListBuiltTiles) ────────────────────────────────── */
#define MAX_BUILT_TILES 4096
typedef struct { int x, y; TileType type; int game_hour; } BuiltRecord;
extern BuiltRecord gl_bot_built[MAX_BUILT_TILES];
extern int         gl_bot_built_count;
extern pthread_mutex_t gl_built_lock;

/* ── Buildable tile helpers ──────────────────────────────────────────────── */
bool tile_is_buildable(TileType t);
int  tile_build_cost(TileType t);

/* ── API ─────────────────────────────────────────────────────────────────── */
const char *tile_type_name(TileType t);
TileType    tile_type_from_name(const char *name);

void  gl_initialize_world(bool use_fog, int rocks_target);
void  gl_update(float dt);

/* Tools */
ToolResult gl_move_to(int target_x, int target_y);
ToolResult gl_move_direction_bucket(Direction dir, DistBucket dist);
ToolResult gl_move_direction_target(Direction dir, TileType target_type);
ToolResult gl_look_close(void);
ToolResult gl_look_far(void);
ToolResult gl_dig(void);
ToolResult gl_create(const char *tile_type_str);
ToolResult gl_list_built_tiles(void);

/* Neighbours line: "Neighbours: N=gravel, NE=sand, ... . Current=gravel." */
void gl_build_neighbours_line(char *out, size_t cap);

/* Known-features summary populated from the last LookFar; empty string
 * when the cache is empty. Each kept entry becomes one sentence, e.g.
 * "Rocks south-west (close). Water north (medium)." */
void gl_build_known_features_summary(char *out, size_t cap);

/* Solar flare / hour */
bool gl_advance_solar_flare_hour(int current_hour);
void gl_apply_solar_flare_interval(int hours);
void gl_apply_initial_town_size(int n);
bool gl_bot_habitat_network_charge_active(void);
/* Detailed status of the power-network at the bot's current tile.
 * Every out-pointer may be NULL; callers typically use this to explain
 * WHY charging is inactive.
 *   on_habitat   - bot is standing on a TILE_HABITAT
 *   has_battery  - the orthogonally-connected structure cluster contains
 *                  at least one TILE_BATTERY (false if on_habitat is false)
 *   has_solar    - same for TILE_SOLAR_PANEL
 * Charging is active iff all three are true. */
void gl_bot_charge_network_status(bool *on_habitat,
                                  bool *has_battery,
                                  bool *has_solar);
void gl_print_hour_status(void);

/* Color lookup */
typedef struct { unsigned char r, g, b; } TileColor;
TileColor tile_color(TileType t);
const char *tile_description(TileType t);

/* Grid helpers */
void gl_bot_grid_pos(int *gx, int *gy);
/* Bot's initial spawn cell, captured by gl_initialize_world. */
void gl_bot_spawn_grid_pos(int *gx, int *gy);

/* ── Post-mortem summary ─────────────────────────────────────────────────── */
/* End-of-run statistics assembled by walking gl_tile_matrix and
 * gl_bot_built[]. Populated on demand by gl_compute_postmortem. */
typedef struct {
    int  hours_survived;
    int  spawn_gx, spawn_gy;
    int  final_gx, final_gy;
    int  final_energy;
    int  final_rocks;
    bool destroyed_by_flare;   /* true if gl_bot_state==BOT_DESTROYED,
                                * else the run ended on energy<=0 only. */
    /* Exploration */
    int  tiles_discovered;     /* fog=false cells */
    int  tiles_total;          /* GRID_WIDTH * GRID_HEIGHT */
    int  explore_min_gx, explore_max_gx;
    int  explore_min_gy, explore_max_gy;
    int  travel_chebyshev;     /* max(|final-spawn|) in tiles */
    /* Structures */
    int  built_habitats;
    int  built_batteries;
    int  built_solar_panels;
    int  built_total;
    int  powered_clusters;     /* habitat groups with >=1 battery AND
                                * >=1 solar_panel reachable by orthogonal
                                * habitat+structure flood-fill. */
} PostmortemStats;

void gl_compute_postmortem(PostmortemStats *out);

#endif /* GAME_LOGIC_H */
