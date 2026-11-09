#include "touchremap.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <string>
#include <vector>

#include <windows.h>
#include <cfgmgr32.h>

#include "hooks/graphics/graphics.h"
#include "util/logging.h"
#include "util/utils.h"

namespace touchremap {

    // the key written by "MultiDigiMon -touch"
    static const wchar_t *REG_KEY_CLI = L"HKCU\\Software\\Microsoft\\Wisp\\Pen\\Digimon";
    static const wchar_t *REG_SUBKEY = L"Software\\Microsoft\\Wisp\\Pen\\Digimon";

    // timing
    static constexpr DWORD DISABLE_SETTLE_MS = 2000; // same delay that worked in the PowerShell version
    static constexpr DWORD ENABLE_POLL_MS = 5000;
    static constexpr DWORD REG_TIMEOUT_MS = 5000;
    static constexpr unsigned int RESTORE_BUDGET_MS = 12000;

    // HID usage for a touch screen (usage page 0x0D = Digitizers, usage 0x04 = Touch Screen)
    static constexpr USHORT HID_USAGE_PAGE_DIGITIZER = 0x0D;
    static constexpr USHORT HID_USAGE_TOUCH_SCREEN = 0x04;

    // state
    static std::atomic<bool> ACTIVE { false };
    static std::wstring ACTIVE_DIR;

    /*
     * small file helpers (raw wchar_t content, we only ever read back what we wrote)
     */

