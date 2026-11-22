// switchpro_vjoy.c — feed a Nintendo Switch Pro Controller (read via SDL3)
// into vJoy device 1 so Windows sees a standard calibratable joystick.
//
// Build (MSYS2 UCRT64):
//   gcc -O2 -o switchpro_vjoy.exe switchpro_vjoy.c -I<SDL3 include> -L<SDL3 lib> -lSDL3
// Run with SDL3.dll and vJoyInterface.dll on PATH (or beside the exe).

#include <SDL3/SDL.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

// ---- vJoy interface (dynamic load to avoid .lib dependency) ----
typedef enum { VJD_STAT_OWN=0, VJD_STAT_FREE, VJD_STAT_BUSY, VJD_STAT_MISS, VJD_STAT_UNKN } VjdStat;
typedef BOOL (__cdecl *VJD_GetStatus)(void);
typedef int  (__cdecl *VJD_GetBtnNum)(UINT);
typedef int  (__cdecl *VJD_GetPovNum)(UINT);
typedef VjdStat (__cdecl *VJD_GetStat)(UINT);
typedef BOOL (__cdecl *VJD_Acquire)(UINT);
typedef void (__cdecl *VJD_Release)(UINT);
typedef BOOL (__cdecl *VJD_Reset)(UINT);
typedef BOOL (__cdecl *VJD_SetAxis)(LONG, UINT, UINT);
typedef BOOL (__cdecl *VJD_SetBtn)(BOOL, UINT, UCHAR);
typedef BOOL (__cdecl *VJD_SetDiscPov)(int, UINT, UCHAR);
typedef BOOL (__cdecl *VJD_SetContPov)(DWORD, UINT, UCHAR);
typedef BOOL (__cdecl *VJD_GetAxisMax)(UINT, UINT, LONG*);
typedef BOOL (__cdecl *VJD_GetAxisMin)(UINT, UINT, LONG*);

static struct {
    HMODULE dll;
    VJD_GetStatus vJoyEnabled;
    VJD_GetBtnNum GetVJDButtonNumber;
    VJD_GetPovNum GetVJDDiscPovNumber;
    VJD_GetStat   GetVJDStatus;
    VJD_Acquire   AcquireVJD;
    VJD_Release   RelinquishVJD;
    VJD_Reset     ResetVJD;
    VJD_SetAxis   SetAxis;
    VJD_SetBtn    SetBtn;
    VJD_SetDiscPov SetDiscPov;
    VJD_SetContPov SetContPov;
    VJD_GetAxisMax GetVJDAxisMax;
    VJD_GetAxisMin GetVJDAxisMin;
} vj;

// HID usages for vJoy axes
#define AXIS_X   0x30
#define AXIS_Y   0x31
#define AXIS_Z   0x32
#define AXIS_RX  0x33
#define AXIS_RY  0x34
#define AXIS_RZ  0x35
#define AXIS_SL0 0x36
#define AXIS_SL1 0x37

static int axis_min(UINT usage, int vjd) { LONG v=0; vj.GetVJDAxisMin(vjd, usage, &v); return (int)v; }
static int axis_max(UINT usage, int vjd) { LONG v=0; vj.GetVJDAxisMax(vjd, usage, &v); return (int)v; }

// map SDL axis (-32768..32767) to vJoy axis range, centered
static LONG axis_to_vjoy(int sdlval, UINT usage, int vjd) {
    int mn = axis_min(usage, vjd), mx = axis_max(usage, vjd);
    // SDL stick range is roughly -32768..32767, center 0
    double t = (double)sdlval / 32767.0;           // -1..1
    if (t > 1) t = 1; if (t < -1) t = -1;
    double mid = (mn + mx) / 2.0;
    double half = (mx - mn) / 2.0;
    return (LONG)(mid + t * half);
}

