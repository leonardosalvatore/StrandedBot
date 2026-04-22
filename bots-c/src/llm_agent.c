#include "llm_agent.h"
#include "game_logic.h"
#include "message_log.h"
#include "raylib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

#include "http_post.h"
#include "cJSON.h"
#include "config.h"

/* ── Configuration ───────────────────────────────────────────────────────── */
#define MAX_MESSAGES     512
#define MAX_MSG_CONTENT  8192
#define SIM_HOUR_WALL_SEC 2.0
#define LLM_CONTEXT_HOURS 30
#define USER_REPLY_TIMEOUT 10.0

/* Rough per-request byte budget for the messages array (system + user prompt
 * + chat history). Roughly 4 bytes per token, so ~16 KiB ≈ 4k tokens of
 * history. Tools schema + response buffer account for the rest of a typical
 * 8k context window. If the payload would exceed this, the oldest hours are
 * dropped from history until it fits. */
#define HISTORY_BYTE_BUDGET 16000

/* ── Message history ─────────────────────────────────────────────────────── */
#define MAX_TOOL_CALLS_JSON 4096
#define MAX_TOOL_CALL_ID    64

typedef struct {
    char role[16];
    char content[MAX_MSG_CONTENT];
    /* For role=="assistant": stringified JSON array to emit as "tool_calls".
     * Empty string means: no tool_calls field. */
    char tool_calls_json[MAX_TOOL_CALLS_JSON];
    /* For role=="tool": id that matches one of the previous assistant's
     * tool_calls[i].id. Empty if unknown. */
    char tool_call_id[MAX_TOOL_CALL_ID];
    int  game_hour;
} ChatMsg;

static ChatMsg  messages[MAX_MESSAGES];
static int      msg_count = 0;

static char     agent_model[256] = "";
static bool     agent_interactive = false;
static volatile bool agent_running_flag = false;
static pthread_t agent_thread;

/* Interactive reply */
static pthread_mutex_t reply_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  reply_cond = PTHREAD_COND_INITIALIZER;
static volatile bool   waiting_reply = false;
static char            reply_buf[4096] = "";
static volatile bool   reply_ready = false;
static double          reply_deadline = 0.0;

/* Curl replaced by POSIX socket http_post. */

/* ── Byte-budget trim ────────────────────────────────────────────────────── */
static size_t history_bytes(void) {
    size_t total = 0;
    for (int i = 0; i < msg_count; i++) {
        total += strlen(messages[i].content);
        total += strlen(messages[i].tool_calls_json);
        /* JSON overhead per message: role + field names + punctuation. */
        total += 40;
    }
    return total;
}

/* Drop the oldest whole game_hour from history[2..msg_count) until the
 * estimated payload size fits the budget. The first two messages (system +
 * initial user prompt) are always preserved. Dropping by whole hour keeps
 * the assistant/tool/synthetic-assistant group intact and preserves chat
 * template alternation. */
static void trim_history_to_budget(size_t budget_bytes) {
    while (msg_count > 2 && history_bytes() > budget_bytes) {
        int oldest = messages[2].game_hour;
        for (int i = 3; i < msg_count; i++) {
            if (messages[i].game_hour < oldest) oldest = messages[i].game_hour;
        }
        int w = 2;
        for (int r = 2; r < msg_count; r++) {
            if (messages[r].game_hour != oldest) {
                if (w != r) messages[w] = messages[r];
                w++;
            }
        }
        int removed = msg_count - w;
        msg_count = w;
        if (removed == 0) break; /* safety: nothing droppable */
    }
}

/* ── Append message ──────────────────────────────────────────────────────── */
static ChatMsg *append_msg(const char *role, const char *content, int hour) {
    if (msg_count >= MAX_MESSAGES) {
        /* Slide: keep first 2 (system + initial user), drop oldest after that */
        int keep_prefix = 2;
        int drop = MAX_MESSAGES / 4;
        if (drop < 1) drop = 1;
        int src = keep_prefix + drop;
        int n = msg_count - src;
        memmove(&messages[keep_prefix], &messages[src], n * sizeof(ChatMsg));
        msg_count = keep_prefix + n;
    }
    ChatMsg *m = &messages[msg_count++];
    memset(m, 0, sizeof(*m));
    strncpy(m->role, role, sizeof(m->role) - 1);
    strncpy(m->content, content, sizeof(m->content) - 1);
    m->game_hour = hour;
    return m;
}

/* Merge `extra` into the "content" string of an existing message object,
 * separated by a blank line. */
