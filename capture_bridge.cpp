/*
 * External Display Bridge v3.0 — C++ / DirectX 11
 *
 * Ключевые оптимизации:
 *   - Нет cv::cvtColor: сырые BGR данные идут напрямую в GPU
 *   - GPU Swizzling: BGR→RGB перестановка в HLSL пиксельном шейдере
 *   - Zero-Copy Upload: D3D11_MAP_WRITE_DISCARD, только добавляем alpha=255
 *   - Triple Buffering: атомарный свап без мьютексов
 *   - Адаптивный рендерер: автопересоздание текстуры при смене разрешения
 *   - FPS оверлей без .clone(): рисуем прямо в буфер
 *   - MMCSS "Pro Audio" / "Games", REALTIME_PRIORITY_CLASS
 *   - Интерактивный выбор устройства при запуске
 *   - Настраиваемые клавиши управления (сохранение в keybindings.bin)
 *
 * Сборка (x64 Developer Command Prompt):
 *
 *   cd C:\Users\Кирилл\Downloads
 *   cl /O2 /EHsc /std:c++17 capture_bridge.cpp ^
 *      /I"C:\Users\Кирилл\Downloads\opencv\build\include" ^
 *      /link /LIBPATH:"C:\Users\Кирилл\Downloads\opencv\build\x64\vc16\lib" ^
 *      opencv_world4120.lib d3d11.lib dxgi.lib d3dcompiler.lib avrt.lib ^
 *      user32.lib kernel32.lib ole32.lib oleaut32.lib strmiids.lib
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define _WIN32_DCOM
#define NOMINMAX

#include <windows.h>
#include <dshow.h>
#include <comdef.h>
#include <avrt.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>

#include <atomic>
#include <array>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <algorithm>
#include <vector>
#include <fstream>
#include <cstring>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

// ─── Глобальные флаги ────────────────────────────────────────────────────────

static std::atomic<bool> g_running { true  };
static std::atomic<bool> g_showFPS  { false };
static std::atomic<bool> g_vsync    { false };

// ─── UI helpers ──────────────────────────────────────────────────────────────
// Все блоки шириной 50 символов внутри (52 с рамкой), отступ 2 пробела слева.
// Content-строки — чистый ASCII, поэтому size() == display width.

static const char* UI_TOP = "  ╔══════════════════════════════════════════════════╗";
static const char* UI_SEP = "  ╠══════════════════════════════════════════════════╣";
static const char* UI_BOT = "  ╚══════════════════════════════════════════════════╝";
static const int   UI_W   = 50;

// Возвращает количество кодовых точек UTF-8 (= display-ширину для BMP символов).
// c.size() считает байты: многобайтные символы (╠, —, █ и т.д.) дают лишние байты,
// из-за чего padding получается меньше нужного.
// UTF-8: байты 0x80-0xBF — continuation bytes, не считаем их.
static int utf8Len(const std::string& s) {
    int n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n; // считаем только leading bytes
    return n;
}

// Строка с левым отступом 2 пробела
static void uiLine(const std::string& s) {
    std::string c = "  " + s;
    int dw = utf8Len(c);
    while (dw < UI_W) { c += ' '; ++dw; }
    // Если строка слишком длинная — обрезаем по display-ширине
    if (dw > UI_W) {
        int keep = 0, bytes = 0;
        for (unsigned char ch : c) {
            if ((ch & 0xC0) != 0x80) { if (keep >= UI_W) break; ++keep; }
            ++bytes;
        }
        c = c.substr(0, bytes);
    }
    std::cout << "  ║" << c << "║\n";
}

// Центрированная строка
static void uiCenter(const std::string& s) {
    int sw = utf8Len(s);
    int lp = (UI_W - sw) / 2;
    if (lp < 0) lp = 0;
    int rp = UI_W - lp - sw;
    if (rp < 0) rp = 0;
    std::cout << "  ║"
              << std::string(lp, ' ') << s << std::string(rp, ' ')
              << "║\n";
}

// Пустая строка внутри блока
static void uiBlank() {
    std::cout << "  ║" << std::string(UI_W, ' ') << "║\n";
}

// ─── Стартовый баннер ────────────────────────────────────────────────────────

static void printBanner()
{
    // FIGlet-стиль «EDB», 24 символа шириной, центр в 50 → 13 слева
    // Строки 1 и 6 шириной 23 → 13 слева, 14 справа
    // Строки 2-5 шириной 24 → 13 слева, 13 справа
    static const char* art[] = {
        "             ███████╗██████╗ ██████╗              ",
        "             ██╔════╝██╔══██╗██╔══██╗             ",
        "             █████╗  ██║  ██║██████╔╝             ",
        "             ██╔══╝  ██║  ██║██╔══██╗             ",
        "             ███████╗██████╔╝██████╔╝             ",
        "             ╚══════╝╚═════╝ ╚═════╝              ",
    };

    std::cout << "\n" << UI_TOP << "\n";
    uiBlank();
    for (auto& row : art)
        std::cout << "  ║" << row << "║\n";
    uiBlank();
    // "External  Display  Bridge" — 25 символов, lp=12 rp=13
    std::cout << "  ║" << "            External  Display  Bridge             " << "║\n";
    // "v3.0" — 4 символа, lp=23 rp=23
    std::cout << "  ║" << "                       v3.0                       " << "║\n";
    uiBlank();
    // Технологии — ровно 50 символов
    std::cout << "  ║" << " DirectX 11  |  Triple Buffer  |  MMCSS  |  YUY2  " << "║\n";
    std::cout << UI_BOT << "\n\n";
}


// ─── Настройки клавиш ────────────────────────────────────────────────────────

static const char*     KEYBIND_FILE    = "keybindings.bin";
static const DWORD     KB_MAGIC        = 0x4B425633; // "KBV3"

struct KeyBindings {
    DWORD magic   = KB_MAGIC;
    int   vkFPS   = 'F';
    int   vkVSync = 'V';
    int   vkExit  = VK_ESCAPE;
};

static bool saveKeyBindings(const KeyBindings& kb)
{
    std::ofstream f(KEYBIND_FILE, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(&kb), sizeof(kb));
    return f.good();
}

static KeyBindings loadKeyBindings()
{
    KeyBindings kb;
    std::ifstream f(KEYBIND_FILE, std::ios::binary);
    if (!f) return kb;
    KeyBindings tmp;
    f.read(reinterpret_cast<char*>(&tmp), sizeof(tmp));
    if (f.gcount() == sizeof(tmp) && tmp.magic == KB_MAGIC)
        kb = tmp;
    return kb;
}

// ─── Человекочитаемые имена VK-кодов ─────────────────────────────────────────

static std::string vkToString(int vk)
{
    // Mouse buttons
    switch (vk) {
    case VK_LBUTTON:  return "Mouse Left";
    case VK_RBUTTON:  return "Mouse Right";
    case VK_MBUTTON:  return "Mouse Middle";
    case VK_XBUTTON1: return "Mouse X1 (Back)";
    case VK_XBUTTON2: return "Mouse X2 (Forward)";
    case VK_BACK:     return "Backspace";
    case VK_TAB:      return "Tab";
    case VK_RETURN:   return "Enter";
    case VK_ESCAPE:   return "Escape";
    case VK_SPACE:    return "Space";
    case VK_PRIOR:    return "Page Up";
    case VK_NEXT:     return "Page Down";
    case VK_END:      return "End";
    case VK_HOME:     return "Home";
    case VK_LEFT:     return "Left";
    case VK_UP:       return "Up";
    case VK_RIGHT:    return "Right";
    case VK_DOWN:     return "Down";
    case VK_INSERT:   return "Insert";
    case VK_DELETE:   return "Delete";
    case VK_CAPITAL:  return "Caps Lock";
    case VK_SHIFT:    return "Shift";
    case VK_CONTROL:  return "Ctrl";
    case VK_MENU:     return "Alt";
    case VK_PAUSE:    return "Pause";
    case VK_SNAPSHOT: return "Print Screen";
    case VK_SCROLL:   return "Scroll Lock";
    case VK_NUMLOCK:  return "Num Lock";
    case VK_LWIN:     return "Left Win";
    case VK_RWIN:     return "Right Win";
    case VK_APPS:     return "Menu";
    // F keys
    case VK_F1:  return "F1";  case VK_F2:  return "F2";
    case VK_F3:  return "F3";  case VK_F4:  return "F4";
    case VK_F5:  return "F5";  case VK_F6:  return "F6";
    case VK_F7:  return "F7";  case VK_F8:  return "F8";
    case VK_F9:  return "F9";  case VK_F10: return "F10";
    case VK_F11: return "F11"; case VK_F12: return "F12";
    case VK_F13: return "F13"; case VK_F14: return "F14";
    case VK_F15: return "F15"; case VK_F16: return "F16";
    case VK_F17: return "F17"; case VK_F18: return "F18";
    case VK_F19: return "F19"; case VK_F20: return "F20";
    case VK_F21: return "F21"; case VK_F22: return "F22";
    case VK_F23: return "F23"; case VK_F24: return "F24";
    // Numpad
    case VK_NUMPAD0: return "Num 0"; case VK_NUMPAD1: return "Num 1";
    case VK_NUMPAD2: return "Num 2"; case VK_NUMPAD3: return "Num 3";
    case VK_NUMPAD4: return "Num 4"; case VK_NUMPAD5: return "Num 5";
    case VK_NUMPAD6: return "Num 6"; case VK_NUMPAD7: return "Num 7";
    case VK_NUMPAD8: return "Num 8"; case VK_NUMPAD9: return "Num 9";
    case VK_MULTIPLY: return "Num *"; case VK_ADD:      return "Num +";
    case VK_SUBTRACT: return "Num -"; case VK_DECIMAL:  return "Num .";
    case VK_DIVIDE:   return "Num /";
    // Media / browser / app keys
    case VK_BROWSER_BACK:       return "Browser Back";
    case VK_BROWSER_FORWARD:    return "Browser Forward";
    case VK_BROWSER_REFRESH:    return "Browser Refresh";
    case VK_BROWSER_STOP:       return "Browser Stop";
    case VK_BROWSER_SEARCH:     return "Browser Search";
    case VK_BROWSER_FAVORITES:  return "Browser Favorites";
    case VK_BROWSER_HOME:       return "Browser Home";
    case VK_VOLUME_MUTE:        return "Volume Mute";
    case VK_VOLUME_DOWN:        return "Volume Down";
    case VK_VOLUME_UP:          return "Volume Up";
    case VK_MEDIA_NEXT_TRACK:   return "Media Next";
    case VK_MEDIA_PREV_TRACK:   return "Media Prev";
    case VK_MEDIA_STOP:         return "Media Stop";
    case VK_MEDIA_PLAY_PAUSE:   return "Media Play/Pause";
    // OEM punctuation — use GetKeyNameText for these
    default: break;
    }

    // Printable ASCII (letters and digits)
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
        return std::string(1, static_cast<char>(vk));

    // Fallback: use GetKeyNameText (requires scan code)
    UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    if (scan) {
        wchar_t buf[64] = {};
        LONG lParam = static_cast<LONG>(scan << 16);
        if (GetKeyNameTextW(lParam, buf, 64) > 0) {
            int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
            std::string s(len, '\0');
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, &s[0], len, nullptr, nullptr);
            if (!s.empty()) s.pop_back();
            return s;
        }
    }

    char hex[16];
    snprintf(hex, sizeof(hex), "VK 0x%02X", vk);
    return hex;
}

// ─── Захват любой клавиши/кнопки мыши ───────────────────────────────────────
//
// GetAsyncKeyState — единственный надёжный способ поймать VK_XBUTTON1/2.
// ReadConsoleInput не передаёт XButton-события (ограничение Win32 Console API).
//
// Алгоритм:
//   1. Sleep(200) — даём «хвосту» предыдущего Enter гарантированно уйти.
//   2. Сначала мышь (5 кнопок), потом клавиатура 0x08-0xDE,
//      пропуская зарезервированные диапазоны Microsoft.
//   3. Sleep(100) после срабатывания — дебаунс, исключает двойную регистрацию.
//   4. Sleep(10) между итерациями — CPU < 0.1%, на видео не влияет.

static int captureAnyKey()
{
    Sleep(200); // ждём пока отпустится предыдущая клавиша (Enter / Y)

    while (true) {
        // Мышь — проверяем первой, включая боковые кнопки X1/X2
        for (int vk : { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 }) {
            if (GetAsyncKeyState(vk) & 0x8000) {
                Sleep(100); // дебаунс
                return vk;
            }
        }

        // Клавиатура: диапазон 0x08-0xDE, зарезервированные диапазоны пропускаем
        for (int vk = 0x08; vk <= 0xDE; ++vk) {
            // Зарезервировано / undefined по таблице Microsoft Virtual-Key Codes
            if (vk == 0x0A || vk == 0x0B || vk == 0x0E || vk == 0x0F) continue;
            if (vk >= 0x3A && vk <= 0x40) continue; // undefined
            if (vk >= 0x5B && vk <= 0x5F) continue; // Win keys + reserved
            if (vk >= 0x88 && vk <= 0x8F) continue; // unassigned
            if (vk >= 0x97 && vk <= 0x9F) continue; // unassigned
            if (vk >= 0xB8 && vk <= 0xB9) continue; // reserved
            if (vk >= 0xC1 && vk <= 0xC2) continue; // reserved
            // В диапазоне 0xC3-0xDA оставляем только F1-F12, остальное — OEM/reserved
            if (vk >= 0xC3 && vk <= 0xDA &&
                vk != VK_F1  && vk != VK_F2  && vk != VK_F3  && vk != VK_F4  &&
                vk != VK_F5  && vk != VK_F6  && vk != VK_F7  && vk != VK_F8  &&
                vk != VK_F9  && vk != VK_F10 && vk != VK_F11 && vk != VK_F12) continue;

            if (GetAsyncKeyState(vk) & 0x8000) {
                Sleep(100); // дебаунс
                return vk;
            }
        }

        Sleep(10);
    }
}

// ─── Консольный UI настройки клавиш ─────────────────────────────────────────

static KeyBindings configureKeys(KeyBindings current)
{
    std::cout << "\n" << UI_TOP << "\n";
    uiCenter("Key Binding Configuration");
    std::cout << UI_SEP << "\n";
    uiLine("Keyboard, mouse side buttons (X1/X2) and all");
    uiLine("keys from the Windows Virtual-Key table.");
    std::cout << UI_SEP << "\n";

    struct Action {
        const char* label;
        int*        vk;
    };

    Action actions[] = {
        { "Toggle FPS overlay",     &current.vkFPS   },
        { "Toggle VSync on/off",    &current.vkVSync },
        { "Exit the program",       &current.vkExit  },
    };
    const int N = 3;

    for (int i = 0; i < N; ++i) {
        // Показываем текущую привязку
        std::cout << "  [" << (i+1) << "/" << N << "] "
                  << actions[i].label << "\n"
                  << "        Current: " << vkToString(*actions[i].vk) << "\n"
                  << "        Press any key or mouse button... ";
        std::cout.flush();

        bool accepted = false;
        while (!accepted) {
            int vk = captureAnyKey();

            // Проверяем дублирование: та же клавиша уже назначена другому действию?
            bool duplicate = false;
            for (int j = 0; j < N; ++j) {
                if (j == i) continue;
                if (*actions[j].vk == vk) {
                    duplicate = true;
                    std::cout << "\n        [!] \"" << vkToString(vk)
                              << "\" is already assigned to ["
                              << (j+1) << "/" << N << "] " << actions[j].label
                              << ".\n"
                              << "        Press a different key... ";
                    std::cout.flush();
                    break;
                }
            }

            if (!duplicate) {
                *actions[i].vk = vk;
                std::cout << vkToString(vk) << "\n\n";
                accepted = true;
            }
        }
    }

    std::cout << UI_BOT << "\n\n";
    return current;
}

// ─── Запрос на настройку клавиш при старте ───────────────────────────────────

static KeyBindings startupKeySetup()
{
    KeyBindings kb = loadKeyBindings();

    // ── Key Bindings box ─────────────────────────────────────────────────
    std::cout << "\n" << UI_TOP << "\n";
    uiCenter("Key Bindings");
    std::cout << UI_SEP << "\n";
    uiLine("FPS overlay  :  " + vkToString(kb.vkFPS));
    uiLine("VSync        :  " + vkToString(kb.vkVSync));
    uiLine("Exit         :  " + vkToString(kb.vkExit));
    std::cout << UI_SEP << "\n";
    uiLine("To reset defaults — delete \"keybindings.bin\"");
    std::cout << UI_BOT << "\n";
    std::cout << "\n  Change key bindings? [Y / N]: ";
    std::cout.flush();

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    FlushConsoleInputBuffer(hIn);
    char buf[8] = {};
    DWORD nRead = 0;
    ReadConsoleA(hIn, buf, sizeof(buf) - 1, &nRead, nullptr);

    char ch = (nRead > 0) ? static_cast<char>(toupper(buf[0])) : 'N';
    if (ch == 'Y') {
        kb = configureKeys(kb);
        if (saveKeyBindings(kb))
            std::cout << "  [OK] Key bindings saved to " << KEYBIND_FILE << "\n";
        else
            std::cout << "  [WARN] Could not save key bindings.\n";
    }

    std::cout << "\n" << UI_TOP << "\n";
    uiCenter("Active Key Bindings");
    std::cout << UI_SEP << "\n";
    uiLine("FPS overlay  :  " + vkToString(kb.vkFPS));
    uiLine("VSync        :  " + vkToString(kb.vkVSync));
    uiLine("Exit         :  " + vkToString(kb.vkExit));
    std::cout << UI_BOT << "\n\n";

    return kb;
}

// ─── Питание / курсор ────────────────────────────────────────────────────────

static void preventSleep() { SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED); }
static void allowSleep()   { SetThreadExecutionState(ES_CONTINUOUS); }
static void hideCursor()   { while (ShowCursor(FALSE) >= 0) {} }
static void showCursor()   { while (ShowCursor(TRUE)  <  0) {} }

// ─── Приоритеты ──────────────────────────────────────────────────────────────

static void setProcessPriority()
{
    HANDLE h = GetCurrentProcess();
    if (!SetPriorityClass(h, REALTIME_PRIORITY_CLASS))
        SetPriorityClass(h, HIGH_PRIORITY_CLASS);
}

static HANDLE registerMMCSS(const wchar_t* task)
{
    DWORD idx = 0;
    HANDLE h = AvSetMmThreadCharacteristicsW(task, &idx);
    return h;
}

// ─── Перечисление и выбор устройства ─────────────────────────────────────────

struct DeviceInfo {
    int         index;
    std::string name;
};

static std::vector<DeviceInfo> enumerateDevices()
{
    std::vector<DeviceInfo> result;

    HRESULT hr = S_OK;
    (void)hr;

    ICreateDevEnum* devEnum = nullptr;
    CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                     IID_ICreateDevEnum, reinterpret_cast<void**>(&devEnum));
    if (!devEnum) { CoUninitialize(); return result; }

    IEnumMoniker* enumMon = nullptr;
    devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMon, 0);
    devEnum->Release();
    if (!enumMon) { CoUninitialize(); return result; }

    IMoniker* moniker = nullptr;
    int index = 0;

    while (enumMon->Next(1, &moniker, nullptr) == S_OK) {
        IPropertyBag* propBag = nullptr;
        if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_IPropertyBag,
                                              reinterpret_cast<void**>(&propBag)))) {
            VARIANT var; VariantInit(&var);
            if (SUCCEEDED(propBag->Read(L"FriendlyName", &var, nullptr))) {
                int len = WideCharToMultiByte(CP_UTF8, 0, var.bstrVal, -1,
                                              nullptr, 0, nullptr, nullptr);
                std::string name(len, '\0');
                WideCharToMultiByte(CP_UTF8, 0, var.bstrVal, -1,
                                    &name[0], len, nullptr, nullptr);
                if (!name.empty()) name.pop_back();
                result.push_back({ index, name });
                VariantClear(&var);
            }
            propBag->Release();
        }
        moniker->Release();
        ++index;
    }
    enumMon->Release();
    return result;
}

static int selectDevice()
{
    auto devices = enumerateDevices();

    if (devices.empty()) {
        std::cerr << "[ERROR] No video devices found.\n";
        return -1;
    }

    std::cout << "\n" << UI_TOP << "\n";
    uiCenter("Video Devices");
    std::cout << UI_SEP << "\n";
    for (auto& d : devices) {
        std::string entry = "[" + std::to_string(d.index) + "]  " + d.name;
        uiLine(entry);
    }
    std::cout << UI_BOT << "\n";

    if (devices.size() == 1) {
        std::cout << "\n  [AUTO] Only one device — selecting: "
                  << devices[0].name << "\n\n";
        return devices[0].index;
    }

    std::cout << "\n  Select device [0-" << (devices.size()-1) << "]: ";
    std::cout.flush();

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    FlushConsoleInputBuffer(hIn);

    char buf[16] = {};
    DWORD read = 0;
    ReadConsoleA(hIn, buf, sizeof(buf) - 1, &read, nullptr);

    int choice = -1;
    try { choice = std::stoi(std::string(buf)); } catch (...) {}

    for (auto& d : devices)
        if (d.index == choice) {
            std::cout << "  [OK] Selected: " << d.name << "\n\n";
            return d.index;
        }

    std::cout << "  [WARN] Invalid index, defaulting to 0\n\n";
    return devices[0].index;
}

// ─── Triple Buffer ────────────────────────────────────────────────────────────

struct TripleBuffer {
    std::array<cv::Mat, 3> bufs;
    std::atomic<int> latest  { -1 };
    std::atomic<int> writing {  0 };

    void commitWrite()
    {
        int w = writing.load(std::memory_order_relaxed);
        latest.store(w, std::memory_order_release);
        writing.store((w + 1) % 3, std::memory_order_relaxed);
    }

    cv::Mat* tryRead()
    {
        int idx = latest.load(std::memory_order_acquire);
        return (idx >= 0) ? &bufs[idx] : nullptr;
    }
};

// ─── Поток захвата ────────────────────────────────────────────────────────────

class VideoStream {
public:
    explicit VideoStream(int deviceId, TripleBuffer& tb)
        : tb_(tb), running_(true), width_(0), height_(0)
    {
        cap_.open(deviceId, cv::CAP_DSHOW);

        cap_.set(cv::CAP_PROP_FOURCC,     cv::VideoWriter::fourcc('Y','U','Y','2'));
        cap_.set(cv::CAP_PROP_FPS,        60);
        cap_.set(cv::CAP_PROP_BUFFERSIZE, 1);
        cap_.set(cv::CAP_PROP_FRAME_WIDTH,  1920);
        cap_.set(cv::CAP_PROP_FRAME_HEIGHT, 1080);

        width_  = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_WIDTH));
        height_ = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_HEIGHT));

        captureThread_ = std::thread(&VideoStream::captureLoop, this);
    }

    ~VideoStream() { stop(); }

    double get(int p)  const { return cap_.get(p); }
    int    width()     const { return width_;  }
    int    height()    const { return height_; }

    void stop()
    {
        running_ = false;
        if (captureThread_.joinable()) captureThread_.join();
        cap_.release();
    }

private:
    void captureLoop()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        HANDLE mmh = registerMMCSS(L"Pro Audio");

        while (running_) {
            int w = tb_.writing.load(std::memory_order_relaxed);
            bool ok = cap_.read(tb_.bufs[w]);
            if (ok && !tb_.bufs[w].empty())
                tb_.commitWrite();
        }
        if (mmh) AvRevertMmThreadCharacteristics(mmh);
    }

    cv::VideoCapture  cap_;
    TripleBuffer&     tb_;
    std::atomic<bool> running_;
    std::thread       captureThread_;
    int               width_, height_;
};

// ─── HLSL шейдеры ────────────────────────────────────────────────────────────

static const char* s_vsCode = R"(
struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
VS_OUT main(uint id : SV_VertexID) {
    float2 uv  = float2((id & 1) ? 2.0f : 0.0f, (id & 2) ? 2.0f : 0.0f);
    VS_OUT o;
    o.pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    o.uv  = uv;
    return o;
}
)";

static const char* s_psCode = R"(
Texture2D    tex : register(t0);
SamplerState sam : register(s0);
struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
float4 main(VS_OUT i) : SV_TARGET {
    float4 c = tex.Sample(sam, i.uv);
    return float4(c.b, c.g, c.r, 1.0f);
}
)";

// ─── DirectX 11 Renderer ─────────────────────────────────────────────────────

struct DX11Renderer {
    ID3D11Device*             device    = nullptr;
    ID3D11DeviceContext*      ctx       = nullptr;
    IDXGISwapChain1*          swapChain = nullptr;
    ID3D11RenderTargetView*   rtv       = nullptr;
    ID3D11VertexShader*       vs        = nullptr;
    ID3D11PixelShader*        ps        = nullptr;
    ID3D11Texture2D*          dynTex    = nullptr;
    ID3D11ShaderResourceView* srv       = nullptr;
    ID3D11SamplerState*       sampler   = nullptr;

    int  winW = 0, winH = 0;
    int  texW = 0, texH = 0;
    bool tearingOk = false;

    bool init(HWND hwnd, int w, int h)
    {
        winW = w; winH = h;

        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                     &fl, 1, D3D11_SDK_VERSION,
                                     &device, nullptr, &ctx))) {
            std::cerr << "[DX11] CreateDevice failed\n"; return false;
        }

        IDXGIFactory2* factory = nullptr;
        {
            IDXGIDevice*  dxgiDev = nullptr;
            IDXGIAdapter* adapter = nullptr;
            device->QueryInterface(__uuidof(IDXGIDevice),  reinterpret_cast<void**>(&dxgiDev));
            dxgiDev->GetAdapter(&adapter);
            adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
            adapter->Release(); dxgiDev->Release();
        }

        IDXGIFactory5* f5 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5),
                                              reinterpret_cast<void**>(&f5)))) {
            BOOL t = FALSE;
            f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &t, sizeof(t));
            tearingOk = (t == TRUE);
            f5->Release();
        }

        DXGI_SWAP_CHAIN_DESC1 scd = {};
        scd.Width       = w; scd.Height = h;
        scd.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.BufferCount = 2;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.SampleDesc  = { 1, 0 };
        scd.Flags       = tearingOk ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

        if (FAILED(factory->CreateSwapChainForHwnd(device, hwnd, &scd,
                                                    nullptr, nullptr, &swapChain))) {
            std::cerr << "[DX11] CreateSwapChain failed\n";
            factory->Release(); return false;
        }

        IDXGIFactory1* f1 = nullptr;
        swapChain->GetParent(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f1));
        if (f1) { f1->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER); f1->Release(); }
        factory->Release();

        if (!rebuildRTV()) return false;
        if (!compileShaders()) return false;

        D3D11_SAMPLER_DESC sd = {};
        sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        device->CreateSamplerState(&sd, &sampler);

        return true;
    }

    bool compileShaders()
    {
        ID3DBlob *blob = nullptr, *err = nullptr;

        D3DCompile(s_vsCode, strlen(s_vsCode), nullptr, nullptr, nullptr,
                   "main", "vs_5_0", 0, 0, &blob, &err);
        if (!blob) {
            std::cerr << "[DX11] VS: " << (err?(char*)err->GetBufferPointer():"?") << "\n";
            if (err) err->Release(); return false;
        }
        device->CreateVertexShader(blob->GetBufferPointer(),
                                   blob->GetBufferSize(), nullptr, &vs);
        blob->Release();

        D3DCompile(s_psCode, strlen(s_psCode), nullptr, nullptr, nullptr,
                   "main", "ps_5_0", 0, 0, &blob, &err);
        if (!blob) {
            std::cerr << "[DX11] PS: " << (err?(char*)err->GetBufferPointer():"?") << "\n";
            if (err) err->Release(); return false;
        }
        device->CreatePixelShader(blob->GetBufferPointer(),
                                  blob->GetBufferSize(), nullptr, &ps);
        blob->Release();
        return true;
    }

    bool rebuildRTV()
    {
        if (rtv) { rtv->Release(); rtv = nullptr; }
        ID3D11Texture2D* back = nullptr;
        swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                             reinterpret_cast<void**>(&back));
        if (!back) return false;
        device->CreateRenderTargetView(back, nullptr, &rtv);
        back->Release();
        return rtv != nullptr;
    }

    void ensureTexture(int w, int h)
    {
        if (texW == w && texH == h) return;

        if (srv)    { srv->Release();    srv    = nullptr; }
        if (dynTex) { dynTex->Release(); dynTex = nullptr; }

        D3D11_TEXTURE2D_DESC td = {};
        td.Width          = w; td.Height = h;
        td.MipLevels      = 1; td.ArraySize = 1;
        td.Format         = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc     = { 1, 0 };
        td.Usage          = D3D11_USAGE_DYNAMIC;
        td.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        if (FAILED(device->CreateTexture2D(&td, nullptr, &dynTex))) {
            std::cerr << "[DX11] CreateTexture2D failed (" << w << "x" << h << ")\n";
            return;
        }
        device->CreateShaderResourceView(dynTex, nullptr, &srv);
        texW = w; texH = h;
    }

    void uploadFrame(const cv::Mat& frame)
    {
        ensureTexture(frame.cols, frame.rows);
        if (!dynTex) return;

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(ctx->Map(dynTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;

        const uint8_t* src = frame.ptr(0);
        uint8_t*       dst = static_cast<uint8_t*>(mapped.pData);
        const int      W   = frame.cols;
        const int      H   = frame.rows;
        const int      srcStep = static_cast<int>(frame.step[0]);
        const UINT     dstPitch = mapped.RowPitch;

        thread_local cv::Mat bgraRow;
        for (int y = 0; y < H; ++y) {
            const cv::Mat srcRow(1, W, CV_8UC3, const_cast<uint8_t*>(src + y * srcStep));
            bgraRow.create(1, W, CV_8UC4);
            cv::cvtColor(srcRow, bgraRow, cv::COLOR_BGR2BGRA);
            memcpy(dst + y * dstPitch, bgraRow.ptr(0), W * 4);
        }
        ctx->Unmap(dynTex, 0);
    }

    void render()
    {
        if (!srv) return;

        float scaleX = (float)winW / (float)texW;
        float scaleY = (float)winH / (float)texH;
        float scale  = (std::min)(scaleX, scaleY);
        float vpW    = texW * scale;
        float vpH    = texH * scale;
        float vpX    = (winW - vpW) * 0.5f;
        float vpY    = (winH - vpH) * 0.5f;

        D3D11_VIEWPORT vp = { vpX, vpY, vpW, vpH, 0.0f, 1.0f };
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(1, &rtv, nullptr);

        float black[4] = { 0, 0, 0, 1 };
        ctx->ClearRenderTargetView(rtv, black);

        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->PSSetSamplers(0, 1, &sampler);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->Draw(3, 0);

        bool vsync = g_vsync.load();
        UINT flags = (!vsync && tearingOk) ? DXGI_PRESENT_ALLOW_TEARING : 0;
        swapChain->Present(vsync ? 1 : 0, flags);
    }

    void release()
    {
        if (sampler)   sampler->Release();
        if (srv)       srv->Release();
        if (dynTex)    dynTex->Release();
        if (ps)        ps->Release();
        if (vs)        vs->Release();
        if (rtv)       rtv->Release();
        if (swapChain) swapChain->Release();
        if (ctx)       ctx->Release();
        if (device)    device->Release();
    }
};

// ─── Win32 окно ──────────────────────────────────────────────────────────────

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        g_running = false;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND createFullscreenWindow(int& outW, int& outH)
{
    outW = GetSystemMetrics(SM_CXSCREEN);
    outH = GetSystemMetrics(SM_CYSCREEN);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BridgeWnd";
    wc.hCursor       = nullptr;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"BridgeWnd",
        L"External Display Bridge",
        WS_POPUP | WS_VISIBLE,
        0, 0, outW, outH,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    return hwnd;
}

// ─── FPS оверлей ─────────────────────────────────────────────────────────────

static void drawFPS(cv::Mat* frame, double fps, const char* codec)
{
    if (!frame || frame->empty()) return;
    char buf[80];
    const char* vsyncStr = g_vsync.load() ? "VSync ON" : "VSync OFF";
    snprintf(buf, sizeof(buf), "FPS: %d | %s | %s", (int)fps, codec, vsyncStr);
    cv::putText(*frame, buf,
                cv::Point(20, frame->rows - 20),
                cv::FONT_HERSHEY_SIMPLEX, 0.6,
                cv::Scalar(160, 160, 160), 1, cv::LINE_AA);
}

// ─── Опрос клавиш и кнопок мыши в главном цикле ─────────────────────────────
//
// GetAsyncKeyState работает для всех VK кодов, включая VK_XBUTTON1/2.
// Вызывается каждый кадр, но это ~1 мкс накладных расходов — нет влияния
// на задержку видео.

struct KeyState {
    bool wasDown = false;

    // Возвращает true только на передний фронт нажатия
    bool poll(int vk) {
        bool isDown = (GetAsyncKeyState(vk) & 0x8000) != 0;
        bool edge   = isDown && !wasDown;
        wasDown     = isDown;
        return edge;
    }
};

// ─── main ─────────────────────────────────────────────────────────────────────

int main()
{
    SetConsoleOutputCP(CP_UTF8);
    printBanner();
    setProcessPriority();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HANDLE mmh = registerMMCSS(L"Games");
    preventSleep();

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // ── Настройка клавиш ─────────────────────────────────────────────────────
    KeyBindings kb = startupKeySetup();

    // ── Выбор устройства ─────────────────────────────────────────────────────
    int deviceId = selectDevice();
    if (deviceId < 0) { allowSleep(); return 1; }

    TripleBuffer tb;
    VideoStream  vs(deviceId, tb);

    int    srcW      = vs.width();
    int    srcH      = vs.height();
    int    fourccInt = static_cast<int>(vs.get(cv::CAP_PROP_FOURCC));
    double srcFps    = vs.get(cv::CAP_PROP_FPS);

    char fourccStr[5] = {};
    for (int i = 0; i < 4; ++i)
        fourccStr[i] = static_cast<char>((fourccInt >> (8 * i)) & 0xFF);

    {
        std::string res = std::to_string(srcW) + " x " + std::to_string(srcH);
        std::string fps = std::to_string((int)srcFps);
        std::string keys = vkToString(kb.vkFPS)   + " = FPS  |  "
                         + vkToString(kb.vkVSync) + " = VSync  |  "
                         + vkToString(kb.vkExit)  + " = Exit";
        std::cout << "\n" << UI_TOP << "\n";
        uiCenter("Capture Device Info");
        std::cout << UI_SEP << "\n";
        uiLine("Resolution  :  " + res);
        uiLine(std::string("Codec       :  ") + fourccStr);
        uiLine("Target FPS  :  " + fps);
        if (std::string(fourccStr) != "YUY2")
            uiLine("[!] MJPG mode — extra 5-15ms decode delay");
        std::cout << UI_SEP << "\n";
        uiLine(keys);
        std::cout << UI_BOT << "\n\n";
    }

    // ── Окно + DirectX ───────────────────────────────────────────────────────
    int winW = 0, winH = 0;
    HWND hwnd = createFullscreenWindow(winW, winH);
    hideCursor();

    DX11Renderer dx;
    if (!dx.init(hwnd, winW, winH)) {
        std::cerr << "[ERROR] DX11 init failed.\n";
        vs.stop(); allowSleep(); return 1;
    }

    auto prevTime = std::chrono::steady_clock::now();

    // ── Состояния клавиш (защита от дребезга) ────────────────────────────────
    KeyState ksFPS, ksVSync, ksExit;

    // ── Главный цикл ─────────────────────────────────────────────────────────
    while (g_running) {
        // 1. Системные сообщения
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // 2. Опрос настроенных клавиш (передний фронт нажатия)
        //    GetAsyncKeyState поддерживает все VK, включая XBUTTONs.
        if (ksFPS.poll(kb.vkFPS))     g_showFPS = !g_showFPS.load();
        if (ksVSync.poll(kb.vkVSync)) g_vsync   = !g_vsync.load();
        if (ksExit.poll(kb.vkExit))   g_running = false;

        // 3. Захват и вывод кадра
        cv::Mat* framePtr = tb.tryRead();
        if (!framePtr || framePtr->empty()) { Sleep(1); continue; }

        auto   now = std::chrono::steady_clock::now();
        double fps = 1e9 / static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                now - prevTime).count());
        prevTime = now;

        if (g_showFPS)
            drawFPS(framePtr, fps, fourccStr);

        dx.uploadFrame(*framePtr);
        dx.render();
    }

    // ── Очистка ───────────────────────────────────────────────────────────────
    dx.release();
    vs.stop();
    DestroyWindow(hwnd);
    showCursor();
    allowSleep();
    if (mmh) AvRevertMmThreadCharacteristics(mmh);

    CoUninitialize();
    std::cout << "[INFO] Session ended.\n";
    return 0;
}
