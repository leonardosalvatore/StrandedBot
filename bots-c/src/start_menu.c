#include "start_menu.h"
#include "game_logic.h"
#include "raylib.h"

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
static char buf_model[256]  = "";
static char buf_rocks[32]   = "";
static char buf_town[32]    = "";
static char buf_energy[32]  = "";
static char buf_inv[32]     = "";
static char buf_flare[32]   = "";
static char buf_prompt[4096] = "";

static bool edit_model  = false;
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

void start_menu_init(const char *default_model) {
    menu_active = true;
    show_custom_prompt = false;
    memset(&result, 0, sizeof(result));
    strncpy(buf_model, default_model ? default_model : "ministral-3:8b",
            sizeof(buf_model) - 1);
    apply_preset(0);
    buf_prompt[0] = '\0';
}

static void fill_result(void) {
    result.started = true;
    result.scenario = scenario == 0 ? SCENARIO_EXPLORER : SCENARIO_BUILDER;
    strncpy(result.model, buf_model, sizeof(result.model) - 1);
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
    int win_w = 620, win_h = 560;
    int ox = (sw - win_w) / 2, oy = (sh - win_h) / 2;

    /* Dim background */
    DrawRectangle(0, 0, sw, sh, (Color){0, 0, 0, 160});

    /* Panel */
    GuiPanel((Rectangle){ox, oy, win_w, win_h}, "Start Game");

    float y = oy + 35;
    float lx = ox + 20, ex = ox + 380, ew = 210;

    /* Scenario toggle */
    GuiLabel((Rectangle){lx, y, 120, 25}, "Scenario:");
    if (GuiButton((Rectangle){lx + 130, y, 120, 25},
                  scenario == 0 ? "[Explorer]" : "Explorer")) {
        scenario = 0; apply_preset(0);
    }
    if (GuiButton((Rectangle){lx + 260, y, 120, 25},
                  scenario == 1 ? "[Builder]" : "Builder")) {
        scenario = 1; apply_preset(1);
    }
    y += 35;

    /* Start buttons */
    if (GuiButton((Rectangle){lx, y, 350, 35}, "Start with default prompt")) {
        show_custom_prompt = false;
        fill_result();
        menu_active = false;
        return false;
    }
    y += 45;
    if (GuiButton((Rectangle){lx, y, 350, 35}, "Start with custom prompt")) {
        show_custom_prompt = true;
    }
    y += 50;

    /* Fields */
    GuiLabel((Rectangle){lx, y, 350, 25}, "Model (llama-server):");
    if (GuiTextBox((Rectangle){ex, y, ew, 25}, buf_model, sizeof(buf_model), edit_model))
        edit_model = !edit_model;
    y += 30;

    GuiLabel((Rectangle){lx, y, 350, 25}, "Rock clusters:");
    if (GuiTextBox((Rectangle){ex, y, ew, 25}, buf_rocks, sizeof(buf_rocks), edit_rocks))
        edit_rocks = !edit_rocks;
    y += 30;

    GuiLabel((Rectangle){lx, y, 350, 25}, "Initial town (habitat groups):");
    if (GuiTextBox((Rectangle){ex, y, ew, 25}, buf_town, sizeof(buf_town), edit_town))
        edit_town = !edit_town;
    y += 30;

    GuiLabel((Rectangle){lx, y, 350, 25}, "Energy:");
    if (GuiTextBox((Rectangle){ex, y, ew, 25}, buf_energy, sizeof(buf_energy), edit_energy))
        edit_energy = !edit_energy;
    y += 30;

    GuiLabel((Rectangle){lx, y, 350, 25}, "Inventory rocks:");
    if (GuiTextBox((Rectangle){ex, y, ew, 25}, buf_inv, sizeof(buf_inv), edit_inv))
        edit_inv = !edit_inv;
    y += 30;

    GuiLabel((Rectangle){lx, y, 350, 25}, "Hours between solar flares:");
    if (GuiTextBox((Rectangle){ex, y, ew, 25}, buf_flare, sizeof(buf_flare), edit_flare))
        edit_flare = !edit_flare;
    y += 35;

    /* Interactive toggle */
    GuiCheckBox((Rectangle){lx, y, 20, 20}, "Interactive mode (reply to bot questions)", &interactive);
    y += 30;

    /* Custom prompt area */
    if (show_custom_prompt) {
        GuiLabel((Rectangle){lx, y, 350, 25}, "Custom prompt:");
        y += 25;
        if (GuiTextBox((Rectangle){lx, y, win_w - 60, 80}, buf_prompt, sizeof(buf_prompt), edit_prompt))
            edit_prompt = !edit_prompt;
        y += 90;
        if (GuiButton((Rectangle){lx, y, 260, 30}, "Confirm & Start")) {
            fill_result();
            menu_active = false;
            return false;
        }
        if (GuiButton((Rectangle){lx + 280, y, 120, 30}, "Cancel")) {
            show_custom_prompt = false;
        }
    }

    return true;
}

StartMenuResult start_menu_result(void) {
    return result;
}