static void merge_into_content(cJSON *msg_obj, const char *extra) {
    if (!msg_obj || !extra) return;
    cJSON *c = cJSON_GetObjectItem(msg_obj, "content");
    const char *prev = (c && cJSON_IsString(c) && c->valuestring) ? c->valuestring : "";
    size_t n = strlen(prev) + strlen(extra) + 3;
    char *merged = (char *)malloc(n);
    if (!merged) return;
    if (prev[0])
        snprintf(merged, n, "%s\n\n%s", prev, extra);
    else
        snprintf(merged, n, "%s", extra);
    cJSON_ReplaceItemInObject(msg_obj, "content", cJSON_CreateString(merged));
    free(merged);
}

/* ── Build JSON messages array (trimmed by game hour window) ─────────────── */
/* Emits the persistent history verbatim, collapsing consecutive USER messages
 * (chat templates like ministral/mistral enforce strict alternation).
 * Assistant messages re-emit their stored tool_calls; tool messages re-emit
 * their tool_call_id so the template can match tool results to the preceding
 * assistant turn. */
static cJSON *build_messages_json(void) {
    cJSON *arr = cJSON_CreateArray();
    int min_hour = gl_bot_hour_count - LLM_CONTEXT_HOURS + 1;
    const char *last_role = "";
    cJSON *last_item = NULL;
    for (int i = 0; i < msg_count; i++) {
        bool keep = (i < 2) || (messages[i].game_hour >= min_hour);
        if (!keep) continue;
        const char *role = messages[i].role;
        const char *content = messages[i].content;

        /* Collapse consecutive user messages only. */
        if (strcmp(role, "user") == 0 && last_item &&
            strcmp(last_role, "user") == 0) {
            merge_into_content(last_item, content);
            continue;
        }

        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", role);
        cJSON_AddStringToObject(m, "content", content);

        if (strcmp(role, "assistant") == 0 && messages[i].tool_calls_json[0]) {
            cJSON *tc = cJSON_Parse(messages[i].tool_calls_json);
            if (tc) cJSON_AddItemToObject(m, "tool_calls", tc);
        }
        if (strcmp(role, "tool") == 0 && messages[i].tool_call_id[0]) {
            cJSON_AddStringToObject(m, "tool_call_id",
                                    messages[i].tool_call_id);
        }

        cJSON_AddItemToArray(arr, m);
        last_role = role;
        last_item = m;
    }
    return arr;
}

/* ── Build tools JSON ────────────────────────────────────────────────────── */
static cJSON *build_tools_json(void) {
    cJSON *tools = cJSON_CreateArray();

    /* Helper: create a tool entry */
    #define ADD_TOOL(NAME, DESC, PARAMS_BLOCK) do { \
        cJSON *t = cJSON_CreateObject(); \
        cJSON_AddStringToObject(t, "type", "function"); \
        cJSON *fn = cJSON_CreateObject(); \
        cJSON_AddStringToObject(fn, "name", NAME); \
        cJSON_AddStringToObject(fn, "description", DESC); \
        cJSON *params = cJSON_CreateObject(); \
        cJSON_AddStringToObject(params, "type", "object"); \
        cJSON *props = cJSON_CreateObject(); \
        cJSON *req = cJSON_CreateArray(); \
        PARAMS_BLOCK \
        cJSON_AddItemToObject(params, "properties", props); \
        cJSON_AddItemToObject(params, "required", req); \
        cJSON_AddItemToObject(fn, "parameters", params); \
        cJSON_AddItemToObject(t, "function", fn); \
        cJSON_AddItemToArray(tools, t); \
    } while(0)

    /* Pull tool-description templates from the active config (if any);
     * fallbacks match the previously hardcoded strings so the agent still
     * works before main() sets the active config. */
    const BotsConfig *cfg = config_active();
    const char *t_move =
        (cfg && cfg->prompts.tool_move_to[0])
            ? cfg->prompts.tool_move_to
            : "Move the bot toward a target tile (x,y) on the %dx%d grid. "
              "Up to %d tiles per call. Costs 1 energy per tile.";
    const char *t_look_far =
        (cfg && cfg->prompts.tool_look_far[0])
            ? cfg->prompts.tool_look_far
            : "Wide scan radius %d tiles. Returns notable features with coords "
              "and distance. Rock fields block line-of-sight. Costs 1 energy.";
    const char *t_dig =
        (cfg && cfg->prompts.tool_dig[0])
            ? cfg->prompts.tool_dig
            : "Dig a rock from current 'rocks' tile. Adds 1 rock to inventory. "
              "Tile becomes gravel. Costs 1 energy.";
    const char *t_create =
        (cfg && cfg->prompts.tool_create[0])
            ? cfg->prompts.tool_create
            : "Create a structure on your CURRENT tile. Valid ground: gravel, "
              "sand, or rocks. Invalid ground: water, broken_habitat, or any "
              "tile that already has a structure. Rock cost: habitat=%d, "
              "battery=%d, solar_panel=%d. Also costs 1 energy.";
    const char *t_list =
        (cfg && cfg->prompts.tool_list_built_tiles[0])
            ? cfg->prompts.tool_list_built_tiles
            : "Returns every structure the bot placed via Create. Costs 1 energy.";

    /* Format %d slots from the templates. Strings without placeholders are
     * unaffected; extra printf args are ignored, so user edits that drop
     * placeholders remain safe. */
    char move_desc[768];
    snprintf(move_desc, sizeof(move_desc), t_move,
             GRID_WIDTH, GRID_HEIGHT, MOVE_MAX_TILES);
    ADD_TOOL("MoveTo", move_desc, {
        cJSON *px = cJSON_CreateObject();
        cJSON_AddStringToObject(px, "type", "integer");
        cJSON_AddStringToObject(px, "description", "Target tile X index.");
        cJSON_AddItemToObject(props, "target_x", px);
        cJSON *py_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(py_obj, "type", "integer");
        cJSON_AddStringToObject(py_obj, "description", "Target tile Y index.");
        cJSON_AddItemToObject(props, "target_y", py_obj);
        cJSON_AddItemToArray(req, cJSON_CreateString("target_x"));
        cJSON_AddItemToArray(req, cJSON_CreateString("target_y"));
    });

    char lf_desc[384];
    snprintf(lf_desc, sizeof(lf_desc), t_look_far, gl_bot_lookfar_distance);
    ADD_TOOL("LookFar", lf_desc, { /* no params */ });

    ADD_TOOL("Dig", t_dig, { /* no params */ });

    char cr_desc[1280];
    snprintf(cr_desc, sizeof(cr_desc), t_create,
             ROCKS_REQUIRED_FOR_HABITAT, ROCKS_REQUIRED_FOR_BATTERY,
             ROCKS_REQUIRED_FOR_SOLAR_PANEL);
    ADD_TOOL("Create", cr_desc, {
        cJSON *tt = cJSON_CreateObject();
        cJSON_AddStringToObject(tt, "type", "string");
        cJSON_AddStringToObject(tt, "description", "Type: habitat, battery, or solar_panel.");
        cJSON_AddItemToObject(props, "tile_type", tt);
        cJSON_AddItemToArray(req, cJSON_CreateString("tile_type"));
    });

    ADD_TOOL("ListBuiltTiles", t_list, { /* no params */ });

    #undef ADD_TOOL
    return tools;
}

