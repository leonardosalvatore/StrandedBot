#include "particles.h"
#include "raylib.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>

/* ── Config ───────────────────────────────────────────────────────────────── */
#define MAX_PARTICLES  1024
#define MAX_RINGS      16

/* ── Particle kinds ───────────────────────────────────────────────────────── */
typedef enum {
    P_DUST,     /* small filled circle, gravity, fades to transparent */
    P_SPARKLE,  /* flickering bright dot, rises gently */
    P_DEBRIS,   /* rotating rectangle, gravity */
    P_ENERGY,   /* rising bright dot, slight horizontal sine wiggle */
    P_ZAP,      /* short bright streak, very brief */
} PKind;

typedef struct {
    bool   active;
    PKind  kind;
    float  x, y;
    float  vx, vy;
    float  age, lifetime;
    float  size;         /* world units */
    float  rot, vrot;    /* radians for debris */
    Color  c_start, c_end;
    float  gravity;      /* world-units / sec^2, negative lifts upward */
    float  wiggle_phase;
    float  wiggle_amp;
} Particle;

/* Scan rings are rendered differently (as expanding hollow circles) so we
 * keep them in a separate tiny pool. */
typedef struct {
    bool   active;
    float  x, y;
    float  age, lifetime;
    float  max_radius;
    Color  color;
} Ring;

/* ── State ────────────────────────────────────────────────────────────────── */
static Particle       g_p[MAX_PARTICLES];
static Ring           g_r[MAX_RINGS];
static int            g_p_cursor = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── RNG (called under g_lock so rand() is safe) ──────────────────────────── */
static float frand01(void) { return (float)rand() / (float)RAND_MAX; }
static float frand_range(float lo, float hi) { return lo + (hi - lo) * frand01(); }

/* ── Allocation (round-robin with "pick inactive first") ──────────────────── */
static Particle *alloc_particle(void) {
    /* Quick scan for an inactive slot starting from cursor. */
    for (int probe = 0; probe < MAX_PARTICLES; probe++) {
        int idx = (g_p_cursor + probe) % MAX_PARTICLES;
        if (!g_p[idx].active) {
            g_p_cursor = (idx + 1) % MAX_PARTICLES;
            return &g_p[idx];
        }
    }
    /* All slots active — overwrite the cursor slot. */
    int idx = g_p_cursor;
    g_p_cursor = (g_p_cursor + 1) % MAX_PARTICLES;
    return &g_p[idx];
}

static Ring *alloc_ring(void) {
    for (int i = 0; i < MAX_RINGS; i++) {
        if (!g_r[i].active) return &g_r[i];
    }
    /* Overwrite the one with the highest age/lifetime ratio (nearly done). */
    int best = 0;
    float best_score = -1.0f;
    for (int i = 0; i < MAX_RINGS; i++) {
        float s = g_r[i].age / (g_r[i].lifetime + 1e-6f);
        if (s > best_score) { best_score = s; best = i; }
    }
    return &g_r[best];
}

/* ── Public API ───────────────────────────────────────────────────────────── */
void particles_init(void) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_PARTICLES; i++) g_p[i].active = false;
    for (int i = 0; i < MAX_RINGS; i++)     g_r[i].active = false;
    g_p_cursor = 0;
    pthread_mutex_unlock(&g_lock);
}

void particles_update(float dt) {
    if (dt <= 0.0f) return;
    pthread_mutex_lock(&g_lock);

    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &g_p[i];
        if (!p->active) continue;
        p->age += dt;
        if (p->age >= p->lifetime) { p->active = false; continue; }
        p->vy += p->gravity * dt;
        p->x  += p->vx * dt;
        p->y  += p->vy * dt;
        p->rot += p->vrot * dt;
        p->wiggle_phase += dt * 8.0f;
    }

    for (int i = 0; i < MAX_RINGS; i++) {
        Ring *r = &g_r[i];
        if (!r->active) continue;
        r->age += dt;
        if (r->age >= r->lifetime) r->active = false;
    }

    pthread_mutex_unlock(&g_lock);
}

static Color lerp_color(Color a, Color b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    Color c;
    c.r = (unsigned char)(a.r + (b.r - a.r) * t);
    c.g = (unsigned char)(a.g + (b.g - a.g) * t);
    c.b = (unsigned char)(a.b + (b.b - a.b) * t);
    c.a = (unsigned char)(a.a + (b.a - a.a) * t);
    return c;
}

