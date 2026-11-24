// enable Windows 8 pointer and touch injection types; the functions are loaded dynamically
// so spice2x keeps running on older systems
#define _WIN32_WINNT 0x0602

#include "touch2main.h"

#include <atomic>
#include <thread>

#include <windows.h>

#include "hooks/graphics/graphics.h"
#include "touch/native/inject_internal.h"
#include "util/libutils.h"
#include "util/logging.h"

#ifndef WM_POINTERUPDATE
#define WM_POINTERUPDATE 0x0245
#endif
#ifndef WM_POINTERDOWN
#define WM_POINTERDOWN 0x0246
#endif
#ifndef WM_POINTERUP
#define WM_POINTERUP 0x0247
#endif
#ifndef WM_POINTERACTIVATE
#define WM_POINTERACTIVATE 0x024B
#endif
#ifndef PA_NOACTIVATE
#define PA_NOACTIVATE 3
#endif
#ifndef GET_POINTERID_WPARAM
#define GET_POINTERID_WPARAM(wParam) (LOWORD(wParam))
#endif

namespace touch2main {

    bool ENABLED = false;

    uint32_t injection_contact_count() {
        return ENABLED ? MAX_CONTACTS + 1 : 1;
    }

    namespace {

        // Windows 8+ / 10+ APIs, resolved at runtime
        using GetPointerInfo_t = BOOL (WINAPI *)(UINT32, POINTER_INFO *);
        using GetPointerDeviceRects_t = BOOL (WINAPI *)(HANDLE, RECT *, RECT *);
        using InjectTouchInput_t = BOOL (WINAPI *)(UINT32, const POINTER_TOUCH_INFO *);
        using SetThreadDpiAwarenessContext_t = HANDLE (WINAPI *)(HANDLE);

        // window functions are taken straight from user32, because the graphics module IAT-hooks
        // CreateWindowExW/ShowWindow/SetWindowPos/RegisterClassW in every loaded module (spice
        // included) to manage game windows; the overlay must not go through those hooks
        using CreateWindowExW_t = decltype(CreateWindowExW) *;
        using RegisterClassW_t = decltype(RegisterClassW) *;
        using ShowWindow_t = decltype(ShowWindow) *;
        using SetWindowPos_t = decltype(SetWindowPos) *;

