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
#define WIN32_LEAN_AND_MEAN  // выкидываем из windows.h весь мусор - WinSock, Crypto и прочий балласт, который нам не нужен
#endif
#define _WIN32_DCOM   // говорим, что будем юзать COM - нужно для DirectShow и перечисления устройств
#define NOMINMAX      // запрещаем windows.h переопределять min/max - иначе std::min/max ломается напрочь

#include <windows.h>      // Win32 API - основа основ, без неё нет ни окна, ни ничего
#include <dshow.h>        // DirectShow - перечисляем видео-устройства через COM
#include <comdef.h>       // COM-утилиты - нужны для _com_error и прочих COM-хелперов
#include <avrt.h>         // MMCSS - поднимаем приоритет потока до "Pro Audio" / "Games"
#include <d3d11.h>        // DirectX 11 - сам GPU pipeline: девайс, контекст, текстуры
#include <dxgi1_2.h>      // DXGI 1.2 - IDXGISwapChain1 и IDXGIFactory2 живут здесь
#include <dxgi1_5.h>      // DXGI 1.5 - нужен для проверки поддержки тиринга (Allow Tearing)
#include <d3dcompiler.h>  // рантайм-компилятор HLSL - компилируем шейдеры из строк прямо при запуске

#include <atomic>     // std::atomic - безлоковый обмен данными между потоками
#include <array>      // std::array - фиксированный массив для тройного буфера
#include <chrono>     // std::chrono - замер времени для FPS-счетчика
#include <iostream>   // std::cout / std::cerr - консольный вывод и ошибки
#include <string>     // std::string - строки для имен устройств, клавиш и UI
#include <thread>     // std::thread - отдельный поток захвата видео
#include <algorithm>  // std::min - пригодится при масштабировании viewport
#include <vector>     // std::vector - список найденных видео-устройств
#include <fstream>    // std::ifstream / std::ofstream - читаем и пишем keybindings.bin
#include <cstring>    // memcpy - копируем строки пикселей в текстуру побайтово

#include <opencv2/imgproc.hpp>  // cv::cvtColor, cv::putText - конвертация цветов и FPS-текст
#include <opencv2/videoio.hpp>  // cv::VideoCapture - захват с камеры / capture-карты

// ─── Глобальные флаги ────────────────────────────────────────────────────────

static std::atomic<bool> g_running { true  };  // пока true - главный цикл крутится, false - всё, выходим
static std::atomic<bool> g_showFPS  { false };  // показывать ли FPS-оверлей - по умолчанию выключен
static std::atomic<bool> g_vsync    { false };  // VSync вкл/выкл - atomic потому что читается из двух потоков

// ─── UI helpers ──────────────────────────────────────────────────────────────
// Все блоки шириной 50 символов внутри (52 с рамкой), отступ 2 пробела слева.
// Content-строки — чистый ASCII, поэтому size() == display width.

static const char* UI_TOP = "  ╔══════════════════════════════════════════════════╗";  // верхняя рамка блока
static const char* UI_SEP = "  ╠══════════════════════════════════════════════════╣";  // разделитель внутри блока
static const char* UI_BOT = "  ╚══════════════════════════════════════════════════╝";  // нижняя рамка блока
static const int   UI_W   = 50;  // внутренняя ширина блока в символах - всё под это число и верстается

// Возвращает количество кодовых точек UTF-8 (= display-ширину для BMP символов).
// c.size() считает байты: многобайтные символы (╠, —, █ и т.д.) дают лишние байты,
// из-за чего padding получается меньше нужного.
// UTF-8: байты 0x80-0xBF — continuation bytes, не считаем их.
static int utf8Len(const std::string& s) {
    int n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;  // continuation byte (10xxxxxx) - не считаем, это не отдельный символ
    return n;
}

// Строка с левым отступом 2 пробела
static void uiLine(const std::string& s) {
    std::string c = "  " + s;  // приклеиваем 2 пробела слева для отступа
    int dw = utf8Len(c);       // считаем дисплейную ширину в символах, а не байтах
    while (dw < UI_W) { c += ' '; ++dw; }  // добиваем пробелами до ровно 50 символов
    // Если строка слишком длинная — обрезаем по display-ширине
    if (dw > UI_W) {
        int keep = 0, bytes = 0;
        for (unsigned char ch : c) {
            if ((ch & 0xC0) != 0x80) { if (keep >= UI_W) break; ++keep; }  // считаем символы, не байты
            ++bytes;  // байты считаем отдельно - нужно знать где обрезать строку
        }
        c = c.substr(0, bytes);  // обрезаем по байтам, но с учетом символьной ширины
    }
    std::cout << "  ║" << c << "║\n";  // выводим строку в рамку
}

// Центрированная строка
static void uiCenter(const std::string& s) {
    int sw = utf8Len(s);          // дисплейная ширина самой строки
    int lp = (UI_W - sw) / 2;     // отступ слева - половина свободного места
    if (lp < 0) lp = 0;           // на случай если строка шире блока - не уходим в минус
    int rp = UI_W - lp - sw;      // отступ справа - остаток после левого и текста
    if (rp < 0) rp = 0;           // аналогично - защита от отрицательного padding
    std::cout << "  ║"
              << std::string(lp, ' ') << s << std::string(rp, ' ')  // лево-текст-право
              << "║\n";
}

// Пустая строка внутри блока
static void uiBlank() {
    std::cout << "  ║" << std::string(UI_W, ' ') << "║\n";  // 50 пробелов в рамке - просто воздух для читаемости
}

// ─── Стартовый баннер ────────────────────────────────────────────────────────