// SDL gamepad button -> vJoy button number (1..16)
typedef struct { SDL_GamepadButton btn; int vbtn; const char *name; } BtnMap;
static const BtnMap btnmap[] = {
    { SDL_GAMEPAD_BUTTON_SOUTH,        1,  "SOUTH" },
    { SDL_GAMEPAD_BUTTON_EAST,         2,  "EAST" },
    { SDL_GAMEPAD_BUTTON_WEST,         3,  "WEST" },
    { SDL_GAMEPAD_BUTTON_NORTH,        4,  "NORTH" },
    { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,5,  "L_SHOULDER" },
    { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,6, "R_SHOULDER" },
    { SDL_GAMEPAD_BUTTON_BACK,         9,  "BACK" },
    { SDL_GAMEPAD_BUTTON_START,       10,  "START" },
    { SDL_GAMEPAD_BUTTON_LEFT_STICK,  11,  "L_STICK" },
    { SDL_GAMEPAD_BUTTON_RIGHT_STICK, 12, "R_STICK" },
    { SDL_GAMEPAD_BUTTON_MISC1,       13,  "MISC1(capture)" },
    { SDL_GAMEPAD_BUTTON_GUIDE,       14,  "GUIDE(home)" },
    { SDL_GAMEPAD_BUTTON_MISC2,       15,  "MISC2" },
    { SDL_GAMEPAD_BUTTON_MISC3,       16,  "MISC3" },
};
#define N_BTNS 14
// triggers are analog axes in SDL3 -> threshold to buttons 7/8
enum { TRIG_L_VBTN = 7, TRIG_R_VBTN = 8 };

typedef struct { SDL_GamepadAxis axis; UINT usage; const char *name; } AxisMap;
static const AxisMap axismap[] = {
    { SDL_GAMEPAD_AXIS_LEFTX,  AXIS_X,   "LX" },
    { SDL_GAMEPAD_AXIS_LEFTY,  AXIS_Y,   "LY" },
    { SDL_GAMEPAD_AXIS_RIGHTX, AXIS_RX,  "RX" },
    { SDL_GAMEPAD_AXIS_RIGHTY, AXIS_RY,  "RY" },
};

// ---- pad slots: one per physical controller, each owns one vJoy device ----
typedef struct {
    SDL_Gamepad *gp;
    SDL_Joystick *joy;
    SDL_JoystickID id;
    int vjd;
    bool prev[N_BTNS];
    bool prev_tl, prev_tr;
    Uint64 frame;
} PadSlot;

static void release_slot(PadSlot *s) {
    if (s->gp) {
        printf("vJoy %d <- pad removed, released\n", s->vjd);
        SDL_CloseGamepad(s->gp);
        s->gp = NULL; s->joy = NULL; s->id = 0;
        vj.ResetVJD(s->vjd);
        vj.RelinquishVJD(s->vjd);
        s->vjd = 0;
    }
}

static int open_slot(PadSlot *s, SDL_JoystickID id, int vjd) {
    SDL_Gamepad *gp = SDL_OpenGamepad(id);
    if (!gp) return 0;
    if (!vj.AcquireVJD(vjd)) { SDL_CloseGamepad(gp); return 0; }
    vj.ResetVJD(vjd);
    s->gp = gp;
    s->joy = SDL_GetGamepadJoystick(gp);
    s->id = id;
    s->vjd = vjd;
    memset(s->prev, 0, sizeof(s->prev));
    s->prev_tl = s->prev_tr = false;
    s->frame = 0;
    printf("vJoy %d <- %s\n", vjd, SDL_GetGamepadName(gp));
    if (SDL_GamepadHasSensor(gp, SDL_SENSOR_GYRO)) {
        SDL_SetGamepadSensorEnabled(gp, SDL_SENSOR_GYRO, true);
        printf("  gyro %.0f Hz\n", SDL_GetGamepadSensorDataRate(gp, SDL_SENSOR_GYRO));
    }
    if (SDL_GamepadHasSensor(gp, SDL_SENSOR_ACCEL)) {
        SDL_SetGamepadSensorEnabled(gp, SDL_SENSOR_ACCEL, true);
        printf("  accel %.0f Hz\n", SDL_GetGamepadSensorDataRate(gp, SDL_SENSOR_ACCEL));
    }
    return 1;
}

