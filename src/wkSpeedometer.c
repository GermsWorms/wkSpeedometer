/* wkSpeedometer: a WormKit module for Worms Armageddon 3.8.1. It shows the speed of the worm whose turn it is (a digital
 * dial, or the number alone, in the bottom right corner or just above the worm's name), and optionally a movement line:
 * when a worm's turn starts, where that worm has been (its last turn bright, earlier turns dimmer), coloured by speed or
 * all red, until it moves (or all turn). Split out of wkMapZoom 4.3 beta 2: the speedometer, the line and the way the
 * module finds the turn's worm are ported from there. The module only reads the game; it changes nothing in it.
 *
 * How it works. WA's team tasks are seen through their message handler, and the worms through their physics step (both
 * hooks only look). Each frame (GameRender) the module notes which team has the turn, where its current worm is and how
 * fast it goes. Right after WA's own RenderDrawingQueue has returned for the game's display (so over the HUD), the line
 * and the speedometer are drawn one of two ways: through wkDirect3D (another module: the graphics card draws them over
 * the finished frame) when it is loaded and draws this frame, else into WA's 8-bit frame in the nearest colours of WA's
 * palette as shown. Where the map is on the screen comes from wkMapZoom when it is loaded and tells (its zoom), else
 * from WA's camera (100%).
 *
 * Hooks (WA 3.8.1 only: the SHA-256 of WA.exe, and WA's own bytes or another module's jump at each hook point):
 *   00533DC0 GameRender (ECX the runtime; ret)
 *   00542350 RenderDrawingQueue (EAX the queue; display, camera; ret 8)
 *   00557310 a team task's message handler (thiscall, ECX the task): observe only
 *   004FCDF0 an object's physics step (its manager, the object): observe only
 * wkMapZoom and wkWAI hook some of the same functions: a jump found there is chained onto through MinHook, and the
 * module it leads into is pinned in memory so it stays loaded.
 *
 * Settings (all optional) in wkSpeedometer.ini beside WA.exe, section [wkSpeedometer]: Enabled, Speedometer,
 * SpeedometerPlace, RedFrom, TurnLine, TurnLineKeep, TurnLineColour (see the README).
 * Log: %LOCALAPPDATA%\wkSpeedometer\wkSpeedometer.log, or the file named by WKSPEEDO_LOG.
 *
 * Test switches (environment variables, for isolated test copies of WA only): WKSPEEDO_TEST=1 one log line a second
 * (the turn's worm, its speed, which way the dial and the line were drawn); WKSPEEDO_TEST_LINE=1 the line shown all the
 * time for the turn's worm, as TurnLine=all with this turn's own points too (to see it in a replay); WKSPEEDO_NO_GPU=1
 * never through wkDirect3D.
 *
 * MinHook (third_party/minhook) is BSD licensed. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MinHook.h"
#include "image_guard.h"
#include "speedo.h"
#include "overlay_draw.h"

#define WKSPEEDO_VERSION "1.0 beta"

#define WA_BASE 0x400000u
#define GAME_RENDER_VA 0x533dc0u
#define QUEUE_VA 0x542350u
#define TEAM_HANDLER_VA 0x557310u       /* a team task's message handler (thiscall, ECX the task) */
#define PHYSICS_VA 0x4fcdf0u            /* an object's physics step (its manager, the object; every object, every step) */
#define WORM_VTABLE_VA 0x6644c8u        /* a worm: +0x2C the game, +0x84 / +0x88 its place (1/65536 px), +0x90 / +0x94 its
                                           move a step (1/65536 px), +0xFC its team, +0x100 its number in the team */
#define DISPLAY_VTABLE_VA 0x66a218u     /* the game display (DDDisplay) */
#define FRAME_PTR_VA 0x7a0eecu          /* the renderer's 8-bit frame: its pixels, width (= row stride), height */
#define FRAME_W_VA 0x7a0ef0u
#define FRAME_H_VA 0x7a0ef4u
#define SHOWN_PALETTE_VA 0x8ac8c8u      /* WA's palette as shown (256 x R, G, B, flags); the display's own copy (+0x358D)
                                           is wrong during fades */

#define RT_WORLD 0x488                  /* GameRender's runtime: +0x488 the world */
#define RT_ESC_MENU 0x434                   /* ... +0x434 == 1: WA's Esc menu is open */
#define WO_DISPLAY 0x4                  /* the world: +4 the display */
#define D_W 0x3548                      /* the display: its size, origin */
#define D_H 0x354c
#define D_ORG 0x3560
#define D_TERRAIN 0x4dd8                /* the land: an object whose +4 is its tile bitmaps row by row; the land's size, a */
#define D_LAND_W 0x4ddc                 /* tile's size */
#define D_LAND_H 0x4de0
#define D_TILE_W 0x4de4
#define D_TILE_H 0x4de8

#define VA(a) ((uintptr_t)(base + ((uintptr_t)(a) - WA_BASE)))
#define FIELD(p, off) (*(int32_t *)((char *)(p) + (off)))
#define VTBL(obj) (*(void ***)(obj))

static uintptr_t base;
static FILE *log_file;
static int installed, enabled = 1, test_mode, test_line, no_gpu;
static HINSTANCE self;
static void *frame_display, *last_world;
static int frame_drawn;                 /* the line and the speedometer were drawn this frame (once a frame) */
static int menu_open;                   /* WA's Esc menu is open this frame: nothing drawn */
static LARGE_INTEGER clock_freq;
static DWORD live_tick;                 /* GetTickCount of the last game frame */
static unsigned frames;                 /* GameRenders this game */

/* the movement line: where each worm went this game (TurnLine=all) or in its last turn (TurnLine=last), shown when its
 * turn starts */
#define TRAIL_TEAMS 16
#define TRAIL_PER_TEAM 16
#define TRAIL_WORMS 64
#define TRAIL_MAX 32768
typedef struct { tpoint *pt; int n, cap; } team_trail;
static team_trail *trails;              /* by worm: its team (a team task's +0x38) * TRAIL_PER_TEAM + its number in it */
static uintptr_t team_tasks[TRAIL_TEAMS], worm_objs[TRAIL_WORMS];
static int team_flags[TRAIL_TEAMS], n_team_tasks, n_worm_objs;
static int worms_ok;                    /* the team task and physics step hooks are on */
static int trail_on, trail_all = 1, trail_keep, trail_colour = 1;   /* TurnLine (off by default), TurnLineKeep,
                                                                       TurnLineColour (1 speed, 0 red) */
static int trail_show = -1, trail_hl, trail_have0, trail_multi, trail_owner = -1, trail_visible, trail_turn;
static int line_slot = -1;              /* (WKSPEEDO_TEST_LINE: the turn's worm's line, shown all the time) */
static double trail_x0, trail_y0;

/* the turn's worm and its speed */
static int turn_worm_ok;                /* where the turn's worm is, this frame */
static double turn_wx, turn_wy, turn_gv;   /* its place; its speed as the game has it (px/s; team_worm) */
static int turn_key = -1, track_key = -1, track_turn = -1;   /* the turn's worm (team x 64 + its number), and whose
                                                                and which turn's the track is */
