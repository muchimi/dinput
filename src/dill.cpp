#include "dill.h"
#include <Xinput.h>
#include <SetupAPI.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <cwchar>
#include <cctype>
#include <iterator>
#include <cwctype>

#include <atomic>
#include <iostream>
#include <memory>
#include <string.h>
#include <sstream>
#include <algorithm>
#include <utility>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h" 
#include "spdlog/sinks/basic_file_sink.h"


#define HID_CLASSGUID {0x4d1e55b2, 0xf16f, 0x11cf,{ 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}}
#define CLS_NAME TEXT("GremlinInputListener")
#define HWND_MESSAGE ((HWND)-3)


namespace spd = spdlog;

// Setup logger
//auto logger = spd::stdout_color_mt("console");
auto logger = spd::basic_logger_mt("debug", "dill_debug.txt");

// DirectInput system handle
LPDIRECTINPUT8 g_direct_input = nullptr;

// Size of the device object read buffer
static const int g_buffer_size = 64;

// Storage for device data
static DeviceDataStore g_data_store;
// Enumeration, polling, and exported reads share the same native store.
// Python callbacks are dispatched after releasing this mutex to avoid GIL /
// application-lock inversions when callbacks call back into DILL.
static std::recursive_mutex g_data_mutex;
static thread_local std::vector<JoystickInputData> g_pending_inputs;
static thread_local std::vector<std::pair<DeviceSummary, DeviceActionType>> g_pending_devices;

// Callback handles
std::atomic<JoystickInputEventCallback> g_event_callback{nullptr};
std::atomic<DeviceChangeCallback> g_device_change_callback{nullptr};

// Thread handles
HANDLE g_message_thread = NULL;
HANDLE g_joystick_thread = NULL;

// Flag indicating whether or not device initialization is complete
std::atomic<bool> g_initialization_done = false;
static std::mutex g_lifecycle_mutex;
static HANDLE g_stop_event = nullptr;
static HANDLE g_ready_event = nullptr;
static std::atomic<DWORD> g_message_thread_id{0};
static std::atomic<HWND> g_message_window{nullptr};
static HDEVNOTIFY g_device_notification = nullptr;
static std::atomic<bool> g_running{false};
static std::atomic<HRESULT> g_startup_error{S_OK};
static thread_local bool g_native_worker = false;
static thread_local HRESULT g_last_error = S_OK;
static thread_local std::string g_last_error_text;
static thread_local HRESULT g_enumeration_callback_error = S_OK;
static std::uint64_t g_inventory_generation = 0; // guarded by g_data_mutex
static constexpr UINT_PTR kHotplugTimer = 1;
static constexpr UINT kHotplugDelayMs = 150;
static constexpr DWORD kApiVersion = 0x00020000;
static std::unordered_map<GUID, HRESULT> g_device_errors;
static std::unordered_map<GUID, DIDEVCAPS> g_device_capabilities;
struct PollFailure { HRESULT code = S_OK; ULONGLONG last_log = 0; DWORD suppressed = 0; };
static std::unordered_map<GUID, PollFailure> g_poll_failures;

using XInputReadState = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);
using XInputReadCapabilities = DWORD (WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);

// Windows' optional XInput 1.4 extension provides the hardware IDs needed for labels.
struct XInputExtendedCapabilities
{
    XINPUT_CAPABILITIES capabilities;
    WORD vendor_id;
    WORD product_id;
    WORD product_version;
    WORD reserved;
    DWORD extra;
};
static_assert(sizeof(XInputExtendedCapabilities) == 32, "XInput extended capabilities layout");
using XInputReadExtendedCapabilities = DWORD (WINAPI*)(DWORD, DWORD, DWORD, XInputExtendedCapabilities*);

static HMODULE g_xinput_module = nullptr;
static XInputReadState g_xinput_get_state = nullptr;
static XInputReadCapabilities g_xinput_get_capabilities = nullptr;
static XInputReadExtendedCapabilities g_xinput_get_extended_capabilities = nullptr;
static bool g_xinput_checked = false;
static ULONGLONG g_next_xinput_scan = 0;

#ifdef DILL_TESTING
static HRESULT (*g_test_enumerate)() = nullptr;
static std::atomic<int> g_test_notifications{0};
static std::atomic<int> g_test_timer_calls{0};
static HRESULT g_test_poll_startup = S_OK;
#endif
static void release_all_devices();
static void request_stop();

static BOOL api_error(HRESULT code, const char* operation)
{
    g_last_error = code;
    g_last_error_text = std::string(operation) + ": " + error_to_string(code);
    return FALSE;
}

static void api_success()
{
    g_last_error = S_OK;
    g_last_error_text.clear();
}



static void dispatch_input_events()
{
    auto events = std::move(g_pending_inputs);
    g_pending_inputs.clear();
    for (const auto& event : events)
    {
        auto callback = g_event_callback.load();
        if (callback) callback(event);
    }
}

static void dispatch_device_events()
{
    auto events = std::move(g_pending_devices);
    g_pending_devices.clear();
    for (const auto& event : events)
    {
        auto callback = g_device_change_callback.load();
        if (callback) callback(event.first, event.second);
    }
}

DeviceState::DeviceState()
    :   axis(9, 0)
      , button(129, false)
      , hat(5, -1)
{}


std::string error_to_string(DWORD error_code)
{
    static const std::unordered_map<DWORD, std::string> lut{
        // Success codes
        {DI_OK, "The operation completed successfully (DI_OK)"},
        {S_FALSE, "S_FALSE"},
        {DI_BUFFEROVERFLOW, "The device buffer overflowed.  Some input was lost. (DI_BUFFEROVERFLOW)"},
        {DI_PROPNOEFFECT, "The change in device properties had no effect. (DI_PROPNOEFFECT)"},
        // Error codes
        {E_HANDLE, "Invalid handle (E_HANDLE)"},
        {DIERR_INVALIDPARAM, "An invalid parameter was passed to the returning function, or the object was not in a state that admitted the function to be called (DIERR_INVALIDPARAM)"},
        {DIERR_NOTINITIALIZED, "This object has not been initialized (DIERR_NOTINITIALIZED)"},
        {DIERR_OTHERAPPHASPRIO, "Another app has a higher priority level, preventing this call from succeeding. (DIERR_OTHERAPPHASPRIO)"},
        {DIERR_ACQUIRED, "The operation cannot be performed while the device is acquired. (DIERR_ACQUIRED)"},
        {DIERR_DEVICENOTREG, "The device or device instance or effect is not registered with DirectInput. (DIERR_DEVICENOTREG)"},
        {DIERR_INPUTLOST, "Access to the device has been lost. It must be re-acquired. (DIERR_INPUTLOST)"},
        {DIERR_NOTACQUIRED, "The operation cannot be performed unless the device is acquired. (DIERR_NOTACQUIRED)"},
        {DIERR_NOTBUFFERED, "Attempted to read buffered device data from a device that is not buffered. (DIERR_NOTBUFFERED)"},
        {DIERR_NOINTERFACE, "The specified interface is not supported by the object (DIERR_NOINTERFACE)"},
        {DIERR_OBJECTNOTFOUND, "The requested object does not exist. (DIERR_OBJECTNOTFOUND)"},
        {DIERR_UNSUPPORTED, "The function called is not supported at this time (DIERR_UNSUPPORTED)"},
        {DI_POLLEDDEVICE, "The device is a polled device.  As a result, device buffering will not collect any data and event notifications will not be signalled until GetDeviceState is called. (DI_POLLEDDEVICE)"}
    };

    auto itr = lut.find(error_code);
    if(itr != lut.end())
    {
        return itr->second;
    }
    else
    {
        LPSTR message = nullptr;
        DWORD code = HRESULT_FACILITY(error_code) == FACILITY_WIN32 ? HRESULT_CODE(error_code) : error_code;
        DWORD size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<LPSTR>(&message), 0, nullptr);
        if (size && message)
        {
            std::string text(message, size);
            LocalFree(message);
            while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
            return text;
        }
        std::ostringstream text;
        text << "HRESULT 0x" << std::hex << error_code;
        return text.str();
    }
}