// push one frame of pad state into its vJoy device
static void feed_slot(PadSlot *s) {
    SDL_Gamepad *gp = s->gp;
    int vjd = s->vjd;
    for (size_t i = 0; i < sizeof(axismap)/sizeof(axismap[0]); i++) {
        int v = SDL_GetGamepadAxis(gp, axismap[i].axis);
        vj.SetAxis(axis_to_vjoy(v, axismap[i].usage, vjd), vjd, axismap[i].usage);
    }
    // motion sensors -> spare axes: gyro XYZ -> Z,RZ,SL0 ; accel Y -> SL1
    {
        static const struct { SDL_SensorType type; int idx; UINT usage; float scale; } smap[] = {
            { SDL_SENSOR_GYRO, 0, AXIS_Z,   35.0f },
            { SDL_SENSOR_GYRO, 1, AXIS_RZ,  35.0f },
            { SDL_SENSOR_GYRO, 2, AXIS_SL0, 35.0f },
            { SDL_SENSOR_ACCEL,1, AXIS_SL1, 20.0f },
        };
        for (size_t i = 0; i < sizeof(smap)/sizeof(smap[0]); i++) {
            float d[3] = {0};
            if (SDL_GetGamepadSensorData(gp, smap[i].type, d, 3)) {
                float t = d[smap[i].idx] / smap[i].scale;
                if (t > 1) t = 1; if (t < -1) t = -1;
                LONG mn, mx; vj.GetVJDAxisMin(vjd, smap[i].usage, &mn); vj.GetVJDAxisMax(vjd, smap[i].usage, &mx);
                vj.SetAxis((LONG)(((double)mn + (double)mx) / 2.0 + t * ((double)mx - mn) / 2.0), vjd, smap[i].usage);
            }
        }
    }
    // triggers (analog axes -> digital buttons)
    {
        int tl = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        int tr = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        bool tl_d = tl > 12000, tr_d = tr > 12000;
        if (tl_d != s->prev_tl) { s->prev_tl = tl_d; vj.SetBtn(tl_d, vjd, TRIG_L_VBTN); }
        if (tr_d != s->prev_tr) { s->prev_tr = tr_d; vj.SetBtn(tr_d, vjd, TRIG_R_VBTN); }
    }
    // buttons
    for (size_t i = 0; i < (size_t)N_BTNS; i++) {
        bool down = SDL_GetGamepadButton(gp, btnmap[i].btn);
        if (down != s->prev[i]) {
            s->prev[i] = down;
            vj.SetBtn(down, vjd, (UCHAR)btnmap[i].vbtn);
        }
    }
    // hat -> discrete POV (1 hat)
    if (s->joy) {
        Uint8 hat = SDL_GetJoystickHat(s->joy, 0);
        int pov = -1;
        bool u = hat & 0x01, r = hat & 0x02, d = hat & 0x04, l = hat & 0x08;
        if (u && !d && !l && !r) pov = 0;
        else if (r && !u && !d && !l) pov = 1;
        else if (d && !u && !r && !l) pov = 2;
        else if (l && !u && !d && !r) pov = 3;
        else pov = -1; // diagonals unsupported by 4-way hat
        vj.SetDiscPov(pov, vjd, 1);
    }
    s->frame++;
}

static int is_nintendo_pad(const char *name) {
    return name && (strstr(name, "Nintendo") || strstr(name, "Pro Controller") || strstr(name, "Joy-Con"));
}

