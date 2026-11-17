/* Hamlet TD — порт на PSP, этап 1: каркас + ядро игры.
 * Поле, дорога (карта «Змейка»), волны зомби, башня-стрелок, деньги 1-5 за зомби,
 * бесконечные улучшения, управление под PSP.
 * Рисуем прямо в VRAM (без внешних библиотек), текст — цифрами и значками. */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspctrl.h>
#include<math.h>
#include <stdlib.h>
#include <string.h>

PSP_MODULE_INFO("HamletTD", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);

/* ---------- выход по кнопке HOME ---------- */
static int exit_cb(int a1, int a2, void *c) { sceKernelExitGame(); return 0; }
static int cb_thread(SceSize args, void *argp) {
    int id = sceKernelCreateCallback("exit_cb", exit_cb, NULL);
    sceKernelRegisterExitCallback(id);
    sceKernelSleepThreadCB();
    return 0;
}
static void setup_callbacks(void) {
    int t = sceKernelCreateThread("cb_thread", cb_thread, 0x11, 0xFA0, PSP_THREAD_ATTR_USER, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}

/* ---------- экран ---------- */
#define SW 480
#define SH 272
#define BUFW 512
#define RGB(r, g, b) (0xFF000000u | ((unsigned)(b) << 16) | ((unsigned)(g) << 8) | (unsigned)(r))

typedef unsigned int u32;
static int cur_buf = 0;

static u32 *drawbuf(void) { return (u32 *)(0x44000000 + cur_buf * BUFW * SH * 4); }
static void present(void) {
    sceDisplaySetFrameBuf((void *)(0x04000000 + cur_buf * BUFW * SH * 4), BUFW,
                          PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitVblankStart();
    cur_buf ^= 1;
}

static void rect(int x, int y, int w, int h, u32 c) {
    u32 *b = drawbuf();
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SW) w = SW - x;
    if (y + h > SH) h = SH - y;
    for (int j = 0; j < h; j++) {
        u32 *p = b + (y + j) * BUFW + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}
static void frame(int x, int y, int w, int h, u32 c) {
    rect(x, y, w, 1, c); rect(x, y + h - 1, w, 1, c);
    rect(x, y, 1, h, c); rect(x + w - 1, y, 1, h, c);
}
static void disc(float cx, float cy, float r, u32 c) {
    u32 *b = drawbuf();
    int ir = (int)ceilf(r);
    for (int j = -ir; j <= ir; j++)
        for (int i = -ir; i <= ir; i++)
            if (i * i + j * j <= r * r) {
                int x = (int)cx + i, y = (int)cy + j;
                if (x >= 0 && x < SW && y >= 0 && y < SH) b[y * BUFW + x] = c;
            }
}
static void ring(float cx, float cy, float r, u32 c) {
    u32 *b = drawbuf();
    for (int a = 0; a < 90; a++) {
        float t = a * 6.2831853f / 90.0f;
        int x = (int)(cx + cosf(t) * r), y = (int)(cy + sinf(t) * r);
        if (x >= 0 && x < SW && y >= 0 && y < SH) b[y * BUFW + x] = c;
    }
}

/* цифры 3x5 */
static const unsigned short DIG[10] = {0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9,
                                       0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF};
static int draw_num(int x, int y, int n, int sc, u32 c) {
    char s[16]; int len = 0;
    if (n < 0) n = 0;
    if (n == 0) s[len++] = 0;
    else { int t = n; char tmp[16]; int k = 0; while (t) { tmp[k++] = t % 10; t /= 10; } while (k) s[len++] = tmp[--k]; }
    for (int d = 0; d < len; d++) {
        unsigned short g = DIG[(int)s[d]];
        for (int r = 0; r < 5; r++)
            for (int cc = 0; cc < 3; cc++)
                if (g & (1 << (14 - (r * 3 + cc))))
                    rect(x + d * 4 * sc + cc * sc, y + r * sc, sc, sc, c);
    }
    return len * 4 * sc;
}

/* ---------- данные игры ---------- */
#define COLS 18
#define ROWS 16
#define CELL 40.0f
#define SCALE (17.0f / 40.0f)  /* логические 960x640 -> экран PSP: клетка 17 px */
#define FIELD_PX 306           /* 18*17 */

static const int PATHC[8][2] = {{-1, 2}, {14, 2}, {14, 6}, {3, 6}, {3, 10}, {15, 10}, {15, 13}, {19, 13}};
#define NPATH 8
static float PX[NPATH], PY[NPATH];
static int blocked[ROWS][COLS];

#define MAXE 256
#define MAXT 200
#define MAXP 512

typedef struct { float x, y, dist, hp, maxhp; int wp, alive, id, reward; } Enemy;
typedef struct { int cx, cy, level, invested; float dmg, range, rate, cd, ang; } Tower;
typedef struct { float x, y, dmg; int target, tid, alive; } Proj;

static Enemy en[MAXE]; static Tower tw[MAXT]; static Proj pr[MAXP];
static int nt = 0, next_id = 1;
static int money, lives, wave, to_spawn, in_wave, speed, paused, over;
static float spawn_cd, wave_cd, zhp;
static int ccx = 5, ccy = 4;

#define ARCHER_COST 125
static void build_map(void) {
    for (int i = 0; i < NPATH; i++) { PX[i] = PATHC[i][0] * CELL + CELL / 2; PY[i] = PATHC[i][1] * CELL + CELL / 2; }
    for (int cy = 0; cy < ROWS; cy++)
        for (int cx = 0; cx < COLS; cx++) {
            float px = cx * CELL + CELL / 2, py = cy * CELL + CELL / 2, best = 1e9f;
            for (int i = 0; i < NPATH - 1; i++) {
                float ax = PX[i], ay = PY[i], bx = PX[i + 1], by = PY[i + 1];
                float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
                float t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
                if (t < 0) t = 0; if (t > 1) t = 1;
                float d = hypotf(px - (ax + t * dx), py - (ay + t * dy));
                if (d < best) best = d;
            }
            blocked[cy][cx] = best < CELL * 0.85f;
        }
}

static void new_game(void) {
    memset(en, 0, sizeof en); memset(tw, 0, sizeof tw); memset(pr, 0, sizeof pr);
    nt = 0; next_id = 1; money = 350; lives = 20; wave = 0; to_spawn = 0; in_wave = 0;
    speed = 1; paused = 0; over = 0; spawn_cd = 0; wave_cd = 4.0f; zhp = 26;
}

static Tower *tower_at(int cx, int cy) {
    for (int i = 0; i < nt; i++) if (tw[i].cx == cx && tw[i].cy == cy) return &tw[i];
    return NULL;
}
static int can_place(int cx, int cy) {
    if (cx < 0 || cy < 0 || cx >= COLS || cy >= ROWS) return 0;
    if (blocked[cy][cx] || tower_at(cx, cy)) return 0;
    return money >= ARCHER_COST && nt < MAXT;
}
static void place_tower(int cx, int cy) {
    if (!can_place(cx, cy)) return;
    Tower *t = &tw[nt++];
    t->cx = cx; t->cy = cy; t->level = 1; t->invested = ARCHER_COST;
    t->dmg = 10; t->range = 130; t->rate = 0.6f; t->cd = 0; t->ang = 0;
    money -= ARCHER_COST;
}
static int upg_cost(Tower *t) { return (int)(ARCHER_COST * 0.6f * t->level); }
static int upgrade(Tower *t) {
    int c = upg_cost(t);
    if (money < c) return 0;
    money -= c; t->invested += c; t->level++;
    t->dmg *= 1.3f; if (t->level <= 11) t->range *= 1.07f;
    t->rate *= 0.92f; if (t->rate < 0.05f) t->rate = 0.05f;
    return 1;
}
static void upgrade_max(Tower *t) { int n = 0; while (n < 500 && upgrade(t)) n++; }
static void sell(Tower *t) {
    money += (int)(t->invested * 0.6f);
    *t = tw[--nt];
}

static void spawn_enemy(void) {
    for (int i = 0; i < MAXE; i++) if (!en[i].alive) {
        Enemy *e = &en[i];
        e->alive = 1; e->id = next_id++; e->wp = 0; e->dist = 0;
        e->x = PX[0]; e->y = PY[0]; e->hp = e->maxhp = zhp;
        e->reward = 1 + rand() % 5;  /* 1–5 монет за зомби */
        return;
    }
}

static void update(float dt) {
    /* волны */
    if (!in_wave) {
        wave_cd -= dt;
        if (wave_cd <= 0) { wave++; in_wave = 1; to_spawn = 5 + 2 * wave; spawn_cd = 0; zhp = 26 * powf(1.14f, (float)wave); }
    } else {
        if (to_spawn > 0) {
            spawn_cd -= dt;
            if (spawn_cd <= 0) { spawn_enemy(); to_spawn--; spawn_cd = 0.9f; }
        } else {
            int any = 0; for (int i = 0; i < MAXE; i++) if (en[i].alive) { any = 1; break; }
            if (!any) { in_wave = 0; wave_cd = 5.0f; money += 20 + wave * 5; }
        }
    }
    /* враги идут по точкам пути */
    for (int i = 0; i < MAXE; i++) {
        Enemy *e = &en[i]; if (!e->alive) continue;
        float step = 58.0f * dt;
        while (step > 0 && e->alive) {
            float tx = PX[e->wp + 1], ty = PY[e->wp + 1];
            float dx = tx - e->x, dy = ty - e->y, d = hypotf(dx, dy);
            if (d <= step) { e->x = tx; e->y = ty; e->dist += d; step -= d; e->wp++;
                if (e->wp >= NPATH - 1) { e->alive = 0; lives--; if (lives <= 0) over = 1; } }
            else { e->x += dx / d * step; e->y += dy / d * step; e->dist += step; step = 0; }
        }
    }
    /* башни стреляют по первому на пути */
    for (int i = 0; i < nt; i++) {
        Tower *t = &tw[i];
        float tx = t->cx * CELL + CELL / 2, ty = t->cy * CELL + CELL / 2;
        t->cd -= dt;
        int best = -1; float bd = -1;
        for (int k = 0; k < MAXE; k++) if (en[k].alive && hypotf(en[k].x - tx, en[k].y - ty) <= t->range && en[k].dist > bd) { bd = en[k].dist; best = k; }
        if (best >= 0) {
            t->ang = atan2f(en[best].y - ty, en[best].x - tx);
            if (t->cd <= 0) {
                t->cd = t->rate;
                for (int p = 0; p < MAXP; p++) if (!pr[p].alive) {
                    pr[p].alive = 1; pr[p].x = tx; pr[p].y = ty; pr[p].dmg = t->dmg;
                    pr[p].target = best; pr[p].tid = en[best].id; break;
                }
            }
        }
    }
    /* снаряды (в 1.6 раза быстрее, как в браузерной версии) */
    for (int p = 0; p < MAXP; p++) {
        Proj *q = &pr[p]; if (!q->alive) continue;
        Enemy *e = &en[q->target];
        if (!e->alive || e->id != q->tid) { q->alive = 0; continue; }
        float dx = e->x - q->x, dy = e->y - q->y, d = hypotf(dx, dy), st = 1280.0f * dt;
        if (d <= st + 6) {
            e->hp -= q->dmg; q->alive = 0;
            if (e->hp <= 0) { e->alive = 0; money += e->reward; }
        } else { q->x += dx / d * st; q->y += dy / d * st; }
    }
}

static void draw_game(void) {
    rect(0, 0, SW, SH, RGB(58, 118, 50));
    /* дорога */
    for (int i = 0; i < NPATH - 1; i++) {
        float ax = PX[i] * SCALE, ay = PY[i] * SCALE, bx = PX[i + 1] * SCALE, by = PY[i + 1] * SCALE;
        float hw = CELL * 0.42f * SCALE;
        float x0 = fminf(ax, bx) - hw, x1 = fmaxf(ax, bx) + hw, y0 = fminf(ay, by) - hw, y1 = fmaxf(ay, by) + hw;
        rect((int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0), RGB(150, 120, 80));
    }
    /* башни */
    for (int i = 0; i < nt; i++) {
        Tower *t = &tw[i];
        float cx = (t->cx * CELL + CELL / 2) * SCALE, cy = (t->cy * CELL + CELL / 2) * SCALE;
        disc(cx, cy, 7, RGB(18, 24, 43)); ring(cx, cy, 7, RGB(255, 183, 77));
        disc(cx + cosf(t->ang) * 5, cy + sinf(t->ang) * 5, 2, RGB(255, 255, 255));
        if (t->level > 1) draw_num((int)cx - 3, (int)cy + 5, t->level, 1, RGB(255, 213, 79));
    }
    /* враги */
    for (int i = 0; i < MAXE; i++) if (en[i].alive) {
        Enemy *e = &en[i]; float x = e->x * SCALE, y = e->y * SCALE;
        disc(x, y, 4.5f, RGB(124, 179, 66));
        rect((int)x - 4, (int)y - 8, 8, 1, RGB(60, 0, 0));
        rect((int)x - 4, (int)y - 8, (int)(8 * e->hp / e->maxhp), 1, RGB(255, 80, 80));
    }
    for (int p = 0; p < MAXP; p++) if (pr[p].alive) disc(pr[p].x * SCALE, pr[p].y * SCALE, 1.5f, RGB(255, 245, 157));
    /* курсор */
    {
        int x = (int)(ccx * CELL * SCALE), y = (int)(ccy * CELL * SCALE);
        Tower *t = tower_at(ccx, ccy);
        u32 col = t ? RGB(120, 220, 255) : (can_place(ccx, ccy) ? RGB(129, 199, 132) : RGB(239, 83, 80));
        frame(x, y, 17, 17, col); frame(x + 1, y + 1, 15, 15, col);
        if (t) ring((ccx * CELL + CELL / 2) * SCALE, (ccy * CELL + CELL / 2) * SCALE, t->range * SCALE, RGB(255, 255, 255));
    }
    /* панель */
    rect(FIELD_PX, 0, SW - FIELD_PX, SH, RGB(16, 22, 42));
    int px = FIELD_PX + 10, y = 10;
    disc(px + 4, y + 5, 4, RGB(255, 213, 79)); draw_num(px + 14, y, money, 2, RGB(255, 213, 79)); y += 18;
    disc(px + 4, y + 5, 4, RGB(239, 83, 80));  draw_num(px + 14, y, lives, 2, RGB(239, 154, 154)); y += 18;
    rect(px, y, 8, 8, RGB(79, 195, 247));       draw_num(px + 14, y, wave, 2, RGB(144, 202, 249)); y += 18;
    rect(px, y, 8, 8, RGB(129, 199, 132));      draw_num(px + 14, y, speed, 2, RGB(165, 214, 167)); y += 24;
    Tower *t = tower_at(ccx, ccy);
    if (t) {
        draw_num(px, y, t->level, 2, RGB(255, 213, 79)); y += 14;
        draw_num(px, y, (int)t->dmg, 2, RGB(255, 138, 101)); y += 14;
        draw_num(px, y, (int)t->range, 2, RGB(144, 202, 249)); y += 14;
        draw_num(px, y, upg_cost(t), 2, money >= upg_cost(t) ? RGB(165, 214, 167) : RGB(239, 154, 154));
    } else {
        draw_num(px, y, ARCHER_COST, 2, money >= ARCHER_COST ? RGB(165, 214, 167) : RGB(239, 154, 154));
    }
    if (paused) { rect(150, 110, 8, 40, RGB(255, 255, 255)); rect(166, 110, 8, 40, RGB(255, 255, 255)); }
    if (over) { rect(100, 100, 106, 60, RGB(120, 20, 20)); draw_num(125, 120, wave, 4, RGB(255, 255, 255)); }
}

int main(void) {
    setup_callbacks();
    sceDisplaySetMode(0, SW, SH);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    build_map(); new_game();

    SceCtrlData pad; unsigned prev = 0; float hold = 0;
    unsigned last = sceKernelGetSystemTimeLow();
    for (;;) {
        unsigned now = sceKernelGetSystemTimeLow();
        float dt = (now - last) / 1000000.0f; last = now;
        if (dt > 0.05f) dt = 0.05f;

        sceCtrlReadBufferPositive(&pad, 1);
        unsigned b = pad.Buttons, edge = b & ~prev;
        int dx = 0, dy = 0;
        if (edge & PSP_CTRL_LEFT) dx = -1;  if (edge & PSP_CTRL_RIGHT) dx = 1;
        if (edge & PSP_CTRL_UP) dy = -1;    if (edge & PSP_CTRL_DOWN) dy = 1;
        if (b & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_UP | PSP_CTRL_DOWN)) {
            hold += dt;
            if (hold > 0.3f && !(edge)) {   /* автоповтор при удержании */
                hold = 0.22f;
                if (b & PSP_CTRL_LEFT) dx = -1;  if (b & PSP_CTRL_RIGHT) dx = 1;
                if (b & PSP_CTRL_UP) dy = -1;    if (b & PSP_CTRL_DOWN) dy = 1;
            }
        } else hold = 0;
        ccx += dx; ccy += dy;
        if (ccx < 0) ccx = 0; if (ccx >= COLS) ccx = COLS - 1;
        if (ccy < 0) ccy = 0; if (ccy >= ROWS) ccy = ROWS - 1;

        if (over) {
            if (edge & PSP_CTRL_START) new_game();
        } else {
            if (edge & PSP_CTRL_START) paused = !paused;
            if (!paused) {
                Tower *t = tower_at(ccx, ccy);
                if (edge & PSP_CTRL_CROSS) place_tower(ccx, ccy);                 /* X — поставить */
                if (edge & PSP_CTRL_SQUARE) { if (t) sell(t); }                    /* квадрат — продать */
                if (edge & PSP_CTRL_TRIANGLE) speed = speed % 3 + 1;               /* треугольник — скорость */
                if (t && ((edge & PSP_CTRL_RTRIGGER) || ((edge & PSP_CTRL_LTRIGGER) && (b & PSP_CTRL_RTRIGGER)))) {
                    if ((b & PSP_CTRL_LTRIGGER) && (b & PSP_CTRL_RTRIGGER)) upgrade_max(t);  /* L+R — максимум */
                    else upgrade(t);                                                          /* R — улучшить */
                }
                float sdt = dt * speed;
                float rem = sdt;
                while (rem > 1e-6f) { float st = rem < (1.0f / 90) ? rem : (1.0f / 90); update(st); rem -= st; }
            }
        }
        prev = b;
        draw_game();
        present();
    }
    return 0;
}
