#ifndef BOTS_CONFIG_H
#define BOTS_CONFIG_H

#include <stdbool.h>

/* Per-scenario preset. Sizes match the start-menu text buffers and the
 * mission-prompt slot that used to be hardcoded in main.c. */
typedef struct {
    int  rocks_amount;
    int  initial_town_size;
    int  energy;
    int  inventory_rocks;
    int  hours_solar_flare_every;
    char mission_prompt[4096];
} ScenarioConfig;

/* Everything the start menu can tweak, plus every LLM prompt the agent uses.
 * Two JSON files back this struct: a shipped read-only defaults file and a
 * user-writable custom file. */
typedef struct {
    ScenarioConfig explorer;
    ScenarioConfig builder;

    int  default_scenario;          /* 0 = explorer, 1 = builder */
    bool interactive_mode;

    struct {
        char start_script[512];
        bool auto_start;
        int  port;              /* TCP port llama-server binds (and client hits) */
    } llama;

    struct {
        char system[4096];
        /* Tool descriptions may contain %d placeholders; the agent applies
         * snprintf against compile-time constants (GRID_WIDTH, etc.). */
        char tool_move_to[512];
        char tool_look_far[256];
        char tool_dig[256];
        char tool_create[1024];
        char tool_list_built_tiles[256];
        char tool_wait[512];
    } prompts;
} BotsConfig;

/* File names searched in CWD then next to the executable (via
 * GetApplicationDirectory). */
#define BOTS_CONFIG_DEFAULTS_FILE "bots-defaults.json"
#define BOTS_CONFIG_CUSTOM_FILE   "bots-custom.json"

/* Populate every field from hardcoded fallbacks, then overlay any values
 * found in bots-defaults.json. Always returns true — the fallback path is
 * always available. */
bool config_load_defaults(BotsConfig *out);

/* Like config_load_defaults, then overlay any values found in
 * bots-custom.json. Missing custom file is not an error; callers can write
 * one with config_save_custom once the user hits RUN. */
bool config_load_custom(BotsConfig *out);

/* Serialize the full BotsConfig to bots-custom.json in CWD. */
bool config_save_custom(const BotsConfig *in);

/* Overwrite bots-custom.json with the contents of bots-defaults.json
 * (falling back to the hardcoded defaults if the defaults file is missing).
 * Used by the "REVERT TO DEFAULTS" button in the start menu. */
bool config_revert_custom_to_defaults(void);

/* Global pointer so modules that are awkward to thread a config through
 * (llm_agent.c) can still reach the prompts. main() sets this once after
 * loading. Returns NULL before config_set_active is called. */
void                 config_set_active(const BotsConfig *cfg);
const BotsConfig    *config_active(void);

#endif /* BOTS_CONFIG_H */