// enumerate candidate pads; returns count, fills ids[]
static int list_pads(SDL_JoystickID *ids, int max, const char *match) {
    int n = 0;
    SDL_JoystickID *all = SDL_GetGamepads(&n);
    int cnt = 0;
    for (int i = 0; i < n && cnt < max; i++) {
        const char *name = SDL_GetGamepadNameForID(all[i]);
        const char *path = SDL_GetGamepadPathForID(all[i]);
        if ((name && strstr(name, "vJoy")) || (path && strstr(path, "vJoy"))) continue;
        int hit;
        if (match && *match)
            hit = (name && strstr(name, match)) || (path && strstr(path, match));
        else
            hit = is_nintendo_pad(name);
        if (hit) ids[cnt++] = all[i];
    }
    SDL_free(all);
    return cnt;
}

// embedded vJoyInterface.dll (resource IDR_VJOYDLL) — extracted to %TEMP% on demand
#define IDR_VJOYDLL 101
static char g_vjoy_path[MAX_PATH];
static int ensure_vjoy_dll(void) {
    if (GetFileAttributesA("vJoyInterface.dll") != INVALID_FILE_ATTRIBUTES) return 1;
    DWORD n = GetTempPathA(MAX_PATH, g_vjoy_path);
    if (!n || n >= MAX_PATH) return 0;
    strcat(g_vjoy_path, "switchpro_vjoy\\");
    CreateDirectoryA(g_vjoy_path, NULL);
    strcat(g_vjoy_path, "vJoyInterface.dll");
    if (GetFileAttributesA(g_vjoy_path) == INVALID_FILE_ATTRIBUTES) {
        HRSRC r = FindResourceA(NULL, MAKEINTRESOURCEA(IDR_VJOYDLL), RT_RCDATA);
        if (!r) return 0;
        HGLOBAL g = LoadResource(NULL, r);
        DWORD sz = SizeofResource(NULL, r);
        void *data = LockResource(g);
        if (!data || !sz) return 0;
        char part[MAX_PATH];
        strcpy(part, g_vjoy_path); strcat(part, ".part");
        FILE *f = fopen(part, "wb");
        if (!f) return 0;
        fwrite(data, 1, sz, f);
        fclose(f);
        if (!MoveFileExA(part, g_vjoy_path, MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(part); return 0; }
    }
    return 1;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *dev_match = NULL;
    int vjd_arg = 0;
    int auto_mode = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) {
            if (!SDL_Init(SDL_INIT_GAMEPAD)) return 1;
            SDL_JoystickID ids[32]; int n = list_pads(ids, 32, NULL);
            for (int j = 0; j < n; j++)
                printf("pad[%d]: %s\n  path: %s\n", j, SDL_GetGamepadNameForID(ids[j]), SDL_GetGamepadPathForID(ids[j]));
            SDL_Quit();
            return 0;
        } else if (!strcmp(argv[i], "--auto")) {
            auto_mode = 1;
        } else if (!dev_match) {
            dev_match = argv[i];
            auto_mode = 0;
        } else {
            vjd_arg = atoi(argv[i]);
        }
    }

    // load vJoyInterface.dll (local copy first, else extract the embedded one)
    ensure_vjoy_dll();
    vj.dll = LoadLibraryA("vJoyInterface.dll");
    if (!vj.dll && g_vjoy_path[0]) vj.dll = LoadLibraryA(g_vjoy_path);
    if (!vj.dll) { fprintf(stderr, "vJoyInterface.dll not found\n"); return 1; }
#define LOAD(n) vj.n = (void*)GetProcAddress(vj.dll, #n); if(!vj.n){fprintf(stderr,"missing %s\n",#n);return 1;}
    LOAD(vJoyEnabled); LOAD(GetVJDButtonNumber); LOAD(GetVJDDiscPovNumber);
    LOAD(GetVJDStatus); LOAD(AcquireVJD); LOAD(RelinquishVJD); LOAD(ResetVJD);
    LOAD(SetAxis); LOAD(SetBtn); LOAD(SetDiscPov); LOAD(SetContPov);
    LOAD(GetVJDAxisMax); LOAD(GetVJDAxisMin);