static void printBanner()
{
    // FIGlet-стиль «EDB», 24 символа шириной, центр в 50 → 13 слева
    // Строки 1 и 6 шириной 23 → 13 слева, 14 справа
    // Строки 2-5 шириной 24 → 13 слева, 13 справа
    static const char* art[] = {
        "             ███████╗██████╗ ██████╗              ",  // E  D  B - первая строка ASCII-арта
        "             ██╔════╝██╔══██╗██╔══██╗             ",
        "             █████╗  ██║  ██║██████╔╝             ",
        "             ██╔══╝  ██║  ██║██╔══██╗             ",
        "             ███████╗██████╔╝██████╔╝             ",
        "             ╚══════╝╚═════╝ ╚═════╝              ",  // последняя строка - основание букв
    };

    std::cout << "\n" << UI_TOP << "\n";  // открываем рамку баннера
    uiBlank();                            // пустая строка сверху - воздух
    for (auto& row : art)
        std::cout << "  ║" << row << "║\n";  // каждая строка арта идет напрямую - padding уже захардкожен
    uiBlank();                                // пустая строка после арта
    // "External  Display  Bridge" — 25 символов, lp=12 rp=13
    std::cout << "  ║" << "            External  Display  Bridge             " << "║\n";  // название приложения
    // "v3.0" — 4 символа, lp=23 rp=23
    std::cout << "  ║" << "                       v3.0                       " << "║\n";  // версия по центру
    uiBlank();
    // Технологии — ровно 50 символов
    std::cout << "  ║" << " DirectX 11  |  Triple Buffer  |  MMCSS  |  YUY2  " << "║\n";  // ключевые технологии в одну строку
    std::cout << UI_BOT << "\n\n";  // закрываем рамку и оставляем отступ
}


// ─── Настройки клавиш ────────────────────────────────────────────────────────

static const char*     KEYBIND_FILE    = "keybindings.bin";   // файл рядом с exe - просто бинарный дамп структуры
static const DWORD     KB_MAGIC        = 0x4B425633;          // "KBV3" в ASCII - magic number для проверки что файл наш и не устарел

struct KeyBindings {
    DWORD magic   = KB_MAGIC;   // по этому числу понимаем что файл от нашей программы, а не мусор
    int   vkFPS   = 'F';        // дефолтная клавиша для FPS-оверлея
    int   vkVSync = 'V';        // дефолтная клавиша для VSync
    int   vkExit  = VK_ESCAPE;  // дефолтный выход - Escape, классика
};

static bool saveKeyBindings(const KeyBindings& kb)
{
    std::ofstream f(KEYBIND_FILE, std::ios::binary);  // открываем в бинарном режиме - без текстовых конвертаций
    if (!f) return false;                              // не смогли создать файл - нет прав или диск занят
    f.write(reinterpret_cast<const char*>(&kb), sizeof(kb));  // пишем структуру как есть, побайтово
    return f.good();  // если поток ок - значит запись прошла без ошибок
}

static KeyBindings loadKeyBindings()
{
    KeyBindings kb;              // дефолтные значения из структуры - F, V, Escape
    std::ifstream f(KEYBIND_FILE, std::ios::binary);  // пробуем открыть бинарник
    if (!f) return kb;           // файла нет - возвращаем дефолты, первый запуск
    KeyBindings tmp;
    f.read(reinterpret_cast<char*>(&tmp), sizeof(tmp));  // читаем ровно столько байт, сколько занимает структура
    if (f.gcount() == sizeof(tmp) && tmp.magic == KB_MAGIC)  // проверяем размер и magic - защита от кривых файлов
        kb = tmp;  // всё ок - берем сохраненные биндинги
    return kb;
}

// ─── Человекочитаемые имена VK-кодов ─────────────────────────────────────────

