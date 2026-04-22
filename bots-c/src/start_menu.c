#include "start_menu.h"
#include "config.h"
#include "game_logic.h"
#include "raylib.h"
#include "ui_theme.h"

#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* We only define RAYGUI_IMPLEMENTATION once here; the rest of the project
   uses raygui as a normal header. */

static bool            menu_active = true;
static StartMenuResult result      = {0};

/* Live config snapshot the menu edits in-memory. Seeded from custom config
 * on init; overwritten by RUN or REVERT. */
static BotsConfig menu_cfg;

/* Editable text buffers (fed by menu_cfg on reset). */
static char buf_rocks[32]       = "";
static char buf_town[32]        = "";
static char buf_energy[32]      = "";
static char buf_inv[32]         = "";
static char buf_flare[32]       = "";
static char buf_script[512]     = "";
static char buf_prompt[4096]    = "";

static bool edit_rocks   = false;
static bool edit_town    = false;
static bool edit_energy  = false;
static bool edit_inv     = false;
static bool edit_flare   = false;
static bool edit_script  = false;
static bool edit_prompt  = false;

static int  scenario           = 0; /* 0=Explorer, 1=Builder */
static bool interactive        = true;
static bool llama_auto         = true;
static bool show_custom_prompt = false;

/* Pull the preset for `sc` out of menu_cfg into the editable buffers. */
static void apply_preset_from_cfg(int sc) {
    const ScenarioConfig *s = (sc == 1) ? &menu_cfg.builder : &menu_cfg.explorer;
    snprintf(buf_rocks,  sizeof(buf_rocks),  "%d", s->rocks_amount);
    snprintf(buf_town,   sizeof(buf_town),   "%d", s->initial_town_size);
    snprintf(buf_energy, sizeof(buf_energy), "%d", s->energy);
    snprintf(buf_inv,    sizeof(buf_inv),    "%d", s->inventory_rocks);
    snprintf(buf_flare,  sizeof(buf_flare),  "%d", s->hours_solar_flare_every);
}

/* Populate every editable widget from menu_cfg. Called on init and after
 * REVERT TO DEFAULTS. */
static void refresh_widgets_from_cfg(void) {
    scenario    = menu_cfg.default_scenario == 1 ? 1 : 0;
    interactive = menu_cfg.interactive_mode;
    llama_auto  = menu_cfg.llama.auto_start;
    strncpy(buf_script, menu_cfg.llama.start_script, sizeof(buf_script) - 1);
    buf_script[sizeof(buf_script) - 1] = '\0';
    apply_preset_from_cfg(scenario);
    buf_prompt[0] = '\0';
}

void start_menu_init(void) {
    menu_active = true;
    show_custom_prompt = false;
    memset(&result, 0, sizeof(result));
    config_load_custom(&menu_cfg);
    refresh_widgets_from_cfg();
}

/* Copy edited buffers back into menu_cfg so we can persist them. */
static void sync_cfg_from_widgets(void) {
    menu_cfg.default_scenario = scenario == 1 ? 1 : 0;
    menu_cfg.interactive_mode = interactive;
    menu_cfg.llama.auto_start = llama_auto;
    strncpy(menu_cfg.llama.start_script, buf_script,
            sizeof(menu_cfg.llama.start_script) - 1);
    menu_cfg.llama.start_script[sizeof(menu_cfg.llama.start_script) - 1] = '\0';

    ScenarioConfig *s = (scenario == 1) ? &menu_cfg.builder : &menu_cfg.explorer;
    s->rocks_amount            = atoi(buf_rocks);
    if (s->rocks_amount < 1) s->rocks_amount = 1;
    s->initial_town_size       = atoi(buf_town);
    s->energy                  = atoi(buf_energy);
    if (s->energy < 1) s->energy = 1;
    s->inventory_rocks         = atoi(buf_inv);
    s->hours_solar_flare_every = atoi(buf_flare);
    if (s->hours_solar_flare_every < 1) s->hours_solar_flare_every = 1;
}

