// FlashLaunch - ultra lightweight Windows quick launcher
// Native Win32 / GDI, no framework, no runtime dependency.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <commctrl.h>
#include <objbase.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cwctype>
#include <cwchar>
#include <cstdio>
#include <cmath>
#include "pinyin_table.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")

#define WM_APP_SHOW       (WM_APP + 1)
#define WM_APP_HIDE       (WM_APP + 2)
#define WM_APP_LAUNCH     (WM_APP + 3)
#define WM_APP_NAV        (WM_APP + 4)
#define WM_APP_OPENFOLDER (WM_APP + 5)
#define WM_APP_INDEXED    (WM_APP + 6)
#define WM_APP_TRAY       (WM_APP + 7)
#define WM_APP_REINDEX    (WM_APP + 8)
#define WM_APP_COPYPATH   (WM_APP + 9)
#define WM_APP_THEME      (WM_APP + 10)
#define WM_APP_THEME_LIVE (WM_APP + 11)

#define TRAY_SHOW     1001
#define TRAY_REINDEX  1002
#define TRAY_AUTOSTART 1003
#define TRAY_SETTINGS 1004
#define TRAY_EXIT     1005

#define ID_HOTKEY_EDIT 2001
#define ID_MAX_EDIT    2002
#define ID_PINYIN_TGL  2003
#define ID_AUTOSTART_TGL 2004
#define ID_PATHS_EDIT  2005
#define ID_BROWSE      2006
#define ID_REBUILD     2007
#define ID_SAVE        2008
#define ID_CANCEL      2009
#define ID_CLOSE       2010
#define ID_NOTE        2011
#define ID_NAV_GENERAL 2012
#define ID_NAV_SEARCH  2013
#define ID_NAV_DIRS    2014
#define ID_NAV_ABOUT   2015
#define ID_HOTKEY_DISABLE 2016
#define ID_THEME_COMBO   2017
#define ID_FOLLOWMOUSE_TGL 2018

static const wchar_t* kClass = L"FlashLaunch_MainClass";
static const wchar_t* kSettingsClass = L"FlashLaunch_SettingsClass";
static const wchar_t* kThemePopupClass = L"FlashLaunch_ThemePopupClass";
static const wchar_t* kMutex = L"FlashLaunch_SingleInstance_v1";

// ---------------------------------------------------------------- helpers
static std::wstring LowerW(const std::wstring& s) {
    std::wstring r(s);
    for (size_t i = 0; i < r.size(); ++i) r[i] = (wchar_t)towlower(r[i]);
    return r;
}

static std::wstring TrimW(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

static std::wstring ExpandEnvW(const std::wstring& s) {
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
    if (!n) return s;
    std::wstring out(n, L'\0');
    ExpandEnvironmentStringsW(s.c_str(), &out[0], n);
    out.resize(n ? n - 1 : 0);
    return out;
}

static std::wstring ExeDir() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}

static std::wstring FileNameNoExt(const std::wstring& p) {
    std::wstring n = p;
    wchar_t* slash = wcsrchr(&n[0], L'\\');
    if (slash) n = slash + 1;
    wchar_t* dot = wcsrchr(&n[0], L'.');
    if (dot && dot != &n[0]) n.resize(dot - &n[0]);
    return n;
}

static bool IsAsciiWord(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
}

// ---------------------------------------------------------------- theme palette
// 主题模式：0/1 兼容旧配置；2 跟随系统；3-5 为 Windows 系统视觉材料。
enum {
    THEME_DARK = 0,
    THEME_LIGHT = 1,
    THEME_SYSTEM = 2,
    THEME_MICA = 3,
    THEME_ACRYLIC = 4,
    THEME_GLASS = 5,
    THEME_COUNT = 6
};
enum { EFFECT_NONE = 0, EFFECT_MICA = 1, EFFECT_ACRYLIC = 2, EFFECT_GLASS = 3 };

static bool SystemUsesLightTheme() {
    DWORD light = 1, size = sizeof(light);
    LSTATUS st = RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return st == ERROR_SUCCESS ? light != 0 : true;
}

static int ThemeBase(int theme) {
    if (theme == THEME_LIGHT) return 1;
    if (theme == THEME_SYSTEM) return SystemUsesLightTheme() ? 1 : 0;
    return 0; // 深色 / 云母 / 亚克力 / 玻璃均以深色为底
}

static int ThemeEffect(int theme) {
    if (theme == THEME_MICA) return EFFECT_MICA;
    if (theme == THEME_ACRYLIC) return EFFECT_ACRYLIC;
    if (theme == THEME_GLASS) return EFFECT_GLASS;
    return EFFECT_NONE;
}

static const wchar_t* ThemeName(int theme) {
    switch (theme) {
    case THEME_LIGHT:   return L"浅色";
    case THEME_SYSTEM:  return L"跟随系统";
    case THEME_MICA:    return L"云母";
    case THEME_ACRYLIC: return L"亚克力";
    case THEME_GLASS:   return L"玻璃";
    default:            return L"深色";
    }
}

// 每套主题为每个语义字段给出颜色。字段清单只写一次（X-macro），
// 同时生成结构体、初始化函数和配置枚举，避免三处手抄不一致。
#define FLASH_PALETTE_FIELDS(X) \
    X(bg,            28, 30, 38,   245, 247, 250) \
    X(headTop,       41, 46, 58,   255, 255, 255) \
    X(headBand,      32, 36, 46,   240, 243, 247) \
    X(headBrush,     34, 37, 47,   242, 245, 249) \
    X(divider,       62, 68, 82,   220, 226, 234) \
    X(searchIcon,   126,176, 222,   60, 120, 190) \
    X(keycap,        50, 56, 68,   228, 233, 240) \
    X(keycapText,   154,164, 180,   90, 100, 115) \
    X(gear,         150,164, 184,  100, 112, 130) \
    X(emptyTitle,   140,150, 166,  110, 120, 135) \
    X(emptySub,     100,110, 126,  140, 150, 165) \
    X(selRow,        44, 58, 78,   215, 232, 248) \
    X(selBrush,      44, 58, 76,   212, 229, 247) \
    X(selBar,        88,184, 255,   45, 130, 215) \
    X(rowSep,        36, 40, 50,   232, 236, 242) \
    X(name,         232,236, 244,   35,  42,  55) \
    X(nameSel,      248,250, 255,   20,  30,  45) \
    X(badge,         42, 48, 60,   228, 233, 240) \
    X(badgeSel,      58, 78,102,   200, 220, 242) \
    X(badgeText,    118,142, 168,   90, 105, 125) \
    X(badgeTextSel, 150,205, 245,   40,  95, 160) \
    X(path,         126,134, 148,  120, 130, 145) \
    X(pathSel,      178,192, 212,   70,  85, 105) \
    X(pill,          38, 43, 54,   232, 236, 242) \
    X(pillSel,       52, 74, 96,   205, 225, 245) \
    X(pillText,     104,116, 132,  110, 122, 140) \
    X(pillTextSel,  130,210, 255,   40, 100, 170) \
    X(footerLine,    52, 58, 70,   222, 227, 234) \
    X(footerText,   132,141, 156,  115, 125, 140) \
    X(countText,     96,105, 120,  150, 158, 170) \
    X(editText,     230,235, 245,   30,  38,  52) \
    X(placeholder,  113,123, 141,  160, 168, 180) \
    X(setBg,         27, 30, 38,   248, 249, 251) \
    X(side,          25, 28, 36,   238, 241, 245) \
    X(card,          34, 38, 49,   255, 255, 255) \
    X(cardBorder,    52, 59, 73,   220, 225, 233) \
    X(setSep,        50, 56, 70,   224, 229, 236) \
    X(editBg,        23, 26, 33,   255, 255, 255) \
    X(editBorder,    59, 67, 82,   205, 213, 224) \
    X(accent,        58,134, 209,   45, 130, 215) \
    X(primaryPress,  42,118, 194,   35, 110, 190) \
    X(textBright,   220,227, 238,   30,  38,  52) \
    X(hint,         107,118, 136,  125, 135, 150) \
    X(ver,           95,106, 125,  140, 148, 162) \
    X(setTitle,     238,242, 250,   25,  32,  45) \
    X(label,        142,160, 187,   70,  85, 105) \
    X(text,         195,203, 218,   60,  70,  88) \
    X(textMuted,    135,146, 166,  110, 120, 138) \
    X(white,        255,255, 255,  255, 255, 255) \
    X(btnBg,         35, 40, 52,   235, 240, 246) \
    X(btnHover,      48, 55, 68,   222, 230, 240) \
    X(btnPress,      44, 50, 62,   205, 216, 230) \
    X(btnBorder,     74, 83, 99,   198, 208, 222) \
    X(btnFg,        205,213, 227,   55,  65,  82) \
    X(closeHot,     230,140, 140,  200,  70,  70) \
    X(closeFg,      139,149, 168,  110, 120, 138) \
    X(toggleOff,     58, 65, 80,   180, 190, 205) \
    X(toggleOffHot,  72, 79, 94,   165, 176, 192) \
    X(hotkeyCapture,130,190, 240,   40, 100, 175)

struct Palette {
#define X(name, r1, g1, b1, r2, g2, b2) COLORREF name;
    FLASH_PALETTE_FIELDS(X)
#undef X
};
static Palette g_pal;
static int g_theme = THEME_DARK;

static COLORREF BlendColor(COLORREF a, COLORREF b, float t);

static void ApplyPalette(int theme) {
    if (theme < 0 || theme >= THEME_COUNT) theme = THEME_DARK;
    g_theme = theme;
    int base = ThemeBase(theme);
#define X(name, r1, g1, b1, r2, g2, b2) \
    g_pal.name = base == 1 ? RGB(r2, g2, b2) : RGB(r1, g1, b1);
    FLASH_PALETTE_FIELDS(X)
#undef X
    // 玻璃主题用淡蓝冷色偏移和更亮的描边表达材质感，不再依赖整窗透明。
    if (theme == THEME_GLASS) {
        const COLORREF cool = RGB(72, 142, 205);
        g_pal.bg = BlendColor(g_pal.bg, cool, 0.10f);
        g_pal.card = BlendColor(g_pal.card, cool, 0.12f);
        g_pal.cardBorder = BlendColor(g_pal.cardBorder, cool, 0.32f);
        g_pal.headTop = BlendColor(g_pal.headTop, cool, 0.12f);
        g_pal.headBand = BlendColor(g_pal.headBand, cool, 0.10f);
        g_pal.editBorder = BlendColor(g_pal.editBorder, cool, 0.20f);
        g_pal.setBg = BlendColor(g_pal.setBg, cool, 0.08f);
        g_pal.side = BlendColor(g_pal.side, cool, 0.08f);
    }
}

// 依据当前调色板（重新）创建主窗口与设置窗口共用的画刷。
// g_bgBrush/g_headBrush/g_selBrush/g_setBgBrush/g_setEditBrush 定义在下方，
// 因此本函数实现放在这些变量声明之后。
static void RebuildThemeBrushes();

// ---------------------------------------------------------------- pinyin
static const char* PinyinOf(unsigned short cp) {
    size_t lo = 0, hi = sizeof(kCharPinyin) / sizeof(kCharPinyin[0]);
    while (lo < hi) {
        size_t mid = (lo + hi) >> 1;
        unsigned short c = kCharPinyin[mid].cp;
        if (c < cp) lo = mid + 1;
        else if (c > cp) hi = mid;
        else return kSyllables[kCharPinyin[mid].idx];
    }
    return nullptr;
}

// ---------------------------------------------------------------- data
struct Item {
    std::wstring name;
    std::wstring path;
    std::wstring args;
    std::wstring workdir;
    std::wstring source;
    std::wstring lower;
    std::wstring pathLower;
    std::wstring iconKey;
    std::string py;
    std::string initials;
    std::vector<BYTE> pidl;
    int iconIndex = 0;
    bool apps = false;
    HICON icon = nullptr;

    void BuildKeys() {
        lower = LowerW(name);
        pathLower = LowerW(path);
        py.clear();
        initials.clear();
        for (wchar_t c : name) {
            if (c >= 0x4E00 && c <= 0x9FFF) {
                const char* p = PinyinOf((unsigned short)c);
                if (p) { py += p; initials += p[0]; continue; }
            }
            if (c < 128 && IsAsciiWord(c)) {
                char a = (char)towlower(c);
                py += a;
                initials += a;
            }
        }
    }
};

struct Scored { int index; int score; };

static std::vector<Item> g_items;
static std::mutex g_itemsMu;
static std::vector<Scored> g_results;
static int g_sel = 0;
static int g_hover = -1;
static bool g_trackingMouse = false;
static int g_maxResults = 8;
static bool g_showPinyin = true;
static std::wstring g_query;
static std::wstring g_iniPath, g_historyPath, g_exePath;
static std::vector<std::wstring> g_recent;
static std::vector<std::wstring> g_extraPaths;
static HWND g_hwnd = nullptr, g_edit = nullptr;
static bool g_editNormalizing = false; // 防止清理换行时 EN_CHANGE 递归
static HFONT g_font = nullptr, g_fontSmall = nullptr, g_fontEdit = nullptr, g_fontBig = nullptr;
static HBRUSH g_bgBrush = nullptr, g_headBrush = nullptr, g_selBrush = nullptr;
static HDC g_memDC = nullptr;
static HBITMAP g_memBmp = nullptr;
static int g_w = 660, g_h = 520;
static int g_gearX = 614;
static ULONG_PTR g_gdiplusToken = 0;
static HICON g_appIcon = nullptr;
static NOTIFYICONDATAW g_nid = {};
static UINT g_hotkeyMod = MOD_ALT;
static int g_hotkeyVK = VK_SPACE;
static std::wstring g_hotkeyText = L"alt+space";
static bool g_hotkeyEnabled = true;
static bool g_autoStart = false;
static bool g_running = true;
static bool g_followMouse = true;   // 唤醒窗口跟随鼠标所在显示器
static int g_themePending = 0;      // 设置窗口中暂存的主题
static int g_themeOriginal = 0;     // 打开设置时的主题，用于取消回滚

// 计算器：当前表达式的计算结果（可求值时非空）
static std::wstring g_calcResult;
static std::wstring g_toast;          // 主窗口底部临时提示
static UINT_PTR g_toastTimer = 0;

// 0 = 尚未索引, 1 = 正在索引, 2 = 索引完成
static std::atomic<int> g_indexState{0};

// 设置窗口
static HWND g_settingsHwnd = nullptr;
static HWND g_noteStatic = nullptr;
static int g_settingsPage = 0;
static bool g_tglPinyin = true, g_tglAutoStart = false, g_tglFollowMouse = true;
static HFONT g_setFont = nullptr, g_setFontTitle = nullptr, g_setFontHint = nullptr;
static bool g_capturingHotkey = false;
static std::wstring g_hotkeyCaptureOriginal;
static bool g_hotkeyCaptureOriginalEnabled = true;
static HBRUSH g_setBgBrush = nullptr, g_setEditBrush = nullptr;

// 自绘主题下拉浮层
static HWND g_themePopup = nullptr;
static HDC g_themePopupDC = nullptr;
static HBITMAP g_themePopupBmp = nullptr;
static int g_themePopupW = 0, g_themePopupH = 0;
static int g_themePopupHover = -1;
static const int THEME_POPUP_SHADOW = 10;
static const int THEME_POPUP_PAD = 6;
static const int THEME_ITEM_H = 30;

static COLORREF BlendColor(COLORREF a, COLORREF b, float t);

static void RebuildThemeBrushes() {
    if (g_bgBrush) DeleteObject(g_bgBrush);
    if (g_headBrush) DeleteObject(g_headBrush);
    if (g_selBrush) DeleteObject(g_selBrush);
    if (g_setBgBrush) DeleteObject(g_setBgBrush);
    if (g_setEditBrush) DeleteObject(g_setEditBrush);
    g_bgBrush = CreateSolidBrush(g_pal.bg);
    // 输入框背景需与渐变头部中段颜色接近，避免出现方形色块
    g_headBrush = CreateSolidBrush(BlendColor(g_pal.headTop, g_pal.headBand, 0.50f));
    LOGBRUSH lb = {BS_SOLID, g_pal.selBrush, 0};
    g_selBrush = CreateBrushIndirect(&lb);
    g_setBgBrush = CreateSolidBrush(g_pal.setBg);
    g_setEditBrush = CreateSolidBrush(g_pal.editBg);
}