static std::string vkToString(int vk)
{
    // Mouse buttons
    switch (vk) {
    case VK_LBUTTON:  return "Mouse Left";          // виртуальный код левой кнопки мыши
    case VK_RBUTTON:  return "Mouse Right";         // правая кнопка
    case VK_MBUTTON:  return "Mouse Middle";        // колесо / средняя кнопка
    case VK_XBUTTON1: return "Mouse X1 (Back)";     // боковая кнопка "назад" - у геймерских мышей
    case VK_XBUTTON2: return "Mouse X2 (Forward)";  // боковая кнопка "вперед"
    case VK_BACK:     return "Backspace";
    case VK_TAB:      return "Tab";
    case VK_RETURN:   return "Enter";
    case VK_ESCAPE:   return "Escape";
    case VK_SPACE:    return "Space";
    case VK_PRIOR:    return "Page Up";     // VK_PRIOR - это Page Up, название нелогичное но так в Win32
    case VK_NEXT:     return "Page Down";   // VK_NEXT - Page Down, аналогично
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
    case VK_MENU:     return "Alt";     // VK_MENU - это Alt, вот так Win32 его называет
    case VK_PAUSE:    return "Pause";
    case VK_SNAPSHOT: return "Print Screen";  // VK_SNAPSHOT - Print Screen, тоже нелогичное имя
    case VK_SCROLL:   return "Scroll Lock";
    case VK_NUMLOCK:  return "Num Lock";
    case VK_LWIN:     return "Left Win";
    case VK_RWIN:     return "Right Win";
    case VK_APPS:     return "Menu";  // VK_APPS - контекстное меню, та кнопка рядом с правым Win
    // F keys
    case VK_F1:  return "F1";  case VK_F2:  return "F2";   // F1-F24 - Win32 поддерживает все, даже экзотику
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
    default: break;  // всё что не попало в switch - обрабатываем ниже
    }

    // Printable ASCII (letters and digits)
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
        return std::string(1, static_cast<char>(vk));  // VK-код цифр и букв совпадает с ASCII - просто кастуем

    // Fallback: use GetKeyNameText (requires scan code)
    UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);  // переводим VK-код в скан-код клавиатуры
    if (scan) {
        wchar_t buf[64] = {};
        LONG lParam = static_cast<LONG>(scan << 16);  // скан-код надо упаковать в lParam - старший байт
        if (GetKeyNameTextW(lParam, buf, 64) > 0) {   // просим Windows дать локализованное имя клавиши
            int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);  // узнаем сколько байт нужно под UTF-8
            std::string s(len, '\0');
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, &s[0], len, nullptr, nullptr);  // конвертируем wide string в UTF-8
            if (!s.empty()) s.pop_back();  // убираем нулевой терминатор который WideCharToMultiByte пишет в конец
            return s;
        }
    }

    char hex[16];
    snprintf(hex, sizeof(hex), "VK 0x%02X", vk);  // последний вариант - показываем сырой hex-код клавиши
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
    Sleep(200);  // ждём пока отпустится предыдущая клавиша (Enter / Y) - иначе сразу схватим её хвост

    while (true) {
        // Мышь — проверяем первой, включая боковые кнопки X1/X2
        for (int vk : { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 }) {
            if (GetAsyncKeyState(vk) & 0x8000) {  // старший бит = клавиша зажата прямо сейчас
                Sleep(100);  // дебаунс - ждем пока юзер уберет палец, чтобы не поймать одно нажатие дважды
                return vk;
            }
        }

        // Клавиатура: диапазон 0x08-0xDE, зарезервированные диапазоны пропускаем
        for (int vk = 0x08; vk <= 0xDE; ++vk) {
            // Зарезервировано / undefined по таблице Microsoft Virtual-Key Codes
            if (vk == 0x0A || vk == 0x0B || vk == 0x0E || vk == 0x0F) continue;  // linefeed и прочий мусор - пропускаем
            if (vk >= 0x3A && vk <= 0x40) continue;  // undefined диапазон - там нет реальных клавиш
            if (vk >= 0x5B && vk <= 0x5F) continue;  // Win-клавиши и reserved - их уже обработали отдельно
            if (vk >= 0x88 && vk <= 0x8F) continue;  // unassigned - Microsoft оставила дыры в таблице
            if (vk >= 0x97 && vk <= 0x9F) continue;  // unassigned
            if (vk >= 0xB8 && vk <= 0xB9) continue;  // reserved
            if (vk >= 0xC1 && vk <= 0xC2) continue;  // reserved
            // В диапазоне 0xC3-0xDA оставляем только F1-F12, остальное — OEM/reserved
            if (vk >= 0xC3 && vk <= 0xDA &&
                vk != VK_F1  && vk != VK_F2  && vk != VK_F3  && vk != VK_F4  &&
                vk != VK_F5  && vk != VK_F6  && vk != VK_F7  && vk != VK_F8  &&
                vk != VK_F9  && vk != VK_F10 && vk != VK_F11 && vk != VK_F12) continue;  // там только F-клавиши, остальное OEM-мусор

            if (GetAsyncKeyState(vk) & 0x8000) {  // старший бит - клавиша зажата
                Sleep(100);  // дебаунс
                return vk;
            }
        }

        Sleep(10);  // ждем 10мс между опросами - CPU почти не грузим, задержка восприятия незаметна
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
        const char* label;  // название действия для вывода в консоль
        int*        vk;     // указатель на поле в KeyBindings - меняем напрямую
    };

    Action actions[] = {
        { "Toggle FPS overlay",     &current.vkFPS   },  // три действия которые нужно переназначить
        { "Toggle VSync on/off",    &current.vkVSync },
        { "Exit the program",       &current.vkExit  },
    };
    const int N = 3;  // количество биндингов - ровно три

    for (int i = 0; i < N; ++i) {
        // Показываем текущую привязку
        std::cout << "  [" << (i+1) << "/" << N << "] "
                  << actions[i].label << "\n"
                  << "        Current: " << vkToString(*actions[i].vk) << "\n"
                  << "        Press any key or mouse button... ";
        std::cout.flush();  // флашим буфер - иначе строка может не появиться до ввода

        bool accepted = false;
        while (!accepted) {
            int vk = captureAnyKey();  // блокируемся и ждем нажатия

            // Проверяем дублирование: та же клавиша уже назначена другому действию?
            bool duplicate = false;
            for (int j = 0; j < N; ++j) {
                if (j == i) continue;  // пропускаем текущий слот - сравниваем только с остальными
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
                *actions[i].vk = vk;                    // пишем новый VK-код напрямую через указатель
                std::cout << vkToString(vk) << "\n\n";  // показываем что записали
                accepted = true;                         // выходим из while - биндинг принят
            }
        }
    }

    std::cout << UI_BOT << "\n\n";
    return current;  // возвращаем обновленную структуру - caller сохранит её в файл
}

// ─── Запрос на настройку клавиш при старте ───────────────────────────────────

static KeyBindings startupKeySetup()
{
    KeyBindings kb = loadKeyBindings();  // пробуем загрузить из файла, если нет - получаем дефолты

    // ── Key Bindings box ─────────────────────────────────────────────────
    std::cout << "\n" << UI_TOP << "\n";
    uiCenter("Key Bindings");
    std::cout << UI_SEP << "\n";
    uiLine("FPS overlay  :  " + vkToString(kb.vkFPS));    // показываем текущие биндинги перед вопросом
    uiLine("VSync        :  " + vkToString(kb.vkVSync));
    uiLine("Exit         :  " + vkToString(kb.vkExit));
    std::cout << UI_SEP << "\n";
    uiLine("To reset defaults — delete \"keybindings.bin\"");  // подсказка - удали файл и получишь дефолты
    std::cout << UI_BOT << "\n";
    std::cout << "\n  Change key bindings? [Y / N]: ";
    std::cout.flush();

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);         // хендл стандартного ввода консоли
    FlushConsoleInputBuffer(hIn);                         // чистим буфер - вдруг там болтается хвост от Enter
    char buf[8] = {};
    DWORD nRead = 0;
    ReadConsoleA(hIn, buf, sizeof(buf) - 1, &nRead, nullptr);  // читаем ответ юзера из консоли

    char ch = (nRead > 0) ? static_cast<char>(toupper(buf[0])) : 'N';  // берем первый символ и апперкейсим - 'y' и 'Y' оба работают
    if (ch == 'Y') {
        kb = configureKeys(kb);           // запускаем интерактивную настройку
        if (saveKeyBindings(kb))
            std::cout << "  [OK] Key bindings saved to " << KEYBIND_FILE << "\n";
        else
            std::cout << "  [WARN] Could not save key bindings.\n";  // диск занят или нет прав - не фатально, просто предупреждение
    }

    std::cout << "\n" << UI_TOP << "\n";
    uiCenter("Active Key Bindings");  // показываем итоговые биндинги - что будет использоваться в сессии
    std::cout << UI_SEP << "\n";
    uiLine("FPS overlay  :  " + vkToString(kb.vkFPS));
    uiLine("VSync        :  " + vkToString(kb.vkVSync));
    uiLine("Exit         :  " + vkToString(kb.vkExit));
    std::cout << UI_BOT << "\n\n";

    return kb;
}

// ─── Питание / курсор ────────────────────────────────────────────────────────

static void preventSleep() { SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED); }  // запрещаем Windows засыпать и гасить экран пока программа работает
static void allowSleep()   { SetThreadExecutionState(ES_CONTINUOUS); }  // снимаем запрет - Windows снова может уходить в сон
static void hideCursor()   { while (ShowCursor(FALSE) >= 0) {} }  // ShowCursor возвращает счетчик показов - крутим пока не уйдет в минус
static void showCursor()   { while (ShowCursor(TRUE)  <  0) {} }  // обратно - крутим пока счетчик не станет >= 0