void particles_draw_world(void) {
    pthread_mutex_lock(&g_lock);

    /* Rings first (underneath particles). */
    for (int i = 0; i < MAX_RINGS; i++) {
        Ring *r = &g_r[i];
        if (!r->active) continue;
        float t = r->age / r->lifetime;
        float radius = r->max_radius * t;
        float alpha_f = 1.0f - t;
        Color c = r->color;
        c.a = (unsigned char)(c.a * alpha_f);
        /* Outer ring */
        float thickness = 1.5f + 2.0f * (1.0f - t);
        DrawRing((Vector2){r->x, r->y},
                 radius - thickness * 0.5f,
                 radius + thickness * 0.5f,
                 0.0f, 360.0f, 48, c);
        /* Soft inner ring for a nicer look */
        Color c2 = c; c2.a = (unsigned char)(c.a * 0.4f);
        float inner = radius * 0.6f;
        DrawRing((Vector2){r->x, r->y},
                 inner - 0.6f, inner + 0.6f,
                 0.0f, 360.0f, 32, c2);
    }

    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &g_p[i];
        if (!p->active) continue;
        float t = p->age / p->lifetime;
        Color c = lerp_color(p->c_start, p->c_end, t);

        switch (p->kind) {
        case P_DUST: {
            float sz = p->size * (1.0f - 0.35f * t);
            DrawCircleV((Vector2){p->x, p->y}, sz, c);
            break;
        }
        case P_SPARKLE: {
            float flicker = 0.7f + 0.3f * sinf(p->wiggle_phase);
            float sz = p->size * flicker * (1.0f - 0.5f * t);
            /* Bright 4-point star: circle + cross. */
            DrawCircleV((Vector2){p->x, p->y}, sz, c);
            Color cx = c; cx.a = (unsigned char)(c.a * 0.7f);
            DrawLineEx((Vector2){p->x - sz * 2.2f, p->y},
                       (Vector2){p->x + sz * 2.2f, p->y}, 0.6f, cx);
            DrawLineEx((Vector2){p->x, p->y - sz * 2.2f},
                       (Vector2){p->x, p->y + sz * 2.2f}, 0.6f, cx);
            break;
        }
        case P_DEBRIS: {
            Rectangle rect = { p->x, p->y, p->size, p->size * 0.6f };
            DrawRectanglePro(rect, (Vector2){p->size * 0.5f, p->size * 0.3f},
                             p->rot * (180.0f / PI), c);
            break;
        }
        case P_ENERGY: {
            float wx = p->x + sinf(p->wiggle_phase) * p->wiggle_amp;
            float sz = p->size * (1.0f - 0.4f * t);
            DrawCircleV((Vector2){wx, p->y}, sz, c);
            Color halo = c; halo.a = (unsigned char)(c.a * 0.35f);
            DrawCircleV((Vector2){wx, p->y}, sz * 2.0f, halo);
            break;
        }
        case P_ZAP: {
            /* A short streak: from current pos backwards along velocity. */
            float len = 2.0f + p->size;
            float vmag = sqrtf(p->vx * p->vx + p->vy * p->vy);
            float dx = vmag > 1e-3f ? p->vx / vmag : 1.0f;
            float dy = vmag > 1e-3f ? p->vy / vmag : 0.0f;
            DrawLineEx((Vector2){p->x - dx * len, p->y - dy * len},
                       (Vector2){p->x, p->y}, 0.8f + 0.6f * (1.0f - t), c);
            break;
        }
        }
    }

    pthread_mutex_unlock(&g_lock);
}

/* ── Spawn helpers ────────────────────────────────────────────────────────── */
static Particle *mk(PKind kind, float x, float y) {
    Particle *p = alloc_particle();
    p->active = true;
    p->kind = kind;
    p->x = x; p->y = y;
    p->vx = p->vy = 0.0f;
    p->age = 0.0f;
    p->rot = p->vrot = 0.0f;
    p->wiggle_phase = frand01() * 6.2831853f;
    p->wiggle_amp = 0.0f;
    p->gravity = 0.0f;
    p->size = 1.0f;
    p->lifetime = 1.0f;
    p->c_start = WHITE;
    p->c_end = (Color){255, 255, 255, 0};
    return p;
}

