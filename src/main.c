/*
 * COAST ENGINE v0.4 TD CORE + WORLD
 * PSP single-file engine expansion
 *
 * World system:
 * - 1024x1024 byte-per-tile world (1 MiB)
 * - 32x32 tile chunks; visible-area rendering only
 * - camera follow
 * - in-game tile editor
 * - world save/load (versioned)
 * - basic object pool, collision, player movement, enemy chase
 *
 * Controls:
 *   D-PAD / ANALOG : move player (game mode)
 *   SELECT         : toggle map editor
 *   D-PAD          : move editor cursor (editor mode)
 *   L / R          : choose tile (editor mode)
 *   X              : paint tile (editor mode)
 *   SQUARE         : erase to grass (editor mode)
 *   CIRCLE         : save world
 *   TRIANGLE       : load world
 *   START          : exit
 *
 * Replace src/main.c. Uses PSPSDK.
 */

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <pspgu.h>
#include <pspge.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>

PSP_MODULE_INFO("COAST ENGINE TD", 0, 4, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-256);

#define CE_VERSION_MAJOR 0
#define CE_VERSION_MINOR 4

#define CE_SCREEN_W 480
#define CE_SCREEN_H 272
#define CE_BUFFER_W 512

#define CE_MAP_W 1024
#define CE_MAP_H 1024
#define CE_TILE_SIZE 32
#define CE_CHUNK_SIZE 32
#define CE_CHUNKS_X (CE_MAP_W / CE_CHUNK_SIZE)
#define CE_CHUNKS_Y (CE_MAP_H / CE_CHUNK_SIZE)
#define CE_WORLD_W (CE_MAP_W * CE_TILE_SIZE)
#define CE_WORLD_H (CE_MAP_H * CE_TILE_SIZE)

#define CE_MAX_OBJECTS 128
#define CE_SAVE_PATH "ms0:/COAST_ENGINE_WORLD03.DAT"
#define CE_PLAYER_SPEED 155.0f
#define CE_ANIMATION_FPS 8.0f
#define CE_SAVE_MAGIC 0x43453357u /* CE3W */
#define CE_SAVE_VERSION 3u

static unsigned int __attribute__((aligned(16))) g_displayList[262144];

/* PSP GU 2D vertex. */
typedef struct {
    short x, y, z;
} CE_Vertex;

enum {
    CE_TILE_GRASS = 0,
    CE_TILE_WALL  = 1,
    CE_TILE_WATER = 2,
    CE_TILE_SAND  = 3,
    CE_TILE_ROAD  = 4,
    CE_TILE_DIRT  = 5,
    CE_TILE_COUNT = 6
};

enum {
    CE_OBJECT_NONE = 0,
    CE_OBJECT_PLAYER = 1,
    CE_OBJECT_ENEMY = 2,
    CE_OBJECT_CRATE = 3,
    CE_OBJECT_TREE = 4
};

#define CE_OBJECT_SOLID 1

typedef struct {
    int active;
    int type;
    int flags;
    float x, y, w, h;
    float vx, vy;
    int hp, maxHp;
    int frame, frameCount;
    float animTimer, animFPS;
} CE_Object;

typedef struct {
    unsigned int buttons, previousButtons;
    float deltaTime, totalTime;
    float fpsTimer;
    unsigned int fpsFrames, fps;
} CE_State;

/*
 * One byte per tile: 1024*1024 = 1 MiB.
 * This is small enough for PSP RAM and lets the renderer touch only
 * the visible tiles. Chunk coordinates are used for editing/rendering.
 */
static unsigned char g_map[CE_MAP_H][CE_MAP_W];
static CE_Object g_objects[CE_MAX_OBJECTS];
static int g_objectCount;
static int g_playerIndex = -1;
static CE_State g_state;
static float g_cameraX, g_cameraY;
static unsigned long long g_lastTime;
static int g_running = 1;
static int td_mode = 1;
static int g_editor = 0;
static int g_cursorX = 32, g_cursorY = 32;
static int g_selectedTile = CE_TILE_WALL;
static int g_saveMessage = 0;

