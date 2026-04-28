#include "raylib.h"
#include "game_logic.h"
#include "rendering.h"
#include "particles.h"
#include "ui_theme.h"
#include "start_menu.h"
#include "llm_agent.h"
#include "llama_launcher.h"
#include "config.h"
#include "message_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* raygui is implemented in start_menu.c; just include the header here */
#include "raygui.h"

/* ── HUD state names ─────────────────────────────────────────────────────── */
static const char *bot_state_name(BotState s) {
    switch (s) {
    case BOT_WAITING:   return "WAIT";
    case BOT_THINKING:  return "THINK";
    case BOT_MOVING:    return "MOVE";
    case BOT_LOOKCLOSE: return "SCAN.CLOSE";
    case BOT_LOOKFAR:   return "SCAN.FAR";
    case BOT_CHARGING:  return "CHARGE";
    case BOT_DESTROYED: return "OFFLINE";
    default:            return "?";
    }
}

/* ── HUD helper: framed panel with scissor-clipped body text ─────────────── */
static void draw_hud_text_panel_ex(int x, int y, int w, int h,
                                   const char *title, const char *body,
                                   bool word_wrap, Color bg) {
    Rectangle frame = {(float)x, (float)y, (float)w, (float)h};
    ui_theme_draw_frame_ex(frame, title, bg);

    /* Content rect: inset inside the double border and below the title plate. */
    Rectangle content = { (float)(x + 10), (float)(y + 22),
                          (float)(w - 20), (float)(h - 30) };

    /* Clip so labels never draw outside the panel. Raylib's scissor takes
     * integer coords in window space. */
    BeginScissorMode((int)content.x, (int)content.y,
                     (int)content.width, (int)content.height);

    int prev_wrap = GuiGetStyle(DEFAULT, TEXT_WRAP_MODE);
    int prev_valign = GuiGetStyle(DEFAULT, TEXT_ALIGNMENT_VERTICAL);
    GuiSetStyle(DEFAULT, TEXT_WRAP_MODE,
                word_wrap ? TEXT_WRAP_WORD : TEXT_WRAP_NONE);
    GuiSetStyle(DEFAULT, TEXT_ALIGNMENT_VERTICAL, TEXT_ALIGN_TOP);

    GuiLabel(content, body);

    GuiSetStyle(DEFAULT, TEXT_WRAP_MODE, prev_wrap);
    GuiSetStyle(DEFAULT, TEXT_ALIGNMENT_VERTICAL, prev_valign);

    EndScissorMode();
}

static void draw_hud_text_panel(int x, int y, int w, int h,
                                const char *title, const char *body,
                                bool word_wrap) {
    draw_hud_text_panel_ex(x, y, w, h, title, body, word_wrap, UI_BG);
}

/* ── UI panels (drawn with raygui after game starts) ─────────────────────── */
static void draw_stats_panel(int x, int y, int w, int h) {
    int gx, gy;
    gl_bot_grid_pos(&gx, &gy);
    char buf[768];
    snprintf(buf, sizeof(buf),
        "> HOUR.......: %d\n"
        "> ENERGY.....: %d\n"
        "> POS........: (%d, %d)\n"
        "> STATE......: %s\n"
        "> FLARE.ETA..: %d h\n"
        "> ROCKS......: %d",
        gl_bot_hour_count, gl_bot_energy, gx, gy,
        bot_state_name(gl_bot_state), gl_hours_to_solar_flare,
        gl_bot_inventory_rocks);
    draw_hud_text_panel(x, y, w, h, "TELEMETRY", buf, /*wrap=*/false);
}

static void draw_log_panel(int x, int y, int w, int h) {
    /* How many lines fit? TEXT_LINE_SPACING is how far the label advances. */
    int line_h = GuiGetStyle(DEFAULT, TEXT_LINE_SPACING);
    if (line_h < 1) line_h = 18;
    int max_lines = (h - 34) / line_h;
    if (max_lines < 1)  max_lines = 1;
    if (max_lines > 20) max_lines = 20;

    const char *lines[20];
    int n = msg_log_get(lines, max_lines);
    char buf[6144] = "";
    int off = 0;
    for (int i = 0; i < n; i++) {
        off += snprintf(buf + off, sizeof(buf) - off, "> %s\n", lines[i]);
        if (off >= (int)sizeof(buf) - 2) break;
    }
    /* SYSLOG sits over the map. UI_BG alpha is 230; we darken it enough
     * to stay readable over bright terrain while still showing some of
     * the map underneath. */
    Color syslog_bg = (Color){ UI_BG.r, UI_BG.g, UI_BG.b, 155 };
    draw_hud_text_panel_ex(x, y, w, h, "SYSLOG", buf, /*wrap=*/false,
                           syslog_bg);
}

