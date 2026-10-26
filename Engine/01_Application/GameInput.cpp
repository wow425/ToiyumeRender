
// 直接套用，还没啃

#include "00_Core/PCH.h"
#include "00_Core/Utility/Utility.h"
#include "GameCore.h"
#include "GameInput.h"

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)

#define USE_XINPUT
#include <XInput.h>
#pragma comment(lib, "xinput9_1_0.lib")

#define USE_KEYBOARD_MOUSE
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

namespace GameCore
{
    extern HWND g_hWnd;
}

#else

using namespace Windows::Gaming::Input;
using namespace Windows::Foundation::Collections;

#define USE_KEYBOARD_MOUSE

struct DIMOUSESTATE2
{
    LONG lX, lY, lZ;
    BYTE rgbButtons[8];
};

#endif

namespace
{
    bool s_Buttons[2][GameInput::kNumDigitalInputs];
    float s_HoldDuration[GameInput::kNumDigitalInputs] = { 0.0f };
    float s_Analogs[GameInput::kNumAnalogInputs];
    float s_AnalogsTC[GameInput::kNumAnalogInputs];

#ifdef USE_KEYBOARD_MOUSE

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    IDirectInput8A* s_DI;
    IDirectInputDevice8A* s_Keyboard;
    IDirectInputDevice8A* s_Mouse;
    // R0：DirectInput 是否真正初始化成功。失败时置 false，
    // 所有键鼠读取路径都应提前返回，避免对空指针调用。
    bool s_KbmReady = false;
#endif

    DIMOUSESTATE2 s_MouseState;
    unsigned char s_Keybuffer[256];

    // R4.5：DirectInput 不可用时的鼠标后备来源基准点（见 KbmUpdate）。
    POINT s_LastCursorPos = { 0, 0 };
    bool  s_LastCursorPosValid = false;

