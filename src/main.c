/*
 * COAST ENGINE — PSP single-file foundation
 * Version: 0.1
 *
 * This is a standalone PSP SDK source file.
 * Replace: src/main.c
 *
 * Current foundation:
 * - PSP startup and exit callback
 * - frame timing
 * - controller input
 * - simple text renderer for the first hardware test
 * - 64x64 map storage
 * - memory helpers
 * - save/load
 *
 * The 2D/3D renderer, audio mixer, and TD v6 gameplay systems
 * will be added in later versions. This is not yet the full TD v6 port.
 */

#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* PSP application                                                            */
/* -------------------------------------------------------------------------- */

PSP_MODULE_INFO("COAST ENGINE", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-256);

#define CE_VERSION_MAJOR 0
#define CE_VERSION_MINOR 1

#define CE_MAP_W 64
#define CE_MAP_H 64
#define CE_SAVE_PATH "ms0:/COAST_ENGINE_SAVE.DAT"

#define CE_SCREEN_W 480
#define CE_SCREEN_H 272

/* -------------------------------------------------------------------------- */
/* Core data                                                                  */
/* -------------------------------------------------------------------------- */

typedef struct {
    int x;
    int y;
    int speed;
} CE_Player;

typedef struct {
    unsigned int magic;
    unsigned int version;
    CE_Player player;
    unsigned char cells[CE_MAP_H][CE_MAP_W];
} CE_SaveData;

typedef struct {
    unsigned int buttons;
    unsigned int previousButtons;
    float deltaTime;
    float totalTime;
    unsigned int frames;
} CE_State;

static CE_Player g_player;
static CE_State g_state;
static unsigned char g_map[CE_MAP_H][CE_MAP_W];
static unsigned long long g_lastTime;
static int g_running = 1;

/* -------------------------------------------------------------------------- */
/* System: PSP exit callback                                                  */
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
/* Core: initialization, timing and shutdown                                  */
/* -------------------------------------------------------------------------- */

static void CE_Init(void) {
    memset(&g_state, 0, sizeof(g_state));
    memset(&g_map, 0, sizeof(g_map));

    g_player.x = CE_MAP_W / 2;
    g_player.y = CE_MAP_H / 2;
    g_player.speed = 1;

    /* A few sample map objects so the first test is visible. */
    {
        int i;
        for (i = 8; i < 18; ++i) {
            g_map[12][i] = 1;
            g_map[28][i] = 1;
        }
        for (i = 12; i < 29; ++i) {
            g_map[i][8] = 1;
        }
    }

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    pspDebugScreenInit();
    pspDebugScreenSetBackColor(0xFF08111D);
    pspDebugScreenSetTextColor(0xFFFFFFFF);

    g_lastTime = sceKernelGetSystemTimeWide();
}

static void CE_UpdateTime(void) {
    unsigned long long now = sceKernelGetSystemTimeWide();
    unsigned long long elapsed = now - g_lastTime;

    g_lastTime = now;
    g_state.deltaTime = (float)elapsed / 1000000.0f;

    /* Avoid a huge simulation step after a pause or debugger stop. */
    if (g_state.deltaTime > 0.05f) {
        g_state.deltaTime = 0.05f;
    }

    if (g_state.deltaTime < 0.0f) {
        g_state.deltaTime = 0.0f;
    }

    g_state.totalTime += g_state.deltaTime;
    g_state.frames++;
}

static void CE_Shutdown(void) {
    /* Reserved for future renderer/audio cleanup. */
}

/* -------------------------------------------------------------------------- */
/* Input: PSP controls                                                        */
/* -------------------------------------------------------------------------- */