#define TRACK_N 48
static double trk_x[TRACK_N], trk_y[TRACK_N], trk_t[TRACK_N];   /* the worm's last places, and when */
static int trk_n, trk_i;
static LARGE_INTEGER spd_clock;
static double spd_from_path;            /* the speed from the worm's path (over the last quarter second) */

/* The speedometer (speedo.h): Speedometer= dial / number / off, SpeedometerPlace= corner / worm, RedFrom= */
static int speedo = 1, speedo_place;    /* 0 off, 1 dial, 2 number; 0 the corner, 1 above the worm */
static double red_from = 800.0;
static double spd_now, spd_disp, spd_best, spd_dist, spd_time;  /* the speed; shown (eased); the turn's best, and how
                                                                  far and how long it moved (the average) */
static double spd_lx, spd_ly, spd_moved;   /* where the worm was last frame; when it last moved (s) */
static int spd_turn = -1, spd_will;     /* spd_will: the speedometer is drawn this frame */
static int spd_cam, spd_cx, spd_cy, spd_ox, spd_oy;   /* WA's camera transform this frame (queue_hook); spd_cam: seen */

/* the land's signature: a new game in the last one's place shows as other land */
static uintptr_t sig_terrain;
static int sig_ok, sig_w, sig_h, sig_tw, sig_th, sig_n;
static uint32_t *sig_tiles;

/* the other modules this one can use (looked up once, at the first GameRender) */
typedef int (__cdecl *d3d_api_fn)(void);
typedef int (__cdecl *d3d_overlay_fn)(const uint32_t *, int, int, int, int, int);
typedef int (__cdecl *d3d_tris_fn)(const wkd3d_vtx *, int);
typedef struct { int size; double zoom; int org_x, org_y, cam_x, cam_y; } wkmz_view;
typedef int (__cdecl *mz_view_fn)(wkmz_view *);
static int looked;
static d3d_overlay_fn gpu_overlay;
static d3d_tris_fn gpu_tris;
static mz_view_fn mz_view;

/* what was drawn this frame, how (0 nothing, 1 wkDirect3D, 2 WA's frame), and where the view came from (for the test
 * log) */
static int dial_way, line_way, view_from;
static long line_count;

static void (__fastcall *original_render)(void *);
void *orig_team, *orig_physics, *orig_queue;   /* (read by the thunks below) */

static void say(const char *fmt, ...) {
    va_list ap;
    SYSTEMTIME t;
    char line[1024];
    int n, m;
    if (!log_file) return;
    GetLocalTime(&t);
    n = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_start(ap, fmt);
    m = vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    if (n + m > (int)sizeof(line) - 2) m = (int)sizeof(line) - 2 - n;
    line[n + m] = '\n';
    fwrite(line, 1, (size_t)(n + m + 1), log_file);
    fflush(log_file);
}

/* ---- the thunks: WA's own conventions <-> C ---------------------------------------------------------------------- */
int __cdecl queue_hook(uint8_t *queue, void *display, void *camera);
void thunk_queue(void);
void thunk_team(void);
void thunk_physics(void);
void __cdecl team_seen(uint32_t task);
void __cdecl worm_seen(uint32_t obj);
int __cdecl call_queue(void *queue, void *display, void *camera);
__asm__(
    ".text\n"
    /* RenderDrawingQueue's entry: EAX the queue, [esp+4] the display, [esp+8] the camera; ret 8 */
    ".globl _thunk_queue\n"
    "_thunk_queue:\n"
    "    pushl 8(%esp)\n"
    "    pushl 8(%esp)\n"
    "    pushl %eax\n"
    "    call _queue_hook\n"
    "    addl $12, %esp\n"
    "    ret $8\n"
    /* the RenderDrawingQueue this hook chains onto (WA's own, or another module's hook) called from C:
     * call_queue(queue, display, camera) */
    ".globl _call_queue\n"
    "_call_queue:\n"
    "    pushl %ebx\n"
    "    pushl %esi\n"
    "    pushl %edi\n"
    "    pushl %ebp\n"
    "    movl 20(%esp), %eax\n"
    "    pushl 28(%esp)\n"
    "    pushl 28(%esp)\n"
    "    call *_orig_queue\n"
    "    popl %ebp\n"
    "    popl %edi\n"
    "    popl %esi\n"
    "    popl %ebx\n"
    "    ret\n"
    /* a team task's message handler: the task noted (observe only: flags, registers and the x87 / SSE state kept) */
    ".globl _thunk_team\n"
    "_thunk_team:\n"
    "    pushfl\n"
    "    pushal\n"
    "    movl %esp, %ebp\n"
    "    subl $544, %esp\n"
    "    andl $-16, %esp\n"
    "    fxsave 16(%esp)\n"
    "    pushl %ecx\n"
    "    call _team_seen\n"
    "    addl $4, %esp\n"
    "    fxrstor 16(%esp)\n"
    "    movl %ebp, %esp\n"
    "    popal\n"
    "    popfl\n"
    "    jmp *_orig_team\n"
    /* an object's physics step: worms noted (observe only, as above; the object is the second stack argument) */
    ".globl _thunk_physics\n"
    "_thunk_physics:\n"
    "    pushfl\n"
    "    pushal\n"
    "    movl %esp, %ebp\n"
    "    subl $544, %esp\n"
    "    andl $-16, %esp\n"
    "    fxsave 16(%esp)\n"
    "    pushl 44(%ebp)\n"
    "    call _worm_seen\n"
    "    addl $4, %esp\n"
    "    fxrstor 16(%esp)\n"
    "    movl %ebp, %esp\n"
    "    popal\n"
    "    popfl\n"
    "    jmp *_orig_physics\n");

/* ---- the teams and worms (ported from wkMapZoom) ------------------------------------------------------------------ */
/* Team tasks are found by their message handler (00557310), worms by their physics step (004FCDF0). A team task: +0x2C
 * the game, +0x38 its team, +0x3C nonzero while it has the turn, +0x48 its current worm. Everything kept from an earlier
 * frame is checked before it is read again (memory, and that it still belongs to this game): pointers left from the
 * previous game crashed WA before. */