    // 注意区分两张表 —— 这是 R4.3 修复的核心：
    //   s_DXKeyMapping : DigitalInput -> **DirectInput 扫描码 (DIK)**
    //                    只用于索引 s_Keybuffer（DirectInput 的原生格式），
    //                    以及 NotifyWin32Key 那条通路的换算。
    //   s_VKMapping    : DigitalInput -> **Windows 虚拟键码 (VK)**
    //                    只用于 GetAsyncKeyState。
    //
    // 历史 bug：原实现只有一张 DIK 表，却把它直接塞给 GetAsyncKeyState。
    // DIK 与 VK 的数值完全不同（DIK_ESCAPE=0x01 vs VK_ESCAPE=0x1B），
    // 于是 0x01 被当成了 VK_LBUTTON —— **按下鼠标左键等于按下 Escape**，
    // IsDone() 立刻返回 true，程序静默退出。表现为"点一下就闪退"。
    unsigned char s_DXKeyMapping[GameInput::kNumKeys]; // DigitalInput -> DIK (扫描码)
    unsigned char s_VKMapping[GameInput::kNumKeys];    // DigitalInput -> VK  (虚拟键码)

#endif

#ifdef USE_XINPUT
    float FilterAnalogInput(int val, int deadZone)
    {
        if (val < 0)
        {
            if (val > -deadZone)
                return 0.0f;
            else
                return (val + deadZone) / (32768.0f - deadZone);
        }
        else
        {
            if (val < deadZone)
                return 0.0f;
            else
                return (val - deadZone) / (32767.0f - deadZone);
        }
    }
#else
    float FilterAnalogInput(float val, float deadZone)
    {
        if (val < -deadZone)
            return (val + deadZone) / (1.0f - deadZone);
        else if (val > deadZone)
            return (val - deadZone) / (1.0f - deadZone);
        else
            return 0.0f;
    }
#endif

#ifdef USE_KEYBOARD_MOUSE
    void KbmBuildKeyMapping()
    {
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
        s_DXKeyMapping[GameInput::kKey_escape] = 1;
        s_DXKeyMapping[GameInput::kKey_1] = 2;
        s_DXKeyMapping[GameInput::kKey_2] = 3;
        s_DXKeyMapping[GameInput::kKey_3] = 4;
        s_DXKeyMapping[GameInput::kKey_4] = 5;
        s_DXKeyMapping[GameInput::kKey_5] = 6;
        s_DXKeyMapping[GameInput::kKey_6] = 7;
        s_DXKeyMapping[GameInput::kKey_7] = 8;
        s_DXKeyMapping[GameInput::kKey_8] = 9;
        s_DXKeyMapping[GameInput::kKey_9] = 10;
        s_DXKeyMapping[GameInput::kKey_0] = 11;
        s_DXKeyMapping[GameInput::kKey_minus] = 12;
        s_DXKeyMapping[GameInput::kKey_equals] = 13;
        s_DXKeyMapping[GameInput::kKey_back] = 14;
        s_DXKeyMapping[GameInput::kKey_tab] = 15;
        s_DXKeyMapping[GameInput::kKey_q] = 16;
        s_DXKeyMapping[GameInput::kKey_w] = 17;
        s_DXKeyMapping[GameInput::kKey_e] = 18;
        s_DXKeyMapping[GameInput::kKey_r] = 19;
        s_DXKeyMapping[GameInput::kKey_t] = 20;
        s_DXKeyMapping[GameInput::kKey_y] = 21;
        s_DXKeyMapping[GameInput::kKey_u] = 22;
        s_DXKeyMapping[GameInput::kKey_i] = 23;
        s_DXKeyMapping[GameInput::kKey_o] = 24;
        s_DXKeyMapping[GameInput::kKey_p] = 25;
        s_DXKeyMapping[GameInput::kKey_lbracket] = 26;
        s_DXKeyMapping[GameInput::kKey_rbracket] = 27;
        s_DXKeyMapping[GameInput::kKey_return] = 28;
        s_DXKeyMapping[GameInput::kKey_lcontrol] = 29;
        s_DXKeyMapping[GameInput::kKey_a] = 30;
        s_DXKeyMapping[GameInput::kKey_s] = 31;
        s_DXKeyMapping[GameInput::kKey_d] = 32;
        s_DXKeyMapping[GameInput::kKey_f] = 33;
        s_DXKeyMapping[GameInput::kKey_g] = 34;
        s_DXKeyMapping[GameInput::kKey_h] = 35;
        s_DXKeyMapping[GameInput::kKey_j] = 36;
        s_DXKeyMapping[GameInput::kKey_k] = 37;
        s_DXKeyMapping[GameInput::kKey_l] = 38;
        s_DXKeyMapping[GameInput::kKey_semicolon] = 39;
        s_DXKeyMapping[GameInput::kKey_apostrophe] = 40;
        s_DXKeyMapping[GameInput::kKey_grave] = 41;
        s_DXKeyMapping[GameInput::kKey_lshift] = 42;
        s_DXKeyMapping[GameInput::kKey_backslash] = 43;
        s_DXKeyMapping[GameInput::kKey_z] = 44;
        s_DXKeyMapping[GameInput::kKey_x] = 45;
        s_DXKeyMapping[GameInput::kKey_c] = 46;
        s_DXKeyMapping[GameInput::kKey_v] = 47;
        s_DXKeyMapping[GameInput::kKey_b] = 48;
        s_DXKeyMapping[GameInput::kKey_n] = 49;
        s_DXKeyMapping[GameInput::kKey_m] = 50;
        s_DXKeyMapping[GameInput::kKey_comma] = 51;
        s_DXKeyMapping[GameInput::kKey_period] = 52;
        s_DXKeyMapping[GameInput::kKey_slash] = 53;
        s_DXKeyMapping[GameInput::kKey_rshift] = 54;
        s_DXKeyMapping[GameInput::kKey_multiply] = 55;
        s_DXKeyMapping[GameInput::kKey_lalt] = 56;
        s_DXKeyMapping[GameInput::kKey_space] = 57;
        s_DXKeyMapping[GameInput::kKey_capital] = 58;
        s_DXKeyMapping[GameInput::kKey_f1] = 59;
        s_DXKeyMapping[GameInput::kKey_f2] = 60;
        s_DXKeyMapping[GameInput::kKey_f3] = 61;
        s_DXKeyMapping[GameInput::kKey_f4] = 62;
        s_DXKeyMapping[GameInput::kKey_f5] = 63;
        s_DXKeyMapping[GameInput::kKey_f6] = 64;
        s_DXKeyMapping[GameInput::kKey_f7] = 65;
        s_DXKeyMapping[GameInput::kKey_f8] = 66;
        s_DXKeyMapping[GameInput::kKey_f9] = 67;
        s_DXKeyMapping[GameInput::kKey_f10] = 68;
        s_DXKeyMapping[GameInput::kKey_numlock] = 69;
        s_DXKeyMapping[GameInput::kKey_scroll] = 70;
        s_DXKeyMapping[GameInput::kKey_numpad7] = 71;
        s_DXKeyMapping[GameInput::kKey_numpad8] = 72;
        s_DXKeyMapping[GameInput::kKey_numpad9] = 73;
        s_DXKeyMapping[GameInput::kKey_subtract] = 74;
        s_DXKeyMapping[GameInput::kKey_numpad4] = 75;
        s_DXKeyMapping[GameInput::kKey_numpad5] = 76;
        s_DXKeyMapping[GameInput::kKey_numpad6] = 77;
        s_DXKeyMapping[GameInput::kKey_add] = 78;
        s_DXKeyMapping[GameInput::kKey_numpad1] = 79;
        s_DXKeyMapping[GameInput::kKey_numpad2] = 80;
        s_DXKeyMapping[GameInput::kKey_numpad3] = 81;
        s_DXKeyMapping[GameInput::kKey_numpad0] = 82;
        s_DXKeyMapping[GameInput::kKey_decimal] = 83;
        s_DXKeyMapping[GameInput::kKey_f11] = 87;
        s_DXKeyMapping[GameInput::kKey_f12] = 88;
        s_DXKeyMapping[GameInput::kKey_numpadenter] = 156;
        s_DXKeyMapping[GameInput::kKey_rcontrol] = 157;
        s_DXKeyMapping[GameInput::kKey_divide] = 181;
        s_DXKeyMapping[GameInput::kKey_sysrq] = 183;
        s_DXKeyMapping[GameInput::kKey_ralt] = 184;
        s_DXKeyMapping[GameInput::kKey_pause] = 197;
        s_DXKeyMapping[GameInput::kKey_home] = 199;
        s_DXKeyMapping[GameInput::kKey_up] = 200;
        s_DXKeyMapping[GameInput::kKey_pgup] = 201;
        s_DXKeyMapping[GameInput::kKey_left] = 203;
        s_DXKeyMapping[GameInput::kKey_right] = 205;
        s_DXKeyMapping[GameInput::kKey_end] = 207;
        s_DXKeyMapping[GameInput::kKey_down] = 208;
        s_DXKeyMapping[GameInput::kKey_pgdn] = 209;
        s_DXKeyMapping[GameInput::kKey_insert] = 210;
        s_DXKeyMapping[GameInput::kKey_delete] = 211;
        s_DXKeyMapping[GameInput::kKey_lwin] = 219;
        s_DXKeyMapping[GameInput::kKey_rwin] = 220;
        s_DXKeyMapping[GameInput::kKey_apps] = 221;

        // ============================================================
        // R4.3：VK（虚拟键码）映射表 —— 供 GetAsyncKeyState 使用。
        // ------------------------------------------------------------
        // 为什么必须与上面的 DIK 表分开：
        //   DIK 是 DirectInput 的**扫描码**（物理按键位置）
        //   VK  是 Windows 的**虚拟键码**（逻辑按键含义）
        //   两者数值体系完全不同，例如：
        //       DIK_ESCAPE = 0x01     VK_ESCAPE = 0x1B
        //       DIK_W      = 0x11     VK_W      = 0x57
        //       DIK_Q      = 0x10     VK_Q      = 0x51
        //
        // 历史 bug（R4.3 修复）：GetAsyncKeyState 被喂了 DIK 值。
        //   最致命的是 DIK_ESCAPE = 0x01，而 0x01 在 VK 里是 **VK_LBUTTON**。
        //   于是「按下鼠标左键」被判定为「按下 Escape」，
        //   IGameApp::IsDone() 立刻返回 true，主循环正常退出 ——
        //   用户看到的就是「点一下就闪退」，而且没有任何错误日志。
        //
        // 未列出的键保持 0；调用处用 `vk != 0` 跳过，
        // 这样不会把 VK_LBUTTON(0x01) 这类**有效**值误当成"未映射"。
        // ============================================================
        s_VKMapping[GameInput::kKey_escape]     = VK_ESCAPE;
        s_VKMapping[GameInput::kKey_1]          = '1';
        s_VKMapping[GameInput::kKey_2]          = '2';
        s_VKMapping[GameInput::kKey_3]          = '3';
        s_VKMapping[GameInput::kKey_4]          = '4';
        s_VKMapping[GameInput::kKey_5]          = '5';
        s_VKMapping[GameInput::kKey_6]          = '6';
        s_VKMapping[GameInput::kKey_7]          = '7';
        s_VKMapping[GameInput::kKey_8]          = '8';
        s_VKMapping[GameInput::kKey_9]          = '9';
        s_VKMapping[GameInput::kKey_0]          = '0';
        s_VKMapping[GameInput::kKey_minus]      = VK_OEM_MINUS;
        s_VKMapping[GameInput::kKey_equals]     = VK_OEM_PLUS;
        s_VKMapping[GameInput::kKey_back]       = VK_BACK;
        s_VKMapping[GameInput::kKey_tab]        = VK_TAB;
        s_VKMapping[GameInput::kKey_q]          = 'Q';
        s_VKMapping[GameInput::kKey_w]          = 'W';
        s_VKMapping[GameInput::kKey_e]          = 'E';
        s_VKMapping[GameInput::kKey_r]          = 'R';
        s_VKMapping[GameInput::kKey_t]          = 'T';
        s_VKMapping[GameInput::kKey_y]          = 'Y';
        s_VKMapping[GameInput::kKey_u]          = 'U';
        s_VKMapping[GameInput::kKey_i]          = 'I';
        s_VKMapping[GameInput::kKey_o]          = 'O';
        s_VKMapping[GameInput::kKey_p]          = 'P';
        s_VKMapping[GameInput::kKey_lbracket]   = VK_OEM_4;
        s_VKMapping[GameInput::kKey_rbracket]   = VK_OEM_6;
        s_VKMapping[GameInput::kKey_return]     = VK_RETURN;
        s_VKMapping[GameInput::kKey_lcontrol]   = VK_LCONTROL;
        s_VKMapping[GameInput::kKey_a]          = 'A';
        s_VKMapping[GameInput::kKey_s]          = 'S';
        s_VKMapping[GameInput::kKey_d]          = 'D';
        s_VKMapping[GameInput::kKey_f]          = 'F';
        s_VKMapping[GameInput::kKey_g]          = 'G';
        s_VKMapping[GameInput::kKey_h]          = 'H';
        s_VKMapping[GameInput::kKey_j]          = 'J';
        s_VKMapping[GameInput::kKey_k]          = 'K';
        s_VKMapping[GameInput::kKey_l]          = 'L';
        s_VKMapping[GameInput::kKey_semicolon]  = VK_OEM_1;
        s_VKMapping[GameInput::kKey_apostrophe] = VK_OEM_7;
        s_VKMapping[GameInput::kKey_grave]      = VK_OEM_3;
        s_VKMapping[GameInput::kKey_lshift]     = VK_LSHIFT;
        s_VKMapping[GameInput::kKey_backslash]  = VK_OEM_5;
        s_VKMapping[GameInput::kKey_z]          = 'Z';
        s_VKMapping[GameInput::kKey_x]          = 'X';
        s_VKMapping[GameInput::kKey_c]          = 'C';
        s_VKMapping[GameInput::kKey_v]          = 'V';
        s_VKMapping[GameInput::kKey_b]          = 'B';
        s_VKMapping[GameInput::kKey_n]          = 'N';
        s_VKMapping[GameInput::kKey_m]          = 'M';
        s_VKMapping[GameInput::kKey_comma]      = VK_OEM_COMMA;
        s_VKMapping[GameInput::kKey_period]     = VK_OEM_PERIOD;
        s_VKMapping[GameInput::kKey_slash]      = VK_OEM_2;
        s_VKMapping[GameInput::kKey_rshift]     = VK_RSHIFT;
        s_VKMapping[GameInput::kKey_multiply]   = VK_MULTIPLY;
        s_VKMapping[GameInput::kKey_lalt]       = VK_LMENU;
        s_VKMapping[GameInput::kKey_space]      = VK_SPACE;
        s_VKMapping[GameInput::kKey_capital]    = VK_CAPITAL;
        s_VKMapping[GameInput::kKey_f1]         = VK_F1;
        s_VKMapping[GameInput::kKey_f2]         = VK_F2;
        s_VKMapping[GameInput::kKey_f3]         = VK_F3;
        s_VKMapping[GameInput::kKey_f4]         = VK_F4;
        s_VKMapping[GameInput::kKey_f5]         = VK_F5;
        s_VKMapping[GameInput::kKey_f6]         = VK_F6;
        s_VKMapping[GameInput::kKey_f7]         = VK_F7;
        s_VKMapping[GameInput::kKey_f8]         = VK_F8;
        s_VKMapping[GameInput::kKey_f9]         = VK_F9;
        s_VKMapping[GameInput::kKey_f10]        = VK_F10;
        s_VKMapping[GameInput::kKey_numlock]    = VK_NUMLOCK;
        s_VKMapping[GameInput::kKey_scroll]     = VK_SCROLL;
        s_VKMapping[GameInput::kKey_numpad7]    = VK_NUMPAD7;
        s_VKMapping[GameInput::kKey_numpad8]    = VK_NUMPAD8;
        s_VKMapping[GameInput::kKey_numpad9]    = VK_NUMPAD9;
        s_VKMapping[GameInput::kKey_subtract]   = VK_SUBTRACT;
        s_VKMapping[GameInput::kKey_numpad4]    = VK_NUMPAD4;
        s_VKMapping[GameInput::kKey_numpad5]    = VK_NUMPAD5;
        s_VKMapping[GameInput::kKey_numpad6]    = VK_NUMPAD6;
        s_VKMapping[GameInput::kKey_add]        = VK_ADD;
        s_VKMapping[GameInput::kKey_numpad1]    = VK_NUMPAD1;
        s_VKMapping[GameInput::kKey_numpad2]    = VK_NUMPAD2;
        s_VKMapping[GameInput::kKey_numpad3]    = VK_NUMPAD3;
        s_VKMapping[GameInput::kKey_numpad0]    = VK_NUMPAD0;
        s_VKMapping[GameInput::kKey_decimal]    = VK_DECIMAL;
        s_VKMapping[GameInput::kKey_f11]        = VK_F11;
        s_VKMapping[GameInput::kKey_f12]        = VK_F12;
        s_VKMapping[GameInput::kKey_numpadenter]= VK_RETURN;
        s_VKMapping[GameInput::kKey_rcontrol]   = VK_RCONTROL;
        s_VKMapping[GameInput::kKey_divide]     = VK_DIVIDE;
        s_VKMapping[GameInput::kKey_sysrq]      = VK_SNAPSHOT;
        s_VKMapping[GameInput::kKey_ralt]       = VK_RMENU;
        s_VKMapping[GameInput::kKey_pause]      = VK_PAUSE;
        s_VKMapping[GameInput::kKey_home]       = VK_HOME;
        s_VKMapping[GameInput::kKey_up]         = VK_UP;
        s_VKMapping[GameInput::kKey_pgup]       = VK_PRIOR;
        s_VKMapping[GameInput::kKey_left]       = VK_LEFT;
        s_VKMapping[GameInput::kKey_right]      = VK_RIGHT;
        s_VKMapping[GameInput::kKey_end]        = VK_END;
        s_VKMapping[GameInput::kKey_down]       = VK_DOWN;
        s_VKMapping[GameInput::kKey_pgdn]       = VK_NEXT;
        s_VKMapping[GameInput::kKey_insert]     = VK_INSERT;
        s_VKMapping[GameInput::kKey_delete]     = VK_DELETE;
        s_VKMapping[GameInput::kKey_lwin]       = VK_LWIN;
        s_VKMapping[GameInput::kKey_rwin]       = VK_RWIN;
        s_VKMapping[GameInput::kKey_apps]       = VK_APPS;
#else
#define WinRTKey(name) (unsigned char)Windows::System::VirtualKey::name
        s_DXKeyMapping[GameInput::kKey_escape] = WinRTKey(Escape);
        s_DXKeyMapping[GameInput::kKey_1] = WinRTKey(Number1);
        s_DXKeyMapping[GameInput::kKey_2] = WinRTKey(Number2);
        s_DXKeyMapping[GameInput::kKey_3] = WinRTKey(Number3);
        s_DXKeyMapping[GameInput::kKey_4] = WinRTKey(Number4);
        s_DXKeyMapping[GameInput::kKey_5] = WinRTKey(Number5);
        s_DXKeyMapping[GameInput::kKey_6] = WinRTKey(Number6);
        s_DXKeyMapping[GameInput::kKey_7] = WinRTKey(Number7);
        s_DXKeyMapping[GameInput::kKey_8] = WinRTKey(Number8);
        s_DXKeyMapping[GameInput::kKey_9] = WinRTKey(Number9);
        s_DXKeyMapping[GameInput::kKey_0] = WinRTKey(Number0);
        s_DXKeyMapping[GameInput::kKey_minus] = WinRTKey(Subtract);
        s_DXKeyMapping[GameInput::kKey_equals] = WinRTKey(Add);
        s_DXKeyMapping[GameInput::kKey_back] = WinRTKey(Back);
        s_DXKeyMapping[GameInput::kKey_tab] = WinRTKey(Tab);
        s_DXKeyMapping[GameInput::kKey_q] = WinRTKey(Q);
        s_DXKeyMapping[GameInput::kKey_w] = WinRTKey(W);
        s_DXKeyMapping[GameInput::kKey_e] = WinRTKey(E);
        s_DXKeyMapping[GameInput::kKey_r] = WinRTKey(R);
        s_DXKeyMapping[GameInput::kKey_t] = WinRTKey(T);
        s_DXKeyMapping[GameInput::kKey_y] = WinRTKey(Y);
        s_DXKeyMapping[GameInput::kKey_u] = WinRTKey(U);
        s_DXKeyMapping[GameInput::kKey_i] = WinRTKey(I);
        s_DXKeyMapping[GameInput::kKey_o] = WinRTKey(O);
        s_DXKeyMapping[GameInput::kKey_p] = WinRTKey(P);
        s_DXKeyMapping[GameInput::kKey_lbracket] = 219;
        s_DXKeyMapping[GameInput::kKey_rbracket] = 221;
        s_DXKeyMapping[GameInput::kKey_return] = WinRTKey(Enter);
        s_DXKeyMapping[GameInput::kKey_lcontrol] = WinRTKey(Control);  // No L/R
        s_DXKeyMapping[GameInput::kKey_a] = WinRTKey(A);
        s_DXKeyMapping[GameInput::kKey_s] = WinRTKey(S);
        s_DXKeyMapping[GameInput::kKey_d] = WinRTKey(D);
        s_DXKeyMapping[GameInput::kKey_f] = WinRTKey(F);
        s_DXKeyMapping[GameInput::kKey_g] = WinRTKey(G);
        s_DXKeyMapping[GameInput::kKey_h] = WinRTKey(H);
        s_DXKeyMapping[GameInput::kKey_j] = WinRTKey(J);
        s_DXKeyMapping[GameInput::kKey_k] = WinRTKey(K);
        s_DXKeyMapping[GameInput::kKey_l] = WinRTKey(L);
        s_DXKeyMapping[GameInput::kKey_semicolon] = 186;
        s_DXKeyMapping[GameInput::kKey_apostrophe] = 222;
        s_DXKeyMapping[GameInput::kKey_grave] = 192; // ` or ~
        s_DXKeyMapping[GameInput::kKey_lshift] = WinRTKey(LeftShift);
        s_DXKeyMapping[GameInput::kKey_backslash] = 220;
        s_DXKeyMapping[GameInput::kKey_z] = WinRTKey(Z);
        s_DXKeyMapping[GameInput::kKey_x] = WinRTKey(X);
        s_DXKeyMapping[GameInput::kKey_c] = WinRTKey(C);
        s_DXKeyMapping[GameInput::kKey_v] = WinRTKey(V);
        s_DXKeyMapping[GameInput::kKey_b] = WinRTKey(B);
        s_DXKeyMapping[GameInput::kKey_n] = WinRTKey(N);
        s_DXKeyMapping[GameInput::kKey_m] = WinRTKey(M);
        s_DXKeyMapping[GameInput::kKey_comma] = 188;
        s_DXKeyMapping[GameInput::kKey_period] = 190;
        s_DXKeyMapping[GameInput::kKey_slash] = 191;
        s_DXKeyMapping[GameInput::kKey_rshift] = WinRTKey(RightShift);
        s_DXKeyMapping[GameInput::kKey_multiply] = WinRTKey(Multiply);
        s_DXKeyMapping[GameInput::kKey_lalt] = 255; // Only a modifier
        s_DXKeyMapping[GameInput::kKey_space] = WinRTKey(Space);
        s_DXKeyMapping[GameInput::kKey_capital] = WinRTKey(CapitalLock);
        s_DXKeyMapping[GameInput::kKey_f1] = WinRTKey(F1);
        s_DXKeyMapping[GameInput::kKey_f2] = WinRTKey(F2);
        s_DXKeyMapping[GameInput::kKey_f3] = WinRTKey(F3);
        s_DXKeyMapping[GameInput::kKey_f4] = WinRTKey(F4);
        s_DXKeyMapping[GameInput::kKey_f5] = WinRTKey(F5);
        s_DXKeyMapping[GameInput::kKey_f6] = WinRTKey(F6);
        s_DXKeyMapping[GameInput::kKey_f7] = WinRTKey(F7);
        s_DXKeyMapping[GameInput::kKey_f8] = WinRTKey(F8);
        s_DXKeyMapping[GameInput::kKey_f9] = WinRTKey(F9);
        s_DXKeyMapping[GameInput::kKey_f10] = WinRTKey(F10);
        s_DXKeyMapping[GameInput::kKey_numlock] = WinRTKey(NumberKeyLock);
        s_DXKeyMapping[GameInput::kKey_scroll] = WinRTKey(Scroll);
        s_DXKeyMapping[GameInput::kKey_numpad7] = WinRTKey(NumberPad7);
        s_DXKeyMapping[GameInput::kKey_numpad8] = WinRTKey(NumberPad8);
        s_DXKeyMapping[GameInput::kKey_numpad9] = WinRTKey(NumberPad9);
        s_DXKeyMapping[GameInput::kKey_subtract] = WinRTKey(Subtract);
        s_DXKeyMapping[GameInput::kKey_numpad4] = WinRTKey(NumberPad4);
        s_DXKeyMapping[GameInput::kKey_numpad5] = WinRTKey(NumberPad5);
        s_DXKeyMapping[GameInput::kKey_numpad6] = WinRTKey(NumberPad6);
        s_DXKeyMapping[GameInput::kKey_add] = WinRTKey(Add);
        s_DXKeyMapping[GameInput::kKey_numpad1] = WinRTKey(NumberPad1);
        s_DXKeyMapping[GameInput::kKey_numpad2] = WinRTKey(NumberPad2);
        s_DXKeyMapping[GameInput::kKey_numpad3] = WinRTKey(NumberPad3);
        s_DXKeyMapping[GameInput::kKey_numpad0] = WinRTKey(NumberPad0);
        s_DXKeyMapping[GameInput::kKey_decimal] = WinRTKey(Decimal);
        s_DXKeyMapping[GameInput::kKey_f11] = WinRTKey(F11);
        s_DXKeyMapping[GameInput::kKey_f12] = WinRTKey(F12);
        s_DXKeyMapping[GameInput::kKey_numpadenter] = WinRTKey(Enter); // No distinction
        s_DXKeyMapping[GameInput::kKey_rcontrol] = WinRTKey(Control);  // No L/R
        s_DXKeyMapping[GameInput::kKey_divide] = WinRTKey(Divide);
        s_DXKeyMapping[GameInput::kKey_sysrq] = 255; // Ignored
        s_DXKeyMapping[GameInput::kKey_ralt] = 255; // Only a modifier
        s_DXKeyMapping[GameInput::kKey_pause] = WinRTKey(Pause);
        s_DXKeyMapping[GameInput::kKey_home] = WinRTKey(Home);
        s_DXKeyMapping[GameInput::kKey_up] = WinRTKey(Up);
        s_DXKeyMapping[GameInput::kKey_pgup] = WinRTKey(PageUp);
        s_DXKeyMapping[GameInput::kKey_left] = WinRTKey(Left);
        s_DXKeyMapping[GameInput::kKey_right] = WinRTKey(Right);
        s_DXKeyMapping[GameInput::kKey_end] = WinRTKey(End);
        s_DXKeyMapping[GameInput::kKey_down] = WinRTKey(Down);
        s_DXKeyMapping[GameInput::kKey_pgdn] = WinRTKey(PageDown);
        s_DXKeyMapping[GameInput::kKey_insert] = WinRTKey(Insert);
        s_DXKeyMapping[GameInput::kKey_delete] = WinRTKey(Delete);
        s_DXKeyMapping[GameInput::kKey_lwin] = WinRTKey(LeftWindows);
        s_DXKeyMapping[GameInput::kKey_rwin] = WinRTKey(RightWindows);
        s_DXKeyMapping[GameInput::kKey_apps] = WinRTKey(Application);
#endif
    }