static float CE_ClampF(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int CE_ClampI(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int CE_ButtonPressed(unsigned int b) {
    return (g_state.buttons & b) && !(g_state.previousButtons & b);
}

static int CE_InMap(int x, int y) {
    return x >= 0 && y >= 0 && x < CE_MAP_W && y < CE_MAP_H;
}

/* Exit callback. */
static int CE_ExitCallback(int a, int b, void *c) {
    (void)a; (void)b; (void)c;
    g_running = 0;
    sceKernelExitGame();
    return 0;
}

static int CE_CallbackThread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int id = sceKernelCreateCallback("CE_EXIT", CE_ExitCallback, NULL);
    if (id >= 0) sceKernelRegisterExitCallback(id);
    sceKernelSleepThreadCB();
    return 0;
}

static void CE_SetupCallbacks(void) {
    int id = sceKernelCreateThread("CE_CALLBACK", CE_CallbackThread,
                                   0x11, 0x1000, 0, NULL);
    if (id >= 0) sceKernelStartThread(id, 0, NULL);
}

/* Graphics. */
static void CE_GraphicsInit(void) {
    sceGuInit();
    sceGuStart(GU_DIRECT, g_displayList);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, CE_BUFFER_W);
    sceGuDispBuffer(CE_SCREEN_W, CE_SCREEN_H, (void *)0x88000, CE_BUFFER_W);
    sceGuOffset(2048 - CE_SCREEN_W / 2, 2048 - CE_SCREEN_H / 2);
    sceGuViewport(2048, 2048, CE_SCREEN_W, CE_SCREEN_H);
    sceGuScissor(0, 0, CE_SCREEN_W, CE_SCREEN_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuClearColor(GU_COLOR(8, 15, 24, 255));
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

static void CE_DrawRect(float x, float y, float w, float h, unsigned int color) {
    CE_Vertex *v = (CE_Vertex *)sceGuGetMemory(sizeof(CE_Vertex) * 4);
    if (!v) return;
    v[0].x = (short)x;     v[0].y = (short)y;     v[0].z = 0;
    v[1].x = (short)(x+w); v[1].y = (short)y;     v[1].z = 0;
    v[2].x = (short)x;     v[2].y = (short)(y+h); v[2].z = 0;
    v[3].x = (short)(x+w); v[3].y = (short)(y+h); v[3].z = 0;
    sceGuDisable(GU_TEXTURE_2D);
    sceGuColor(color);
    sceGuDrawArray(GU_TRIANGLE_STRIP,
        GU_VERTEX_16BIT | GU_TRANSFORM_2D, 4, 0, v);
}

/* World and chunk helpers. */
static unsigned char CE_MapGet(int x, int y) {
    if (!CE_InMap(x, y)) return CE_TILE_WALL;
    return g_map[y][x];
}

static void CE_MapSet(int x, int y, unsigned char tile) {
    if (!CE_InMap(x, y)) return;
    if (tile >= CE_TILE_COUNT) tile = CE_TILE_GRASS;
    g_map[y][x] = tile;
}

static int CE_ChunkX(int tileX) { return tileX / CE_CHUNK_SIZE; }
static int CE_ChunkY(int tileY) { return tileY / CE_CHUNK_SIZE; }

static int CE_TileSolid(int t) {
    return t == CE_TILE_WALL || t == CE_TILE_WATER;
}

static void CE_MapGenerate(void) {
    int x, y;
    memset(g_map, CE_TILE_GRASS, sizeof(g_map));

    /* Outer world boundary. */
    for (x = 0; x < CE_MAP_W; ++x) {
        CE_MapSet(x, 0, CE_TILE_WALL);
        CE_MapSet(x, CE_MAP_H - 1, CE_TILE_WALL);
    }
    for (y = 0; y < CE_MAP_H; ++y) {
        CE_MapSet(0, y, CE_TILE_WALL);
        CE_MapSet(CE_MAP_W - 1, y, CE_TILE_WALL);
    }

    /* Starting region: lake, shore, roads and obstacles. */
    for (y = 18; y <= 25; ++y)
        for (x = 19; x <= 27; ++x) CE_MapSet(x, y, CE_TILE_SAND);
    for (y = 20; y <= 23; ++y)
        for (x = 21; x <= 25; ++x) CE_MapSet(x, y, CE_TILE_WATER);
    for (x = 5; x < 59; ++x) {
        CE_MapSet(x, 34, CE_TILE_ROAD);
        CE_MapSet(x, 35, CE_TILE_ROAD);
    }
    for (y = 10; y < 15; ++y) {
        CE_MapSet(12, y, CE_TILE_WALL);
        CE_MapSet(13, y, CE_TILE_WALL);
    }
    for (x = 40; x < 46; ++x) CE_MapSet(x, 12, CE_TILE_WALL);

    /* A second distant landmark proves the world extends beyond the spawn. */
    for (x = 180; x < 220; ++x) {
        CE_MapSet(x, 180, CE_TILE_DIRT);
        CE_MapSet(x, 181, CE_TILE_DIRT);
    }
    for (y = 150; y < 190; ++y) {
        CE_MapSet(220, y, CE_TILE_SAND);
        CE_MapSet(221, y, CE_TILE_SAND);
    }
}

/* Object pool. */
static int CE_AddObject(int type, float x, float y, float w, float h, int flags) {
    int i;
    for (i = 0; i < CE_MAX_OBJECTS; ++i) {
        if (!g_objects[i].active) {
            CE_Object *o = &g_objects[i];
            memset(o, 0, sizeof(*o));
            o->active = 1; o->type = type; o->flags = flags;
            o->x = x; o->y = y; o->w = w; o->h = h;
            o->hp = o->maxHp = 100;
            o->frameCount = 4; o->animFPS = CE_ANIMATION_FPS;
            if (i >= g_objectCount) g_objectCount = i + 1;
            return i;
        }
    }
    return -1;
}

static int CE_AABB(float ax, float ay, float aw, float ah,
                   float bx, float by, float bw, float bh) {
    return ax < bx+bw && ax+aw > bx && ay < by+bh && ay+ah > by;
}

static int CE_ObjectHitsSolid(const CE_Object *o, int self) {
    int left = (int)floorf(o->x / CE_TILE_SIZE);
    int right = (int)floorf((o->x + o->w - 1.0f) / CE_TILE_SIZE);
    int top = (int)floorf(o->y / CE_TILE_SIZE);
    int bottom = (int)floorf((o->y + o->h - 1.0f) / CE_TILE_SIZE);
    int x, y, i;
    for (y = top; y <= bottom; ++y)
        for (x = left; x <= right; ++x)
            if (CE_TileSolid(CE_MapGet(x, y))) return 1;
    for (i = 0; i < g_objectCount; ++i) {
        CE_Object *other = &g_objects[i];
        if (i == self || !other->active || !(other->flags & CE_OBJECT_SOLID))
            continue;
        if (CE_AABB(o->x,o->y,o->w,o->h,
                    other->x,other->y,other->w,other->h)) return 1;
    }
    return 0;
}

static void CE_MoveObject(int i, float dx, float dy) {
    CE_Object *o = &g_objects[i];
    float ox = o->x, oy = o->y;
    o->x += dx;
    if (CE_ObjectHitsSolid(o, i)) o->x = ox;
    o->y += dy;
    if (CE_ObjectHitsSolid(o, i)) o->y = oy;
}

static void CE_WorldInit(void) {
    int i;
    memset(g_objects, 0, sizeof(g_objects));
    g_objectCount = 0;
    g_playerIndex = -1;
    CE_MapGenerate();

    g_playerIndex = CE_AddObject(CE_OBJECT_PLAYER,
        32.0f * 31.0f, 32.0f * 31.0f, 24.0f, 28.0f, 0);

    for (i = 0; i < 7; ++i)
        CE_AddObject(CE_OBJECT_TREE, 32.0f * (8 + i*2),
            32.0f * 28.0f, 26.0f, 26.0f, CE_OBJECT_SOLID);

    CE_AddObject(CE_OBJECT_CRATE, 32.0f*35, 32.0f*31,
                 26, 26, CE_OBJECT_SOLID);
    CE_AddObject(CE_OBJECT_CRATE, 32.0f*36, 32.0f*31,
                 26, 26, CE_OBJECT_SOLID);
    CE_AddObject(CE_OBJECT_ENEMY, 32.0f*38, 32.0f*37,
                 24, 28, 0);
}

/* Input and editor. */
static void CE_InputUpdate(void) {
    SceCtrlData pad;
    CE_Object *p;
    float dx = 0, dy = 0, len;

    g_state.previousButtons = g_state.buttons;
    sceCtrlPeekBufferPositive(&pad, 1);
    g_state.buttons = pad.Buttons;

    if (CE_ButtonPressed(PSP_CTRL_START)) {
        g_running = 0;
        return;
    }

    if (CE_ButtonPressed(PSP_CTRL_SELECT)) {
        if (!td_mode && !g_editor) {
            td_mode = 1;
            return;
        }
        g_editor = !g_editor;
        if (g_playerIndex >= 0) {
            CE_Object *pl = &g_objects[g_playerIndex];
            g_cursorX = CE_ClampI((int)((pl->x + pl->w*0.5f)/CE_TILE_SIZE),0,CE_MAP_W-1);
            g_cursorY = CE_ClampI((int)((pl->y + pl->h*0.5f)/CE_TILE_SIZE),0,CE_MAP_H-1);
        }
    }

    if (CE_ButtonPressed(PSP_CTRL_CIRCLE)) {
        /* Save request is performed by the main loop. */
    }
    if (CE_ButtonPressed(PSP_CTRL_TRIANGLE)) {
        /* Load request is performed by the main loop. */
    }

    if (g_editor) {
        if (CE_ButtonPressed(PSP_CTRL_LEFT))  g_cursorX--;
        if (CE_ButtonPressed(PSP_CTRL_RIGHT)) g_cursorX++;
        if (CE_ButtonPressed(PSP_CTRL_UP))    g_cursorY--;
        if (CE_ButtonPressed(PSP_CTRL_DOWN))  g_cursorY++;
        if (pad.Lx < 90) g_cursorX--;
        if (pad.Lx > 165) g_cursorX++;
        if (pad.Ly < 90) g_cursorY--;
        if (pad.Ly > 165) g_cursorY++;
        g_cursorX = CE_ClampI(g_cursorX, 0, CE_MAP_W-1);
        g_cursorY = CE_ClampI(g_cursorY, 0, CE_MAP_H-1);
        if (CE_ButtonPressed(PSP_CTRL_LTRIGGER))
            g_selectedTile = (g_selectedTile + CE_TILE_COUNT - 1) % CE_TILE_COUNT;
        if (CE_ButtonPressed(PSP_CTRL_RTRIGGER))
            g_selectedTile = (g_selectedTile + 1) % CE_TILE_COUNT;
        if (CE_ButtonPressed(PSP_CTRL_CROSS))
            CE_MapSet(g_cursorX, g_cursorY, (unsigned char)g_selectedTile);
        if (CE_ButtonPressed(PSP_CTRL_SQUARE))
            CE_MapSet(g_cursorX, g_cursorY, CE_TILE_GRASS);
        return;
    }

    if (g_playerIndex < 0) return;
    p = &g_objects[g_playerIndex];

    if (g_state.buttons & PSP_CTRL_LEFT) dx -= 1;
    if (g_state.buttons & PSP_CTRL_RIGHT) dx += 1;
    if (g_state.buttons & PSP_CTRL_UP) dy -= 1;
    if (g_state.buttons & PSP_CTRL_DOWN) dy += 1;
    if (pad.Lx < 108) dx = -1;
    if (pad.Lx > 148) dx = 1;
    if (pad.Ly < 108) dy = -1;
    if (pad.Ly > 148) dy = 1;

    len = sqrtf(dx*dx + dy*dy);
    if (len > 1.0f) { dx /= len; dy /= len; }
    p->vx = dx * CE_PLAYER_SPEED;
    p->vy = dy * CE_PLAYER_SPEED;
}

/* Simulation. */
static void CE_UpdateObjects(float dt) {
    int i;
    CE_Object *player = (g_playerIndex >= 0) ? &g_objects[g_playerIndex] : NULL;
    for (i = 0; i < g_objectCount; ++i) {
        CE_Object *o = &g_objects[i];
        if (!o->active) continue;

        if (o->type == CE_OBJECT_ENEMY && player) {
            float vx = player->x - o->x;
            float vy = player->y - o->y;
            float d = sqrtf(vx*vx + vy*vy);
            o->vx = o->vy = 0;
            if (d > 36.0f && d < 320.0f && d > 0.001f) {
                o->vx = (vx/d) * 58.0f;
                o->vy = (vy/d) * 58.0f;
            }
        }
        if (o->type == CE_OBJECT_PLAYER || o->type == CE_OBJECT_ENEMY)
            CE_MoveObject(i, o->vx*dt, o->vy*dt);

        if (fabsf(o->vx) + fabsf(o->vy) > 2.0f) {
            o->animTimer += dt;
            if (o->animTimer >= 1.0f / o->animFPS) {
                o->animTimer = 0;
                o->frame = (o->frame + 1) % o->frameCount;
            }
        } else o->animTimer = 0;
    }
}

static void CE_UpdateCamera(float dt) {
    CE_Object *p;
    float tx, ty, smooth;
    if (g_editor) {
        tx = g_cursorX * CE_TILE_SIZE + CE_TILE_SIZE*0.5f - CE_SCREEN_W*0.5f;
        ty = g_cursorY * CE_TILE_SIZE + CE_TILE_SIZE*0.5f - CE_SCREEN_H*0.5f;
    } else {
        if (g_playerIndex < 0) return;
        p = &g_objects[g_playerIndex];
        tx = p->x + p->w*0.5f - CE_SCREEN_W*0.5f;
        ty = p->y + p->h*0.5f - CE_SCREEN_H*0.5f;
    }
    tx = CE_ClampF(tx, 0, CE_WORLD_W - CE_SCREEN_W);
    ty = CE_ClampF(ty, 0, CE_WORLD_H - CE_SCREEN_H);
    smooth = CE_ClampF(dt*8.0f, 0, 1);
    g_cameraX += (tx-g_cameraX)*smooth;
    g_cameraY += (ty-g_cameraY)*smooth;
}

/* Save/load: version 3 stores the complete 1 MiB world and object pool. */
typedef struct {
    unsigned int magic;
    unsigned int version;
    unsigned int mapWidth;
    unsigned int mapHeight;
    int objectCount;
    int playerIndex;
    CE_Object objects[CE_MAX_OBJECTS];
    unsigned char map[CE_MAP_H][CE_MAP_W];
} CE_SaveData;

static int CE_Save(void) {
    CE_SaveData *d = (CE_SaveData *)malloc(sizeof(CE_SaveData));
    SceUID f;
    int n;
    if (!d) return 0;
    d->magic = CE_SAVE_MAGIC;
    d->version = CE_SAVE_VERSION;
    d->mapWidth = CE_MAP_W;
    d->mapHeight = CE_MAP_H;
    d->objectCount = g_objectCount;
    d->playerIndex = g_playerIndex;
    memcpy(d->objects, g_objects, sizeof(g_objects));
    memcpy(d->map, g_map, sizeof(g_map));

    f = sceIoOpen(CE_SAVE_PATH, PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC, 0777);
    if (f < 0) { free(d); return 0; }
    n = sceIoWrite(f, d, sizeof(*d));
    sceIoClose(f);
    free(d);
    return n == (int)sizeof(*d);
}

static int CE_Load(void) {
    CE_SaveData *d = (CE_SaveData *)malloc(sizeof(CE_SaveData));
    SceUID f;
    int n;
    if (!d) return 0;
    f = sceIoOpen(CE_SAVE_PATH, PSP_O_RDONLY, 0);
    if (f < 0) { free(d); return 0; }
    n = sceIoRead(f, d, sizeof(*d));
    sceIoClose(f);
    if (n != (int)sizeof(*d) || d->magic != CE_SAVE_MAGIC ||
        d->version != CE_SAVE_VERSION || d->mapWidth != CE_MAP_W ||
        d->mapHeight != CE_MAP_H || d->objectCount < 1 ||
        d->objectCount > CE_MAX_OBJECTS || d->playerIndex < 0 ||
        d->playerIndex >= CE_MAX_OBJECTS) {
        free(d);
        return 0;
    }
    g_objectCount = d->objectCount;
    g_playerIndex = d->playerIndex;
    memcpy(g_objects, d->objects, sizeof(g_objects));
    memcpy(g_map, d->map, sizeof(g_map));
    free(d);
    return g_objects[g_playerIndex].active &&
           g_objects[g_playerIndex].type == CE_OBJECT_PLAYER;
}

/* Rendering. */
static unsigned int CE_TileColor(int t) {
    switch (t) {
        case CE_TILE_WALL:  return GU_COLOR(75,82,96,255);
        case CE_TILE_WATER: return GU_COLOR(25,105,180,255);
        case CE_TILE_SAND:  return GU_COLOR(205,178,112,255);
        case CE_TILE_ROAD:  return GU_COLOR(65,70,78,255);
        case CE_TILE_DIRT:  return GU_COLOR(130,95,62,255);
        default:            return GU_COLOR(48,125,73,255);
    }
}

static void CE_RenderMap(void) {
    int startX = CE_ClampI((int)(g_cameraX/CE_TILE_SIZE), 0, CE_MAP_W-1);
    int startY = CE_ClampI((int)(g_cameraY/CE_TILE_SIZE), 0, CE_MAP_H-1);
    int endX = CE_ClampI(startX + CE_SCREEN_W/CE_TILE_SIZE + 2, 0, CE_MAP_W);
    int endY = CE_ClampI(startY + CE_SCREEN_H/CE_TILE_SIZE + 2, 0, CE_MAP_H);
    int x, y;

    /* Chunk bounds are calculated explicitly; only visible chunks are traversed. */
    {
        int chunkStartX = CE_ChunkX(startX);
        int chunkStartY = CE_ChunkY(startY);
        int chunkEndX = CE_ChunkX(endX-1);
        int chunkEndY = CE_ChunkY(endY-1);
        int cx, cy;
        for (cy = chunkStartY; cy <= chunkEndY; ++cy) {
            for (cx = chunkStartX; cx <= chunkEndX; ++cx) {
                int minX = cx*CE_CHUNK_SIZE;
                int minY = cy*CE_CHUNK_SIZE;
                int maxX = CE_ClampI(minX+CE_CHUNK_SIZE, 0, CE_MAP_W);
                int maxY = CE_ClampI(minY+CE_CHUNK_SIZE, 0, CE_MAP_H);
                if (minX < startX) minX = startX;
                if (minY < startY) minY = startY;
                if (maxX > endX) maxX = endX;
                if (maxY > endY) maxY = endY;
                for (y = minY; y < maxY; ++y) {
                    for (x = minX; x < maxX; ++x) {
                        int t = CE_MapGet(x,y);
                        float sx = x*CE_TILE_SIZE-g_cameraX;
                        float sy = y*CE_TILE_SIZE-g_cameraY;
                        CE_DrawRect(sx,sy,CE_TILE_SIZE+1,CE_TILE_SIZE+1,
                                    CE_TileColor(t));
                        if (t == CE_TILE_ROAD)
                            CE_DrawRect(sx+4,sy+15,CE_TILE_SIZE-8,2,
                                        GU_COLOR(220,205,120,255));
                    }
                }
            }
        }
    }
}

static void CE_RenderObjects(void) {
    int i;
    for (i = 0; i < g_objectCount; ++i) {
        CE_Object *o = &g_objects[i];
        float sx, sy;
        if (!o->active) continue;
        sx = o->x-g_cameraX; sy = o->y-g_cameraY;
        if (sx < -64 || sy < -64 || sx > CE_SCREEN_W+64 ||
            sy > CE_SCREEN_H+64) continue;

        if (o->type == CE_OBJECT_PLAYER) {
            CE_DrawRect(sx,sy,o->w,o->h,GU_COLOR(70,235,145,255));
            CE_DrawRect(sx+5,sy+5,5,5,GU_COLOR(15,35,35,255));
            CE_DrawRect(sx+15,sy+5,5,5,GU_COLOR(15,35,35,255));
        } else if (o->type == CE_OBJECT_ENEMY) {
            CE_DrawRect(sx,sy,o->w,o->h,GU_COLOR(220,65,70,255));
            CE_DrawRect(sx+4,sy+5,5,5,GU_COLOR(255,245,220,255));
            CE_DrawRect(sx+15,sy+5,5,5,GU_COLOR(255,245,220,255));
        } else if (o->type == CE_OBJECT_TREE) {
            CE_DrawRect(sx+8,sy+12,10,16,GU_COLOR(105,70,42,255));
            CE_DrawRect(sx,sy,26,20,GU_COLOR(30,155,80,255));
        } else if (o->type == CE_OBJECT_CRATE) {
            CE_DrawRect(sx,sy,o->w,o->h,GU_COLOR(155,105,55,255));
            CE_DrawRect(sx+3,sy+3,o->w-6,3,GU_COLOR(205,155,90,255));
        }
    }
}

static void CE_RenderEditor(void) {
    float sx = g_cursorX*CE_TILE_SIZE-g_cameraX;
    float sy = g_cursorY*CE_TILE_SIZE-g_cameraY;
    unsigned int c = GU_COLOR(255,255,255,255);
    /* Four thin edges make the selected tile clear without a font renderer. */
    CE_DrawRect(sx,sy,CE_TILE_SIZE,2,c);
    CE_DrawRect(sx,sy+CE_TILE_SIZE-2,CE_TILE_SIZE,2,c);
    CE_DrawRect(sx,sy,2,CE_TILE_SIZE,c);
    CE_DrawRect(sx+CE_TILE_SIZE-2,sy,2,CE_TILE_SIZE,c);
}

static void CE_RenderHUD(void) {
    CE_DrawRect(0,0,CE_SCREEN_W,22,GU_COLOR(8,15,24,220));
    CE_DrawRect(8,7,90,7,GU_COLOR(45,55,65,255));
    CE_DrawRect(8,7,90,7,GU_COLOR(70,220,120,255));
    CE_DrawRect(430,6,42,8,GU_COLOR(45,55,65,255));
    CE_DrawRect(430,6,CE_ClampF((float)g_state.fps,0,60)*0.7f,8,
                GU_COLOR(80,175,245,255));

    if (g_editor) {
        /* Compact color-coded editor status strip. */
        CE_DrawRect(112,6,18,10,CE_TileColor(g_selectedTile));
        CE_DrawRect(138,6,8,10,GU_COLOR(255,220,80,255));
        CE_DrawRect(150,6,8,10,GU_COLOR(255,255,255,255));
    } else {
        CE_DrawRect(112,6,8,10,GU_COLOR(70,235,145,255));
    }

    CE_DrawRect(8,CE_SCREEN_H-7,464,2,GU_COLOR(8,15,24,190));
    if (g_saveMessage > 0) {
        CE_DrawRect(190,CE_SCREEN_H-22,100,8,
                    GU_COLOR(80,220,130,255));
    }
}

static void CE_Render(void) {
    sceGuStart(GU_DIRECT,g_displayList);
    sceGuClearColor(GU_COLOR(15,28,38,255));
    sceGuClear(GU_COLOR_BUFFER_BIT);
    CE_RenderMap();
    CE_RenderObjects();
    if (g_editor) CE_RenderEditor();
    CE_RenderHUD();
    sceGuFinish();
    sceGuSync(0,0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}


/* -------------------------------------------------------------------------- */
/* COAST ENGINE TD CORE v0.4                                                  */
/* First native PSP tower-defense gameplay slice.                             */
/* -------------------------------------------------------------------------- */
#define TD_MAX_TOWERS 16
#define TD_MAX_ENEMIES 48
#define TD_MAX_SHOTS 64
#define TD_PAD_COUNT 8

typedef struct {
    int active, level, type;
    float x, y, cooldown, range, damage, rate;
} TD_Tower;

typedef struct {
    int active, hp, maxHp, waypoint;
    float x, y, speed;
} TD_Enemy;

typedef struct {
    int active, target;
    float x, y, speed, damage;
} TD_Shot;

static TD_Tower td_towers[TD_MAX_TOWERS];
static TD_Enemy td_enemies[TD_MAX_ENEMIES];
static TD_Shot td_shots[TD_MAX_SHOTS];
static const float td_pad_x[TD_PAD_COUNT] = {52, 145, 205, 310, 355, 445, 165, 285};
static const float td_pad_y[TD_PAD_COUNT] = {135, 35, 235, 145, 35, 155, 115, 225};
static const float td_path_x[8] = {-18, 112, 112, 258, 258, 398, 398, 500};
static const float td_path_y[8] = {82, 82, 192, 192, 92, 92, 222, 222};
static int td_cursor, td_selected = -1, td_wave, td_money, td_lives;
static int td_spawn_left, td_spawned, td_kills, td_game_over;
static float td_spawn_timer, td_wave_timer, td_clock;
static float TD_Clamp(float v, float a, float b) {
    return v < a ? a : (v > b ? b : v);
}
static float TD_Dist(float ax, float ay, float bx, float by) {
    float dx=ax-bx, dy=ay-by;
    return sqrtf(dx*dx+dy*dy);
}
static void TD_Init(void) {
    memset(td_towers,0,sizeof(td_towers));
    memset(td_enemies,0,sizeof(td_enemies));
    memset(td_shots,0,sizeof(td_shots));
    td_cursor=0; td_selected=-1; td_wave=0; td_money=180; td_lives=20;
    td_spawn_left=td_spawned=td_kills=td_game_over=0;
    td_spawn_timer=td_wave_timer=td_clock=0;
}
static int TD_AddEnemy(void) {
    int i;
    for(i=0;i<TD_MAX_ENEMIES;i++) if(!td_enemies[i].active) {
        TD_Enemy *e=&td_enemies[i];
        memset(e,0,sizeof(*e)); e->active=1;
        e->x=td_path_x[0]; e->y=td_path_y[0];
        e->waypoint=1; e->speed=34.0f+(float)(td_wave>10?10:td_wave)*1.4f;
        e->maxHp=e->hp=24+td_wave*9;
        return i;
    }
    return -1;
}
static int TD_AddShot(float x,float y,int target,float damage) {
    int i;
    for(i=0;i<TD_MAX_SHOTS;i++) if(!td_shots[i].active) {
        td_shots[i].active=1; td_shots[i].x=x; td_shots[i].y=y;
        td_shots[i].target=target; td_shots[i].damage=damage;
        td_shots[i].speed=260.0f; return i;
    }
    return -1;
}
static void TD_StartWave(void) {
    if(td_game_over || td_spawn_left>0) return;
    {
        int i, alive=0;
        for(i=0;i<TD_MAX_ENEMIES;i++) if(td_enemies[i].active) alive++;
        if(alive) return;
    }
    td_wave++;
    td_spawn_left=5+td_wave*2;
    if(td_spawn_left>34) td_spawn_left=34;
    td_spawned=0; td_spawn_timer=0.2f; td_wave_timer=0;
    td_money+=20+td_wave*3;
}
static void TD_PlaceTower(void) {
    int i, slot=-1;
    if(td_game_over || td_money<60) return;
    for(i=0;i<TD_MAX_TOWERS;i++) if(!td_towers[i].active) {slot=i;break;}
    if(slot<0) return;
    /* One tower per pad. */
    for(i=0;i<TD_MAX_TOWERS;i++)
        if(td_towers[i].active &&
           TD_Dist(td_towers[i].x,td_towers[i].y,td_pad_x[td_cursor],td_pad_y[td_cursor])<2.0f)
            return;
    td_towers[slot].active=1; td_towers[slot].level=1;
    td_towers[slot].type=td_cursor%3;
    td_towers[slot].x=td_pad_x[td_cursor]; td_towers[slot].y=td_pad_y[td_cursor];
    td_towers[slot].cooldown=0; td_towers[slot].range=105.0f;
    td_towers[slot].damage=td_towers[slot].type==0?13.0f:(td_towers[slot].type==1?7.0f:23.0f);
    td_towers[slot].rate=td_towers[slot].type==1?0.38f:(td_towers[slot].type==2?1.15f:0.72f);
    td_money-=60; td_selected=slot;
}
static void TD_Upgrade(void) {
    TD_Tower *t;
    int cost;
    if(td_selected<0 || td_selected>=TD_MAX_TOWERS) return;
    t=&td_towers[td_selected]; if(!t->active || t->level>=5) return;
    cost=55*t->level; if(td_money<cost) return;
    td_money-=cost; t->level++; t->damage*=1.42f;
    t->range+=9.0f; t->rate*=0.90f;
}
static void TD_Sell(void) {
    if(td_selected<0 || td_selected>=TD_MAX_TOWERS) return;
    if(td_towers[td_selected].active) {
        td_money+=30*td_towers[td_selected].level;
        memset(&td_towers[td_selected],0,sizeof(TD_Tower));
    }
    td_selected=-1;
}
static void TD_Input(void) {
    SceCtrlData p;
    g_state.previousButtons=g_state.buttons;
    sceCtrlPeekBufferPositive(&p,1); g_state.buttons=p.Buttons;
    if(CE_ButtonPressed(PSP_CTRL_START)){g_running=0;return;}
    if(CE_ButtonPressed(PSP_CTRL_SELECT)){td_mode=0;g_editor=1;return;}
    if(p.Buttons & PSP_CTRL_LEFT) { if(CE_ButtonPressed(PSP_CTRL_LEFT)) td_cursor=(td_cursor+TD_PAD_COUNT-1)%TD_PAD_COUNT; }
    if(p.Buttons & PSP_CTRL_RIGHT){ if(CE_ButtonPressed(PSP_CTRL_RIGHT)) td_cursor=(td_cursor+1)%TD_PAD_COUNT; }
    if(CE_ButtonPressed(PSP_CTRL_UP)) td_cursor=(td_cursor+TD_PAD_COUNT-1)%TD_PAD_COUNT;
    if(CE_ButtonPressed(PSP_CTRL_DOWN)) td_cursor=(td_cursor+1)%TD_PAD_COUNT;
    if(CE_ButtonPressed(PSP_CTRL_CROSS)) TD_PlaceTower();
    if(CE_ButtonPressed(PSP_CTRL_CIRCLE)) TD_StartWave();
    if(CE_ButtonPressed(PSP_CTRL_SQUARE)) TD_Upgrade();
    if(CE_ButtonPressed(PSP_CTRL_TRIANGLE)) TD_Sell();
    if(CE_ButtonPressed(PSP_CTRL_LTRIGGER) || CE_ButtonPressed(PSP_CTRL_RTRIGGER)) {
        int i,best=-1; float bd=99999.0f;
        for(i=0;i<TD_MAX_TOWERS;i++) if(td_towers[i].active) {
            float d=TD_Dist(td_towers[i].x,td_towers[i].y,td_pad_x[td_cursor],td_pad_y[td_cursor]);
            if(d<bd){bd=d;best=i;}
        }
        td_selected=best;
    }
}
static void TD_Update(float dt) {
    int i,j;
    if(td_game_over) return;
    td_clock+=dt;
    if(td_spawn_left>0) {
        td_spawn_timer-=dt;
        if(td_spawn_timer<=0) {
            if(TD_AddEnemy()>=0){td_spawn_left--;td_spawned++;}
            td_spawn_timer=0.78f;
        }
    }
    for(i=0;i<TD_MAX_ENEMIES;i++) if(td_enemies[i].active) {
        TD_Enemy *e=&td_enemies[i];
        float tx=td_path_x[e->waypoint], ty=td_path_y[e->waypoint];
        float dx=tx-e->x,dy=ty-e->y,d=sqrtf(dx*dx+dy*dy);
        if(d<3.0f) {
            e->waypoint++;
            if(e->waypoint>=8){e->active=0;td_lives--;if(td_lives<=0)td_game_over=1;continue;}
        } else if(d>0.001f) {
            float step=e->speed*dt;
            if(step>d)step=d;
            e->x+=(dx/d)*step;e->y+=(dy/d)*step;
        }
    }
    for(i=0;i<TD_MAX_TOWERS;i++) if(td_towers[i].active) {
        TD_Tower *t=&td_towers[i]; int target=-1; float best=-1;
        t->cooldown-=dt;
        if(t->cooldown>0)continue;
        for(j=0;j<TD_MAX_ENEMIES;j++) if(td_enemies[j].active) {
            float d=TD_Dist(t->x,t->y,td_enemies[j].x,td_enemies[j].y);
            if(d<=t->range && (target<0 || td_enemies[j].waypoint>best)) {
                target=j;best=(float)td_enemies[j].waypoint;
            }
        }
        if(target>=0 && TD_AddShot(t->x,t->y,target,t->damage)>=0)t->cooldown=t->rate;
    }
    for(i=0;i<TD_MAX_SHOTS;i++) if(td_shots[i].active) {
        TD_Shot *s=&td_shots[i];
        if(s->target<0 || s->target>=TD_MAX_ENEMIES || !td_enemies[s->target].active){s->active=0;continue;}
        {
            TD_Enemy *e=&td_enemies[s->target];
            float dx=e->x-s->x,dy=e->y-s->y,d=sqrtf(dx*dx+dy*dy);
            float step=s->speed*dt;
            if(d<=step+4.0f) {
                e->hp-=(int)s->damage;s->active=0;
                if(e->hp<=0){e->active=0;td_money+=12;td_kills++;}
            } else if(d>0.001f){s->x+=(dx/d)*step;s->y+=(dy/d)*step;}
        }
    }
}
static void TD_DrawPath(void) {
    int i;
    for(i=0;i<7;i++) {
        float x1=td_path_x[i],y1=td_path_y[i],x2=td_path_x[i+1],y2=td_path_y[i+1];
        if(x1==x2) {
            float y=y1<y2?y1:y2; CE_DrawRect(x1-10,y,20,(y1<y2?y2-y1:y1-y2),GU_COLOR(70,75,83,255));
        } else {
            float x=x1<x2?x1:x2; CE_DrawRect(x,y1-10,(x1<x2?x2-x1:x1-x2),20,GU_COLOR(70,75,83,255));
        }
    }
    for(i=0;i<7;i++) {
        if(td_path_x[i]==td_path_x[i+1]) {
            float y=(td_path_y[i]+td_path_y[i+1])*0.5f;
            CE_DrawRect(td_path_x[i]-1,y-3,2,6,GU_COLOR(230,205,120,255));
        } else {
            float x=(td_path_x[i]+td_path_x[i+1])*0.5f;
            CE_DrawRect(x-3,td_path_y[i]-1,6,2,GU_COLOR(230,205,120,255));
        }
    }
}
static void TD_Render(void) {
    int i;
    sceGuStart(GU_DIRECT,g_displayList);
    sceGuClearColor(GU_COLOR(28,75,52,255)); sceGuClear(GU_COLOR_BUFFER_BIT);
    /* Subtle grass grid. */
    for(i=0;i<16;i++) CE_DrawRect(i*32,0,1,CE_SCREEN_H,GU_COLOR(38,88,60,255));
    for(i=0;i<9;i++) CE_DrawRect(0,i*32,CE_SCREEN_W,1,GU_COLOR(38,88,60,255));
    TD_DrawPath();
    for(i=0;i<TD_PAD_COUNT;i++) {
        unsigned int c=(i==td_cursor)?GU_COLOR(255,220,80,255):GU_COLOR(155,175,145,255);
        CE_DrawRect(td_pad_x[i]-9,td_pad_y[i]-9,18,18,c);
        CE_DrawRect(td_pad_x[i]-5,td_pad_y[i]-5,10,10,GU_COLOR(35,48,38,255));
    }
    for(i=0;i<TD_MAX_TOWERS;i++) if(td_towers[i].active) {
        TD_Tower *t=&td_towers[i];
        unsigned int c=t->type==0?GU_COLOR(70,170,245,255):(t->type==1?GU_COLOR(90,220,145,255):GU_COLOR(235,125,70,255));
        CE_DrawRect(t->x-8,t->y-8,16,16,c);
        CE_DrawRect(t->x-3,t->y-3,6,6,GU_COLOR(245,245,230,255));
        if(i==td_selected) {
            CE_DrawRect(t->x-11,t->y-11,22,2,GU_COLOR(255,240,120,255));
            CE_DrawRect(t->x-11,t->y+9,22,2,GU_COLOR(255,240,120,255));
        }
        CE_DrawRect(t->x-8,t->y+10,16,2,GU_COLOR(20,25,25,255));
        CE_DrawRect(t->x-8,t->y+10,16.0f*t->level/5.0f,2,GU_COLOR(255,215,80,255));
    }
    for(i=0;i<TD_MAX_ENEMIES;i++) if(td_enemies[i].active) {
        TD_Enemy *e=&td_enemies[i];
        CE_DrawRect(e->x-5,e->y-5,10,10,GU_COLOR(190,65,70,255));
        CE_DrawRect(e->x-5,e->y-8,10,2,GU_COLOR(45,25,25,255));
        CE_DrawRect(e->x-5,e->y-8,10.0f*TD_Clamp((float)e->hp/e->maxHp,0,1),2,GU_COLOR(100,235,110,255));
    }
    for(i=0;i<TD_MAX_SHOTS;i++) if(td_shots[i].active)
        CE_DrawRect(td_shots[i].x-2,td_shots[i].y-2,4,4,GU_COLOR(255,235,125,255));
    /* Top status bar and bottom controls legend. */
    CE_DrawRect(0,0,480,22,GU_COLOR(8,15,24,230));
    CE_DrawRect(8,7,TD_Clamp((float)td_lives,0,20)*4.0f,7,GU_COLOR(235,75,85,255));
    CE_DrawRect(100,7,TD_Clamp((float)td_money,0,300)*0.5f,7,GU_COLOR(245,195,65,255));
    CE_DrawRect(270,7,TD_Clamp((float)td_wave,0,20)*7.0f,7,GU_COLOR(80,175,245,255));
    CE_DrawRect(0,264,480,8,GU_COLOR(8,15,24,220));
    /* Button key: Cross=build, Circle=wave, Square=upgrade, Triangle=sell. */
    CE_DrawRect(8,266,8,4,GU_COLOR(70,220,145,255));
    CE_DrawRect(24,266,8,4,GU_COLOR(90,175,245,255));
    CE_DrawRect(40,266,8,4,GU_COLOR(245,195,65,255));
    CE_DrawRect(56,266,8,4,GU_COLOR(235,115,90,255));
    if(td_game_over) CE_DrawRect(150,112,180,44,GU_COLOR(115,25,35,235));
    sceGuFinish(); sceGuSync(0,0); sceDisplayWaitVblankStart(); sceGuSwapBuffers();
}


/* Core timing and startup. */
static void CE_UpdateTime(void) {
    unsigned long long now = sceKernelGetSystemTimeWide();
    unsigned long long elapsed = now-g_lastTime;
    g_lastTime = now;
    g_state.deltaTime = CE_ClampF((float)elapsed/1000000.0f,0,0.05f);
    g_state.totalTime += g_state.deltaTime;
    g_state.fpsTimer += g_state.deltaTime;
    g_state.fpsFrames++;
    if (g_state.fpsTimer >= 1.0f) {
        g_state.fps = g_state.fpsFrames;
        g_state.fpsFrames = 0;
        g_state.fpsTimer -= 1.0f;
    }
}

static void CE_Init(void) {
    memset(&g_state,0,sizeof(g_state));
    memset(g_map,CE_TILE_GRASS,sizeof(g_map));
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    CE_WorldInit();
    TD_Init();
    CE_GraphicsInit();
    g_lastTime = sceKernelGetSystemTimeWide();
}

static void CE_Shutdown(void) {
    sceGuTerm();
}

int main(void) {
    CE_SetupCallbacks();
    CE_Init();

    while (g_running) {
        CE_UpdateTime();
        if (td_mode) {
            TD_Input();
            TD_Update(g_state.deltaTime);
            TD_Render();
        } else {
            CE_InputUpdate();
            if (CE_ButtonPressed(PSP_CTRL_SELECT)) {
                td_mode = 1;
                g_editor = 0;
            }
            if (CE_ButtonPressed(PSP_CTRL_CIRCLE)) {
                g_saveMessage = CE_Save() ? 45 : 0;
            }
            if (CE_ButtonPressed(PSP_CTRL_TRIANGLE)) {
                if (CE_Load()) {
                    g_cursorX = CE_ClampI(g_cursorX,0,CE_MAP_W-1);
                    g_cursorY = CE_ClampI(g_cursorY,0,CE_MAP_H-1);
                }
            }
            if (!g_editor) CE_UpdateObjects(g_state.deltaTime);
            CE_UpdateCamera(g_state.deltaTime);
            CE_Render();
            if (g_saveMessage > 0) g_saveMessage--;
        }
    }

    CE_Shutdown();
    sceKernelExitGame();
    return 0;
}