// 应用 Windows 系统视觉材料。Win11 用 DWM 系统背景（Mica/Acrylic），
// 旧系统用 DWM BlurBehind + 分层窗口透明度做玻璃降级；不支持的 API 直接忽略。
static void ApplyDwmAttributes(HWND h, int effect, BOOL dark) {
    if (!h || !IsWindow(h)) return;
    DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(h, (DWMWINDOWATTRIBUTE)19, &dark, sizeof(dark)); // Win10 1903 前

    BOOL hostBackdrop = (effect != EFFECT_NONE) ? TRUE : FALSE;
    DwmSetWindowAttribute(h, DWMWA_USE_HOSTBACKDROPBRUSH, &hostBackdrop, sizeof(hostBackdrop));

    int backdrop = DWMSBT_NONE;
    if (effect == EFFECT_MICA) backdrop = DWMSBT_MAINWINDOW;
    else if (effect == EFFECT_ACRYLIC || effect == EFFECT_GLASS) backdrop = DWMSBT_TRANSIENTWINDOW;
    DwmSetWindowAttribute(h, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));

    // 关闭 DWM 系统边框：WS_THICKFRAME 窗口失焦时系统会画一圈浅色边框（深色
    // 界面下即为白边）。DWMWA_COLOR_NONE 让 DWM 完全不画边框，圆角/阴影由
    // CORNER_PREFERENCE 提供，不受影响。Win10 19041 以下不支持会直接忽略。
    COLORREF borderNone = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(h, DWMWA_BORDER_COLOR, &borderNone, sizeof(borderNone));

    // 不再启用整窗 BlurBehind：它会让背景窗口/桌面文字直接透进来，
    // 在深色主题下尤其难读。Win11 的 Mica / Acrylic 交给系统背景属性处理。
    DWM_BLURBEHIND blur = {};
    blur.dwFlags = DWM_BB_ENABLE;
    blur.fEnable = FALSE;
    DwmEnableBlurBehindWindow(h, &blur);
}

// 窗口外框圆角：Win11 交给 DWM 做抗锯齿圆角，旧系统退回硬区域裁剪。
static void ApplyWindowCorners(HWND hwnd, int w, int hh, int radius) {
    (void)w; (void)hh; (void)radius;
    if (!hwnd || !IsWindow(hwnd)) return;
    // 不使用任何 Window Region：微软文档明确说明带 region 的窗口系统不会应用圆角，
    // 而 CreateRoundRectRgn 是像素级硬裁剪，必然产生阶梯状锯齿（此前锯齿的根因）。
    // 圆角完全交给 DWM：窗口带 WS_THICKFRAME + WM_NCCALCSIZE 隐藏可见边框后，
    // Win11 上会得到系统级抗锯齿圆角 + 阴影；Win10 上该 API 不存在，窗口为直角，
    // 但至少不再有锯齿。
    int pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
}

// 主窗口与设置窗口同步应用主题材料效果。
static void ApplyWindowEffect() {
    int effect = ThemeEffect(g_theme);
    BOOL dark = (ThemeBase(g_theme) == 0) ? TRUE : FALSE;

    // 自绘内容层保持不透明，保证文字和控件对比度；材质感由 DWM 背景和
    // 玻璃主题自身的淡蓝冷色 / 高亮描边表达。窗口不再使用分层透明度，
    // 这样 Win11 的 DWM 圆角才能正确应用到主窗和设置窗。
    ApplyDwmAttributes(g_hwnd, effect, dark);
    ApplyDwmAttributes(g_settingsHwnd, effect, dark);
}

// ---------------------------------------------------------------- config
static bool HotkeyKeyName(UINT vk, wchar_t* out, size_t n);

// 把解析出的组合键统一格式化为设置界面使用的规范文案：
// Ctrl + Shift + F12 / Alt + Space，避免 ini 里的 alt+space 与顶栏显示不一致。
static std::wstring FormatHotkeyText(UINT mod, int vk) {
    std::wstring s;
    if (mod & MOD_CONTROL) s += L"Ctrl + ";
    if (mod & MOD_ALT) s += L"Alt + ";
    if (mod & MOD_SHIFT) s += L"Shift + ";
    if (mod & MOD_WIN) s += L"Win + ";
    wchar_t key[32] = {0};
    if (!HotkeyKeyName((UINT)vk, key, 32)) return L"";
    s += key;
    return s;
}

static bool ParseHotkey(const std::wstring& text, UINT& mod, int& vk) {
    std::wstring hk = LowerW(text);
    mod = 0;
    if (hk.find(L"ctrl") != std::wstring::npos) mod |= MOD_CONTROL;
    if (hk.find(L"alt") != std::wstring::npos) mod |= MOD_ALT;
    if (hk.find(L"shift") != std::wstring::npos) mod |= MOD_SHIFT;
    if (hk.find(L"win") != std::wstring::npos) mod |= MOD_WIN;
    if (!mod) mod = MOD_ALT;

    std::wstring key = hk;
    size_t plus = key.rfind(L'+');
    if (plus != std::wstring::npos) key = key.substr(plus + 1);
    key = TrimW(key);
    bool ok = true;
    if (key.size() == 1 && ((key[0] >= L'a' && key[0] <= L'z') || (key[0] >= L'0' && key[0] <= L'9'))) vk = toupper(key[0]);
    else if (key == L"space") vk = VK_SPACE;
    else if (key == L"enter") vk = VK_RETURN;
    else if (key == L"tab") vk = VK_TAB;
    else if (key[0] == L'f' && key.size() >= 2 && key.size() <= 3) {
        int n = 0;
        bool digits = true;
        for (size_t i = 1; i < key.size(); ++i) {
            if (key[i] < L'0' || key[i] > L'9') { digits = false; break; }
            n = n * 10 + (key[i] - L'0');
        }
        if (digits && n >= 1 && n <= 12) vk = VK_F1 + (n - 1);
        else ok = false;
    }
    else ok = false;
    return ok;
}

static void LoadConfig() {
    WCHAR buf[512];
    g_exePath.resize(MAX_PATH);
    GetModuleFileNameW(nullptr, &g_exePath[0], MAX_PATH);
    g_exePath.resize(wcslen(g_exePath.c_str()));

    std::wstring dir = ExeDir();
    g_iniPath = dir + L"\\FlashLaunch.ini";
    g_historyPath = dir + L"\\FlashLaunch.history";

    GetPrivateProfileStringW(L"General", L"Hotkey", L"alt+space", buf, 512, g_iniPath.c_str());
    g_hotkeyText = TrimW(buf);
    std::wstring hkLower = LowerW(g_hotkeyText);
    g_hotkeyEnabled = !(hkLower.empty() || hkLower == L"disabled" || hkLower == L"none" || hkLower == L"off");
    if (!g_hotkeyEnabled) g_hotkeyText = L"disabled";
    ParseHotkey(g_hotkeyText, g_hotkeyMod, g_hotkeyVK);
    if (g_hotkeyEnabled) {
        std::wstring canonical = FormatHotkeyText(g_hotkeyMod, g_hotkeyVK);
        if (!canonical.empty()) g_hotkeyText = canonical;
    }

    g_maxResults = GetPrivateProfileIntW(L"General", L"MaxResults", 8, g_iniPath.c_str());
    if (g_maxResults < 3) g_maxResults = 3;
    if (g_maxResults > 16) g_maxResults = 16;
    g_showPinyin = GetPrivateProfileIntW(L"General", L"ShowPinyin", 1, g_iniPath.c_str()) != 0;
    g_autoStart = GetPrivateProfileIntW(L"General", L"AutoStart", 0, g_iniPath.c_str()) != 0;
    g_theme = GetPrivateProfileIntW(L"General", L"Theme", 0, g_iniPath.c_str());
    if (g_theme < 0 || g_theme >= THEME_COUNT) g_theme = THEME_DARK;
    g_followMouse = GetPrivateProfileIntW(L"General", L"FollowMouse", 1, g_iniPath.c_str()) != 0;

    GetPrivateProfileStringW(L"General", L"IndexPath", L"", buf, 512, g_iniPath.c_str());
    std::wstring paths = buf;
    size_t pos = 0;
    while (pos < paths.size()) {
        size_t e = paths.find(L';', pos);
        if (e == std::wstring::npos) e = paths.size();
        std::wstring p = TrimW(paths.substr(pos, e - pos));
        if (!p.empty()) g_extraPaths.push_back(ExpandEnvW(p));
        pos = e + 1;
    }

    if (!PathFileExistsW(g_iniPath.c_str())) {
        WritePrivateProfileStringW(L"General", L"Hotkey", L"alt+space", g_iniPath.c_str());
        WritePrivateProfileStringW(L"General", L"MaxResults", L"8", g_iniPath.c_str());
        WritePrivateProfileStringW(L"General", L"ShowPinyin", L"1", g_iniPath.c_str());
        WritePrivateProfileStringW(L"General", L"AutoStart", L"0", g_iniPath.c_str());
        WritePrivateProfileStringW(L"General", L"Theme", L"0", g_iniPath.c_str());
        WritePrivateProfileStringW(L"General", L"FollowMouse", L"1", g_iniPath.c_str());
        WritePrivateProfileStringW(L"General", L"IndexPath", L"", g_iniPath.c_str());
    }

    FILE* f = _wfopen(g_historyPath.c_str(), L"rb");
    if (f) {
        WCHAR line[MAX_PATH];
        while (fgetws(line, MAX_PATH, f)) {
            std::wstring s = TrimW(line);
            if (!s.empty()) g_recent.push_back(s);
        }
        fclose(f);
    }
}

static void SaveConfig() {
    WCHAR buf[32];
    swprintf(buf, 32, L"%d", g_maxResults);
    WritePrivateProfileStringW(L"General", L"Hotkey",
                               g_hotkeyEnabled ? g_hotkeyText.c_str() : L"disabled", g_iniPath.c_str());
    WritePrivateProfileStringW(L"General", L"MaxResults", buf, g_iniPath.c_str());
    WritePrivateProfileStringW(L"General", L"ShowPinyin", g_showPinyin ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"General", L"AutoStart", g_autoStart ? L"1" : L"0", g_iniPath.c_str());
    swprintf(buf, 32, L"%d", g_theme);
    WritePrivateProfileStringW(L"General", L"Theme", buf, g_iniPath.c_str());
    WritePrivateProfileStringW(L"General", L"FollowMouse",
                               g_followMouse ? L"1" : L"0", g_iniPath.c_str());
    std::wstring paths;
    for (size_t i = 0; i < g_extraPaths.size(); ++i) {
        if (i) paths += L";";
        paths += g_extraPaths[i];
    }
    WritePrivateProfileStringW(L"General", L"IndexPath", paths.c_str(), g_iniPath.c_str());
}

static void SaveHistory() {
    FILE* f = _wfopen(g_historyPath.c_str(), L"wb");
    if (!f) return;
    for (const auto& s : g_recent) {
        if (!s.empty()) fwprintf(f, L"%s\n", s.c_str());
    }
    fclose(f);
}

static void SetAutoStart(bool on) {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
        if (on) {
            std::wstring cmd = L"\"" + g_exePath + L"\"";
            RegSetValueExW(hk, L"FlashLaunch", 0, REG_SZ, (const BYTE*)cmd.c_str(),
                           (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hk, L"FlashLaunch");
        }
        RegCloseKey(hk);
    }
    g_autoStart = on;
    WritePrivateProfileStringW(L"General", L"AutoStart", on ? L"1" : L"0", g_iniPath.c_str());
}

// ---------------------------------------------------------------- indexing
static void AddItem(std::vector<Item>& v, std::unordered_set<std::wstring>& seen,
                    const std::wstring& name, const std::wstring& path,
                    const std::wstring& args, const std::wstring& workdir,
                    const std::wstring& source) {
    if (name.empty() || path.empty()) return;
    std::wstring p = LowerW(path);
    std::wstring key = p + L"|" + LowerW(args);
    if (seen.count(key)) return;
    seen.insert(key);
    Item it;
    it.name = FileNameNoExt(name);
    if (it.name.empty()) it.name = name;
    it.path = path;
    it.args = args;
    it.workdir = workdir;
    it.source = source;
    it.BuildKeys();
    v.push_back(std::move(it));
}

static bool ResolveLnk(const std::wstring& file, std::wstring& target,
                       std::wstring& args, std::wstring& dir) {
    IShellLinkW* sl = nullptr;
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&sl))) return false;
    if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        if (SUCCEEDED(pf->Load(file.c_str(), STGM_READ))) {
            WCHAR buf[2048] = {0};
            WIN32_FIND_DATAW fd = {};
            if (SUCCEEDED(sl->GetPath(buf, 2048, &fd, SLGP_RAWPATH)) && buf[0]) {
                target = ExpandEnvW(buf);
                if (PathIsRelativeW(target.c_str())) {
                    std::wstring base = file.substr(0, file.rfind(L'\\') + 1);
                    target = base + target;
                }
                WCHAR a[1024] = {0}, d[1024] = {0};
                sl->GetArguments(a, 1024);
                sl->GetWorkingDirectory(d, 1024);
                args = a;
                dir = d;
                ok = !target.empty();
            }
        }
        pf->Release();
    }
    sl->Release();
    return ok;
}

static void ScanDir(const std::wstring& root, int depth, const std::wstring& source,
                    std::vector<Item>& v, std::unordered_set<std::wstring>& seen,
                    int& counter) {
    if (root.empty() || counter > 6000) return;
    std::wstring pattern = root;
    if (pattern.empty() || pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (counter > 6000) break;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth <= 0) continue;
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L".." || n[0] == L'.') continue;
            if (n == L"Startup") continue;
            ScanDir(root + L"\\" + n, depth - 1, source, v, seen, counter);
            continue;
        }
        std::wstring name = fd.cFileName;
        std::wstring full = root + L"\\" + name;
        const wchar_t* ext = PathFindExtensionW(name.c_str());
        if (!_wcsicmp(ext, L".lnk")) {
            std::wstring t, a, d;
            if (ResolveLnk(full, t, a, d)) {
                AddItem(v, seen, name, t, a, d, source);
                ++counter;
            }
        } else if (!_wcsicmp(ext, L".exe")) {
            AddItem(v, seen, name, full, L"", L"", source);
            ++counter;
        } else if (!_wcsicmp(ext, L".url")) {
            FILE* f = _wfopen(full.c_str(), L"rb");
            if (f) {
                WCHAR line[1024];
                while (fgetws(line, 1024, f)) {
                    if (!wcsncmp(line, L"URL=", 4)) {
                        std::wstring url = TrimW(line + 4);
                        if (!url.empty()) { AddItem(v, seen, name, url, L"", L"", source); ++counter; }
                        break;
                    }
                }
                fclose(f);
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) {
        std::wstring s = p;
        CoTaskMemFree(p);
        return s;
    }
    return L"";
}

static void ScanAppPaths(std::vector<Item>& v, std::unordered_set<std::wstring>& seen, int& counter) {
    const wchar_t* keys[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\App Paths"
    };
    for (auto root : keys) {
        HKEY hk = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ, &hk) != ERROR_SUCCESS) continue;
        WCHAR name[512];
        for (DWORD i = 0; RegEnumKeyW(hk, i, name, 512) == ERROR_SUCCESS; ++i) {
            HKEY sub = nullptr;
            if (RegOpenKeyExW(hk, name, 0, KEY_QUERY_VALUE, &sub) == ERROR_SUCCESS) {
                WCHAR val[2048];
                DWORD len = sizeof(val);
                if (RegQueryValueExW(sub, nullptr, nullptr, nullptr, (BYTE*)val, &len) == ERROR_SUCCESS) {
                    len /= sizeof(wchar_t);
                    val[len] = 0;
                    std::wstring p = ExpandEnvW(val);
                    if (!p.empty() && PathFileExistsW(p.c_str())) {
                        AddItem(v, seen, name, p, L"", L"", L"注册表");
                        ++counter;
                    }
                }
                RegCloseKey(sub);
            }
        }
        RegCloseKey(hk);
    }
}