    void KbmZeroInputs()
    {
        memset(&s_MouseState, 0, sizeof(DIMOUSESTATE2));
        memset(s_Keybuffer, 0, sizeof(s_Keybuffer));
    }

    void KbmInitialize()
    {
        LOG_TRACE(L"[Boot] GameInput: KbmBuildKeyMapping\n");
        KbmBuildKeyMapping();
        LOG_TRACE(L"[Boot] GameInput: KbmBuildKeyMapping done\n");

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
        // R0 修复（两处）：
        // 1. 接口类型不一致：原来用 IID_IDirectInput8（宽字符版）却把结果存进
        //    IDirectInput8A*（窄字符版）。两者是不同的 COM 接口，指针不能互换，
        //    后续 s_DI->CreateDevice 会走到错误的虚表槽。改用 IID_IDirectInput8A。
        // 2. 失败不致命：DirectInput 在某些环境（服务会话、远程桌面、或被安全软件
        //    拦截）下会返回 E_FAIL。原来这里直接 ASSERT + __debugbreak，导致
        //    程序在「输入不可用」这种非核心问题上直接退出，无法继续渲染验证。
        //    现在改为记录警告 + 关闭键鼠输入，渲染链路继续运行。
        LOG_TRACE(L"[Boot] GameInput: DirectInput8Create\n");
        HRESULT hrDI = DirectInput8Create(GetModuleHandle(nullptr), DIRECTINPUT_VERSION,
                                          IID_IDirectInput8A, (void**)&s_DI, nullptr);
        LOG_TRACE(L"[Boot] GameInput: DirectInput8Create hr=0x%08X\n", (unsigned)hrDI);

        bool kbmReady = SUCCEEDED(hrDI) && s_DI != nullptr;
        if (!kbmReady)
        {
            LOG_TRACE(L"[Boot] GameInput: DirectInput 不可用，键鼠输入已禁用（渲染不受影响）\n");
            s_DI = nullptr;
        }

        if (kbmReady && FAILED(s_DI->CreateDevice(GUID_SysKeyboard, &s_Keyboard, nullptr)))
        {
            LOG_TRACE(L"[Boot] GameInput: Keyboard CreateDevice failed\n");
            kbmReady = false;
        }
        if (kbmReady && FAILED(s_Keyboard->SetDataFormat(&c_dfDIKeyboard)))
        {
            LOG_TRACE(L"[Boot] GameInput: Keyboard SetDataFormat failed\n");
            kbmReady = false;
        }
        if (kbmReady && FAILED(s_Keyboard->SetCooperativeLevel(GameCore::g_hWnd, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE)))
        {
            LOG_TRACE(L"[Boot] GameInput: Keyboard SetCooperativeLevel failed\n");
            kbmReady = false;
        }

        if (kbmReady)
        {
            DIPROPDWORD dipdw;
            dipdw.diph.dwSize = sizeof(DIPROPDWORD);
            dipdw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
            dipdw.diph.dwObj = 0;
            dipdw.diph.dwHow = DIPH_DEVICE;
            dipdw.dwData = 10;
            if (FAILED(s_Keyboard->SetProperty(DIPROP_BUFFERSIZE, &dipdw.diph)))
            {
                LOG_TRACE(L"[Boot] GameInput: Keyboard set buffer size failed\n");
                kbmReady = false;
            }
        }

        if (kbmReady && FAILED(s_DI->CreateDevice(GUID_SysMouse, &s_Mouse, nullptr)))
        {
            LOG_TRACE(L"[Boot] GameInput: Mouse CreateDevice failed\n");
            kbmReady = false;
        }
        if (kbmReady && FAILED(s_Mouse->SetDataFormat(&c_dfDIMouse2)))
        {
            LOG_TRACE(L"[Boot] GameInput: Mouse SetDataFormat failed\n");
            kbmReady = false;
        }
        if (kbmReady && FAILED(s_Mouse->SetCooperativeLevel(GameCore::g_hWnd, DISCL_FOREGROUND | DISCL_EXCLUSIVE)))
        {
            LOG_TRACE(L"[Boot] GameInput: Mouse SetCooperativeLevel failed\n");
            kbmReady = false;
        }

        s_KbmReady = kbmReady;
        LOG_TRACE(L"[Boot] GameInput: Kbm ready = %s\n", kbmReady ? L"yes" : L"no");
#endif

        KbmZeroInputs();
    }

