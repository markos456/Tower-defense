/*
 * COAST ENGINE v0.5 TD COMPLETE + WORLD
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

PSP_MODULE_INFO("COAST ENGINE TD BIG PORT", 0, 5, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-256);

#define CE_VERSION_MAJOR 0
#define CE_VERSION_MINOR 5

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
/* COAST ENGINE v0.5 — TD COMPLETE PORT                                       */
/* Native PSP systems: maps, modes, large tower/enemy catalog, rune stacking, */
/* challenges, campaign progression, save/load, and an in-game route editor.   */
/* -------------------------------------------------------------------------- */
#define TD_MAX_TOWERS 64
#define TD_MAX_ENEMIES 96
#define TD_MAX_SHOTS 160
#define TD_MAX_RUNES 64
#define TD_MAP_COUNT 20
#define TD_GRID_W 20
#define TD_GRID_H 15
#define TD_CELL_W 24.0f
#define TD_CELL_H 16.0f
#define TD_TOP 24.0f
#define TD_TOWER_COUNT 188
#define TD_ENEMY_TYPES ENEMY_TYPE_COUNT
#define TD_SAVE_PATH "ms0:/COAST_TD_V05.DAT"

enum { TD_E_ARMORED=1,TD_E_GHOST=2,TD_E_INVISIBLE=4,TD_E_HEALER=8,TD_E_BOSS=16,TD_E_SPLIT=32,TD_E_THIEF=64,TD_E_ICE=128 };
enum { TD_RUNE_DAMAGE=0,TD_RUNE_RANGE=1,TD_RUNE_RATE=2,TD_RUNE_CRIT=3,TD_RUNE_SPLASH=4,TD_RUNE_GOLD=5 };
enum { TD_SHOT_SPLASH=1,TD_SHOT_SLOW=2,TD_SHOT_BURN=4,TD_SHOT_POISON=8,TD_SHOT_PIERCE=16,TD_SHOT_STUN=32 };

typedef struct { const char *name; float cost, range, damage, rate, splash; int support, effects; } TD_TowerDef;
typedef struct { const char *name; float speed; int hp,reward,leak,radius,flags; } TD_EnemyDef;
typedef struct { const char *name; int lives,money,waves; float hpMul,moneyMul; int flags; } TD_Mode;
typedef struct { const char *name; int count; short x[12],y[12]; } TD_Map;
typedef struct { unsigned char kind; float power; } TD_Rune;
typedef struct {
    int active,type,level,runeCount;
    float x,y,cooldown,damage,range,rate;
    TD_Rune runes[TD_MAX_RUNES];
} TD_Tower;
typedef struct {
    int active,type,hp,maxHp,waypoint,flags;
    float x,y,speed,slowTimer,burnTimer,burnDmg,poisonTimer,poisonDmg;
} TD_Enemy;
typedef struct { int active,target; float x,y,speed,damage,splash; int flags; } TD_Shot;
typedef struct { unsigned int magic,version; int mode,map,wave,money,lives,kills,bossKills,challenge[6],campaignRegion,customCount; short customX[12],customY[12]; TD_Tower towers[TD_MAX_TOWERS]; } TD_Save;

