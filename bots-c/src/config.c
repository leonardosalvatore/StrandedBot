#include "config.h"
#include "cJSON.h"
#include "raylib.h"  /* GetApplicationDirectory, TraceLog */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Hardcoded fallback defaults ──────────────────────────────────────────── */
/* These mirror the values that used to be inline in main.c / llm_agent.c.
 * They are only used if bots-defaults.json cannot be read; having them in
 * code means the binary still runs when executed outside the repo. */

static const char *FALLBACK_EXPLORER_MISSION =
    "MISSION: explore the map. Your unit of progress is distance covered "
    "from your starting position. Each turn either MoveTo in a fresh "
    "direction using 'distance=far' (prefer big jumps, not 1-tile steps), "
    "or LookFar when the Known features list is empty or points only to "
    "tiles behind you. Only build a small recharge base (habitat + "
    "solar_panel + battery, all orthogonally adjacent: N/E/S/W "
    "neighbours) when energy drops below 400. When you do build, any "
    "tile where SYSTEM STATUS shows current_tile_buildable=true works "
    "\xe2\x80\x94 gravel, sand, and rocks all count. Do not waste hours "
    "hunting for a 'better' tile.";

static const char *FALLBACK_BUILDER_MISSION =
    "MISSION: expand habitats to build a big town. Your unit of progress "
    "is the number of habitat tiles placed. Each turn do ONE of: "
    "(a) Create a habitat/battery/solar_panel if SYSTEM STATUS shows "
    "current_tile_buildable=true and you have the rocks, "
    "(b) MoveTo(direction=<N|E|S|W>, distance=close) to step to an "
    "orthogonal neighbour of the last placed structure so the cluster "
    "keeps growing, or (c) Dig rocks when Current=rocks and inventory "
    "is too low to build your next piece. Place structures in "
    "orthogonally-connected clusters (habitat + battery + solar_panel "
    "sharing N/E/S/W edges) so the network charges; diagonal "
    "neighbours do NOT connect. Any gravel, sand, or rocks tile is a "
    "valid build spot; don't hunt for a special one. Keep pushing into "
    "fresh tiles.";

static const char *FALLBACK_SYSTEM_PROMPT =
    "You are the operator of a single autonomous robot on a tile grid. "
    "You act by calling ONE tool per turn via native tool_calls. The "
    "bot never sees raw coordinates; it reasons in compass directions "
    "(north, north-east, east, south-east, south, south-west, west, "
    "north-west) and distance buckets (adjacent|close|medium|far).\n"
    "\n"
    "OUTPUT STYLE (strict):\n"
    "- Reply in AT MOST 2 short sentences, then emit exactly ONE tool call.\n"
    "- Never print tool calls as text or pseudo-JSON; use the tool_calls channel.\n"
    "- Never restate the telemetry the system already gave you.\n"
    "- No numbered 'Next Steps' lists, no headings, no markdown.\n"
    "\n"
    "HARD RULES:\n"
    "- Trust the SYSTEM STATUS line; it is ground truth.\n"
    "- Each turn you also get a 'Neighbours:' line listing the 8 tiles\n"
    "  touching the bot, labelled N, NE, E, SE, S, SW, W, NW, plus\n"
    "  'Current='. Use it to pick an orthogonally-adjacent Create target\n"
    "  or a single-step MoveTo without burning a LookFar.\n"
    "- MoveTo requires 'direction' plus EXACTLY ONE of:\n"
    "    * 'distance' in {adjacent, close, medium, far} \xe2\x80\x94 walks that\n"
    "      bucket in that direction (adjacent=1 tile, close~=3, medium~=8,\n"
    "      far~=20), or\n"
    "    * 'target' \xe2\x80\x94 a tile type the most recent LookFar reported\n"
    "      in that same direction. If the cache is empty or the feature\n"
    "      lies in a different direction, call LookFar first.\n"
    "- LookFar reveals the nearest feature of every tile type within its\n"
    "  scan radius and tags each with a direction + distance bucket.\n"
    "  Those entries persist as 'Known features' until a Dig/Create\n"
    "  changes them or the bot wanders out of range. Do NOT call LookFar\n"
    "  every turn \xe2\x80\x94 reuse the Known features list.\n"
    "- If the tile you want is already visible in the current Neighbours\n"
    "  line, use MoveTo(direction=<that label>, distance=adjacent). Do\n"
    "  NOT use 'target=...' for something in Neighbours \xe2\x80\x94 'target'\n"
    "  reads the older LookFar cache and may point elsewhere or be stale.\n"
    "- Dig is only useful when you need more rocks for a PLANNED Create.\n"
    "  Dig only when Current=rocks; the tile becomes gravel.\n"
    "- Create acts on the CURRENT tile whenever SYSTEM STATUS says\n"
    "  current_tile_buildable=true (gravel, sand, or rocks). If that\n"
    "  flag is true AND you have enough rocks, Create NOW.\n"
    "- Power requires ONLY orthogonal adjacency of habitat + battery +\n"
    "  solar_panel tiles (the N, E, S, W neighbours in the Neighbours\n"
    "  line). NE, SE, SW, NW neighbours do NOT count. There is no\n"
    "  wiring, no cabling, no 'connect' step. When extending a cluster,\n"
    "  ALWAYS MoveTo(direction=<N|E|S|W>, distance=adjacent) so you land\n"
    "  on an orthogonal neighbour of the last placed structure, then\n"
    "  Create. distance=close walks 3 tiles and breaks adjacency.\n"
    "- Dig and Create change the current tile. Old Known-features entries\n"
    "  that pointed to it are dropped automatically; trust the newest\n"
    "  tool results and the Neighbours line over older LookFar memory.\n"
    "- If energy drops below 200, move onto a habitat tile and stay\n"
    "  there until recharged.";