    void KbmShutdown()
    {
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
        if (s_Keyboard)
        {
            s_Keyboard->Unacquire();
            s_Keyboard->Release();
            s_Keyboard = nullptr;
        }
        if (s_Mouse)
        {
            s_Mouse->Unacquire();
            s_Mouse->Release();
            s_Mouse = nullptr;
        }
        if (s_DI)
        {
            s_DI->Release();
            s_DI = nullptr;
        }
#endif
    }

    void KbmUpdate()
    {
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
        // ============================================================
        // DirectInput 不可用时的处理（R1 修正）
        // ------------------------------------------------------------
        // R0 的写法是「直接 KbmZeroInputs() 然后 return」，
        // 想避免空指针，但副作用是：**每帧都把整个 s_Keybuffer 清零**，
        // 连带把 WndProc 通过 NotifyWin32Key 写入的键位也擦掉，
        // 结果就是 WASD 完全无响应（这是 R1 期间发现的真实回归）。
        //
        // 正确做法：只跳过 DirectInput 自己的读取，不要清空共享状态。
        // 键位由 Win32 消息通路维护（NotifyWin32Key）。
        // ============================================================
        if (!s_KbmReady)
        {
            // ============================================================
            // R4.5：DirectInput 不可用时的鼠标后备来源（修复「鼠标无法转视角」）
            // ------------------------------------------------------------
            // 原实现直接 memset(s_MouseState, 0) 然后 return：键盘因为有
            // Win32 消息通路还能用，但**鼠标位移彻底变成 0**，
            // 表现为「鼠标左右拖动，视角完全不动」。
            //
            // 本机实测 DirectInput8Create 失败，走的正是这条分支 ——
            // 所以这个"后备"其实是唯一通路。
            //
            // 后备方案：用 GetCursorPos 求相邻两帧的差分。
            //   - 只在窗口是前台时采样，避免在后台误转视角
            //   - 每次算完更新 s_LastCursorPos 作为下一次基准
            //   - 不改变 s_MouseState 的语义（lX/lY 仍是相对位移）
            // 缺点：光标会跑到屏幕边缘后不再产生位移（没有独占捕获）。
            // 这是无 DirectInput 时的固有限制，可在后续用 ClipCursor 改进。
            // ============================================================
            memset(&s_MouseState, 0, sizeof(s_MouseState));

            HWND fg = ::GetForegroundWindow();
            if (fg == GameCore::g_hWnd && ::IsWindowVisible(fg))
            {
                POINT pt = {};
                if (::GetCursorPos(&pt))
                {
                    if (s_LastCursorPosValid)
                    {
                        s_MouseState.lX = (LONG)(pt.x - s_LastCursorPos.x);
                        s_MouseState.lY = (LONG)(pt.y - s_LastCursorPos.y);
                    }
                    s_LastCursorPos = pt;
                    s_LastCursorPosValid = true;
                }

                // 鼠标按键：DirectInput 不可用时同样要补
                // 用 GetAsyncKeyState 的 VK_LBUTTON / VK_RBUTTON / VK_MBUTTON 系列
                for (int b = 0; b < 8; ++b)
                {
                    // VK_LBUTTON=0x01, VK_RBUTTON=0x02, VK_MBUTTON=0x04, VK_XBUTTON1=0x05, VK_XBUTTON2=0x06
                    static const int kMouseVK[8] = { 0x01, 0x02, 0x04, 0x05, 0x06, 0, 0, 0 };
                    if (kMouseVK[b] != 0 && (::GetAsyncKeyState(kMouseVK[b]) & 0x8000) != 0)
                        s_MouseState.rgbButtons[b] = 0x80;
                }
            }
            else
            {
                s_LastCursorPosValid = false;
            }
            return;
        }

        HWND foreground = GetForegroundWindow();
        bool visible = IsWindowVisible(foreground) != 0;

        if (foreground != GameCore::g_hWnd // wouldn't be able to acquire
            || !visible)
        {
            KbmZeroInputs();
        }
        else
        {
            s_Mouse->Acquire();
            s_Mouse->GetDeviceState(sizeof(DIMOUSESTATE2), &s_MouseState);
            s_Keyboard->Acquire();
            s_Keyboard->GetDeviceState(sizeof(s_Keybuffer), s_Keybuffer);
        }
#endif
    }

#endif

}

