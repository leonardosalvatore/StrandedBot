#ifndef START_MENU_H
#define START_MENU_H

#include <stdbool.h>

typedef struct {
    bool started;
    int  rocks_amount;
    int  initial_town_size;
    int  energy;
    int  inventory_rocks;
    int  hours_solar_flare_every;
    bool use_custom_prompt;
    char custom_prompt[4096];

    /* llama.cpp launcher fields */
    char llama_start_script[512];
    bool llama_auto_start;
} StartMenuResult;

void             start_menu_init(void);
bool             start_menu_update(void);   /* true while menu is active */
StartMenuResult  start_menu_result(void);

#endif /* START_MENU_H */