static const char *FALLBACK_TOOL_MOVE_TO =
    "Walk the bot in a compass direction. Supply 'direction' plus "
    "EXACTLY ONE of 'distance' (adjacent|close|medium|far) or 'target' "
    "(a tile type the last LookFar reported in that direction). "
    "adjacent=1 tile (use for cluster extension). Up to %d tiles per "
    "call; costs 1 energy per tile.";

static const char *FALLBACK_TOOL_LOOK_FAR =
    "Wide scan radius %d tiles. Returns the nearest feature per tile "
    "type, each tagged with direction (north, north-east, ...) and "
    "distance bucket (adjacent|close|medium|far). Rock fields block "
    "line-of-sight. Costs 1 energy.";

static const char *FALLBACK_TOOL_DIG =
    "Dig a rock from the current 'rocks' tile. Adds 1 rock to inventory. "
    "Tile becomes gravel. Costs 1 energy.";

static const char *FALLBACK_TOOL_CREATE =
    "Create a structure on your CURRENT tile. "
    "Valid ground: gravel, sand, or rocks. "
    "Invalid ground: water, broken_habitat, or any tile that already has "
    "a structure. "
    "Rock cost: habitat=%d, battery=%d, solar_panel=%d. Also costs 1 energy.";

static const char *FALLBACK_TOOL_LIST_BUILT =
    "Returns every structure the bot placed via Create, each tagged with "
    "direction and distance bucket relative to the bot now. Costs 1 energy.";

/* ── File helpers ─────────────────────────────────────────────────────────── */

/* Attempts to read an entire file into a null-terminated malloc'd buffer.
 * Returns NULL if the file cannot be opened. */
static char *read_file_alloc(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t r = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[r] = '\0';
    return buf;
}

/* Search order: current working directory, then the raylib application
 * directory. First file that opens wins. Returns a malloc'd string or
 * NULL. Writes the winning path into out_path when non-NULL. */
static char *read_config_file(const char *name, char *out_path, size_t out_path_cap) {
    char *buf = read_file_alloc(name);
    if (buf) {
        if (out_path) snprintf(out_path, out_path_cap, "%s", name);
        return buf;
    }
    const char *appdir = GetApplicationDirectory();
    if (appdir && appdir[0]) {
        char path[1024];
        snprintf(path, sizeof(path), "%s%s", appdir, name);
        buf = read_file_alloc(path);
        if (buf) {
            if (out_path) snprintf(out_path, out_path_cap, "%s", path);
            return buf;
        }
    }
    return NULL;
}