#if !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_TV_TITLE)
void GameInput::SetKeyState(Windows::System::VirtualKey key, bool IsDown)
{
    s_Keybuffer[(unsigned char)key] = IsDown ? 0x80 : 0x00;
    //DEBUGPRINT("%d key is %s", (unsigned int)key, IsDown ? "down" : "up");
}
#endif

void GameInput::Initialize()
{
    // For Windows 8
    //    XInputEnable(TRUE);

    ZeroMemory(s_Buttons, sizeof(s_Buttons));
    ZeroMemory(s_Analogs, sizeof(s_Analogs));

#ifdef USE_KEYBOARD_MOUSE
    KbmInitialize();
#endif
}

// ============================================================
// Win32 键盘消息注入（R1）
// ------------------------------------------------------------
// DirectInput 在本机不可用（DirectInput8Create 返回 E_FAIL），
// 而 GetAsyncKeyState 对合成输入（SendKeys / 远程桌面 / 自动化测试）
// 也经常返回 0，导致 WASD 完全无响应。
//
// 最可靠的通路是 Win32 的 WM_KEYDOWN/WM_KEYUP 消息：
// 无论输入来自真实键盘还是 SendInput/SendKeys，窗口都会收到。
// WndProc 收到消息后调用本函数写入 s_Keybuffer，
// 与 KbmUpdate 读取的是同一份状态，因此相机等上层逻辑无需改动。
// ============================================================
void GameInput::NotifyWin32Key(UINT vkCode, bool isDown)
{
#ifdef USE_KEYBOARD_MOUSE
    if (vkCode < 256)
    {
        s_Keybuffer[vkCode] = isDown ? 0x80 : 0x00;
    }
#endif
}

