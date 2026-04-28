#include "start_menu.h"
#include "config.h"
#include "game_logic.h"
#include "raylib.h"
#include "ui_theme.h"

#define RAYGUI_IMPLEMENTATION
/* Slow down the initial auto-repeat in GuiTextBox.
 *
 * raygui's defaults (raygui.h ~line 2441) make backspace / delete / arrow
 * keys start repeating after just COOLDOWN=40 frames (~0.66s at 60 FPS),
 * which fires accidentally every time the operator holds a key for a
 * fraction of a second longer than they intended (e.g. while moving the
 * cursor through the long custom prompt). Bumping the cooldown to 90
 * frames (~1.5s) gives a much more deliberate "press-and-hold to start
 * repeating" feel without affecting the post-cooldown repeat rate. */
#define RAYGUI_TEXTBOX_AUTO_CURSOR_COOLDOWN 90
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

static bool llama_auto = true;

/* Populate every editable widget from menu_cfg. Called on init and after
 * REVERT TO DEFAULTS. */
static void refresh_widgets_from_cfg(void) {
    llama_auto = menu_cfg.llama.auto_start;
    strncpy(buf_script, menu_cfg.llama.start_script, sizeof(buf_script) - 1);
    buf_script[sizeof(buf_script) - 1] = '\0';

    snprintf(buf_rocks,  sizeof(buf_rocks),  "%d", menu_cfg.game.rocks_amount);
    snprintf(buf_town,   sizeof(buf_town),   "%d", menu_cfg.game.initial_town_size);
    snprintf(buf_energy, sizeof(buf_energy), "%d", menu_cfg.game.energy);
    snprintf(buf_inv,    sizeof(buf_inv),    "%d", menu_cfg.game.inventory_rocks);
    snprintf(buf_flare,  sizeof(buf_flare),  "%d", menu_cfg.game.hours_solar_flare_every);
    /* Pre-fill from the persisted "last custom prompt" so the operator
     * doesn't have to retype their mission on every launch. REVERT TO
     * DEFAULTS clears it (defaults set custom = ""). */
    strncpy(buf_prompt, menu_cfg.prompts.custom, sizeof(buf_prompt) - 1);
    buf_prompt[sizeof(buf_prompt) - 1] = '\0';
}

void start_menu_init(void) {
    menu_active = true;
    memset(&result, 0, sizeof(result));
    config_load_custom(&menu_cfg);
    refresh_widgets_from_cfg();
}

/* Copy edited buffers back into menu_cfg so we can persist them. */
static void sync_cfg_from_widgets(void) {
    menu_cfg.llama.auto_start = llama_auto;
    strncpy(menu_cfg.llama.start_script, buf_script,
            sizeof(menu_cfg.llama.start_script) - 1);
    menu_cfg.llama.start_script[sizeof(menu_cfg.llama.start_script) - 1] = '\0';

    menu_cfg.game.rocks_amount            = atoi(buf_rocks);
    if (menu_cfg.game.rocks_amount < 1) menu_cfg.game.rocks_amount = 1;
    menu_cfg.game.initial_town_size       = atoi(buf_town);
    menu_cfg.game.energy                  = atoi(buf_energy);
    if (menu_cfg.game.energy < 1) menu_cfg.game.energy = 1;
    menu_cfg.game.inventory_rocks         = atoi(buf_inv);
    menu_cfg.game.hours_solar_flare_every = atoi(buf_flare);
    if (menu_cfg.game.hours_solar_flare_every < 1)
        menu_cfg.game.hours_solar_flare_every = 1;

    strncpy(menu_cfg.prompts.custom, buf_prompt,
            sizeof(menu_cfg.prompts.custom) - 1);
    menu_cfg.prompts.custom[sizeof(menu_cfg.prompts.custom) - 1] = '\0';
}

static void fill_result(void) {
    sync_cfg_from_widgets();

    result.started                 = true;
    result.rocks_amount            = menu_cfg.game.rocks_amount;
    result.initial_town_size       = menu_cfg.game.initial_town_size;
    result.energy                  = menu_cfg.game.energy;
    result.inventory_rocks         = menu_cfg.game.inventory_rocks;
    result.hours_solar_flare_every = menu_cfg.game.hours_solar_flare_every;

    strncpy(result.llama_start_script, menu_cfg.llama.start_script,
            sizeof(result.llama_start_script) - 1);
    result.llama_start_script[sizeof(result.llama_start_script) - 1] = '\0';
    result.llama_auto_start = llama_auto;

    if (buf_prompt[0]) {
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
    int win_h = 560;
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

    /* REVERT lives at the top — it's a setup-time reset the operator
     * generally hits BEFORE filling things in. RUN moved to the bottom
     * (after the custom prompt) so it's the natural last action. */
    if (GuiButton((Rectangle){lx, y, win_w - 40, row_h + 5},
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
    GuiCheckBox((Rectangle){lx, y, 18, 18},
                "  AUTO-START LLAMA SERVER IF NOT RUNNING",
                &llama_auto);
    y += row_h;

    /* Custom prompt area (always visible; empty is fine and means "no
     * mission prompt", which the agent handles cleanly). */
    GuiLabel((Rectangle){lx, y, win_w - 40, row_h},
             "> CUSTOM PROMPT (optional):");
    y += row_h;
    /* IMPORTANT: raygui 4.0 GuiTextBox silently refuses input whenever
     * TEXT_WRAP_MODE != TEXT_WRAP_NONE (see raygui.h, ~line 2492). The
     * previous version set TEXT_WRAP_WORD here, which made the box
     * completely uneditable. raygui 4.0 textboxes are single-line and
     * scroll horizontally when the text exceeds the width; long
     * prompts are fine, they just aren't visually wrapped. */
    if (GuiTextBox((Rectangle){lx, y, win_w - 40, 80},
                   buf_prompt, sizeof(buf_prompt), edit_prompt))
        edit_prompt = !edit_prompt;
    y += 80 + gap;

    /* RUN button — the final action, anchored at the bottom of the panel
     * so it stays visible and reachable after the operator has filled in
     * the form (especially the custom prompt above it). */
    if (GuiButton((Rectangle){lx, y, win_w - 40, row_h + 5},
                  ">> RUN <<")) {
        fill_result();
        menu_active = false;
        return false;
    }

    return true;
}

StartMenuResult start_menu_result(void) {
    return result;
}