// ─── Приоритеты ──────────────────────────────────────────────────────────────

static void setProcessPriority()
{
    HANDLE h = GetCurrentProcess();  // хендл нашего процесса
    if (!SetPriorityClass(h, REALTIME_PRIORITY_CLASS))  // пробуем REALTIME - нужны права администратора
        SetPriorityClass(h, HIGH_PRIORITY_CLASS);        // fallback - HIGH тоже хорошо, без прав адмнистратора
}

static HANDLE registerMMCSS(const wchar_t* task)
{
    DWORD idx = 0;
    HANDLE h = AvSetMmThreadCharacteristicsW(task, &idx);  // регистрируем поток в Multimedia Class Scheduler - Windows даёт ему приоритет над обычными потоками
    return h;  // возвращаем хендл - потом нужен для AvRevertMmThreadCharacteristics при выходе
}

// ─── Перечисление и выбор устройства ─────────────────────────────────────────

struct DeviceInfo {
    int         index;  // индекс для cv::VideoCapture - именно им открываем устройство
    std::string name;   // человекочитаемое название - то что видит юзер в списке
};

static std::vector<DeviceInfo> enumerateDevices()
{
    std::vector<DeviceInfo> result;

    HRESULT hr = S_OK;
    (void)hr;  // подавляем warning о неиспользуемой переменной - hr используется в assert-режиме

    ICreateDevEnum* devEnum = nullptr;
    CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                     IID_ICreateDevEnum, reinterpret_cast<void**>(&devEnum));  // создаем COM-объект системного перечислителя устройств
    if (!devEnum) { CoUninitialize(); return result; }  // COM не инициализирован или нет устройств - выходим

    IEnumMoniker* enumMon = nullptr;
    devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMon, 0);  // просим перечислитель для категории "видеовхода"
    devEnum->Release();  // перечислитель устройств больше не нужен
    if (!enumMon) { CoUninitialize(); return result; }  // нет ни одного видеоустройства в системе

    IMoniker* moniker = nullptr;
    int index = 0;

    while (enumMon->Next(1, &moniker, nullptr) == S_OK) {  // берем по одному монникеру из перечислителя
        IPropertyBag* propBag = nullptr;
        if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_IPropertyBag,
                                              reinterpret_cast<void**>(&propBag)))) {  // биндимся к хранилищу свойств устройства
            VARIANT var; VariantInit(&var);  // VARIANT - COM-тип данных, инициализируем нулями
            if (SUCCEEDED(propBag->Read(L"FriendlyName", &var, nullptr))) {  // читаем "FriendlyName" - это и есть имя устройства
                int len = WideCharToMultiByte(CP_UTF8, 0, var.bstrVal, -1,
                                              nullptr, 0, nullptr, nullptr);  // узнаем сколько байт нужно под UTF-8 строку
                std::string name(len, '\0');
                WideCharToMultiByte(CP_UTF8, 0, var.bstrVal, -1,
                                    &name[0], len, nullptr, nullptr);  // конвертируем wide string с именем в UTF-8
                if (!name.empty()) name.pop_back();  // убираем нулевой терминатор в конце
                result.push_back({ index, name });   // добавляем устройство в список
                VariantClear(&var);  // освобождаем память VARIANT
            }
            propBag->Release();  // освобождаем property bag
        }
        moniker->Release();  // освобождаем монникер текущего устройства
        ++index;  // следующий индекс для VideoCapture
    }
    enumMon->Release();  // освобождаем перечислитель
    return result;
}

static int selectDevice()
{
    auto devices = enumerateDevices();  // получаем список всех видеоустройств через DirectShow

    if (devices.empty()) {
        std::cerr << "[ERROR] No video devices found.\n";  // ни одной камеры или capture-карты - ничего не можем сделать
        return -1;  // сигнал для main что запуск невозможен
    }

    std::cout << "\n" << UI_TOP << "\n";
    uiCenter("Video Devices");
    std::cout << UI_SEP << "\n";
    for (auto& d : devices) {
        std::string entry = "[" + std::to_string(d.index) + "]  " + d.name;  // форматируем: [0]  Elgato HD60 X
        uiLine(entry);
    }
    std::cout << UI_BOT << "\n";

    if (devices.size() == 1) {
        std::cout << "\n  [AUTO] Only one device — selecting: "
                  << devices[0].name << "\n\n";  // одно устройство - выбираем автоматически, не мешаем юзеру
        return devices[0].index;
    }

    std::cout << "\n  Select device [0-" << (devices.size()-1) << "]: ";
    std::cout.flush();

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    FlushConsoleInputBuffer(hIn);  // чистим буфер ввода перед чтением

    char buf[16] = {};
    DWORD read = 0;
    ReadConsoleA(hIn, buf, sizeof(buf) - 1, &read, nullptr);  // читаем ввод юзера

    int choice = -1;
    try { choice = std::stoi(std::string(buf)); } catch (...) {}  // парсим число - если юзер ввел мусор, остаемся на -1

    for (auto& d : devices)
        if (d.index == choice) {
            std::cout << "  [OK] Selected: " << d.name << "\n\n";
            return d.index;  // нашли совпадение - возвращаем индекс
        }

    std::cout << "  [WARN] Invalid index, defaulting to 0\n\n";  // юзер ввел не то - берем первое устройство
    return devices[0].index;
}

// ─── Triple Buffer ────────────────────────────────────────────────────────────

struct TripleBuffer {
    std::array<cv::Mat, 3> bufs;       // три кадра: один пишет поток захвата, один читает рендерер, третий в запасе
    std::atomic<int> latest  { -1 };   // индекс последнего записанного кадра, -1 = кадров ещё не было
    std::atomic<int> writing {  0 };   // в какой слот сейчас пишет поток захвата

    void commitWrite()
    {
        int w = writing.load(std::memory_order_relaxed);   // узнаем в какой слот только что записали
        latest.store(w, std::memory_order_release);         // публикуем - теперь рендерер видит новый кадр
        writing.store((w + 1) % 3, std::memory_order_relaxed);  // переключаем на следующий слот по кругу
    }

    cv::Mat* tryRead()
    {
        int idx = latest.load(std::memory_order_acquire);  // читаем индекс последнего кадра с acquire-барьером
        return (idx >= 0) ? &bufs[idx] : nullptr;          // нет кадров - null, есть - указатель на буфер
    }
};