void GameInput::Shutdown()
{
#ifdef USE_KEYBOARD_MOUSE
    KbmShutdown();
#endif
}

void GameInput::Update(float frameDelta)
{
    memcpy(s_Buttons[1], s_Buttons[0], sizeof(s_Buttons[0]));
    memset(s_Buttons[0], 0, sizeof(s_Buttons[0]));
    memset(s_Analogs, 0, sizeof(s_Analogs));

#ifdef USE_XINPUT
    XINPUT_STATE newInputState;
    if (ERROR_SUCCESS == XInputGetState(0, &newInputState))
    {
        if (newInputState.Gamepad.wButtons & (1 << 0)) s_Buttons[0][kDPadUp] = true;
        if (newInputState.Gamepad.wButtons & (1 << 1)) s_Buttons[0][kDPadDown] = true;
        if (newInputState.Gamepad.wButtons & (1 << 2)) s_Buttons[0][kDPadLeft] = true;
        if (newInputState.Gamepad.wButtons & (1 << 3)) s_Buttons[0][kDPadRight] = true;
        if (newInputState.Gamepad.wButtons & (1 << 4)) s_Buttons[0][kStartButton] = true;
        if (newInputState.Gamepad.wButtons & (1 << 5)) s_Buttons[0][kBackButton] = true;
        if (newInputState.Gamepad.wButtons & (1 << 6)) s_Buttons[0][kLThumbClick] = true;
        if (newInputState.Gamepad.wButtons & (1 << 7)) s_Buttons[0][kRThumbClick] = true;
        if (newInputState.Gamepad.wButtons & (1 << 8)) s_Buttons[0][kLShoulder] = true;
        if (newInputState.Gamepad.wButtons & (1 << 9)) s_Buttons[0][kRShoulder] = true;
        if (newInputState.Gamepad.wButtons & (1 << 12)) s_Buttons[0][kAButton] = true;
        if (newInputState.Gamepad.wButtons & (1 << 13)) s_Buttons[0][kBButton] = true;
        if (newInputState.Gamepad.wButtons & (1 << 14)) s_Buttons[0][kXButton] = true;
        if (newInputState.Gamepad.wButtons & (1 << 15)) s_Buttons[0][kYButton] = true;

        s_Analogs[kAnalogLeftTrigger] = newInputState.Gamepad.bLeftTrigger / 255.0f;
        s_Analogs[kAnalogRightTrigger] = newInputState.Gamepad.bRightTrigger / 255.0f;
        s_Analogs[kAnalogLeftStickX] = FilterAnalogInput(newInputState.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        s_Analogs[kAnalogLeftStickY] = FilterAnalogInput(newInputState.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        s_Analogs[kAnalogRightStickX] = FilterAnalogInput(newInputState.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        s_Analogs[kAnalogRightStickY] = FilterAnalogInput(newInputState.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    }
#else

    IVectorView<Gamepad^>^ gamepads = Gamepad::Gamepads;
    if (gamepads->Size != 0)
    {
        IGamepad^ gamepad = gamepads->GetAt(0);
        GamepadReading reading = gamepad->GetCurrentReading();
        uint32_t Buttons = (uint32_t)reading.Buttons;
        if (Buttons & (uint32_t)GamepadButtons::DPadUp) s_Buttons[0][kDPadUp] = true;
        if (Buttons & (uint32_t)GamepadButtons::DPadDown) s_Buttons[0][kDPadDown] = true;
        if (Buttons & (uint32_t)GamepadButtons::DPadLeft) s_Buttons[0][kDPadLeft] = true;
        if (Buttons & (uint32_t)GamepadButtons::DPadRight) s_Buttons[0][kDPadRight] = true;
        if (Buttons & (uint32_t)GamepadButtons::Menu) s_Buttons[0][kStartButton] = true;
        if (Buttons & (uint32_t)GamepadButtons::View) s_Buttons[0][kBackButton] = true;
        if (Buttons & (uint32_t)GamepadButtons::LeftThumbstick) s_Buttons[0][kLThumbClick] = true;
        if (Buttons & (uint32_t)GamepadButtons::RightThumbstick) s_Buttons[0][kRThumbClick] = true;
        if (Buttons & (uint32_t)GamepadButtons::LeftShoulder) s_Buttons[0][kLShoulder] = true;
        if (Buttons & (uint32_t)GamepadButtons::RightShoulder) s_Buttons[0][kRShoulder] = true;
        if (Buttons & (uint32_t)GamepadButtons::A) s_Buttons[0][kAButton] = true;
        if (Buttons & (uint32_t)GamepadButtons::B) s_Buttons[0][kBButton] = true;
        if (Buttons & (uint32_t)GamepadButtons::X) s_Buttons[0][kXButton] = true;
        if (Buttons & (uint32_t)GamepadButtons::Y) s_Buttons[0][kYButton] = true;

        static const float kAnalogStickDeadZone = 0.18f;

        s_Analogs[kAnalogLeftTrigger] = (float)reading.LeftTrigger;
        s_Analogs[kAnalogRightTrigger] = (float)reading.RightTrigger;
        s_Analogs[kAnalogLeftStickX] = FilterAnalogInput((float)reading.LeftThumbstickX, kAnalogStickDeadZone);
        s_Analogs[kAnalogLeftStickY] = FilterAnalogInput((float)reading.LeftThumbstickY, kAnalogStickDeadZone);
        s_Analogs[kAnalogRightStickX] = FilterAnalogInput((float)reading.RightThumbstickX, kAnalogStickDeadZone);
        s_Analogs[kAnalogRightStickY] = FilterAnalogInput((float)reading.RightThumbstickY, kAnalogStickDeadZone);
    }

#endif

#ifdef USE_KEYBOARD_MOUSE
    KbmUpdate();

    // ============================================================
    // 键位聚合（R1 修复：DirectInput 不可用时 WASD 完全失效）
    // ------------------------------------------------------------
    // 原实现只从 s_Keybuffer（DirectInput 的键盘状态）取键，
    // 而某些环境（本机实测）DirectInput8Create 直接返回 E_FAIL，
    // 于是 s_Keybuffer 恒为 0 —— 相机无法用 WASD 移动，
    // 但程序本身还能正常渲染，问题很隐蔽。
    //
    // 现在改用 Win32 的 GetAsyncKeyState 作为键位来源：
    //   - 不依赖 DirectInput，COM 初始化失败也不受影响
    //   - 语义等价（都返回"当前是否按下"）
    //   - 只在窗口是前台时读取，避免在后台误响应
    // DirectInput 仍保留用于鼠标（相对位移需要独占读取），
    // 若 DirectInput 可用则鼠标部分照常工作。
    // ============================================================
    const HWND fg = ::GetForegroundWindow();
    const bool windowActive = (fg == GameCore::g_hWnd);

    for (uint32_t i = 0; i < kNumKeys; ++i)
    {
        // 三条通路取「或」，任一可用即可：
        //   1) Win32 消息（NotifyWin32Key 写入 s_Keybuffer）—— 最可靠
        //   2) GetAsyncKeyState —— 真实键盘
        //   3) DirectInput 的 s_Keybuffer（若可用）
        // 之所以不做互斥选择，是因为不同环境（本机/远程/自动化）下
        // 三种来源的可用性不同，取并集最稳。
        bool down = false;

        // 通路 1/3：DirectInput 的 s_Keybuffer 与 NotifyWin32Key 通路。
        // 两者都用 **DIK 扫描码** 作为下标，所以这里取 s_DXKeyMapping。
        const unsigned char dik = s_DXKeyMapping[i];
        if (dik < 256 && (s_Keybuffer[dik] & 0x80) != 0)
            down = true;

        // 通路 2：GetAsyncKeyState —— 必须用 **VK 虚拟键码**。
        // R4.3 修复：原实现这里传的是 dik，导致两个后果：
        //   1) 几乎所有字母键都读错位置（DIK_W=0x11 vs VK_W=0x57），
        //      这也是"WASD 时灵时不灵"的根源；
        //   2) 最致命的是 DIK_ESCAPE=0x01 恰好等于 VK_LBUTTON ——
        //      **点击鼠标左键 = 按下 Escape**，IsDone() 立即为真，
        //      主循环正常退出，表现为"点一下就闪退"且无任何错误日志。
        const unsigned char vk = s_VKMapping[i];
        if (!down && windowActive && vk != 0)
            down = (::GetAsyncKeyState(vk) & 0x8000) != 0;

        s_Buttons[0][i] = down;
    }

    for (uint32_t i = 0; i < 8; ++i)
    {
        if (s_MouseState.rgbButtons[i] > 0) s_Buttons[0][kMouse0 + i] = true;
    }

    s_Analogs[kAnalogMouseX] = (float)s_MouseState.lX * .0018f;
    s_Analogs[kAnalogMouseY] = (float)s_MouseState.lY * -.0018f;

    if (s_MouseState.lZ > 0)
        s_Analogs[kAnalogMouseScroll] = 1.0f;
    else if (s_MouseState.lZ < 0)
        s_Analogs[kAnalogMouseScroll] = -1.0f;
#endif

    // Update time duration for buttons pressed
    for (uint32_t i = 0; i < kNumDigitalInputs; ++i)
    {
        if (s_Buttons[0][i])
        {
            if (!s_Buttons[1][i])
                s_HoldDuration[i] = 0.0f;
            else
                s_HoldDuration[i] += frameDelta;
        }
    }

    for (uint32_t i = 0; i < kNumAnalogInputs; ++i)
    {
        s_AnalogsTC[i] = s_Analogs[i] * frameDelta;
    }

}

bool GameInput::IsAnyPressed(void)
{
    return s_Buttons[0] != 0;
}

bool GameInput::IsPressed(DigitalInput di)
{
    return s_Buttons[0][di];
}

bool GameInput::IsFirstPressed(DigitalInput di)
{
    return s_Buttons[0][di] && !s_Buttons[1][di];
}

bool GameInput::IsReleased(DigitalInput di)
{
    return !s_Buttons[0][di];
}

bool GameInput::IsFirstReleased(DigitalInput di)
{
    return !s_Buttons[0][di] && s_Buttons[1][di];
}

float GameInput::GetDurationPressed(DigitalInput di)
{
    return s_HoldDuration[di];
}

float GameInput::GetAnalogInput(AnalogInput ai)
{
    return s_Analogs[ai];
}

float GameInput::GetTimeCorrectedAnalogInput(AnalogInput ai)
{
    return s_AnalogsTC[ai];
}