/* Top-right mirror panel: shows the most recent "[Bot] ..." line that was
 * pushed into the SYSLOG ring. Single line, clipped to the panel width.
 * Walks the full ring once per frame; cheap (<=1000 pointers). */
static void draw_last_bot_panel(int x, int y, int w, int h) {
    static const char *ring_view[MSG_LOG_MAX_LINES];
    int n = msg_log_get(ring_view, MSG_LOG_MAX_LINES);
    const char *latest = NULL;
    for (int i = n - 1; i >= 0; i--) {
        const char *s = ring_view[i];
        if (!s) continue;
        /* The bot's own lines are logged as "  [Bot] <content>". Accept
         * both with and without the leading indent, for resilience. */
        const char *hit = strstr(s, "[Bot] ");
        if (hit) { latest = hit + strlen("[Bot] "); break; }
    }
    char buf[512];
    if (latest && latest[0]) {
        snprintf(buf, sizeof(buf), "> %s", latest);
    } else {
        snprintf(buf, sizeof(buf), "> (waiting for bot...)");
    }
    draw_hud_text_panel(x, y, w, h, "LAST BOT", buf, /*wrap=*/false);
}

/* ── Reply dialog ────────────────────────────────────────────────────────── */
static bool show_reply_dialog = false;
static char reply_text[4096] = "";
static bool reply_edit = false;

static void draw_reply_dialog(void) {
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    int dw = 450, dh = 160;
    int dx = sw - dw - 10, dy = sh - dh - 10;
    Rectangle r = {(float)dx, (float)dy, (float)dw, (float)dh};
    ui_theme_draw_frame(r, "INCOMING QUERY");

    float rem = llm_agent_reply_seconds_remaining();
    char title[160];
    snprintf(title, sizeof(title), "> bot is awaiting input  (T-%.0fs)", rem);
    GuiLabel((Rectangle){dx + 12, dy + 22, dw - 24, 20}, title);

    if (GuiTextBox((Rectangle){dx + 12, dy + 48, dw - 24, 65}, reply_text,
                   sizeof(reply_text), reply_edit))
        reply_edit = !reply_edit;

    if (GuiButton((Rectangle){dx + dw - 140, dy + dh - 35, 130, 28},
                  ">> TRANSMIT <<")) {
        if (reply_text[0]) {
            llm_agent_submit_reply(reply_text);
            reply_text[0] = '\0';
            show_reply_dialog = false;
        }
    }
}

/* ── Persistent compose box ─────────────────────────────────────────────── */
/* Always-visible input panel that lets the operator queue user-role
 * messages into the LLM conversation. The agent drains the queue at the
 * start of its next turn (see llm_agent_queue_user_message). Docked in
 * the bottom-right corner, above the occasional reply dialog. */
static char compose_text[4096] = "";
static bool compose_edit = false;

static void draw_compose_box(int dy_top) {
    int sw = GetScreenWidth();
    int dw = 450, dh = 140;
    int dx = sw - dw - 10;
    int dy = dy_top;
    Rectangle r = {(float)dx, (float)dy, (float)dw, (float)dh};
    ui_theme_draw_frame(r, "COMPOSE");

    GuiLabel((Rectangle){dx + 12, dy + 22, dw - 24, 20},
             "> queue a message for the bot");

    if (GuiTextBox((Rectangle){dx + 12, dy + 46, dw - 24, 50}, compose_text,
                   sizeof(compose_text), compose_edit))
        compose_edit = !compose_edit;

    if (GuiButton((Rectangle){dx + dw - 140, dy + dh - 35, 130, 28},
                  ">> SEND <<")) {
        if (compose_text[0]) {
            llm_agent_queue_user_message(compose_text);
            compose_text[0] = '\0';
            compose_edit = false;
        }
    }
}