// ─── Поток захвата ────────────────────────────────────────────────────────────

class VideoStream {
public:
    explicit VideoStream(int deviceId, TripleBuffer& tb)
        : tb_(tb), running_(true), width_(0), height_(0)
    {
        cap_.open(deviceId, cv::CAP_DSHOW);  // открываем через DirectShow - это дает доступ к YUY2 и меньше задержку чем MSMF

        cap_.set(cv::CAP_PROP_FOURCC,     cv::VideoWriter::fourcc('Y','U','Y','2'));  // просим YUY2 - нет MJPG-декодирования = меньше задержка
        cap_.set(cv::CAP_PROP_FPS,        60);          // 60 fps - целевая частота захвата
        cap_.set(cv::CAP_PROP_BUFFERSIZE, 1);           // минимальный буфер - меньше задержка, драйвер держит только 1 кадр
        cap_.set(cv::CAP_PROP_FRAME_WIDTH,  1920);      // 1080p разрешение
        cap_.set(cv::CAP_PROP_FRAME_HEIGHT, 1080);

        width_  = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_WIDTH));   // читаем обратно - устройство могло округлить
        height_ = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_HEIGHT));  // то что реально выдает, а не что просили

        captureThread_ = std::thread(&VideoStream::captureLoop, this);  // запускаем поток захвата - он будет крутиться независимо
    }

    ~VideoStream() { stop(); }  // деструктор сам останавливает поток - нет утечки треда

    double get(int p)  const { return cap_.get(p); }   // прокидываем cap_.get наружу - для FPS и FOURCC
    int    width()     const { return width_;  }
    int    height()    const { return height_; }

    void stop()
    {
        running_ = false;                               // сигнализируем потоку захвата что пора заканчивать
        if (captureThread_.joinable()) captureThread_.join();  // ждем пока поток сам завершится - без force-kill
        cap_.release();  // освобождаем захватчик - отпускаем устройство
    }

private:
    void captureLoop()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);  // максимальный приоритет потока - пропускаем вперед всех
        HANDLE mmh = registerMMCSS(L"Pro Audio");  // "Pro Audio" - самый высокий профиль MMCSS, даже выше "Games"

        while (running_) {
            int w = tb_.writing.load(std::memory_order_relaxed);  // узнаем в какой слот писать
            bool ok = cap_.read(tb_.bufs[w]);                      // читаем кадр прямо в буфер тройного буфера - без копирования
            if (ok && !tb_.bufs[w].empty())
                tb_.commitWrite();  // кадр готов - атомарно публикуем для рендерера
        }
        if (mmh) AvRevertMmThreadCharacteristics(mmh);  // откатываем MMCSS перед выходом из потока
    }

    cv::VideoCapture  cap_;           // OpenCV захватчик с DirectShow бэкендом
    TripleBuffer&     tb_;            // ссылка на тройной буфер - разделяем с рендерером
    std::atomic<bool> running_;       // флаг работы - atomic потому что пишем из main, читаем из captureLoop
    std::thread       captureThread_; // тред захвата - живет пока жив VideoStream
    int               width_, height_;  // реальное разрешение которое выдает устройство
};

// ─── HLSL шейдеры ────────────────────────────────────────────────────────────

static const char* s_vsCode = R"(
struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
VS_OUT main(uint id : SV_VertexID) {
    float2 uv  = float2((id & 1) ? 2.0f : 0.0f, (id & 2) ? 2.0f : 0.0f);  // из id вершины делаем UV-координаты - трюк с fullscreen triangle без vertex buffer
    VS_OUT o;
    o.pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);  // UV [0,1] -> clip space [-1,1], Y инвертируем потому что DX считает Y снизу
    o.uv  = uv;
    return o;
}
)";

static const char* s_psCode = R"(
Texture2D    tex : register(t0);   // слот t0 - туда биндим SRV нашей текстуры
SamplerState sam : register(s0);   // слот s0 - point sampler для пикселперфект-рендеринга
struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
float4 main(VS_OUT i) : SV_TARGET {
    float4 c = tex.Sample(sam, i.uv);
    return float4(c.b, c.g, c.r, 1.0f);  // BGR->RGB swizzle прямо на GPU - cpu не тратим на cvtColor
}
)";

// ─── DirectX 11 Renderer ─────────────────────────────────────────────────────

struct DX11Renderer {
    ID3D11Device*             device    = nullptr;  // GPU-устройство - создает ресурсы и шейдеры
    ID3D11DeviceContext*      ctx       = nullptr;  // контекст - выполняет draw calls и апдейты
    IDXGISwapChain1*          swapChain = nullptr;  // цепочка буферов - показывает кадры на экран
    ID3D11RenderTargetView*   rtv       = nullptr;  // куда пишем - вью на back buffer
    ID3D11VertexShader*       vs        = nullptr;  // вертексный шейдер - генерирует fullscreen triangle
    ID3D11PixelShader*        ps        = nullptr;  // пиксельный шейдер - сэмплирует текстуру и делает BGR->RGB
    ID3D11Texture2D*          dynTex    = nullptr;  // динамическая текстура - CPU пишет, GPU читает
    ID3D11ShaderResourceView* srv       = nullptr;  // вью на текстуру - биндится к пиксельному шейдеру
    ID3D11SamplerState*       sampler   = nullptr;  // point sampler - без интерполяции, пиксель в пиксель

    int  winW = 0, winH = 0;    // размер окна - нужен для вычисления viewport
    int  texW = 0, texH = 0;    // размер текстуры - если кадр поменял разрешение пересоздаем
    bool tearingOk = false;     // поддерживает ли монитор/драйвер Allow Tearing для безVSync рендеринга

