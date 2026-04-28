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

/* #region agent log (session a21715) */
static void dbg_log(const char *hyp, const char *loc, const char *msg,
                    const char *data_json) {
    FILE *f = fopen("/mnt/Data/Dev/robots/bots/.cursor/debug-a21715.log", "a");
    if (!f) return;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    long long tms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    fprintf(f,
        "{\"sessionId\":\"a21715\",\"runId\":\"broadwatch\","
        "\"hypothesisId\":\"%s\",\"location\":\"%s\",\"message\":\"%s\","
        "\"data\":%s,\"timestamp\":%lld}\n",
        hyp, loc, msg, data_json ? data_json : "{}", tms);
    fclose(f);
}
static void dbg_json_escape(const char *in, char *out, size_t out_cap) {
    size_t j = 0;
    for (size_t i = 0; in && in[i] && j + 2 < out_cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\\' || c == '"') { out[j++] = '\\'; out[j++] = (char)c; }
        else if (c == '\n')         { out[j++] = '\\'; out[j++] = 'n'; }
        else if (c < 0x20)          { /* skip control chars */ }
        else                        { out[j++] = (char)c; }
    }
    out[j] = '\0';
}
/* #endregion */

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
static volatile bool agent_running_flag = false;
static pthread_t agent_thread;

/* Interactive reply */
static pthread_mutex_t reply_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  reply_cond = PTHREAD_COND_INITIALIZER;
static volatile bool   waiting_reply = false;
static char            reply_buf[4096] = "";
static volatile bool   reply_ready = false;
static double          reply_deadline = 0.0;

/* Persistent compose-box queue. Messages typed by the user between turns
 * are concatenated here; the agent drains this buffer at the start of
 * each turn and appends it as a "user" message to the LLM conversation. */