/* ── HTTP POST to llama-server ───────────────────────────────────────────── */
static char *llm_chat(cJSON *messages_json, cJSON *tools_json) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", agent_model);
    cJSON_AddItemToObject(body, "messages", cJSON_Duplicate(messages_json, 1));
    if (tools_json)
        cJSON_AddItemToObject(body, "tools", cJSON_Duplicate(tools_json, 1));
    /* Reserve enough budget for reasoning models to think AND produce a final
     * reply + tool call. Without this, llama-server's default can clip the
     * answer mid-sentence once the thinking trace is long. */
    cJSON_AddNumberToObject(body, "max_tokens", 2048);
    cJSON_AddNumberToObject(body, "temperature", 0.4);

    char *json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json_str) return NULL;

    /* Debug: write the outgoing request to /tmp so we can inspect it when
     * the server 500s on template errors. Only keeps the most recent call. */
    FILE *dbg = fopen("/tmp/bots_last_request.json", "w");
    if (dbg) { fputs(json_str, dbg); fclose(dbg); }

    const BotsConfig *cfg_port = config_active();
    int llm_port = (cfg_port && cfg_port->llama.port > 0)
                       ? cfg_port->llama.port : 53425;
    char *resp = http_post("127.0.0.1", llm_port,
                           "/v1/chat/completions",
                           "application/json",
                           json_str, 120);
    free(json_str);
    if (!resp) {
        msg_log("  [LLM] HTTP POST failed (is llama-server running on :%d?)",
                llm_port);
    } else {
        /* Log a short excerpt if the server returned an error envelope. */
        const char *err = strstr(resp, "\"error\"");
        if (err) {
            char snippet[256];
            snprintf(snippet, sizeof(snippet), "%.240s", err);
            msg_log("  [LLM] Server error: %s", snippet);
            FILE *dbg_resp = fopen("/tmp/bots_last_response.json", "w");
            if (dbg_resp) { fputs(resp, dbg_resp); fclose(dbg_resp); }
        }
    }
    return resp;
}

/* ── Extract a JSON object substring from arbitrary text ─────────────────── */
/* Returns a malloc'd copy of the first balanced {...} block, skipping strings
 * and escapes properly. Caller must free. Returns NULL if none found. */