static void CE_InputUpdate(void) {
    SceCtrlData pad;

    g_state.previousButtons = g_state.buttons;
    sceCtrlPeekBufferPositive(&pad, 1);
    g_state.buttons = pad.Buttons;

    if (g_state.buttons & PSP_CTRL_LEFT) {
        g_player.x -= g_player.speed;
    }
    if (g_state.buttons & PSP_CTRL_RIGHT) {
        g_player.x += g_player.speed;
    }
    if (g_state.buttons & PSP_CTRL_UP) {
        g_player.y -= g_player.speed;
    }
    if (g_state.buttons & PSP_CTRL_DOWN) {
        g_player.y += g_player.speed;
    }

    /* Keep the player inside the map. */
    if (g_player.x < 0) g_player.x = 0;
    if (g_player.y < 0) g_player.y = 0;
    if (g_player.x >= CE_MAP_W) g_player.x = CE_MAP_W - 1;
    if (g_player.y >= CE_MAP_H) g_player.y = CE_MAP_H - 1;

    if (g_state.buttons & PSP_CTRL_START) {
        g_running = 0;
    }
}

/* -------------------------------------------------------------------------- */
/* Map: 64x64 byte grid                                                       */
/* -------------------------------------------------------------------------- */

static void CE_MapSet(int x, int y, unsigned char value) {
    if (x < 0 || y < 0 || x >= CE_MAP_W || y >= CE_MAP_H) {
        return;
    }

    g_map[y][x] = value;
}

static unsigned char CE_MapGet(int x, int y) {
    if (x < 0 || y < 0 || x >= CE_MAP_W || y >= CE_MAP_H) {
        return 0;
    }

    return g_map[y][x];
}

/* -------------------------------------------------------------------------- */
/* Memory helpers                                                             */
/* -------------------------------------------------------------------------- */

static void *CE_Alloc(unsigned int bytes) {
    return malloc(bytes);
}

static void CE_Free(void *memory) {
    free(memory);
}

/* -------------------------------------------------------------------------- */
/* Save system                                                                */
/* -------------------------------------------------------------------------- */

static int CE_Save(void) {
    CE_SaveData *data;
    SceUID file;
    int written;

    data = (CE_SaveData *)CE_Alloc(sizeof(CE_SaveData));
    if (!data) {
        return 0;
    }

    data->magic = 0x434F4153; /* "COAS" */
    data->version = 1;
    data->player = g_player;
    memcpy(data->cells, g_map, sizeof(g_map));

    file = sceIoOpen(
        CE_SAVE_PATH,
        PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC,
        0777
    );

    if (file < 0) {
        CE_Free(data);
        return 0;
    }

    written = sceIoWrite(file, data, sizeof(CE_SaveData));
    sceIoClose(file);
    CE_Free(data);

    return written == (int)sizeof(CE_SaveData);
}

static int CE_Load(void) {
    CE_SaveData *data;
    SceUID file;
    int bytesRead;

    data = (CE_SaveData *)CE_Alloc(sizeof(CE_SaveData));
    if (!data) {
        return 0;
    }

    file = sceIoOpen(CE_SAVE_PATH, PSP_O_RDONLY, 0777);
    if (file < 0) {
        CE_Free(data);
        return 0;
    }

    bytesRead = sceIoRead(file, data, sizeof(CE_SaveData));
    sceIoClose(file);

    if (bytesRead != (int)sizeof(CE_SaveData) ||
        data->magic != 0x434F4153 ||
        data->version != 1) {
        CE_Free(data);
        return 0;
    }

    g_player = data->player;
    memcpy(g_map, data->cells, sizeof(g_map));

    CE_Free(data);
    return 1;
}

/* -------------------------------------------------------------------------- */
/* Audio: reserved subsystem                                                  */
/* -------------------------------------------------------------------------- */

static int CE_AudioInit(void) {
    /*
     * Audio mixer and sound channels will be implemented here.
     * Keeping this function separate lets the rest of the engine
     * initialize without requiring audio assets yet.
     */
    return 1;
}

static void CE_AudioShutdown(void) {
    /* Reserved for audio channel release. */
}