std::string guid_to_string(GUID guid)
{
    char text[39]{};
    sprintf_s(text, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        static_cast<unsigned long>(guid.Data1), static_cast<unsigned>(guid.Data2), static_cast<unsigned>(guid.Data3),
        static_cast<unsigned>(guid.Data4[0]), static_cast<unsigned>(guid.Data4[1]),
        static_cast<unsigned>(guid.Data4[2]), static_cast<unsigned>(guid.Data4[3]),
        static_cast<unsigned>(guid.Data4[4]), static_cast<unsigned>(guid.Data4[5]),
        static_cast<unsigned>(guid.Data4[6]), static_cast<unsigned>(guid.Data4[7]));
    return text;
}

BOOL on_create_window(HWND window_hdl, LPARAM l_param)
{
    auto params = reinterpret_cast<LPCREATESTRUCT>(l_param);
    DEV_BROADCAST_DEVICEINTERFACE notification{};
    notification.dbcc_size = sizeof(notification);
    notification.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    notification.dbcc_classguid = *reinterpret_cast<GUID*>(params->lpCreateParams);
    g_device_notification = RegisterDeviceNotification(window_hdl, &notification, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!g_device_notification)
    {
        g_startup_error = HRESULT_FROM_WIN32(GetLastError());
        return FALSE;
    }
    return TRUE;
}

BOOL on_device_change(LPARAM l_param, WPARAM w_param)
{
    if (!l_param || (w_param != DBT_DEVICEARRIVAL && w_param != DBT_DEVICEREMOVECOMPLETE))
        return TRUE;
    auto notification = reinterpret_cast<PDEV_BROADCAST_HDR>(l_param);
    if (notification->dbch_size >= sizeof(DEV_BROADCAST_HDR) &&
        notification->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
    {
        HWND window = g_message_window.load();
#ifdef DILL_TESTING
        ++g_test_notifications;
#endif
        if (window && !SetTimer(window, kHotplugTimer, kHotplugDelayMs, nullptr))
            logger->error("Hotplug timer creation failed: {}", GetLastError());
    }
    return TRUE;
}

LRESULT CALLBACK window_proc(HWND window_hdl, UINT msg_type, WPARAM w_param, LPARAM l_param)
{
    switch (msg_type)
    {
    case WM_CREATE:
        return on_create_window(window_hdl, l_param) ? 0 : -1;
    case WM_DEVICECHANGE:
        return on_device_change(l_param, w_param);
#ifdef DILL_TESTING
    case WM_APP + 1: // synthetic notifications avoid Win32 system-message marshalling
        return on_device_change(l_param, w_param);
#endif
    case WM_TIMER:
        if (w_param == kHotplugTimer)
        {
            KillTimer(window_hdl, kHotplugTimer);
#ifdef DILL_TESTING
            ++g_test_timer_calls;
#endif
            try { enumerate_devices(); }
            catch (const std::exception& error) { logger->error("Hotplug enumeration failed: {}", error.what()); }
            catch (...) { logger->error("Hotplug enumeration failed with an unknown error"); }
            return 0;
        }
        break;
    case WM_CLOSE:
        request_stop();
        return 0;
    }
    return DefWindowProc(window_hdl, msg_type, w_param, l_param);
}

void emit_joystick_input_event(DIDEVICEOBJECTDATA const& data, GUID const& guid)
{
    JoystickInputData evt{};
    evt.device_guid = guid;

    static std::unordered_map<DWORD, int> axis_id_lookup =
    {
        {offsetof(DIJOYSTATE2, lX), 1},
        {offsetof(DIJOYSTATE2, lY), 2},
        {offsetof(DIJOYSTATE2, lZ), 3},
        {offsetof(DIJOYSTATE2, lRx), 4},
        {offsetof(DIJOYSTATE2, lRy), 5},
        {offsetof(DIJOYSTATE2, lRz), 6},
        {offsetof(DIJOYSTATE2, rglSlider[0]), 7},
        {offsetof(DIJOYSTATE2, rglSlider[1]), 8}
    };
    static std::unordered_map<DWORD, int> hat_id_lookup = 
    {
        {offsetof(DIJOYSTATE2, rgdwPOV[0]), 1},
        {offsetof(DIJOYSTATE2, rgdwPOV[1]), 2},
        {offsetof(DIJOYSTATE2, rgdwPOV[2]), 3},
        {offsetof(DIJOYSTATE2, rgdwPOV[3]), 4}
    };

    // Figure out the type
    if(data.dwOfs < offsetof(DIJOYSTATE2, rgdwPOV))
    {
        evt.input_type = JoystickInputType::Axis;
        evt.input_index = axis_id_lookup[data.dwOfs];
        evt.value = data.dwData;
        g_data_store.state[guid].axis[evt.input_index] = evt.value;
    }
    else if(data.dwOfs < offsetof(DIJOYSTATE2, rgbButtons))
    {
        evt.input_type = JoystickInputType::Hat;
        evt.input_index = hat_id_lookup[data.dwOfs];
        evt.value = data.dwData;
        g_data_store.state[guid].hat[evt.input_index] = evt.value;
    }
    else if(data.dwOfs < offsetof(DIJOYSTATE2, lVX))
    {
        evt.input_type = JoystickInputType::Button;
        evt.input_index = static_cast<UINT8>(data.dwOfs - offsetof(DIJOYSTATE2, rgbButtons) + 1);
        evt.value = (data.dwData & 0x0080) == 0 ? 0 : 1;
        g_data_store.state[guid].button[evt.input_index] = evt.value == 0 ? false : true;
    }
    else
    {
        logger->warn(
            "{}: Unexpected type of input event occurred",
            guid_to_string(guid)
        );
    }

    if(g_event_callback != nullptr)
    {
        g_pending_inputs.push_back(evt);
    }
}

static bool report_poll_failure(GUID guid, HRESULT code, const char* operation)
{
    auto& failure = g_poll_failures[guid];
    ULONGLONG now = GetTickCount64();
    g_device_errors[guid] = code;
    if (failure.code == code && failure.last_log && now - failure.last_log < 5000)
    { ++failure.suppressed; return false; }
    logger->warn("{}: {} failed: {} (suppressed {} repeats)", guid_to_string(guid), operation,
                 error_to_string(code), failure.suppressed);
    failure.code = code;
    failure.last_log = now;
    failure.suppressed = 0;
    return true;
}

static void report_poll_recovery(GUID guid)
{
    if (g_poll_failures.erase(guid)) logger->info("{}: input polling recovered", guid_to_string(guid));
    g_device_errors[guid] = S_OK;
}

static HRESULT poll_or_reacquire(LPDIRECTINPUTDEVICE8 instance)
{
    HRESULT result = instance->Poll();
    if (FAILED(result))
    {
        result = instance->Acquire();
        if (SUCCEEDED(result)) result = instance->Poll();
    }
    return result;
}

void process_buffered_events(LPDIRECTINPUTDEVICE8 instance, GUID const& guid)
{
    HRESULT result = poll_or_reacquire(instance);
    if (FAILED(result)) { report_poll_failure(guid, result, "Poll/Acquire"); return; }
    DIDEVICEOBJECTDATA device_data[g_buffer_size]{};
    DWORD count = g_buffer_size;
    while (count == g_buffer_size)
    {
        count = g_buffer_size;
        result = instance->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), device_data, &count, 0);
        if (FAILED(result))
        {
            report_poll_failure(guid, result, "GetDeviceData");
            if (result == DIERR_NOTBUFFERED) g_data_store.is_buffered[guid] = false;
            return;
        }
        if (result == DI_BUFFEROVERFLOW) report_poll_failure(guid, result, "input buffer overflow");
        else report_poll_recovery(guid);
        for (DWORD index = 0; index < count; ++index) emit_joystick_input_event(device_data[index], guid);
    }
}