/* ── String copy helpers ──────────────────────────────────────────────────── */

static void set_str(char *dst, size_t cap, const char *src) {
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

/* Copy a JSON string into dst if the item exists and is a string; otherwise
 * leave dst unchanged (so fallbacks already written in previous stages stay). */
static void copy_json_string(cJSON *parent, const char *key, char *dst, size_t cap) {
    cJSON *item = cJSON_GetObjectItem(parent, key);
    if (item && cJSON_IsString(item) && item->valuestring) {
        set_str(dst, cap, item->valuestring);
    }
}

static void copy_json_int(cJSON *parent, const char *key, int *dst) {
    cJSON *item = cJSON_GetObjectItem(parent, key);
    if (item && cJSON_IsNumber(item)) *dst = item->valueint;
}

static void copy_json_bool(cJSON *parent, const char *key, bool *dst) {
    cJSON *item = cJSON_GetObjectItem(parent, key);
    if (cJSON_IsBool(item))      *dst = cJSON_IsTrue(item);
    else if (cJSON_IsNumber(item)) *dst = item->valueint != 0;
}

/* ── Hardcoded-default population ─────────────────────────────────────────── */

static void apply_fallback_defaults(BotsConfig *out) {
    memset(out, 0, sizeof(*out));

    /* Explorer preset: sparse map, fewer rocks needed, flares far apart. */
    out->explorer.rocks_amount            = 20;
    out->explorer.initial_town_size       = 0;
    out->explorer.energy                  = 1000;
    out->explorer.inventory_rocks         = 10;
    out->explorer.hours_solar_flare_every = 100;
    set_str(out->explorer.mission_prompt, sizeof(out->explorer.mission_prompt),
            FALLBACK_EXPLORER_MISSION);

    /* Builder preset: dense map, generous rocks, extremely long flare gap. */
    out->builder.rocks_amount            = 100;
    out->builder.initial_town_size       = 0;
    out->builder.energy                  = 1000;
    out->builder.inventory_rocks         = 100;
    out->builder.hours_solar_flare_every = 3000;
    set_str(out->builder.mission_prompt, sizeof(out->builder.mission_prompt),
            FALLBACK_BUILDER_MISSION);

    out->default_scenario = 0;
    out->interactive_mode = true;

    set_str(out->llama.start_script, sizeof(out->llama.start_script),
            "./start-llama-server.sh");
    out->llama.auto_start = true;
    out->llama.port       = 53425; /* unprivileged, well outside common defaults */

    set_str(out->prompts.system, sizeof(out->prompts.system),
            FALLBACK_SYSTEM_PROMPT);
    set_str(out->prompts.tool_move_to, sizeof(out->prompts.tool_move_to),
            FALLBACK_TOOL_MOVE_TO);
    set_str(out->prompts.tool_look_far, sizeof(out->prompts.tool_look_far),
            FALLBACK_TOOL_LOOK_FAR);
    set_str(out->prompts.tool_dig, sizeof(out->prompts.tool_dig),
            FALLBACK_TOOL_DIG);
    set_str(out->prompts.tool_create, sizeof(out->prompts.tool_create),
            FALLBACK_TOOL_CREATE);
    set_str(out->prompts.tool_list_built_tiles,
            sizeof(out->prompts.tool_list_built_tiles),
            FALLBACK_TOOL_LIST_BUILT);
}

/* ── JSON overlay ─────────────────────────────────────────────────────────── */

static void overlay_scenario(cJSON *parent, const char *key,
                             ScenarioConfig *out) {
    cJSON *sc = cJSON_GetObjectItem(parent, key);
    if (!cJSON_IsObject(sc)) return;
    copy_json_int   (sc, "rocks_amount",            &out->rocks_amount);
    copy_json_int   (sc, "initial_town_size",       &out->initial_town_size);
    copy_json_int   (sc, "energy",                  &out->energy);
    copy_json_int   (sc, "inventory_rocks",         &out->inventory_rocks);
    copy_json_int   (sc, "hours_solar_flare_every", &out->hours_solar_flare_every);
    copy_json_string(sc, "mission_prompt",
                     out->mission_prompt, sizeof(out->mission_prompt));
}

/* Overlay the values found in `json_text` on top of `out`. Fields not present
 * in the JSON are left untouched so callers can stack this on top of
 * hardcoded defaults. */
static bool overlay_json(BotsConfig *out, const char *json_text,
                         const char *source_path) {
    cJSON *root = cJSON_Parse(json_text);
    if (!root) {
        const char *err = cJSON_GetErrorPtr();
        TraceLog(LOG_WARNING, "CONFIG: parse error in %s near: %s",
                 source_path ? source_path : "<mem>",
                 err ? err : "(unknown)");
        return false;
    }

    cJSON *scenarios = cJSON_GetObjectItem(root, "scenarios");
    if (cJSON_IsObject(scenarios)) {
        overlay_scenario(scenarios, "explorer", &out->explorer);
        overlay_scenario(scenarios, "builder",  &out->builder);
    }

    cJSON *game = cJSON_GetObjectItem(root, "game");
    if (cJSON_IsObject(game)) {
        cJSON *ds = cJSON_GetObjectItem(game, "default_scenario");
        if (cJSON_IsString(ds) && ds->valuestring) {
            out->default_scenario =
                (strcmp(ds->valuestring, "builder") == 0) ? 1 : 0;
        } else {
            copy_json_int(game, "default_scenario", &out->default_scenario);
        }
        copy_json_bool(game, "interactive_mode", &out->interactive_mode);
    }

    cJSON *llama = cJSON_GetObjectItem(root, "llama");
    if (cJSON_IsObject(llama)) {
        copy_json_string(llama, "start_script",
                         out->llama.start_script, sizeof(out->llama.start_script));
        copy_json_bool(llama, "auto_start", &out->llama.auto_start);
        copy_json_int (llama, "port",       &out->llama.port);
    }

    cJSON *prompts = cJSON_GetObjectItem(root, "prompts");
    if (cJSON_IsObject(prompts)) {
        copy_json_string(prompts, "system",
                         out->prompts.system, sizeof(out->prompts.system));
        copy_json_string(prompts, "tool_move_to",
                         out->prompts.tool_move_to, sizeof(out->prompts.tool_move_to));
        copy_json_string(prompts, "tool_look_far",
                         out->prompts.tool_look_far, sizeof(out->prompts.tool_look_far));
        copy_json_string(prompts, "tool_dig",
                         out->prompts.tool_dig, sizeof(out->prompts.tool_dig));
        copy_json_string(prompts, "tool_create",
                         out->prompts.tool_create, sizeof(out->prompts.tool_create));
        copy_json_string(prompts, "tool_list_built_tiles",
                         out->prompts.tool_list_built_tiles,
                         sizeof(out->prompts.tool_list_built_tiles));
    }

    cJSON_Delete(root);
    return true;
}

/* ── Serialization ────────────────────────────────────────────────────────── */

static cJSON *scenario_to_json(const ScenarioConfig *sc) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "rocks_amount",            sc->rocks_amount);
    cJSON_AddNumberToObject(obj, "initial_town_size",       sc->initial_town_size);
    cJSON_AddNumberToObject(obj, "energy",                  sc->energy);
    cJSON_AddNumberToObject(obj, "inventory_rocks",         sc->inventory_rocks);
    cJSON_AddNumberToObject(obj, "hours_solar_flare_every", sc->hours_solar_flare_every);
    cJSON_AddStringToObject(obj, "mission_prompt",          sc->mission_prompt);
    return obj;
}