static char *extract_first_json_object(const char *text) {
    if (!text) return NULL;
    const char *p = text;
    /* Prefer a fenced block if present (```json ... ``` or ``` ... ```). */
    const char *fence = strstr(p, "```");
    if (fence) {
        const char *start = fence + 3;
        /* Skip an optional language tag like "json" up to newline */
        while (*start && *start != '\n' && *start != '{') start++;
        if (*start == '{') p = start;
    }
    /* Find first '{' from p */
    while (*p && *p != '{') p++;
    if (*p != '{') return NULL;

    int depth = 0;
    bool in_str = false;
    bool escape = false;
    const char *start = p;
    for (; *p; p++) {
        char c = *p;
        if (in_str) {
            if (escape) { escape = false; continue; }
            if (c == '\\') { escape = true; continue; }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) {
                size_t len = (size_t)(p - start + 1);
                char *out = (char *)malloc(len + 1);
                if (!out) return NULL;
                memcpy(out, start, len);
                out[len] = '\0';
                return out;
            }
        }
    }
    return NULL;
}

/* Try to synthesize a tool_calls cJSON array from a free-form content string.
 * Handles these shapes:
 *   { "name": "MoveTo", "arguments": {...} }
 *   { "tool": "MoveTo", "arguments": {...} }            // alt key
 *   { "function": { "name": "...", "arguments": {...} } }
 *   { "tool_calls": [ ... ] }                           // already in OpenAI shape
 * Returns a new cJSON array (caller owns) or NULL. */
static cJSON *tool_calls_from_content(const char *content) {
    if (!content || !content[0]) return NULL;
    char *obj_str = extract_first_json_object(content);
    if (!obj_str) return NULL;
    cJSON *root = cJSON_Parse(obj_str);
    free(obj_str);
    if (!root) return NULL;

    /* Shape: { "tool_calls": [...] } — use directly. */
    cJSON *tc = cJSON_GetObjectItem(root, "tool_calls");
    if (cJSON_IsArray(tc)) {
        cJSON *dup = cJSON_Duplicate(tc, 1);
        cJSON_Delete(root);
        return dup;
    }

    /* Shape: { "function": { "name": ..., "arguments": ... } } */
    cJSON *fn = cJSON_GetObjectItem(root, "function");
    const char *name = NULL;
    cJSON *args = NULL;
    if (cJSON_IsObject(fn)) {
        cJSON *n = cJSON_GetObjectItem(fn, "name");
        if (n && cJSON_IsString(n)) name = n->valuestring;
        args = cJSON_GetObjectItem(fn, "arguments");
    }
    if (!name) {
        cJSON *n = cJSON_GetObjectItem(root, "name");
        if (!n) n = cJSON_GetObjectItem(root, "tool");
        if (n && cJSON_IsString(n)) name = n->valuestring;
    }
    if (!args) {
        args = cJSON_GetObjectItem(root, "arguments");
        if (!args) args = cJSON_GetObjectItem(root, "parameters");
        if (!args) args = cJSON_GetObjectItem(root, "args");
    }
    if (!name) { cJSON_Delete(root); return NULL; }

    cJSON *arr = cJSON_CreateArray();
    cJSON *call = cJSON_CreateObject();
    /* Synthesize an id so tool_call_id can reference it. */
    cJSON_AddStringToObject(call, "id", "call_synth_0");
    cJSON_AddStringToObject(call, "type", "function");
    cJSON *fn_out = cJSON_CreateObject();
    cJSON_AddStringToObject(fn_out, "name", name);
    if (args) {
        /* Some servers want arguments as a string; stringify it. */
        char *args_str = cJSON_PrintUnformatted(args);
        cJSON_AddStringToObject(fn_out, "arguments", args_str ? args_str : "{}");
        if (args_str) free(args_str);
    } else {
        cJSON_AddStringToObject(fn_out, "arguments", "{}");
    }
    cJSON_AddItemToObject(call, "function", fn_out);
    cJSON_AddItemToArray(arr, call);

    cJSON_Delete(root);
    return arr;
}

/* ── Dispatch a single tool call ─────────────────────────────────────────── */
static ToolResult dispatch_tool(const char *name, cJSON *args) {
    if (strcmp(name, "MoveTo") == 0) {
        int tx = 0, ty = 0;
        cJSON *jx = cJSON_GetObjectItem(args, "target_x");
        cJSON *jy = cJSON_GetObjectItem(args, "target_y");
        if (jx) tx = jx->valueint;
        if (jy) ty = jy->valueint;
        gl_bot_state = BOT_MOVING;
        return gl_move_to(tx, ty);
    }
    if (strcmp(name, "LookFar") == 0) {
        gl_bot_state = BOT_LOOKFAR;
        return gl_look_far();
    }
    if (strcmp(name, "Dig") == 0) {
        gl_bot_state = BOT_LOOKCLOSE;
        return gl_dig();
    }
    if (strcmp(name, "Create") == 0) {
        cJSON *jt = cJSON_GetObjectItem(args, "tile_type");
        const char *tt = jt ? jt->valuestring : "";
        gl_bot_state = BOT_LOOKCLOSE;
        return gl_create(tt);
    }
    if (strcmp(name, "ListBuiltTiles") == 0) {
        gl_bot_state = BOT_LOOKCLOSE;
        return gl_list_built_tiles();
    }
    ToolResult r = {0};
    snprintf(r.json, TOOL_RESULT_MAX_LEN, "{\"ok\":false,\"error\":\"Unknown tool: %s\"}", name);
    return r;
}

