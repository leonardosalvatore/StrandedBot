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
    /* SYSLOG sits over the map, so use a noticeably more transparent fill
     * than UI_BG (alpha 230 → ~110) to reveal the terrain underneath. */
    Color syslog_bg = (Color){ UI_BG.r, UI_BG.g, UI_BG.b, 110 };
    draw_hud_text_panel_ex(x, y, w, h, "SYSLOG", buf, /*wrap=*/false,
                           syslog_bg);
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

/* ── CLI autostart helpers ───────────────────────────────────────────────── */
typedef enum {
    AUTOSTART_NONE = 0,
    AUTOSTART_DEFAULT,
    AUTOSTART_CUSTOM,
} AutostartMode;

static void synthesise_result_from_cfg(const BotsConfig *cfg,
                                       StartMenuResult *out) {
    const ScenarioConfig *s = cfg->default_scenario == 1 ? &cfg->builder
                                                         : &cfg->explorer;
    memset(out, 0, sizeof(*out));
    out->started  = true;
    out->scenario = cfg->default_scenario == 1 ? SCENARIO_BUILDER
                                               : SCENARIO_EXPLORER;
    out->rocks_amount            = s->rocks_amount > 0 ? s->rocks_amount : 1;
    out->initial_town_size       = s->initial_town_size;
    out->energy                  = s->energy > 0 ? s->energy : 1;
    out->inventory_rocks         = s->inventory_rocks;
    out->hours_solar_flare_every = s->hours_solar_flare_every > 0
                                       ? s->hours_solar_flare_every : 1;
    out->interactive_mode        = cfg->interactive_mode;

    strncpy(out->llama_start_script, cfg->llama.start_script,
            sizeof(out->llama_start_script) - 1);
    out->llama_auto_start = cfg->llama.auto_start;
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    AutostartMode autostart = AUTOSTART_NONE;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--autostart-default") == 0) {
            autostart = AUTOSTART_DEFAULT;
        } else if (strcmp(argv[i], "--autostart-custom") == 0) {
            autostart = AUTOSTART_CUSTOM;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--autostart-default|--autostart-custom]\n"
                   "  --autostart-default  skip the start menu using "
                   "bots-defaults.json\n"
                   "  --autostart-custom   skip the start menu using "
                   "bots-custom.json (falls back to defaults)\n",
                   argv[0]);
            return 0;
        }
    }

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

    while (!WindowShouldClose()) {
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
                    char prompt[4096];
                    if (r.use_custom_prompt && r.custom_prompt[0]) {
                        strncpy(prompt, r.custom_prompt, sizeof(prompt) - 1);
                        prompt[sizeof(prompt) - 1] = '\0';
                    } else {
                        const ScenarioConfig *sc =
                            r.scenario == SCENARIO_BUILDER ? &g_cfg.builder
                                                           : &g_cfg.explorer;
                        strncpy(prompt, sc->mission_prompt,
                                sizeof(prompt) - 1);
                        prompt[sizeof(prompt) - 1] = '\0';
                    }
                    llm_agent_start(prompt, r.interactive_mode);
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
            draw_log_panel(pad, sh - bot_h - pad, log_w, bot_h);

            /* Interactive reply dialog */
            if (llm_agent_waiting_for_reply()) {
                show_reply_dialog = true;
            }
            if (show_reply_dialog && llm_agent_waiting_for_reply()) {
                draw_reply_dialog();
            } else {
                show_reply_dialog = false;
            }
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