static cJSON *config_to_json(const BotsConfig *cfg) {
    cJSON *root = cJSON_CreateObject();

    cJSON *game = cJSON_CreateObject();
    cJSON_AddStringToObject(game, "default_scenario",
                            cfg->default_scenario == 1 ? "builder" : "explorer");
    cJSON_AddBoolToObject(game, "interactive_mode", cfg->interactive_mode);
    cJSON_AddItemToObject(root, "game", game);

    cJSON *scenarios = cJSON_CreateObject();
    cJSON_AddItemToObject(scenarios, "explorer", scenario_to_json(&cfg->explorer));
    cJSON_AddItemToObject(scenarios, "builder",  scenario_to_json(&cfg->builder));
    cJSON_AddItemToObject(root, "scenarios", scenarios);

    cJSON *llama = cJSON_CreateObject();
    cJSON_AddStringToObject(llama, "start_script", cfg->llama.start_script);
    cJSON_AddBoolToObject  (llama, "auto_start",   cfg->llama.auto_start);
    cJSON_AddNumberToObject(llama, "port",         cfg->llama.port);
    cJSON_AddItemToObject(root, "llama", llama);

    cJSON *prompts = cJSON_CreateObject();
    cJSON_AddStringToObject(prompts, "system",                cfg->prompts.system);
    cJSON_AddStringToObject(prompts, "tool_move_to",          cfg->prompts.tool_move_to);
    cJSON_AddStringToObject(prompts, "tool_look_far",         cfg->prompts.tool_look_far);
    cJSON_AddStringToObject(prompts, "tool_dig",              cfg->prompts.tool_dig);
    cJSON_AddStringToObject(prompts, "tool_create",           cfg->prompts.tool_create);
    cJSON_AddStringToObject(prompts, "tool_list_built_tiles", cfg->prompts.tool_list_built_tiles);
    cJSON_AddItemToObject(root, "prompts", prompts);

    return root;
}

