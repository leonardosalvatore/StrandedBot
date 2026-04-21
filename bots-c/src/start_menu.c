#include "start_menu.h"
#include "game_logic.h"
#include "raylib.h"
#include "ui_theme.h"

#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* We only define RAYGUI_IMPLEMENTATION once here; the rest of the project
   uses raygui as a normal header. The compile definition in CMake is removed
   in favour of this single TU. */

static bool         menu_active = true;
static StartMenuResult result = {0};

/* Editable text buffers */
static char buf_rocks[32]   = "";
static char buf_town[32]    = "";
static char buf_energy[32]  = "";
static char buf_inv[32]     = "";
static char buf_flare[32]   = "";
static char buf_prompt[4096] = "";

static bool edit_rocks  = false;
static bool edit_town   = false;
static bool edit_energy = false;
static bool edit_inv    = false;
static bool edit_flare  = false;
static bool edit_prompt = false;

static int  scenario = 0;   /* 0=Explorer, 1=Builder */
static bool interactive = true;
static bool show_custom_prompt = false;

/* Presets per scenario index */
static void apply_preset(int sc) {
    if (sc == 0) { /* Explorer */
        snprintf(buf_rocks,  sizeof(buf_rocks),  "20");
        snprintf(buf_town,   sizeof(buf_town),   "0");
        snprintf(buf_energy, sizeof(buf_energy), "1000");
        snprintf(buf_inv,    sizeof(buf_inv),    "10");
        snprintf(buf_flare,  sizeof(buf_flare),  "100");
    } else { /* Builder */
        snprintf(buf_rocks,  sizeof(buf_rocks),  "100");
        snprintf(buf_town,   sizeof(buf_town),   "0");
        snprintf(buf_energy, sizeof(buf_energy), "1000");
        snprintf(buf_inv,    sizeof(buf_inv),    "100");
        snprintf(buf_flare,  sizeof(buf_flare),  "3000");
    }
}

void start_menu_init(void) {
    menu_active = true;
    show_custom_prompt = false;
    memset(&result, 0, sizeof(result));
    apply_preset(0);
    buf_prompt[0] = '\0';
}

static void fill_result(void) {
    result.started = true;
    result.scenario = scenario == 0 ? SCENARIO_EXPLORER : SCENARIO_BUILDER;
    result.rocks_amount = atoi(buf_rocks);
    if (result.rocks_amount < 1) result.rocks_amount = 1;
    result.initial_town_size = atoi(buf_town);
    result.energy = atoi(buf_energy);
    if (result.energy < 1) result.energy = 1;
    result.inventory_rocks = atoi(buf_inv);
    result.hours_solar_flare_every = atoi(buf_flare);
    if (result.hours_solar_flare_every < 1) result.hours_solar_flare_every = 1;
    result.interactive_mode = interactive;
    if (show_custom_prompt && buf_prompt[0]) {
        result.use_custom_prompt = true;
        strncpy(result.custom_prompt, buf_prompt, sizeof(result.custom_prompt) - 1);
    }
}

bool start_menu_update(void) {
    if (!menu_active) return false;

    int sw = GetScreenWidth(), sh = GetScreenHeight();
    int win_w = 740;
    int win_h = show_custom_prompt ? 520 : 400;
    if (win_w > sw - 20) win_w = sw - 20;
    if (win_h > sh - 20) win_h = sh - 20;
    int ox = (sw - win_w) / 2, oy = (sh - win_h) / 2;

    /* Very dark background, like a bootup shell. */
    DrawRectangle(0, 0, sw, sh, (Color){0, 12, 6, 220});

    /* Hacker-style frame with title. */
    Rectangle panel = { (float)ox, (float)oy, (float)win_w, (float)win_h };
    ui_theme_draw_frame(panel, "BOOT :: NEW SESSION");

    int row_h = 30;
    int gap   = 9;
    float y   = oy + 45;
    float lx  = ox + 20;
    float ex  = ox + 380;
    float ew  = 340;
    float label_w = ex - lx - 10;

    /* Scenario toggle */
    GuiLabel((Rectangle){lx, y, label_w, row_h}, "> SCENARIO.....:");
    if (GuiButton((Rectangle){ex, y, 160, row_h},
                  scenario == 0 ? "[*] EXPLORER" : "[ ] EXPLORER")) {
        scenario = 0; apply_preset(0);
    }
    if (GuiButton((Rectangle){ex + 170, y, 160, row_h},
                  scenario == 1 ? "[*] BUILDER" : "[ ] BUILDER")) {
        scenario = 1; apply_preset(1);
    }
    y += row_h + gap;

    /* Start buttons */
    if (GuiButton((Rectangle){lx, y, win_w - 40, row_h + 5},
                  ">> RUN WITH DEFAULT PROMPT <<")) {
        show_custom_prompt = false;
        fill_result();
        menu_active = false;
        return false;
    }
    y += row_h + 5 + gap;
    if (GuiButton((Rectangle){lx, y, win_w - 40, row_h + 5},
                  ">> RUN WITH CUSTOM PROMPT <<")) {
        show_custom_prompt = true;
    }
    y += row_h + 5 + gap + 4;

    /* Fields */
    struct { const char *label; char *buf; int bufsz; bool *edit; } fields[] = {
        { "> ROCK CLUSTERS:", buf_rocks,  sizeof(buf_rocks),  &edit_rocks  },
        { "> INITIAL TOWN.:", buf_town,   sizeof(buf_town),   &edit_town   },
        { "> ENERGY.......:", buf_energy, sizeof(buf_energy), &edit_energy },
        { "> INV. ROCKS...:", buf_inv,    sizeof(buf_inv),    &edit_inv    },
        { "> FLARE EVERY..:", buf_flare,  sizeof(buf_flare),  &edit_flare  },
    };
    int nfields = (int)(sizeof(fields) / sizeof(fields[0]));
    for (int i = 0; i < nfields; i++) {
        GuiLabel((Rectangle){lx, y, label_w, row_h}, fields[i].label);
        if (GuiTextBox((Rectangle){ex, y, ew, row_h},
                       fields[i].buf, fields[i].bufsz, *fields[i].edit))
            *fields[i].edit = !*fields[i].edit;
        y += row_h + 3;
    }
    y += gap;

    /* Interactive toggle */
    GuiCheckBox((Rectangle){lx, y, 18, 18}, "  INTERACTIVE MODE (reply to bot)",
                &interactive);
    y += row_h;

    /* Custom prompt area */
    if (show_custom_prompt) {
        GuiLabel((Rectangle){lx, y, win_w - 40, row_h}, "> CUSTOM PROMPT:");
        y += row_h;
        int prev_wrap = GuiGetStyle(DEFAULT, TEXT_WRAP_MODE);
        GuiSetStyle(DEFAULT, TEXT_WRAP_MODE, TEXT_WRAP_WORD);
        if (GuiTextBox((Rectangle){lx, y, win_w - 40, 80},
                       buf_prompt, sizeof(buf_prompt), edit_prompt))
            edit_prompt = !edit_prompt;
        GuiSetStyle(DEFAULT, TEXT_WRAP_MODE, prev_wrap);
        y += 88;
        if (GuiButton((Rectangle){lx, y, 260, row_h + 3},
                      ">> CONFIRM & RUN <<")) {
            fill_result();
            menu_active = false;
            return false;
        }
        if (GuiButton((Rectangle){lx + 270, y, 150, row_h + 3}, "CANCEL")) {
            show_custom_prompt = false;
        }
    }

    return true;
}

StartMenuResult start_menu_result(void) {
    return result;
}