static void fill_result(void) {
    sync_cfg_from_widgets();

    result.started  = true;
    result.scenario = scenario == 0 ? SCENARIO_EXPLORER : SCENARIO_BUILDER;
    const ScenarioConfig *s = (scenario == 1) ? &menu_cfg.builder
                                              : &menu_cfg.explorer;
    result.rocks_amount            = s->rocks_amount;
    result.initial_town_size       = s->initial_town_size;
    result.energy                  = s->energy;
    result.inventory_rocks         = s->inventory_rocks;
    result.hours_solar_flare_every = s->hours_solar_flare_every;
    result.interactive_mode        = interactive;

    strncpy(result.llama_start_script, menu_cfg.llama.start_script,
            sizeof(result.llama_start_script) - 1);
    result.llama_start_script[sizeof(result.llama_start_script) - 1] = '\0';
    result.llama_auto_start = llama_auto;

    if (show_custom_prompt && buf_prompt[0]) {
        result.use_custom_prompt = true;
        strncpy(result.custom_prompt, buf_prompt,
                sizeof(result.custom_prompt) - 1);
    }

    /* Persist the edited menu to bots-custom.json so the next run (or
     * --autostart-custom) sees the same settings. */
    config_save_custom(&menu_cfg);
}

bool start_menu_update(void) {
    if (!menu_active) return false;

    int sw = GetScreenWidth(), sh = GetScreenHeight();
    int win_w = 740;
    int win_h = show_custom_prompt ? 610 : 490;
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
    float ex  = ox + 260;
    float ew  = win_w - (ex - ox) - 20;
    float label_w = ex - lx - 10;

    /* Scenario toggle */
    GuiLabel((Rectangle){lx, y, label_w, row_h}, "> SCENARIO.....:");
    if (GuiButton((Rectangle){ex, y, 160, row_h},
                  scenario == 0 ? "[*] EXPLORER" : "[ ] EXPLORER")) {
        scenario = 0; apply_preset_from_cfg(0);
    }
    if (GuiButton((Rectangle){ex + 170, y, 160, row_h},
                  scenario == 1 ? "[*] BUILDER" : "[ ] BUILDER")) {
        scenario = 1; apply_preset_from_cfg(1);
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
    if (GuiButton((Rectangle){lx, y, (win_w - 40) / 2 - 5, row_h + 5},
                  ">> RUN WITH CUSTOM PROMPT <<")) {
        show_custom_prompt = true;
    }
    if (GuiButton((Rectangle){lx + (win_w - 40) / 2 + 5, y,
                              (win_w - 40) / 2 - 5, row_h + 5},
                  ">> REVERT TO DEFAULTS <<")) {
        config_revert_custom_to_defaults();
        config_load_custom(&menu_cfg);
        refresh_widgets_from_cfg();
    }
    y += row_h + 5 + gap + 4;

    /* Numeric / path fields */
    struct { const char *label; char *buf; int bufsz; bool *edit; } fields[] = {
        { "> ROCK CLUSTERS:", buf_rocks,  sizeof(buf_rocks),  &edit_rocks  },
        { "> INITIAL TOWN.:", buf_town,   sizeof(buf_town),   &edit_town   },
        { "> ENERGY.......:", buf_energy, sizeof(buf_energy), &edit_energy },
        { "> INV. ROCKS...:", buf_inv,    sizeof(buf_inv),    &edit_inv    },
        { "> FLARE EVERY..:", buf_flare,  sizeof(buf_flare),  &edit_flare  },
        { "> LLAMA SCRIPT.:", buf_script, sizeof(buf_script), &edit_script },
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

    /* Toggles */
    GuiCheckBox((Rectangle){lx, y, 18, 18}, "  INTERACTIVE MODE (reply to bot)",
                &interactive);
    y += 24;
    GuiCheckBox((Rectangle){lx, y, 18, 18},
                "  AUTO-START LLAMA SERVER IF NOT RUNNING",
                &llama_auto);
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
