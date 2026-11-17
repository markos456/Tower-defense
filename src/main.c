/*
 * COAST ENGINE v0.2 — PSP SINGLE-FILE GAME ENGINE
 *
 * One source file: src/main.c
 * Features:
 * - PSP GU accelerated 2D renderer
 * - BMP texture and sprite-sheet loader (24/32-bit, uncompressed)
 * - Animated sprites
 * - 64x64 tile map and camera follow
 * - Game object pool
 * - AABB and solid-tile collision
 * - D-pad + analog input, edge-triggered buttons
 * - Delta-time game loop
 * - Save/load of map and objects
 *
 * Optional sprite sheet:
 * ms0:/PSP/GAME/HamletTD/ASSETS/SPRITES.BMP
 * Use a 4-frame horizontal strip of 32x32 frames for the demo player.
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

/* -------------------------------------------------------------------------- */
/* PSP application                                                            */
/* -------------------------------------------------------------------------- */

PSP_MODULE_INFO("COAST ENGINE", 0, 2, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-256);

#define CE_VERSION_MAJOR       0
#define CE_VERSION_MINOR       2

#define CE_SCREEN_W            480
#define CE_SCREEN_H            272
#define CE_BUFFER_W            512

#define CE_MAP_W               64
#define CE_MAP_H               64
#define CE_TILE_SIZE           32
#define CE_WORLD_W             (CE_MAP_W * CE_TILE_SIZE)
#define CE_WORLD_H             (CE_MAP_H * CE_TILE_SIZE)

#define CE_MAX_OBJECTS         128
#define CE_SAVE_PATH           "ms0:/COAST_ENGINE_SAVE.DAT"
#define CE_SPRITE_PATH         "ms0:/PSP/GAME/HamletTD/ASSETS/SPRITES.BMP"

#define CE_PLAYER_SPEED        155.0f
#define CE_ANIMATION_FPS       8.0f

/* -------------------------------------------------------------------------- */
/* GU buffers                                                                 */
/* -------------------------------------------------------------------------- */

static unsigned int __attribute__((aligned(16))) g_displayList[262144];

typedef struct {
    short x;
    short y;
    short z;
} CE_Vertex;

typedef struct {
    unsigned int color;
    unsigned short u;
    unsigned short v;
    short x;
    short y;
    short z;
} CE_TextureVertex;

/* -------------------------------------------------------------------------- */
/* Engine data                                                                */
/* -------------------------------------------------------------------------- */

enum {
    CE_TILE_GRASS = 0,
    CE_TILE_WALL  = 1,
    CE_TILE_WATER = 2,
    CE_TILE_SAND  = 3,
    CE_TILE_ROAD  = 4
};

enum {
    CE_OBJECT_NONE   = 0,
    CE_OBJECT_PLAYER = 1,
    CE_OBJECT_ENEMY  = 2,
    CE_OBJECT_CRATE  = 3,
    CE_OBJECT_TREE   = 4
};

#define CE_OBJECT_SOLID  1

typedef struct {
    int active;
    int type;
    int flags;

    float x;
    float y;
    float w;
    float h;

    float vx;
    float vy;

    int hp;
    int maxHp;

    int frame;
    int frameCount;
    int frameW;
    int frameH;
    float animTimer;
    float animFPS;
} CE_Object;

typedef struct {
    int width;
    int height;
    int textureWidth;
    int textureHeight;
    unsigned int *pixels;
    int loaded;
} CE_Texture;

typedef struct {
    unsigned int magic;
    unsigned int version;
    int objectCount;
    CE_Object objects[CE_MAX_OBJECTS];
    unsigned char map[CE_MAP_H][CE_MAP_W];
} CE_SaveData;

typedef struct {
    unsigned int buttons;
    unsigned int previousButtons;
    float deltaTime;
    float totalTime;
    unsigned int frames;
    float fpsTimer;
    unsigned int fpsFrames;
    unsigned int fps;
} CE_State;

static CE_Object g_objects[CE_MAX_OBJECTS];
static int g_objectCount = 0;
static unsigned char g_map[CE_MAP_H][CE_MAP_W];
static CE_Texture g_spriteTexture;
static CE_State g_state;
static float g_cameraX = 0.0f;
static float g_cameraY = 0.0f;
static unsigned long long g_lastTime = 0;
static int g_running = 1;
static int g_playerIndex = -1;