static void update_device_state(const DIJOYSTATE2& state, const GUID& guid)
{
    // Event structure to use with the callback
    JoystickInputData evt{};
    evt.device_guid = guid;

    // Detect and handle axis state changes
    for(size_t i=0; i<g_data_store.cache[guid].axis_count; ++i)
    {
        auto axis_index = g_data_store.cache[guid].axis_map[i].axis_index;
        if (axis_index < 1 || axis_index > 8) continue;
        auto value = 0;
        if     (axis_index == 1) { value = state.lX; }
        else if(axis_index == 2) { value = state.lY; }
        else if(axis_index == 3) { value = state.lZ; }
        else if(axis_index == 4) { value = state.lRx; }
        else if(axis_index == 5) { value = state.lRy; }
        else if(axis_index == 6) { value = state.lRz; }
        else if(axis_index == 7) { value = state.rglSlider[0]; }
        else if(axis_index == 8) { value = state.rglSlider[1]; }

        if(g_data_store.state[guid].axis[axis_index] != value)
        {
            evt.input_type = JoystickInputType::Axis;
            evt.input_index = static_cast<UINT8>(axis_index);
            evt.value = value;
            g_data_store.state[guid].axis[axis_index] = value;

            if(g_event_callback != nullptr)
            {
                g_pending_inputs.push_back(evt);
            }
        }
    }

    // Detect and handle button state changes
    for(size_t i=0; i<std::min<DWORD>(128, g_data_store.cache[guid].button_count); ++i)
    {
        auto is_pressed = (state.rgbButtons[i] & 0x0080) == 0 ? false : true;
        if(g_data_store.state[guid].button[i+1] != is_pressed)
        {
            evt.input_type = JoystickInputType::Button;
            evt.input_index = static_cast<UINT8>(i + 1);
            evt.value = is_pressed;
            g_data_store.state[guid].button[evt.input_index] = is_pressed;

            if(g_event_callback != nullptr)
            {
                g_pending_inputs.push_back(evt);
            }
        }
    }

    // Detect and handle hat state changes
    for(size_t i=0; i<std::min<DWORD>(4, g_data_store.cache[guid].hat_count); ++i)
    {
        LONG direction = state.rgdwPOV[i];
        if(direction < 0 || direction > 36000)
        {
            direction = -1;
        }
        if(g_data_store.state[guid].hat[i+1] != direction)
        {
            evt.input_type = JoystickInputType::Hat;
            evt.input_index = static_cast<UINT8>(i+1);
            evt.value = direction;
            g_data_store.state[guid].hat[evt.input_index] = evt.value;

            if(g_event_callback != nullptr)
            {
                g_pending_inputs.push_back(evt);
            }
        }
    }
}

void poll_device(LPDIRECTINPUTDEVICE8 instance, GUID const& guid)
{
    HRESULT result = poll_or_reacquire(instance);
    if (FAILED(result)) { report_poll_failure(guid, result, "Poll/Acquire"); return; }
    DIJOYSTATE2 state{};
    result = instance->GetDeviceState(sizeof(state), &state);
    if (FAILED(result)) { report_poll_failure(guid, result, "GetDeviceState"); return; }
    report_poll_recovery(guid);
    update_device_state(state, guid);
}

static void load_xinput()
{
    if (g_xinput_checked) return;
    g_xinput_checked = true;
    g_xinput_module = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_xinput_module) return;
    g_xinput_get_state = reinterpret_cast<XInputReadState>(GetProcAddress(g_xinput_module, "XInputGetState"));
    g_xinput_get_capabilities = reinterpret_cast<XInputReadCapabilities>(
        GetProcAddress(g_xinput_module, "XInputGetCapabilities"));
    g_xinput_get_extended_capabilities = reinterpret_cast<XInputReadExtendedCapabilities>(
        GetProcAddress(g_xinput_module, MAKEINTRESOURCEA(108)));
}

static GUID xinput_guid(DWORD slot)
{
    // The last byte identifies the Windows XInput user slot (0 through 3).
    return {0x6456D620, 0x99D9, 0x47B0, {0xAD, 0x76, 0x21, 0xFF, 0xED, 0xF4, 0xAE, static_cast<BYTE>(slot)}};
}

static DIJOYSTATE2 xinput_joystick_state(const XINPUT_GAMEPAD& pad)
{
    DIJOYSTATE2 state{};
    state.lX = pad.sThumbLX;
    state.lY = std::min<LONG>(32767, -static_cast<LONG>(pad.sThumbLY));
    state.lRx = pad.sThumbRX;
    state.lRy = std::min<LONG>(32767, -static_cast<LONG>(pad.sThumbRY));
    state.lZ = static_cast<LONG>(pad.bLeftTrigger) * 257 - 32768;
    state.lRz = static_cast<LONG>(pad.bRightTrigger) * 257 - 32768;

    constexpr WORD button_masks[] = {
        XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START,
        XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB
    };
    for (size_t index = 0; index < sizeof(button_masks) / sizeof(button_masks[0]); ++index)
        state.rgbButtons[index] = (pad.wButtons & button_masks[index]) ? 0x80 : 0;

    const int horizontal = !!(pad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) - !!(pad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT);
    const int vertical = !!(pad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) - !!(pad.wButtons & XINPUT_GAMEPAD_DPAD_UP);
    constexpr DWORD hats[3][3] = {{31500, 0, 4500}, {27000, DWORD(-1), 9000}, {22500, 18000, 13500}};
    std::fill(std::begin(state.rgdwPOV), std::end(state.rgdwPOV), DWORD(-1));
    state.rgdwPOV[0] = hats[vertical + 1][horizontal + 1];
    return state;
}

