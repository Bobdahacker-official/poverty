#define _GNU_SOURCE
/* ClassiCube window backend for Avalanche's Linux64 graphical ABI.
 * ClassiCube is Copyright 2014-2026 ClassiCube contributors, BSD-3-Clause. */
#include "Core.h"
#include "_WindowBase.h"
#include "Bitmap.h"
#include "Errors.h"
#include "Event.h"
#include "Input.h"
#include "Platform.h"
#include "String_.h"
#include "Utils.h"

#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

struct av_gui_open { uint32_t width, height; char title[64]; };
struct av_gui_draw { uint32_t x, y, width, height, stride, reserved; uint64_t pixels; };
struct av_gui_event { uint32_t type; int32_t a, b, c; };
enum { AV_GUI_TEXT = 1, AV_GUI_KEY, AV_GUI_MOUSE, AV_GUI_QUIT };

static int mouseX, mouseY, buttons;
static cc_bool opened, pendingClose;

static int MapKey(int key) {
    if (key >= 'a' && key <= 'z') key -= 'a' - 'A';
    if (key >= 'A' && key <= 'Z') return CCKEY_A + key - 'A';
    if (key >= '0' && key <= '9') return CCKEY_0 + key - '0';
    switch (key) {
    case ' ': return CCKEY_SPACE;
    case 8: return CCKEY_BACKSPACE;
    case 9: return CCKEY_TAB;
    case 10: case 13: return CCKEY_ENTER;
    case 27: return CCKEY_ESCAPE;
    case 0x4b: return CCKEY_LEFT;
    case 0x4d: return CCKEY_RIGHT;
    case 0x48: return CCKEY_UP;
    case 0x50: return CCKEY_DOWN;
    case 0x47: return CCKEY_HOME;
    case 0x4f: return CCKEY_END;
    case 0x49: return CCKEY_PAGEUP;
    case 0x51: return CCKEY_PAGEDOWN;
    case 0x53: return CCKEY_DELETE;
    default: return 0;
    }
}

static void Cursor_GetRawPos(int* x, int* y) { *x = mouseX; *y = mouseY; }
static void Cursor_DoSetVisible(cc_bool visible) { (void)visible; }
static void SetMousePosition(int x, int y) { mouseX = x; mouseY = y; Pointer_SetPosition(0, x, y); }
static void SendButton(int bit, int key, int now) {
    if ((buttons & bit) != (now & bit)) Input_SetNonRepeatable(key, now & bit);
}

void Window_PreInit(void) { DisplayInfo.CursorVisible = true; }
void Window_Init(void) {
    Input.Sources = INPUT_SOURCE_NORMAL;
    DisplayInfo.Depth = 32;
    DisplayInfo.Width = 960; DisplayInfo.Height = 600;
    DisplayInfo.ScaleX = 1.0f; DisplayInfo.ScaleY = 1.0f;
    Platform_Flags |= PLAT_FLAG_SINGLE_PROCESS;
}
void Window_Free(void) { if (opened) syscall(4099, 0); opened = false; }
static void DoCreateWindow(int width, int height) {
    struct av_gui_open open;
    if (width < 320) width = 960;
    if (height < 200) height = 600;
    if (width > 1600) width = 1600;
    if (height > 1000) height = 1000;
    memset(&open, 0, sizeof(open));
    open.width = width; open.height = height;
    memcpy(open.title, "ClassiCube", 11);
    if (syscall(4096, &open) < 0) Process_Abort("Opening Magenta window");
    opened = true;
    DisplayInfo.Width = Window_Main.Width = width;
    DisplayInfo.Height = Window_Main.Height = height;
    Window_Main.Exists = true; Window_Main.Focused = true;
    Window_Main.UIScaleX = DEFAULT_UI_SCALE_X;
    Window_Main.UIScaleY = DEFAULT_UI_SCALE_Y;
    Event_RaiseVoid(&WindowEvents.Created);
}
void Window_Create2D(int width, int height) { DoCreateWindow(width, height); }
void Window_Create3D(int width, int height) { DoCreateWindow(width, height); }
/* Destroying a surface during launcher/game transitions is not an app quit.
 * Window_ProcessEvents marks Exists false only for an explicit close request. */