static TD_TowerDef td_defs[TD_TOWER_COUNT];
static TD_Tower td_towers[TD_MAX_TOWERS];
static TD_Enemy td_enemies[TD_MAX_ENEMIES];
static TD_Shot td_shots[TD_MAX_SHOTS];
static int td_towerCount,td_enemyCount;
static int td_cursorX=1,td_cursorY=1,td_selected=-1,td_type=0;
static int td_wave,td_money,td_lives,td_kills,td_bossKills,td_game_over,td_paused;
static int td_modeIndex=1,td_mapIndex=0,td_menu=0,td_editor=0,td_editorNode=0;
static int td_spawnLeft,td_spawned,td_spawnType,td_campaignRegion,td_campaignStage;
static int td_challenge[6],td_challengeClaimed[6],td_bestWave,td_totalGames,td_waveComplete=1;
static float td_spawnTimer,td_clock,td_betweenTimer;
static short td_customX[12],td_customY[12]; static int td_customCount;
static const char *td_tower_names[TD_TOWER_COUNT];
static int td_radarBuilt,td_waveLeak;
static TD_Save td_saveBuffer;
static unsigned int td_rng=0xC0A57EEDu;
static float TD_Clamp(float v,float a,float b){return v<a?a:(v>b?b:v);}
static float TD_Dist(float ax,float ay,float bx,float by){float x=ax-bx,y=ay-by;return sqrtf(x*x+y*y);}
static unsigned int TD_Rand(void){td_rng^=td_rng<<13;td_rng^=td_rng>>17;td_rng^=td_rng<<5;return td_rng;}
static int TD_Range(int n){return n>0?(int)(TD_Rand()%(unsigned)n):0;}
static void TD_SetDef(int id,const char*n,float c,float r,float d,float rt,int support){
    int fx=0; float splash=0;
    /* Effects are mapped from the original TD v6 tower families. */
    switch(id){
      case 2:case 8:case 10:case 15:case 17:case 22:case 24:case 25:case 26:case 28:case 29:case 31:case 36:case 37: fx|=TD_SHOT_SPLASH; splash= id==17?85.0f:55.0f; break;
      default: break;
    }
    if(id==3||id==15||id==24||id==30||id==36)fx|=TD_SHOT_SLOW;
    if(id==5||id==28||id==36)fx|=TD_SHOT_BURN;
    if(id==7||id==22)fx|=TD_SHOT_POISON;
    if(id==14||id==33||id==37)fx|=TD_SHOT_PIERCE;
    if(id==25)fx|=TD_SHOT_STUN;
    if(id>=38&&id<138){int m=(id-38)%12;if(m==0)fx|=TD_SHOT_SPLASH;else if(m==1)fx|=TD_SHOT_SLOW;else if(m==2)fx|=TD_SHOT_BURN;else if(m==3)fx|=TD_SHOT_POISON;else if(m==4||m==5)fx|=TD_SHOT_PIERCE;else if(m==8)fx|=TD_SHOT_STUN;}
    if(id==11||id==16||id==19||id==32||id>=138){support=1;}
    td_defs[id].name=n;td_defs[id].cost=c;td_defs[id].range=r;td_defs[id].damage=d;td_defs[id].rate=rt;td_defs[id].support=support;td_defs[id].effects=fx;td_defs[id].splash=splash;td_tower_names[id]=n;
}
static void TD_InitDefs(void){
    TD_SetDef(0, "Стрелок", 125.0f, 110.0f, 9.0f, .55f, 0);
    TD_SetDef(1, "Пулемёт", 212.5f, 95.0f, 4.0f, .12f, 0);
    TD_SetDef(2, "Пушка", 325.0f, 130.0f, 28.0f, 1.5f, 0);
    TD_SetDef(3, "Ледяная", 250.0f, 105.0f, 4.0f, .7f, 0);
    TD_SetDef(4, "Снайпер", 400.0f, 250.0f, 90.0f, 2.0f, 0);
    TD_SetDef(5, "Огнемёт", 300.0f, 78.0f, 7.0f, .12f, 0);
    TD_SetDef(6, "Тесла", 475.0f, 125.0f, 20.0f, .9f, 0);
    TD_SetDef(7, "Яд", 350.0f, 115.0f, 6.0f, .9f, 0);
    TD_SetDef(8, "Ракетница", 525.0f, 180.0f, 55.0f, 1.9f, 0);
    TD_SetDef(9, "Лазер", 650.0f, 155.0f, 5.0f, .06f, 0);
    TD_SetDef(10, "Миномёт", 425.0f, 230.0f, 48.0f, 2.4f, 0);
    TD_SetDef(11, "Бустер", 450.0f, 115.0f, 0.0f, 999.0f, 0);
    TD_SetDef(12, "Дробовик", 362.5f, 90.0f, 10.0f, 1.1f, 0);
    TD_SetDef(13, "Гарпун", 337.5f, 130.0f, 22.0f, 1.4f, 0);
    TD_SetDef(14, "Плазма", 462.5f, 140.0f, 16.0f, .65f, 0);
    TD_SetDef(15, "Криопушка", 387.5f, 120.0f, 8.0f, .9f, 0);
    TD_SetDef(16, "Тотем", 500.0f, 110.0f, 0.0f, 999.0f, 0);
    TD_SetDef(17, "Мортира", 700.0f, 320.0f, 85.0f, 3.5f, 0);
    TD_SetDef(18, "Телепорт", 550.0f, 150.0f, 0.0f, 3.0f, 0);
    TD_SetDef(19, "Щит", 425.0f, 100.0f, 0.0f, 999.0f, 0);
    TD_SetDef(20, "Магнит", 487.5f, 130.0f, 3.0f, .5f, 0);
    TD_SetDef(21, "Бензопила", 500.0f, 70.0f, 15.0f, .25f, 0);
    TD_SetDef(22, "Кислота", 400.0f, 120.0f, 10.0f, .7f, 0);
    TD_SetDef(23, "Гроза", 575.0f, 180.0f, 35.0f, 1.6f, 0);
    TD_SetDef(24, "Мороз", 525.0f, 160.0f, 6.0f, .3f, 0);
    TD_SetDef(25, "Землетряс", 700.0f, 140.0f, 60.0f, 3.0f, 0);
    TD_SetDef(26, "Чёрная дыра", 850.0f, 120.0f, 25.0f, 2.5f, 0);
    TD_SetDef(27, "Иллюзия", 450.0f, 130.0f, 12.0f, .6f, 0);
    TD_SetDef(28, "Вулкан", 625.0f, 200.0f, 70.0f, 2.8f, 0);
    TD_SetDef(29, "Осколочная", 475.0f, 140.0f, 35.0f, 1.3f, 0);
    TD_SetDef(30, "Стрела Мороза", 437.5f, 200.0f, 12.0f, .8f, 0);
    TD_SetDef(31, "Гранатомёт", 412.5f, 120.0f, 30.0f, 1.6f, 0);
    TD_SetDef(32, "Святыня", 550.0f, 140.0f, 0.0f, 999.0f, 0);
    TD_SetDef(33, "Призрак", 525.0f, 170.0f, 20.0f, 1.0f, 0);
    TD_SetDef(34, "Вихрь", 500.0f, 110.0f, 18.0f, .4f, 0);
    TD_SetDef(35, "Стрелок Скорости", 475.0f, 130.0f, 6.0f, .1f, 0);
    TD_SetDef(36, "Мастерство", 625.0f, 150.0f, 40.0f, 1.2f, 0);
    TD_SetDef(37, "Абсолют", 1000.0f, 200.0f, 100.0f, 2.0f, 0);
    TD_SetDef(38, "Дротикомёт", 312.5f, 100.0f, 7.0f, 0.34f, 0);
    TD_SetDef(39, "Арбалетная турель", 360.0f, 119.0f, 9.0f, 0.47f, 0);
    TD_SetDef(40, "Буровая пушка", 407.5f, 138.0f, 11.0f, 0.60f, 0);
    TD_SetDef(41, "Рикошет", 455.0f, 157.0f, 13.0f, 0.73f, 0);
    TD_SetDef(42, "Шаровая молния", 502.5f, 176.0f, 15.0f, 0.86f, 0);
    TD_SetDef(43, "Солнечный луч", 550.0f, 195.0f, 17.0f, 0.99f, 0);
    TD_SetDef(44, "Лунный луч", 597.5f, 100.0f, 19.0f, 1.12f, 0);
    TD_SetDef(45, "Кристалломёт", 645.0f, 119.0f, 21.0f, 0.34f, 0);
    TD_SetDef(46, "Грибная пушка", 692.5f, 138.0f, 23.0f, 0.47f, 0);
    TD_SetDef(47, "Ветряк", 740.0f, 157.0f, 7.0f, 0.60f, 0);
    TD_SetDef(48, "Сейсмобур", 332.5f, 176.0f, 9.0f, 0.73f, 0);
    TD_SetDef(49, "Гидропушка", 380.0f, 195.0f, 11.0f, 0.86f, 0);
    TD_SetDef(50, "Паровой автомат", 427.5f, 100.0f, 13.0f, 0.99f, 0);
    TD_SetDef(51, "Медная пушка", 475.0f, 119.0f, 15.0f, 1.12f, 0);
    TD_SetDef(52, "Серебряный луч", 522.5f, 138.0f, 17.0f, 0.34f, 0);
    TD_SetDef(53, "Золотой стрелок", 570.0f, 157.0f, 19.0f, 0.47f, 0);
    TD_SetDef(54, "Рубиновый луч", 617.5f, 176.0f, 21.0f, 0.60f, 0);
    TD_SetDef(55, "Сапфировый луч", 665.0f, 195.0f, 23.0f, 0.73f, 0);
    TD_SetDef(56, "Изумрудный луч", 712.5f, 100.0f, 7.0f, 0.86f, 0);
    TD_SetDef(57, "Обсидиановая пушка", 760.0f, 119.0f, 9.0f, 0.99f, 0);
    TD_SetDef(58, "Песчаный вихрь", 352.5f, 138.0f, 11.0f, 1.12f, 0);
    TD_SetDef(59, "Пылевой шторм", 400.0f, 157.0f, 13.0f, 0.34f, 0);
    TD_SetDef(60, "Лавовый плевок", 447.5f, 176.0f, 15.0f, 0.47f, 0);
    TD_SetDef(61, "Магма-пушка", 495.0f, 195.0f, 17.0f, 0.60f, 0);
    TD_SetDef(62, "Ледяной кристалл", 542.5f, 100.0f, 19.0f, 0.73f, 0);
    TD_SetDef(63, "Снежная турель", 590.0f, 119.0f, 22.0f, 0.86f, 0);
    TD_SetDef(64, "Полярный луч", 637.5f, 138.0f, 24.0f, 0.99f, 0);
    TD_SetDef(65, "Северное сияние", 685.0f, 157.0f, 8.0f, 1.12f, 0);
    TD_SetDef(66, "Токсичный распылитель", 732.5f, 176.0f, 10.0f, 0.34f, 0);
    TD_SetDef(67, "Споровая пушка", 780.0f, 195.0f, 12.0f, 0.47f, 0);
    TD_SetDef(68, "Кислотный дождь", 372.5f, 100.0f, 14.0f, 0.60f, 0);
    TD_SetDef(69, "Биолуч", 420.0f, 119.0f, 16.0f, 0.73f, 0);
    TD_SetDef(70, "Вакуумный импульс", 467.5f, 138.0f, 18.0f, 0.86f, 0);
    TD_SetDef(71, "Гравитационный луч", 515.0f, 157.0f, 20.0f, 0.99f, 0);
    TD_SetDef(72, "Пульсар", 562.5f, 176.0f, 22.0f, 1.12f, 0);
    TD_SetDef(73, "Квазар", 610.0f, 195.0f, 24.0f, 0.34f, 0);
    TD_SetDef(74, "Нейтронная пушка", 657.5f, 100.0f, 8.0f, 0.47f, 0);
    TD_SetDef(75, "Протонный луч", 705.0f, 119.0f, 10.0f, 0.60f, 0);
    TD_SetDef(76, "Антиматерийная турель", 752.5f, 138.0f, 12.0f, 0.73f, 0);
    TD_SetDef(77, "Квантовый стрелок", 800.0f, 157.0f, 14.0f, 0.86f, 0);
    TD_SetDef(78, "Хрономёт", 392.5f, 176.0f, 16.0f, 0.99f, 0);
    TD_SetDef(79, "Замедлитель времени", 440.0f, 195.0f, 18.0f, 1.12f, 0);
    TD_SetDef(80, "Временной разлом", 487.5f, 100.0f, 20.0f, 0.34f, 0);
    TD_SetDef(81, "Фазовая пушка", 535.0f, 119.0f, 22.0f, 0.47f, 0);
    TD_SetDef(82, "Эхо-турель", 582.5f, 138.0f, 24.0f, 0.60f, 0);
    TD_SetDef(83, "Звуковая волна", 630.0f, 157.0f, 8.0f, 0.73f, 0);
    TD_SetDef(84, "Ультразвук", 677.5f, 176.0f, 10.0f, 0.86f, 0);
    TD_SetDef(85, "Резонатор", 725.0f, 195.0f, 12.0f, 0.99f, 0);
    TD_SetDef(86, "Рельсотрон МК-II", 772.5f, 100.0f, 14.0f, 1.12f, 0);
    TD_SetDef(87, "Магнитный рельсотрон", 820.0f, 119.0f, 16.0f, 0.34f, 0);
    TD_SetDef(88, "Импульсная пушка", 412.5f, 138.0f, 19.0f, 0.47f, 0);
    TD_SetDef(89, "Электромагнит", 460.0f, 157.0f, 21.0f, 0.60f, 0);
    TD_SetDef(90, "Тесла МК-II", 507.5f, 176.0f, 23.0f, 0.73f, 0);
    TD_SetDef(91, "Грозовой столб", 555.0f, 195.0f, 25.0f, 0.86f, 0);
    TD_SetDef(92, "Штормогенератор", 602.5f, 100.0f, 9.0f, 0.99f, 0);
    TD_SetDef(93, "Молниемёт", 650.0f, 119.0f, 11.0f, 1.12f, 0);
    TD_SetDef(94, "Плазменный резак", 697.5f, 138.0f, 13.0f, 0.34f, 0);
    TD_SetDef(95, "Плазменная сфера", 745.0f, 157.0f, 15.0f, 0.47f, 0);
    TD_SetDef(96, "Сгусток энергии", 792.5f, 176.0f, 17.0f, 0.60f, 0);
    TD_SetDef(97, "Энергоядро", 840.0f, 195.0f, 19.0f, 0.73f, 0);
    TD_SetDef(98, "Фотонная пушка", 432.5f, 100.0f, 21.0f, 0.86f, 0);
    TD_SetDef(99, "Гамма-излучатель", 480.0f, 119.0f, 23.0f, 0.99f, 0);
    TD_SetDef(100, "Рентген-турель", 527.5f, 138.0f, 25.0f, 1.12f, 0);
    TD_SetDef(101, "Радиант", 575.0f, 157.0f, 9.0f, 0.34f, 0);
    TD_SetDef(102, "Световая призма", 622.5f, 176.0f, 11.0f, 0.47f, 0);
    TD_SetDef(103, "Призматический луч", 670.0f, 195.0f, 13.0f, 0.60f, 0);
    TD_SetDef(104, "Осколкомёт МК-II", 717.5f, 100.0f, 15.0f, 0.73f, 0);
    TD_SetDef(105, "Кассетная пушка", 765.0f, 119.0f, 17.0f, 0.86f, 0);
    TD_SetDef(106, "Дробитель", 812.5f, 138.0f, 19.0f, 0.99f, 0);
    TD_SetDef(107, "Кластерная турель", 860.0f, 157.0f, 21.0f, 1.12f, 0);
    TD_SetDef(108, "Бомбардир", 452.5f, 176.0f, 23.0f, 0.34f, 0);
    TD_SetDef(109, "Тяжёлый бомбардир", 500.0f, 195.0f, 25.0f, 0.47f, 0);
    TD_SetDef(110, "Огненный шар", 547.5f, 100.0f, 9.0f, 0.60f, 0);
    TD_SetDef(111, "Пепельная пушка", 595.0f, 119.0f, 11.0f, 0.73f, 0);
    TD_SetDef(112, "Факельная турель", 642.5f, 138.0f, 13.0f, 0.86f, 0);
    TD_SetDef(113, "Драконье дыхание", 690.0f, 157.0f, 16.0f, 0.99f, 0);
    TD_SetDef(114, "Морозный залп", 737.5f, 176.0f, 18.0f, 1.12f, 0);
    TD_SetDef(115, "Ледяная буря", 785.0f, 195.0f, 20.0f, 0.34f, 0);
    TD_SetDef(116, "Снежный ком", 832.5f, 100.0f, 22.0f, 0.47f, 0);
    TD_SetDef(117, "Ледяной метеор", 880.0f, 119.0f, 24.0f, 0.60f, 0);
    TD_SetDef(118, "Корнестрел", 472.5f, 138.0f, 26.0f, 0.73f, 0);
    TD_SetDef(119, "Шипастая турель", 520.0f, 157.0f, 10.0f, 0.86f, 0);
    TD_SetDef(120, "Лесной страж", 567.5f, 176.0f, 12.0f, 0.99f, 0);
    TD_SetDef(121, "Каменный метатель", 615.0f, 195.0f, 14.0f, 1.12f, 0);
    TD_SetDef(122, "Гранитная пушка", 662.5f, 100.0f, 16.0f, 0.34f, 0);
    TD_SetDef(123, "Кристальный страж", 710.0f, 119.0f, 18.0f, 0.47f, 0);
    TD_SetDef(124, "Рой дронов", 757.5f, 138.0f, 20.0f, 0.60f, 0);
    TD_SetDef(125, "Дрон-охотник", 805.0f, 157.0f, 22.0f, 0.73f, 0);
    TD_SetDef(126, "Микроракеты", 852.5f, 176.0f, 24.0f, 0.86f, 0);
    TD_SetDef(127, "Ракетный рой", 900.0f, 195.0f, 26.0f, 0.99f, 0);
    TD_SetDef(128, "Автопушка", 492.5f, 100.0f, 10.0f, 1.12f, 0);
    TD_SetDef(129, "Двойной стрелок", 540.0f, 119.0f, 12.0f, 0.34f, 0);
    TD_SetDef(130, "Тройной залп", 587.5f, 138.0f, 14.0f, 0.47f, 0);
    TD_SetDef(131, "Барабанная пушка", 635.0f, 157.0f, 16.0f, 0.60f, 0);
    TD_SetDef(132, "Скорострел", 682.5f, 176.0f, 18.0f, 0.73f, 0);
    TD_SetDef(133, "Тяжёлый скорострел", 730.0f, 195.0f, 20.0f, 0.86f, 0);
    TD_SetDef(134, "Дальний дозор", 777.5f, 100.0f, 22.0f, 0.99f, 0);
    TD_SetDef(135, "Орбитальная пушка", 825.0f, 119.0f, 24.0f, 1.12f, 0);
    TD_SetDef(136, "Звёздный разрушитель", 872.5f, 138.0f, 26.0f, 0.34f, 0);
    TD_SetDef(137, "Финальная турель", 920.0f, 157.0f, 10.0f, 0.47f, 0);
    TD_SetDef(138, "Маяк урона 1", 375.0f, 115.0f, 0.0f, 999.0f, 1);
    TD_SetDef(139, "Маяк урона 2", 382.0f, 123.0f, 0.0f, 999.0f, 1);
    TD_SetDef(140, "Маяк урона 3", 390.0f, 131.0f, 0.0f, 999.0f, 1);
    TD_SetDef(141, "Маяк урона 4", 397.0f, 139.0f, 0.0f, 999.0f, 1);
    TD_SetDef(142, "Маяк урона 5", 405.0f, 147.0f, 0.0f, 999.0f, 1);
    TD_SetDef(143, "Ритм-башня 1", 412.0f, 115.0f, 0.0f, 999.0f, 1);
    TD_SetDef(144, "Ритм-башня 2", 420.0f, 123.0f, 0.0f, 999.0f, 1);
    TD_SetDef(145, "Ритм-башня 3", 427.0f, 131.0f, 0.0f, 999.0f, 1);
    TD_SetDef(146, "Ритм-башня 4", 435.0f, 139.0f, 0.0f, 999.0f, 1);
    TD_SetDef(147, "Ритм-башня 5", 442.0f, 147.0f, 0.0f, 999.0f, 1);
    TD_SetDef(148, "Оптический маяк 1", 450.0f, 115.0f, 0.0f, 999.0f, 1);
    TD_SetDef(149, "Оптический маяк 2", 457.0f, 123.0f, 0.0f, 999.0f, 1);
    TD_SetDef(150, "Оптический маяк 3", 465.0f, 131.0f, 0.0f, 999.0f, 1);
    TD_SetDef(151, "Оптический маяк 4", 472.0f, 139.0f, 0.0f, 999.0f, 1);
    TD_SetDef(152, "Оптический маяк 5", 480.0f, 147.0f, 0.0f, 999.0f, 1);
    TD_SetDef(153, "Критический тотем 1", 487.0f, 115.0f, 0.0f, 999.0f, 1);
    TD_SetDef(154, "Критический тотем 2", 495.0f, 123.0f, 0.0f, 999.0f, 1);
    TD_SetDef(155, "Критический тотем 3", 502.0f, 131.0f, 0.0f, 999.0f, 1);
    TD_SetDef(156, "Критический тотем 4", 510.0f, 139.0f, 0.0f, 999.0f, 1);
    TD_SetDef(157, "Критический тотем 5", 517.0f, 147.0f, 0.0f, 999.0f, 1);
    TD_SetDef(158, "Радиусный узел 1", 525.0f, 115.0f, 0.0f, 999.0f, 1);
    TD_SetDef(159, "Радиусный узел 2", 532.0f, 123.0f, 0.0f, 999.0f, 1);
    TD_SetDef(160, "Радиусный узел 3", 540.0f, 131.0f, 0.0f, 999.0f, 1);
    TD_SetDef(161, "Радиусный узел 4", 547.0f, 139.0f, 0.0f, 999.0f, 1);
    TD_SetDef(162, "Радиусный узел 5", 555.0f, 147.0f, 0.0f, 999.0f, 1);
    TD_SetDef(163, "Пиромантский маяк 1", 562.0f, 115.0f, 0.0f, 999.0f, 1);
    TD_SetDef(164, "Пиромантский маяк 2", 570.0f, 123.0f, 0.0f, 999.0f, 1);
    TD_SetDef(165, "Пиромантский маяк 3", 577.0f, 131.0f, 0.0f, 999.0f, 1);
    TD_SetDef(166, "Пиромантский маяк 4", 585.0f, 139.0f, 0.0f, 999.0f, 1);
    TD_SetDef(167, "Пиромантский маяк 5", 592.0f, 147.0f, 0.0f, 999.0f, 1);
    TD_SetDef(168, "Токсичный ретранслятор 1", 600.0f, 115.0f, 0.0f, 999.0f, 1);
    TD_SetDef(169, "Токсичный ретранслятор 2", 607.0f, 123.0f, 0.0f, 999.0f, 1);
    TD_SetDef(170, "Токсичный ретранслятор 3", 615.0f, 131.0f, 0.0f, 999.0f, 1);
    TD_SetDef(171, "Токсичный ретранслятор 4", 622.0f, 139.0f, 0.0f, 999.0f, 1);
    TD_SetDef(172, "Токсичный ретранслятор 5", 630.0f, 147.0f, 0.0f, 999.0f, 1);
    TD_SetDef(173, "Ледяной генератор 1", 637.0f, 115.0f, 0.0f, 999.0f, 1);
    TD_SetDef(174, "Ледяной генератор 2", 645.0f, 123.0f, 0.0f, 999.0f, 1);
    TD_SetDef(175, "Ледяной генератор 3", 652.0f, 131.0f, 0.0f, 999.0f, 1);
    TD_SetDef(176, "Ледяной генератор 4", 660.0f, 139.0f, 0.0f, 999.0f, 1);
    TD_SetDef(177, "Ледяной генератор 5", 667.0f, 147.0f, 0.0f, 999.0f, 1);
    TD_SetDef(178, "Грозовой усилитель 1", 675.0f, 115.0f, 0.0f, 999.0f, 1);
    TD_SetDef(179, "Грозовой усилитель 2", 682.0f, 123.0f, 0.0f, 999.0f, 1);
    TD_SetDef(180, "Грозовой усилитель 3", 690.0f, 131.0f, 0.0f, 999.0f, 1);
    TD_SetDef(181, "Грозовой усилитель 4", 697.0f, 139.0f, 0.0f, 999.0f, 1);
    TD_SetDef(182, "Грозовой усилитель 5", 705.0f, 147.0f, 0.0f, 999.0f, 1);
    TD_SetDef(183, "Ракетный ускоритель 1", 712.0f, 115.0f, 0.0f, 999.0f, 1);
    TD_SetDef(184, "Ракетный ускоритель 2", 720.0f, 123.0f, 0.0f, 999.0f, 1);
    TD_SetDef(185, "Ракетный ускоритель 3", 727.0f, 131.0f, 0.0f, 999.0f, 1);
    TD_SetDef(186, "Ракетный ускоритель 4", 735.0f, 139.0f, 0.0f, 999.0f, 1);
    TD_SetDef(187, "Ракетный ускоритель 5", 742.0f, 147.0f, 0.0f, 999.0f, 1);
}


