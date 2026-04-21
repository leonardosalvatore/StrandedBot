#ifndef START_MENU_H
#define START_MENU_H

#include <stdbool.h>

typedef enum {
    SCENARIO_EXPLORER = 0,
    SCENARIO_BUILDER,
    SCENARIO_COUNT
} Scenario;

typedef struct {
    bool started;
    Scenario scenario;
    int  rocks_amount;
    int  initial_town_size;
    int  energy;
    int  inventory_rocks;
    int  hours_solar_flare_every;
    bool interactive_mode;
    bool use_custom_prompt;
    char custom_prompt[4096];
} StartMenuResult;

void             start_menu_init(void);
bool             start_menu_update(void);   /* true while menu is active */
StartMenuResult  start_menu_result(void);

#endif /* START_MENU_H */