static void release_device_locked(GUID guid, bool notify);

struct GamepadName
{
    DWORD hardware_id;
    GUID container_id;
    std::wstring product_name;
    std::wstring xbox_name;
};

static DWORD gamepad_hardware_id(const wchar_t* instance_id)
{
    std::wstring id(instance_id);
    std::transform(id.begin(), id.end(), id.begin(), [](wchar_t character) { return std::towupper(character); });
    // USB and HID use VID_/PID_. Bluetooth can use VID&/PID& instead.
    auto read_id = [&](const wchar_t* usb_prefix, const wchar_t* bluetooth_prefix) {
        auto offset = id.find(usb_prefix);
        if (offset == std::wstring::npos) offset = id.find(bluetooth_prefix);
        if (offset == std::wstring::npos) return WORD(0);
        return LOWORD(std::wcstoul(id.c_str() + offset + 4, nullptr, 16));
    };
    const auto vendor = read_id(L"VID_", L"VID&");
    const auto product = read_id(L"PID_", L"PID&");
    return vendor && product ? MAKELONG(vendor, product) : 0;
}

static std::wstring gamepad_property(HDEVINFO devices, SP_DEVINFO_DATA& device, const DEVPROPKEY& key)
{
    wchar_t value[MAX_PATH]{};
    DEVPROPTYPE type = 0;
    if (SetupDiGetDevicePropertyW(devices, &device, &key, &type, reinterpret_cast<PBYTE>(value),
                                  sizeof(value), nullptr, 0) && type == DEVPROP_TYPE_STRING)
        return value;
    return {};
}

static std::vector<GamepadName> read_gamepad_names()
{
    struct DeviceList
    {
        HDEVINFO handle;
        ~DeviceList() { if (handle != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(handle); }
    } devices{SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT)};
    if (devices.handle == INVALID_HANDLE_VALUE) return {};

    std::vector<GamepadName> names;
    for (DWORD index = 0; ; ++index)
    {
        SP_DEVINFO_DATA device{sizeof(SP_DEVINFO_DATA)};
        if (!SetupDiEnumDeviceInfo(devices.handle, index, &device)) break;
        wchar_t instance_id[MAX_DEVICE_ID_LEN]{};
        if (!SetupDiGetDeviceInstanceIdW(devices.handle, &device, instance_id, MAX_DEVICE_ID_LEN, nullptr))
            continue;
        const DWORD hardware_id = gamepad_hardware_id(instance_id);
        if (!hardware_id) continue;

        GUID container{};
        DEVPROPTYPE type = 0;
        if (!SetupDiGetDevicePropertyW(devices.handle, &device, &DEVPKEY_Device_ContainerId, &type,
                                       reinterpret_cast<PBYTE>(&container), sizeof(container), nullptr, 0) ||
            type != DEVPROP_TYPE_GUID) container = GUID{};

        auto product = gamepad_property(devices.handle, device, DEVPKEY_Device_BusReportedDeviceDesc);
        auto classification = gamepad_property(devices.handle, device, DEVPKEY_Device_Class);
        std::wstring xbox_name;
        if (classification == L"XboxComposite" || classification == L"XnaComposite")
        {
            xbox_name = gamepad_property(devices.handle, device, DEVPKEY_Device_FriendlyName);
            if (xbox_name.empty()) xbox_name = gamepad_property(devices.handle, device, DEVPKEY_Device_DeviceDesc);
        }
        names.push_back({hardware_id, container, std::move(product), std::move(xbox_name)});
    }
    return names;
}

static std::vector<std::wstring> find_gamepad_names(DWORD hardware_id, const std::vector<GamepadName>& names)
{
    std::vector<std::wstring> matches;
    int priority = 0;
    auto consider = [&](const std::wstring& candidate, int rank) {
        if (rank < priority || candidate.empty() || _wcsicmp(candidate.c_str(), L"Controller") == 0 ||
            _wcsicmp(candidate.c_str(), L"Gamepad") == 0) return;
        if (rank > priority)
        {
            matches.clear();
            priority = rank;
        }
        auto same_name = [&](const std::wstring& name) { return _wcsicmp(name.c_str(), candidate.c_str()) == 0; };
        if (std::none_of(matches.begin(), matches.end(), same_name)) matches.push_back(candidate);
    };
    for (const auto& match : names)
    {
        if (match.hardware_id != hardware_id) continue;
        consider(match.product_name, 3);
        consider(match.xbox_name, 2);
        if (match.container_id == GUID{}) continue;
        // An Xbox controller can expose a compatibility interface with a different PID.
        for (const auto& sibling : names)
            if (sibling.container_id == match.container_id) consider(sibling.xbox_name, 1);
    }
    std::sort(matches.begin(), matches.end());
    return matches;
}

static std::string xinput_display_name(DWORD slot, DWORD hardware_id, const std::vector<GamepadName>& names)
{
    const auto number = std::to_string(slot + 1);
    const auto fallback = "XInput Gamepad " + number;
    const auto candidates = find_gamepad_names(hardware_id, names);
    if (candidates.empty()) return fallback;

    const auto slot_label = " (XInput " + number + ")";
    const auto suffix = candidates.size() > 1 ? "+" + slot_label : slot_label;
    char utf8[MAX_PATH]{};
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, candidates.front().c_str(), -1, utf8,
                             MAX_PATH - static_cast<int>(suffix.size()), nullptr, nullptr))
        return fallback;

    // The first line is the display label. GEX uses subsequent lines in the device tooltip.
    std::string label = utf8 + suffix;
    for (auto name = std::next(candidates.begin()); name != candidates.end(); ++name)
    {
        const auto remaining = MAX_PATH - static_cast<int>(label.size()) - 1;
        if (remaining > 1 && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name->c_str(), -1,
                                                utf8, remaining, nullptr, nullptr))
        {
            label += '\n';
            label += utf8;
        }
    }
    return label;
}

static void connect_xinput_device(DWORD slot, const GUID& guid, const std::string& name)
{
    DeviceSummary summary{};
    summary.device_guid = guid;
    summary.joystick_id = slot;
    strcpy_s(summary.name, MAX_PATH, name.c_str());
    summary.axis_count = 6;
    for (DWORD index = 0; index < summary.axis_count; ++index)
        summary.axis_map[index] = {index + 1, index + 1};
    summary.button_count = 10;
    summary.hat_count = 1;
    summary.usage_page = 1;
    summary.usage = 5;
    g_data_store.cache[guid] = summary;
    g_data_store.state[guid] = DeviceState{};
    g_data_store.active_guids.push_back(guid);
    ++g_inventory_generation;
    g_pending_devices.emplace_back(summary, DeviceActionType::Connected);
    logger->info("XInput slot {} connected: {}", slot, guid_to_string(guid));
}

