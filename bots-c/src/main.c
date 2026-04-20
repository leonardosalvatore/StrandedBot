#include "raylib.h"
#include "game_logic.h"
#include "rendering.h"
#include "start_menu.h"
#include "llm_agent.h"
#include "message_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* raygui is implemented in start_menu.c; just include the header here */
#include "raygui.h"

/* ── UI panels (drawn with raygui after game starts) ─────────────────────── */
static void draw_stats_panel(int x, int y, int w, int h, const char *model) {
    GuiPanel((Rectangle){x, y, w, h}, "Bot Stats");
    int gx, gy;
    gl_bot_grid_pos(&gx, &gy);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "Hour: %d\n"
        "Energy: %d\n"
        "Position: (%d, %d)\n"
        "State: %d\n"
        "Solar Flare in: %d\n"
        "Rocks: %d\n"
        "Model: %s",
        gl_bot_hour_count, gl_bot_energy, gx, gy,
        (int)gl_bot_state, gl_hours_to_solar_flare,
        gl_bot_inventory_rocks, model);
    GuiLabel((Rectangle){x + 10, y + 30, w - 20, h - 40}, buf);
}

static void draw_speech_panel(int x, int y, int w, int h) {
    GuiPanel((Rectangle){x, y, w, h}, "Bot Speech");
    if (gl_bot_last_speech[0]) {
        /* Truncate for display */
        char display[512];
        strncpy(display, gl_bot_last_speech, sizeof(display) - 1);
        display[sizeof(display) - 1] = '\0';
        GuiLabel((Rectangle){x + 10, y + 30, w - 20, h - 40}, display);
    }
}

static void draw_log_panel(int x, int y, int w, int h) {
    GuiPanel((Rectangle){x, y, w, h}, "Message Log");
    const char *lines[30];
    int n = msg_log_get(lines, 30);
    char buf[4096] = "";
    int off = 0;
    for (int i = 0; i < n; i++) {
        off += snprintf(buf + off, sizeof(buf) - off, "%s\n", lines[i]);
        if (off >= (int)sizeof(buf) - 2) break;
    }
    GuiLabel((Rectangle){x + 10, y + 30, w - 20, h - 40}, buf);
}

/* ── Reply dialog ────────────────────────────────────────────────────────── */
static bool show_reply_dialog = false;
static char reply_text[4096] = "";
static bool reply_edit = false;

static void draw_reply_dialog(void) {
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    int dw = 600, dh = 180;
    int dx = sw - dw - 20, dy = sh - dh - 20;
    GuiPanel((Rectangle){dx, dy, dw, dh}, "Reply to Bot");

    float rem = llm_agent_reply_seconds_remaining();
    char title[128];
    snprintf(title, sizeof(title), "Bot asked a question (%.0fs remaining)", rem);
    GuiLabel((Rectangle){dx + 15, dy + 30, dw - 30, 20}, title);

    if (GuiTextBox((Rectangle){dx + 15, dy + 60, dw - 30, 60}, reply_text,
                   sizeof(reply_text), reply_edit))
        reply_edit = !reply_edit;

    if (GuiButton((Rectangle){dx + dw - 200, dy + 130, 180, 35}, "Send Reply")) {
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
    start_menu_init("ministral-3:8b");

    bool game_started = false;
    char current_model[256] = "ministral-3:8b";
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
                    strncpy(current_model, r.model, sizeof(current_model) - 1);
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
                        } else {
                            /* Build default prompt from scenario */
                            const char *head =
                                "YOUR MISSION:\n"
                                "Stay in a habitat if energy is below 200 to avoid solar flare damage,";
                            const char *power_rules =
                                "Power works only through orthogonal adjacency of habitat, battery, and solar_panel tiles. "
                                "There is no wiring or connect step.";
                            if (r.scenario == SCENARIO_BUILDER) {
                                snprintf(prompt, sizeof(prompt),
                                    "%s expand habitats to build a big town.\n%s", head, power_rules);
                            } else {
                                snprintf(prompt, sizeof(prompt),
                                    "%s explore the map. Build habitat and solar+storage network only to recharge.\n%s",
                                    head, power_rules);
                            }
                        }
                        llm_agent_start(current_model, prompt, r.interactive_mode);
                    }

                    msg_log("Game started! Model: %s", current_model);
                }
            }
        } else {
            /* Game running */
            gl_update(dt);
            draw_game();

            /* UI panels on top of the game */
            draw_stats_panel(10, 10, 300, 220, current_model);
            draw_speech_panel(GetScreenWidth() - 420, 10, 410, 200);
            draw_log_panel(10, GetScreenHeight() - 260, 500, 250);

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
    CloseWindow();
    return 0;
}