    static bool file_exists(const std::wstring &path) {
        return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    static bool write_wide_file(const std::wstring &path, const std::wstring &content) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }
        DWORD written = 0;
        const DWORD size = (DWORD) (content.size() * sizeof(wchar_t));
        const bool ok = WriteFile(file, content.data(), size, &written, nullptr) && written == size;
        CloseHandle(file);
        return ok;
    }

    static bool read_wide_file(const std::wstring &path, std::wstring &content) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }
        const DWORD size = GetFileSize(file, nullptr);
        if (size == INVALID_FILE_SIZE || size > 64 * 1024) {
            CloseHandle(file);
            return false;
        }
        std::wstring result(size / sizeof(wchar_t), L'\0');
        DWORD read = 0;
        const bool ok = ReadFile(file, result.data(), size - (size % sizeof(wchar_t)), &read, nullptr) != FALSE;
        CloseHandle(file);
        if (ok) {
            content = std::move(result);
        }
        return ok;
    }

    /*
     * where the "normal" snapshot and the pending-restore state live
     */

    static std::wstring state_dir() {
        wchar_t buf[MAX_PATH] {};
        std::wstring dir;
        DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            dir = std::wstring(buf) + L"\\spice2x";
            CreateDirectoryW(dir.c_str(), nullptr); // fine if it already exists
        } else {
            len = GetTempPathW(MAX_PATH, buf);
            dir = (len > 0 && len < MAX_PATH) ? std::wstring(buf) : L".";
            while (dir.size() > 1 && (dir.back() == L'\\' || dir.back() == L'/')) {
                dir.pop_back();
            }
        }
        return dir;
    }

    static std::wstring state_path(const std::wstring &dir) {
        return dir + L"\\touchremap_state.txt";
    }

    static std::wstring backup_path(const std::wstring &dir) {
        return dir + L"\\touchremap_normal.reg";
    }

    /*
     * state file format (two lines):
     *   <device instance id>
     *   backup | none        (whether the Digimon key existed before we touched it)
     */
    struct State {
        std::wstring device;
        bool had_key = false;
    };

    static bool write_state(const std::wstring &dir, const State &state) {
        return write_wide_file(state_path(dir),
                state.device + L"\n" + (state.had_key ? L"backup" : L"none") + L"\n");
    }

    static bool read_state(const std::wstring &dir, State &state) {
        std::wstring content;
        if (!read_wide_file(state_path(dir), content)) {
            return false;
        }
        const auto newline = content.find(L'\n');
        if (newline == std::wstring::npos) {
            return false;
        }
        state.device = content.substr(0, newline);
        while (!state.device.empty() && (state.device.back() == L'\r' || state.device.back() == L' ')) {
            state.device.pop_back();
        }
        state.had_key = content.compare(newline + 1, 6, L"backup") == 0;
        return !state.device.empty();
    }

    static void clear_state(const std::wstring &dir) {
        DeleteFileW(state_path(dir).c_str());
        DeleteFileW(backup_path(dir).c_str());
    }

    /*
     * registry, via reg.exe (same tool the manual workflow used)
     */

    static bool run_reg(const std::wstring &args) {
        wchar_t sys[MAX_PATH] {};
        const UINT len = GetSystemDirectoryW(sys, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) {
            return false;
        }
        const std::wstring exe = std::wstring(sys) + L"\\reg.exe";
        std::wstring cmd = L"\"" + exe + L"\" " + args;
        std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
        cmd_buf.push_back(L'\0');

        STARTUPINFOW si {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi {};
        if (!CreateProcessW(exe.c_str(), cmd_buf.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            return false;
        }

        DWORD exit_code = 1;
        const DWORD wait = WaitForSingleObject(pi.hProcess, REG_TIMEOUT_MS);
        if (wait == WAIT_OBJECT_0) {
            GetExitCodeProcess(pi.hProcess, &exit_code);
        } else {
            TerminateProcess(pi.hProcess, 1);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return wait == WAIT_OBJECT_0 && exit_code == 0;
    }

    static bool reg_key_exists() {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_SUBKEY, 0, KEY_READ, &key) == ERROR_SUCCESS) {
            RegCloseKey(key);
            return true;
        }
        return false;
    }

    static bool reg_export(const std::wstring &file) {
        return run_reg(std::wstring(L"export \"") + REG_KEY_CLI + L"\" \"" + file + L"\" /y");
    }

    static bool reg_import(const std::wstring &file) {
        return run_reg(L"import \"" + file + L"\"");
    }

    static bool reg_delete() {
        return run_reg(std::wstring(L"delete \"") + REG_KEY_CLI + L"\" /f");
    }

    /*
     * device handling (cfgmgr32 is loaded dynamically; no extra link dependency)
     */

    typedef CONFIGRET (WINAPI *CM_Locate_DevNodeW_t)(PDEVINST, DEVINSTID_W, ULONG);
    typedef CONFIGRET (WINAPI *CM_Disable_DevNode_t)(DEVINST, ULONG);
    typedef CONFIGRET (WINAPI *CM_Enable_DevNode_t)(DEVINST, ULONG);
    typedef CONFIGRET (WINAPI *CM_Get_DevNode_Status_t)(PULONG, PULONG, DEVINST, ULONG);

    struct CmApi {
        CM_Locate_DevNodeW_t locate = nullptr;
        CM_Disable_DevNode_t disable = nullptr;
        CM_Enable_DevNode_t enable = nullptr;
        CM_Get_DevNode_Status_t status = nullptr;

        bool load() {
            HMODULE cm = LoadLibraryW(L"cfgmgr32.dll");
            if (!cm) {
                return false;
            }
            locate = reinterpret_cast<CM_Locate_DevNodeW_t>(GetProcAddress(cm, "CM_Locate_DevNodeW"));
            disable = reinterpret_cast<CM_Disable_DevNode_t>(GetProcAddress(cm, "CM_Disable_DevNode"));
            enable = reinterpret_cast<CM_Enable_DevNode_t>(GetProcAddress(cm, "CM_Enable_DevNode"));
            status = reinterpret_cast<CM_Get_DevNode_Status_t>(GetProcAddress(cm, "CM_Get_DevNode_Status"));
            return locate && disable && enable && status;
        }
    };

    static bool is_admin() {
        HMODULE shell32 = LoadLibraryW(L"shell32.dll");
        if (!shell32) {
            return true;
        }
        const auto fn = reinterpret_cast<BOOL (WINAPI *)()>(GetProcAddress(shell32, "IsUserAnAdmin"));
        return fn == nullptr || fn() != FALSE;
    }

    static bool device_exists(const std::wstring &device_id) {
        CmApi cm;
        if (!cm.load()) {
            return false;
        }
        DEVINST inst = 0;
        return cm.locate(&inst, const_cast<wchar_t *>(device_id.c_str()), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS;
    }

    // disable + enable the device so Windows re-reads the digitizer mapping
    static bool cycle_device(const std::wstring &device_id) {
        CmApi cm;
        if (!cm.load()) {
            log_warning("touchremap", "cfgmgr32 is not available");
            return false;
        }

        DEVINST inst = 0;
        auto cr = cm.locate(&inst, const_cast<wchar_t *>(device_id.c_str()), CM_LOCATE_DEVNODE_NORMAL);
        if (cr != CR_SUCCESS) {
            log_warning("touchremap", "device not found: {} (CONFIGRET 0x{:x})", ws2s(device_id), (unsigned) cr);
            return false;
        }

        // skip the disable step if a previous attempt already left it disabled
        ULONG status = 0, problem = 0;
        const bool already_disabled =
                cm.status(&status, &problem, inst, 0) == CR_SUCCESS &&
                (status & DN_HAS_PROBLEM) && problem == CM_PROB_DISABLED;

        if (!already_disabled) {
            cr = cm.disable(inst, 0);
            if (cr != CR_SUCCESS) {
                if (cr == CR_ACCESS_DENIED) {
                    log_warning("touchremap", "access denied while disabling the touch screen; "
                            "run spice as administrator");
                } else {
                    log_warning("touchremap", "failed to disable device (CONFIGRET 0x{:x})", (unsigned) cr);
                }
                return false;
            }
            Sleep(DISABLE_SETTLE_MS);
        }

        // a device left disabled means a dead touch screen, so retry the enable a few times
        bool enabled = false;
        for (int attempt = 0; attempt < 5; attempt++) {
            cr = cm.enable(inst, 0);
            if (cr == CR_SUCCESS) {
                enabled = true;
                break;
            }
            Sleep(500);
        }
        if (!enabled) {
            log_warning("touchremap", "FAILED to re-enable the touch screen (CONFIGRET 0x{:x}); "
                    "enable it manually in Device Manager. spice will retry on next launch.", (unsigned) cr);
            return false;
        }

        // wait for the device to come back
        for (DWORD waited = 0; waited < ENABLE_POLL_MS; waited += 100) {
            if (cm.status(&status, &problem, inst, 0) == CR_SUCCESS &&
                (status & DN_STARTED) && !(status & DN_HAS_PROBLEM)) {
                break;
            }
            Sleep(100);
        }
        Sleep(300);
        return true;
    }

    /*
     * touch screen discovery via raw input:
     *   \\?\HID#VID_2A94&PID_564D&MI_00#9&2b0bef74&0&0000#{guid}
     * becomes the device instance ID:
     *   HID\VID_2A94&PID_564D&MI_00\9&2B0BEF74&0&0000
     */

    static std::wstring instance_id_from_interface_path(std::wstring path) {
        if (path.rfind(L"\\\\?\\", 0) == 0) {
            path.erase(0, 4);
        }
        const auto guid = path.rfind(L"#{");
        if (guid != std::wstring::npos) {
            path.resize(guid);
        }
        for (auto &c : path) {
            c = (c == L'#') ? L'\\' : (wchar_t) towupper(c);
        }
        return path;
    }

    static std::vector<std::wstring> find_touchscreens() {
        std::vector<std::wstring> result;

        UINT count = 0;
        if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) == (UINT) -1 || count == 0) {
            return result;
        }
        std::vector<RAWINPUTDEVICELIST> list(count);
        count = GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST));
        if (count == (UINT) -1) {
            return result;
        }
        list.resize(count);

        for (const auto &entry : list) {
            if (entry.dwType != RIM_TYPEHID) {
                continue;
            }

            RID_DEVICE_INFO info {};
            info.cbSize = sizeof(info);
            UINT info_size = sizeof(info);
            if (GetRawInputDeviceInfoW(entry.hDevice, RIDI_DEVICEINFO, &info, &info_size) == (UINT) -1) {
                continue;
            }
            if (info.hid.usUsagePage != HID_USAGE_PAGE_DIGITIZER ||
                info.hid.usUsage != HID_USAGE_TOUCH_SCREEN) {
                continue;
            }

            UINT name_len = 0;
            if (GetRawInputDeviceInfoW(entry.hDevice, RIDI_DEVICENAME, nullptr, &name_len) == (UINT) -1 ||
                name_len == 0) {
                continue;
            }
            std::wstring name(name_len, L'\0');
            if (GetRawInputDeviceInfoW(entry.hDevice, RIDI_DEVICENAME, name.data(), &name_len) == (UINT) -1) {
                continue;
            }
            while (!name.empty() && name.back() == L'\0') {
                name.pop_back();
            }
            result.push_back(instance_id_from_interface_path(name));
        }

        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    static std::wstring resolve_device(const std::string &device_override) {

        // explicit setting wins
        std::string id = device_override;
        while (!id.empty() && (id.front() == ' ' || id.front() == '\t' || id.front() == '"')) {
            id.erase(id.begin());
        }
        while (!id.empty() && (id.back() == ' ' || id.back() == '\t' || id.back() == '"')) {
            id.pop_back();
        }
        if (!id.empty()) {
            return s2ws(id);
        }

        // otherwise auto-detect
        const auto candidates = find_touchscreens();
        if (candidates.size() == 1) {
            log_info("touchremap", "auto-detected touch screen: {}", ws2s(candidates[0]));
            return candidates[0];
        }
        if (candidates.empty()) {
            log_warning("touchremap", "no touch screen found; set -touchremapdevice to its instance ID");
        } else {
            log_warning("touchremap", "multiple touch screens found; set -touchremapdevice to one of:");
            for (const auto &candidate : candidates) {
                log_warning("touchremap", "    {}", ws2s(candidate));
            }
        }
        return L"";
    }

    /*
     * restore
     */

    static bool restore_registry(const std::wstring &dir, bool had_key) {
        if (had_key) {
            const auto backup = backup_path(dir);
            if (!file_exists(backup)) {
                log_warning("touchremap", "normal mapping backup is missing: {}", ws2s(backup));
                return false;
            }

            // delete first so values that only exist in the game mapping do not linger
            reg_delete();
            if (!reg_import(backup)) {
                log_warning("touchremap", "failed to import normal mapping from {}", ws2s(backup));
                return false;
            }
            return true;
        }

        // the key did not exist before we applied the game mapping
        return !reg_key_exists() || reg_delete();
    }

    static void restore_from_state(const std::wstring &dir, bool cycle) {
        State state;
        if (!read_state(dir, state)) {
            return;
        }

        const bool registry_ok = restore_registry(dir, state.had_key);
        const bool device_ok = !cycle || cycle_device(state.device);

        if (registry_ok && device_ok) {
            clear_state(dir);
            log_info("touchremap", "normal touch mapping restored");
        } else {
            log_warning("touchremap", "could not fully restore the normal touch mapping; "
                    "will retry next launch (backup: {})", ws2s(backup_path(dir)));
        }
    }

    /*
     * public API
     */

    void recover_pending() {
        if (ACTIVE) {
            return;
        }
        const auto dir = state_dir();
        State state;
        if (!read_state(dir, state)) {
            return;
        }

        log_warning("touchremap", "previous session did not restore the normal touch mapping, restoring now");
        if (!is_admin()) {
            log_warning("touchremap", "cannot restore without administrator rights; "
                    "run spice as administrator once, or re-import {}", ws2s(backup_path(dir)));
            return;
        }
        restore_from_state(dir, true);
    }

    void apply(const std::string &game_reg_path, const std::string &device_override) {
        if (ACTIVE) {
            return;
        }

        // fullscreen only
        if (GRAPHICS_WINDOWED) {
            log_info("touchremap", "windowed mode; touch remap is skipped");
            return;
        }

        if (game_reg_path.empty()) {
            log_warning("touchremap", "no game mapping set; point -touchremapreg at the .reg file "
                    "exported while the wizard mapping was active");
            return;
        }
        const auto game_reg = s2ws(game_reg_path);
        if (!file_exists(game_reg)) {
            log_warning("touchremap", "game mapping file not found: {}", game_reg_path);
            return;
        }

        // disabling a device requires elevation; bail out before touching anything
        if (!is_admin()) {
            log_warning("touchremap", "administrator rights are required to disable/enable the touch "
                    "screen; touch remap is skipped. run spice as administrator.");
            return;
        }

        const auto device = resolve_device(device_override);
        if (device.empty()) {
            return;
        }
        if (!device_exists(device)) {
            log_warning("touchremap", "touch screen device not found: {}", ws2s(device));
            return;
        }

        // snapshot the normal mapping
        const auto dir = state_dir();
        State state;
        state.device = device;
        state.had_key = reg_key_exists();
        if (state.had_key && !reg_export(backup_path(dir))) {
            log_warning("touchremap", "failed to back up the current touch mapping; touch remap is skipped");
            return;
        }
        if (!write_state(dir, state)) {
            log_warning("touchremap", "failed to write state file in {}; touch remap is skipped", ws2s(dir));
            clear_state(dir);
            return;
        }

        // from here on, restore() has something to undo
        ACTIVE_DIR = dir;
        ACTIVE = true;

        log_info("touchremap", "applying game touch mapping from {}", game_reg_path);
        if (!reg_import(game_reg)) {
            log_warning("touchremap", "failed to import game mapping; rolling back");
            ACTIVE = false;
            restore_from_state(dir, false);
            return;
        }
        if (!cycle_device(device)) {
            log_warning("touchremap", "failed to restart the touch screen; rolling back");
            ACTIVE = false;
            restore_from_state(dir, true);
            return;
        }

        log_info("touchremap", "game touch mapping active for {}", ws2s(device));
    }

    void restore() {
        if (!ACTIVE.exchange(false)) {
            return;
        }
        log_info("touchremap", "restoring normal touch mapping");
        restore_from_state(ACTIVE_DIR, true);
    }

    unsigned int restore_budget_ms() {
        return ACTIVE ? RESTORE_BUDGET_MS : 0;
    }
}