/* ── Base system prompt: rules that never change across scenarios ────────── */
/* Derived from observed failure modes of small local models:
 *   - calling MoveTo with current coordinates (no-op, wastes an hour)
 *   - re-calling LookFar every turn instead of acting on the previous scan
 *   - chasing tiles (sand/gravel/water) that have no mechanical use
 *   - verbose multi-paragraph replies that overflow max_tokens and clip
 *     the trailing tool-call arguments. */
/* Fallback text used if no config was loaded (e.g. early startup, unit
 * tests). The runtime SYSTEM prompt comes from config_active() via
 * active_system_prompt() below. */
static const char *SYSTEM_PROMPT_FALLBACK =
    "You are the operator of a single autonomous robot on a tile grid. "
    "You act by calling ONE tool per turn via native tool_calls.\n"
    "\n"
    "OUTPUT STYLE (strict):\n"
    "- Reply in AT MOST 2 short sentences, then emit exactly ONE tool call.\n"
    "- Never print tool calls as text or pseudo-JSON; use the tool_calls channel.\n"
    "- Never restate the telemetry the system already gave you.\n"
    "- No numbered 'Next Steps' lists, no headings, no markdown.\n"
    "\n"
    "HARD RULES:\n"
    "- Trust the SYSTEM STATUS line; it is ground truth.\n"
    "- Never call MoveTo with target equal to your current (x,y). Read the\n"
    "  position from SYSTEM STATUS before picking a target.\n"
    "- LookFar has radius 40 and reveals the nearest feature of every type.\n"
    "  Its results remain valid until you move more than ~15 tiles. Do NOT\n"
    "  call LookFar again within that radius — pick a target from the\n"
    "  previous LookFar result instead.\n"
    "- MoveTo can travel up to the tool's max tiles in one call. Prefer big\n"
    "  jumps to distant targets over 1-tile steps.\n"
    "- Dig is only useful when you need more rocks for a PLANNED Create.\n"
    "  Rocks have no value beyond building, so don't hoard past what your\n"
    "  plan requires. Dig once per rocks tile; the tile becomes gravel.\n"
    "- Create works on any tile where SYSTEM STATUS says\n"
    "  current_tile_buildable=true (that's gravel, sand, or rocks). There is\n"
    "  no 'better' tile. If current_tile_buildable=true AND you have enough\n"
    "  rocks for the thing you want, Create NOW; do not wander to find a\n"
    "  different tile.\n"
    "- Power requires ONLY orthogonal adjacency of habitat + battery +\n"
    "  solar_panel tiles. There is no wiring, no cabling, no 'connect' step.\n"
    "  Don't plan or narrate verifying wiring.\n"
    "- 'Orthogonally adjacent' means the Manhattan distance is EXACTLY 1:\n"
    "  |dx|+|dy|=1. So (100,137) is adjacent to (99,137), (101,137),\n"
    "  (100,136), (100,138) — and NOT to (99,136), (104,136), etc.\n"
    "  A solar_panel 2+ tiles from the battery contributes NOTHING; the\n"
    "  network will not charge. When you extend a cluster, MoveTo a tile\n"
    "  where |dx|+|dy|=1 from the last placed structure BEFORE calling\n"
    "  Create.\n"
    "- Dig and Create DESTROY the source tile: after Dig the tile is\n"
    "  gravel; after Create on a rocks tile the tile is the structure. A\n"
    "  coordinate that LookFar once reported as 'rocks' is no longer rocks\n"
    "  if you have already acted on it. Trust the most recent tool results,\n"
    "  not old LookFar memory.\n"
    "- If energy drops below 200, get onto a habitat tile and stay there\n"
    "  until recharged.";

static const char *active_system_prompt(void) {
    const BotsConfig *cfg = config_active();
    if (cfg && cfg->prompts.system[0]) return cfg->prompts.system;
    return SYSTEM_PROMPT_FALLBACK;
}

static const char *POWER_GRID_RULES =
    "Power works only through orthogonal adjacency of habitat, battery, and "
    "solar_panel tiles. There is no wiring or connect step.";