    bool init(HWND hwnd, int w, int h)
    {
        winW = w; winH = h;

        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                     &fl, 1, D3D11_SDK_VERSION,
                                     &device, nullptr, &ctx))) {  // создаем DX11 девайс и immediate context на дефолтном адаптере
            std::cerr << "[DX11] CreateDevice failed\n"; return false;
        }

        IDXGIFactory2* factory = nullptr;
        {
            IDXGIDevice*  dxgiDev = nullptr;
            IDXGIAdapter* adapter = nullptr;
            device->QueryInterface(__uuidof(IDXGIDevice),  reinterpret_cast<void**>(&dxgiDev));  // получаем DXGI-интерфейс нашего девайса
            dxgiDev->GetAdapter(&adapter);                                                         // из него - адаптер (видеокарта)
            adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));      // из адаптера - фабрика которая его создала
            adapter->Release(); dxgiDev->Release();  // временные объекты освобождаем сразу
        }

        IDXGIFactory5* f5 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5),
                                              reinterpret_cast<void**>(&f5)))) {  // пробуем получить Factory5 - только там есть CheckFeatureSupport для тиринга
            BOOL t = FALSE;
            f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &t, sizeof(t));  // спрашиваем - можем ли рендерить без VSync без разрывов
            tearingOk = (t == TRUE);  // сохраняем результат - используем при Present
            f5->Release();
        }

        DXGI_SWAP_CHAIN_DESC1 scd = {};
        scd.Width       = w; scd.Height = h;                    // размер под разрешение экрана
        scd.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;          // стандартный формат 8 бит на канал
        scd.BufferCount = 2;                                     // front + back buffer, flip model требует минимум 2
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;      // буферы используются как render target
        scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;        // flip model - быстро, не копирует буфер, поддерживает тиринг
        scd.SampleDesc  = { 1, 0 };                              // MSAA выключен - нам не нужна сглаживание
        scd.Flags       = tearingOk ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;  // включаем тиринг-флаг если железо поддерживает

        if (FAILED(factory->CreateSwapChainForHwnd(device, hwnd, &scd,
                                                    nullptr, nullptr, &swapChain))) {  // создаем swapchain привязанный к нашему HWND
            std::cerr << "[DX11] CreateSwapChain failed\n";
            factory->Release(); return false;
        }

        IDXGIFactory1* f1 = nullptr;
        swapChain->GetParent(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f1));
        if (f1) { f1->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER); f1->Release(); }  // отключаем Alt+Enter -> полноэкранный режим DXGI, управляем сами
        factory->Release();

        if (!rebuildRTV()) return false;      // создаем render target view на back buffer
        if (!compileShaders()) return false;  // компилируем HLSL шейдеры из строк

        D3D11_SAMPLER_DESC sd = {};
        sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;                       // point filtering - без билинейного сглаживания, пиксель в пиксель
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;  // края клампаем - UV за [0,1] дают крайний пиксель, без повторений
        device->CreateSamplerState(&sd, &sampler);

        return true;
    }

    bool compileShaders()
    {
        ID3DBlob *blob = nullptr, *err = nullptr;

        D3DCompile(s_vsCode, strlen(s_vsCode), nullptr, nullptr, nullptr,
                   "main", "vs_5_0", 0, 0, &blob, &err);  // компилируем вертексный шейдер под shader model 5.0
        if (!blob) {
            std::cerr << "[DX11] VS: " << (err?(char*)err->GetBufferPointer():"?") << "\n";  // выводим ошибку компиляции HLSL
            if (err) err->Release(); return false;
        }
        device->CreateVertexShader(blob->GetBufferPointer(),
                                   blob->GetBufferSize(), nullptr, &vs);  // создаем шейдерный объект из скомпилированного байткода
        blob->Release();  // байткод больше не нужен - шейдер уже на GPU

        D3DCompile(s_psCode, strlen(s_psCode), nullptr, nullptr, nullptr,
                   "main", "ps_5_0", 0, 0, &blob, &err);  // компилируем пиксельный шейдер
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
        if (rtv) { rtv->Release(); rtv = nullptr; }  // освобождаем старый RTV перед пересозданием
        ID3D11Texture2D* back = nullptr;
        swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                             reinterpret_cast<void**>(&back));  // берем back buffer из swap chain - индекс 0
        if (!back) return false;
        device->CreateRenderTargetView(back, nullptr, &rtv);  // создаем view на back buffer - теперь можем рендерить в него
        back->Release();  // view уже держит ссылку, сам указатель нам не нужен
        return rtv != nullptr;
    }

    void ensureTexture(int w, int h)
    {
        if (texW == w && texH == h) return;  // разрешение не поменялось - ничего не делаем

        if (srv)    { srv->Release();    srv    = nullptr; }  // сначала освобождаем view...
        if (dynTex) { dynTex->Release(); dynTex = nullptr; }  // ...потом саму текстуру

        D3D11_TEXTURE2D_DESC td = {};
        td.Width          = w; td.Height = h;
        td.MipLevels      = 1; td.ArraySize = 1;             // один уровень MIP, один слой
        td.Format         = DXGI_FORMAT_R8G8B8A8_UNORM;      // RGBA 8 бит - пиксельный шейдер ждет именно это
        td.SampleDesc     = { 1, 0 };                         // без MSAA
        td.Usage          = D3D11_USAGE_DYNAMIC;              // CPU пишет каждый кадр - нужен DYNAMIC
        td.BindFlags      = D3D11_BIND_SHADER_RESOURCE;       // используем как texture в шейдере
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;           // CPU должен мапить текстуру для записи

        if (FAILED(device->CreateTexture2D(&td, nullptr, &dynTex))) {
            std::cerr << "[DX11] CreateTexture2D failed (" << w << "x" << h << ")\n";
            return;
        }
        device->CreateShaderResourceView(dynTex, nullptr, &srv);  // создаем SRV для биндинга в пиксельный шейдер
        texW = w; texH = h;  // запоминаем новое разрешение
    }

    void uploadFrame(const cv::Mat& frame)
    {
        ensureTexture(frame.cols, frame.rows);  // пересоздаем текстуру если разрешение поменялось
        if (!dynTex) return;

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(ctx->Map(dynTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;  // WRITE_DISCARD - говорим GPU что перезаписываем всё, он не ждет завершения предыдущего кадра

        const uint8_t* src = frame.ptr(0);           // указатель на начало пикселей OpenCV кадра
        uint8_t*       dst = static_cast<uint8_t*>(mapped.pData);  // указатель на память текстуры после Map
        const int      W   = frame.cols;
        const int      H   = frame.rows;
        const int      srcStep  = static_cast<int>(frame.step[0]);  // размер строки в OpenCV - может быть с паддингом
        const UINT     dstPitch = mapped.RowPitch;                   // размер строки в DX текстуре - тоже может быть с паддингом, другим

        thread_local cv::Mat bgraRow;  // thread_local - аллоцируем один раз на поток, не каждый кадр
        for (int y = 0; y < H; ++y) {
            const cv::Mat srcRow(1, W, CV_8UC3, const_cast<uint8_t*>(src + y * srcStep));  // вью на одну строку BGR без копирования
            bgraRow.create(1, W, CV_8UC4);                                                  // выделяем строку BGRA если ещё не выделено
            cv::cvtColor(srcRow, bgraRow, cv::COLOR_BGR2BGRA);                              // BGR -> BGRA, добавляем alpha=255
            memcpy(dst + y * dstPitch, bgraRow.ptr(0), W * 4);                             // копируем строку в текстуру с учетом DX-pitch
        }
        ctx->Unmap(dynTex, 0);  // анмапим - GPU может снова читать текстуру
    }

    void render()
    {
        if (!srv) return;  // текстура ещё не создана - пропускаем кадр

        float scaleX = (float)winW / (float)texW;  // масштаб по X чтобы вписать кадр в окно
        float scaleY = (float)winH / (float)texH;  // масштаб по Y
        float scale  = (std::min)(scaleX, scaleY);  // берем меньший - letterbox без обрезки
        float vpW    = texW * scale;
        float vpH    = texH * scale;
        float vpX    = (winW - vpW) * 0.5f;  // центрируем по X
        float vpY    = (winH - vpH) * 0.5f;  // центрируем по Y

        D3D11_VIEWPORT vp = { vpX, vpY, vpW, vpH, 0.0f, 1.0f };
        ctx->RSSetViewports(1, &vp);          // устанавливаем viewport - GPU рендерит только в эту область
        ctx->OMSetRenderTargets(1, &rtv, nullptr);  // биндим back buffer как render target

        float black[4] = { 0, 0, 0, 1 };
        ctx->ClearRenderTargetView(rtv, black);  // чистим чёрным - letterbox поля будут чёрными

        ctx->VSSetShader(vs, nullptr, 0);           // биндим вертексный шейдер
        ctx->PSSetShader(ps, nullptr, 0);           // биндим пиксельный шейдер
        ctx->PSSetShaderResources(0, 1, &srv);      // биндим текстуру в слот t0 пиксельного шейдера
        ctx->PSSetSamplers(0, 1, &sampler);         // биндим sampler в слот s0
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);  // треугольники
        ctx->IASetInputLayout(nullptr);             // нет vertex buffer - шейдер сам генерирует вершины из SV_VertexID
        ctx->Draw(3, 0);                            // рисуем один fullscreen triangle - 3 вершины, нет буфера

        bool vsync = g_vsync.load();
        UINT flags = (!vsync && tearingOk) ? DXGI_PRESENT_ALLOW_TEARING : 0;  // без VSync и с поддержкой - разрешаем тиринг для минимальной задержки
        swapChain->Present(vsync ? 1 : 0, flags);  // 1 = ждать VBlank, 0 = показать сразу
    }

    void release()
    {
        if (sampler)   sampler->Release();    // освобождаем в обратном порядке создания
        if (srv)       srv->Release();
        if (dynTex)    dynTex->Release();
        if (ps)        ps->Release();
        if (vs)        vs->Release();
        if (rtv)       rtv->Release();
        if (swapChain) swapChain->Release();
        if (ctx)       ctx->Release();
        if (device)    device->Release();     // девайс последним - все зависящие от него объекты уже освобождены
    }
};