/* -------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* -------------------------------------------------------------------------- */

static float CE_ClampF(float value, float minimum, float maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static int CE_ClampI(int value, int minimum, int maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static int CE_NextPowerOfTwo(int value) {
    int result = 1;
    while (result < value && result < 1024) result <<= 1;
    return result;
}

static unsigned int CE_ReadLE32(const unsigned char *p) {
    return (unsigned int)p[0] |
           ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) |
           ((unsigned int)p[3] << 24);
}

static unsigned short CE_ReadLE16(const unsigned char *p) {
    return (unsigned short)((unsigned int)p[0] |
           ((unsigned int)p[1] << 8));
}

static int CE_ButtonPressed(unsigned int button) {
    return ((g_state.buttons & button) != 0) &&
           ((g_state.previousButtons & button) == 0);
}

/* -------------------------------------------------------------------------- */
/* PSP exit callback                                                          */
/* -------------------------------------------------------------------------- */

static int CE_ExitCallback(int arg1, int arg2, void *common) {
    (void)arg1;
    (void)arg2;
    (void)common;
    g_running = 0;
    sceKernelExitGame();
    return 0;
}

static int CE_CallbackThread(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    int callbackId = sceKernelCreateCallback(
        "COAST_ENGINE_EXIT",
        CE_ExitCallback,
        NULL
    );

    if (callbackId >= 0) {
        sceKernelRegisterExitCallback(callbackId);
    }

    sceKernelSleepThreadCB();
    return 0;
}

static void CE_SetupCallbacks(void) {
    int threadId = sceKernelCreateThread(
        "COAST_ENGINE_CALLBACK",
        CE_CallbackThread,
        0x11,
        0x1000,
        0,
        NULL
    );

    if (threadId >= 0) {
        sceKernelStartThread(threadId, 0, NULL);
    }
}

/* -------------------------------------------------------------------------- */
/* Graphics: PSP GU initialization and primitives                            */
/* -------------------------------------------------------------------------- */

static void CE_GraphicsInit(void) {
    sceGuInit();

    sceGuStart(GU_DIRECT, g_displayList);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, CE_BUFFER_W);
    sceGuDispBuffer(CE_SCREEN_W, CE_SCREEN_H, (void *)0x88000, CE_BUFFER_W);
    sceGuOffset(2048 - (CE_SCREEN_W / 2), 2048 - (CE_SCREEN_H / 2));
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

static void CE_DrawRect(float x, float y, float w, float h,
                        unsigned int color) {
    CE_Vertex *vertices = (CE_Vertex *)sceGuGetMemory(sizeof(CE_Vertex) * 4);
    if (!vertices) return;

    vertices[0].x = (short)x;
    vertices[0].y = (short)y;
    vertices[0].z = 0;

    vertices[1].x = (short)(x + w);
    vertices[1].y = (short)y;
    vertices[1].z = 0;

    vertices[2].x = (short)x;
    vertices[2].y = (short)(y + h);
    vertices[2].z = 0;

    vertices[3].x = (short)(x + w);
    vertices[3].y = (short)(y + h);
    vertices[3].z = 0;

    sceGuDisable(GU_TEXTURE_2D);
    sceGuColor(color);
    sceGuDrawArray(
        GU_TRIANGLE_STRIP,
        GU_VERTEX_16BIT | GU_TRANSFORM_2D,
        4,
        0,
        vertices
    );
}

static void CE_TextureUnload(CE_Texture *texture) {
    if (texture->pixels) {
        free(texture->pixels);
    }
    memset(texture, 0, sizeof(*texture));
}

/* -------------------------------------------------------------------------- */
/* Graphics: BMP texture loader                                               */
/* Supports uncompressed 24-bit and 32-bit BMP files up to 512x512.           */
/* -------------------------------------------------------------------------- */

static int CE_TextureLoadBMP(CE_Texture *texture, const char *path) {
    unsigned char header[54];
    unsigned char *row = NULL;
    SceUID file;
    int width;
    int height;
    int topDown;
    int bitsPerPixel;
    unsigned int compression;
    unsigned int pixelOffset;
    int rowBytes;
    int texW;
    int texH;
    int y;
    int x;

    CE_TextureUnload(texture);

    file = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (file < 0) return 0;

    if (sceIoRead(file, header, sizeof(header)) != (int)sizeof(header)) {
        sceIoClose(file);
        return 0;
    }

    if (header[0] != 'B' || header[1] != 'M') {
        sceIoClose(file);
        return 0;
    }

    pixelOffset = CE_ReadLE32(header + 10);
    width = (int)CE_ReadLE32(header + 18);
    height = (int)CE_ReadLE32(header + 22);
    bitsPerPixel = (int)CE_ReadLE16(header + 28);
    compression = CE_ReadLE32(header + 30);

    if (height < 0) {
        topDown = 1;
        height = -height;
    } else {
        topDown = 0;
    }

    if (width <= 0 || height <= 0 || width > 512 || height > 512 ||
        (bitsPerPixel != 24 && bitsPerPixel != 32) || compression != 0) {
        sceIoClose(file);
        return 0;
    }

    texW = CE_NextPowerOfTwo(width);
    texH = CE_NextPowerOfTwo(height);

    texture->pixels = (unsigned int *)memalign(16, texW * texH * 4);
    if (!texture->pixels) {
        sceIoClose(file);
        return 0;
    }

    memset(texture->pixels, 0, texW * texH * 4);
    rowBytes = ((width * bitsPerPixel + 31) / 32) * 4;
    row = (unsigned char *)malloc(rowBytes);

    if (!row) {
        CE_TextureUnload(texture);
        sceIoClose(file);
        return 0;
    }

    sceIoLseek(file, pixelOffset, 0);

    for (y = 0; y < height; ++y) {
        int destY = topDown ? y : (height - 1 - y);

        if (sceIoRead(file, row, rowBytes) != rowBytes) {
            free(row);
            CE_TextureUnload(texture);
            sceIoClose(file);
            return 0;
        }

        for (x = 0; x < width; ++x) {
            unsigned char b = row[x * (bitsPerPixel / 8) + 0];
            unsigned char g = row[x * (bitsPerPixel / 8) + 1];
            unsigned char r = row[x * (bitsPerPixel / 8) + 2];
            unsigned char a = (bitsPerPixel == 32)
                ? row[x * 4 + 3] : 255;

            texture->pixels[destY * texW + x] =
                ((unsigned int)a << 24) |
                ((unsigned int)b << 16) |
                ((unsigned int)g << 8) |
                (unsigned int)r;
        }
    }

    free(row);
    sceIoClose(file);

    texture->width = width;
    texture->height = height;
    texture->textureWidth = texW;
    texture->textureHeight = texH;
    texture->loaded = 1;

    sceKernelDcacheWritebackRange(
        texture->pixels,
        texW * texH * sizeof(unsigned int)
    );

    return 1;
}

static void CE_DrawSprite(float x, float y, int frame,
                          int frameW, int frameH,
                          unsigned int tint) {
    CE_TextureVertex *vertices =
        (CE_TextureVertex *)sceGuGetMemory(sizeof(CE_TextureVertex) * 4);
    int u0;
    int v0;
    int u1;
    int v1;

    if (!g_spriteTexture.loaded || frameW <= 0 || frameH <= 0 ||
        g_spriteTexture.width < frameW ||
        g_spriteTexture.height < frameH || !vertices) {
        CE_DrawRect(x, y, (float)frameW, (float)frameH, tint);
        return;
    }

    frame = CE_ClampI(frame, 0,
        (g_spriteTexture.width / frameW) - 1);

    u0 = frame * frameW;
    v0 = 0;
    u1 = u0 + frameW;
    v1 = frameH;

    vertices[0].color = GU_COLOR(255, 255, 255, 255);
    vertices[0].u = (unsigned short)u0;
    vertices[0].v = (unsigned short)v0;
    vertices[0].x = (short)x;
    vertices[0].y = (short)y;
    vertices[0].z = 0;

    vertices[1].color = GU_COLOR(255, 255, 255, 255);
    vertices[1].u = (unsigned short)u1;
    vertices[1].v = (unsigned short)v0;
    vertices[1].x = (short)(x + frameW);
    vertices[1].y = (short)y;
    vertices[1].z = 0;

    vertices[2].color = GU_COLOR(255, 255, 255, 255);
    vertices[2].u = (unsigned short)u0;
    vertices[2].v = (unsigned short)v1;
    vertices[2].x = (short)x;
    vertices[2].y = (short)(y + frameH);
    vertices[2].z = 0;

    vertices[3].color = GU_COLOR(255, 255, 255, 255);
    vertices[3].u = (unsigned short)u1;
    vertices[3].v = (unsigned short)v1;
    vertices[3].x = (short)(x + frameW);
    vertices[3].y = (short)(y + frameH);
    vertices[3].z = 0;

    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(
        0,
        g_spriteTexture.textureWidth,
        g_spriteTexture.textureHeight,
        g_spriteTexture.textureWidth,
        g_spriteTexture.pixels
    );
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuColor(tint);
    sceGuDrawArray(
        GU_TRIANGLE_STRIP,
        GU_TEXTURE_16BIT | GU_COLOR_8888 |
        GU_VERTEX_16BIT | GU_TRANSFORM_2D,
        4,
        0,
        vertices
    );
    sceGuDisable(GU_TEXTURE_2D);
}

/* -------------------------------------------------------------------------- */
/* Map system                                                                 */
/* -------------------------------------------------------------------------- */

static void CE_MapSet(int x, int y, unsigned char tile) {
    if (x < 0 || y < 0 || x >= CE_MAP_W || y >= CE_MAP_H) return;
    g_map[y][x] = tile;
}

static unsigned char CE_MapGet(int x, int y) {
    if (x < 0 || y < 0 || x >= CE_MAP_W || y >= CE_MAP_H) {
        return CE_TILE_WALL;
    }
    return g_map[y][x];
}

static int CE_TileIsSolid(int tile) {
    return tile == CE_TILE_WALL || tile == CE_TILE_WATER;
}

static void CE_MapGenerate(void) {
    int x;
    int y;

    memset(g_map, CE_TILE_GRASS, sizeof(g_map));

    for (x = 0; x < CE_MAP_W; ++x) {
        CE_MapSet(x, 0, CE_TILE_WALL);
        CE_MapSet(x, CE_MAP_H - 1, CE_TILE_WALL);
    }

    for (y = 0; y < CE_MAP_H; ++y) {
        CE_MapSet(0, y, CE_TILE_WALL);
        CE_MapSet(CE_MAP_W - 1, y, CE_TILE_WALL);
    }

    /* A small lake with a sandy shore. */
    for (y = 18; y <= 25; ++y) {
        for (x = 19; x <= 27; ++x) {
            CE_MapSet(x, y, CE_TILE_SAND);
        }
    }
    for (y = 20; y <= 23; ++y) {
        for (x = 21; x <= 25; ++x) {
            CE_MapSet(x, y, CE_TILE_WATER);
        }
    }

    /* A road crossing the test area. */
    for (x = 5; x < 59; ++x) {
        CE_MapSet(x, 34, CE_TILE_ROAD);
        CE_MapSet(x, 35, CE_TILE_ROAD);
    }

    /* A few solid blocks to test collision. */
    for (y = 10; y < 15; ++y) {
        CE_MapSet(12, y, CE_TILE_WALL);
        CE_MapSet(13, y, CE_TILE_WALL);
    }
    for (x = 40; x < 46; ++x) {
        CE_MapSet(x, 12, CE_TILE_WALL);
    }
}

/* -------------------------------------------------------------------------- */
/* Game object system                                                         */
/* -------------------------------------------------------------------------- */

static int CE_AddObject(int type, float x, float y,
                        float w, float h, int flags) {
    int index;

    if (g_objectCount >= CE_MAX_OBJECTS) return -1;

    index = g_objectCount++;
    memset(&g_objects[index], 0, sizeof(CE_Object));

    g_objects[index].active = 1;
    g_objects[index].type = type;
    g_objects[index].flags = flags;
    g_objects[index].x = x;
    g_objects[index].y = y;
    g_objects[index].w = w;
    g_objects[index].h = h;
    g_objects[index].hp = 100;
    g_objects[index].maxHp = 100;
    g_objects[index].frameCount = 4;
    g_objects[index].frameW = 32;
    g_objects[index].frameH = 32;
    g_objects[index].animFPS = CE_ANIMATION_FPS;

    return index;
}

static int CE_AABB(float ax, float ay, float aw, float ah,
                   float bx, float by, float bw, float bh) {
    return ax < bx + bw && ax + aw > bx &&
           ay < by + bh && ay + ah > by;
}

static int CE_ObjectHitsSolid(const CE_Object *object, int selfIndex) {
    int left = (int)(object->x / CE_TILE_SIZE);
    int right = (int)((object->x + object->w - 1.0f) / CE_TILE_SIZE);
    int top = (int)(object->y / CE_TILE_SIZE);
    int bottom = (int)((object->y + object->h - 1.0f) / CE_TILE_SIZE);
    int x;
    int y;
    int i;

    for (y = top; y <= bottom; ++y) {
        for (x = left; x <= right; ++x) {
            if (CE_TileIsSolid(CE_MapGet(x, y))) return 1;
        }
    }

    for (i = 0; i < g_objectCount; ++i) {
        CE_Object *other = &g_objects[i];

        if (i == selfIndex || !other->active) continue;
        if (!(other->flags & CE_OBJECT_SOLID)) continue;

        if (CE_AABB(object->x, object->y, object->w, object->h,
                    other->x, other->y, other->w, other->h)) {
            return 1;
        }
    }

    return 0;
}

static void CE_MoveObject(int index, float dx, float dy) {
    CE_Object *object = &g_objects[index];
    float oldX = object->x;
    float oldY = object->y;

    object->x += dx;
    if (CE_ObjectHitsSolid(object, index)) {
        object->x = oldX;
    }

    object->y += dy;
    if (CE_ObjectHitsSolid(object, index)) {
        object->y = oldY;
    }
}

static void CE_WorldInit(void) {
    int i;

    memset(g_objects, 0, sizeof(g_objects));
    g_objectCount = 0;
    g_playerIndex = -1;

    CE_MapGenerate();

    g_playerIndex = CE_AddObject(
        CE_OBJECT_PLAYER,
        32.0f * 31.0f,
        32.0f * 31.0f,
        24.0f,
        28.0f,
        0
    );

    /* Trees and crates are ordinary engine objects with solid collision. */
    for (i = 0; i < 7; ++i) {
        CE_AddObject(
            CE_OBJECT_TREE,
            32.0f * (8 + i * 2),
            32.0f * 28.0f,
            26.0f,
            26.0f,
            CE_OBJECT_SOLID
        );
    }

    CE_AddObject(CE_OBJECT_CRATE, 32.0f * 35.0f, 32.0f * 31.0f,
                 26.0f, 26.0f, CE_OBJECT_SOLID);
    CE_AddObject(CE_OBJECT_CRATE, 32.0f * 36.0f, 32.0f * 31.0f,
                 26.0f, 26.0f, CE_OBJECT_SOLID);

    CE_AddObject(CE_OBJECT_ENEMY, 32.0f * 38.0f, 32.0f * 37.0f,
                 24.0f, 28.0f, 0);
}

/* -------------------------------------------------------------------------- */
/* Input system                                                               */
/* -------------------------------------------------------------------------- */

static void CE_InputUpdate(void) {
    SceCtrlData pad;
    CE_Object *player;
    float dx = 0.0f;
    float dy = 0.0f;
    float length;

    g_state.previousButtons = g_state.buttons;
    sceCtrlPeekBufferPositive(&pad, 1);
    g_state.buttons = pad.Buttons;

    if (g_playerIndex < 0) return;
    player = &g_objects[g_playerIndex];

    if (g_state.buttons & PSP_CTRL_LEFT)  dx -= 1.0f;
    if (g_state.buttons & PSP_CTRL_RIGHT) dx += 1.0f;
    if (g_state.buttons & PSP_CTRL_UP)    dy -= 1.0f;
    if (g_state.buttons & PSP_CTRL_DOWN)  dy += 1.0f;

    if (pad.Lx < 108) dx = -1.0f;
    if (pad.Lx > 148) dx = 1.0f;
    if (pad.Ly < 108) dy = -1.0f;
    if (pad.Ly > 148) dy = 1.0f;

    length = sqrtf(dx * dx + dy * dy);
    if (length > 1.0f) {
        dx /= length;
        dy /= length;
    }

    player->vx = dx * CE_PLAYER_SPEED;
    player->vy = dy * CE_PLAYER_SPEED;

    if (dx < -0.1f) player->frame = 1;
    else if (dx > 0.1f) player->frame = 2;
    else if (dy < -0.1f) player->frame = 3;
    else if (dy > 0.1f) player->frame = 0;

    if (CE_ButtonPressed(PSP_CTRL_START)) g_running = 0;

    if (CE_ButtonPressed(PSP_CTRL_CIRCLE)) {
        /* Save is handled in the main loop. */
    }
}

/* -------------------------------------------------------------------------- */
/* Animation, object update and camera                                        */
/* -------------------------------------------------------------------------- */

static void CE_UpdateObjects(float dt) {
    int i;
    CE_Object *player = (g_playerIndex >= 0)
        ? &g_objects[g_playerIndex] : NULL;

    for (i = 0; i < g_objectCount; ++i) {
        CE_Object *object = &g_objects[i];

        if (!object->active) continue;

        if (object->type == CE_OBJECT_ENEMY && player) {
            float centerX = object->x - player->x;
            float centerY = object->y - player->y;
            float distance = sqrtf(centerX * centerX + centerY * centerY);

            object->vx = 0.0f;
            object->vy = 0.0f;

            if (distance > 36.0f && distance < 260.0f) {
                object->vx = (-centerX / distance) * 58.0f;
                object->vy = (-centerY / distance) * 58.0f;
            }
        }

        if (object->type == CE_OBJECT_PLAYER ||
            object->type == CE_OBJECT_ENEMY) {
            CE_MoveObject(i, object->vx * dt, object->vy * dt);
        }

        if (fabsf(object->vx) + fabsf(object->vy) > 2.0f) {
            object->animTimer += dt;
            if (object->animTimer >= (1.0f / object->animFPS)) {
                object->animTimer = 0.0f;
                object->frame = (object->frame + 1) % object->frameCount;
            }
        } else {
            object->animTimer = 0.0f;
        }
    }
}

static void CE_UpdateCamera(float dt) {
    CE_Object *player;
    float targetX;
    float targetY;
    float smooth;

    if (g_playerIndex < 0) return;
    player = &g_objects[g_playerIndex];

    targetX = player->x + player->w * 0.5f - CE_SCREEN_W * 0.5f;
    targetY = player->y + player->h * 0.5f - CE_SCREEN_H * 0.5f;

    targetX = CE_ClampF(targetX, 0.0f, CE_WORLD_W - CE_SCREEN_W);
    targetY = CE_ClampF(targetY, 0.0f, CE_WORLD_H - CE_SCREEN_H);

    smooth = CE_ClampF(dt * 7.0f, 0.0f, 1.0f);
    g_cameraX += (targetX - g_cameraX) * smooth;
    g_cameraY += (targetY - g_cameraY) * smooth;
}

/* -------------------------------------------------------------------------- */
/* Save/load                                                                  */
/* -------------------------------------------------------------------------- */

static int CE_Save(void) {
    CE_SaveData *data;
    SceUID file;
    int written;

    data = (CE_SaveData *)malloc(sizeof(CE_SaveData));
    if (!data) return 0;

    data->magic = 0x434F4153;
    data->version = 2;
    data->objectCount = g_objectCount;
    memcpy(data->objects, g_objects, sizeof(g_objects));
    memcpy(data->map, g_map, sizeof(g_map));

    file = sceIoOpen(
        CE_SAVE_PATH,
        PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC,
        0777
    );

    if (file < 0) {
        free(data);
        return 0;
    }

    written = sceIoWrite(file, data, sizeof(CE_SaveData));
    sceIoClose(file);
    free(data);

    return written == (int)sizeof(CE_SaveData);
}

static int CE_Load(void) {
    CE_SaveData *data;
    SceUID file;
    int bytesRead;

    data = (CE_SaveData *)malloc(sizeof(CE_SaveData));
    if (!data) return 0;

    file = sceIoOpen(CE_SAVE_PATH, PSP_O_RDONLY, 0);
    if (file < 0) {
        free(data);
        return 0;
    }

    bytesRead = sceIoRead(file, data, sizeof(CE_SaveData));
    sceIoClose(file);

    if (bytesRead != (int)sizeof(CE_SaveData) ||
        data->magic != 0x434F4153 ||
        data->version != 2 ||
        data->objectCount < 1 ||
        data->objectCount > CE_MAX_OBJECTS) {
        free(data);
        return 0;
    }

    g_objectCount = data->objectCount;
    memcpy(g_objects, data->objects, sizeof(g_objects));
    memcpy(g_map, data->map, sizeof(g_map));

    g_playerIndex = -1;
    {
        int i;
        for (i = 0; i < g_objectCount; ++i) {
            if (g_objects[i].active &&
                g_objects[i].type == CE_OBJECT_PLAYER) {
                g_playerIndex = i;
                break;
            }
        }
    }

    free(data);
    return g_playerIndex >= 0;
}

/* -------------------------------------------------------------------------- */
/* Rendering                                                                  */
/* -------------------------------------------------------------------------- */

static unsigned int CE_TileColor(int tile) {
    switch (tile) {
        case CE_TILE_WALL:  return GU_COLOR(75, 82, 96, 255);
        case CE_TILE_WATER: return GU_COLOR(25, 105, 180, 255);
        case CE_TILE_SAND:  return GU_COLOR(205, 178, 112, 255);
        case CE_TILE_ROAD:  return GU_COLOR(65, 70, 78, 255);
        default:            return GU_COLOR(48, 125, 73, 255);
    }
}

static void CE_RenderMap(void) {
    int startX = (int)(g_cameraX / CE_TILE_SIZE);
    int startY = (int)(g_cameraY / CE_TILE_SIZE);
    int endX = startX + (CE_SCREEN_W / CE_TILE_SIZE) + 2;
    int endY = startY + (CE_SCREEN_H / CE_TILE_SIZE) + 2;
    int x;
    int y;

    endX = CE_ClampI(endX, 0, CE_MAP_W);
    endY = CE_ClampI(endY, 0, CE_MAP_H);

    for (y = startY; y < endY; ++y) {
        for (x = startX; x < endX; ++x) {
            int tile = CE_MapGet(x, y);
            float screenX = x * CE_TILE_SIZE - g_cameraX;
            float screenY = y * CE_TILE_SIZE - g_cameraY;

            CE_DrawRect(
                screenX,
                screenY,
                CE_TILE_SIZE + 1,
                CE_TILE_SIZE + 1,
                CE_TileColor(tile)
            );

            if (tile == CE_TILE_ROAD) {
                CE_DrawRect(
                    screenX + 4,
                    screenY + 15,
                    CE_TILE_SIZE - 8,
                    2,
                    GU_COLOR(220, 205, 120, 255)
                );
            }
        }
    }
}

static void CE_RenderObjects(void) {
    int i;

    for (i = 0; i < g_objectCount; ++i) {
        CE_Object *object = &g_objects[i];
        float sx;
        float sy;

        if (!object->active) continue;

        sx = object->x - g_cameraX;
        sy = object->y - g_cameraY;

        if (sx < -64 || sy < -64 ||
            sx > CE_SCREEN_W + 64 || sy > CE_SCREEN_H + 64) {
            continue;
        }

        if (object->type == CE_OBJECT_PLAYER) {
            if (g_spriteTexture.loaded) {
                CE_DrawSprite(
                    sx - 4,
                    sy - 2,
                    object->frame,
                    object->frameW,
                    object->frameH,
                    GU_COLOR(255, 255, 255, 255)
                );
            } else {
                CE_DrawRect(sx, sy, object->w, object->h,
                            GU_COLOR(70, 235, 145, 255));
                CE_DrawRect(sx + 5, sy + 5, 5, 5,
                            GU_COLOR(15, 35, 35, 255));
                CE_DrawRect(sx + 15, sy + 5, 5, 5,
                            GU_COLOR(15, 35, 35, 255));
            }
        } else if (object->type == CE_OBJECT_ENEMY) {
            CE_DrawRect(sx, sy, object->w, object->h,
                        GU_COLOR(220, 65, 70, 255));
            CE_DrawRect(sx + 4, sy + 5, 5, 5,
                        GU_COLOR(255, 245, 220, 255));
            CE_DrawRect(sx + 15, sy + 5, 5, 5,
                        GU_COLOR(255, 245, 220, 255));
        } else if (object->type == CE_OBJECT_TREE) {
            CE_DrawRect(sx + 8, sy + 12, 10, 16,
                        GU_COLOR(105, 70, 42, 255));
            CE_DrawRect(sx, sy, 26, 20,
                        GU_COLOR(30, 155, 80, 255));
        } else if (object->type == CE_OBJECT_CRATE) {
            CE_DrawRect(sx, sy, object->w, object->h,
                        GU_COLOR(155, 105, 55, 255));
            CE_DrawRect(sx + 3, sy + 3, object->w - 6, 3,
                        GU_COLOR(205, 155, 90, 255));
        }
    }
}

static void CE_RenderHUD(void) {
    CE_Object *player = (g_playerIndex >= 0)
        ? &g_objects[g_playerIndex] : NULL;
    float healthRatio = 1.0f;

    CE_DrawRect(0, 0, CE_SCREEN_W, 18,
                GU_COLOR(8, 15, 24, 210));
    CE_DrawRect(8, 6, 90, 6,
                GU_COLOR(45, 55, 65, 255));

    if (player && player->maxHp > 0) {
        healthRatio = (float)player->hp / (float)player->maxHp;
    }

    CE_DrawRect(8, 6, 90 * CE_ClampF(healthRatio, 0.0f, 1.0f), 6,
                GU_COLOR(70, 220, 120, 255));

    /* Small frame-rate meter; no font dependency. */
    CE_DrawRect(430, 5, 42, 8,
                GU_COLOR(45, 55, 65, 255));
    CE_DrawRect(430, 5, CE_ClampF((float)g_state.fps, 0, 60) * 0.7f,
                8, GU_COLOR(80, 175, 245, 255));

    CE_DrawRect(8, CE_SCREEN_H - 7, 464, 2,
                GU_COLOR(8, 15, 24, 190));
}

static void CE_Render(void) {
    sceGuStart(GU_DIRECT, g_displayList);

    sceGuClearColor(GU_COLOR(15, 28, 38, 255));
    sceGuClear(GU_COLOR_BUFFER_BIT);

    CE_RenderMap();
    CE_RenderObjects();
    CE_RenderHUD();

    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

/* -------------------------------------------------------------------------- */
/* Core: timing, initialization and shutdown                                  */
/* -------------------------------------------------------------------------- */

static void CE_UpdateTime(void) {
    unsigned long long now = sceKernelGetSystemTimeWide();
    unsigned long long elapsed = now - g_lastTime;

    g_lastTime = now;
    g_state.deltaTime = (float)elapsed / 1000000.0f;
    g_state.deltaTime = CE_ClampF(g_state.deltaTime, 0.0f, 0.05f);

    g_state.totalTime += g_state.deltaTime;
    g_state.frames++;

    g_state.fpsTimer += g_state.deltaTime;
    g_state.fpsFrames++;

    if (g_state.fpsTimer >= 1.0f) {
        g_state.fps = g_state.fpsFrames;
        g_state.fpsFrames = 0;
        g_state.fpsTimer -= 1.0f;
    }
}

static void CE_Init(void) {
    memset(&g_state, 0, sizeof(g_state));
    memset(&g_spriteTexture, 0, sizeof(g_spriteTexture));

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    CE_WorldInit();
    CE_GraphicsInit();

    /* Texture is optional: the engine uses colored sprites if it is absent. */
    CE_TextureLoadBMP(&g_spriteTexture, CE_SPRITE_PATH);

    g_lastTime = sceKernelGetSystemTimeWide();
}

static void CE_Shutdown(void) {
    CE_TextureUnload(&g_spriteTexture);
    sceGuTerm();
}

/* -------------------------------------------------------------------------- */
/* Main                                                                       */
/* -------------------------------------------------------------------------- */

int main(void) {
    CE_SetupCallbacks();
    CE_Init();

    while (g_running) {
        CE_UpdateTime();
        CE_InputUpdate();

        if (CE_ButtonPressed(PSP_CTRL_CIRCLE)) {
            CE_Save();
        }

        if (CE_ButtonPressed(PSP_CTRL_TRIANGLE)) {
            CE_Load();
        }

        CE_UpdateObjects(g_state.deltaTime);
        CE_UpdateCamera(g_state.deltaTime);
        CE_Render();
    }

    CE_Shutdown();
    sceKernelExitGame();
    return 0;
}