static int readable(uintptr_t p, size_t n) {
    MEMORY_BASIC_INFORMATION m;
    return p && VirtualQuery((void *)p, &m, sizeof(m)) && m.State == MEM_COMMIT && !(m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
           p + n >= p && p + n <= (uintptr_t)m.BaseAddress + m.RegionSize;
}

void __cdecl team_seen(uint32_t task) {
    int i;
    if (!worms_ok || !last_world || n_team_tasks >= TRAIL_TEAMS || !readable(task, 0x50) || *(uint32_t *)(task + 0x2c) != (uint32_t)(uintptr_t)last_world)
        return;
    for (i = 0; i < n_team_tasks; i++) if (team_tasks[i] == task) return;
    team_tasks[n_team_tasks] = task;
    team_flags[n_team_tasks] = 0;
    n_team_tasks++;
}

void __cdecl worm_seen(uint32_t obj) {
    int i;
    if (!worms_ok || !obj || *(uint32_t *)obj != VA(WORM_VTABLE_VA) || n_worm_objs >= TRAIL_WORMS) return;
    for (i = 0; i < n_worm_objs; i++) if (worm_objs[i] == obj) return;
    worm_objs[n_worm_objs++] = obj;
}

static void trail_reset(void) {
    int k;
    n_team_tasks = 0;
    n_worm_objs = 0;
    trail_show = -1;
    trail_owner = -1;
    trail_visible = 0;
    trail_turn = 0;
    trail_multi = 0;
    line_slot = -1;
    if (trails) for (k = 0; k < TRAIL_TEAMS * TRAIL_PER_TEAM; k++) trails[k].n = 0;
}

/* a team's current worm and where it is (px); 0 when it isn't known. v (may be NULL): its speed as the game has it
 * (+0x90 / +0x94: its move each step, 1/65536 px; WA steps 50 times a second), in px/s */
static int team_worm(uintptr_t task, double *x, double *y, double *v) {
    const int32_t team = *(int32_t *)(task + 0x38), index = *(int32_t *)(task + 0x48);
    int i;
    for (i = 0; i < n_worm_objs; i++) {
        const uintptr_t o = worm_objs[i];
        if (*(int32_t *)(o + 0xfc) == team && *(int32_t *)(o + 0x100) == index) {
            *x = *(int32_t *)(o + 0x84) / 65536.0;
            *y = *(int32_t *)(o + 0x88) / 65536.0;
            if (v) *v = hypot(*(int32_t *)(o + 0x90) / 65536.0, *(int32_t *)(o + 0x94) / 65536.0) * 50.0;
            return 1;
        }
    }
    return 0;
}

static void trail_add(team_trail *t, double x, double y, int own, double spd) {
    tpoint *e, *p;
    e = t->n ? &t->pt[t->n - 1] : NULL;
    if (e && fabs(x - e->x) < 2.0 && fabs(y - e->y) < 2.0 && e->own == own) return;
    if (t->n == t->cap && t->cap < TRAIL_MAX) {
        int cap = t->cap ? t->cap * 2 : 256;
        tpoint *np = (tpoint *)realloc(t->pt, sizeof(tpoint) * (size_t)(cap > TRAIL_MAX ? TRAIL_MAX : cap));
        if (!np) return;
        t->pt = np;
        t->cap = cap > TRAIL_MAX ? TRAIL_MAX : cap;
        e = t->n ? &t->pt[t->n - 1] : NULL;
    }
    if (!t->cap) return;
    if (t->n == TRAIL_MAX) {                /* (a long game: its oldest quarter dropped) */
        memmove(t->pt, t->pt + TRAIL_MAX / 4, sizeof(tpoint) * (TRAIL_MAX - TRAIL_MAX / 4));
        t->n -= TRAIL_MAX / 4;
        t->pt[0].brk = 1;
        e = &t->pt[t->n - 1];
    }
    p = &t->pt[t->n++];
    p->x = (float)x; p->y = (float)y;
    p->turn = trail_turn;
    p->brk = (int8_t)(!e || fabs(x - e->x) > 80.0 || fabs(y - e->y) > 80.0);
    p->own = (int8_t)own;
    p->spd = (uint16_t)(spd < 0.0 ? 0.0 : spd > 65535.0 ? 65535.0 : spd);
}

/* each frame, before WA's own drawing: the worms still there, turns starting and ending, the turn's worm found, every
 * worm's place noted (the line), the shown line hidden once the worm moves */
static void trail_frame(void *world) {
    int i, flagged = 0;
    uintptr_t owner_task = 0;
    double x, y;
    turn_worm_ok = 0;
    line_slot = -1;
    if (!worms_ok || !trails) return;
    for (i = 0; i < n_worm_objs;) {
        const uintptr_t o = worm_objs[i];
        if (!readable(o, 0x104) || *(uint32_t *)o != VA(WORM_VTABLE_VA) || *(uint32_t *)(o + 0x2c) != (uint32_t)(uintptr_t)world)
            worm_objs[i] = worm_objs[--n_worm_objs];
        else
            i++;
    }
    for (i = 0; i < n_team_tasks;) {
        const uintptr_t t = team_tasks[i];
        int team, flag;
        if (!readable(t, 0x50) || *(uint32_t *)(t + 0x2c) != (uint32_t)(uintptr_t)world) {
            team_tasks[i] = team_tasks[--n_team_tasks];
            team_flags[i] = team_flags[n_team_tasks];
            continue;
        }
        team = *(int32_t *)(t + 0x38);
        flag = *(int32_t *)(t + 0x3c) != 0;
        if (team >= 0 && team < TRAIL_TEAMS) {
            if (flag && !team_flags[i]) {           /* its turn starts: its worm's line shown (the worm found below) */
                trail_turn++;
                trail_owner = team;
                trail_visible = 1;
                trail_show = -1;
            } else if (!flag && team_flags[i] && team == trail_owner) {
                trail_visible = 0;
                trail_show = -1;
            }
            if (flag) flagged++;
            if (team == trail_owner) owner_task = t;
        }
        team_flags[i] = flag;
        i++;
    }
    /* (wkRealTime: several teams have the turn at once; there is no one turn's worm, so nothing is shown) */
    if (flagged > 1 && !trail_multi) { trail_multi = 1; say("several teams have the turn at once (real-time play?): no speedometer or movement line"); }
    if (trail_multi) return;
    if (trail_owner >= 0 && owner_task && team_worm(owner_task, &turn_wx, &turn_wy, &turn_gv)) {
        turn_worm_ok = 1;
        turn_key = trail_owner * 64 + (*(int32_t *)(owner_task + 0x48) & 63);
    }
    if (!trail_on) return;
    {   /* every worm, every frame (knocked about in other turns too: its line ends where it is); "own" while it is the
         * current worm of the team that has (or last had) the turn */
        const int owner_index = owner_task ? *(int32_t *)(owner_task + 0x48) : -1;
        for (i = 0; i < n_worm_objs; i++) {
            const uintptr_t o = worm_objs[i];
            const int team = *(int32_t *)(o + 0xfc), index = *(int32_t *)(o + 0x100);
            if (team < 0 || team >= TRAIL_TEAMS || index < 0 || index >= TRAIL_PER_TEAM) continue;
            trail_add(&trails[team * TRAIL_PER_TEAM + index], *(int32_t *)(o + 0x84) / 65536.0, *(int32_t *)(o + 0x88) / 65536.0,
                      team == trail_owner && index == owner_index,
                      hypot(*(int32_t *)(o + 0x90) / 65536.0, *(int32_t *)(o + 0x94) / 65536.0) * 50.0);
        }
    }
    if (trail_owner < 0 || !owner_task || !team_worm(owner_task, &x, &y, NULL)) return;
    {
        const int index = *(int32_t *)(owner_task + 0x48), slot = trail_owner * TRAIL_PER_TEAM + index;
        team_trail *tr;
        if (index < 0 || index >= TRAIL_PER_TEAM) return;
        if (test_line) { line_slot = slot; return; }   /* (the test switch: shown all the time) */
        tr = &trails[slot];
        if (!trail_visible) return;
        if (slot != trail_show) {               /* the turn's worm: its last own turn bright */
            int k;
            trail_show = slot;
            trail_have0 = 0;
            for (trail_hl = 0, k = tr->n - 1; k >= 0 && !trail_hl; k--) if (tr->pt[k].own && tr->pt[k].turn < trail_turn) trail_hl = tr->pt[k].turn;
            if (trail_hl) {
                int n_hl = 0;
                for (k = 0; k < tr->n; k++) n_hl += tr->pt[k].own && tr->pt[k].turn == trail_hl;
                say("turn %d: team %d's worm %d: its line shown (%d points; its last own turn, turn %d, bright: %d of them)",
                    trail_turn, trail_owner, index, tr->n, trail_hl, n_hl);
            }
        }
        if (!trail_have0) { trail_x0 = x; trail_y0 = y; trail_have0 = 1; }     /* hidden once the worm moves (unless
                                                                                     TurnLineKeep: at the turn's end) */
        else if (trail_keep && trail_hl && trail_have0 == 1 && (fabs(x - trail_x0) > 6.0 || fabs(y - trail_y0) > 6.0)) {
            trail_have0 = 2;                    /* (once a showing) */
            say("team %d's worm %d moved: its line stays (TurnLineKeep)", trail_owner, index);
        } else if (!trail_keep && (fabs(x - trail_x0) > 6.0 || fabs(y - trail_y0) > 6.0)) {
            if (trail_hl) say("team %d's worm %d moved: its line hidden", trail_owner, index);
            trail_visible = 0;
            trail_show = -1;
        }
    }
}

/* ---- the land's signature ------------------------------------------------------------------------------------------ */
/* WA can start a new game with its world, display and terrain at the very addresses of the last one (in wkMapZoom this
 * was a crash: round 5 of a match taken for round 4). So, as wkMapZoom does, the land is compared each frame: its sizes,
 * its terrain object and its tile bitmaps. 0 the same; 1 other tiles (other land); 2 other sizes, or gone */
static int land_read(void *display, uintptr_t *terrain, uint32_t **arr, int *w, int *h, int *tw, int *th, int *n) {
    *terrain = *(uintptr_t *)((char *)display + D_TERRAIN);
    *w = FIELD(display, D_LAND_W); *h = FIELD(display, D_LAND_H); *tw = FIELD(display, D_TILE_W); *th = FIELD(display, D_TILE_H);
    if (!*terrain || !readable(*terrain, 8)) return 0;
    *arr = *(uint32_t **)(*terrain + 4);
    if (!*arr || *w <= 0 || *h <= 0 || *tw <= 0 || *th <= 0 || *w > 0x8000 || *h > 0x8000) return 0;
    *n = ((*w + *tw - 1) / *tw) * ((*h + *th - 1) / *th);
    return *n > 0 && readable((uintptr_t)*arr, (size_t)*n * 4);
}

static void land_take(uintptr_t terrain, const uint32_t *arr, int w, int h, int tw, int th, int n) {
    if (n > sig_n || !sig_tiles) {
        uint32_t *t = (uint32_t *)realloc(sig_tiles, sizeof(uint32_t) * (size_t)n);
        if (!t) { sig_ok = 0; return; }
        sig_tiles = t;
    }
    memcpy(sig_tiles, arr, sizeof(uint32_t) * (size_t)n);
    sig_terrain = terrain; sig_w = w; sig_h = h; sig_tw = tw; sig_th = th; sig_n = n;
    sig_ok = 1;
}

static int land_check(void *display) {
    uintptr_t terrain = 0;
    uint32_t *arr = NULL;
    int w = 0, h = 0, tw = 0, th = 0, n = 0;
    const int ok = land_read(display, &terrain, &arr, &w, &h, &tw, &th, &n);
    if (!sig_ok) { if (ok) land_take(terrain, arr, w, h, tw, th, n); return 0; }
    if (!ok) { sig_ok = 0; return 2; }
    if (w != sig_w || h != sig_h || tw != sig_tw || th != sig_th) { land_take(terrain, arr, w, h, tw, th, n); return 2; }
    if (terrain == sig_terrain && n == sig_n && !memcmp(arr, sig_tiles, sizeof(uint32_t) * (size_t)n)) return 0;
    land_take(terrain, arr, w, h, tw, th, n);
    return 1;
}

/* ---- the speed (ported from wkMapZoom's auto_zoom_frame: its speed part) -------------------------------------------- */
/* The speed as the game has it; when that is about nothing (walking), the speed from the worm's path over the last
 * quarter second. Shown eased (0.06 s); the turn's best; its average: the way it went over the time it was moving. */
static void speed_frame(void) {
    LARGE_INTEGER now;
    double dt;
    int track;
    QueryPerformanceCounter(&now);
    dt = spd_clock.QuadPart && clock_freq.QuadPart ? (double)(now.QuadPart - spd_clock.QuadPart) / clock_freq.QuadPart : 0.0;
    spd_clock = now;
    if (dt > 0.25) dt = 0.25;
    /* the track in a loose box: high above the level is fine; not WA's far-off place for a worm not placed yet (y -10000) */
    track = turn_worm_ok && sig_ok && turn_wx > -4000.0 && turn_wx < sig_w + 4000.0 && turn_wy > -8000.0 &&
            turn_wy < sig_h + 4000.0;
    if (speedo && track && clock_freq.QuadPart) {
        const double ts = (double)now.QuadPart / clock_freq.QuadPart;
        const int last = (trk_i + TRACK_N - 1) % TRACK_N;
        double vx = 0.0, vy = 0.0;
        int k, j = -1;
        if (turn_key != track_key || trail_turn != track_turn) {   /* another worm or turn: a new track, from rest */
            track_key = turn_key; track_turn = trail_turn;
            trk_n = 0;
            spd_disp = 0.0;
            spd_lx = turn_wx; spd_ly = turn_wy; spd_moved = 0.0;
        }
        if (trk_n && hypot(turn_wx - trk_x[last], turn_wy - trk_y[last]) > 300.0) trk_n = 0;   /* (a teleport) */
        trk_x[trk_i] = turn_wx; trk_y[trk_i] = turn_wy; trk_t[trk_i] = ts;
        trk_i = (trk_i + 1) % TRACK_N;
        if (trk_n < TRACK_N) trk_n++;
        for (k = 1; k < trk_n; k++) {               /* the speed: over the last quarter second */
            const int s = (trk_i + TRACK_N - 1 - k) % TRACK_N;
            if (ts - trk_t[s] > 0.25) break;
            j = s;
        }
        if (j >= 0 && ts - trk_t[j] > 0.05) {
            vx = (turn_wx - trk_x[j]) / (ts - trk_t[j]);
            vy = (turn_wy - trk_y[j]) / (ts - trk_t[j]);
        }
        spd_from_path = hypot(vx, vy);
        spd_now = turn_gv > 1.0 ? turn_gv : spd_from_path;
        spd_disp = spd_disp + (spd_now - spd_disp) * (1.0 - exp(-dt / 0.06));
        if (trail_turn != spd_turn) { spd_turn = trail_turn; spd_best = spd_dist = spd_time = 0.0; }
        if (spd_now > spd_best) spd_best = spd_now;
        {   /* the average: the way it went over the time it was moving (a frozen game, a paused replay or an online
             * game waiting, adds nothing: nothing moves) */
            const double step_ = hypot(turn_wx - spd_lx, turn_wy - spd_ly);
            const int moved = step_ > 0.0 && step_ < 300.0;
            if (moved) spd_moved = ts;
            if (spd_moved > 0.0 && ts - spd_moved < 0.1 && spd_now > 30.0) {   /* (the way and the time on the same
                                                                                   frames) */
                spd_time += dt;
                if (moved) spd_dist += step_;
            }
            spd_lx = turn_wx; spd_ly = turn_wy;
        }
    } else {
        spd_now = spd_disp = spd_from_path = 0.0;
        trk_n = 0;
    }
}

/* ---- the other modules ------------------------------------------------------------------------------------------ */
typedef BOOL (WINAPI *handle_ex_fn)(DWORD, LPCSTR, HMODULE *);   /* (GetModuleHandleExA: behind _WIN32_WINNT >= XP in
                                                                    this MinGW's headers; flags PIN 1, UNCHANGED 2,
                                                                    FROM_ADDRESS 4) */
static handle_ex_fn handle_ex(void) {
    return (handle_ex_fn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetModuleHandleExA");
}

/* Pins the module that holds addr (another WormKit module whose hook ours chains onto, or whose functions we call: if
 * it were unloaded, our call into it would land in freed memory); WA itself, Windows' own DLLs and this module are left
 * alone. */
static void pin_owner(const void *addr, const char *what) {
    const handle_ex_fn ex = handle_ex();
    HMODULE m = NULL, pinned;
    char name[MAX_PATH] = "", win[MAX_PATH] = "";
    UINT wn;
    if (!ex || !addr || !ex(0x2 | 0x4, (LPCSTR)addr, &m) || !m) return;
    if ((uintptr_t)m == base || m == self || !GetModuleFileNameA(m, name, sizeof(name))) return;
    wn = GetSystemWindowsDirectoryA(win, sizeof(win));
    if (wn && wn < sizeof(win)) {           /* (Windows' own DLLs stay loaded anyway) */
        UINT k;
        for (k = 0; k < wn && name[k] && (name[k] | 0x20) == (win[k] | 0x20); k++) {}
        if (k == wn) return;
    }
    if (ex(0x1 | 0x4, (LPCSTR)addr, &pinned)) say("%s: in %s, pinned too, so it stays loaded", what, name);
}

/* wkDirect3D (the graphics card draws the line and the dial) and wkMapZoom (where the map is on the screen when it is
 * zoomed): both optional, both looked up once */
static void look_up_others(void) {
    HMODULE m;
    looked = 1;
    if (no_gpu) say("WKSPEEDO_NO_GPU: wkDirect3D not used");
    else if ((m = GetModuleHandleA("wkDirect3D.dll")) != NULL) {
        const d3d_api_fn api = (d3d_api_fn)(void *)GetProcAddress(m, "wkDirect3D_Api");
        const d3d_overlay_fn ov = (d3d_overlay_fn)(void *)GetProcAddress(m, "wkDirect3D_Overlay");
        const d3d_tris_fn tr = (d3d_tris_fn)(void *)GetProcAddress(m, "wkDirect3D_Triangles");
        const int level = api ? api() : 0;
        if (level >= 1) {
            gpu_overlay = ov;
            gpu_tris = tr;
            pin_owner((const void *)api, "wkDirect3D's drawing functions");
        }
        say("wkDirect3D is loaded: api %d, overlay %s, triangles %s", level, gpu_overlay ? "used" : "not there",
            gpu_tris ? "used" : "not there");
    } else say("wkDirect3D is not loaded: everything is drawn into WA's frame");
    if ((m = GetModuleHandleA("wkMapZoom.dll")) != NULL) {
        mz_view = (mz_view_fn)(void *)GetProcAddress(m, "wkMapZoom_View");
        if (mz_view) pin_owner((const void *)mz_view, "wkMapZoom's view");
        say("wkMapZoom is loaded: %s", mz_view ? "its view is used (the line and the dial above the worm follow its zoom)"
                                               : "this version doesn't tell its zoom (the line is right at 100% only)");
    }
}

/* ---- drawing ----------------------------------------------------------------------------------------------------- */
/* WA's palette as shown, and the tables made from it (made again when it changes; only when WA's frame is drawn into) */
static uint8_t pal_seen[1024], lut12[4096], dark_lut[256], line_idx[256];
static uint32_t line_lut[256];          /* the line's colours by speed index (0xRRGGBB) */
static unsigned pal_gen, dial_gen, idx_gen;
static int pal_have;

static void palette_frame(void) {
    const uint8_t *pal = (const uint8_t *)VA(SHOWN_PALETTE_VA);
    if (pal_have && !memcmp(pal, pal_seen, sizeof(pal_seen))) return;
    memcpy(pal_seen, pal, sizeof(pal_seen));
    pal_have = 1;
    if (++pal_gen == 0) pal_gen = 1;
}

/* WA's 8-bit frame: its pixels, width (= row stride) and height; 0 when there is none */
static int wa_frame(uint8_t **px, int *stride, int *h) {
    *px = *(uint8_t **)VA(FRAME_PTR_VA);
    *stride = *(int32_t *)VA(FRAME_W_VA);
    *h = *(int32_t *)VA(FRAME_H_VA);
    return *px && *stride >= 64 && *h >= 64 && *stride <= 16384 && *h <= 16384;
}

/* the shown movement line: its segments on the screen, through wkDirect3D as triangles, else into WA's frame */
#define MAX_VTX 190000
static line_seg *segs;
static int cap_segs;
static wkd3d_vtx *vtxs;
static void draw_line(const line_view *v) {
    const team_trail *tr;
    int slot, all, n;
    int32_t limit, hl;
    uint8_t *frame;
    int stride, fh;
    line_way = 0; line_count = 0;
    if (!trails) return;
    if (test_line) {                        /* (all the time, this turn's own points too, bright) */
        if (line_slot < 0 || !trail_turn) return;
        slot = line_slot; limit = INT32_MAX; hl = trail_turn; all = 1;
    } else {
        if (!trail_on || !trail_visible || trail_show < 0 || !trail_hl) return;
        slot = trail_show; limit = trail_turn; hl = trail_hl; all = trail_all;
    }
    tr = &trails[slot];
    if (tr->n < 2) return;
    if (tr->n > cap_segs) {
        line_seg *s = (line_seg *)realloc(segs, sizeof(line_seg) * (size_t)tr->n);
        if (!s) return;
        segs = s; cap_segs = tr->n;
    }
    n = line_segments(tr->pt, tr->n, limit, hl, all, v, segs);
    if (!n) return;
    if (gpu_tris) {
        if (!vtxs) vtxs = (wkd3d_vtx *)malloc(sizeof(wkd3d_vtx) * MAX_VTX);
        if (vtxs) {
            const int k = line_triangles(segs, n, line_lut, vtxs, MAX_VTX);
            if (k > 0 && gpu_tris(vtxs, k)) { line_way = 1; line_count = k; return; }
        }
    }
    if (!wa_frame(&frame, &stride, &fh)) return;
    palette_frame();
    if (idx_gen != pal_gen) {
        int k;
        for (k = 0; k < 256; k++)
            line_idx[k] = (uint8_t)pal_nearest(pal_seen, (int)(line_lut[k] >> 16), (int)((line_lut[k] >> 8) & 255u), (int)(line_lut[k] & 255u));
        idx_gen = pal_gen;
    }
    line_count = line_frame(segs, n, line_idx, frame, stride, stride, fh);
    line_way = 2;
}

/* The speedometer (speedo.h): the dial or the number for the turn's worm, in the screen's bottom right corner (above
 * WA's wind bar) or just above the worm's name (WA draws the name and the energy above the worm, zoomed with the map:
 * the picture's bottom SPEEDO_UP map pixels above the worm's middle, as shown, and 2 screen pixels more); made again
 * when what it shows changes; not over WA's Esc menu. */
#define SPEEDO_UP 44.0
static speedo_dial spd_dial;
static speedo_number spd_numpic;
static uint32_t *spd_img;
static int spd_cap, spd_key[7] = {-1};
static double spd_digits, spd_avg;      /* the digits' speed: taken 10 times a second (as digital gauges do); the
                                           average shown, in tens, twice a second */
static DWORD spd_digits_tick, spd_avg_tick;

static int speedo_will(void) {          /* (before WA draws the frame) */
    return speedo && worms_ok && turn_worm_ok && trk_n && !menu_open;
}

static void draw_speedo(const line_view *v, int fw, int fh) {
    const double avg = spd_time > 0.5 ? (spd_dist / spd_time < spd_best ? spd_dist / spd_time : spd_best) : 0.0;
    const int dial = speedo == 1, size = dial ? (fh * 120 / 1080 < 110 ? 110 : fh * 120 / 1080) : (fh * 34 / 1080 < 22 ? 22 : fh * 34 / 1080);
    int w, h, x0, y0, key[7], stride, frame_h;
    uint8_t *frame;
    dial_way = 0;
    if (!spd_will || fw < 64 || fh < 64) return;
    if (dial) { w = h = size; } else speedo_number_size(size, &w, &h);
    if (GetTickCount() - spd_digits_tick >= 100) { spd_digits = spd_disp; spd_digits_tick = GetTickCount(); }
    if (GetTickCount() - spd_avg_tick >= 500 || (avg > 0.0) != (spd_avg > 0.0)) {
        spd_avg = avg > 0.0 ? floor(avg / 10.0 + 0.5) * 10.0 : 0.0;
        spd_avg_tick = GetTickCount();
    }
    key[0] = (int)(spd_digits + 0.5);
    key[1] = dial ? (int)ceil(spd_disp / (SPEEDO_VMAX / SPEEDO_SEGS)) : 0;   /* (the ring: how many segments lit) */
    key[2] = dial ? (int)(spd_best / (SPEEDO_VMAX / SPEEDO_SEGS)) : 0;
    key[3] = dial ? (int)spd_avg : 0;
    key[4] = speedo; key[5] = size; key[6] = (int)red_from;
    if (memcmp(key, spd_key, sizeof(key)) || !spd_img) {   /* the picture, made again when what it shows changes */
        if (w * h > spd_cap) {
            free(spd_img);
            spd_img = (uint32_t *)malloc((size_t)w * h * 4);
            spd_cap = spd_img ? w * h : 0;
            if (!spd_img) return;
        }
        if (dial ? !speedo_dial_paint(&spd_dial, size, red_from, spd_disp, key[0], spd_best, spd_avg, spd_img, w * 4)
                 : !speedo_number_paint(&spd_numpic, size, key[0], red_from, spd_img, w * 4)) { spd_key[0] = -1; return; }
        memcpy(spd_key, key, sizeof(key));
    }
    if (speedo_place == 1) {                /* above the worm's name */
        if (!v) return;
        x0 = (int)floor(v->org_x + (turn_wx - v->cam_x) * v->zoom - w / 2.0 + 0.5);
        y0 = (int)floor(v->org_y + (turn_wy - SPEEDO_UP - v->cam_y) * v->zoom + 0.5) - 2 - h;
    } else {                                /* the bottom right corner */
        x0 = fw - w - fh * 14 / 1080;
        y0 = fh - h - 30 - fh * 8 / 1080;   /* (WA's wind bar: about 30 px at the bottom, at any size) */
    }
    if (x0 + w <= 0 || x0 >= fw || y0 + h <= 0 || y0 >= fh) return;
    if (gpu_overlay && gpu_overlay(spd_img, w, h, w * 4, x0, y0)) { dial_way = 1; return; }
    if (!wa_frame(&frame, &stride, &frame_h)) return;
    palette_frame();                        /* WA's own frame: its palette's nearest colours; the shadow darkens */
    if (dial_gen != pal_gen) { pal_tables(pal_seen, lut12, dark_lut); dial_gen = pal_gen; }
    speedo_frame(spd_img, w, h, x0, y0, frame, stride, stride, frame_h, lut12, dark_lut);
    dial_way = 2;
}

/* everything, right after WA's own RenderDrawingQueue has returned for the game's display: the line, then the dial */
static void draw_all(void *display) {
    line_view v;
    int have_view = 0, sw_ = FIELD(display, D_W), sh_ = FIELD(display, D_H);
    dial_way = line_way = 0; line_count = 0; view_from = 0;
    if (menu_open) return;
    if (sw_ < 64 || sh_ < 64 || sw_ > 16384 || sh_ > 16384) {   /* (the display's size unlikely: the frame's instead) */
        uint8_t *f;
        int stride, h;
        if (!wa_frame(&f, &stride, &h)) return;
        sw_ = stride; sh_ = h;
    }
    memset(&v, 0, sizeof(v));
    if (mz_view) {                          /* wkMapZoom's view (its zoom) */
        wkmz_view m;
        memset(&m, 0, sizeof(m));
        m.size = (int)sizeof(m);
        if (mz_view(&m) == 1 && m.zoom > 0.01 && m.zoom < 100.0) {
            v.zoom = m.zoom; v.org_x = m.org_x; v.org_y = m.org_y; v.cam_x = m.cam_x; v.cam_y = m.cam_y;
            have_view = 1; view_from = 2;
        }
    }
    if (!have_view && spd_cam) {            /* WA's camera at 100% */
        v.zoom = 1.0; v.org_x = spd_ox; v.org_y = spd_oy; v.cam_x = spd_cx; v.cam_y = spd_cy;
        have_view = 1; view_from = 1;
    }
    v.w = sw_; v.h = sh_;
    if (have_view) draw_line(&v);
    draw_speedo(have_view ? &v : NULL, sw_, sh_);
}

/* ---- the hooks --------------------------------------------------------------------------------------------------- */
int __cdecl queue_hook(uint8_t *queue, void *display, void *camera) {
    const int ours = display && display == frame_display && !frame_drawn;
    int r;
    if (ours && camera) {                   /* WA's camera transform this frame: screen = origin + (world - camera) */
        spd_cx = ((const int32_t *)camera)[0] >> 16; spd_cy = ((const int32_t *)camera)[1] >> 16;
        spd_ox = FIELD(display, D_ORG); spd_oy = FIELD(display, D_ORG + 4);
        spd_cam = 1;
    }
    r = call_queue(queue, display, camera);
    if (ours) { frame_drawn = 1; draw_all(display); }
    return r;
}

static void test_log(void) {
    static DWORD last;
    static const char *const ways[3] = {"none", "wkDirect3D", "WA's frame"};
    static const char *const views[3] = {"none", "WA's camera", "wkMapZoom"};
    if (GetTickCount() - last < 1000) return;
    last = GetTickCount();
    say("test: frame %u: turn's worm %s at %.0f,%.0f; speed %.0f (game %.0f, path %.0f), shown %.0f, best %.0f; dial: %s; "
        "line: %s, %ld %s; view: %s", frames, turn_worm_ok ? "known" : "not known", turn_wx, turn_wy, spd_now, turn_gv,
        spd_from_path, spd_disp, spd_best, ways[dial_way], ways[line_way], line_count,
        line_way == 1 ? "vertices" : "pixels", views[view_from]);
}

static void __fastcall hook_render(void *runtime) {
    void *world = runtime ? *(void **)((char *)runtime + RT_WORLD) : NULL;
    void *display = world ? *(void **)((char *)world + WO_DISPLAY) : NULL;
    if (!looked) look_up_others();
    frame_display = NULL;
    frame_drawn = 0;
    if (display && (uintptr_t)VTBL(display) == VA(DISPLAY_VTABLE_VA)) {
        const int land_changed = land_check(display);
        const DWORD gap = GetTickCount() - live_tick;   /* (since the last game frame: the lobby between games) */
        if (world != last_world || land_changed == 2 || (land_changed && gap > 1500)) {   /* a new game (also when WA
                                                           put it where the last one was: other land after a pause) */
            if (world == last_world) say("a new game in the last one's place (the land %s after %lu ms without game frames)",
                                         land_changed == 2 ? "has other sizes" : "is made of other tiles", (unsigned long)gap);
            last_world = world;
            frames = 0;
            trail_reset();
            spd_turn = -1; spd_best = spd_dist = spd_time = 0.0; track_key = track_turn = -1;
            trk_n = 0;
            say("a new game (world %p)", world);
        }
        trail_frame(world);
        speed_frame();
        frame_display = display;
        live_tick = GetTickCount();
    }
    menu_open = runtime && *(uint32_t *)((char *)runtime + RT_ESC_MENU) == 1;
    spd_cam = 0;
    spd_will = frame_display && speedo_will();
    original_render(runtime);
    if (frame_display) {
        frames++;
        if (test_mode) test_log();
    }
    frame_display = NULL;
}

/* ---- start-up ---------------------------------------------------------------------------------------------------- */
static int ini_flag(const char *ini, const char *key, int dflt) {
    char v[16];
    if (!GetPrivateProfileStringA("wkSpeedometer", key, "", v, sizeof(v), ini) || !v[0]) return dflt;
    return atoi(v) != 0 || !lstrcmpiA(v, "on") || !lstrcmpiA(v, "yes") || !lstrcmpiA(v, "true");
}

static void read_settings(const char *exe) {
    char ini[MAX_PATH], v[64], *slash;
    if (strlen(exe) + 20 >= sizeof(ini)) return;
    strcpy(ini, exe);
    slash = strrchr(ini, '\\');
    if (!slash) return;
    strcpy(slash + 1, "wkSpeedometer.ini");
    enabled = ini_flag(ini, "Enabled", 1);
    if (GetPrivateProfileStringA("wkSpeedometer", "Speedometer", "", v, sizeof(v), ini) && v[0])
        speedo = !lstrcmpiA(v, "off") || !strcmp(v, "0") ? 0 : !lstrcmpiA(v, "number") || !strcmp(v, "2") ? 2 : 1;
    if (GetPrivateProfileStringA("wkSpeedometer", "SpeedometerPlace", "", v, sizeof(v), ini) && v[0])
        speedo_place = !lstrcmpiA(v, "worm") || !lstrcmpiA(v, "above") || !strcmp(v, "1");
    if (GetPrivateProfileStringA("wkSpeedometer", "RedFrom", "", v, sizeof(v), ini) && v[0]) {
        const double d = atof(v);
        if (d >= 50.0 && d <= 5000.0) red_from = d;
    }
    if (GetPrivateProfileStringA("wkSpeedometer", "TurnLine", "", v, sizeof(v), ini) && v[0]) {
        trail_on = lstrcmpiA(v, "off") && strcmp(v, "0");
        trail_all = lstrcmpiA(v, "last") != 0;
    }
    trail_keep = ini_flag(ini, "TurnLineKeep", 0);
    if (GetPrivateProfileStringA("wkSpeedometer", "TurnLineColour", "", v, sizeof(v), ini) && v[0])
        trail_colour = lstrcmpiA(v, "red") && strcmp(v, "0");
}

static int runs_without_screen(void) {
    const char *line = GetCommandLineA();
    static const char *const switches[] = {"/getvideo", "/getlog", "/getmap", "/getscheme", "/sanitise", "/sanitize", "/repair"};
    unsigned i;
    for (i = 0; i < sizeof(switches) / sizeof(switches[0]); i++)
        if (line && strstr(line, switches[i])) return 1;
    return 0;
}

static void open_log(void) {
    char path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    DWORD k = GetEnvironmentVariableA("WKSPEEDO_LOG", path, sizeof(path));
    if (!k || k >= sizeof(path)) {
        k = GetEnvironmentVariableA("LOCALAPPDATA", path, sizeof(path) - 40);
        if (!k || k >= sizeof(path) - 40) return;
        strcat(path, "\\wkSpeedometer");
        CreateDirectoryA(path, NULL);
        strcat(path, "\\wkSpeedometer.log");
    }
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > (2u << 20)) DeleteFileA(path);
    log_file = fopen(path, "a");
}

typedef struct { uint32_t va; const unsigned char *bytes; unsigned n; void *detour; void **orig; const char *name; } hook_def;

static void install(void) {
    static const unsigned char render_bytes[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x83, 0xec, 0x20};
    static const unsigned char queue_bytes[] = {0x83, 0xec, 0x1c, 0x53, 0x55, 0x8b, 0x6c, 0x24, 0x28};
    static const unsigned char team_bytes[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00};
    static const unsigned char physics_bytes[] = {0x83, 0xec, 0x08, 0x53, 0x8b, 0x5c, 0x24, 0x14};
    const hook_def hooks[] = {
        {GAME_RENDER_VA, render_bytes, sizeof(render_bytes), (void *)hook_render, (void **)&original_render, "GameRender"},
        {QUEUE_VA, queue_bytes, sizeof(queue_bytes), (void *)thunk_queue, &orig_queue, "RenderDrawingQueue"},
        {TEAM_HANDLER_VA, team_bytes, sizeof(team_bytes), (void *)thunk_team, &orig_team, "the team tasks' handler"},
        {PHYSICS_VA, physics_bytes, sizeof(physics_bytes), (void *)thunk_physics, &orig_physics, "the physics step"},
    };
    const unsigned nhooks = sizeof(hooks) / sizeof(hooks[0]);
    char exe[MAX_PATH], name[64];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    MH_STATUS s = MH_OK;
    unsigned i;
    open_log();
    {   /* Steam's WA.exe 3.8.1, or another with the very same code and layout (GOG's): the hook points are checked too */
        char why[256] = "WA.exe's path unknown";
        const int kind = n && n < sizeof(exe) ? wa_supported_image(exe, why, sizeof(why)) : 0;
        if (!kind) { say("not the WA 3.8.1 this module knows (%s): not installed", why); return; }
        if (kind == 2) say("%s: not Steam's file, but the same game (GOG's edition?)", why);
    }
    if (runs_without_screen()) { say("a /get... or repair run: not installed"); return; }
    base = (uintptr_t)GetModuleHandleA(NULL);
    read_settings(exe);
    if (!enabled) { say("Enabled=0 in wkSpeedometer.ini: not installed"); return; }
    {   /* one copy a process (renamed copies too): the first one loaded wins */
        HANDLE mx;
        snprintf(name, sizeof(name), "wkSpeedometer-loaded-%lu", (unsigned long)GetCurrentProcessId());
        mx = CreateMutexA(NULL, FALSE, name);
        if (mx && GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(mx);
            say("another copy of wkSpeedometer is loaded already: this one does nothing (keep one copy in the WA folder)");
            return;
        }
        if (!mx) say("could not make the one-copy check (error %lu): going on", GetLastError());
        /* (the handle is kept open until WA exits) */
    }
    for (i = 0; i < nhooks; i++) {          /* WA's own code, or another module's jump into live code */
        const uint8_t *at = (const uint8_t *)VA(hooks[i].va);
        if (*at == 0xe9) {
            MEMORY_BASIC_INFORMATION mi;
            const uint8_t *to = at + 5 + *(const int32_t *)(at + 1);
            if (!VirtualQuery(to, &mi, sizeof(mi)) || mi.State != MEM_COMMIT ||
                !(mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
                say("WA's %s at %08X jumps into a module that is no longer loaded: not installed (WA will crash there: "
                    "restart WA)", hooks[i].name, hooks[i].va);
                return;
            }
        } else if (memcmp(at, hooks[i].bytes, hooks[i].n)) {
            say("unexpected code at %08X (%s): not installed", hooks[i].va, hooks[i].name);
            return;
        }
    }
    QueryPerformanceFrequency(&clock_freq);
    test_mode = GetEnvironmentVariableA("WKSPEEDO_TEST", NULL, 0) > 0;
    test_line = GetEnvironmentVariableA("WKSPEEDO_TEST_LINE", NULL, 0) > 0;
    no_gpu = GetEnvironmentVariableA("WKSPEEDO_NO_GPU", NULL, 0) > 0;
    if (test_line) { trail_on = 1; trail_all = 1; say("WKSPEEDO_TEST_LINE: the line shown all the time, as TurnLine=all"); }
    if (test_mode) say("WKSPEEDO_TEST: a log line a second");
    line_colours(line_lut, trail_colour, red_from);
    trails = (team_trail *)calloc(TRAIL_TEAMS * TRAIL_PER_TEAM, sizeof(team_trail));
    if (!trails) { say("no memory: not installed"); return; }
    {   /* Pinned before any of WA's code points into this DLL, so it is never unloaded while WA runs (an unloaded
         * module with its hooks still in WA crashed WA in wkMapZoom's past). Not pinned, it doesn't install. */
        const handle_ex_fn ex = handle_ex();
        HMODULE me;
        if (!ex || !ex(0x1 | 0x4, (LPCSTR)(void *)&install, &me)) {
            say("could not pin the module in memory (error %lu): not installed", GetLastError());
            return;
        }
    }
    s = MH_Initialize();
    if (s != MH_OK) say("MinHook could not start (%d)", (int)s);
    for (i = 0; s == MH_OK && i < nhooks; i++)
        if ((s = MH_CreateHook((void *)VA(hooks[i].va), hooks[i].detour, hooks[i].orig)) != MH_OK)
            say("could not hook %s (%d)", hooks[i].name, (int)s);
    if (s != MH_OK) {
        MH_Uninitialize();                  /* (the hooks made so far: never switched on, taken away) */
        say("wkSpeedometer %s: not installed", WKSPEEDO_VERSION);
        return;
    }
    for (i = 0; i < nhooks; i++) {          /* (another module's hook already there: ours chains onto it, so it is kept
                                               loaded) */
        const uint8_t *at = (const uint8_t *)VA(hooks[i].va);
        if (*at == 0xe9) pin_owner(at + 5 + *(const int32_t *)(at + 1), hooks[i].name);
    }
    worms_ok = 1;
    s = MH_EnableHook(MH_ALL_HOOKS);
    if (s != MH_OK) { worms_ok = 0; say("could not switch the hooks on (%d)", (int)s); }
    installed = s == MH_OK;
    say("wkSpeedometer %s %s (base %08x): speedometer %s (%s), red from %.0f px/s; movement line %s%s%s",
        WKSPEEDO_VERSION, installed ? "ready" : "failed", (unsigned)base,
        speedo == 1 ? "dial" : speedo == 2 ? "number" : "off", speedo_place ? "above the worm" : "bottom right corner",
        red_from, trail_on ? (trail_all ? "on (whole game)" : "on (last turn)") : "off",
        trail_on && trail_keep ? ", stays all turn" : "", trail_on ? (trail_colour ? ", coloured by speed" : ", red") : "");
    {   /* (which file this is, and where it lies: two copies, or one unloaded, show in the log) */
        char me[MAX_PATH] = "";
        GetModuleFileNameA(self, me, sizeof(me));
        say("this module: %s at %08x, pinned (it stays loaded until WA exits)", me, (unsigned)(uintptr_t)self);
    }
}

BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) { self = dll; DisableThreadLibraryCalls(dll); install(); }
    else if (reason == DLL_PROCESS_DETACH) {   /* (pinned: only at WA's exit, the hooks still live, so no say() after) */
        if (log_file) fclose(log_file);
        log_file = NULL;
    }
    return TRUE;
}