static void ScanPathExe(std::vector<Item>& v, std::unordered_set<std::wstring>& seen, int& counter) {
    WCHAR env[32768];
    DWORD n = GetEnvironmentVariableW(L"PATH", env, 32768);
    if (!n) return;
    std::wstring path = env;
    size_t pos = 0;
    while (pos < path.size() && counter < 5000) {
        size_t e = path.find(L';', pos);
        if (e == std::wstring::npos) e = path.size();
        std::wstring dir = ExpandEnvW(path.substr(pos, e - pos));
        if (!dir.empty()) ScanDir(dir, 0, L"PATH", v, seen, counter);
        pos = e + 1;
    }
}

static void EnumAppsFolder(std::vector<Item>& v, std::unordered_set<std::wstring>& seen, int& counter) {
    IShellFolder* desktop = nullptr;
    if (FAILED(SHGetDesktopFolder(&desktop))) return;
    PIDLIST_ABSOLUTE rootPidl = nullptr;
    if (FAILED(SHGetKnownFolderIDList(FOLDERID_AppsFolder, 0, nullptr, &rootPidl))) {
        desktop->Release();
        return;
    }
    IShellFolder* apps = nullptr;
    if (FAILED(desktop->BindToObject(rootPidl, nullptr, IID_IShellFolder, (void**)&apps))) {
        ILFree(rootPidl);
        desktop->Release();
        return;
    }
    IEnumIDList* en = nullptr;
    if (SUCCEEDED(apps->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &en))) {
        PITEMID_CHILD child = nullptr;
        while (en->Next(1, &child, nullptr) == S_OK && counter < 3000) {
            STRRET sr = {}, sr2 = {};
            WCHAR disp[512] = {0}, parse[1024] = {0};
            if (SUCCEEDED(apps->GetDisplayNameOf(child, SHGDN_NORMAL, &sr)))
                StrRetToBufW(&sr, child, disp, 512);
            if (SUCCEEDED(apps->GetDisplayNameOf(child, SHGDN_FORPARSING, &sr2)))
                StrRetToBufW(&sr2, child, parse, 1024);
            if (disp[0] && parse[0]) {
                std::wstring p = parse;
                size_t slash = p.rfind(L'\\');
                if (slash != std::wstring::npos) p = p.substr(slash + 1);
                std::wstring full = L"shell:AppsFolder\\" + p;
                std::wstring key = LowerW(full);
                if (!seen.count(key)) {
                    seen.insert(key);
                    Item it;
                    it.name = disp;
                    it.path = full;
                    it.source = L"应用";
                    it.apps = true;
                    PITEMID_CHILD abs = ILCombine(rootPidl, child);
                    if (abs) {
                        int sz = ILGetSize(abs);
                        it.pidl.assign((BYTE*)abs, (BYTE*)abs + sz);
                        ILFree(abs);
                    }
                    it.BuildKeys();
                    v.push_back(std::move(it));
                    ++counter;
                }
            }
            CoTaskMemFree(child);
            child = nullptr;
        }
        en->Release();
    }
    apps->Release();
    ILFree(rootPidl);
    desktop->Release();
}

static DWORD WINAPI IndexThread(LPVOID) {
    g_indexState.store(1);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::vector<Item> v;
    std::unordered_set<std::wstring> seen;
    int counter = 0;
    std::wstring sm = KnownFolder(FOLDERID_Programs);
    std::wstring desktop = KnownFolder(FOLDERID_Desktop);
    std::wstring commonSm = KnownFolder(FOLDERID_CommonPrograms);
    std::wstring commonDesktop = KnownFolder(FOLDERID_PublicDesktop);
    ScanDir(sm, 3, L"开始菜单", v, seen, counter);
    ScanDir(commonSm, 3, L"开始菜单", v, seen, counter);
    ScanDir(desktop, 1, L"桌面", v, seen, counter);
    ScanDir(commonDesktop, 1, L"桌面", v, seen, counter);
    for (const auto& p : g_extraPaths) ScanDir(p, 3, L"自定义", v, seen, counter);
    ScanAppPaths(v, seen, counter);
    ScanPathExe(v, seen, counter);
    EnumAppsFolder(v, seen, counter);
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_itemsMu);
        g_items.swap(v);
        g_results.clear();
        g_sel = 0;
        count = g_items.size();
    }
    g_indexState.store(2);
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_INDEXED, (WPARAM)count, 0);
    CoUninitialize();
    return 0;
}

static void StartIndex() {
    if (g_indexState.exchange(1) == 1) return;
    HANDLE h = CreateThread(nullptr, 0, IndexThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else g_indexState.store(0);
}

// ---------------------------------------------------------------- icons
static std::unordered_map<std::wstring, HICON> g_iconCache;
static std::vector<std::wstring> g_iconOrder;

static HICON GetItemIcon(const Item& it) {
    std::wstring key = it.pathLower;
    auto found = g_iconCache.find(key);
    if (found != g_iconCache.end()) return found->second;

    SHFILEINFOW sfi = {};
    HICON icon = nullptr;
    if (it.apps && !it.pidl.empty()) {
        SHGetFileInfoW((LPCWSTR)it.pidl.data(), 0, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_SMALLICON | SHGFI_PIDL);
        icon = sfi.hIcon;
    } else {
        DWORD flags = SHGFI_ICON | SHGFI_SMALLICON;
        if (!PathFileExistsW(it.path.c_str())) flags |= SHGFI_USEFILEATTRIBUTES;
        SHGetFileInfoW(it.path.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), flags);
        icon = sfi.hIcon;
    }
    if (!icon) return nullptr;
    if (g_iconOrder.size() >= 400) {
        std::wstring old = g_iconOrder.front();
        g_iconOrder.erase(g_iconOrder.begin());
        auto o = g_iconCache.find(old);
        if (o != g_iconCache.end()) { DestroyIcon(o->second); g_iconCache.erase(o); }
    }
    g_iconCache[key] = icon;
    g_iconOrder.push_back(key);
    return icon;
}

// ---------------------------------------------------------------- calculator
// 递归下降解析四则运算，支持 + - * / % ^ 括号、小数与前置负号；
// 不使用 eval，纯栈式计算，输入非法时返回 false。
struct CalcParser {
    const std::wstring& s;
    size_t i = 0;
    bool ok = true;
    explicit CalcParser(const std::wstring& text) : s(text) {}

    void skip() { while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) ++i; }
    bool eat(wchar_t c) { skip(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }

    double parseExpr() {
        double v = parseTerm();
        for (;;) {
            skip();
            if (i >= s.size()) break;
            wchar_t op = s[i];
            if (op != L'+' && op != L'-') break;
            ++i;
            double r = parseTerm();
            v = (op == L'+') ? v + r : v - r;
        }
        return v;
    }
    double parseTerm() {
        double v = parseFactor();
        for (;;) {
            skip();
            if (i >= s.size()) break;
            wchar_t op = s[i];
            if (op != L'*' && op != L'/' && op != L'%') break;
            ++i;
            double r = parseFactor();
            if (op == L'*') v *= r;
            else if (op == L'/') { if (r == 0) { ok = false; return 0; } v /= r; }
            else { if ((int)r == 0) { ok = false; return 0; } v = (int)v % (int)r; }
        }
        return v;
    }
    double parseFactor() {
        // 幂运算（右结合）
        double base = parseUnary();
        skip();
        if (i < s.size() && s[i] == L'^') {
            ++i;
            double exp = parseFactor();
            return pow(base, exp);
        }
        return base;
    }
    double parseUnary() {
        skip();
        if (i < s.size() && s[i] == L'+') { ++i; return parseUnary(); }
        if (i < s.size() && s[i] == L'-') { ++i; return -parseUnary(); }
        if (eat(L'(')) {
            double v = parseExpr();
            if (!eat(L')')) ok = false;
            return v;
        }
        return parseNumber();
    }
    double parseNumber() {
        skip();
        size_t start = i;
        bool digit = false;
        while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') { ++i; digit = true; }
        if (i < s.size() && s[i] == L'.') {
            ++i;
            while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') { ++i; digit = true; }
        }
        if (!digit) { ok = false; return 0; }
        return _wtof(s.substr(start, i - start).c_str());
    }
};

// 尝试把查询解析为算术表达式。要求至少含一个运算符，纯数字不视为计算。
static bool EvalExpression(const std::wstring& input, std::wstring& out) {
    std::wstring q = TrimW(input);
    if (q.empty() || q.size() > 120) return false;
    bool hasDigit = false, hasOp = false;
    for (wchar_t c : q) {
        if (c >= L'0' && c <= L'9') hasDigit = true;
        else if (c == L'+' || c == L'-' || c == L'*' || c == L'/' ||
                 c == L'%' || c == L'^' || c == L'(' || c == L')' || c == L'.') hasOp = true;
        else return false; // 含字母/中文等，交给普通搜索
    }
    if (!hasDigit || !hasOp) return false;

    CalcParser p(q);
    double v = p.parseExpr();
    p.skip();
    if (!p.ok || p.i != q.size()) return false;
    if (!std::isfinite(v)) return false;

    wchar_t buf[64];
    // 结果较整时输出整数，否则保留最多 6 位小数并去尾零
    double rounded = round(v * 1e6) / 1e6;
    if (fabs(rounded - (double)(long long)rounded) < 1e-9)
        swprintf(buf, 64, L"%lld", (long long)rounded);
    else
        swprintf(buf, 64, L"%g", rounded);
    out = buf;
    return true;
}

// ---------------------------------------------------------------- search
static int WideMatch(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return 0;
    size_t p = hay.find(needle);
    if (p == std::wstring::npos) return 0;
    if (p == 0) return 1000;
    wchar_t prev = hay[p - 1];
    if (iswspace(prev) || prev == L'_' || prev == L'-' || prev == L'.' || prev == L'\\') return 850;
    return 700 - (int)p * 3;
}

static int AsciiMatch(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    size_t p = hay.find(needle);
    if (p == std::string::npos) return 0;
    if (p == 0) return 960;
    char prev = hay[p - 1];
    if (prev == ' ' || prev == '_' || prev == '-' || prev == '.') return 820;
    return 660 - (int)p * 3;
}

static int Subseq(const std::string& hay, const std::string& needle) {
    size_t j = 0, gaps = 0, last = 0;
    for (size_t i = 0; i < hay.size() && j < needle.size(); ++i) {
        if (hay[i] == needle[j]) { gaps += i - last; last = i + 1; ++j; }
    }
    if (j == needle.size()) return 320 - (int)gaps * 2;
    return 0;
}

static int RecencyBoost(const Item& it) {
    for (size_t i = 0; i < g_recent.size(); ++i)
        if (g_recent[i] == it.path) return (int)(g_recent.size() - i) * 8;
    return 0;
}

static int TokenScore(const Item& it, const std::wstring& tok) {
    std::wstring wtok = LowerW(tok);
    std::string atok;
    bool ascii = true;
    for (wchar_t c : tok) {
        if (c >= 128 || !IsAsciiWord(c)) { ascii = false; break; }
        atok += (char)towlower(c);
    }
    int best = 0;
    if (ascii && !atok.empty()) {
        best = std::max(best, AsciiMatch(it.py, atok));
        best = std::max(best, AsciiMatch(it.initials, atok) + 40);
        best = std::max(best, Subseq(it.py, atok));
        best = std::max(best, Subseq(it.initials, atok) + 30);
    }
    best = std::max(best, WideMatch(it.lower, wtok));
    best = std::max(best, WideMatch(it.pathLower, wtok) / 3);
    return best;
}

static void UpdateResults() {
    std::wstring q = TrimW(g_query);
    g_calcResult.clear();
    if (q.empty()) {
        g_results.clear();
        g_sel = 0;
        return;
    }

    // 表达式优先：结果作为置顶特殊项（index = -1），等号在绘制时补上
    std::wstring calc;
    if (EvalExpression(q, calc)) {
        g_calcResult = calc;
        g_results.clear();
        g_results.push_back({-1, 1000000});
        g_sel = 0;
        return;
    }

    if (g_indexState.load() != 2) {
        g_results.clear();
        g_sel = 0;
        return;
    }
    std::vector<Scored> out;
    std::lock_guard<std::mutex> lock(g_itemsMu);

    std::vector<std::wstring> tokens;
    size_t pos = 0;
    while (pos < q.size()) {
        size_t e = q.find(L' ', pos);
        if (e == std::wstring::npos) e = q.size();
        std::wstring t = q.substr(pos, e - pos);
        if (!t.empty()) tokens.push_back(t);
        pos = e + 1;
    }

    for (size_t i = 0; i < g_items.size(); ++i) {
        const Item& it = g_items[i];
        int score = 0;
        if (tokens.empty()) {
            score = 200 + RecencyBoost(it);
        } else {
            bool all = true;
            for (const auto& t : tokens) {
                int s = TokenScore(it, t);
                if (s <= 0) { all = false; break; }
                score += s;
            }
            if (!all) score = 0;
            else score += RecencyBoost(it);
        }
        if (score > 0) out.push_back({(int)i, score});
    }

    std::sort(out.begin(), out.end(), [](const Scored& a, const Scored& b) {
        if (a.score != b.score) return a.score > b.score;
        const Item& ia = g_items[a.index];
        const Item& ib = g_items[b.index];
        if (ia.lower.size() != ib.lower.size()) return ia.lower.size() < ib.lower.size();
        return ia.lower < ib.lower;
    });
    if ((int)out.size() > g_maxResults) out.resize(g_maxResults);
    g_results.swap(out);
    if (g_sel >= (int)g_results.size()) g_sel = 0;
}

static void AddRecent(const std::wstring& path) {
    std::vector<std::wstring> v;
    v.push_back(path);
    for (const auto& p : g_recent) if (p != path) v.push_back(p);
    if (v.size() > 30) v.resize(30);
    g_recent.swap(v);
    SaveHistory();
}

