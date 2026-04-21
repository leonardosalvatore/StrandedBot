#include "raylib.h"
#include "game_logic.h"
#include "rendering.h"
#include "particles.h"
#include "ui_theme.h"
#include "start_menu.h"
#include "llm_agent.h"
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
static void draw_hud_text_panel(int x, int y, int w, int h,
                                const char *title, const char *body,
                                bool word_wrap) {
    Rectangle frame = {(float)x, (float)y, (float)w, (float)h};
    ui_theme_draw_frame(frame, title);

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
    draw_hud_text_panel(x, y, w, h, "SYSLOG", buf, /*wrap=*/false);
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

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(void) {
    srand((unsigned)time(NULL));
    msg_log_init();

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(WIN_WIDTH, WIN_HEIGHT, "Bots");
    SetTargetFPS(60);

    rendering_init();
    particles_init();
    ui_theme_apply();
    start_menu_init();

    bool game_started = false;
    bool llm_enabled = true;

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();

        /* Toggle fullscreen */
        if (IsKeyPressed(KEY_F11)) ToggleFullscreen();

        BeginDrawing();

        if (!game_started) {
            ClearBackground(DARKGRAY);
            /* Draw the start menu */
            if (!start_menu_update()) {
                StartMenuResult r = start_menu_result();
                if (r.started) {
                    gl_bot_energy = r.energy;
                    gl_bot_inventory_rocks = r.inventory_rocks;
                    gl_apply_solar_flare_interval(r.hours_solar_flare_every);
                    gl_apply_initial_town_size(r.initial_town_size);

                    gl_initialize_world(llm_enabled, r.rocks_amount);
                    game_started = true;

                    if (llm_enabled) {
                        char prompt[4096];
                        if (r.use_custom_prompt && r.custom_prompt[0]) {
                            strncpy(prompt, r.custom_prompt, sizeof(prompt) - 1);
                        } else if (r.scenario == SCENARIO_BUILDER) {
                            snprintf(prompt, sizeof(prompt),
                                "MISSION: expand habitats to build a big town. "
                                "Your unit of progress is the number of "
                                "habitat tiles placed. Each turn do ONE of: "
                                "(a) Create a habitat/battery/solar_panel if "
                                "SYSTEM STATUS shows current_tile_buildable=true "
                                "and you have the rocks, (b) MoveTo an adjacent "
                                "buildable tile to extend the cluster, or "
                                "(c) Dig rocks when inventory is too low to "
                                "build your next piece. Place structures in "
                                "orthogonally-connected clusters (habitat next "
                                "to battery next to solar_panel) so the network "
                                "charges. Any gravel, sand, or rocks tile is a "
                                "valid build spot; don't hunt for a special "
                                "one. Keep pushing into fresh tiles; do not "
                                "revisit tiles you already built on.");
                        } else {
                            snprintf(prompt, sizeof(prompt),
                                "MISSION: explore the map. Your unit of "
                                "progress is distance covered from your "
                                "starting position. Each turn either MoveTo a "
                                "far target (use the full MoveTo range when "
                                "possible, not 1-tile steps), or LookFar when "
                                "you have no target left from the previous "
                                "scan. Only build a small recharge base "
                                "(habitat + solar + battery, orthogonally "
                                "adjacent) when energy drops below 400. When "
                                "you do build, any tile where SYSTEM STATUS "
                                "shows current_tile_buildable=true works — "
                                "gravel, sand, and rocks all count. Do not "
                                "waste hours hunting for a 'better' tile.");
                        }
                        llm_agent_start(prompt, r.interactive_mode);
                    }

                    msg_log("Game started.");
                }
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
            int bot_h = 180;
            int side_w = 310;
            draw_stats_panel(pad, pad, side_w, top_h);
            draw_log_panel(pad, sh - bot_h - pad, sw - 2 * pad, bot_h);

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
    rendering_unload();
    ui_theme_unload();
    CloseWindow();
    return 0;
}
