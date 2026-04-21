#ifndef PARTICLES_H
#define PARTICLES_H

/* Tiny particle system used for visual feedback of bot actions. All
 * coordinates are world-space (same space as tiles and the bot sprite).
 *
 * Threading model: spawn functions are safe to call from any thread
 * (they lock internally). particles_update() and particles_draw_world()
 * must only be called from the main/render thread. */

void particles_init(void);
void particles_update(float dt);
void particles_draw_world(void);

/* Action-triggered spawns. (x, y) is world-space center of the effect. */
void particles_spawn_dig_dust(float x, float y);
void particles_spawn_build_sparkle(float x, float y);
void particles_spawn_scan_pulse(float x, float y, float world_radius);
void particles_spawn_move_dust(float x, float y);
void particles_spawn_charge_spark(float x, float y);
void particles_spawn_destruction(float x, float y);
void particles_spawn_flare_zap(float x, float y);

#endif /* PARTICLES_H */