static void build_base_prompt(char *out, size_t cap, int scenario) {
    const char *head =
        "YOUR MISSION:\n"
        "Stay in a habitat if energy is below 200 to avoid solar flare damage,";

    if (scenario == 1) { /* Builder */
        snprintf(out, cap,
            "%s expand habitats to build a big town. "
            "Create habitats, batteries, and solar panels in expanding clusters.\n"
            "Keep pushing expansion forward instead of revisiting old tiles.\n"
            "%s\nEND OF YOUR MISSION DEFINITIONS",
            head, POWER_GRID_RULES);
    } else { /* Explorer */
        snprintf(out, cap,
            "%s explore the map. Build habitat and solar+storage network only to recharge. "
            "Keep moving forward instead of looping.\n"
            "%s\nEND OF YOUR MISSION DEFINITIONS",
            head, POWER_GRID_RULES);
    }
}

/* ── Wait for user reply (interactive mode) ──────────────────────────────── */
static const char *wait_for_user_reply(void) {
    pthread_mutex_lock(&reply_mtx);
    waiting_reply = true;
    reply_ready = false;
    reply_deadline = GetTime() + USER_REPLY_TIMEOUT;
    pthread_mutex_unlock(&reply_mtx);

    msg_log("  [System] Waiting for user reply (%.0fs timeout)...", USER_REPLY_TIMEOUT);

    while (1) {
        pthread_mutex_lock(&reply_mtx);
        if (reply_ready) {
            waiting_reply = false;
            pthread_mutex_unlock(&reply_mtx);
            return reply_buf;
        }
        double now = GetTime();
        if (now >= reply_deadline) {
            waiting_reply = false;
            strncpy(reply_buf, "Yes", sizeof(reply_buf));
            pthread_mutex_unlock(&reply_mtx);
            msg_log("  [System] No reply; defaulting to \"Yes\".");
            return reply_buf;
        }
        pthread_mutex_unlock(&reply_mtx);
        usleep(50000);
    }
}

/* Wrapper so agent thread can call consume_energy */
static void consume_energy_wrapper(void) {
    gl_bot_energy--;
    if (gl_bot_energy < 0) gl_bot_energy = 0;
}