/* ── Post-mortem overlay ────────────────────────────────────────────────── */
/* Shown once the bot is destroyed (solar flare → BOT_DESTROYED) or the
 * run ends on energy==0. Draws a dimmed full-screen veil plus a centred
 * "MISSION REPORT" panel listing survival time, exploration coverage,
 * structure counts, and powered clusters. The user can dismiss the panel
 * to inspect the frozen map, or close the window to exit. */
static bool postmortem_shown_once = false;
static bool show_postmortem = false;
static bool quit_requested = false;
static PostmortemStats postmortem_stats = {0};

static bool run_is_over(void) {
    return !llm_agent_running() &&
           (gl_bot_state == BOT_DESTROYED || gl_bot_energy <= 0);
}

static void draw_postmortem_overlay(void) {
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    /* Dim veil so the frozen map stays visible behind the panel. */
    DrawRectangle(0, 0, sw, sh, (Color){0, 0, 0, 170});

    int pw = 560, ph = 645;
    int px = (sw - pw) / 2, py = (sh - ph) / 2;
    Rectangle r = { (float)px, (float)py, (float)pw, (float)ph };
    const char *title = postmortem_stats.destroyed_by_flare
                            ? "MISSION REPORT — DESTROYED BY SOLAR FLARE"
                            : "MISSION REPORT — OUT OF ENERGY";
    ui_theme_draw_frame(r, title);

    const PostmortemStats *s = &postmortem_stats;
    int explore_w = s->explore_max_gx - s->explore_min_gx + 1;
    int explore_h = s->explore_max_gy - s->explore_min_gy + 1;
    float coverage = s->tiles_total > 0
                         ? 100.0f * (float)s->tiles_discovered /
                               (float)s->tiles_total
                         : 0.0f;

    char buf[2048];
    snprintf(buf, sizeof(buf),
        "// TIMELINE\n"
        "  hours survived     : %d\n"
        "  spawn cell         : (%d, %d)\n"
        "  final cell         : (%d, %d)\n"
        "  distance travelled : %d tiles (Chebyshev)\n"
        "  final energy       : %d\n"
        "  final rocks        : %d\n"
        "\n"
        "// EXPLORATION\n"
        "  tiles discovered   : %d / %d  (%.2f%% of map)\n"
        "  bounding box       : %d x %d   (x %d..%d, y %d..%d)\n"
        "\n"
        "// STRUCTURES BUILT\n"
        "  habitats           : %d\n"
        "  batteries          : %d\n"
        "  solar panels       : %d\n"
        "  total placed       : %d\n"
        "  powered clusters   : %d   (habitat + battery + solar_panel,\n"
        "                             orthogonally connected)\n",
        s->hours_survived,
        s->spawn_gx, s->spawn_gy,
        s->final_gx, s->final_gy,
        s->travel_chebyshev,
        s->final_energy,
        s->final_rocks,
        s->tiles_discovered, s->tiles_total, coverage,
        explore_w, explore_h,
        s->explore_min_gx, s->explore_max_gx,
        s->explore_min_gy, s->explore_max_gy,
        s->built_habitats,
        s->built_batteries,
        s->built_solar_panels,
        s->built_total,
        s->powered_clusters);

    Rectangle body = { (float)(px + 20), (float)(py + 36),
                       (float)(pw - 40), (float)(ph - 96) };
    BeginScissorMode((int)body.x, (int)body.y,
                     (int)body.width, (int)body.height);
    int prev_wrap = GuiGetStyle(DEFAULT, TEXT_WRAP_MODE);
    int prev_align = GuiGetStyle(DEFAULT, TEXT_ALIGNMENT);
    GuiSetStyle(DEFAULT, TEXT_WRAP_MODE, TEXT_WRAP_NONE);
    GuiSetStyle(DEFAULT, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
    GuiLabel(body, buf);
    GuiSetStyle(DEFAULT, TEXT_WRAP_MODE, prev_wrap);
    GuiSetStyle(DEFAULT, TEXT_ALIGNMENT, prev_align);
    EndScissorMode();

    if (GuiButton((Rectangle){(float)(px + 20),
                              (float)(py + ph - 46), 180, 32},
                  "VIEW MAP")) {
        show_postmortem = false;
    }
    if (GuiButton((Rectangle){(float)(px + pw - 140),
                              (float)(py + ph - 46), 120, 32},
                  "EXIT")) {
        quit_requested = true;
    }
}

/* ── CLI autostart helpers ───────────────────────────────────────────────── */
typedef enum {
    AUTOSTART_NONE = 0,
    AUTOSTART_DEFAULT,
    AUTOSTART_CUSTOM,
} AutostartMode;

static void synthesise_result_from_cfg(const BotsConfig *cfg,
                                       StartMenuResult *out) {
    const GamePreset *g = &cfg->game;
    memset(out, 0, sizeof(*out));
    out->started                 = true;
    out->rocks_amount            = g->rocks_amount > 0 ? g->rocks_amount : 1;
    out->initial_town_size       = g->initial_town_size;
    out->energy                  = g->energy > 0 ? g->energy : 1;
    out->inventory_rocks         = g->inventory_rocks;
    out->hours_solar_flare_every = g->hours_solar_flare_every > 0
                                       ? g->hours_solar_flare_every : 1;

    strncpy(out->llama_start_script, cfg->llama.start_script,
            sizeof(out->llama_start_script) - 1);
    out->llama_auto_start = cfg->llama.auto_start;
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    AutostartMode autostart = AUTOSTART_NONE;
    bool enable_llama_log = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--autostart-default") == 0) {
            autostart = AUTOSTART_DEFAULT;
        } else if (strcmp(argv[i], "--autostart-custom") == 0) {
            autostart = AUTOSTART_CUSTOM;
        } else if (strcmp(argv[i], "--enable-llama-log") == 0) {
            enable_llama_log = true;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--autostart-default|--autostart-custom] "
                   "[--enable-llama-log]\n"
                   "  --autostart-default  skip the start menu using "
                   "bots-defaults.json\n"
                   "  --autostart-custom   skip the start menu using "
                   "bots-custom.json (falls back to defaults)\n"
                   "  --enable-llama-log   pipe the auto-spawned "
                   "llama-server's stdout/stderr to this terminal "
                   "(muted by default)\n",
                   argv[0]);
            return 0;
        }
    }
    llama_launcher_set_log_enabled(enable_llama_log);

    srand((unsigned)time(NULL));
    msg_log_init();

    /* Config is loaded up front so the start menu, the agent, and the
     * launcher share one in-memory snapshot. */
    static BotsConfig g_cfg;
    config_load_custom(&g_cfg);
    config_set_active(&g_cfg);

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(WIN_WIDTH, WIN_HEIGHT, "Bots");
    SetTargetFPS(60);

    rendering_init();
    particles_init();
    ui_theme_apply();
    start_menu_init();

    bool game_started = false;
    bool llm_enabled  = true;

    /* Resolve the autostart path (if any) now that the window/theme are up.
     * Menu stays skipped by flipping game_started ourselves. */
    StartMenuResult autostart_result = {0};
    if (autostart != AUTOSTART_NONE) {
        BotsConfig cfg_auto;
        if (autostart == AUTOSTART_DEFAULT) config_load_defaults(&cfg_auto);
        else                                 config_load_custom(&cfg_auto);
        /* Keep the active config pointing at the autostart snapshot so
         * prompt lookups match the flag the user asked for. */
        g_cfg = cfg_auto;
        synthesise_result_from_cfg(&g_cfg, &autostart_result);
        msg_log("Autostart: %s",
                autostart == AUTOSTART_DEFAULT ? "defaults" : "custom");
    }

    while (!WindowShouldClose() && !quit_requested) {
        float dt = GetFrameTime();

        /* Toggle fullscreen */
        if (IsKeyPressed(KEY_F11)) ToggleFullscreen();

        BeginDrawing();

        if (!game_started) {
            ClearBackground(DARKGRAY);

            StartMenuResult r = {0};
            bool ready = false;
            if (autostart != AUTOSTART_NONE) {
                r = autostart_result;
                ready = true;
            } else if (!start_menu_update()) {
                r = start_menu_result();
                ready = true;
            }

            if (ready && r.started) {
                gl_bot_energy          = r.energy;
                gl_bot_inventory_rocks = r.inventory_rocks;
                gl_apply_solar_flare_interval(r.hours_solar_flare_every);
                gl_apply_initial_town_size(r.initial_town_size);

                gl_initialize_world(llm_enabled, r.rocks_amount);
                game_started = true;

                /* Spawn llama-server in the background if requested AND the
                 * port is not already open. Inherits our stdout/stderr. */
                if (llm_enabled && r.llama_auto_start) {
                    llama_launcher_set_port(g_cfg.llama.port);
                    if (llama_launcher_start(r.llama_start_script)) {
                        /* 120 s covers port-bind (~5-10 s) + model load
                         * (~20-60 s on ROCm, more on cold cache). The
                         * launcher returns as soon as /health is 200. */
                        llama_launcher_wait_ready(120);
                    }
                }

                if (llm_enabled) {
                    /* Empty custom prompt is fine: the agent then starts
                     * with only the system prompt seeded and lets the
                     * per-turn SYSTEM STATUS messages drive everything. */
                    const char *prompt = (r.use_custom_prompt &&
                                          r.custom_prompt[0])
                                             ? r.custom_prompt
                                             : "";
                    llm_agent_start(prompt);
                }

                msg_log("Game started.");
            }
        } else {
            /* Game running */
            gl_update(dt);
            particles_update(dt);
            draw_game();

            /* UI panels on top of the game */
            int sw = GetScreenWidth(), sh = GetScreenHeight();
            int pad = 8;
            int top_h = 160;
            int bot_h = 360;                        /* 2x the previous 180 */
            int full_w = sw - 2 * pad;
            int log_w = (int)(full_w * 0.70f);      /* 30% narrower */
            int side_w = 310;
            draw_stats_panel(pad, pad, side_w, top_h);
            /* Top-right: one-line mirror of the latest [Bot] message.
             * 3x the TELEMETRY width, much shorter in height. Clamped
             * so it never overlaps the TELEMETRY panel. */
            int bot_line_w = side_w * 3;
            int bot_line_h = 56;
            int max_bot_line_w = sw - (pad + side_w + 12) - pad;
            if (bot_line_w > max_bot_line_w) bot_line_w = max_bot_line_w;
            if (bot_line_w > 200) {
                int bot_line_x = sw - pad - bot_line_w;
                draw_last_bot_panel(bot_line_x, pad, bot_line_w, bot_line_h);
            }
            draw_log_panel(pad, sh - bot_h - pad, log_w, bot_h);

            /* Bottom-right stack: compose box (always) + reply dialog
             * (only while the bot is awaiting an answer to a direct
             * question). Compose sits on top so it never overlaps the
             * reply dialog. */
            int compose_h = 140;
            int reply_h  = 160;
            int stack_bottom = sh - 10;
            if (llm_agent_waiting_for_reply()) {
                show_reply_dialog = true;
            } else {
                show_reply_dialog = false;
            }
            int reply_y = stack_bottom - reply_h;
            int compose_y = show_reply_dialog
                                ? reply_y - compose_h - 6
                                : stack_bottom - compose_h;
            draw_compose_box(compose_y);
            if (show_reply_dialog) {
                draw_reply_dialog();
            }

            /* Latch the post-mortem the first time the run terminates.
             * We snapshot stats once (so the grid can continue rendering
             * particles without changing the numbers) and re-show the
             * modal on demand via a keypress. */
            if (run_is_over() && !postmortem_shown_once) {
                gl_compute_postmortem(&postmortem_stats);
                postmortem_shown_once = true;
                show_postmortem = true;
            }
            if (postmortem_shown_once && !show_postmortem &&
                IsKeyPressed(KEY_R)) {
                show_postmortem = true;
            }
            if (show_postmortem) draw_postmortem_overlay();
        }

        EndDrawing();
    }

    llm_agent_stop();
    llama_launcher_stop();
    rendering_unload();
    ui_theme_unload();
    CloseWindow();
    return 0;
}