static const TD_EnemyDef td_enemyDefs[]={
    {"Зомби",58.0f,26,8,1,11,0},
    {"Бегун",118.0f,16,10,1,9,0},
    {"Танк",36.0f,85,20,2,15,0},
    {"БОСС",30.0f,520,90,5,22,16},
    {"Броненосец",42.0f,55,16,2,13,1},
    {"Лекарь",52.0f,40,15,1,12,8},
    {"Делитель",48.0f,60,18,1,14,32},
    {"Призрак",70.0f,35,15,1,12,2},
    {"Щитоносец",40.0f,75,22,2,14,0},
    {"Камикадзе",90.0f,30,14,3,11,0},
    {"Мини",70.0f,15,4,1,7,0},
    {"Мини-босс",45.0f,200,40,3,18,0},
    {"МЕГА-БОСС",22.0f,2000,300,10,30,16},
    {"Прыгун",80.0f,30,18,1,10,0},
    {"Магнитоголовый",50.0f,80,25,2,14,0},
    {"Каменюка",25.0f,200,30,3,18,1},
    {"Улей",65.0f,60,22,1,13,0},
    {"Ледяной",55.0f,50,18,1,12,128},
    {"Цементный",20.0f,300,35,4,16,1},
    {"Королева",40.0f,400,120,6,20,24},
    {"Клон",60.0f,45,20,2,12,0},
    {"Ниндзя",130.0f,40,25,2,11,2},
    {"Элементаль",45.0f,120,30,2,15,9},
    {"Титан",30.0f,800,180,5,24,17},
    {"Призрак-мать",35.0f,250,70,3,18,2},
    {"Туманщик",43.0f,105,34,2,14,0},
    {"Морозный вредитель",48.0f,85,28,2,13,0},
    {"ЭМИ-зомби",42.0f,100,35,2,14,0},
    {"Паутинщик",50.0f,90,30,2,13,0},
    {"Токсик",46.0f,95,32,2,13,0},
    {"Крикун",58.0f,82,30,2,12,0},
    {"Магнитный",40.0f,125,38,3,15,0},
    {"Теневой",72.0f,78,34,2,12,0},
    {"Вихревой",66.0f,90,34,2,13,0},
    {"Химик",44.0f,112,36,2,14,0},
    {"Заразитель",45.0f,110,36,2,14,0},
    {"Камнелом",32.0f,190,42,3,17,0},
    {"Диверсант",62.0f,92,35,2,13,0},
    {"Усыпляющий",38.0f,120,40,2,14,0},
    {"Глушитель",48.0f,115,38,2,14,0},
    {"Иллюзионист",55.0f,100,40,2,13,0},
    {"Вор",92.0f,72,32,2,11,64},
    {"Замораживатель",42.0f,130,42,3,15,0},
    {"Подавитель",43.0f,145,44,3,15,0},
    {"Полководец",34.0f,230,65,4,18,0},
    {"Невидимка",92.0f,42,18,1,11,6},
};
#define ENEMY_TYPE_COUNT ((int)(sizeof(td_enemyDefs)/sizeof(td_enemyDefs[0])))
static const TD_Mode td_modes[]={
    {"Обычный",20,350,30,1.00f,1.00f,0},
    {"Бесконечный",20,320,9999,1.00f,1.00f,0},
    {"Хардкор",15,280,9999,1.60f,.75f,0},
    {"Одни боссы",25,500,9999,1.00f,1.00f,1},
    {"Хаос",30,500,9999,1.30f,1.20f,2},
    {"Босс-раш",40,800,30,1.40f,1.50f,1},
    {"Защита",25,400,25,1.00f,1.00f,4},
    {"Ночная оборона",18,380,30,1.18f,1.05f,8},
    {"Экономический",22,520,35,1.00f,1.10f,16},
    {"Минимализм",15,260,30,1.25f,1.25f,32},
    {"Апокалипсис",12,600,40,1.80f,1.35f,2},
    {"Песочница",999,99999,9999,1.00f,1.00f,64},
    {"Кампания",25,500,100,1.08f,1.12f,128}
};
static const TD_Map td_maps[]={
    {"Змейка",8,{-1,14,14,3,3,15,15,19,0,0,0,0},{2,2,6,6,10,10,13,13,0,0,0,0}},
    {"Спираль",10,{-1,16,16,2,2,13,13,6,6,19,0,0},{1,1,14,14,4,4,11,11,7,7,0,0}},
    {"Зигзаг",8,{-1,4,4,9,9,14,14,19,0,0,0,0},{3,3,10,10,3,3,10,10,0,0,0,0}},
    {"Диагональ",8,{-1,5,5,10,10,15,15,19,0,0,0,0},{1,1,5,5,9,9,13,13,0,0,0,0}},
    {"Кольцо",8,{-1,17,17,2,2,11,11,19,0,0,0,0},{2,2,13,13,7,7,10,10,0,0,0,0}},
    {"Лабиринт",10,{-1,8,8,2,2,12,12,5,5,19,0,0},{1,1,4,4,8,8,11,11,14,14,0,0}},
    {"Пустыня",8,{-1,5,5,15,15,3,3,19,0,0,0,0},{2,2,5,5,12,12,14,14,0,0,0,0}},
    {"Вулкан",8,{-1,4,4,10,10,16,16,19,0,0,0,0},{13,13,3,3,10,10,1,1,0,0,0,0}},
    {"Снежная долина",8,{-1,15,15,3,3,17,17,19,0,0,0,0},{1,1,5,5,10,10,14,14,0,0,0,0}},
    {"Тропический остров",8,{-1,4,4,12,12,7,7,19,0,0,0,0},{7,7,2,2,12,12,5,5,0,0,0,0}},
    {"Мегаполис",8,{-1,7,7,2,2,14,14,19,0,0,0,0},{2,2,6,6,12,12,8,8,0,0,0,0}},
    {"Заброшенный город",10,{-1,3,3,11,11,6,6,17,17,19,0,0},{13,13,3,3,8,8,12,12,2,2,0,0}},
    {"Тёмный лес",8,{-1,3,3,8,8,14,14,19,0,0,0,0},{1,1,9,9,3,3,13,13,0,0,0,0}},
    {"Фабрика",10,{-1,5,5,12,12,3,3,16,16,19,0,0},{4,4,1,1,8,8,13,13,5,5,0,0}},
    {"Средневековый замок",8,{-1,16,16,4,4,13,13,19,0,0,0,0},{2,2,6,6,11,11,14,14,0,0,0,0}},
    {"Зона радиации",8,{-1,6,6,12,12,2,2,19,0,0,0,0},{12,12,3,3,9,9,1,1,0,0,0,0}},
    {"Затопленный город",8,{-1,10,10,4,4,15,15,19,0,0,0,0},{1,1,5,5,12,12,7,7,0,0,0,0}},
    {"Космическая станция",8,{-1,4,4,15,15,8,8,19,0,0,0,0},{7,7,2,2,11,11,5,5,0,0,0,0}},
    {"Горный перевал",8,{-1,4,4,9,9,15,15,19,0,0,0,0},{14,14,8,8,2,2,10,10,0,0,0,0}},
    {"Чужая планета",8,{-1,6,6,12,12,3,3,19,0,0,0,0},{3,3,12,12,6,6,1,1,0,0,0,0}},
};
static const char *td_challengeNames[6]={"Уничтожить 100 врагов","Победить 10 боссов","Дойти до волны 20","Построить 10 башен","Пройти 5 волн без потерь","Дойти до 50-й волны"};
static const int td_challengeGoals[6]={100,10,20,10,5,50};
static int td_challengeValue[6];
static const char *td_mapName(void){return td_mapIndex==20?"Своя карта":td_maps[td_mapIndex].name;}
static void TD_MapPoint(int i,float *x,float *y){const TD_Map*m=&td_maps[td_mapIndex];int px=m->x[i],py=m->y[i];*x=px*TD_CELL_W+TD_CELL_W*.5f;*y=TD_TOP+py*TD_CELL_H+TD_CELL_H*.5f;}
static int TD_PathCount(void){return td_mapIndex==20?td_customCount:td_maps[td_mapIndex].count;}
static void TD_GetPathPoint(int i,float*x,float*y){if(td_mapIndex==20){*x=td_customX[i]*TD_CELL_W+TD_CELL_W*.5f;*y=TD_TOP+td_customY[i]*TD_CELL_H+TD_CELL_H*.5f;}else TD_MapPoint(i,x,y);}
static int TD_NearPath(float x,float y){int i,n=TD_PathCount();for(i=0;i<n-1;i++){float ax,ay,bx,by,dx,dy,t,px,py;TD_GetPathPoint(i,&ax,&ay);TD_GetPathPoint(i+1,&bx,&by);dx=bx-ax;dy=by-ay;t=((x-ax)*dx+(y-ay)*dy)/(dx*dx+dy*dy+0.001f);t=TD_Clamp(t,0,1);px=ax+t*dx;py=ay+t*dy;if(TD_Dist(x,y,px,py)<11.0f)return 1;}return 0;}
static void TD_ResetGame(void){memset(td_towers,0,sizeof(td_towers));memset(td_enemies,0,sizeof(td_enemies));memset(td_shots,0,sizeof(td_shots));td_wave=td_kills=td_bossKills=td_game_over=td_paused=0;td_towerCount=0;td_lives=td_modes[td_modeIndex].lives;td_money=td_modes[td_modeIndex].money;td_spawnLeft=0;td_spawnTimer=0;td_betweenTimer=3;td_selected=-1;td_waveLeak=0;td_radarBuilt=0;td_waveComplete=1;memset(td_challengeValue,0,sizeof(td_challengeValue));memset(td_challengeClaimed,0,sizeof(td_challengeClaimed));td_totalGames++;}
static int TD_AliveEnemies(void){int i,n=0;for(i=0;i<TD_MAX_ENEMIES;i++)if(td_enemies[i].active)n++;return n;}
static int TD_AddEnemy(int type){int i;for(i=0;i<TD_MAX_ENEMIES;i++)if(!td_enemies[i].active){TD_Enemy*e=&td_enemies[i];float x,y;memset(e,0,sizeof(*e));TD_GetPathPoint(0,&x,&y);e->active=1;e->type=type;e->flags=td_enemyDefs[type].flags;e->x=x;e->y=y;e->waypoint=1;e->speed=td_enemyDefs[type].speed*(1.0f+td_wave*.012f);e->maxHp=e->hp=(int)(td_enemyDefs[type].hp*powf(1.14f,td_wave-1)*td_modes[td_modeIndex].hpMul);if(e->maxHp<1)e->maxHp=1;return i;}return -1;}
static int TD_AddShot(float x,float y,int target,float dmg,float splash,int flags){int i;for(i=0;i<TD_MAX_SHOTS;i++)if(!td_shots[i].active){td_shots[i].active=1;td_shots[i].x=x;td_shots[i].y=y;td_shots[i].target=target;td_shots[i].damage=dmg;td_shots[i].splash=splash;td_shots[i].flags=flags;td_shots[i].speed=260;return i;}return -1;}
static int TD_SelectEnemyType(void){int n=ENEMY_TYPE_COUNT;if(td_modes[td_modeIndex].flags&1){int bosses[6],c=0,i;for(i=0;i<n;i++)if(td_enemyDefs[i].flags&TD_E_BOSS)bosses[c++]=i;return bosses[TD_Range(c)];}if(td_wave>=25&&TD_Range(4)==0){for(int i=0;i<n;i++)if(td_enemyDefs[i].flags&TD_E_INVISIBLE)return i;}if(td_wave>=20&&TD_Range(100)<8)return TD_Range(n);if(td_wave>=15&&TD_Range(100)<12)return TD_Range(n);if(td_wave>=9&&TD_Range(100)<20)return 4+TD_Range(n-4);if(td_wave>=5&&TD_Range(100)<25)return 1+TD_Range(4);return 0;}
static void TD_StartWave(void){if(td_game_over||td_spawnLeft||TD_AliveEnemies())return;td_wave++;td_waveLeak=0;td_waveComplete=0;td_spawnLeft=5+td_wave*2;if(td_spawnLeft>70)td_spawnLeft=70;if(td_modes[td_modeIndex].flags&1)td_spawnLeft=td_wave%5==0?2:1;if(td_modes[td_modeIndex].flags&128)td_spawnLeft=8+td_wave*2;td_spawnTimer=.15f;td_money+=(int)((20+td_wave*4)*td_modes[td_modeIndex].moneyMul);if(td_money>999999)td_money=999999;}
static void TD_AddRune(TD_Tower*t){TD_Rune*r;if(!t||t->runeCount>=TD_MAX_RUNES||td_money<75)return;td_money-=75;r=&t->runes[t->runeCount++];r->kind=(unsigned char)TD_Range(6);r->power=1.08f+(TD_Range(8)*.01f);if(r->kind==TD_RUNE_CRIT)r->power=.04f+(TD_Range(6)*.01f);}
static void TD_PlaceTower(void){int i,slot=-1;float x=td_cursorX*TD_CELL_W+12,y=TD_TOP+td_cursorY*TD_CELL_H+8;TD_Tower*t;if(TD_NearPath(x,y))return;for(i=0;i<TD_MAX_TOWERS;i++){if(td_towers[i].active&&TD_Dist(td_towers[i].x,td_towers[i].y,x,y)<14){td_selected=i;return;}if(!td_towers[i].active&&slot<0)slot=i;}if(slot<0||td_type<0||td_type>=TD_TOWER_COUNT)return;if(td_money<td_defs[td_type].cost)return;if((td_modes[td_modeIndex].flags&32)&&td_towerCount>=12)return;td_money-=(int)td_defs[td_type].cost;t=&td_towers[slot];memset(t,0,sizeof(*t));t->active=1;t->type=td_type;t->level=1;if(td_type==138)td_radarBuilt=1;t->x=x;t->y=y;t->range=td_defs[td_type].range;t->damage=td_defs[td_type].damage;t->rate=td_defs[td_type].rate;t->runeCount=0;td_towerCount++;td_selected=slot;}
static void TD_Upgrade(void){TD_Tower*t;int cost;if(td_selected<0||td_selected>=TD_MAX_TOWERS)return;t=&td_towers[td_selected];if(!t->active||t->level>=10)return;cost=50*t->level;if(td_money<cost)return;td_money-=cost;t->level++;t->damage*=1.22f;t->range+=4;t->rate*=.94f;}
static void TD_Sell(void){TD_Tower*t;if(td_selected<0||td_selected>=TD_MAX_TOWERS)return;t=&td_towers[td_selected];if(t->active){td_money+=(int)(td_defs[t->type].cost*.55f);memset(t,0,sizeof(*t));if(td_towerCount>0)td_towerCount--;td_selected=-1;}}
static void TD_SaveGame(void){TD_Save*s=&td_saveBuffer;SceUID fd;memset(s,0,sizeof(*s));s->magic=0x54444335;s->version=5;s->mode=td_modeIndex;s->map=td_mapIndex;s->wave=td_wave;s->money=td_money;s->lives=td_lives;s->kills=td_kills;s->bossKills=td_bossKills;s->campaignRegion=td_campaignRegion;s->customCount=td_customCount;memcpy(s->customX,td_customX,sizeof(td_customX));memcpy(s->customY,td_customY,sizeof(td_customY));memcpy(s->towers,td_towers,sizeof(td_towers));fd=sceIoOpen(TD_SAVE_PATH,PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC,0777);if(fd>=0){sceIoWrite(fd,s,sizeof(*s));sceIoClose(fd);}}
static void TD_LoadGame(void){TD_Save*s=&td_saveBuffer;SceUID fd=sceIoOpen(TD_SAVE_PATH,PSP_O_RDONLY,0777);if(fd<0)return;if(sceIoRead(fd,s,sizeof(*s))==(int)sizeof(*s)&&s->magic==0x54444335&&s->version==5){td_modeIndex=(int)TD_Clamp(s->mode,0,12);td_mapIndex=(int)TD_Clamp(s->map,0,20);td_wave=s->wave;td_money=s->money;td_lives=s->lives;td_kills=s->kills;td_bossKills=s->bossKills;td_campaignRegion=s->campaignRegion;td_customCount=(int)TD_Clamp(s->customCount,0,12);memcpy(td_customX,s->customX,sizeof(td_customX));memcpy(td_customY,s->customY,sizeof(td_customY));memcpy(td_towers,s->towers,sizeof(td_towers));td_towerCount=0;for(int i=0;i<TD_MAX_TOWERS;i++)if(td_towers[i].active)td_towerCount++;}sceIoClose(fd);}
static void TD_ChallengeUpdate(void){int i;td_challengeValue[0]=td_kills;td_challengeValue[1]=td_bossKills;td_challengeValue[2]=td_wave;td_challengeValue[3]=td_towerCount;td_challengeValue[5]=td_bestWave;for(i=0;i<6;i++)if(!td_challengeClaimed[i]&&td_challengeValue[i]>=td_challengeGoals[i]){td_challengeClaimed[i]=1;td_money+=50+i*25;}}
static void TD_Input(void){SceCtrlData p;int i;g_state.previousButtons=g_state.buttons;sceCtrlPeekBufferPositive(&p,1);g_state.buttons=p.Buttons;if(CE_ButtonPressed(PSP_CTRL_START)){td_paused=!td_paused;return;}if(td_menu){if(CE_ButtonPressed(PSP_CTRL_LEFT))td_modeIndex=(td_modeIndex+12)%13;if(CE_ButtonPressed(PSP_CTRL_RIGHT))td_modeIndex=(td_modeIndex+1)%13;if(CE_ButtonPressed(PSP_CTRL_UP))td_mapIndex=(td_mapIndex+19)%20;if(CE_ButtonPressed(PSP_CTRL_DOWN))td_mapIndex=(td_mapIndex+1)%20;if(CE_ButtonPressed(PSP_CTRL_CROSS)){TD_ResetGame();td_menu=0;}if(CE_ButtonPressed(PSP_CTRL_CIRCLE))TD_LoadGame();if(CE_ButtonPressed(PSP_CTRL_TRIANGLE)){td_editor=1;td_menu=0;td_customCount=0;td_mapIndex=20;td_cursorX=0;td_cursorY=7;}return;}if(CE_ButtonPressed(PSP_CTRL_SELECT)){td_menu=1;return;}if(td_editor){if(CE_ButtonPressed(PSP_CTRL_LEFT))td_cursorX=(td_cursorX+19)%20;if(CE_ButtonPressed(PSP_CTRL_RIGHT))td_cursorX=(td_cursorX+1)%20;if(CE_ButtonPressed(PSP_CTRL_UP))td_cursorY=(td_cursorY+14)%15;if(CE_ButtonPressed(PSP_CTRL_DOWN))td_cursorY=(td_cursorY+1)%15;if(CE_ButtonPressed(PSP_CTRL_CROSS)){if(td_customCount<12){td_customX[td_customCount]=td_cursorX;td_customY[td_customCount]=td_cursorY;td_customCount++;}}if(CE_ButtonPressed(PSP_CTRL_CIRCLE)){if(td_customCount>=2){td_mapIndex=20;td_editor=0;TD_ResetGame();}}if(CE_ButtonPressed(PSP_CTRL_TRIANGLE)){td_customCount=0;}return;}if(CE_ButtonPressed(PSP_CTRL_LEFT))td_cursorX=(td_cursorX+19)%20;if(CE_ButtonPressed(PSP_CTRL_RIGHT))td_cursorX=(td_cursorX+1)%20;if(CE_ButtonPressed(PSP_CTRL_UP))td_cursorY=(td_cursorY+14)%15;if(CE_ButtonPressed(PSP_CTRL_DOWN))td_cursorY=(td_cursorY+1)%15;if(CE_ButtonPressed(PSP_CTRL_LTRIGGER))td_type=(td_type+TD_TOWER_COUNT-1)%TD_TOWER_COUNT;if(CE_ButtonPressed(PSP_CTRL_RTRIGGER))td_type=(td_type+1)%TD_TOWER_COUNT;if(CE_ButtonPressed(PSP_CTRL_CROSS)){if((p.Buttons&PSP_CTRL_LTRIGGER)&&td_selected>=0)TD_AddRune(&td_towers[td_selected]);else TD_PlaceTower();}if(CE_ButtonPressed(PSP_CTRL_CIRCLE))TD_StartWave();if(CE_ButtonPressed(PSP_CTRL_SQUARE))TD_Upgrade();if(CE_ButtonPressed(PSP_CTRL_LTRIGGER)&&CE_ButtonPressed(PSP_CTRL_TRIANGLE))TD_SaveGame();else if(CE_ButtonPressed(PSP_CTRL_RTRIGGER)&&CE_ButtonPressed(PSP_CTRL_TRIANGLE))TD_LoadGame();else if(CE_ButtonPressed(PSP_CTRL_TRIANGLE))TD_Sell();if(CE_ButtonPressed(PSP_CTRL_HOME))td_editor=1;for(i=0;i<TD_MAX_TOWERS;i++)if(td_towers[i].active&&TD_Dist(td_towers[i].x,td_towers[i].y,td_cursorX*TD_CELL_W+12,TD_TOP+td_cursorY*TD_CELL_H+8)<13)td_selected=i;}
static void TD_Update(float dt){int i,j;if(td_paused||td_menu||td_editor||td_game_over)return;td_clock+=dt;if(td_spawnLeft>0){td_spawnTimer-=dt;if(td_spawnTimer<=0){int type=TD_SelectEnemyType();if(TD_AddEnemy(type)>=0){td_spawnLeft--;td_spawned++;}td_spawnTimer=td_modes[td_modeIndex].flags&1?1.3f:.48f;}}for(i=0;i<TD_MAX_ENEMIES;i++)if(td_enemies[i].active){TD_Enemy*e=&td_enemies[i];float tx,ty,dx,dy,d,step;int n=TD_PathCount();if(e->hp<=0){e->active=0;td_money+=td_enemyDefs[e->type].reward;td_kills++;if(e->flags&TD_E_BOSS)td_bossKills++;continue;}if(e->burnTimer>0){e->burnTimer-=dt;e->hp-=(int)(e->burnDmg*dt);if(e->hp<=0)continue;}if(e->poisonTimer>0){e->poisonTimer-=dt;e->hp-=(int)(e->poisonDmg*dt);}if(e->slowTimer>0)e->slowTimer-=dt;if(e->waypoint>=n){e->active=0;td_lives-=td_enemyDefs[e->type].leak;td_waveLeak=1;if(td_lives<=0)td_game_over=1;continue;}TD_GetPathPoint(e->waypoint,&tx,&ty);dx=tx-e->x;dy=ty-e->y;d=sqrtf(dx*dx+dy*dy);step=e->speed*(e->slowTimer>0?.55f:1.0f)*dt;if(d<step+2){e->waypoint++;}else if(d>0.01f){e->x+=dx/d*step;e->y+=dy/d*step;}}
for(i=0;i<TD_MAX_TOWERS;i++)if(td_towers[i].active){TD_Tower*t=&td_towers[i];int target=-1;float best=-1,damage=t->damage,range=t->range,rate=t->rate;int flags=0;if(td_defs[t->type].support)continue;flags=td_defs[t->type].effects;for(j=0;j<t->runeCount;j++){TD_Rune*r=&t->runes[j];if(r->kind==TD_RUNE_DAMAGE)damage*=r->power;else if(r->kind==TD_RUNE_RANGE)range*=r->power;else if(r->kind==TD_RUNE_RATE)rate*=r->power;else if(r->kind==TD_RUNE_SPLASH)flags|=TD_SHOT_SPLASH;}for(j=0;j<TD_MAX_TOWERS;j++)if(td_towers[j].active&&td_defs[td_towers[j].type].support&&TD_Dist(t->x,t->y,td_towers[j].x,td_towers[j].y)<td_towers[j].range){damage*=1.08f;rate*=.96f;}t->cooldown-=dt;if(t->cooldown>0)continue;for(j=0;j<TD_MAX_ENEMIES;j++)if(td_enemies[j].active&&(!(td_enemies[j].flags&TD_E_INVISIBLE)||td_radarBuilt)){float d=TD_Dist(t->x,t->y,td_enemies[j].x,td_enemies[j].y);if(d<=range&&(target<0||td_enemies[j].waypoint>best)){target=j;best=(float)td_enemies[j].waypoint;}}if(target>=0&&TD_AddShot(t->x,t->y,target,damage,td_defs[t->type].splash,flags)>=0)t->cooldown=rate;}
for(i=0;i<TD_MAX_SHOTS;i++)if(td_shots[i].active){TD_Shot*s=&td_shots[i];if(s->target<0||s->target>=TD_MAX_ENEMIES||!td_enemies[s->target].active){s->active=0;continue;}{TD_Enemy*e=&td_enemies[s->target];float dx=e->x-s->x,dy=e->y-s->y,d=sqrtf(dx*dx+dy*dy),step=s->speed*dt;if(d<=step+e->x*0.0f+4){int damage=(int)s->damage;if(e->flags&TD_E_ARMORED)damage=(damage*85)/100;if(damage<1)damage=1;e->hp-=damage;if(s->flags&TD_SHOT_SLOW)e->slowTimer=1.5f;if(s->flags&TD_SHOT_BURN){e->burnTimer=2.5f;e->burnDmg=4;}if(s->flags&TD_SHOT_POISON){e->poisonTimer=3;e->poisonDmg=3;}if(s->flags&TD_SHOT_SPLASH){float rad=s->splash>0?s->splash:44.0f;for(j=0;j<TD_MAX_ENEMIES;j++)if(j!=s->target&&td_enemies[j].active&&TD_Dist(e->x,e->y,td_enemies[j].x,td_enemies[j].y)<rad)td_enemies[j].hp-=damage/2;}if(s->flags&TD_SHOT_PIERCE){s->target=-1;s->active=0;}else s->active=0;}else if(d>0.01f){s->x+=dx/d*step;s->y+=dy/d*step;}}}
if(td_spawnLeft==0&&TD_AliveEnemies()==0&&td_wave>0&&!td_waveComplete){td_waveComplete=1;if(!td_waveLeak)td_challengeValue[4]++;if(td_modeIndex==12){td_campaignRegion=(td_wave-1)/10;if(td_wave>=100)td_game_over=1;else td_money+=100;}}if(td_wave>td_bestWave)td_bestWave=td_wave;TD_ChallengeUpdate();}
static void TD_DrawPath(void){int i,n=TD_PathCount();for(i=0;i<n-1;i++){float ax,ay,bx,by;if(td_mapIndex==20){ax=td_customX[i]*TD_CELL_W+12;ay=TD_TOP+td_customY[i]*TD_CELL_H+8;bx=td_customX[i+1]*TD_CELL_W+12;by=TD_TOP+td_customY[i+1]*TD_CELL_H+8;}else{TD_GetPathPoint(i,&ax,&ay);TD_GetPathPoint(i+1,&bx,&by);}if(fabsf(ax-bx)<1)CE_DrawRect(ax-5,ay<by?ay:by,10,fabsf(by-ay),GU_COLOR(110,103,85,255));else CE_DrawRect(ax<bx?ax:bx,ay-5,fabsf(bx-ax),10,GU_COLOR(110,103,85,255));}}
static void TD_Render(void){int i;unsigned int bg=GU_COLOR(38,82,55,255);if(td_mapIndex==7)bg=GU_COLOR(60,42,39,255);if(td_mapIndex==8)bg=GU_COLOR(170,205,215,255);if(td_mapIndex==9)bg=GU_COLOR(35,105,65,255);sceGuStart(GU_DIRECT,g_displayList);sceGuClearColor(bg);sceGuClear(GU_COLOR_BUFFER_BIT);for(i=0;i<15;i++)CE_DrawRect(0,TD_TOP+i*TD_CELL_H,480,1,GU_COLOR(255,255,255,14));for(i=0;i<20;i++)CE_DrawRect(i*TD_CELL_W,TD_TOP,1,240,GU_COLOR(255,255,255,14));TD_DrawPath();for(i=0;i<TD_MAX_TOWERS;i++)if(td_towers[i].active){TD_Tower*t=&td_towers[i];unsigned int col=GU_COLOR(75+(t->type*31)%170,100+(t->type*47)%130,155+(t->type*19)%90,255);CE_DrawRect(t->x-6,t->y-6,12,12,col);if(i==td_selected)CE_DrawRect(t->x-9,t->y-9,18,2,GU_COLOR(255,245,120,255));CE_DrawRect(t->x-6,t->y+7,12,2,GU_COLOR(20,20,20,255));CE_DrawRect(t->x-6,t->y+7,12.0f*t->level/10,2,GU_COLOR(255,220,70,255));if(t->runeCount)CE_DrawRect(t->x-2,t->y-2,4,4,GU_COLOR(220,160,255,255));}for(i=0;i<TD_MAX_ENEMIES;i++)if(td_enemies[i].active){TD_Enemy*e=&td_enemies[i];unsigned int col=(e->flags&TD_E_BOSS)?GU_COLOR(230,55,65,255):((e->flags&TD_E_INVISIBLE)?GU_COLOR(180,190,210,100):GU_COLOR(175,75,65,255));CE_DrawRect(e->x-4,e->y-4,8,8,col);CE_DrawRect(e->x-5,e->y-7,10,2,GU_COLOR(30,20,20,255));CE_DrawRect(e->x-5,e->y-7,10.0f*TD_Clamp((float)e->hp/e->maxHp,0,1),2,GU_COLOR(90,230,110,255));}for(i=0;i<TD_MAX_SHOTS;i++)if(td_shots[i].active)CE_DrawRect(td_shots[i].x-2,td_shots[i].y-2,4,4,GU_COLOR(255,230,100,255));CE_DrawRect(0,0,480,23,GU_COLOR(8,14,24,245));CE_DrawRect(8,7,TD_Clamp((float)td_lives,0,50)*2,7,GU_COLOR(235,70,80,255));CE_DrawRect(115,7,TD_Clamp((float)td_money,0,1000)*.25f,7,GU_COLOR(245,190,55,255));CE_DrawRect(380,7,TD_Clamp((float)td_wave,0,50)*1.6f,7,GU_COLOR(80,170,245,255));CE_DrawRect(0,264,480,8,GU_COLOR(8,14,24,240));CE_DrawRect(6,266,12,3,GU_COLOR(90,220,145,255));CE_DrawRect(24,266,12,3,GU_COLOR(90,170,245,255));CE_DrawRect(42,266,12,3,GU_COLOR(245,195,75,255));CE_DrawRect(60,266,12,3,GU_COLOR(240,105,90,255));/* Cursor */CE_DrawRect(td_cursorX*TD_CELL_W+2,TD_TOP+td_cursorY*TD_CELL_H+2,20,12,GU_COLOR(255,255,255,55));if(td_menu){CE_DrawRect(70,55,340,150,GU_COLOR(10,18,32,245));CE_DrawRect(90,78,300,4,GU_COLOR(70,190,220,255));CE_DrawRect(90,112,300,3,GU_COLOR(255,205,80,255));CE_DrawRect(90,145,300,3,GU_COLOR(110,220,150,255));}if(td_paused)CE_DrawRect(160,110,160,45,GU_COLOR(20,30,50,235));if(td_game_over)CE_DrawRect(145,105,190,55,GU_COLOR(120,25,35,245));if(td_editor)CE_DrawRect(4,28,472,18,GU_COLOR(25,40,70,230));sceGuFinish();sceGuSync(0,0);sceDisplayWaitVblankStart();sceGuSwapBuffers();}


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
    TD_InitDefs();
    td_towerCount=0; td_enemyCount=ENEMY_TYPE_COUNT;
    td_menu=0; td_modeIndex=1; td_mapIndex=0;
    td_customCount=0; td_campaignRegion=0; td_bestWave=0;
    td_rng=(unsigned int)sceKernelGetSystemTimeLow();
    TD_ResetGame();
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