/* -------------------------------------------------------------------------- */
/* Graphics: first PSP display test                                           */
/* -------------------------------------------------------------------------- */

static void CE_DrawMap(void) {
    int screenX;
    int screenY;
    int mapX;
    int mapY;
    int startX = g_player.x - 10;
    int startY = g_player.y - 5;

    pspDebugScreenSetTextColor(0xFF78D7FF);
    pspDebugScreenPrintf("MAP VIEW\n");

    for (screenY = 0; screenY < 11; ++screenY) {
        mapY = startY + screenY;
        if (mapY < 0) mapY = 0;
        if (mapY >= CE_MAP_H) mapY = CE_MAP_H - 1;

        for (screenX = 0; screenX < 21; ++screenX) {
            mapX = startX + screenX;
            if (mapX < 0) mapX = 0;
            if (mapX >= CE_MAP_W) mapX = CE_MAP_W - 1;

            if (mapX == g_player.x && mapY == g_player.y) {
                pspDebugScreenSetTextColor(0xFF55FF88);
                pspDebugScreenPrintf("@");
            } else if (CE_MapGet(mapX, mapY) == 1) {
                pspDebugScreenSetTextColor(0xFFFFC45C);
                pspDebugScreenPrintf("#");
            } else {
                pspDebugScreenSetTextColor(0xFF526477);
                pspDebugScreenPrintf(".");
            }
        }

        pspDebugScreenPrintf("\n");
    }
}

static void CE_Render(void) {
    pspDebugScreenClear();
    pspDebugScreenSetXY(0, 0);

    pspDebugScreenSetTextColor(0xFF5DE0C1);
    pspDebugScreenPrintf("COAST ENGINE  v%d.%d  |  PSP SINGLE-FILE BUILD\n",
        CE_VERSION_MAJOR, CE_VERSION_MINOR);

    pspDebugScreenSetTextColor(0xFFFFFFFF);
    pspDebugScreenPrintf("-----------------------------------------------\n");
    pspDebugScreenPrintf("Engine core: OK     Input: OK     Map: 64x64\n");
    pspDebugScreenPrintf("Player position: X=%d  Y=%d\n", g_player.x, g_player.y);
    pspDebugScreenPrintf("Delta time: %.4f sec\n", g_state.deltaTime);
    pspDebugScreenPrintf("Uptime: %.1f sec\n\n", g_state.totalTime);

    CE_DrawMap();

    pspDebugScreenSetTextColor(0xFFFFFFFF);
    pspDebugScreenPrintf("\nCONTROLS\n");
    pspDebugScreenPrintf("D-PAD  - move around the map\n");
    pspDebugScreenPrintf("CIRCLE - save game\n");
    pspDebugScreenPrintf("TRIANGLE - load game\n");
    pspDebugScreenPrintf("START - exit\n");

    pspDebugScreenSetTextColor(0xFF9AAABD);
    pspDebugScreenPrintf("\nCOAST ENGINE is running. TD v6 systems are next.");
}

/* -------------------------------------------------------------------------- */
/* Main loop                                                                  */
/* -------------------------------------------------------------------------- */

int main(void) {
    CE_SetupCallbacks();
    CE_Init();
    CE_AudioInit();

    while (g_running) {
        CE_UpdateTime();
        CE_InputUpdate();

        /* Save/load are triggered only once per button press. */
        if ((g_state.buttons & PSP_CTRL_CIRCLE) &&
            !(g_state.previousButtons & PSP_CTRL_CIRCLE)) {
            CE_Save();
        }

        if ((g_state.buttons & PSP_CTRL_TRIANGLE) &&
            !(g_state.previousButtons & PSP_CTRL_TRIANGLE)) {
            CE_Load();
        }

        CE_Render();
        sceDisplayWaitVblankStart();
    }

    CE_AudioShutdown();
    CE_Shutdown();
    sceKernelExitGame();
    return 0;
}