void particles_spawn_dig_dust(float x, float y) {
    pthread_mutex_lock(&g_lock);
    /* Brown dust cloud + a few small debris chips. */
    for (int i = 0; i < 22; i++) {
        Particle *p = mk(P_DUST, x, y);
        float ang = frand01() * 2.0f * PI;
        float spd = frand_range(8.0f, 38.0f);
        p->vx = cosf(ang) * spd;
        p->vy = sinf(ang) * spd - frand_range(6.0f, 22.0f);
        p->lifetime = frand_range(0.6f, 1.1f);
        p->size = frand_range(1.2f, 2.6f);
        p->gravity = 32.0f;
        unsigned char tone = (unsigned char)frand_range(90, 160);
        p->c_start = (Color){(unsigned char)(tone + 20), (unsigned char)(tone - 20),
                             (unsigned char)(tone - 50), 220};
        p->c_end   = (Color){60, 40, 25, 0};
    }
    for (int i = 0; i < 6; i++) {
        Particle *p = mk(P_DEBRIS, x, y);
        float ang = frand01() * 2.0f * PI;
        float spd = frand_range(20.0f, 55.0f);
        p->vx = cosf(ang) * spd;
        p->vy = sinf(ang) * spd - frand_range(10.0f, 30.0f);
        p->lifetime = frand_range(0.7f, 1.2f);
        p->size = frand_range(1.2f, 2.0f);
        p->gravity = 70.0f;
        p->rot = frand01() * 2.0f * PI;
        p->vrot = frand_range(-8.0f, 8.0f);
        p->c_start = (Color){80, 55, 35, 255};
        p->c_end   = (Color){60, 40, 25, 0};
    }
    pthread_mutex_unlock(&g_lock);
}

void particles_spawn_build_sparkle(float x, float y) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < 24; i++) {
        Particle *p = mk(P_SPARKLE, x, y);
        float ang = frand01() * 2.0f * PI;
        float spd = frand_range(6.0f, 28.0f);
        p->vx = cosf(ang) * spd;
        p->vy = sinf(ang) * spd - frand_range(4.0f, 14.0f);
        p->lifetime = frand_range(0.5f, 1.1f);
        p->size = frand_range(0.7f, 1.6f);
        p->gravity = -6.0f; /* drifts gently upward */
        /* Warm gold → cool cyan twinkle. */
        bool warm = (frand01() < 0.65f);
        if (warm) {
            p->c_start = (Color){255, 230, 150, 255};
            p->c_end   = (Color){255, 180, 80, 0};
        } else {
            p->c_start = (Color){210, 240, 255, 255};
            p->c_end   = (Color){120, 200, 255, 0};
        }
    }
    /* A bright central puff. */
    Ring *r = alloc_ring();
    r->active = true;
    r->x = x; r->y = y;
    r->age = 0.0f;
    r->lifetime = 0.45f;
    r->max_radius = 10.0f;
    r->color = (Color){255, 230, 180, 220};
    pthread_mutex_unlock(&g_lock);
}

void particles_spawn_scan_pulse(float x, float y, float world_radius) {
    pthread_mutex_lock(&g_lock);
    Ring *r = alloc_ring();
    r->active = true;
    r->x = x; r->y = y;
    r->age = 0.0f;
    r->lifetime = 0.9f;
    r->max_radius = world_radius;
    r->color = (Color){100, 220, 255, 230};
    /* Small bright motes around the bot. */
    for (int i = 0; i < 10; i++) {
        Particle *p = mk(P_ENERGY, x, y);
        float ang = frand01() * 2.0f * PI;
        float spd = frand_range(4.0f, 18.0f);
        p->vx = cosf(ang) * spd;
        p->vy = sinf(ang) * spd;
        p->lifetime = frand_range(0.5f, 0.9f);
        p->size = frand_range(0.6f, 1.2f);
        p->wiggle_amp = frand_range(0.3f, 1.0f);
        p->c_start = (Color){180, 240, 255, 230};
        p->c_end   = (Color){100, 200, 255, 0};
    }
    pthread_mutex_unlock(&g_lock);
}