static void poll_xinput_devices(bool publish_input)
{
    if (!g_xinput_get_state || !g_xinput_get_capabilities) return;
    const auto now = GetTickCount64();
    const bool scan = now >= g_next_xinput_scan;
    if (scan) g_next_xinput_scan = now + 500;
    std::vector<GamepadName> names;
    bool names_read = false;
    for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot)
    {
        const GUID guid = xinput_guid(slot);
        const bool connected = g_data_store.state.find(guid) != g_data_store.state.end();
        if (!connected && !scan) continue;
        XINPUT_STATE state{};
        const DWORD result = g_xinput_get_state(slot, &state);
        if (result == ERROR_DEVICE_NOT_CONNECTED && connected)
        {
            release_device_locked(guid, true);
            g_next_xinput_scan = 0;
            logger->info("XInput slot {} disconnected: {}", slot, guid_to_string(guid));
        }
        if (result != ERROR_SUCCESS) continue;
        if (!connected)
        {
            XINPUT_CAPABILITIES capabilities{};
            if (g_xinput_get_capabilities(slot, XINPUT_FLAG_GAMEPAD, &capabilities) != ERROR_SUCCESS ||
                capabilities.SubType != XINPUT_DEVSUBTYPE_GAMEPAD) continue;
            DWORD hardware_id = 0;
            XInputExtendedCapabilities extended{};
            if (g_xinput_get_extended_capabilities &&
                g_xinput_get_extended_capabilities(1, slot, XINPUT_FLAG_GAMEPAD, &extended) == ERROR_SUCCESS &&
                extended.vendor_id && extended.product_id)
            {
                hardware_id = MAKELONG(extended.vendor_id, extended.product_id);
                if (!names_read)
                {
                    names = read_gamepad_names();
                    names_read = true;
                }
            }
            connect_xinput_device(slot, guid, xinput_display_name(slot, hardware_id, names));
        }
        if (publish_input) update_device_state(xinput_joystick_state(state.Gamepad), guid);
    }
}

static void poll_devices_once()
{
    {
        std::lock_guard<std::recursive_mutex> guard(g_data_mutex);
        if(g_initialization_done)
        {
            poll_xinput_devices(true);
            for(const auto& entry : g_data_store.device_map)
            {
                if(!entry.second || !g_data_store.is_ready[entry.first]) continue;
                if(g_data_store.is_buffered[entry.first])
                    process_buffered_events(entry.second, entry.first);
                else
                    poll_device(entry.second, entry.first);
            }
        }
    }
    dispatch_device_events();
    dispatch_input_events();
}

DWORD WINAPI joystick_update_thread(LPVOID)
{
    g_native_worker = true;
    HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT startup = com_result;
#ifdef DILL_TESTING
    if (SUCCEEDED(startup)) startup = g_test_poll_startup;
#endif
    g_startup_error = FAILED(startup) ? startup : S_OK;
    SetEvent(g_ready_event);
    if (SUCCEEDED(startup))
    {
        try
        {
            while (WaitForSingleObject(g_stop_event, 4) == WAIT_TIMEOUT)
                poll_devices_once();
        }
        catch (const std::exception& error) { logger->error("Polling thread failed: {}", error.what()); request_stop(); }
        catch (...) { logger->error("Polling thread failed with an unknown error"); request_stop(); }
    }
    else request_stop();
    if (SUCCEEDED(com_result)) CoUninitialize();
    return 0;
}

DWORD WINAPI message_handler_thread(LPVOID)
{
    g_native_worker = true;
    HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT startup = FAILED(com_result) ? com_result : S_OK;
    HWND window = nullptr;
    try
    {
        if (SUCCEEDED(startup))
        {
            window = create_window();
            g_message_window = window;
            if (!window)
            {
                startup = g_startup_error.load();
                if (SUCCEEDED(startup)) startup = HRESULT_FROM_WIN32(GetLastError());
                if (SUCCEEDED(startup)) startup = E_FAIL;
            }
            else
            {
                enumerate_devices();
                startup = g_last_error;
            }
        }
    }
    catch (const std::exception& error) { logger->error("Native startup failed: {}", error.what()); startup = E_FAIL; }
    catch (...) { startup = E_FAIL; }
    g_startup_error = startup;
    SetEvent(g_ready_event);
    if (SUCCEEDED(startup))
    {
        MSG message{};
        while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0)
        {
            int result = GetMessage(&message, nullptr, 0, 0);
            if (result <= 0)
            {
                if (result == -1) logger->error("GetMessage failed: {}", GetLastError());
                request_stop();
                break;
            }
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
    }
    if (window) KillTimer(window, kHotplugTimer);
    if (g_device_notification) { UnregisterDeviceNotification(g_device_notification); g_device_notification = nullptr; }
    if (window) DestroyWindow(window);
    g_message_window = nullptr;
    UnregisterClass(CLS_NAME, GetModuleHandle(nullptr));
    release_all_devices();
    if (SUCCEEDED(com_result)) CoUninitialize();
    return 0;
}