void Window_Destroy(void) { if (opened) { syscall(4099, 0); opened = false; } }
void Window_SetTitle(const cc_string* title) { (void)title; }
void Clipboard_GetText(cc_string* value) { value->length = 0; }
void Clipboard_SetText(const cc_string* value) { (void)value; }
int Window_GetWindowState(void) { return WINDOW_STATE_NORMAL; }
cc_result Window_EnterFullscreen(void) { return ERR_NOT_SUPPORTED; }
cc_result Window_ExitFullscreen(void) { return ERR_NOT_SUPPORTED; }
int Window_IsObscured(void) { return 0; }
void Window_Show(void) { }
void Window_SetSize(int width, int height) { (void)width; (void)height; }
void Window_RequestClose(void) { pendingClose = true; }
void Window_ProcessEvents(float delta) {
    struct av_gui_event event;
    (void)delta;
    while (syscall(4098, &event) > 0) {
        if (event.type == AV_GUI_QUIT) { pendingClose = true; continue; }
        if (event.type == AV_GUI_MOUSE) {
            if (Input.RawMode) Event_RaiseRawMove(&PointerEvents.RawMoved, event.a - mouseX, event.b - mouseY);
            SetMousePosition(event.a, event.b);
            SendButton(1, CCMOUSE_L, event.c); SendButton(2, CCMOUSE_R, event.c); SendButton(4, CCMOUSE_M, event.c);
            buttons = event.c;
        } else if (event.type == AV_GUI_TEXT || event.type == AV_GUI_KEY) {
            int key = MapKey(event.a);
            if (key) { Input_SetPressed(key); Input_SetReleased(key); }
            if (event.type == AV_GUI_TEXT && event.a >= 32 && event.a <= 0x10ffff) Event_RaiseInt(&InputEvents.Press, event.a);
        }
    }
    if (pendingClose) { pendingClose = false; Window_Main.Exists = false; Event_RaiseVoid(&WindowEvents.Closing); }
}
void Gamepads_PreInit(void) { }
void Gamepads_Init(void) { }
void Gamepads_Process(float delta) { (void)delta; }
void Cursor_SetPosition(int x, int y) { mouseX = x; mouseY = y; }
void Window_EnableRawMouse(void) { Input.RawMode = true; }
void Window_UpdateRawMouse(void) { }
void Window_DisableRawMouse(void) { Input.RawMode = false; }
static void ShowDialogCore(const char* title, const char* msg) { Platform_LogConst(title); Platform_LogConst(msg); }
cc_result Window_OpenFileDialog(const struct OpenFileDialogArgs* args) { (void)args; return ERR_NOT_SUPPORTED; }
cc_result Window_SaveFileDialog(const struct SaveFileDialogArgs* args) { (void)args; return ERR_NOT_SUPPORTED; }
void Window_AllocFramebuffer(struct Bitmap* bmp, int width, int height) {
    bmp->scan0 = (BitmapCol*)Mem_Alloc(width * height, BITMAPCOLOR_SIZE, "window pixels");
    bmp->width = width; bmp->height = height;
}
void Window_FreeFramebuffer(struct Bitmap* bmp) { Mem_Free(bmp->scan0); bmp->scan0 = NULL; }
void Window_DrawFramebuffer(Rect2D r, struct Bitmap* bmp) {
    struct av_gui_draw draw;
    if (!opened || r.width <= 0 || r.height <= 0) return;
    if (r.x < 0) { r.width += r.x; r.x = 0; }
    if (r.y < 0) { r.height += r.y; r.y = 0; }
    if (r.x + r.width > bmp->width) r.width = bmp->width - r.x;
    if (r.y + r.height > bmp->height) r.height = bmp->height - r.y;
    if (r.width <= 0 || r.height <= 0) return;
    memset(&draw, 0, sizeof(draw));
    draw.x = r.x; draw.y = r.y; draw.width = r.width; draw.height = r.height;
    draw.stride = bmp->width; draw.pixels = (uintptr_t)(bmp->scan0 + r.y * bmp->width + r.x);
    syscall(4097, &draw);
}
void OnscreenKeyboard_Open(struct OpenKeyboardArgs* args) { (void)args; }
void OnscreenKeyboard_SetText(const cc_string* text) { (void)text; }
void OnscreenKeyboard_Close(void) { }
void Window_LockLandscapeOrientation(cc_bool lock) { (void)lock; }