static pthread_mutex_t queued_user_mtx = PTHREAD_MUTEX_INITIALIZER;
static char            queued_user_buf[4096] = "";

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
     * works before main() sets the active config. All descriptions now
     * speak compass directions and distance buckets; no (x,y). */
    const BotsConfig *cfg = config_active();
    const char *t_move =
        (cfg && cfg->prompts.tool_move_to[0])
            ? cfg->prompts.tool_move_to
            : "Walk the bot in a compass direction. Supply 'direction' plus "
              "EXACTLY ONE of 'distance' (close|medium|far) or 'target' "
              "(a tile type the last LookFar reported in that direction). "
              "Up to %d tiles per call; costs 1 energy per tile.";
    const char *t_look_far =
        (cfg && cfg->prompts.tool_look_far[0])
            ? cfg->prompts.tool_look_far
            : "Wide scan radius %d tiles. Returns nearest feature per tile "
              "type, each tagged with direction (north, north-east, ...) and "
              "distance bucket (close|medium|far). Rock fields block "
              "line-of-sight. Costs 1 energy.";
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
            : "Returns every structure the bot placed via Create, each with "
              "direction and distance relative to the bot now. Costs 1 energy.";
    const char *t_wait =
        (cfg && cfg->prompts.tool_wait[0])
            ? cfg->prompts.tool_wait
            : "Stay on the current tile for one hour. No movement. Costs 1 "
              "energy. On a POWERED habitat (habitat orthogonally adjacent "
              "to both a battery and a solar_panel) the hour starts with +25 "
              "energy, so Wait nets +24/hour of recharge. Use Wait to "
              "recharge up to a target energy instead of MoveTo(adjacent), "
              "which leaves the habitat and wastes tiles.";

    char move_desc[768];
    snprintf(move_desc, sizeof(move_desc), t_move, MOVE_MAX_TILES);
    /* Commas inside array initialisers are treated as macro-argument
     * separators, so we build MoveTo's parameters without ADD_TOOL and
     * then push it onto the tools array manually. */
    {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "function");
        cJSON *fn = cJSON_CreateObject();
        cJSON_AddStringToObject(fn, "name", "MoveTo");
        cJSON_AddStringToObject(fn, "description", move_desc);
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "type", "object");
        cJSON *props = cJSON_CreateObject();
        cJSON *req = cJSON_CreateArray();

        cJSON *pd = cJSON_CreateObject();
        cJSON_AddStringToObject(pd, "type", "string");
        cJSON_AddStringToObject(pd, "description",
            "Compass direction: north, north-east, east, south-east, south, "
            "south-west, west, or north-west.");
        cJSON *pd_enum = cJSON_CreateArray();
        cJSON_AddItemToArray(pd_enum, cJSON_CreateString("north"));
        cJSON_AddItemToArray(pd_enum, cJSON_CreateString("north-east"));
        cJSON_AddItemToArray(pd_enum, cJSON_CreateString("east"));
        cJSON_AddItemToArray(pd_enum, cJSON_CreateString("south-east"));
        cJSON_AddItemToArray(pd_enum, cJSON_CreateString("south"));
        cJSON_AddItemToArray(pd_enum, cJSON_CreateString("south-west"));
        cJSON_AddItemToArray(pd_enum, cJSON_CreateString("west"));
        cJSON_AddItemToArray(pd_enum, cJSON_CreateString("north-west"));
        cJSON_AddItemToObject(pd, "enum", pd_enum);
        cJSON_AddItemToObject(props, "direction", pd);

        cJSON *pdist = cJSON_CreateObject();
        cJSON_AddStringToObject(pdist, "type", "string");
        cJSON_AddStringToObject(pdist, "description",
            "How far to walk: adjacent=1 tile (use for cluster extension), "
            "close~=3, medium~=8, far~=20. Exactly one of 'distance' or "
            "'target' must be present.");
        cJSON *pdist_enum = cJSON_CreateArray();
        cJSON_AddItemToArray(pdist_enum, cJSON_CreateString("adjacent"));
        cJSON_AddItemToArray(pdist_enum, cJSON_CreateString("close"));
        cJSON_AddItemToArray(pdist_enum, cJSON_CreateString("medium"));
        cJSON_AddItemToArray(pdist_enum, cJSON_CreateString("far"));
        cJSON_AddItemToObject(pdist, "enum", pdist_enum);
        cJSON_AddItemToObject(props, "distance", pdist);

        cJSON *ptgt = cJSON_CreateObject();
        cJSON_AddStringToObject(ptgt, "type", "string");
        cJSON_AddStringToObject(ptgt, "description",
            "Tile type to approach; must match a feature the last LookFar "
            "reported in the chosen direction. Exactly one of 'distance' "
            "or 'target' must be present.");
        cJSON *ptgt_enum = cJSON_CreateArray();
        cJSON_AddItemToArray(ptgt_enum, cJSON_CreateString("rocks"));
        cJSON_AddItemToArray(ptgt_enum, cJSON_CreateString("water"));
        cJSON_AddItemToArray(ptgt_enum, cJSON_CreateString("sand"));
        cJSON_AddItemToArray(ptgt_enum, cJSON_CreateString("gravel"));
        cJSON_AddItemToArray(ptgt_enum, cJSON_CreateString("habitat"));
        cJSON_AddItemToArray(ptgt_enum, cJSON_CreateString("battery"));
        cJSON_AddItemToArray(ptgt_enum, cJSON_CreateString("solar_panel"));
        cJSON_AddItemToObject(ptgt, "enum", ptgt_enum);
        cJSON_AddItemToObject(props, "target", ptgt);

        cJSON_AddItemToArray(req, cJSON_CreateString("direction"));

        cJSON_AddItemToObject(params, "properties", props);
        cJSON_AddItemToObject(params, "required", req);
        cJSON_AddItemToObject(fn, "parameters", params);
        cJSON_AddItemToObject(t, "function", fn);
        cJSON_AddItemToArray(tools, t);
    }

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

    ADD_TOOL("Wait", t_wait, { /* no params */ });

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
    /* #region agent log (session a21715) */
    {
        int ok = resp ? 1 : 0;
        int has_err = (resp && strstr(resp, "\"error\"")) ? 1 : 0;
        int has_loading = (resp && strstr(resp, "Loading model")) ? 1 : 0;
        int has_503 = (resp && strstr(resp, "\"code\":503")) ? 1 : 0;
        size_t rlen = resp ? strlen(resp) : 0;
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "{\"hour\":%d,\"ok\":%d,\"has_error\":%d,"
                 "\"has_loading_model\":%d,\"has_503\":%d,\"bytes\":%zu}",
                 gl_bot_hour_count, ok, has_err,
                 has_loading, has_503, rlen);
        dbg_log("H2", "llm_agent.c:llm_chat", "chat_response", buf);
    }
    /* #endregion */
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
        gl_bot_state = BOT_MOVING;
        /* New direction-based schema. Legacy (target_x, target_y) payloads
         * from models that ignore the enum are rejected explicitly so the
         * LLM gets a pointed error rather than a silent no-op. */
        cJSON *jdir = cJSON_GetObjectItem(args, "direction");
        cJSON *jdist = cJSON_GetObjectItem(args, "distance");
        cJSON *jtgt = cJSON_GetObjectItem(args, "target");
        const char *dir_s = (jdir && cJSON_IsString(jdir)) ? jdir->valuestring : NULL;
        const char *dist_s = (jdist && cJSON_IsString(jdist)) ? jdist->valuestring : NULL;
        const char *tgt_s = (jtgt && cJSON_IsString(jtgt)) ? jtgt->valuestring : NULL;

        ToolResult r = {0};
        if (!dir_s || !dir_s[0]) {
            snprintf(r.json, TOOL_RESULT_MAX_LEN,
                "{\"ok\":false,\"error\":\"MoveTo requires 'direction' "
                "(north, north-east, east, south-east, south, south-west, "
                "west, north-west).\"}");
            return r;
        }
        bool has_dist = (dist_s && dist_s[0]);
        bool has_tgt  = (tgt_s  && tgt_s[0]);
        if (has_dist && has_tgt) {
            snprintf(r.json, TOOL_RESULT_MAX_LEN,
                "{\"ok\":false,\"error\":\"MoveTo needs exactly one of "
                "'distance' or 'target', not both.\"}");
            return r;
        }
        if (!has_dist && !has_tgt) {
            snprintf(r.json, TOOL_RESULT_MAX_LEN,
                "{\"ok\":false,\"error\":\"MoveTo needs 'distance' "
                "(adjacent|close|medium|far) or 'target' (a tile type "
                "seen in the last LookFar).\"}");
            return r;
        }
        Direction dir = direction_from_name(dir_s);
        if (dir == DIR_NONE) {
            snprintf(r.json, TOOL_RESULT_MAX_LEN,
                "{\"ok\":false,\"error\":\"Unknown direction '%s'.\"}", dir_s);
            return r;
        }
        if (has_dist) {
            DistBucket db = dist_bucket_from_name(dist_s);
            if (db == DIST_NONE) {
                snprintf(r.json, TOOL_RESULT_MAX_LEN,
                    "{\"ok\":false,\"error\":\"Unknown distance '%s' (use "
                    "adjacent, close, medium, or far).\"}", dist_s);
                return r;
            }
            return gl_move_direction_bucket(dir, db);
        }
        /* has_tgt */
        TileType tt = tile_type_from_name(tgt_s);
        /* tile_type_from_name falls back to TILE_GRAVEL on unknown input;
         * detect that explicitly so bogus targets don't silently march to
         * the nearest gravel. */
        if (strcmp(tgt_s, tile_type_name(tt)) != 0) {
            snprintf(r.json, TOOL_RESULT_MAX_LEN,
                "{\"ok\":false,\"error\":\"Unknown target tile type '%s'.\"}",
                tgt_s);
            return r;
        }
        return gl_move_direction_target(dir, tt);
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
    if (strcmp(name, "Wait") == 0) {
        /* gl_wait owns its own animation state: BOT_CHARGING with a long
         * timer while on a powered habitat, BOT_WAITING otherwise. */
        return gl_wait();
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
    "You act by calling ONE tool per turn via native tool_calls. The bot "
    "never sees raw coordinates; it reasons in compass directions "
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
    "- Each turn you get a 'Neighbours:' line listing the 8 tiles touching\n"
    "  the bot, labelled N, NE, E, SE, S, SW, W, NW, plus 'Current='.\n"
    "  Use it to pick an orthogonally-adjacent Create target or a\n"
    "  one-step MoveTo without burning a LookFar.\n"
    "- MoveTo needs a 'direction' plus EXACTLY ONE of:\n"
    "    * 'distance' in {adjacent, close, medium, far} — walks that bucket\n"
    "      in that direction (adjacent=1 tile, close≈3, medium≈8, far≈20),\n"
    "      or\n"
    "    * 'target' — a tile type the most recent LookFar reported in that\n"
    "      same direction. If the cache is empty or the feature is in a\n"
    "      different direction, call LookFar first.\n"
    "- LookFar reveals the nearest feature of every type within the scan\n"
    "  radius and tags each with direction + distance bucket. Its results\n"
    "  are kept as 'Known features' until you act in a way that changes\n"
    "  those tiles (Dig, Create) or the bot wanders out of range. Do NOT\n"
    "  call LookFar every turn — re-use the Known features list.\n"
    "- If the tile you want is already visible in the current Neighbours\n"
    "  line, use MoveTo(direction=<that label>, distance=adjacent). Do NOT\n"
    "  use 'target=...' for something in Neighbours — 'target' reads the\n"
    "  older LookFar cache and may point elsewhere or be stale.\n"
    "- Dig is only useful when you need more rocks for a PLANNED Create.\n"
    "  Rocks have no value beyond building, so don't hoard past what your\n"
    "  plan requires. Dig only when Current=rocks; the tile becomes gravel.\n"
    "- Create works on the CURRENT tile whenever SYSTEM STATUS says\n"
    "  current_tile_buildable=true (gravel, sand, or rocks). If that flag\n"
    "  is true AND you have enough rocks for the structure, Create NOW;\n"
    "  do not wander looking for a 'better' tile.\n"
    "- Power requires ONLY orthogonal adjacency of habitat + battery +\n"
    "  solar_panel tiles (|dx|+|dy|=1, i.e. N, E, S, or W neighbour). NE,\n"
    "  SE, SW, NW neighbours do NOT count. There is no wiring, no cabling,\n"
    "  no 'connect' step. When extending a cluster, step to an N/E/S/W\n"
    "  neighbour of the last placed structure (use the Neighbours line)\n"
    "  BEFORE calling Create.\n"
    "- Dig and Create change the current tile. Old Known-features entries\n"
    "  that pointed here are automatically dropped; trust the newest tool\n"
    "  results and the Neighbours line over older LookFar memory.\n"
    "- Recharge on a POWERED habitat (habitat whose N/E/S/W neighbours\n"
    "  include both a battery and a solar_panel) by calling Wait. The\n"
    "  habitat grants +25 energy per hour, Wait costs 1, so Wait nets\n"
    "  +24/hour. DO NOT MoveTo(adjacent) to 'stay' — MoveTo leaves the\n"
    "  tile. To recharge to a target E, call Wait repeatedly until\n"
    "  energy >= E.\n"
    "- If energy<200 or the user asks you to recharge to a specific\n"
    "  value, MoveTo a powered habitat and Wait until the target is\n"
    "  reached.";

static const char *active_system_prompt(void) {
    const BotsConfig *cfg = config_active();
    if (cfg && cfg->prompts.system[0]) return cfg->prompts.system;
    return SYSTEM_PROMPT_FALLBACK;
}

/* ── Wait for user reply ─────────────────────────────────────────────────── */
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
    msg_log("============================================================");

    /* Messages were already seeded by llm_agent_start (system, plus an
     * optional user mission). If somehow empty, seed just the system
     * prompt; per-turn SYSTEM STATUS user messages drive the rest. */
    if (msg_count == 0) {
        append_msg("system", active_system_prompt(), 0);
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

        /* Drain the compose-box queue: inject any user-typed messages
         * into the conversation BEFORE we build this turn's SYSTEM STATUS
         * so the bot sees the new instruction on this very hour. */
        pthread_mutex_lock(&queued_user_mtx);
        if (queued_user_buf[0]) {
            char injected[sizeof(queued_user_buf)];
            snprintf(injected, sizeof(injected), "%s", queued_user_buf);
            queued_user_buf[0] = '\0';
            pthread_mutex_unlock(&queued_user_mtx);
            msg_log("  [User] %s", injected);
            append_msg("user", injected, hour);
        } else {
            pthread_mutex_unlock(&queued_user_mtx);
        }

        /* Build status grounding */
        int gx, gy;
        gl_bot_grid_pos(&gx, &gy);
        bool charge_on_hab = false, charge_has_bat = false, charge_has_sol = false;
        gl_bot_charge_network_status(&charge_on_hab, &charge_has_bat, &charge_has_sol);
        bool charge_active = charge_on_hab && charge_has_bat && charge_has_sol;
        pthread_mutex_lock(&gl_tiles_lock);
        TileType cur_tile_type = gl_tile_matrix[gx][gy].type;
        pthread_mutex_unlock(&gl_tiles_lock);
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
        /* Compass-labelled neighbours line — replaces the old 3x3 glyph
         * grid. Mirrored verbatim to stdout + SYSLOG so operators see
         * exactly what the LLM sees. */
        char neighbours[384];
        gl_build_neighbours_line(neighbours, sizeof(neighbours));
        msg_log("  %s", neighbours);

        /* Optional "Known features" line from the last LookFar cache. When
         * no LookFar has been called yet this stays empty. */
        char known[512];
        gl_build_known_features_summary(known, sizeof(known));
        if (known[0]) msg_log("  Known features: %s", known);

        char status[2560];
        if (known[0]) {
            snprintf(status, sizeof(status),
                "SYSTEM STATUS (truth source): hour=%d, energy=%d, "
                "current_tile_buildable=%s, inventory_rocks=%d, "
                "hours_to_solar_flare=%d, %s.\n"
                "%s\n"
                "Known features (last LookFar): %s",
                gl_bot_hour_count, gl_bot_energy,
                cur_buildable ? "true" : "false",
                gl_bot_inventory_rocks,
                gl_hours_to_solar_flare,
                charge_field,
                neighbours,
                known);
        } else {
            snprintf(status, sizeof(status),
                "SYSTEM STATUS (truth source): hour=%d, energy=%d, "
                "current_tile_buildable=%s, inventory_rocks=%d, "
                "hours_to_solar_flare=%d, %s.\n"
                "%s",
                gl_bot_hour_count, gl_bot_energy,
                cur_buildable ? "true" : "false",
                gl_bot_inventory_rocks,
                gl_hours_to_solar_flare,
                charge_field,
                neighbours);
        }

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

            /* Whenever the bot asks a question, pause for a typed reply.
             * The compose box is always available; the reply dialog
             * latches onto llm_agent_waiting_for_reply() in main.c. */
            if (strchr(content, '?')) {
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
            /* #region agent log (session a21715) */
            {
                char args_esc[256];
                dbg_json_escape(args_pretty ? args_pretty : "{}",
                                args_esc, sizeof(args_esc));
                char name_esc[64];
                dbg_json_escape(fn_name, name_esc, sizeof(name_esc));
                char buf[512];
                snprintf(buf, sizeof(buf),
                         "{\"hour\":%d,\"tool\":\"%s\",\"args\":\"%s\"}",
                         hour, name_esc, args_esc);
                dbg_log("H3", "llm_agent.c:tool_call",
                        "tool_invoked", buf);
            }
            /* #endregion */
            if (args_pretty) free(args_pretty);
            ToolResult tr = dispatch_tool(fn_name, args);
            /* #region agent log (session a21715) */
            {
                char res_esc[256];
                /* Truncate tr.json to 220 chars so the log line stays short
                 * even for ListBuiltTiles / LookFar payloads. */
                char trunc[240];
                snprintf(trunc, sizeof(trunc), "%.220s", tr.json);
                dbg_json_escape(trunc, res_esc, sizeof(res_esc));
                char name_esc[64];
                dbg_json_escape(fn_name, name_esc, sizeof(name_esc));
                int gx, gy;
                gl_bot_grid_pos(&gx, &gy);
                char buf[600];
                snprintf(buf, sizeof(buf),
                         "{\"hour\":%d,\"tool\":\"%s\",\"result\":\"%s\","
                         "\"pos_x\":%d,\"pos_y\":%d,\"energy\":%d,"
                         "\"rocks\":%d}",
                         hour, name_esc, res_esc, gx, gy,
                         gl_bot_energy, gl_bot_inventory_rocks);
                /* H3 for all tools; H4 specifically for Create since that
                 * is the build outcome we want to audit. */
                dbg_log(strcmp(fn_name, "Create") == 0 ? "H4" : "H3",
                        "llm_agent.c:tool_call",
                        "tool_result", buf);
            }
            /* #endregion */
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
void llm_agent_start(const char *initial_prompt) {
    if (agent_running_flag) return;
    /* llama.cpp's server ignores the "model" field (it serves whatever model
       was loaded at startup), but the OpenAI-compatible schema requires the
       field to exist. We send a fixed placeholder. */
    strncpy(agent_model, "local", sizeof(agent_model) - 1);
    agent_running_flag = true;

    /* Reset messages and seed the conversation. The system prompt is
     * always added; the user mission is optional — an empty initial_prompt
     * just leaves the conversation seeded with the system prompt and lets
     * per-turn SYSTEM STATUS messages drive everything. */
    msg_count = 0;
    append_msg("system", active_system_prompt(), 0);
    if (initial_prompt && initial_prompt[0]) {
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

void llm_agent_queue_user_message(const char *text) {
    if (!text || !text[0]) return;
    pthread_mutex_lock(&queued_user_mtx);
    size_t used = strlen(queued_user_buf);
    size_t cap = sizeof(queued_user_buf) - 1;
    if (used > 0 && used < cap) {
        queued_user_buf[used++] = '\n';
        queued_user_buf[used] = '\0';
    }
    size_t room = cap - used;
    if (room > 0) {
        strncpy(queued_user_buf + used, text, room);
        queued_user_buf[cap] = '\0';
    }
    pthread_mutex_unlock(&queued_user_mtx);
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