// ---------------------------------------------------------------- painting
static void MakeFonts() {
    g_font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_fontSmall = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                               OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_fontEdit = CreateFontW(-20, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_fontBig = CreateFontW(-19, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

// 双缓冲位图改用 32bpp DIB，并把 alpha 通道显式初始化为不透明(255)。
// CreateCompatibleBitmap 的 alpha 通道常常无效(0)，GDI+ 在画半透明圆角/描边时
// 会以目标 alpha=0 参与混合，导致边缘出现黑边和锯齿。有效 alpha 才能得到平滑抗锯齿。
static void ResizeBuffer(int w, int h) {
    if (g_memBmp) { DeleteObject(g_memBmp); g_memBmp = nullptr; }
    if (g_memDC) { DeleteDC(g_memDC); g_memDC = nullptr; }
    HDC dc = GetDC(g_hwnd);
    g_memDC = CreateCompatibleDC(dc);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    g_memBmp = CreateDIBSection(g_memDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (g_memBmp && bits) {
        // DIB 自底向上；0xFF000000 = BGRA 中 alpha=255。
        for (long i = 0; i < (long)w * h; ++i)
            ((DWORD*)bits)[i] = 0xFF000000;
        SelectObject(g_memDC, g_memBmp);
    }
    ReleaseDC(g_hwnd, dc);
}

// 单行 Edit 不支持 EM_SETRECT（微软文档：仅 ES_MULTILINE 有效），所以给
// 输入框用的都是 ES_MULTILINE | ES_AUTOHSCROLL（外观行为等同单行），再用
// EM_SETRECT 把文本矩形精确设成“一行高 + 上下留白对称”，实现真正的垂直居中。
// 这样系统的光标、选区、中文 IME 组合输入都照常工作，只是文本位置被精确控制。
static void CenterEditText(HWND hEdit, HFONT font) {
    if (!hEdit || !font) return;
    HDC dc = GetDC(hEdit);
    HFONT oldFont = (HFONT)SelectObject(dc, font);
    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, oldFont);
    ReleaseDC(hEdit, dc);

    RECT rc;
    GetClientRect(hEdit, &rc);
    int clientH = rc.bottom - rc.top;
    // 行高用字体实际高度 + 2px 余量，避免下行字形被裁切。
    int lineH = std::min<int>((int)tm.tmHeight + 2, clientH);
    int top = std::max(0, (clientH - lineH) / 2);
    RECT tr = { 0, top, rc.right, top + lineH };
    SendMessageW(hEdit, EM_SETRECT, 0, (LPARAM)&tr);
}

// 顶栏布局：右上角不再放快捷键键帽，设置齿轮靠右固定，
// 搜索框延伸到齿轮左侧，保持整行视觉重心一致。
static void UpdateHeaderLayout() {
    if (!g_hwnd || !g_edit) return;
    g_gearX = g_w - 46;
    int editRight = g_gearX - 10;
    int editW = std::max(120, editRight - 52);
    SetWindowPos(g_edit, nullptr, 52, 20, editW, 28, SWP_NOZORDER | SWP_NOACTIVATE);
    CenterEditText(g_edit, g_fontEdit);
    InvalidateRect(g_hwnd, nullptr, TRUE);
}

// 用 GDI+ 抗锯齿绘制搜索图标；圆心固定在头部中线 y=34。
static void DrawSearchIcon(HDC dc) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    Gdiplus::Color color(GetRValue(g_pal.searchIcon), GetGValue(g_pal.searchIcon), GetBValue(g_pal.searchIcon));
    Gdiplus::Pen pen(color, 1.6f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    // 外圆 20,26.5 - 35,41.5 => 圆心 (27.5, 34)，与输入框文字中线一致。
    g.DrawEllipse(&pen, 20.0f, 26.5f, 15.0f, 15.0f);
    g.DrawLine(&pen, 32.5f, 39.0f, 42.0f, 48.0f);
}

// ---------------------------------------------------------------- fluent helpers
static Gdiplus::Color GdipColor(COLORREF c, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

static COLORREF BlendColor(COLORREF a, COLORREF b, float t) {
    int r = (int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t);
    int g = (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t);
    int bl = (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t);
    r = std::max(0, std::min(255, r));
    g = std::max(0, std::min(255, g));
    bl = std::max(0, std::min(255, bl));
    return RGB(r, g, bl);
}

static void FillGradientRect(HDC dc, RECT r, COLORREF top, COLORREF bottom) {
    Gdiplus::Graphics g(dc);
    Gdiplus::LinearGradientBrush brush(
        Gdiplus::Point(r.left, r.top), Gdiplus::Point(r.left, r.bottom),
        GdipColor(top), GdipColor(bottom));
    g.FillRectangle(&brush, (int)r.left, (int)r.top,
                    (int)(r.right - r.left), (int)(r.bottom - r.top));
}

static void AddRoundRectPath(Gdiplus::GraphicsPath& path, int l, int t, int r, int b, int radius) {
    int d = radius * 2;
    d = std::min(d, r - l);
    d = std::min(d, b - t);
    path.AddArc((Gdiplus::REAL)l, (Gdiplus::REAL)t, (Gdiplus::REAL)d, (Gdiplus::REAL)d, 180.0f, 90.0f);
    path.AddArc((Gdiplus::REAL)(r - d), (Gdiplus::REAL)t, (Gdiplus::REAL)d, (Gdiplus::REAL)d, 270.0f, 90.0f);
    path.AddArc((Gdiplus::REAL)(r - d), (Gdiplus::REAL)(b - d), (Gdiplus::REAL)d, (Gdiplus::REAL)d, 0.0f, 90.0f);
    path.AddArc((Gdiplus::REAL)l, (Gdiplus::REAL)(b - d), (Gdiplus::REAL)d, (Gdiplus::REAL)d, 90.0f, 90.0f);
    path.CloseFigure();
}

static void FillRoundGradient(HDC dc, RECT r, int radius,
                              COLORREF top, COLORREF bottom,
                              COLORREF border, BYTE borderAlpha = 255) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    Gdiplus::GraphicsPath path;
    AddRoundRectPath(path, (int)r.left, (int)r.top, (int)r.right, (int)r.bottom, radius);
    Gdiplus::LinearGradientBrush brush(
        Gdiplus::Point(r.left, r.top), Gdiplus::Point(r.left, r.bottom),
        GdipColor(top), GdipColor(bottom));
    g.FillPath(&brush, &path);
    if (borderAlpha) {
        Gdiplus::Pen pen(GdipColor(border, borderAlpha), 1.0f);
        g.DrawPath(&pen, &path);
    }
}

// 纯色圆角填充：内部卡片、徽章、图标底等都走 GDI+ 抗锯齿路径。
static void FillRoundSolid(HDC dc, RECT r, int radius, COLORREF color) {
    FillRoundGradient(dc, r, radius, color, color, color, 0);
}

// 只画圆角描边（输入框边框等），不覆盖已有背景。
static void DrawRoundBorder(HDC dc, RECT r, int radius, COLORREF color,
                            BYTE alpha = 255, float width = 1.0f) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    Gdiplus::GraphicsPath path;
    AddRoundRectPath(path, r.left, r.top, r.right, r.bottom, radius);
    Gdiplus::Pen pen(GdipColor(color, alpha), width);
    g.DrawPath(&pen, &path);
}

static void DrawUI(HDC dc) {
    std::lock_guard<std::mutex> lock(g_itemsMu);
    SetBkMode(dc, TRANSPARENT);

    RECT rc = {0, 0, g_w, g_h};
    // 细腻的纵向渐变背景，比纯色更有层次（2026 扁平+微质感风格）。
    FillGradientRect(dc, rc,
        BlendColor(g_pal.bg, g_pal.white, 0.045f),
        BlendColor(g_pal.bg, g_pal.headBand, 0.20f));

    // Header: 平滑渐变替代原来的双色硬分界
    RECT headerRect = {0, 0, g_w, 68};
    FillGradientRect(dc, headerRect, g_pal.headTop, g_pal.headBand);

    HPEN oldPen = nullptr;
    HBRUSH oldBrush = nullptr;
    HFONT oldFont = nullptr;

    // Divider
    HPEN divider = CreatePen(PS_SOLID, 1, g_pal.divider);
    oldPen = (HPEN)SelectObject(dc, divider);
    MoveToEx(dc, 0, 68, nullptr);
    LineTo(dc, g_w, 68);
    SelectObject(dc, oldPen);
    DeleteObject(divider);

    // Search glyph：GDI+ 抗锯齿，圆心与输入框文字中线均对齐头部中线 y=34
    DrawSearchIcon(dc);

    // Settings gear (clickable)：与搜索图标共用头部中线
    int gx = g_gearX, gy = 20;
    HPEN gearPen = CreatePen(PS_SOLID, 2, g_pal.gear);
    oldPen = (HPEN)SelectObject(dc, gearPen);
    oldBrush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, gx + 5, gy + 5, gx + 23, gy + 23);
    Ellipse(dc, gx + 10, gy + 10, gx + 18, gy + 18);
    for (int a = 0; a < 4; ++a) {
        int ox = (a == 0 || a == 2) ? 14 : ((a == 1) ? 23 : 5);
        int oy = (a == 1 || a == 3) ? 14 : ((a == 0) ? 5 : 23);
        MoveToEx(dc, gx + 14, gy + 14, nullptr);
        LineTo(dc, gx + ox, gy + oy);
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(gearPen);

    int top = 80, rowH = 50;
    int shown = std::min((int)g_results.size(), (g_h - top - 42) / rowH);
    if (shown < 0) shown = 0;

    if (shown == 0) {
        bool querying = !TrimW(g_query).empty();
        bool indexing = g_indexState.load() == 1;
        const wchar_t* title = L"输入关键词开始搜索";
        const wchar_t* sub = L"支持中文、全拼或首字母，例如  微信 / weixin / wx";
        if (indexing && !querying) { title = L"正在建立索引…"; sub = L"首次启动会扫描程序列表，稍候即可搜索"; }
        else if (indexing && querying) { title = L"正在建立索引…"; sub = L"索引完成后会自动显示搜索结果"; }
        else if (querying) { title = L"没有匹配的结果"; sub = L"试试程序名、全拼或首字母"; }

        oldFont = (HFONT)SelectObject(dc, g_font);
        SetTextColor(dc, g_pal.emptyTitle);
        RECT r = {0, top + 48, g_w, top + 78};
        DrawTextW(dc, title, -1, &r, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, g_fontSmall);
        SetTextColor(dc, g_pal.emptySub);
        RECT r2 = {0, top + 80, g_w, top + 104};
        DrawTextW(dc, sub, -1, &r2, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, oldFont);
    }

    for (int i = 0; i < shown; ++i) {
        int y = top + i * rowH;
        bool sel = (i == g_sel);

        // 鼠标悬停的轻暗示：比选中态弱，避免视觉噪音
        if (!sel && i == g_hover) {
            RECT row = {8, y, g_w - 8, y + rowH - 4};
            COLORREF hoverTop = BlendColor(g_pal.bg, g_pal.selRow, 0.42f);
            COLORREF hoverBottom = BlendColor(g_pal.bg, g_pal.selRow, 0.26f);
            FillRoundGradient(dc, row, 10, hoverTop, hoverBottom, g_pal.cardBorder, 70);
        }

        // 计算结果特殊项
        if (g_results[i].index < 0) {
            if (sel) {
                RECT row = {8, y, g_w - 8, y + rowH - 4};
                FillRoundGradient(dc, row, 10,
                    BlendColor(g_pal.selRow, g_pal.accent, 0.10f),
                    BlendColor(g_pal.selRow, g_pal.accent, 0.22f),
                    BlendColor(g_pal.selBar, g_pal.white, 0.35f), 90);
                RECT bar = {8, y + 8, 11, y + rowH - 12};
                HBRUSH ac = CreateSolidBrush(g_pal.selBar);
                FillRect(dc, &bar, ac); DeleteObject(ac);
            }
            RECT calcIcon = {26, y + 9, 56, y + 39};
            FillRoundSolid(dc, calcIcon, 7, g_pal.accent);
            oldFont = (HFONT)SelectObject(dc, g_fontBig);
            SetTextColor(dc, g_pal.white);
            RECT eqr = {26, y + 9, 56, y + 39};
            DrawTextW(dc, L"=", -1, &eqr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            oldFont = (HFONT)SelectObject(dc, g_font);
            SetTextColor(dc, sel ? g_pal.nameSel : g_pal.name);
            RECT exprRect = {70, y + 4, g_w - 150, y + 25};
            DrawTextW(dc, TrimW(g_query).c_str(), -1, &exprRect,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            oldFont = (HFONT)SelectObject(dc, g_fontBig);
            SetTextColor(dc, g_pal.accent);
            RECT resRect = {70, y + 24, g_w - 150, y + 46};
            std::wstring eq = L"= " + g_calcResult;
            DrawTextW(dc, eq.c_str(), -1, &resRect,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            SelectObject(dc, oldFont);
            // 右侧胶囊提示「回车复制」
            RECT cp = {g_w - 118, y + 12, g_w - 22, y + 38};
            FillRoundSolid(dc, cp, 9, g_pal.pill);
            oldFont = (HFONT)SelectObject(dc, g_fontSmall);
            SetTextColor(dc, g_pal.pillText);
            DrawTextW(dc, L"\u56de\u8f66\u590d\u5236", -1, &cp,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, oldFont);
            continue;
        }

        const Item& it = g_items[g_results[i].index];

        if (sel) {
            RECT row = {8, y, g_w - 8, y + rowH - 4};
            FillRoundGradient(dc, row, 10,
                BlendColor(g_pal.selRow, g_pal.accent, 0.10f),
                BlendColor(g_pal.selRow, g_pal.accent, 0.22f),
                BlendColor(g_pal.selBar, g_pal.white, 0.35f), 90);

            RECT bar = {8, y + 8, 11, y + rowH - 12};
            HBRUSH accent = CreateSolidBrush(g_pal.selBar);
            FillRect(dc, &bar, accent);
            DeleteObject(accent);
        } else {
            HPEN sep = CreatePen(PS_SOLID, 1, g_pal.rowSep);
            oldPen = (HPEN)SelectObject(dc, sep);
            MoveToEx(dc, 18, y + rowH - 2, nullptr);
            LineTo(dc, g_w - 18, y + rowH - 2);
            SelectObject(dc, oldPen);
            DeleteObject(sep);
        }

        HICON icon = GetItemIcon(it);
        if (icon) DrawIconEx(dc, 26, y + 9, icon, 30, 30, 0, nullptr, DI_NORMAL);

        oldFont = (HFONT)SelectObject(dc, g_font);
        SetTextColor(dc, sel ? g_pal.nameSel : g_pal.name);
        RECT nameRect = {70, y + 5, g_w - 112, y + 28};
        DrawTextW(dc, it.name.c_str(), -1, &nameRect,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, oldFont);

        oldFont = (HFONT)SelectObject(dc, g_fontSmall);
        RECT measure = {0, 0, 160, 20};
        DrawTextW(dc, it.source.c_str(), -1, &measure,
                  DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        int badgeW = std::max(30, (int)(measure.right - measure.left) + 16);
        RECT badge = {70, y + 29, 70 + badgeW, y + 46};
        FillRoundSolid(dc, badge, 7, sel ? g_pal.badgeSel : g_pal.badge);
        SetTextColor(dc, sel ? g_pal.badgeTextSel : g_pal.badgeText);
        DrawTextW(dc, it.source.c_str(), -1, &badge,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        RECT pathRect = {70 + badgeW + 8, y + 28, g_w - 112, y + 47};
        SetTextColor(dc, sel ? g_pal.pathSel : g_pal.path);
        DrawTextW(dc, it.path.c_str(), -1, &pathRect,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

        if (g_showPinyin && !it.initials.empty()) {
            std::wstring iniW;
            for (unsigned char c : it.initials) iniW.push_back((wchar_t)c);
            RECT pm = {0, 0, 120, 20};
            DrawTextW(dc, iniW.c_str(), -1, &pm, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
            int pillW = std::max(26, (int)(pm.right - pm.left) + 16);
            RECT pill = {g_w - 22 - pillW, y + 12, g_w - 22, y + 38};
            FillRoundSolid(dc, pill, 9, sel ? g_pal.pillSel : g_pal.pill);
            SetTextColor(dc, sel ? g_pal.pillTextSel : g_pal.pillText);
            DrawTextW(dc, iniW.c_str(), -1, &pill,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(dc, oldFont);
    }

    // Footer
    HPEN footLine = CreatePen(PS_SOLID, 1, g_pal.footerLine);
    oldPen = (HPEN)SelectObject(dc, footLine);
    MoveToEx(dc, 0, g_h - 40, nullptr);
    LineTo(dc, g_w, g_h - 40);
    SelectObject(dc, oldPen);
    DeleteObject(footLine);

    oldFont = (HFONT)SelectObject(dc, g_fontSmall);

    wchar_t countText[32];
    if (g_indexState.load() == 1) wcscpy(countText, L"索引中");
    else if (!TrimW(g_query).empty()) swprintf(countText, 32, L"%d 项", (int)g_results.size());
    else swprintf(countText, 32, L"已索引 %d 项", (int)g_items.size());

    // 先按文本实际宽度给右侧计数分配空间，避免“已索引 778 项”被窗口边缘截断。
    RECT countMeasure = {0, 0, 0, 0};
    DrawTextW(dc, countText, -1, &countMeasure, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    int countW = std::min(160, std::max(56, (int)(countMeasure.right - countMeasure.left) + 4));
    RECT countRect = {g_w - 18 - countW, g_h - 36, g_w - 18, g_h - 8};

    RECT footer = {18, g_h - 36, countRect.left - 12, g_h - 8};
    if (!g_toast.empty()) {
        SetTextColor(dc, g_pal.accent);
        DrawTextW(dc, g_toast.c_str(), -1, &footer,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    } else {
        SetTextColor(dc, g_pal.footerText);
        DrawTextW(dc, L"\u2191\u2193 选择   Enter 启动   Ctrl+Enter 管理员   Tab 文件夹   Ctrl+Shift+C 复制   Esc 隐藏",
                  -1, &footer, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    SetTextColor(dc, g_pal.countText);
    DrawTextW(dc, countText, -1, &countRect,
              DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

// ---------------------------------------------------------------- actions
static void HideWindow() {
    g_hover = -1;
    g_trackingMouse = false;
    ShowWindow(g_hwnd, SW_HIDE);
}

static void ShowWindowFast() {
    int w = g_w, h = g_h;
    HMONITOR mon;
    if (g_followMouse) {
        POINT pt; GetCursorPos(&pt);
        mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    } else {
        mon = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
    }
    MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
    int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2 - 70;
    if (y < mi.rcWork.top + 20) y = mi.rcWork.top + 20;
    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_edit);
    Edit_SetText(g_edit, L"");
    g_query.clear();
    g_sel = 0;
    UpdateResults();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void SetClipboardText(const std::wstring& text) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        wchar_t* dst = (wchar_t*)GlobalLock(h);
        if (dst) { memcpy(dst, text.c_str(), bytes); GlobalUnlock(h); }
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

// 复制选中项：表达式结果或程序完整路径。返回是否发生了复制。
static bool CopySelected(bool calcResult) {
    if (g_results.empty()) return false;
    if (g_results[g_sel].index < 0) {
        if (!g_calcResult.empty()) SetClipboardText(g_calcResult);
        return true;
    }
    if (calcResult) {
        std::lock_guard<std::mutex> lock(g_itemsMu);
        if (g_sel >= (int)g_results.size()) return false;
        SetClipboardText(g_items[g_results[g_sel].index].path);
    }
    return true;
}

static void LaunchSelected(bool asAdmin, bool openFolder) {
    if (g_results.empty()) return;
    // 计算项：回车复制结果
    if (g_results[g_sel].index < 0) {
        if (!g_calcResult.empty()) SetClipboardText(g_calcResult);
        HideWindow();
        return;
    }
    std::wstring file, args, dir;
    bool apps = false;
    {
        std::lock_guard<std::mutex> lock(g_itemsMu);
        if (g_sel >= (int)g_results.size()) return;
        const Item& src = g_items[g_results[g_sel].index];
        file = src.path; args = src.args; dir = src.workdir; apps = src.apps;
        AddRecent(src.path);
    }
    if (openFolder) {
        if (apps) return;
        std::wstring target = file;
        file = L"explorer.exe";
        args = L"/select,\"" + target + L"\"";
        dir.clear();
    }
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    sei.lpVerb = asAdmin ? L"runas" : L"open";
    sei.lpFile = file.c_str();
    sei.lpParameters = args.empty() ? nullptr : args.c_str();
    sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
    HideWindow();
}

static void Navigate(int delta) {
    std::lock_guard<std::mutex> lock(g_itemsMu);
    if (g_results.empty()) return;
    int n = (int)g_results.size();
    g_sel = (g_sel + delta) % n;
    if (g_sel < 0) g_sel += n;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------- edit subclass
static LRESULT CALLBACK EditProc(HWND h, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN) {
        // Ctrl+Shift+C：复制选中项完整路径（计算项则复制结果）
        if (w == L'C' && (GetKeyState(VK_CONTROL) < 0) && (GetKeyState(VK_SHIFT) < 0)) {
            PostMessageW(g_hwnd, WM_APP_COPYPATH, 0, 0);
            return 0;
        }
        switch (w) {
        case VK_UP: PostMessageW(g_hwnd, WM_APP_NAV, -1, 0); return 0;
        case VK_DOWN: PostMessageW(g_hwnd, WM_APP_NAV, 1, 0); return 0;
        case VK_PRIOR: PostMessageW(g_hwnd, WM_APP_NAV, -5, 0); return 0;
        case VK_NEXT: PostMessageW(g_hwnd, WM_APP_NAV, 5, 0); return 0;
        case VK_RETURN:
            PostMessageW(g_hwnd, WM_APP_LAUNCH, (GetKeyState(VK_CONTROL) < 0) ? 1 : 0, 0);
            return 0;
        case VK_ESCAPE: PostMessageW(g_hwnd, WM_APP_HIDE, 0, 0); return 0;
        case VK_TAB: PostMessageW(g_hwnd, WM_APP_OPENFOLDER, 0, 0); return 0;
        }
    }
    if (msg == WM_PAINT) {
        // 先完整重绘编辑区背景：Edit 控件输入后只局部重绘新文本区域，
        // 若不清除整块区域，较长的占位文字像素会残留在输入内容下方。
        HDC bgdc = GetDC(h);
        RECT bg; GetClientRect(h, &bg);
        FillRect(bgdc, &bg, g_headBrush);
        ReleaseDC(h, bgdc);

        LRESULT r = DefSubclassProc(h, msg, w, l);
        if (Edit_GetTextLength(h) == 0) {
            HDC dc = GetDC(h);
            HFONT oldFont = (HFONT)SelectObject(dc, g_fontEdit);
            COLORREF oldColor = GetTextColor(dc);
            int oldBk = GetBkMode(dc);
            SetTextColor(dc, g_pal.placeholder);
            SetBkMode(dc, TRANSPARENT);
            RECT rc; GetClientRect(h, &rc);
            rc.left += 2;
            DrawTextW(dc, L"\u641c\u7d22\u7a0b\u5e8f\u3001\u8bbe\u7f6e\u3001\u6587\u4ef6\u2026",
                      -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            SetBkMode(dc, oldBk);
            SetTextColor(dc, oldColor);
            SelectObject(dc, oldFont);
            ReleaseDC(h, dc);
        }
        return r;
    }
    return DefSubclassProc(h, msg, w, l);
}

// ---------------------------------------------------------------- settings window
static bool RegisterOurHotkey(UINT mod, int vk, bool& fallback) {
    fallback = false;
    if (g_hwnd) UnregisterHotKey(g_hwnd, 1);
    UINT mods[] = { mod, MOD_CONTROL | MOD_ALT, MOD_CONTROL | MOD_SHIFT, MOD_ALT | MOD_SHIFT };
    int vks[] = { vk, VK_SPACE, VK_SPACE, VK_SPACE };
    for (int i = 0; i < 4; ++i) {
        if (RegisterHotKey(g_hwnd, 1, mods[i], vks[i])) {
            g_hotkeyMod = mods[i];
            g_hotkeyVK = vks[i];
            fallback = (i > 0);
            return true;
        }
    }
    return false;
}

static void SetNote(const wchar_t* text) {
    if (g_noteStatic) SetWindowTextW(g_noteStatic, text);
}

static std::wstring HotkeyDisplay() {
    if (g_capturingHotkey) return L"请按下快捷键…";
    if (!g_hotkeyEnabled) return L"已禁用（点击设置）";
    return g_hotkeyText;
}

static void UnregisterOurHotkey() {
    if (g_hwnd) UnregisterHotKey(g_hwnd, 1);
}

static bool HotkeyKeyName(UINT vk, wchar_t* out, size_t n) {
    if (!out || n < 8) return false;
    if ((vk >= L'A' && vk <= L'Z') || (vk >= L'0' && vk <= L'9')) {
        out[0] = (wchar_t)vk; out[1] = 0;
        return true;
    }
    if (vk >= VK_F1 && vk <= VK_F12) {
        swprintf(out, n, L"F%d", (int)(vk - VK_F1 + 1));
        return true;
    }
    if (vk == VK_SPACE) { wcscpy(out, L"Space"); return true; }
    if (vk == VK_RETURN) { wcscpy(out, L"Enter"); return true; }
    if (vk == VK_TAB) { wcscpy(out, L"Tab"); return true; }
    return false;
}

static void RefreshHotkeyButton() {
    HWND b = GetDlgItem(g_settingsHwnd, ID_HOTKEY_EDIT);
    if (!b) return;
    std::wstring text = HotkeyDisplay();
    SetWindowTextW(b, text.c_str());
    RedrawWindow(b, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}

static void BeginHotkeyCapture() {
    g_hotkeyCaptureOriginal = g_hotkeyText;
    g_hotkeyCaptureOriginalEnabled = g_hotkeyEnabled;
    g_capturingHotkey = true;
    RefreshHotkeyButton();
    SetNote(L"请按下组合键，Esc 取消");
    if (g_settingsHwnd) SetFocus(g_settingsHwnd);
}

static void CancelHotkeyCapture(const wchar_t* note) {
    if (!g_capturingHotkey) return;
    g_capturingHotkey = false;
    g_hotkeyText = g_hotkeyCaptureOriginal;
    g_hotkeyEnabled = g_hotkeyCaptureOriginalEnabled;
    RefreshHotkeyButton();
    if (note) SetNote(note);
}

static void HandleHotkeyCaptureKey(UINT vk) {
    if (vk == VK_ESCAPE) {
        CancelHotkeyCapture(L"已取消");
        return;
    }
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) {
        SetNote(L"请继续按下字母 / 数字 / F1-F12 / 空格");
        return;
    }
    wchar_t keyName[32] = {0};
    if (!HotkeyKeyName(vk, keyName, 32)) {
        SetNote(L"该按键不支持，请换一个");
        return;
    }
    UINT mod = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) mod |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) mod |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000) mod |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) mod |= MOD_WIN;
    if (!mod) {
        SetNote(L"请同时按住 Ctrl / Alt / Shift / Win");
        return;
    }
    std::wstring text;
    if (mod & MOD_CONTROL) text += L"Ctrl + ";
    if (mod & MOD_ALT) text += L"Alt + ";
    if (mod & MOD_SHIFT) text += L"Shift + ";
    if (mod & MOD_WIN) text += L"Win + ";
    text += keyName;

    g_hotkeyText = text;
    g_hotkeyEnabled = true;
    g_capturingHotkey = false;
    RefreshHotkeyButton();
    SetNote(L"已捕获，点击保存生效");
}

static void PaintSettingsButton(LPDRAWITEMSTRUCT di, const wchar_t* text, int style) {
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    bool pressed = (di->itemState & ODS_SELECTED) != 0;
    bool hot = (di->itemState & ODS_HOTLIGHT) != 0;

    // 焦点矩形是用 XOR 绘制的；每次自绘前用所在区域背景色整块覆盖，
    // 彻底清除上一次留下的浅色方框残影（圆角处尤其容易残留）。
    COLORREF bgCol = g_pal.card;
    UINT id = di->CtlID;
    if (id >= ID_NAV_GENERAL && id <= ID_NAV_ABOUT) bgCol = g_pal.side;
    else if (id == ID_CANCEL || id == ID_SAVE || id == ID_CLOSE) bgCol = g_pal.setBg;
    HBRUSH bgBrush = CreateSolidBrush(bgCol);
    FillRect(dc, &r, bgBrush);
    DeleteObject(bgBrush);

    if (style == 10) {
        FillRoundGradient(dc, r, 6, g_pal.editBg, g_pal.editBg,
                          g_capturingHotkey ? g_pal.accent : g_pal.editBorder, 255);
        HFONT oldFont = (HFONT)SelectObject(dc, g_setFont);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_capturingHotkey ? g_pal.hotkeyCapture
                     : (g_hotkeyEnabled ? g_pal.editText : g_pal.textMuted));
        r.left += 10;
        DrawTextW(dc, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        return;
    }

    if (style == 11) {
        // 主题切换小按钮
        FillRoundGradient(dc, r, 6, g_pal.btnBg, g_pal.btnBg, g_pal.btnBorder, 255);
        HFONT oldFont = (HFONT)SelectObject(dc, g_setFont);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_pal.text);
        DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        return;
    }

    if (style == 8 || style == 9) {
        // Toggle 改为 GDI+ 抗锯齿绘制，去掉 GDI RoundRect/Ellipse 的毛刺。
        bool on = (style == 8);
        COLORREF base = on ? g_pal.accent : (hot ? g_pal.toggleOffHot : g_pal.toggleOff);
        {
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
            RECT track = { r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 };
            int radius = (track.bottom - track.top) / 2;
            Gdiplus::GraphicsPath path;
            AddRoundRectPath(path, track.left, track.top, track.right, track.bottom, radius);
            Gdiplus::LinearGradientBrush brush(
                Gdiplus::Point(track.left, track.top),
                Gdiplus::Point(track.left, track.bottom),
                GdipColor(BlendColor(base, g_pal.white, 0.10f), 255),
                GdipColor(BlendColor(base, g_pal.headBand, 0.10f), 255));
            g.FillPath(&brush, &path);
            Gdiplus::Pen edge(GdipColor(BlendColor(base, g_pal.white, 0.25f), 150), 1.0f);
            g.DrawPath(&edge, &path);

            int d = std::min<int>(12, (r.bottom - r.top) - 6);
            Gdiplus::REAL kx = (Gdiplus::REAL)(on ? (r.right - d - 4) : (r.left + 4));
            Gdiplus::REAL ky = (Gdiplus::REAL)(r.top + (r.bottom - r.top - d) / 2);
            Gdiplus::SolidBrush shadow(Gdiplus::Color(55, 0, 0, 0));
            g.FillEllipse(&shadow, kx + 1.0f, ky + 2.0f, (Gdiplus::REAL)d, (Gdiplus::REAL)d);
            Gdiplus::SolidBrush knob(GdipColor(g_pal.white, 255));
            g.FillEllipse(&knob, kx, ky, (Gdiplus::REAL)d, (Gdiplus::REAL)d);
        }
        return;
    }

    COLORREF bg;
    COLORREF fg = g_pal.btnFg;
    COLORREF border = g_pal.btnBorder;
    if (style == 1) { bg = pressed ? g_pal.primaryPress : g_pal.accent; fg = g_pal.white; border = bg; }
    else if (style == 4) { bg = g_pal.headBand; fg = hot ? g_pal.closeHot : g_pal.closeFg; border = bg; }
    else if (style == 5) { bg = g_pal.btnBg; fg = g_pal.textBright; border = bg; }
    else if (style == 6) { bg = g_pal.side; fg = g_pal.textMuted; border = bg; }
    else if (style == 7) { bg = pressed ? g_pal.btnPress : (hot ? g_pal.btnHover : g_pal.btnBg); }
    else { bg = pressed ? g_pal.btnPress : (hot ? g_pal.btnHover : g_pal.btnBg); }

    FillRoundGradient(dc, r, 6, bg, bg, border, 255);
    if (style == 5) {
        HBRUSH accent = CreateSolidBrush(g_pal.accent);
        RECT bar = { r.left, r.top + 6, r.left + 2, r.bottom - 6 };
        FillRect(dc, &bar, accent);
        DeleteObject(accent);
    }

    HFONT oldFont = (HFONT)SelectObject(dc, g_setFont);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    UINT flags = DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
    flags |= (style == 5 || style == 6) ? DT_LEFT : DT_CENTER;
    RECT tr = r;
    if (style == 5 || style == 6) tr.left += 14;
    DrawTextW(dc, text, -1, &tr, flags);
    SelectObject(dc, oldFont);
}

static void CloseThemePopup();

// 选择主题后立即预览，两个窗口与浮层一起刷新。
static void PreviewTheme(int idx) {
    if (idx < 0 || idx >= THEME_COUNT) return;
    g_themePending = idx;
    ApplyPalette(idx);
    RebuildThemeBrushes();
    ApplyWindowEffect();
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, TRUE);
    if (g_settingsHwnd) {
        InvalidateRect(g_settingsHwnd, nullptr, TRUE);
        RedrawWindow(g_settingsHwnd, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }
    std::wstring note = L"已预览：";
    note += ThemeName(idx);
    note += L"，保存后生效";
    SetNote(note.c_str());
}

// 浮层使用 32bpp DIB + UpdateLayeredWindow，实现真正的圆角与柔和阴影。
static void CreateThemePopupSurface() {
    if (g_themePopupDC || g_themePopupBmp) return;
    g_themePopupDC = CreateCompatibleDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_themePopupW;
    bi.bmiHeader.biHeight = g_themePopupH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    g_themePopupBmp = CreateDIBSection(g_themePopupDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (g_themePopupBmp) SelectObject(g_themePopupDC, g_themePopupBmp);
}

static void DestroyThemePopupSurface() {
    if (g_themePopupBmp) { DeleteObject(g_themePopupBmp); g_themePopupBmp = nullptr; }
    if (g_themePopupDC) { DeleteDC(g_themePopupDC); g_themePopupDC = nullptr; }
}

static void RenderThemePopup(HDC dc) {
    if (!dc) return;
    int W = g_themePopupW, H = g_themePopupH;
    int panelW = W - THEME_POPUP_SHADOW * 2;
    int panelH = H - THEME_POPUP_SHADOW * 2;
    RECT panel = {
        THEME_POPUP_SHADOW, THEME_POPUP_SHADOW,
        THEME_POPUP_SHADOW + panelW, THEME_POPUP_SHADOW + panelH
    };

    {
        Gdiplus::Graphics g(dc);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);

        // 多层低透明度圆角叠加出柔和阴影。
        for (int i = 5; i >= 1; --i) {
            RECT sr = {
                panel.left - i, panel.top - i + 1,
                panel.right + i, panel.bottom + i
            };
            Gdiplus::GraphicsPath path;
            AddRoundRectPath(path, sr.left, sr.top, sr.right, sr.bottom, 10 + i);
            Gdiplus::SolidBrush shadow(Gdiplus::Color(12, 0, 0, 0));
            g.FillPath(&shadow, &path);
        }

        Gdiplus::GraphicsPath panelPath;
        AddRoundRectPath(panelPath, panel.left, panel.top, panel.right, panel.bottom, 10);
        Gdiplus::LinearGradientBrush panelBrush(
            Gdiplus::Point(panel.left, panel.top),
            Gdiplus::Point(panel.left, panel.bottom),
            GdipColor(BlendColor(g_pal.card, g_pal.white, 0.04f), 250),
            GdipColor(g_pal.card, 250));
        g.FillPath(&panelBrush, &panelPath);
        Gdiplus::Pen border(GdipColor(g_pal.cardBorder, 185), 1.0f);
        g.DrawPath(&border, &panelPath);

        for (int i = 0; i < THEME_COUNT; ++i) {
            RECT row = {
                panel.left + THEME_POPUP_PAD,
                panel.top + THEME_POPUP_PAD + i * THEME_ITEM_H,
                panel.right - THEME_POPUP_PAD,
                panel.top + THEME_POPUP_PAD + (i + 1) * THEME_ITEM_H
            };
            bool current = (i == g_themePending);
            bool hover = (i == g_themePopupHover);
            if (current || hover) {
                Gdiplus::GraphicsPath rowPath;
                AddRoundRectPath(rowPath, row.left, row.top, row.right, row.bottom, 6);
                COLORREF top = hover ? BlendColor(g_pal.card, g_pal.selRow, 0.85f) : g_pal.selRow;
                COLORREF bottom = hover ? BlendColor(g_pal.card, g_pal.selRow, 0.65f)
                                        : BlendColor(g_pal.selRow, g_pal.card, 0.18f);
                Gdiplus::LinearGradientBrush rowBrush(
                    Gdiplus::Point(row.left, row.top),
                    Gdiplus::Point(row.left, row.bottom),
                    GdipColor(top, 255), GdipColor(bottom, 255));
                g.FillPath(&rowBrush, &rowPath);
                if (current) {
                    Gdiplus::Pen active(GdipColor(g_pal.accent, 220), 1.2f);
                    g.DrawPath(&active, &rowPath);
                }
            }
        }

        // 32bpp DIB 上用 GDI DrawTextW 会出现黑边/发灰；GDI+ DrawString 才清晰。
        HFONT hfont = g_setFont ? g_setFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        Gdiplus::Font font(dc, hfont);
        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentNear);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        for (int i = 0; i < THEME_COUNT; ++i) {
            RECT row = {
                panel.left + THEME_POPUP_PAD,
                panel.top + THEME_POPUP_PAD + i * THEME_ITEM_H,
                panel.right - THEME_POPUP_PAD,
                panel.top + THEME_POPUP_PAD + (i + 1) * THEME_ITEM_H
            };
            bool active = (i == g_themePending || i == g_themePopupHover);
            if (i == g_themePending) {
                // 字体里的 ✓ glyph 在部分字体下会渲染成小点，直接手绘更醒目。
                Gdiplus::REAL cx = (Gdiplus::REAL)(row.left + 15);
                Gdiplus::REAL cy = (Gdiplus::REAL)(row.top + (row.bottom - row.top) / 2);
                Gdiplus::Pen glow(GdipColor(g_pal.accent, 110), 5.0f);
                glow.SetStartCap(Gdiplus::LineCapRound);
                glow.SetEndCap(Gdiplus::LineCapRound);
                g.DrawLine(&glow, cx - 5, cy + 0.5f, cx - 1.5f, cy + 4);
                g.DrawLine(&glow, cx - 1.5f, cy + 4, cx + 6, cy - 4.5f);
                Gdiplus::Pen mark(GdipColor(g_pal.white, 255), 2.4f);
                mark.SetStartCap(Gdiplus::LineCapRound);
                mark.SetEndCap(Gdiplus::LineCapRound);
                g.DrawLine(&mark, cx - 5, cy + 0.5f, cx - 1.5f, cy + 4);
                g.DrawLine(&mark, cx - 1.5f, cy + 4, cx + 6, cy - 4.5f);
            }
            Gdiplus::RectF tr((Gdiplus::REAL)(row.left + 28), (Gdiplus::REAL)row.top,
                              (Gdiplus::REAL)(row.right - row.left - 36),
                              (Gdiplus::REAL)(row.bottom - row.top));
            Gdiplus::SolidBrush textBrush(GdipColor(active ? g_pal.textBright : g_pal.text, 255));
            g.DrawString(ThemeName(i), -1, &font, tr, &sf, &textBrush);
        }
    }
}

static void UpdateThemePopupLayer(HWND h) {
    if (!h || !g_themePopupDC) return;
    RECT wr; GetWindowRect(h, &wr);
    POINT pos = { wr.left, wr.top };
    POINT zero = { 0, 0 };
    SIZE size = { g_themePopupW, g_themePopupH };
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(h, screen, &pos, &size, g_themePopupDC, &zero, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

static int ThemeIndexFromPoint(POINT pt) {
    int panelW = g_themePopupW - THEME_POPUP_SHADOW * 2;
    int x = pt.x - THEME_POPUP_SHADOW - THEME_POPUP_PAD;
    int y = pt.y - THEME_POPUP_SHADOW - THEME_POPUP_PAD;
    if (x < 0 || y < 0 || x >= panelW - THEME_POPUP_PAD * 2 ||
        y >= THEME_COUNT * THEME_ITEM_H) return -1;
    return std::min(THEME_COUNT - 1, y / THEME_ITEM_H);
}

static LRESULT CALLBACK ThemePopupProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE:
        CreateThemePopupSurface();
        return 0;

    case WM_DESTROY:
        DestroyThemePopupSurface();
        g_themePopup = nullptr;
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_ACTIVATEANDEAT;

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        int idx = ThemeIndexFromPoint(pt);
        if (idx != g_themePopupHover) {
            g_themePopupHover = idx;
            RenderThemePopup(g_themePopupDC);
            UpdateThemePopupLayer(h);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        int idx = ThemeIndexFromPoint(pt);
        if (idx < 0) { CloseThemePopup(); return 0; }
        g_themePopupHover = idx;
        RenderThemePopup(g_themePopupDC);
        UpdateThemePopupLayer(h);
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        int idx = ThemeIndexFromPoint(pt);
        int chosen = g_themePopupHover;
        CloseThemePopup();
        if (idx >= 0 && chosen >= 0 && idx == chosen) PreviewTheme(chosen);
        return 0;
    }

    case WM_RBUTTONDOWN:
    case WM_CANCELMODE:
        CloseThemePopup();
        return 0;

    case WM_KEYDOWN:
        if (w == VK_ESCAPE) {
            CloseThemePopup();
        } else if (w == VK_UP || w == VK_DOWN) {
            int cur = g_themePopupHover >= 0 ? g_themePopupHover : g_themePending;
            cur = (cur + (w == VK_UP ? THEME_COUNT - 1 : 1)) % THEME_COUNT;
            g_themePopupHover = cur;
            RenderThemePopup(g_themePopupDC);
            UpdateThemePopupLayer(h);
        } else if (w == VK_RETURN || w == VK_SPACE) {
            int chosen = g_themePopupHover >= 0 ? g_themePopupHover : g_themePending;
            CloseThemePopup();
            PreviewTheme(chosen);
        }
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

static void CloseThemePopup() {
    HWND h = g_themePopup;
    if (!h) return;
    g_themePopup = nullptr;
    if (GetCapture() == h) ReleaseCapture();
    if (IsWindow(h)) DestroyWindow(h);
}

static void ShowThemePopup(HWND button) {
    if (g_themePopup) { CloseThemePopup(); return; }
    RECT br;
    if (!button || !GetWindowRect(button, &br)) return;

    int panelW = std::max(116, (int)(br.right - br.left));
    int panelH = THEME_POPUP_PAD * 2 + THEME_COUNT * THEME_ITEM_H;
    g_themePopupW = panelW + THEME_POPUP_SHADOW * 2;
    g_themePopupH = panelH + THEME_POPUP_SHADOW * 2;
    g_themePopupHover = g_themePending;

    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromRect(&br, MONITOR_DEFAULTTONEAREST), &mi);
    int x = br.left - THEME_POPUP_SHADOW;
    int y = br.bottom + 4 - THEME_POPUP_SHADOW;
    // 优先留在设置窗口内部，避免浮层伸到窗口下方的亮色背景上；
    // 空间不足时再向上翻转，最后才按显示器工作区兜底。
    RECT sr = {};
    if (g_settingsHwnd && GetWindowRect(g_settingsHwnd, &sr)) {
        if (y + g_themePopupH > sr.bottom - 8)
            y = br.top - 4 - panelH - THEME_POPUP_SHADOW;
        x = std::max((int)sr.left + 8,
                     std::min(x, (int)(sr.right - g_themePopupW - 8)));
        y = std::max((int)sr.top + 8,
                     std::min(y, (int)(sr.bottom - g_themePopupH - 8)));
    }
    if (y + g_themePopupH > mi.rcWork.bottom)
        y = br.top - 4 - panelH - THEME_POPUP_SHADOW;
    x = std::max((int)mi.rcWork.left, std::min(x, (int)(mi.rcWork.right - g_themePopupW)));
    y = std::max((int)mi.rcWork.top, std::min(y, (int)(mi.rcWork.bottom - g_themePopupH)));

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ThemePopupProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kThemePopupClass;
        if (RegisterClassExW(&wc)) registered = true;
    }
    HWND p = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        kThemePopupClass, L"", WS_POPUP,
        x, y, g_themePopupW, g_themePopupH,
        g_settingsHwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!p) return;
    g_themePopup = p;
    ShowWindow(p, SW_SHOWNOACTIVATE);
    SetWindowPos(p, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    RenderThemePopup(g_themePopupDC);
    UpdateThemePopupLayer(p);
    SetCapture(p);
    // 让浮层真正拿到焦点，避免主题按钮残留系统白色焦点框。
    SetActiveWindow(p);
    SetFocus(p);
}

// 收起态：圆角按钮 + GDI+ 抗锯齿箭头。
static void PaintThemeComboButton(LPDRAWITEMSTRUCT di) {
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    bool hot = (di->itemState & ODS_HOTLIGHT) != 0;
    bool focus = (di->itemState & ODS_FOCUS) != 0;

    // 先整块铺底色，避免圆角外露出系统按钮的白色背景/焦点框。
    HBRUSH base = CreateSolidBrush(g_pal.card);
    FillRect(dc, &r, base);
    DeleteObject(base);

    COLORREF top = hot ? BlendColor(g_pal.btnHover, g_pal.white, 0.04f)
                       : BlendColor(g_pal.btnBg, g_pal.white, 0.025f);
    COLORREF bottom = hot ? BlendColor(g_pal.btnHover, g_pal.card, 0.18f) : g_pal.btnBg;
    FillRoundGradient(dc, r, 6, top, bottom, g_pal.btnBorder, focus ? 230 : 165);

    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        Gdiplus::Pen pen(GdipColor(g_pal.textMuted), 1.7f);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        int ax = r.right - 15, ay = (r.top + r.bottom) / 2;
        g.DrawLine(&pen, (Gdiplus::REAL)(ax - 4), (Gdiplus::REAL)(ay - 2),
                         (Gdiplus::REAL)ax, (Gdiplus::REAL)(ay + 2));
        g.DrawLine(&pen, (Gdiplus::REAL)ax, (Gdiplus::REAL)(ay + 2),
                         (Gdiplus::REAL)(ax + 4), (Gdiplus::REAL)(ay - 2));
    }

    HFONT oldFont = (HFONT)SelectObject(dc, g_setFont);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, g_pal.textBright);
    RECT tr = r;
    tr.left += 10;
    tr.right -= 24;
    DrawTextW(dc, ThemeName(g_themePending), -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

static bool ApplySettings(bool notify) {
    WCHAR buf[4096];
    std::wstring hk = TrimW(g_hotkeyText);
    UINT mod = 0; int vk = 0;
    if (g_hotkeyEnabled && (hk.empty() || !ParseHotkey(hk, mod, vk))) {
        MessageBoxW(g_settingsHwnd, L"快捷键无效，请使用 Alt+Space 这类格式", L"FlashLaunch", MB_OK | MB_ICONWARNING);
        return false;
    }
    GetWindowTextW(GetDlgItem(g_settingsHwnd, ID_MAX_EDIT), buf, 4096);
    int maxResults = _wtoi(buf);
    if (maxResults < 3 || maxResults > 16) {
        MessageBoxW(g_settingsHwnd, L"结果数量需为 3 - 16", L"FlashLaunch", MB_OK | MB_ICONWARNING);
        return false;
    }

    std::vector<std::wstring> dirs;
    GetWindowTextW(GetDlgItem(g_settingsHwnd, ID_PATHS_EDIT), buf, 4096);
    std::wstring all = buf;
    size_t pos = 0;
    while (pos < all.size()) {
        size_t e = all.find_first_of(L"\n;", pos);
        if (e == std::wstring::npos) e = all.size();
        std::wstring p = TrimW(all.substr(pos, e - pos));
        if (!p.empty()) dirs.push_back(ExpandEnvW(p));
        pos = e + 1;
    }

    g_maxResults = maxResults;
    g_showPinyin = g_tglPinyin;
    if (g_hotkeyEnabled) {
        std::wstring canonical = FormatHotkeyText(mod, vk);
        g_hotkeyText = canonical.empty() ? hk : canonical;
    } else {
        g_hotkeyText = L"disabled";
    }
    g_theme = g_themePending;
    g_followMouse = g_tglFollowMouse;
    ApplyPalette(g_theme);
    RebuildThemeBrushes();
    ApplyWindowEffect();
    UpdateHeaderLayout();
    InvalidateRect(g_hwnd, nullptr, TRUE);
    g_extraPaths.swap(dirs);
    SaveConfig();

    if (g_tglAutoStart != g_autoStart) SetAutoStart(g_tglAutoStart);

    if (!g_hotkeyEnabled) {
        UnregisterOurHotkey();
        if (notify) SetNote(L"已禁用唤醒快捷键");
        return true;
    }
    bool fallback = false;
    if (!RegisterOurHotkey(mod, vk, fallback)) {
        if (notify) SetNote(L"热键注册失败，请更换组合");
        return false;
    }
    if (notify) SetNote(fallback ? L"热键冲突，已使用备用组合" : L"已保存");
    return true;
}

static void ReloadToggleButtons() {
    RedrawWindow(GetDlgItem(g_settingsHwnd, ID_PINYIN_TGL), nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    RedrawWindow(GetDlgItem(g_settingsHwnd, ID_AUTOSTART_TGL), nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    RedrawWindow(GetDlgItem(g_settingsHwnd, ID_FOLLOWMOUSE_TGL), nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}

static void ShowSettingsPage(int page) {
    g_settingsPage = page;
    if (page != 0) CloseThemePopup();
    struct CtrlPage { int id; int page; };
    static const CtrlPage map[] = {
        { ID_HOTKEY_EDIT, 0 }, { ID_HOTKEY_DISABLE, 0 }, { ID_AUTOSTART_TGL, 0 },
        { ID_THEME_COMBO, 0 }, { ID_FOLLOWMOUSE_TGL, 0 },
        { ID_MAX_EDIT, 1 }, { ID_PINYIN_TGL, 1 },
        { ID_PATHS_EDIT, 2 }, { ID_BROWSE, 2 }, { ID_REBUILD, 2 }
    };
    for (const auto& c : map) {
        HWND ctl = GetDlgItem(g_settingsHwnd, c.id);
        if (ctl) ShowWindow(ctl, c.page == page ? SW_SHOW : SW_HIDE);
    }
    for (int i = 0; i < 4; ++i) {
        HWND nav = GetDlgItem(g_settingsHwnd, ID_NAV_GENERAL + i);
        if (nav) RedrawWindow(nav, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }
    InvalidateRect(g_settingsHwnd, nullptr, TRUE);
}

static void DrawCard(HDC dc, int l, int t, int r, int b) {
    // 轻微渐变 + 半透明描边，卡片更有“浮起”的层次感。
    RECT rc = {l, t, r, b};
    FillRoundGradient(dc, rc, 10,
        BlendColor(g_pal.card, g_pal.white, 0.035f),
        BlendColor(g_pal.card, g_pal.headBand, 0.05f),
        g_pal.cardBorder, 150);
}

static void DrawEditBorder(HDC dc, int l, int t, int r, int b) {
    RECT rc = {l, t, r, b};
    DrawRoundBorder(dc, rc, 6, g_pal.editBorder);
}

// 取消/关闭/Esc 时撤销主题即时预览，回到打开设置前的配色。
static void RevertThemeIfNeeded() {
    if (g_theme != g_themeOriginal) {
        g_theme = g_themeOriginal;
        ApplyPalette(g_theme);
        RebuildThemeBrushes();
        ApplyWindowEffect();
        if (g_hwnd) InvalidateRect(g_hwnd, nullptr, TRUE);
    }
}

static LRESULT CALLBACK SettingsProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE:
        g_noteStatic = nullptr;
        g_setBgBrush = CreateSolidBrush(g_pal.setBg);
        g_setEditBrush = CreateSolidBrush(g_pal.editBg);
        g_setFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        g_setFontTitle = CreateFontW(-15, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                     OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        g_setFontHint = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                    OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        ApplyWindowCorners(h, 620, 440, 12);
        break;

    case WM_NCCALCSIZE:
        // 同上：保留框架语义供 DWM 圆角/阴影，去掉可见边框使自绘铺满窗口。
        return 0;

    case WM_NCACTIVATE:
        // WS_THICKFRAME 窗口失焦时系统会用“非激活”配色画边框（深色界面下
        // 就是一圈白边）。强制始终按激活态绘制非客户区，消除失焦白边。
        return TRUE;

    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ScreenToClient(h, &pt);
        if (pt.y < 52 && pt.x < 582) return HTCAPTION;
        return HTCLIENT;
    }

    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)w, TRANSPARENT);
        SetTextColor((HDC)w, g_pal.textMuted);
        return (LRESULT)g_setBgBrush;

    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)w, g_pal.editText);
        SetBkColor((HDC)w, g_pal.editBg);
        return (LRESULT)g_setEditBrush;

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT di = (LPDRAWITEMSTRUCT)l;
        switch (di->CtlID) {
        case ID_NAV_GENERAL: PaintSettingsButton(di, L"常规", g_settingsPage == 0 ? 5 : 6); break;
        case ID_NAV_SEARCH:  PaintSettingsButton(di, L"搜索", g_settingsPage == 1 ? 5 : 6); break;
        case ID_NAV_DIRS:    PaintSettingsButton(di, L"目录", g_settingsPage == 2 ? 5 : 6); break;
        case ID_NAV_ABOUT:   PaintSettingsButton(di, L"关于", g_settingsPage == 3 ? 5 : 6); break;
        case ID_HOTKEY_EDIT: {
            std::wstring text = HotkeyDisplay();
            PaintSettingsButton(di, text.c_str(), 10);
            break;
        }
        case ID_HOTKEY_DISABLE: PaintSettingsButton(di, L"禁用", 7); break;
        case ID_PINYIN_TGL:    PaintSettingsButton(di, L"", g_tglPinyin ? 8 : 9); break;
        case ID_AUTOSTART_TGL: PaintSettingsButton(di, L"", g_tglAutoStart ? 8 : 9); break;
        case ID_THEME_COMBO:  PaintThemeComboButton(di); break;
        case ID_FOLLOWMOUSE_TGL: PaintSettingsButton(di, L"", g_tglFollowMouse ? 8 : 9); break;
        case ID_SAVE:      PaintSettingsButton(di, L"保存", 1); break;
        case ID_CANCEL:    PaintSettingsButton(di, L"取消", 0); break;
        case ID_BROWSE:    PaintSettingsButton(di, L"+ 添加目录", 7); break;
        case ID_REBUILD:   PaintSettingsButton(di, L"重建索引", 7); break;
        case ID_CLOSE:     PaintSettingsButton(di, L"×", 4); break;
        }
        return TRUE;
    }

    case WM_COMMAND: {
        WORD id = LOWORD(w);
        if (g_themePopup && id != ID_THEME_COMBO) CloseThemePopup();
        if (HIWORD(w) == BN_CLICKED) {
            if (id >= ID_NAV_GENERAL && id <= ID_NAV_ABOUT) {
                ShowSettingsPage(id - ID_NAV_GENERAL);
                SetNote(L"保存后立即生效");
                return 0;
            }
            switch (id) {
            case ID_HOTKEY_EDIT:
                BeginHotkeyCapture();
                return 0;
            case ID_HOTKEY_DISABLE:
                CancelHotkeyCapture(nullptr);
                g_hotkeyEnabled = false;
                g_hotkeyText = L"disabled";
                RefreshHotkeyButton();
                SetNote(L"已禁用，保存后生效");
                return 0;
            case ID_THEME_COMBO:
                ShowThemePopup(GetDlgItem(h, ID_THEME_COMBO));
                return 0;
            case ID_PINYIN_TGL:
                g_tglPinyin = !g_tglPinyin;
                ReloadToggleButtons();
                SetNote(L"");
                return 0;
            case ID_AUTOSTART_TGL:
                g_tglAutoStart = !g_tglAutoStart;
                ReloadToggleButtons();
                SetNote(L"");
                return 0;
            case ID_FOLLOWMOUSE_TGL:
                g_tglFollowMouse = !g_tglFollowMouse;
                ReloadToggleButtons();
                SetNote(L"");
                return 0;
            case ID_BROWSE: {
                WCHAR display[MAX_PATH] = {0};
                BROWSEINFOW bi = {};
                bi.hwndOwner = h;
                bi.pszDisplayName = display;
                bi.lpszTitle = L"选择要加入检索的目录";
                bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
                if (pidl) {
                    WCHAR path[MAX_PATH] = {0};
                    if (SHGetPathFromIDListW(pidl, path)) {
                        HWND ed = GetDlgItem(h, ID_PATHS_EDIT);
                        WCHAR existing[4096] = {0};
                        GetWindowTextW(ed, existing, 4096);
                        std::wstring text = existing;
                        if (!text.empty() && text.back() != L'\n') text += L'\n';
                        text += path;
                        SetWindowTextW(ed, text.c_str());
                    }
                    CoTaskMemFree(pidl);
                }
                return 0;
            }
            case ID_REBUILD:
                if (ApplySettings(false)) {
                    StartIndex();
                    SetNote(L"已开始重建索引");
                }
                return 0;
            case ID_SAVE:
                if (ApplySettings(true)) DestroyWindow(h);
                return 0;
            case ID_CANCEL:
            case ID_CLOSE:
                RevertThemeIfNeeded();
                DestroyWindow(h);
                return 0;
            }
        }
        if (HIWORD(w) == EN_CHANGE) SetNote(L"");
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, g_setBgBrush);

        HBRUSH side = CreateSolidBrush(g_pal.side);
        RECT sr = { 0, 0, 150, 440 };
        FillRect(dc, &sr, side);
        DeleteObject(side);
        HPEN sep = CreatePen(PS_SOLID, 1, g_pal.setSep);
        HPEN oldPen = (HPEN)SelectObject(dc, sep);
        MoveToEx(dc, 150, 0, nullptr); LineTo(dc, 150, 440);
        MoveToEx(dc, 150, 52, nullptr); LineTo(dc, 620, 52);
        MoveToEx(dc, 150, 388, nullptr); LineTo(dc, 620, 388);
        SelectObject(dc, oldPen);
        DeleteObject(sep);

        RECT lr = { 14, 16, 32, 34 };
        FillRoundSolid(dc, lr, 5, g_pal.accent);

        SetBkMode(dc, TRANSPARENT);
        HFONT oldFont = (HFONT)SelectObject(dc, g_setFont);
        SetTextColor(dc, g_pal.textBright);
        RECT brand = { 40, 13, 144, 38 };
        DrawTextW(dc, L"FlashLaunch", -1, &brand, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        oldFont = (HFONT)SelectObject(dc, g_setFontHint);
        SetTextColor(dc, g_pal.ver);
        RECT ver = { 14, 392, 140, 422 };
        DrawTextW(dc, L"v1.9\n轻量 · 快速", -1, &ver, DT_LEFT | DT_NOPREFIX);
        SelectObject(dc, oldFont);

        const wchar_t* titles[] = { L"常规", L"搜索", L"目录", L"关于" };
        const wchar_t* subs[] = { L"启动方式与快捷键", L"匹配规则与结果显示", L"附加检索路径", L"版本与说明" };
        oldFont = (HFONT)SelectObject(dc, g_setFontTitle);
        SetTextColor(dc, g_pal.setTitle);
        RECT tr = { 168, 8, 460, 30 };
        DrawTextW(dc, titles[g_settingsPage], -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        oldFont = (HFONT)SelectObject(dc, g_setFontHint);
        SetTextColor(dc, g_pal.hint);
        tr.top = 28; tr.bottom = 46;
        DrawTextW(dc, subs[g_settingsPage], -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, oldFont);

        oldFont = (HFONT)SelectObject(dc, g_setFont);
        SetTextColor(dc, g_pal.label);
        RECT label;
        if (g_settingsPage == 0) {
            DrawCard(dc, 168, 68, 602, 138);
            DrawCard(dc, 168, 148, 602, 226);
            label = { 186, 76, 400, 98 };
            DrawTextW(dc, L"唤醒快捷键", -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            oldFont = (HFONT)SelectObject(dc, g_setFontHint);
            SetTextColor(dc, g_pal.hint);
            RECT h1 = { 470, 102, 596, 132 };
            DrawTextW(dc, L"点击后按键，Esc取消", -1, &h1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            oldFont = (HFONT)SelectObject(dc, g_setFont);
            SetTextColor(dc, g_pal.label);
            label = { 186, 156, 400, 176 };
            DrawTextW(dc, L"启动", -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SetTextColor(dc, g_pal.text);
            RECT t1 = { 228, 178, 400, 204 };
            DrawTextW(dc, g_tglAutoStart ? L"开机自启   已开启" : L"开机自启   已关闭", -1, &t1,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            oldFont = (HFONT)SelectObject(dc, g_setFontHint);
            SetTextColor(dc, g_pal.hint);
            RECT h2 = { 400, 180, 590, 202 };
            DrawTextW(dc, L"登录后自动运行", -1, &h2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            // 第三张卡片：外观与定位。标题独占一行，两行设置整体下移，
            // 避免标题与“界面主题 / 主题切换按钮”重叠。
            DrawCard(dc, 168, 236, 602, 352);
            oldFont = (HFONT)SelectObject(dc, g_setFont);
            SetTextColor(dc, g_pal.label);
            label = { 186, 244, 400, 266 };
            DrawTextW(dc, L"外观与定位", -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SetTextColor(dc, g_pal.text);
            RECT t2 = { 286, 274, 400, 300 };
            DrawTextW(dc, L"界面主题", -1, &t2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            RECT t3 = { 232, 314, 400, 340 };
            DrawTextW(dc, L"跟随鼠标定位", -1, &t3, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            oldFont = (HFONT)SelectObject(dc, g_setFontHint);
            SetTextColor(dc, g_pal.hint);
            RECT h3 = { 400, 274, 590, 300 };
            DrawTextW(dc, L"选择后即时预览", -1, &h3, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            RECT h4 = { 400, 314, 590, 340 };
            DrawTextW(dc, L"在多屏间跟随光标弹出", -1, &h4, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, oldFont);
        } else if (g_settingsPage == 1) {
            DrawCard(dc, 168, 68, 602, 210);
            label = { 186, 76, 400, 96 };
            DrawTextW(dc, L"搜索行为", -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SetTextColor(dc, g_pal.text);
            RECT t0 = { 186, 100, 286, 128 };
            DrawTextW(dc, L"结果数量", -1, &t0, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            DrawEditBorder(dc, 300, 100, 364, 128);
            oldFont = (HFONT)SelectObject(dc, g_setFontHint);
            SetTextColor(dc, g_pal.hint);
            RECT h0 = { 374, 100, 480, 128 };
            DrawTextW(dc, L"3 – 16", -1, &h0, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            oldFont = (HFONT)SelectObject(dc, g_setFont);
            SetTextColor(dc, g_pal.text);
            RECT t1 = { 228, 140, 380, 166 };
            DrawTextW(dc, L"拼音首字母", -1, &t1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            oldFont = (HFONT)SelectObject(dc, g_setFontHint);
            SetTextColor(dc, g_pal.hint);
            RECT h1 = { 330, 142, 590, 164 };
            DrawTextW(dc, L"在结果右侧显示拼音胶囊", -1, &h1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            RECT h3 = { 186, 176, 590, 198 };
            DrawTextW(dc, L"索引在启动时建立；输入关键词后显示匹配结果", -1, &h3,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        } else if (g_settingsPage == 2) {
            DrawCard(dc, 168, 68, 602, 244);
            label = { 186, 76, 500, 96 };
            DrawTextW(dc, L"附加检索目录（每行一个）", -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            DrawEditBorder(dc, 186, 102, 584, 198);
        } else {
            DrawCard(dc, 168, 68, 602, 250);
            oldFont = (HFONT)SelectObject(dc, g_setFont);
            SetTextColor(dc, g_pal.textBright);
            RECT a1 = { 186, 88, 584, 116 };
            DrawTextW(dc, L"FlashLaunch 闪启  v1.9", -1, &a1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            oldFont = (HFONT)SelectObject(dc, g_setFontHint);
            SetTextColor(dc, g_pal.textMuted);
            RECT a2 = { 186, 126, 584, 232 };
            DrawTextW(dc, L"原生 Win32 + GDI 实现，无第三方运行时。\n支持中文、全拼、拼音首字母搜索。\n设置保存在本地，托盘与标题栏齿轮均可打开本窗口。",
                      -1, &a2, DT_LEFT | DT_NOPREFIX);
        }
        SelectObject(dc, oldFont);
        EndPaint(h, &ps);
        return 0;
    }

    case WM_ERASEBKGND: return 1;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g_capturingHotkey) { HandleHotkeyCaptureKey((UINT)w); return 0; }
        if (msg == WM_KEYDOWN && w == VK_ESCAPE) { RevertThemeIfNeeded(); DestroyWindow(h); return 0; }
        break;

    case WM_CHAR:
        if (g_capturingHotkey) return 0;
        break;

    case WM_DESTROY:
        g_capturingHotkey = false;
        CloseThemePopup();
        if (g_setFont) { DeleteObject(g_setFont); g_setFont = nullptr; }
        if (g_setFontTitle) { DeleteObject(g_setFontTitle); g_setFontTitle = nullptr; }
        if (g_setFontHint) { DeleteObject(g_setFontHint); g_setFontHint = nullptr; }
        if (g_setBgBrush) { DeleteObject(g_setBgBrush); g_setBgBrush = nullptr; }
        if (g_setEditBrush) { DeleteObject(g_setEditBrush); g_setEditBrush = nullptr; }
        g_noteStatic = nullptr;
        g_settingsHwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

static void OpenSettings() {
    if (g_settingsHwnd && IsWindow(g_settingsHwnd)) {
        SetForegroundWindow(g_settingsHwnd);
        return;
    }
    g_tglPinyin = g_showPinyin;
    g_tglAutoStart = g_autoStart;
    g_themePending = g_theme;
    g_themeOriginal = g_theme;
    g_tglFollowMouse = g_followMouse;
    g_settingsPage = 0;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hIcon = g_appIcon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kSettingsClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - 620) / 2;
    int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - 440) / 2;
    HWND h = CreateWindowExW(WS_EX_TOPMOST, kSettingsClass, L"设置",
        WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN, x, y, 620, 440, g_hwnd, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!h) return;
    g_settingsHwnd = h;
    // 窗口完全创建后再应用圆角：WM_CREATE 期间句柄尚未就绪，DWM 圆角 API
    // 可能失败而退回硬裁剪区域（有锯齿）。此时调用可确保 Win11 走系统抗锯齿圆角。
    ApplyWindowCorners(h, 620, 440, 12);
    ApplyWindowEffect(); // 设置窗口打开时即同步当前主题材料与透明度

    const wchar_t* navText[] = { L"常规", L"搜索", L"目录", L"关于" };
    for (int i = 0; i < 4; ++i) {
        CreateWindowExW(0, L"Button", navText[i], WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                        0, 72 + i * 32, 150, 32, h, (HMENU)(INT_PTR)(ID_NAV_GENERAL + i), nullptr, nullptr);
    }

    DWORD btn = WS_CHILD | WS_VISIBLE | BS_OWNERDRAW;
    HWND hk = CreateWindowExW(0, L"Button", HotkeyDisplay().c_str(), btn,
                              186, 102, 220, 30, h, (HMENU)ID_HOTKEY_EDIT, nullptr, nullptr);
    CreateWindowExW(0, L"Button", L"禁用", btn,
                    414, 102, 50, 30, h, (HMENU)ID_HOTKEY_DISABLE, nullptr, nullptr);

    // 同样用 ES_MULTILINE|ES_AUTOHSCROLL 模拟单行，支持 EM_SETRECT 精确居中。
    DWORD edit = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOHSCROLL;
    WCHAR buf[32];
    swprintf(buf, 32, L"%d", g_maxResults);
    HWND mx = CreateWindowExW(0, L"Edit", buf, edit,
                              300, 100, 64, 28, h, (HMENU)ID_MAX_EDIT, nullptr, nullptr);

    CreateWindowExW(0, L"Button", L"", btn, 186, 182, 32, 18, h, (HMENU)ID_AUTOSTART_TGL, nullptr, nullptr);
    CreateWindowExW(0, L"Button", L"", btn, 186, 140, 32, 18, h, (HMENU)ID_PINYIN_TGL, nullptr, nullptr);

    // 常规页第三张卡片：主题按钮（点击弹出完全自绘的浮层菜单）
    CreateWindowExW(0, L"Button", L"", btn, 186, 272, 96, 28,
                    h, (HMENU)ID_THEME_COMBO, nullptr, nullptr);
    CreateWindowExW(0, L"Button", L"", btn, 186, 318, 32, 18, h, (HMENU)ID_FOLLOWMOUSE_TGL, nullptr, nullptr);

    std::wstring paths;
    for (size_t i = 0; i < g_extraPaths.size(); ++i) {
        if (i) paths += L"\n";
        paths += g_extraPaths[i];
    }
    HWND pe = CreateWindowExW(0, L"Edit", paths.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
        186, 102, 398, 96, h, (HMENU)ID_PATHS_EDIT, nullptr, nullptr);

    CreateWindowExW(0, L"Button", L"+ 添加目录", btn, 186, 208, 104, 24, h, (HMENU)ID_BROWSE, nullptr, nullptr);
    CreateWindowExW(0, L"Button", L"重建索引", btn, 298, 208, 104, 24, h, (HMENU)ID_REBUILD, nullptr, nullptr);

    g_noteStatic = CreateWindowExW(0, L"Static", L"保存后立即生效", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   168, 400, 250, 28, h, (HMENU)ID_NOTE, nullptr, nullptr);
    CreateWindowExW(0, L"Button", L"取消", btn, 430, 400, 74, 28, h, (HMENU)ID_CANCEL, nullptr, nullptr);
    CreateWindowExW(0, L"Button", L"保存", btn, 512, 400, 74, 28, h, (HMENU)ID_SAVE, nullptr, nullptr);
    CreateWindowExW(0, L"Button", L"×", btn, 582, 12, 26, 26, h, (HMENU)ID_CLOSE, nullptr, nullptr);

    SendMessageW(hk, WM_SETFONT, (WPARAM)g_setFont, TRUE);
    SendMessageW(mx, WM_SETFONT, (WPARAM)g_setFont, TRUE);
    // 用 EM_SETRECT 精确居中"结果数量"数值（ES_MULTILINE 模式才生效）。
    CenterEditText(mx, g_setFont);
    SendMessageW(pe, WM_SETFONT, (WPARAM)g_setFont, TRUE);
    SendMessageW(g_noteStatic, WM_SETFONT, (WPARAM)g_setFontHint, TRUE);

    ShowSettingsPage(0);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    SetFocus(h);
}

// ---------------------------------------------------------------- window proc
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = h;
        MakeFonts();
        ApplyPalette(g_theme);
        RebuildThemeBrushes();
        ApplyWindowEffect();
        // ES_MULTILINE + ES_AUTOHSCROLL 模拟单行：外观仍是单行水平输入，
        // 但支持 EM_SETRECT，才能把文本精确垂直居中（单行 Edit 不支持该消息）。
        g_edit = CreateWindowExW(0, L"Edit", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOHSCROLL | ES_NOHIDESEL,
            52, 20, g_w - 160, 28, h, (HMENU)1, nullptr, nullptr);
        SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_fontEdit, TRUE);
        SendMessageW(g_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        CenterEditText(g_edit, g_fontEdit);
        SetWindowSubclass(g_edit, EditProc, 1, 0);
        UpdateHeaderLayout();
        {
            SetWindowLongPtrW(h, GWL_EXSTYLE,
                GetWindowLongPtrW(h, GWL_EXSTYLE) | WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
        }
        ApplyWindowEffect(); // 应用 DWM 材料与深色属性
        ResizeBuffer(g_w, g_h);
        break;

    case WM_NCCALCSIZE:
        // 保留 WS_THICKFRAME 框架语义供 DWM 画抗锯齿圆角/阴影，
        // 但把可见的 8px 系统边框从客户区里去掉，保证自绘内容铺满窗口。
        return 0;

    case WM_NCACTIVATE:
        // 打开设置窗口后本窗失焦，系统默认会用非激活配色画框架边框 →
        // 深色界面下出现明显白边。强制按激活态绘制，消除失焦白边。
        return TRUE;

    case WM_NCHITTEST:
        // 固定尺寸启动器：把框架的四边/四角命中都当作客户区，
        // 避免鼠标移到边缘出现 resize 光标或拖动改变窗口大小。
        return HTCLIENT;

    case WM_SIZE:
        break;

    case WM_SETTINGCHANGE:
        // 跟随系统模式下，用户切换 Windows 深/浅色时实时同步。
        if (g_theme == THEME_SYSTEM) {
            ApplyPalette(g_theme);
            RebuildThemeBrushes();
            ApplyWindowEffect();
            InvalidateRect(h, nullptr, TRUE);
            if (g_settingsHwnd) {
                InvalidateRect(g_settingsHwnd, nullptr, TRUE);
                RedrawWindow(g_settingsHwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            }
        }
        break;

    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)w, g_pal.editText);
        SetBkColor((HDC)w, g_pal.headBrush);
        return (LRESULT)g_headBrush;

    case WM_COMMAND:
        if (HIWORD(w) == 0) {
            switch (LOWORD(w)) {
            case TRAY_SHOW:
                if (IsWindowVisible(h)) HideWindow(); else ShowWindowFast();
                return 0;
            case TRAY_REINDEX: StartIndex(); return 0;
            case TRAY_AUTOSTART: SetAutoStart(!g_autoStart); return 0;
            case TRAY_SETTINGS:
                OpenSettings();
                return 0;
            case TRAY_EXIT: DestroyWindow(h); return 0;
            }
        }
        if (HIWORD(w) == EN_CHANGE && (HWND)l == g_edit) {
            if (g_editNormalizing) return 0;
            WCHAR buf[512];
            Edit_GetText(g_edit, buf, 512);
            // 多行模式下粘贴可能带入 \r\n；搜索框只允许单行，去掉换行后再写回。
            std::wstring clean;
            clean.reserve(512);
            for (WCHAR* p = buf; *p; ++p)
                if (*p != L'\r' && *p != L'\n') clean.push_back(*p);
            if (clean.size() != wcslen(buf)) {
                g_editNormalizing = true;
                Edit_SetText(g_edit, clean.c_str());
                Edit_SetSel(g_edit, clean.size(), clean.size());
                g_editNormalizing = false;
            }
            g_query = clean;
            g_sel = 0;
            UpdateResults();
            // 文本变化时让编辑框整体重绘，彻底清掉空态占位文字
            InvalidateRect(g_edit, nullptr, TRUE);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        if (g_memDC && g_memBmp) {
            DrawUI(g_memDC);
            BitBlt(ps.hdc, 0, 0, g_w, g_h, g_memDC, 0, 0, SRCCOPY);
        }
        EndPaint(h, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_APP_NAV:
        Navigate((int)w);
        return 0;

    case WM_APP_LAUNCH:
        LaunchSelected(w != 0, false);
        return 0;

    case WM_APP_OPENFOLDER:
        LaunchSelected(false, true);
        return 0;

    case WM_APP_COPYPATH: {
        bool isCalc = !g_results.empty() && g_results[g_sel].index < 0;
        if (CopySelected(true)) {
            g_toast = isCalc ? L"已复制计算结果" : L"已复制文件路径";
            if (g_toastTimer) KillTimer(h, g_toastTimer);
            g_toastTimer = SetTimer(h, 1, 1600, nullptr);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }

    case WM_TIMER:
        if (w == 1) {
            g_toast.clear();
            if (g_toastTimer) { KillTimer(h, g_toastTimer); g_toastTimer = 0; }
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;

    case WM_APP_HIDE:
        HideWindow();
        return 0;

    case WM_APP_SHOW:
        ShowWindowFast();
        return 0;

    case WM_APP_REINDEX:
        StartIndex();
        return 0;

    case WM_APP_INDEXED: {
        UpdateResults();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_HOTKEY:
        if (g_settingsHwnd && IsWindowVisible(g_settingsHwnd)) return 0;
        if (IsWindowVisible(h)) HideWindow();
        else ShowWindowFast();
        return 0;

    case WM_APP_TRAY:
        if (LOWORD(l) == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, TRAY_SHOW, L"\u663e\u793a / \u9690\u85cf");
            AppendMenuW(m, MF_STRING, TRAY_REINDEX, L"\u91cd\u65b0\u7d22\u5f15");
            AppendMenuW(m, MF_STRING | (g_autoStart ? MF_CHECKED : 0), TRAY_AUTOSTART, L"\u5f00\u673a\u81ea\u542f");
            AppendMenuW(m, MF_STRING, TRAY_SETTINGS, L"\u8bbe\u7f6e");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, TRAY_EXIT, L"\u9000\u51fa");
            SetForegroundWindow(h);
            TrackPopupMenu(m, TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, h, nullptr);
            PostMessageW(h, WM_NULL, 0, 0);
            DestroyMenu(m);
        } else if (LOWORD(l) == WM_LBUTTONDBLCLK) {
            ShowWindowFast();
        }
        return 0;

    case WM_MOUSEMOVE: {
        int my = GET_Y_LPARAM(l);
        int hover = -1;
        {
            std::lock_guard<std::mutex> lock(g_itemsMu);
            if (my >= 80) {
                int row = (my - 80) / 50;
                if (row >= 0 && row < (int)g_results.size()) hover = row;
            }
        }
        if (hover != g_hover) {
            g_hover = hover;
            InvalidateRect(h, nullptr, FALSE);
        }
        if (!g_trackingMouse) {
            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = h;
            TrackMouseEvent(&tme);
            g_trackingMouse = true;
        }
        break;
    }

    case WM_MOUSELEAVE:
        g_trackingMouse = false;
        if (g_hover != -1) {
            g_hover = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        break;

    case WM_LBUTTONDOWN: {
        int mx = GET_X_LPARAM(l), my = GET_Y_LPARAM(l);
        if (my < 68) {
            if (my >= 20 && my <= 54 && mx >= g_gearX && mx <= g_gearX + 28) OpenSettings();
            return 0;
        }
        std::lock_guard<std::mutex> lock(g_itemsMu);
        int y = my;
        int row = (y - 80) / 50;
        if (row >= 0 && row < (int)g_results.size()) {
            g_sel = row;
            InvalidateRect(h, nullptr, FALSE);
        } else {
            HideWindow();
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        int y = GET_Y_LPARAM(l);
        int row = (y - 80) / 50;
        bool hit = false;
        {
            std::lock_guard<std::mutex> lock(g_itemsMu);
            hit = (row >= 0 && row < (int)g_results.size());
            if (hit) g_sel = row;
        }
        if (hit) {
            g_sel = row;
            LaunchSelected(false, false);
        }
        return 0;
    }

    case WM_ACTIVATEAPP:
        if (!w) HideWindow();
        break;

    case WM_SYSCOMMAND:
        if ((w & 0xFFF0) == SC_CLOSE) { HideWindow(); return 0; }
        break;

    case WM_DESTROY:
        g_running = false;
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        SaveHistory();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(h, msg, w, l);
}

// ---------------------------------------------------------------- entry
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    HANDLE mtx = CreateMutexW(nullptr, TRUE, kMutex);
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(kClass, nullptr);
        if (other) PostMessageW(other, WM_APP_SHOW, 0, 0);
        CloseHandle(mtx);
        return 0;
    }

    HMODULE u32 = LoadLibraryW(L"user32.dll");
    if (u32) {
        typedef BOOL (WINAPI *SetDpiCtxFn)(void*);
        SetDpiCtxFn fnCtx = (SetDpiCtxFn)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (fnCtx) {
            fnCtx((void*)-4);  // PER_MONITOR_AWARE_V2
        } else {
            typedef BOOL (WINAPI *SetDpiFn)(void);
            SetDpiFn fn = (SetDpiFn)GetProcAddress(u32, "SetProcessDPIAware");
            if (fn) fn();
        }
        FreeLibrary(u32);
    }
    InitCommonControls();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    {
        Gdiplus::GdiplusStartupInput gdiStart;
        Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiStart, nullptr);
    }
    LoadConfig();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, L"APP_ICON");
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    g_appIcon = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    g_w = std::min(660, sw - 80);
    g_h = std::min(520, sh - 120);
    HWND hwnd = CreateWindowExW(0, kClass, L"FlashLaunch",
        WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN, 0, 0, g_w, g_h, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    ApplyWindowCorners(hwnd, g_w, g_h, 16);

    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_appIcon;
    wcsncpy(g_nid.szTip, L"FlashLaunch \u95ea\u542f", 63);
    g_nid.szTip[63] = 0;
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    bool fallback = false;
    bool registered = false;
    if (g_hotkeyEnabled) registered = RegisterOurHotkey(g_hotkeyMod, g_hotkeyVK, fallback);

    UpdateResults();
    StartIndex();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (registered) UnregisterHotKey(hwnd, 1);
    if (g_gdiplusToken) Gdiplus::GdiplusShutdown(g_gdiplusToken);
    CoUninitialize();
    if (mtx) CloseHandle(mtx);
    return 0;
}