#undef LOAD

    if (!vj.vJoyEnabled()) { fprintf(stderr, "vJoy driver not enabled\n"); return 1; }
    if (!SDL_Init(SDL_INIT_GAMEPAD)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }

    if (auto_mode) {
        // manage up to 4 pads -> lowest free vJoy devices
        PadSlot slots[4] = {0};
        Uint64 last_scan = 0;
        printf("auto mode: Nintendo pads -> vJoy devices 1..N (waiting)\n");
        for (;;) {
            SDL_Event ev;
            bool rescan = false;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT) goto auto_done;
                if (ev.type == SDL_EVENT_GAMEPAD_ADDED || ev.type == SDL_EVENT_GAMEPAD_REMOVED)
                    rescan = true;
                if (ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
                    for (int i = 0; i < 4; i++)
                        if (slots[i].gp && ev.gdevice.which == slots[i].id) release_slot(&slots[i]);
                }
            }
            SDL_UpdateJoysticks();
            Uint64 now = SDL_GetTicks();
            if (rescan || now - last_scan > 500) {
                last_scan = now;
                // adopt newly appeared pads into free slots/vJDs
                for (int i = 0; i < 4; i++) if (!slots[i].gp) {
                    SDL_JoystickID ids[8]; int n = list_pads(ids, 8, NULL);
                    for (int j = 0; j < n; j++) {
                        int dup = 0;
                        for (int k = 0; k < 4; k++) if (slots[k].gp && slots[k].id == ids[j]) dup = 1;
                        if (dup) continue;
                        // find a free vJoy device
                        for (int v = 1; v <= 16; v++) {
                            int used = 0;
                            for (int k = 0; k < 4; k++) if (slots[k].gp && slots[k].vjd == v) used = 1;
                            if (used) continue;
                            if (open_slot(&slots[i], ids[j], v)) break;
                        }
                        break; // one pad per empty slot per scan
                    }
                }
            }
            for (int i = 0; i < 4; i++) if (slots[i].gp) feed_slot(&slots[i]);
            SDL_Delay(4);
        }
    auto_done:
        for (int i = 0; i < 4; i++) release_slot(&slots[i]);
        SDL_Quit();
        return 0;
    }

    // single-pad mode: <match> [vjd]
    int vjd = vjd_arg ? vjd_arg : 1;
    if (!vj.AcquireVJD(vjd)) { fprintf(stderr, "VJD %d busy/missing\n", vjd); return 1; }
    vj.ResetVJD(vjd);
    printf("acquired vJoy device %d (%d buttons, %d POVs)\n",
           vjd, vj.GetVJDButtonNumber(vjd), vj.GetVJDDiscPovNumber(vjd));

    PadSlot s;
    memset(&s, 0, sizeof(s));
    bool logged_wait = false;
    for (;;) {
        if (!s.gp) {
            SDL_JoystickID ids[8]; int n = list_pads(ids, 8, dev_match);
            SDL_JoystickID target = n > 0 ? ids[0] : 0;
            if (!target) {
                if (!logged_wait) {
                    printf("waiting for %s...\n", dev_match ? dev_match : "a Nintendo pad");
                    logged_wait = true;
                }
                SDL_Delay(500);
                continue;
            }
            if (!open_slot(&s, target, vjd)) { SDL_Delay(500); continue; }
            logged_wait = false;
        }
        SDL_Event ev;
        bool removed = false;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) goto done;
            if (ev.type == SDL_EVENT_GAMEPAD_REMOVED && ev.gdevice.which == s.id) removed = true;
        }
        if (removed) {
            printf("pad removed\n");
            SDL_CloseGamepad(s.gp);
            s.gp = NULL; s.joy = NULL; s.id = 0;
            vj.ResetVJD(vjd);
            continue;
        }
        SDL_UpdateJoysticks();
        feed_slot(&s);
        SDL_Delay(4);
    }
done:
    printf("closing\n");
    if (s.gp) SDL_CloseGamepad(s.gp);
    vj.ResetVJD(vjd);
    vj.RelinquishVJD(vjd);
    SDL_Quit();
    return 0;
}