// ─── Win32 окно ──────────────────────────────────────────────────────────────

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        g_running = false;  // крестик нажали - выходим из главного цикла через флаг
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);  // посылаем WM_QUIT - нужно для корректного завершения message loop
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);  // все остальные сообщения обрабатывает Windows по умолчанию
}

static HWND createFullscreenWindow(int& outW, int& outH)
{
    outW = GetSystemMetrics(SM_CXSCREEN);  // ширина основного монитора в пикселях
    outH = GetSystemMetrics(SM_CYSCREEN);  // высота основного монитора

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);               // обязательное поле - Win32 API проверяет версию структуры
    wc.lpfnWndProc   = WndProc;                  // наш message handler
    wc.hInstance     = GetModuleHandleW(nullptr);  // хендл нашего .exe
    wc.lpszClassName = L"BridgeWnd";             // имя класса окна - уникальное для регистрации
    wc.hCursor       = nullptr;                  // нет курсора - мы сами его прячем через ShowCursor
    RegisterClassExW(&wc);                        // регистрируем класс - без этого CreateWindow упадёт

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"BridgeWnd",  // WS_EX_TOPMOST - всегда поверх всех окон
        L"External Display Bridge",
        WS_POPUP | WS_VISIBLE,  // WS_POPUP - без рамки и заголовка, WS_VISIBLE - сразу видимое
        0, 0, outW, outH,       // позиция и размер - весь экран
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    SetForegroundWindow(hwnd);  // выводим окно на передний план
    SetFocus(hwnd);             // даём фокус ввода - без этого GetAsyncKeyState может не работать
    return hwnd;
}

// ─── FPS оверлей ─────────────────────────────────────────────────────────────

static void drawFPS(cv::Mat* frame, double fps, const char* codec)
{
    if (!frame || frame->empty()) return;  // на всякий случай - нулевой или пустой кадр не трогаем
    char buf[80];
    const char* vsyncStr = g_vsync.load() ? "VSync ON" : "VSync OFF";
    snprintf(buf, sizeof(buf), "FPS: %d | %s | %s", (int)fps, codec, vsyncStr);  // форматируем строку оверлея
    cv::putText(*frame, buf,
                cv::Point(20, frame->rows - 20),  // левый нижний угол - не мешает контенту
                cv::FONT_HERSHEY_SIMPLEX, 0.6,
                cv::Scalar(160, 160, 160), 1, cv::LINE_AA);  // серый текст с антиалиасингом - видно но не бросается в глаза
}

// ─── Опрос клавиш и кнопок мыши в главном цикле ─────────────────────────────
//
// GetAsyncKeyState работает для всех VK кодов, включая VK_XBUTTON1/2.
// Вызывается каждый кадр, но это ~1 мкс накладных расходов — нет влияния
// на задержку видео.

struct KeyState {
    bool wasDown = false;  // состояние на предыдущем кадре - нужно для детектирования переднего фронта