        GetPointerInfo_t GetPointerInfo_ptr = nullptr;
        GetPointerDeviceRects_t GetPointerDeviceRects_ptr = nullptr;
        InjectTouchInput_t InjectTouchInput_ptr = nullptr;
        SetThreadDpiAwarenessContext_t SetThreadDpiAwarenessContext_ptr = nullptr;
        CreateWindowExW_t CreateWindowExW_ptr = nullptr;
        RegisterClassW_t RegisterClassW_ptr = nullptr;
        ShowWindow_t ShowWindow_ptr = nullptr;
        SetWindowPos_t SetWindowPos_ptr = nullptr;

        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        const HANDLE DPI_PER_MONITOR_AWARE_V2 =
            reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-4));

        constexpr UINT_PTR TIMER_ID = 1;
        constexpr UINT TIMER_INTERVAL_MS = 1000;

        std::atomic<bool> started { false };

        HWND overlay = nullptr;
        HMONITOR touch_monitor = nullptr;
        RECT touch_rect {};     // touchscreen monitor, physical pixels
        RECT main_rect {};      // primary display, physical pixels
        bool standing_down = false;
        int32_t logged_touches = 0;

        struct Contact {
            bool used;
            UINT32 source_id;
            POINTER_TOUCH_INFO info;
        };
        Contact contacts[MAX_CONTACTS] {};

        BOOL CALLBACK enum_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM) {
            MONITORINFO info {};
            info.cbSize = sizeof(info);
            if (!GetMonitorInfoW(monitor, &info)) {
                return TRUE;
            }
            if (info.dwFlags & MONITORINFOF_PRIMARY) {
                main_rect = info.rcMonitor;
            } else if (touch_monitor == nullptr) {
                // the first non-primary monitor is taken as the touchscreen
                touch_monitor = monitor;
                touch_rect = info.rcMonitor;
            }
            return TRUE;
        }

        bool update_monitors() {
            touch_monitor = nullptr;
            touch_rect = {};
            main_rect = {};
            EnumDisplayMonitors(nullptr, nullptr, enum_monitor, 0);

            // the primary display always starts at (0, 0); take its real size from the display
            // mode, which is in physical pixels regardless of DPI scaling
            DEVMODEW mode {};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) &&
                mode.dmPelsWidth > 0 && mode.dmPelsHeight > 0) {
                main_rect = { 0, 0, (LONG) mode.dmPelsWidth, (LONG) mode.dmPelsHeight };
            }

            log_info("touch2main", "primary display: {}x{}, touchscreen monitor: [{}, {}, {}, {}]",
                    main_rect.right - main_rect.left, main_rect.bottom - main_rect.top,
                    touch_rect.left, touch_rect.top, touch_rect.right, touch_rect.bottom);

            return touch_monitor != nullptr &&
                !IsRectEmpty(&main_rect) && !IsRectEmpty(&touch_rect);
        }

        double clamp_unit(double value) {
            if (value < 0.0) {
                return 0.0;
            }
            if (value > 1.0) {
                return 1.0;
            }
            return value;
        }

        POINT scale_to_main(double nx, double ny) {
            nx = clamp_unit(nx);
            ny = clamp_unit(ny);
            const LONG width = main_rect.right - main_rect.left;
            const LONG height = main_rect.bottom - main_rect.top;
            return {
                main_rect.left + (LONG) (nx * (width - 1) + 0.5),
                main_rect.top + (LONG) (ny * (height - 1) + 0.5),
            };
        }

        // map a touch on the touchscreen to a physical pixel on the primary display
        POINT map_pointer(const POINTER_INFO &info) {
            RECT device {}, display {};
            if (GetPointerDeviceRects_ptr != nullptr &&
                GetPointerDeviceRects_ptr(info.sourceDevice, &device, &display) &&
                device.right > device.left && device.bottom > device.top) {

                // raw digitizer position, not affected by Windows display scaling
                const double nx = double(info.ptHimetricLocationRaw.x - device.left) /
                    double(device.right - device.left);
                const double ny = double(info.ptHimetricLocationRaw.y - device.top) /
                    double(device.bottom - device.top);
                const auto mapped = scale_to_main(nx, ny);

                if (logged_touches < 3) {
                    logged_touches++;
                    log_misc("touch2main", "touch raw ({}, {}) in [{}, {}, {}, {}] -> ({}, {})",
                            info.ptHimetricLocationRaw.x, info.ptHimetricLocationRaw.y,
                            device.left, device.top, device.right, device.bottom,
                            mapped.x, mapped.y);
                }
                return mapped;
            }

            // fallback: pixel position relative to the touchscreen monitor
            const LONG width = touch_rect.right - touch_rect.left;
            const LONG height = touch_rect.bottom - touch_rect.top;
            return scale_to_main(
                    double(info.ptPixelLocation.x - touch_rect.left) / width,
                    double(info.ptPixelLocation.y - touch_rect.top) / height);
        }

        // submit one frame with every active contact, then advance their state
        void inject_all() {
            POINTER_TOUCH_INFO frame[MAX_CONTACTS];
            UINT32 count = 0;
            for (auto &contact : contacts) {
                if (contact.used) {
                    frame[count++] = contact.info;
                }
            }
            if (count > 0 && !InjectTouchInput_ptr(count, frame)) {
                static bool error_logged = false;
                if (!error_logged) {
                    error_logged = true;
                    log_warning("touch2main", "failed to inject touch input: {}", GetLastError());
                }
            }

            // lifted contacts are dropped, the rest become updates for the next frame
            for (auto &contact : contacts) {
                if (!contact.used) {
                    continue;
                }
                if (contact.info.pointerInfo.pointerFlags & POINTER_FLAG_UP) {
                    contact.used = false;
                } else {
                    contact.info.pointerInfo.pointerFlags =
                        POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
                }
            }
        }

        void release_all() {
            bool any = false;
            for (auto &contact : contacts) {
                if (contact.used) {
                    contact.info.pointerInfo.pointerFlags = POINTER_FLAG_UP;
                    any = true;
                }
            }
            if (any) {
                inject_all();
            }
        }

        void on_pointer(UINT message, WPARAM w_param) {
            const UINT32 id = GET_POINTERID_WPARAM(w_param);
            POINTER_INFO info {};
            if (!GetPointerInfo_ptr(id, &info) || info.pointerType != PT_TOUCH) {
                return;
            }

            int32_t slot = -1;
            for (uint32_t i = 0; i < MAX_CONTACTS; i++) {
                if (contacts[i].used && contacts[i].source_id == id) {
                    slot = static_cast<int32_t>(i);
                    break;
                }
            }
            if (message == WM_POINTERDOWN && slot < 0) {
                for (uint32_t i = 0; i < MAX_CONTACTS; i++) {
                    if (!contacts[i].used) {
                        slot = static_cast<int32_t>(i);
                        break;
                    }
                }
            }
            if (slot < 0) {
                return;
            }

            auto &contact = contacts[slot];
            if (message == WM_POINTERDOWN) {
                ZeroMemory(&contact, sizeof(contact));
                contact.used = true;
                contact.source_id = id;
            }

            auto &touch = contact.info;
            touch.pointerInfo.pointerType = PT_TOUCH;

            // pointer ID 0 belongs to spice2x's own synthetic/mouse contact
            touch.pointerInfo.pointerId = slot + 1;
            touch.pointerInfo.ptPixelLocation = map_pointer(info);
            touch.touchFlags = TOUCH_FLAG_NONE;
            touch.touchMask = TOUCH_MASK_CONTACTAREA;
            const auto position = touch.pointerInfo.ptPixelLocation;
            touch.rcContact = { position.x - 2, position.y - 2, position.x + 2, position.y + 2 };
            if (message == WM_POINTERDOWN) {
                touch.pointerInfo.pointerFlags =
                    POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
            } else if (message == WM_POINTERUP) {
                touch.pointerInfo.pointerFlags = POINTER_FLAG_UP;
            } else {
                touch.pointerInfo.pointerFlags =
                    POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
            }
            inject_all();
        }

        void place_overlay() {
            SetWindowPos_ptr(overlay, HWND_TOPMOST,
                    touch_rect.left, touch_rect.top,
                    touch_rect.right - touch_rect.left, touch_rect.bottom - touch_rect.top,
                    SWP_NOACTIVATE);
        }

        // a game subscreen window placed on the touchscreen takes touches natively,
        // so touch2main must not cover it
        bool subscreen_window_on_touchscreen() {
            HWND windows[] = {
                GRAPHICS_IIDX_WSUB ? TDJ_SUBSCREEN_WINDOW : nullptr,
                SDVX_SUBSCREEN_WINDOW,
                POPN_SUBSCREEN_WINDOW,
                GFDM_SUBSCREEN_WINDOW,
            };
            for (auto window : windows) {
                if (window != nullptr && IsWindow(window) && IsWindowVisible(window) &&
                    MonitorFromWindow(window, MONITOR_DEFAULTTONULL) == touch_monitor) {
                    return true;
                }
            }
            return false;
        }

        void on_timer() {
            const bool stand_down = subscreen_window_on_touchscreen();
            if (stand_down != standing_down) {
                standing_down = stand_down;
                if (stand_down) {
                    log_info("touch2main", "game subscreen window is on the touchscreen, pausing");
                    release_all();
                    ShowWindow_ptr(overlay, SW_HIDE);
                } else {
                    log_info("touch2main", "resuming");
                    ShowWindow_ptr(overlay, SW_SHOWNOACTIVATE);
                }
            }

            // stay above other topmost windows on the touchscreen
            if (!standing_down) {
                SetWindowPos_ptr(overlay, HWND_TOPMOST, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }

        LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
            switch (message) {
                case WM_POINTERACTIVATE:
                    return PA_NOACTIVATE;
                case WM_POINTERDOWN:
                case WM_POINTERUPDATE:
                case WM_POINTERUP:
                    // handled and swallowed, so Windows does not promote it to mouse input
                    on_pointer(message, w_param);
                    return 0;
                case WM_TIMER:
                    if (w_param == TIMER_ID) {
                        on_timer();
                    }
                    return 0;
                case WM_DISPLAYCHANGE:
                    // fullscreen mode switches and monitor changes move the screens around
                    release_all();
                    if (update_monitors()) {
                        place_overlay();
                    } else {
                        log_warning("touch2main", "touchscreen monitor lost, pausing");
                        standing_down = true;
                        ShowWindow_ptr(overlay, SW_HIDE);
                    }
                    return 0;
                default:
                    break;
            }
            return DefWindowProcW(window, message, w_param, l_param);
        }

        bool resolve_functions() {
            const auto user32 = libutils::load_library("user32.dll");
            GetPointerInfo_ptr = libutils::try_proc<GetPointerInfo_t>(
                    user32, "GetPointerInfo");
            GetPointerDeviceRects_ptr = libutils::try_proc<GetPointerDeviceRects_t>(
                    user32, "GetPointerDeviceRects");
            InjectTouchInput_ptr = libutils::try_proc<InjectTouchInput_t>(
                    user32, "InjectTouchInput");
            SetThreadDpiAwarenessContext_ptr =
                libutils::try_proc<SetThreadDpiAwarenessContext_t>(
                    user32, "SetThreadDpiAwarenessContext");
            CreateWindowExW_ptr = libutils::try_proc<CreateWindowExW_t>(
                    user32, "CreateWindowExW");
            RegisterClassW_ptr = libutils::try_proc<RegisterClassW_t>(
                    user32, "RegisterClassW");
            ShowWindow_ptr = libutils::try_proc<ShowWindow_t>(user32, "ShowWindow");
            SetWindowPos_ptr = libutils::try_proc<SetWindowPos_t>(user32, "SetWindowPos");

            return GetPointerInfo_ptr != nullptr && InjectTouchInput_ptr != nullptr &&
                CreateWindowExW_ptr != nullptr && RegisterClassW_ptr != nullptr &&
                ShowWindow_ptr != nullptr && SetWindowPos_ptr != nullptr;
        }

        void worker() {
            if (!resolve_functions()) {
                log_warning("touch2main",
                        "pointer/touch injection API unavailable (needs Windows 8+), disabled");
                return;
            }

            // physical pixel coordinates for this thread only; the game's DPI behaviour
            // is untouched
            if (SetThreadDpiAwarenessContext_ptr == nullptr ||
                SetThreadDpiAwarenessContext_ptr(DPI_PER_MONITOR_AWARE_V2) == nullptr) {
                log_warning("touch2main",
                        "per-monitor DPI awareness unavailable, scaled displays may be off");
            }

            if (!update_monitors()) {
                log_warning("touch2main", "no secondary (touchscreen) monitor found, disabled");
                return;
            }

            // shared with spice2x's own touch injection, which is initialized once per process
            if (!nativetouch::inject::initialize_touch_injection()) {
                log_warning("touch2main", "touch injection unavailable, disabled");
                return;
            }

            const auto instance = GetModuleHandleW(nullptr);
            WNDCLASSW window_class {};
            window_class.lpfnWndProc = window_proc;
            window_class.hInstance = instance;
            window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
            window_class.lpszClassName = L"spice2x_touch2main";
            RegisterClassW_ptr(&window_class);

            overlay = CreateWindowExW_ptr(
                    WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                    window_class.lpszClassName, L"spice2x touch2main", WS_POPUP,
                    touch_rect.left, touch_rect.top,
                    touch_rect.right - touch_rect.left, touch_rect.bottom - touch_rect.top,
                    nullptr, nullptr, instance, nullptr);
            if (overlay == nullptr) {
                log_warning("touch2main", "failed to create overlay window: {}", GetLastError());
                return;
            }

            // alpha 1: invisible, but still hit-tested and given touch input
            SetLayeredWindowAttributes(overlay, 0, 1, LWA_ALPHA);
            ShowWindow_ptr(overlay, SW_SHOWNOACTIVATE);
            SetTimer(overlay, TIMER_ID, TIMER_INTERVAL_MS, nullptr);
            log_info("touch2main", "active, touches on the touchscreen go to the primary display");

            MSG message;
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }

    void start() {
        if (!ENABLED || started.exchange(true)) {
            return;
        }
        std::thread(worker).detach();
    }
}