void particles_spawn_move_dust(float x, float y) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < 3; i++) {
        Particle *p = mk(P_DUST, x + frand_range(-1.0f, 1.0f),
                                y + frand_range(-0.5f, 0.5f));
        p->vx = frand_range(-8.0f, 8.0f);
        p->vy = frand_range(-14.0f, -2.0f);
        p->lifetime = frand_range(0.3f, 0.6f);
        p->size = frand_range(0.8f, 1.5f);
        p->gravity = 18.0f;
        p->c_start = (Color){170, 160, 140, 180};
        p->c_end   = (Color){120, 110, 90, 0};
    }
    pthread_mutex_unlock(&g_lock);
}

void particles_spawn_charge_spark(float x, float y) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < 2; i++) {
        Particle *p = mk(P_ENERGY, x + frand_range(-4.0f, 4.0f), y + 4.0f);
        p->vx = frand_range(-4.0f, 4.0f);
        p->vy = frand_range(-22.0f, -10.0f);
        p->lifetime = frand_range(0.5f, 0.9f);
        p->size = frand_range(0.7f, 1.3f);
        p->wiggle_amp = frand_range(0.5f, 1.2f);
        p->c_start = (Color){255, 240, 120, 240};
        p->c_end   = (Color){255, 140, 40, 0};
    }
    /* Tiny cyan arc spark occasionally. */
    if (frand01() < 0.4f) {
        Particle *p = mk(P_ZAP, x + frand_range(-3.0f, 3.0f),
                                y + frand_range(-3.0f, 3.0f));
        float ang = frand01() * 2.0f * PI;
        float spd = frand_range(60.0f, 120.0f);
        p->vx = cosf(ang) * spd;
        p->vy = sinf(ang) * spd;
        p->lifetime = 0.15f;
        p->size = 1.2f;
        p->c_start = (Color){180, 240, 255, 255};
        p->c_end   = (Color){120, 180, 255, 0};
    }
    pthread_mutex_unlock(&g_lock);
}

void particles_spawn_destruction(float x, float y) {
    pthread_mutex_lock(&g_lock);
    /* Big shockwave ring. */
    Ring *r = alloc_ring();
    r->active = true;
    r->x = x; r->y = y;
    r->age = 0.0f;
    r->lifetime = 0.9f;
    r->max_radius = 60.0f;
    r->color = (Color){255, 120, 40, 255};

    for (int i = 0; i < 40; i++) {
        Particle *p = mk(P_DEBRIS, x, y);
        float ang = frand01() * 2.0f * PI;
        float spd = frand_range(40.0f, 140.0f);
        p->vx = cosf(ang) * spd;
        p->vy = sinf(ang) * spd - frand_range(10.0f, 40.0f);
        p->lifetime = frand_range(1.0f, 1.8f);
        p->size = frand_range(1.0f, 2.8f);
        p->gravity = 90.0f;
        p->rot = frand01() * 2.0f * PI;
        p->vrot = frand_range(-12.0f, 12.0f);
        p->c_start = (Color){80, 80, 80, 255};
        p->c_end   = (Color){30, 30, 30, 0};
    }
    for (int i = 0; i < 30; i++) {
        Particle *p = mk(P_SPARKLE, x, y);
        float ang = frand01() * 2.0f * PI;
        float spd = frand_range(30.0f, 110.0f);
        p->vx = cosf(ang) * spd;
        p->vy = sinf(ang) * spd;
        p->lifetime = frand_range(0.4f, 1.0f);
        p->size = frand_range(0.8f, 1.8f);
        p->c_start = (Color){255, 200, 80, 255};
        p->c_end   = (Color){255, 40, 20, 0};
    }
    pthread_mutex_unlock(&g_lock);
}

void particles_spawn_flare_zap(float x, float y) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < 14; i++) {
        Particle *p = mk(P_ZAP,
                         x + frand_range(-6.0f, 6.0f),
                         y + frand_range(-6.0f, 6.0f));
        float ang = frand01() * 2.0f * PI;
        float spd = frand_range(120.0f, 260.0f);
        p->vx = cosf(ang) * spd;
        p->vy = sinf(ang) * spd;
        p->lifetime = frand_range(0.12f, 0.28f);
        p->size = frand_range(1.5f, 3.0f);
        p->c_start = (Color){255, 255, 180, 255};
        p->c_end   = (Color){255, 180, 60, 0};
    }
    Ring *r = alloc_ring();
    r->active = true;
    r->x = x; r->y = y;
    r->age = 0.0f;
    r->lifetime = 0.6f;
    r->max_radius = 30.0f;
    r->color = (Color){255, 240, 140, 230};
    pthread_mutex_unlock(&g_lock);
}