static bool write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t n = strlen(data);
    bool ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    return ok;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

bool config_load_defaults(BotsConfig *out) {
    apply_fallback_defaults(out);
    char path[1024] = "";
    char *json = read_config_file(BOTS_CONFIG_DEFAULTS_FILE, path, sizeof(path));
    if (!json) {
        TraceLog(LOG_INFO,
                 "CONFIG: no %s found; using hardcoded defaults",
                 BOTS_CONFIG_DEFAULTS_FILE);
        return true;
    }
    overlay_json(out, json, path);
    free(json);
    TraceLog(LOG_INFO, "CONFIG: loaded defaults from %s", path);
    return true;
}

bool config_load_custom(BotsConfig *out) {
    config_load_defaults(out);
    char path[1024] = "";
    char *json = read_config_file(BOTS_CONFIG_CUSTOM_FILE, path, sizeof(path));
    if (!json) {
        TraceLog(LOG_INFO,
                 "CONFIG: no %s yet; using defaults (first run is fine)",
                 BOTS_CONFIG_CUSTOM_FILE);
        return true;
    }
    overlay_json(out, json, path);
    free(json);
    TraceLog(LOG_INFO, "CONFIG: loaded custom overrides from %s", path);
    return true;
}

bool config_save_custom(const BotsConfig *in) {
    cJSON *root = config_to_json(in);
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) return false;

    bool ok = write_file(BOTS_CONFIG_CUSTOM_FILE, text);
    if (ok) {
        TraceLog(LOG_INFO, "CONFIG: wrote %s", BOTS_CONFIG_CUSTOM_FILE);
    } else {
        TraceLog(LOG_WARNING, "CONFIG: could not write %s",
                 BOTS_CONFIG_CUSTOM_FILE);
    }
    free(text);
    return ok;
}

bool config_revert_custom_to_defaults(void) {
    BotsConfig cfg;
    config_load_defaults(&cfg);
    return config_save_custom(&cfg);
}

/* ── Active-config pointer ────────────────────────────────────────────────── */

static const BotsConfig *g_active_config = NULL;

void config_set_active(const BotsConfig *cfg) { g_active_config = cfg; }
const BotsConfig *config_active(void)         { return g_active_config; }
