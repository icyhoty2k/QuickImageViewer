#pragma once
#include <string>
#include <windows.h>

namespace UI {
    // Three permanently-cached dialog windows, each pre-wired for its role:
    //   s_hwndMsg  — informational,  single OK
    //   s_hwndCfm  — confirmation,   Yes / No
    //   s_hwndInt  — numeric input,  Reset Default / Cancel / OK  +  edit field
    class ThemedDialog {
    public:
        static void Init(HWND hMainWnd);
        static void Message(HWND hOwner, const wchar_t *text, const wchar_t *caption);
        static bool Confirm(HWND hOwner, const wchar_t *text, const wchar_t *caption);
        static int  PromptInt(HWND hOwner, const wchar_t *caption, const wchar_t *label,
                              int currentValue, int minVal, int maxVal, int defaultValue);
        static int  PromptFloat(HWND hOwner, const wchar_t *caption, const wchar_t *label,
                                float currentValue, float minVal, float maxVal, float defaultValue);

        static void ApplyTheme(); // called externally on theme change

    private:
        static void RunModalLoop(HWND hwndDlg, HWND hOwner, HWND focusCtrl = nullptr);
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        // ── shared runtime state ────────────────────────────────────────────────
        static int    s_result;
        static bool   s_running;
        static HWND   s_hOwner;
        static HFONT  s_font;
        static HBRUSH s_bgBrush;
        static HBRUSH s_editBrush;
        static HBRUSH s_errorBrush;
        static int    s_intMin;
        static int    s_intMax;
        static int    s_defaultValue;

        static float  s_floatMin;
        static float  s_floatMax;
        static float  s_floatDefault;
        static bool   s_isFloat;
        static bool   s_isFloatError;
        static bool   s_isIntError;
        static std::wstring s_floatLabel;
        static std::wstring s_intLabel;

        // ── Window 1: Message  [OK] ─────────────────────────────────────────────
        static HWND s_hwndMsg;
        static HWND s_textMsg;
        static HWND s_btnMsgOk;

        // ── Window 2: Confirm  [Yes] [No] ──────────────────────────────────────
        static HWND s_hwndCfm;
        static HWND s_textCfm;
        static HWND s_btnCfmYes;
        static HWND s_btnCfmNo;

        // ── Window 3: PromptInt  [Reset Default]  [Cancel] [OK] ───────────────
        static HWND s_hwndInt;
        static HWND s_textInt;
        static HWND s_editInt;
        static HWND s_btnIntOk;
        static HWND s_btnIntCancel;
        static HWND s_btnIntReset;
    };
}