/* ── Main play loop (runs in background thread) ─────────────────────────── */
static void *agent_loop(void *arg) {
    (void)arg;
    msg_log("============================================================");
    msg_log("LLM PLAY MODE - model: %s", agent_model);
    msg_log("Interactive: %s", agent_interactive ? "yes" : "no");
    msg_log("============================================================");

    /* Messages were already seeded by llm_agent_start (system + user prompt).
     * If somehow empty, add a fallback. */
    if (msg_count < 2) {
        append_msg("system", active_system_prompt(), 0);
        char fallback[4096];
        build_base_prompt(fallback, sizeof(fallback), 0);
        append_msg("user", fallback, 0);
    }

    cJSON *tools = build_tools_json();

    while (agent_running_flag) {
        gl_bot_hour_count++;
        int hour = gl_bot_hour_count;
        if (!gl_advance_solar_flare_hour(hour)) {
            msg_log("  [System] Bot destroyed by solar flare. Stopping.");
            break;
        }

        /* Wait for bot to finish moving */
        while (gl_bot_state == BOT_MOVING || gl_bot_state == BOT_CHARGING) {
            usleep(100000);
            if (!agent_running_flag) goto done;
        }

        msg_log("\n--- hour %d ---", hour);
        gl_bot_state = BOT_THINKING;
        consume_energy_wrapper();

        /* Build status grounding */
        int gx, gy;
        gl_bot_grid_pos(&gx, &gy);
        bool charge_on_hab = false, charge_has_bat = false, charge_has_sol = false;
        gl_bot_charge_network_status(&charge_on_hab, &charge_has_bat, &charge_has_sol);
        bool charge_active = charge_on_hab && charge_has_bat && charge_has_sol;
        pthread_mutex_lock(&gl_tiles_lock);
        TileType cur_tile_type = gl_tile_matrix[gx][gy].type;
        pthread_mutex_unlock(&gl_tiles_lock);
        const char *cur_tile = tile_type_name(cur_tile_type);
        /* Derived: can Create run here? Mirrors the checks in gl_create so
         * the bot sees the answer directly instead of having to infer it
         * from the tile name. Gravel, sand, and rocks are all valid; water,
         * broken_habitat, and existing structures are not. */
        bool cur_buildable = (cur_tile_type == TILE_GRAVEL ||
                              cur_tile_type == TILE_SAND   ||
                              cur_tile_type == TILE_ROCKS);
        /* Build a human-readable reason when the network is not charging, so
         * the bot doesn't have to deduce it from the prompt and current
         * topology. Small models respond much more reliably to an explicit
         * diagnosis than to a bare 'false'. */
        char charge_field[192];
        if (charge_active) {
            snprintf(charge_field, sizeof(charge_field),
                "habitat_hourly_charge_active=true");
        } else if (!charge_on_hab) {
            snprintf(charge_field, sizeof(charge_field),
                "habitat_hourly_charge_active=false (reason: "
                "bot not on a habitat tile)");
        } else if (!charge_has_bat && !charge_has_sol) {
            snprintf(charge_field, sizeof(charge_field),
                "habitat_hourly_charge_active=false (reason: "
                "habitat cluster has no battery AND no solar_panel "
                "orthogonally connected; |dx|+|dy|=1 required)");
        } else if (!charge_has_bat) {
            snprintf(charge_field, sizeof(charge_field),
                "habitat_hourly_charge_active=false (reason: "
                "habitat cluster is missing a battery orthogonally "
                "connected; |dx|+|dy|=1 required)");
        } else {
            snprintf(charge_field, sizeof(charge_field),
                "habitat_hourly_charge_active=false (reason: "
                "habitat cluster is missing a solar_panel orthogonally "
                "connected; |dx|+|dy|=1 required)");
        }
        char status[1280];
        snprintf(status, sizeof(status),
            "SYSTEM STATUS (truth source): hour=%d, energy=%d, position=(%d,%d), "
            "tile=%s, current_tile_buildable=%s, inventory_rocks=%d, "
            "hours_to_solar_flare=%d, %s",
            gl_bot_hour_count, gl_bot_energy, gx, gy,
            cur_tile,
            cur_buildable ? "true" : "false",
            gl_bot_inventory_rocks,
            gl_hours_to_solar_flare,
            charge_field);

        /* Persist status grounding as a user message in history so subsequent
         * turns see a clean user/assistant alternation (required by mistral
         * and similar chat templates). Consecutive user messages will be
         * collapsed at build time. */
        append_msg("user", status, hour);

        /* Keep the request from exceeding the server's context window. */
        trim_history_to_budget(HISTORY_BYTE_BUDGET);

        cJSON *msgs_json = build_messages_json();
        char *response_str = llm_chat(msgs_json, tools);
        cJSON_Delete(msgs_json);

        if (!response_str) {
            msg_log("  [LLM] No response; retrying in 5s...");
            usleep(5000000);
            continue;
        }

        cJSON *resp = cJSON_Parse(response_str);
        free(response_str);
        if (!resp) {
            msg_log("  [LLM] Failed to parse response JSON.");
            usleep(5000000);
            continue;
        }

        /* Extract message */
        cJSON *choices = cJSON_GetObjectItem(resp, "choices");
        cJSON *choice0 = choices ? cJSON_GetArrayItem(choices, 0) : NULL;
        cJSON *message = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;
        if (!message) {
            msg_log("  [LLM] No message in response.");
            cJSON_Delete(resp);
            usleep(5000000);
            continue;
        }

        cJSON *content_j = cJSON_GetObjectItem(message, "content");
        const char *content = (content_j && content_j->valuestring) ? content_j->valuestring : "";

        if (content[0]) {
            msg_log("  [Bot] %s", content);
            strncpy(gl_bot_last_speech, content, sizeof(gl_bot_last_speech) - 1);

            /* Interactive: check for question */
            if (agent_interactive && strchr(content, '?')) {
                append_msg("assistant", content, hour);
                const char *user_reply = wait_for_user_reply();
                msg_log("  [User Reply] %s", user_reply);
                append_msg("user", user_reply, hour);
                cJSON_Delete(resp);
                continue;
            }
        }

        ChatMsg *assistant_entry = append_msg("assistant", content, hour);

        /* Process tool calls */
        cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
        int n_calls = tool_calls ? cJSON_GetArraySize(tool_calls) : 0;

        /* Fallback: some models (small ones especially) print the tool call
         * as JSON inside the message content instead of emitting native
         * tool_calls. Parse it out. Do NOT append any user message here —
         * the chat template requires `tool` messages to directly follow the
         * `assistant` message. */
        cJSON *synth_tool_calls = NULL;
        if (n_calls == 0) {
            synth_tool_calls = tool_calls_from_content(content);
            if (synth_tool_calls) {
                tool_calls = synth_tool_calls;
                n_calls = cJSON_GetArraySize(tool_calls);
                msg_log("  [System] Parsed tool call from content (non-native).");
            }
        }

        if (n_calls == 0) {
            gl_bot_state = BOT_WAITING;
            msg_log("  [System] No tool call; nudging bot.");
            append_msg("user",
                "Keep exploring! Use LookFar or MoveTo. "
                "Emit a native tool call via the tools API, "
                "not a JSON code block.",
                hour);
            cJSON_Delete(resp);
            usleep(1000000);
            continue;
        }

        /* Persist tool_calls on the assistant entry so the chat template
         * recognizes the subsequent "tool" messages as belonging to it. */
        {
            char *tc_str = cJSON_PrintUnformatted(tool_calls);
            if (tc_str) {
                strncpy(assistant_entry->tool_calls_json, tc_str,
                        sizeof(assistant_entry->tool_calls_json) - 1);
                assistant_entry->tool_calls_json
                    [sizeof(assistant_entry->tool_calls_json) - 1] = '\0';
                free(tc_str);
            }
        }

        for (int i = 0; i < n_calls; i++) {
            cJSON *tc = cJSON_GetArrayItem(tool_calls, i);
            cJSON *fn = cJSON_GetObjectItem(tc, "function");
            if (!fn) continue;
            cJSON *fn_name_j = cJSON_GetObjectItem(fn, "name");
            cJSON *fn_args_j = cJSON_GetObjectItem(fn, "arguments");
            cJSON *tc_id_j = cJSON_GetObjectItem(tc, "id");
            const char *fn_name = fn_name_j ? fn_name_j->valuestring : "";
            const char *tc_id = (tc_id_j && cJSON_IsString(tc_id_j))
                                    ? tc_id_j->valuestring : NULL;

            cJSON *args = NULL;
            if (fn_args_j) {
                if (cJSON_IsString(fn_args_j))
                    args = cJSON_Parse(fn_args_j->valuestring);
                else if (cJSON_IsObject(fn_args_j))
                    args = cJSON_Duplicate(fn_args_j, 1);
            }
            if (!args) args = cJSON_CreateObject();

            char *args_pretty = cJSON_PrintUnformatted(args);
            msg_log("  [Tool Call] %s(%s)", fn_name, args_pretty ? args_pretty : "{}");
            if (args_pretty) free(args_pretty);
            ToolResult tr = dispatch_tool(fn_name, args);
            cJSON_Delete(args);

            ChatMsg *tool_entry = append_msg("tool", tr.json, hour);
            if (tc_id) {
                strncpy(tool_entry->tool_call_id, tc_id,
                        sizeof(tool_entry->tool_call_id) - 1);
                tool_entry->tool_call_id
                    [sizeof(tool_entry->tool_call_id) - 1] = '\0';
            }
        }

        /* Mistral/Ministral chat template: `assistant` messages that have
         * `tool_calls` and `tool` messages are BOTH skipped by the
         * alternation check (user, assistant, user, assistant, ...).
         * After the tool results we therefore need an `assistant` message
         * without tool_calls to restore alternation before the next `user`
         * status. Empty content is fine — it renders as just eos_token. */
        append_msg("assistant", "", hour);

        if (synth_tool_calls) cJSON_Delete(synth_tool_calls);
        cJSON_Delete(resp);
        gl_bot_state = BOT_WAITING;
        gl_print_hour_status();

        if (gl_bot_energy <= 0) {
            msg_log("  *** ROBOT SHUT DOWN — OUT OF ENERGY ***");
            break;
        }
        usleep(500000);
    }

done:
    if (tools) cJSON_Delete(tools);
    agent_running_flag = false;
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────────────────── */
void llm_agent_start(const char *initial_prompt, bool interactive_mode) {
    if (agent_running_flag) return;
    /* llama.cpp's server ignores the "model" field (it serves whatever model
       was loaded at startup), but the OpenAI-compatible schema requires the
       field to exist. We send a fixed placeholder. */
    strncpy(agent_model, "local", sizeof(agent_model) - 1);
    agent_interactive = interactive_mode;
    agent_running_flag = true;

    /* Reset messages and add initial prompt */
    msg_count = 0;
    if (initial_prompt && initial_prompt[0]) {
        append_msg("system", active_system_prompt(), 0);
        append_msg("user", initial_prompt, 0);
    }

    pthread_create(&agent_thread, NULL, agent_loop, NULL);
    pthread_detach(agent_thread);
}

void llm_agent_stop(void) {
    agent_running_flag = false;
}

bool llm_agent_running(void) {
    return agent_running_flag;
}

void llm_agent_submit_reply(const char *text) {
    pthread_mutex_lock(&reply_mtx);
    strncpy(reply_buf, text, sizeof(reply_buf) - 1);
    reply_ready = true;
    waiting_reply = false;
    pthread_mutex_unlock(&reply_mtx);
}

bool llm_agent_waiting_for_reply(void) {
    return waiting_reply;
}

float llm_agent_reply_seconds_remaining(void) {
    if (!waiting_reply) return -1.0f;
    pthread_mutex_lock(&reply_mtx);
    double rem = reply_deadline - GetTime();
    pthread_mutex_unlock(&reply_mtx);
    return rem > 0 ? (float)rem : 0.0f;
}