    // Возвращает true только на передний фронт нажатия
    bool poll(int vk) {
        bool isDown = (GetAsyncKeyState(vk) & 0x8000) != 0;  // старший бит = клавиша зажата прямо сейчас
        bool edge   = isDown && !wasDown;  // передний фронт - не было, стало - значит только что нажали
        wasDown     = isDown;              // запоминаем состояние для следующего кадра
        return edge;
    }
};

// ─── main ─────────────────────────────────────────────────────────────────────

int main()
{
    SetConsoleOutputCP(CP_UTF8);                                         // переключаем консоль в UTF-8 - иначе юникодные символы рамок не отобразятся
    printBanner();                                                        // красивый баннер EDB при запуске
    setProcessPriority();                                                 // поднимаем приоритет процесса до REALTIME или HIGH
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);      // главный поток тоже поднимаем - он рендерит
    HANDLE mmh = registerMMCSS(L"Games");                                 // главный поток регистрируем в MMCSS как "Games"
    preventSleep();                                                       // запрещаем Windows засыпать пока работаем

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // инициализируем COM в многопоточном режиме - нужно для DirectShow

    // ── Настройка клавиш ─────────────────────────────────────────────────────
    KeyBindings kb = startupKeySetup();  // загружаем биндинги из файла или предлагаем настроить

    // ── Выбор устройства ─────────────────────────────────────────────────────
    int deviceId = selectDevice();
    if (deviceId < 0) { allowSleep(); return 1; }  // устройств нет - снимаем запрет сна и выходим

    TripleBuffer tb;
    VideoStream  vs(deviceId, tb);  // запускаем поток захвата - он сразу начинает писать кадры в буфер

    int    srcW      = vs.width();
    int    srcH      = vs.height();
    int    fourccInt = static_cast<int>(vs.get(cv::CAP_PROP_FOURCC));  // числовой код кодека - YUY2 или MJPG
    double srcFps    = vs.get(cv::CAP_PROP_FPS);                       // реальный FPS который сообщает устройство

    char fourccStr[5] = {};
    for (int i = 0; i < 4; ++i)
        fourccStr[i] = static_cast<char>((fourccInt >> (8 * i)) & 0xFF);  // распаковываем 4-байтный int в ASCII-строку типа "YUY2"

    {
        std::string res  = std::to_string(srcW) + " x " + std::to_string(srcH);
        std::string fps  = std::to_string((int)srcFps);
        std::string keys = vkToString(kb.vkFPS)   + " = FPS  |  "
                         + vkToString(kb.vkVSync) + " = VSync  |  "
                         + vkToString(kb.vkExit)  + " = Exit";  // строка с активными биндингами для инфо-блока
        std::cout << "\n" << UI_TOP << "\n";
        uiCenter("Capture Device Info");
        std::cout << UI_SEP << "\n";
        uiLine("Resolution  :  " + res);
        uiLine(std::string("Codec       :  ") + fourccStr);
        uiLine("Target FPS  :  " + fps);
        if (std::string(fourccStr) != "YUY2")
            uiLine("[!] MJPG mode — extra 5-15ms decode delay");  // предупреждаем если не YUY2 - задержка будет выше
        std::cout << UI_SEP << "\n";
        uiLine(keys);
        std::cout << UI_BOT << "\n\n";
    }

    // ── Окно + DirectX ───────────────────────────────────────────────────────
    int winW = 0, winH = 0;
    HWND hwnd = createFullscreenWindow(winW, winH);  // создаём безрамочное WS_POPUP окно на весь экран
    hideCursor();                                      // прячем курсор - полноэкранный режим без мыши

    DX11Renderer dx;
    if (!dx.init(hwnd, winW, winH)) {
        std::cerr << "[ERROR] DX11 init failed.\n";
        vs.stop(); allowSleep(); return 1;  // DX11 не поднялся - останавливаем захват и выходим
    }

    auto prevTime = std::chrono::steady_clock::now();  // точка отсчета для FPS-счетчика

    // ── Состояния клавиш (защита от дребезга) ────────────────────────────────
    KeyState ksFPS, ksVSync, ksExit;  // три отдельных трекера для трёх биндингов

    // ── Главный цикл ─────────────────────────────────────────────────────────
    while (g_running) {
        // 1. Системные сообщения
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {  // вычитываем все накопившиеся сообщения без блокировки
            if (msg.message == WM_QUIT) g_running = false;       // WM_QUIT - начинаем завершение
            TranslateMessage(&msg);                               // переводим виртуальные клавиши в символы
            DispatchMessageW(&msg);                               // отправляем в WndProc
        }

        // 2. Опрос настроенных клавиш (передний фронт нажатия)
        //    GetAsyncKeyState поддерживает все VK, включая XBUTTONs.
        if (ksFPS.poll(kb.vkFPS))     g_showFPS = !g_showFPS.load();  // тоглим FPS-оверлей по переднему фронту
        if (ksVSync.poll(kb.vkVSync)) g_vsync   = !g_vsync.load();    // тоглим VSync
        if (ksExit.poll(kb.vkExit))   g_running = false;              // выход

        // 3. Захват и вывод кадра
        cv::Mat* framePtr = tb.tryRead();
        if (!framePtr || framePtr->empty()) { Sleep(1); continue; }  // кадра ещё нет - ждем 1мс и пробуем снова

        auto   now = std::chrono::steady_clock::now();
        double fps = 1e9 / static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                now - prevTime).count());  // FPS = 1 секунда / время между кадрами в наносекундах
        prevTime = now;

        if (g_showFPS)
            drawFPS(framePtr, fps, fourccStr);  // рисуем FPS прямо в буфер кадра перед апгрейдом текстуры

        dx.uploadFrame(*framePtr);  // копируем кадр в GPU-текстуру через Map/memcpy/Unmap
        dx.render();                // биндим шейдеры, устанавливаем viewport, Draw(3), Present
    }

    // ── Очистка ───────────────────────────────────────────────────────────────
    dx.release();                // освобождаем все DX11 объекты
    vs.stop();                   // останавливаем поток захвата и отпускаем устройство
    DestroyWindow(hwnd);         // уничтожаем окно
    showCursor();                // возвращаем курсор - вежливо
    allowSleep();                // снимаем запрет сна
    if (mmh) AvRevertMmThreadCharacteristics(mmh);  // откатываем MMCSS-регистрацию главного потока

    CoUninitialize();            // завершаем COM - должно быть последним среди COM-операций
    std::cout << "[INFO] Session ended.\n";
    return 0;
}