HWND create_window()
{
    WNDCLASSEX window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandle(nullptr);
    window_class.lpszClassName = CLS_NAME;
    if (!RegisterClassEx(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return nullptr;
    GUID guid = HID_CLASSGUID;
    return CreateWindow(CLS_NAME, TEXT("DevNotifWnd"), 0, 0, 0, 0, 0,
                        HWND_MESSAGE, nullptr, window_class.hInstance, &guid);
}

BOOL CALLBACK set_axis_range(LPCDIDEVICEOBJECTINSTANCE lpddoi, LPVOID pvRef)
{
    LPDIRECTINPUTDEVICE8 device = reinterpret_cast<LPDIRECTINPUTDEVICE8>(pvRef);
    if(lpddoi->dwType & DIDFT_AXIS)
    {
        DIPROPRANGE range;

        ZeroMemory(&range, sizeof(range));
        range.diph.dwSize = sizeof(range);
        range.diph.dwHeaderSize = sizeof(range.diph);
        range.diph.dwObj = lpddoi->dwType;
        range.diph.dwHow = DIPH_BYID;
        range.lMin = -32768;
        range.lMax = 32767;

        auto result = device->SetProperty(DIPROP_RANGE, &range.diph);
        if(FAILED(result))
        {
            logger->error(
                "Error while setting axis range, {}",
                error_to_string(result)
            );
            return DIENUM_CONTINUE;
        }
    }
    return DIENUM_CONTINUE;
}

template<typename Query>
static HRESULT formatted_axis_indices(Query query, std::vector<int>& axes)
{
    // EnumObjects().dwOfs is the driver's native offset, NOT a DIJOYSTATE2
    // offset. Query each slot in our configured format to identify its object.
    const size_t offsets[] = { offsetof(DIJOYSTATE2, lX), offsetof(DIJOYSTATE2, lY),
        offsetof(DIJOYSTATE2, lZ), offsetof(DIJOYSTATE2, lRx), offsetof(DIJOYSTATE2, lRy),
        offsetof(DIJOYSTATE2, lRz), offsetof(DIJOYSTATE2, rglSlider[0]), offsetof(DIJOYSTATE2, rglSlider[1]) };
    std::vector<int> found;
    for (int index = 0; index < 8; ++index)
    {
        DIDEVICEOBJECTINSTANCE object{};
        object.dwSize = sizeof(object);
        HRESULT result = query(&object, static_cast<DWORD>(offsets[index]), DIPH_BYOFFSET);
        if (result == DIERR_OBJECTNOTFOUND) continue;
        if (FAILED(result)) return result;
        if (object.dwType & DIDFT_AXIS) found.push_back(index + 1);
    }
    axes.swap(found);
    return S_OK;
}

static HRESULT enumerate_formatted_axes(LPDIRECTINPUTDEVICE8 device, std::vector<int>& axes)
{
    return formatted_axis_indices([device](LPDIDEVICEOBJECTINSTANCE object, DWORD offset, DWORD how) {
        return device->GetObjectInfo(object, offset, how);
    }, axes);
}

static void release_device_locked(GUID guid, bool notify)
{
    auto device = g_data_store.device_map.find(guid);
    if (device != g_data_store.device_map.end())
    {
        if (device->second) { device->second->Unacquire(); device->second->Release(); }
        g_data_store.device_map.erase(device);
    }
    auto& active = g_data_store.active_guids;
    auto found = std::find(active.begin(), active.end(), guid);
    if (found != active.end())
    {
        active.erase(found);
        ++g_inventory_generation;
        auto summary = g_data_store.cache.find(guid);
        if (notify && summary != g_data_store.cache.end())
            g_pending_devices.emplace_back(summary->second, DeviceActionType::Disconnected);
    }
    g_data_store.is_ready.erase(guid);
    g_data_store.is_buffered.erase(guid);
    g_data_store.state.erase(guid);
    g_poll_failures.erase(guid);
    g_device_capabilities.erase(guid);
}

static bool is_xinput_duplicate(LPDIRECTINPUTDEVICE8 device, const GUID& guid, const std::string& name,
                                  WORD usage_page, WORD usage, DWORD device_type)
{
    if (!g_xinput_get_state || !g_xinput_get_capabilities) return false;
    // Keep non-gamepad HID devices, including wheels and flight sticks, on DirectInput.
    if (usage_page != 0 && (usage_page != 1 || usage != 5)) return false;
    if (usage_page == 0 && GET_DIDEVICE_TYPE(device_type) != DI8DEVTYPE_GAMEPAD) return false;

    DIPROPGUIDANDPATH path{};
    path.diph = {sizeof(path), sizeof(path.diph), 0, DIPH_DEVICE};
    if (SUCCEEDED(device->GetProperty(DIPROP_GUIDANDPATH, &path.diph)))
    {
        std::wstring device_path(path.wszPath);
        std::transform(device_path.begin(), device_path.end(), device_path.begin(),
                       [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
        // XUSB compatibility devices use Windows' XInput interface-group marker.
        if (device_path.find(L"&ig_") != std::wstring::npos) return true;
    }

    // Bluetooth XInput-compatible and native Xbox HID interfaces can omit the marker.
    auto lower_name = name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return lower_name.find("xinput") != std::string::npos ||
        (lower_name.find("xbox") != std::string::npos && get_vendor_id(device, guid) == 0x045E);
}

void initialize_device(GUID guid, std::string name, WORD usage_page, WORD usage)
{
    release_device_locked(guid, true);
    LPDIRECTINPUTDEVICE8 device = nullptr;
    auto fail = [&](HRESULT code, const char* operation) {
        g_device_errors[guid] = code;
        logger->error("{}: device initialization failed at {}: {}", guid_to_string(guid), operation, error_to_string(code));
        if (device) { device->Unacquire(); device->Release(); device = nullptr; }
    };
    HRESULT result = g_direct_input->CreateDevice(guid, &device, nullptr);
    if (FAILED(result) || !device) { fail(FAILED(result) ? result : E_POINTER, "CreateDevice"); return; }
    DIDEVICEINSTANCE deviceInfo{};
    deviceInfo.dwSize = sizeof(deviceInfo);
    result = device->GetDeviceInfo(&deviceInfo);
    if (SUCCEEDED(result)) { usage_page = deviceInfo.wUsagePage; usage = deviceInfo.wUsage; }
    else logger->warn("{}: GetDeviceInfo failed; using enumeration metadata: {}", guid_to_string(guid), error_to_string(result));
    if (is_xinput_duplicate(device, guid, name, usage_page, usage, deviceInfo.dwDevType))
    {
        device->Release();
        return;
    }
    result = device->SetCooperativeLevel(nullptr, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    if (FAILED(result)) { fail(result, "SetCooperativeLevel"); return; }
    result = device->SetDataFormat(&c_dfDIJoystick2);
    if (FAILED(result)) { fail(result, "SetDataFormat"); return; }
    DIDEVCAPS capabilities{};
    capabilities.dwSize = sizeof(capabilities);
    result = device->GetCapabilities(&capabilities);
    if (FAILED(result)) { fail(result, "GetCapabilities"); return; }
    std::vector<int> axes;
    result = enumerate_formatted_axes(device, axes);
    if (FAILED(result)) { fail(result, "GetObjectInfo formatted axes"); return; }
    std::sort(axes.begin(), axes.end());
    device->EnumObjects(set_axis_range, device, DIDFT_AXIS);
    DIPROPDWORD buffer{};
    buffer.diph = {sizeof(buffer), sizeof(buffer.diph), 0, DIPH_DEVICE};
    buffer.dwData = g_buffer_size;
    device->SetProperty(DIPROP_BUFFERSIZE, &buffer.diph);
    HRESULT acquire_result = device->Acquire(); // temporary focus/input loss can recover in Poll
    DeviceSummary summary{};
    summary.device_guid = guid;
    summary.vendor_id = get_vendor_id(device, guid);
    summary.product_id = get_product_id(device, guid);
    summary.joystick_id = get_joystick_id(device, guid);
    strcpy_s(summary.name, MAX_PATH, name.c_str());
    summary.axis_count = static_cast<DWORD>(axes.size());
    summary.button_count = capabilities.dwButtons;
    summary.hat_count = capabilities.dwPOVs;
    summary.usage_page = usage_page;
    summary.usage = usage;
    for (size_t index = 0; index < axes.size(); ++index)
        summary.axis_map[index] = {static_cast<DWORD>(index + 1), static_cast<DWORD>(axes[index])};
    g_data_store.device_map[guid] = device;
    g_data_store.cache[guid] = summary;
    g_device_capabilities[guid] = capabilities;
    g_data_store.state[guid] = DeviceState{};
    g_data_store.is_buffered[guid] = false;
    g_data_store.is_ready[guid] = true;
    g_data_store.active_guids.push_back(guid);
    g_device_errors[guid] = FAILED(acquire_result) ? acquire_result : S_OK;
    ++g_inventory_generation;
    g_pending_devices.emplace_back(summary, DeviceActionType::Connected);
    logger->info("Device ready: {} {} axes={} buttons={} hats={}", name, guid_to_string(guid), axes.size(), summary.button_count, summary.hat_count);
    if (FAILED(acquire_result)) report_poll_failure(guid, acquire_result, "initial Acquire");
}

static bool unchanged_device(LPCDIDEVICEINSTANCE instance)
{
    GUID guid = instance->guidInstance;
    auto existing = g_data_store.device_map.find(guid);
    auto cached = g_data_store.cache.find(guid);
    auto previous = g_device_capabilities.find(guid);
    if (previous == g_device_capabilities.end()) return false;
    if (existing == g_data_store.device_map.end() || !existing->second || cached == g_data_store.cache.end() || !g_data_store.is_ready[guid]) return false;
    DIDEVCAPS capabilities{};
    capabilities.dwSize = sizeof(capabilities);
    if (FAILED(existing->second->GetCapabilities(&capabilities))) return false;
    return std::string(cached->second.name) == instance->tszInstanceName &&
        cached->second.usage_page == instance->wUsagePage && cached->second.usage == instance->wUsage &&
        previous->second.dwAxes == capabilities.dwAxes && previous->second.dwButtons == capabilities.dwButtons &&
        previous->second.dwPOVs == capabilities.dwPOVs;
}

BOOL CALLBACK handle_device_cb(LPCDIDEVICEINSTANCE instance, LPVOID data)
{
    try
    {
        auto& current = *static_cast<std::unordered_map<GUID, bool>*>(data);
        current[instance->guidInstance] = true;
        if (!unchanged_device(instance))
            initialize_device(instance->guidInstance, instance->tszInstanceName, instance->wUsagePage, instance->wUsage);
        return DIENUM_CONTINUE;
    }
    catch (const std::bad_alloc&) { g_enumeration_callback_error = E_OUTOFMEMORY; }
    catch (...) { g_enumeration_callback_error = E_FAIL; }
    // Never unwind a C++ exception through DirectInput's callback frame.
    return DIENUM_STOP;
}

static HRESULT enumerate_devices_locked()
{
#ifdef DILL_TESTING
    if (g_test_enumerate) return g_test_enumerate();
#endif
    const bool first_enumeration = !g_initialization_done;
    g_initialization_done = false;
    load_xinput();
    if (!g_direct_input)
    {
        HRESULT result = DirectInput8Create(GetModuleHandle(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8,
                                            reinterpret_cast<void**>(&g_direct_input), nullptr);
        if (FAILED(result)) return result;
    }
    std::unordered_map<GUID, bool> current;
    g_enumeration_callback_error = S_OK;
    HRESULT result = g_direct_input->EnumDevices(DI8DEVCLASS_GAMECTRL, handle_device_cb, &current, DIEDFL_ATTACHEDONLY);
    if (SUCCEEDED(result) && FAILED(g_enumeration_callback_error)) result = g_enumeration_callback_error;
    if (FAILED(result)) { g_initialization_done = true; return result; } // never prune from a partial enumeration
    std::vector<GUID> removed;
    for (const auto& entry : g_data_store.device_map)
        if (current.find(entry.first) == current.end()) removed.push_back(entry.first);
    for (const auto& guid : removed) release_device_locked(guid, true);
    g_next_xinput_scan = 0;
    if (first_enumeration) poll_xinput_devices(false);
    g_initialization_done = true;
    return S_OK;
}

void enumerate_devices()
{
    HRESULT result;
    {
        std::lock_guard<std::recursive_mutex> guard(g_data_mutex);
        result = enumerate_devices_locked();
    }
    if (FAILED(result)) { api_error(result, "DirectInput enumeration"); logger->error("{}", g_last_error_text); }
    else api_success();
    dispatch_device_events();
}

static void request_stop()
{
    g_running = false;
    if (g_stop_event) SetEvent(g_stop_event);
    if (g_message_thread_id) PostThreadMessage(g_message_thread_id, WM_QUIT, 0, 0);
}

static void release_all_devices()
{
    std::lock_guard<std::recursive_mutex> guard(g_data_mutex);
    g_initialization_done = false;
    for (const auto& item : g_data_store.device_map)
        if (item.second) { item.second->Unacquire(); item.second->Release(); }
    if (!g_data_store.active_guids.empty()) ++g_inventory_generation;
    g_data_store = DeviceDataStore{};
    g_device_errors.clear();
    g_device_capabilities.clear();
    g_poll_failures.clear();
    g_next_xinput_scan = 0;
    g_xinput_get_state = nullptr;
    g_xinput_get_capabilities = nullptr;
    g_xinput_get_extended_capabilities = nullptr;
    g_xinput_checked = false;
    if (g_xinput_module) { FreeLibrary(g_xinput_module); g_xinput_module = nullptr; }
    if (g_direct_input) { g_direct_input->Release(); g_direct_input = nullptr; }
}

static void stop_and_join_threads()
{
    g_event_callback = nullptr;
    g_device_change_callback = nullptr;
    request_stop();
    if (g_joystick_thread) { WaitForSingleObject(g_joystick_thread, INFINITE); CloseHandle(g_joystick_thread); }
    if (g_message_thread) { WaitForSingleObject(g_message_thread, INFINITE); CloseHandle(g_message_thread); }
    g_joystick_thread = nullptr;
    g_message_thread = nullptr;
    g_message_thread_id = 0;
    release_all_devices();
    if (g_ready_event) CloseHandle(g_ready_event);
    if (g_stop_event) CloseHandle(g_stop_event);
    g_ready_event = nullptr;
    g_stop_event = nullptr;
    g_running = false;
}

BOOL init()
{
    if (g_native_worker) return api_error(HRESULT_FROM_WIN32(ERROR_BUSY), "init from a native callback");
    std::lock_guard<std::mutex> lifecycle(g_lifecycle_mutex);
    if (g_running && WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT)
    { api_success(); return TRUE; }
    if (g_message_thread || g_joystick_thread) stop_and_join_threads();
    logger->info("Initializing DILL v1.6.2.0 (managed lifecycle, API 2)");
    g_stop_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    g_ready_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    g_startup_error = S_OK;
    HRESULT result = S_OK;
    if (!g_stop_event || !g_ready_event) result = HRESULT_FROM_WIN32(GetLastError());
    else
    {
        DWORD thread_id = 0;
        g_message_thread = CreateThread(nullptr, 0, message_handler_thread, nullptr, 0, &thread_id);
        g_message_thread_id = thread_id;
        if (!g_message_thread) result = HRESULT_FROM_WIN32(GetLastError());
        else
        {
            DWORD wait = WaitForSingleObject(g_ready_event, 30000);
            result = wait == WAIT_OBJECT_0 ? g_startup_error.load() : HRESULT_FROM_WIN32(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
            if (SUCCEEDED(result) && WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0) result = E_ABORT;
        }
        if (SUCCEEDED(result))
        {
            ResetEvent(g_ready_event);
            g_startup_error = S_OK;
            g_running = true;
            g_joystick_thread = CreateThread(nullptr, 0, joystick_update_thread, nullptr, 0, nullptr);
            if (!g_joystick_thread) result = HRESULT_FROM_WIN32(GetLastError());
            else
            {
                DWORD wait = WaitForSingleObject(g_ready_event, 30000);
                result = wait == WAIT_OBJECT_0 ? g_startup_error.load() : HRESULT_FROM_WIN32(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
                if (SUCCEEDED(result) && WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0) result = E_ABORT;
            }
        }
    }
    if (FAILED(result))
    {
        stop_and_join_threads();
        return api_error(result, "DILL initialization");
    }
    api_success();
    return TRUE;
}

BOOL shutdown()
{
    // A callback may request a stop, but cannot join its own native thread.
    if (g_native_worker)
    {
        request_stop();
        return api_error(HRESULT_FROM_WIN32(ERROR_BUSY), "shutdown requested from a native callback; call again from the owner thread");
    }
    std::lock_guard<std::mutex> lifecycle(g_lifecycle_mutex);
    stop_and_join_threads();
    api_success();
    return TRUE;
}

DWORD get_api_version() { return kApiVersion; }
BOOL is_initialized() { return g_running.load(); }
BOOL is_worker_thread() { return g_native_worker; }
LONG get_last_error_code() { return g_last_error; }
size_t get_last_error_message(char* buffer, size_t capacity)
{
    const std::string& message = g_last_error_text;
    if (buffer && capacity) strcpy_s(buffer, capacity, message.substr(0, capacity - 1).c_str());
    return message.size() + 1;
}

BOOL get_abi_info(DillAbiInfo* info, size_t size)
{
    if (!info || size != sizeof(DillAbiInfo)) return api_error(E_INVALIDARG, "get_abi_info structure size");
    *info = { kApiVersion, sizeof(DillAbiInfo), sizeof(GUID), sizeof(DeviceSummary), sizeof(JoystickInputData),
              sizeof(AxisMap), offsetof(DeviceSummary, usage_page), offsetof(DeviceSummary, usage),
              offsetof(JoystickInputData, input_type), offsetof(JoystickInputData, input_index),
              offsetof(JoystickInputData, value) };
    api_success();
    return TRUE;
}

BOOL get_device_snapshot(DeviceSummary* buffer, size_t capacity, size_t* count, std::uint64_t* generation)
{
    if (!count || !generation || (!buffer && capacity)) return api_error(E_INVALIDARG, "get_device_snapshot arguments");
    std::lock_guard<std::recursive_mutex> guard(g_data_mutex);
    *count = g_data_store.active_guids.size();
    *generation = g_inventory_generation;
    if (capacity < *count) return api_error(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER), "get_device_snapshot buffer");
    for (size_t index = 0; index < *count; ++index)
        buffer[index] = g_data_store.cache.at(g_data_store.active_guids[index]);
    api_success();
    return TRUE;
}

LONG get_device_error(GUID guid)
{
    std::lock_guard<std::recursive_mutex> guard(g_data_mutex);
    auto found = g_device_errors.find(guid);
    return found == g_device_errors.end() ? HRESULT_FROM_WIN32(ERROR_NOT_FOUND) : found->second;
}

void set_input_event_callback(JoystickInputEventCallback cb)
{
    logger->info("Setting event callback");
    g_event_callback = cb;
}

void set_device_change_callback(DeviceChangeCallback cb)
{
    logger->info("Setting device change callback");
    g_device_change_callback = cb;
}

DeviceSummary get_device_information_by_index(size_t index)
{
    std::lock_guard<std::recursive_mutex> guard(g_data_mutex);
    if(index < 0 || index >= g_data_store.active_guids.size())
    {
        return DeviceSummary();
    }
    auto guid = g_data_store.active_guids[index];
    return get_device_information_by_guid(guid);
}

DeviceSummary get_device_information_by_guid(GUID guid)
{
    std::lock_guard<std::recursive_mutex> guard(g_data_mutex);
    if(g_data_store.cache.find(guid) == g_data_store.cache.end())
    {
        return DeviceSummary();
    }

    return g_data_store.cache[guid];
}


size_t get_device_count()
{
    std::lock_guard<std::recursive_mutex> guard(g_data_mutex);
    return g_data_store.active_guids.size();
}

bool device_exists(GUID guid)
{
    std::lock_guard<std::recursive_mutex> guard(g_data_mutex);
    bool exists = false;
    for(auto const& dev_guid : g_data_store.active_guids)
    {
        if(dev_guid == guid)
        {
            exists = true;
        }
    }

    return exists;
}

LONG get_axis(GUID guid, DWORD index)
{
    std::lock_guard<std::recursive_mutex> guard(g_data_mutex);
    if(index < 1 || index > 8)
    {
        logger->error(
            "{}: Requested invalid axis index {}",
            guid_to_string(guid),
            index
        );
        return 0;
    }

    auto state = g_data_store.state.find(guid);
    return state == g_data_store.state.end() ? 0 : state->second.axis[index];
}

bool get_button(GUID guid, DWORD index)
{
    std::lock_guard<std::recursive_mutex> guard(g_data_mutex);
    if(index < 1 || index > 128)
    {
        logger->error(
            "{}: Requested invalid button index {}",
            guid_to_string(guid),
            index
        );
        return false;
    }

    auto state = g_data_store.state.find(guid);
    return state == g_data_store.state.end() ? false : state->second.button[index];
}

LONG get_hat(GUID guid, DWORD index)
{
    std::lock_guard<std::recursive_mutex> guard(g_data_mutex);
    if(index < 1 || index > 4)
    {
        logger->error(
            "{}: Requested invalid hat index {}",
            guid_to_string(guid),
            index
        );
        return -1;
    }

    auto state = g_data_store.state.find(guid);
    return state == g_data_store.state.end() ? -1 : state->second.hat[index];
}

std::vector<int> used_axis_indices(GUID guid)
{
    std::lock_guard<std::recursive_mutex> guard(g_data_mutex);
    std::vector<int> axes;
    auto device = g_data_store.device_map.find(guid);
    if (device == g_data_store.device_map.end() || !device->second) return axes;
    HRESULT result = enumerate_formatted_axes(device->second, axes);
    if (FAILED(result)) { g_device_errors[guid] = result; return {}; }
    std::sort(axes.begin(), axes.end());
    return axes;
}

DWORD get_vendor_id(LPDIRECTINPUTDEVICE8 device, GUID guid)
{
    DIPROPDWORD data;

    ZeroMemory(&data, sizeof(DIPROPDWORD));
    data.diph.dwSize = sizeof(DIPROPDWORD);
    data.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    data.diph.dwObj = 0;
    data.diph.dwHow = DIPH_DEVICE;

    auto result = device->GetProperty(
        DIPROP_VIDPID,
        &data.diph
    );

    if(FAILED(result))
    {
        logger->critical(
            "{} Failed retrieving joystick vendor id data, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
        return 0;
    }

    return LOWORD(data.dwData);
}

DWORD get_product_id(LPDIRECTINPUTDEVICE8 device, GUID guid)
{
    DIPROPDWORD data;

    ZeroMemory(&data, sizeof(DIPROPDWORD));
    data.diph.dwSize = sizeof(DIPROPDWORD);
    data.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    data.diph.dwObj = 0;
    data.diph.dwHow = DIPH_DEVICE;

    auto result = device->GetProperty(
        DIPROP_VIDPID,
        &data.diph
    );

    if(FAILED(result))
    {
        logger->critical(
            "{} Failed retrieving joystick product id data, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
        return 0;
    }

    return HIWORD(data.dwData);
}

DWORD get_joystick_id(LPDIRECTINPUTDEVICE8 device, GUID guid)
{
    DIPROPDWORD data;

    ZeroMemory(&data, sizeof(DIPROPDWORD));
    data.diph.dwSize = sizeof(DIPROPDWORD);
    data.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    data.diph.dwObj = 0;
    data.diph.dwHow = DIPH_DEVICE;

    auto result = device->GetProperty(
        DIPROP_JOYSTICKID,
        &data.diph
    );

    if(FAILED(result))
    {
        logger->critical(
            "{} Failed retrieving joystick id data, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
        return 0;
    }

    return data.dwData;
}