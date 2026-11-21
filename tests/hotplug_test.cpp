// No driver installation or real device mutation: exercise the native store.
#include <cassert>
#include <cstddef>
#include <future>
#include <thread>
#define DILL_TESTING
#include "../src/dill.cpp"

static_assert(sizeof(DeviceSummary) == 368, "Python DeviceSummary ABI must match");
static_assert(offsetof(DeviceSummary, usage_page) == 364, "HID usage ABI offset");
static_assert(offsetof(DeviceSummary, usage) == 366, "HID usage ABI offset");

#define require(condition) do { if (!(condition)) { std::cerr << "Failed at line " << __LINE__ << ": " << #condition << std::endl; std::abort(); } } while (false)

static void check_callback_can_read_store()
{
    std::promise<void> done;
    auto completion = done.get_future();
    std::thread reader([&] { get_device_count(); done.set_value(); });
    require(completion.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    reader.join();
}

static int input_callbacks = 0;
static int device_callbacks = 0;
static void on_input(JoystickInputData event)
{
    check_callback_can_read_store();
    ++input_callbacks;
}
static void on_device(DeviceSummary event, DeviceActionType action)
{
    check_callback_can_read_store();
    ++device_callbacks;
}

static XINPUT_STATE fake_pads[XUSER_MAX_COUNT]{};
static DWORD fake_pad_status[XUSER_MAX_COUNT] = {
    ERROR_SUCCESS, ERROR_SUCCESS, ERROR_DEVICE_NOT_CONNECTED, ERROR_SUCCESS
};

static DWORD WINAPI read_fake_pad(DWORD slot, XINPUT_STATE* state)
{
    *state = fake_pads[slot];
    return fake_pad_status[slot];
}

static DWORD WINAPI read_fake_pad_capabilities(DWORD slot, DWORD flags, XINPUT_CAPABILITIES* caps)
{
    require(flags == XINPUT_FLAG_GAMEPAD);
    caps->SubType = slot == 3 ? XINPUT_DEVSUBTYPE_WHEEL : XINPUT_DEVSUBTYPE_GAMEPAD;
    return ERROR_SUCCESS;
}

static void check_xinput_names()
{
    const auto compatibility = gamepad_hardware_id(L"HID\\VID_045E&PID_02FF&IG_00\\instance");
    const auto usb = gamepad_hardware_id(L"USB\\VID_045E&PID_02D1\\instance");
    const auto bluetooth = gamepad_hardware_id(L"HID\\{GUID}_VID&0002045E_PID&0B13\\instance");
    require(compatibility == MAKELONG(0x045E, 0x02FF));
    require(usb == MAKELONG(0x045E, 0x02D1));
    require(bluetooth == MAKELONG(0x045E, 0x0B13));
    require(!gamepad_hardware_id(L"HID\\Unknown"));
    const GUID xbox_container{0x3};
    std::vector<GamepadName> names = {
        {usb, xbox_container, L"Controller", L"Xbox One Controller"},
        {compatibility, xbox_container, {}, {}},
        {bluetooth, {}, L"Xbox Wireless Controller", {}},
    };
    require(xinput_display_name(0, compatibility, names) == "Xbox One Controller (XInput 1)");
    require(xinput_display_name(1, bluetooth, names) == "Xbox Wireless Controller (XInput 2)");
    names.push_back({bluetooth, {}, L"Other Controller", {}});
    names.push_back({bluetooth, {}, L"other controller", {}});
    require(xinput_display_name(1, bluetooth, names) ==
            "Other Controller+ (XInput 2)\nXbox Wireless Controller");
    require(xinput_display_name(3, 0, names) == "XInput Gamepad 4");
}

static void check_xinput()
{
    g_xinput_get_state = read_fake_pad;
    g_xinput_get_capabilities = read_fake_pad_capabilities;
    const auto first = xinput_guid(0);
    const auto second = xinput_guid(1);
    require(first != second);
    fake_pads[0].Gamepad = {
        XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_RIGHT_THUMB | XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT,
        255, 128, 12000, -32768, -1234, 2000
    };
    fake_pads[1].Gamepad.wButtons = XINPUT_GAMEPAD_B;

    set_input_event_callback(on_input);
    poll_xinput_devices(false);
    require(get_device_count() == 2 && g_pending_devices.size() == 2 && g_pending_inputs.empty());
    require(device_exists(first) && device_exists(second) && !device_exists(xinput_guid(3)));
    auto summary = get_device_information_by_guid(first);
    require(summary.joystick_id == 0 && summary.axis_count == 6 && summary.button_count == 10);
    require(summary.usage_page == 1 && summary.usage == 5 && summary.hat_count == 1);
    poll_xinput_devices(true);
    require(get_axis(first, 1) == 12000 && get_axis(first, 2) == 32767);
    require(get_axis(first, 3) == 32767 && get_axis(first, 6) == 128);
    require(get_axis(first, 4) == -1234 && get_axis(first, 5) == -2000);
    require(get_button(first, 1) && get_button(first, 10) && get_hat(first, 1) == 4500);
    require(get_button(second, 2) && !get_button(second, 1));
    auto input_events = g_pending_inputs.size();
    require(input_events == 12);
    poll_xinput_devices(true);
    require(g_pending_inputs.size() == input_events && g_pending_devices.size() == 2);

    fake_pads[0] = {};
    poll_xinput_devices(true);
    require(get_axis(first, 3) == -32768 && !get_button(first, 1) && get_hat(first, 1) == -1);
    fake_pad_status[0] = ERROR_DEVICE_NOT_CONNECTED;
    poll_xinput_devices(true);
    require(get_device_count() == 1 && !device_exists(first));
    require(g_pending_devices.back().first.device_guid == first);
    require(g_pending_devices.back().second == DeviceActionType::Disconnected);
    fake_pad_status[0] = ERROR_SUCCESS;
    poll_xinput_devices(true);
    require(get_device_count() == 2 && device_exists(first));
    require(g_pending_devices.back().first.device_guid == first);
    require(g_pending_devices.back().second == DeviceActionType::Connected);
    g_pending_inputs.clear();
    g_pending_devices.clear();
    set_input_event_callback(nullptr);
    release_all_devices();
}

int main()
{
    logger->set_level(spd::level::off);
    check_xinput_names();
    check_xinput();
    GUID key{0x12345678, 2, 3, {4, 5, 6, 7, 8, 9, 10, 11}};
    g_initialization_done = true;
    auto replace_store = [&] {
        std::lock_guard<std::recursive_mutex> guard(g_data_mutex);
        g_data_store = DeviceDataStore{};
        DeviceSummary summary{};
        summary.device_guid = key;
        summary.usage_page = 1;
        summary.usage = 4;
        g_data_store.device_map[key] = nullptr; // disconnected/not ready
        ++g_inventory_generation;
        summary.joystick_id = static_cast<DWORD>(g_inventory_generation);
        g_data_store.cache[key] = summary;
        g_data_store.active_guids.push_back(key);
        g_data_store.state[key] = DeviceState{};
    };
    replace_store();
    std::atomic<bool> begin{false};
    std::thread writer([&] {
        while (!begin.load()) std::this_thread::yield();
        for (int i = 0; i < 50000; ++i) replace_store();
    });
    std::thread polling([&] {
        while (!begin.load()) std::this_thread::yield();
        for (int i = 0; i < 50000; ++i) poll_devices_once();
    });
    std::thread reading([&] {
        while (!begin.load()) std::this_thread::yield();
        for (int i = 0; i < 50000; ++i)
        {
            require(get_device_count() == 1);
            require(get_device_information_by_index(0).usage == 4);
            require(get_device_information_by_guid(key).usage_page == 1);
            require(device_exists(key));
            DeviceSummary snapshot{};
            size_t count = 0;
            std::uint64_t generation = 0;
            require(get_device_snapshot(&snapshot, 1, &count, &generation));
            require(count == 1 && snapshot.joystick_id == generation);
            get_axis(key, 8);
            get_button(key, 128);
            get_hat(key, 4);
        }
    });
    begin = true;
    writer.join();
    polling.join();
    reading.join();
    set_input_event_callback(on_input);
    set_device_change_callback(on_device);
    {
        std::lock_guard<std::recursive_mutex> guard(g_data_mutex);
        DIDEVICEOBJECTDATA data{};
        data.dwOfs = offsetof(DIJOYSTATE2, rgbButtons) + 127;
        data.dwData = 0x80;
        emit_joystick_input_event(data, key);
        data.dwOfs = offsetof(DIJOYSTATE2, rgdwPOV) + 3 * sizeof(DWORD);
        data.dwData = 9000;
        emit_joystick_input_event(data, key);
        g_pending_devices.emplace_back(g_data_store.cache[key], DeviceActionType::Disconnected);
    }
    dispatch_input_events();
    dispatch_device_events();
    require(input_callbacks == 2 && device_callbacks == 1);
    require(get_button(key, 128));
    require(get_hat(key, 4) == 9000);
    require(on_device_change(0, DBT_DEVNODES_CHANGED) == TRUE);
    GUID absent{};
    require(get_axis(absent, 1) == 0);
    require(!get_button(absent, 1));
    require(get_hat(absent, 1) == -1);
    require(g_data_store.state.size() == 1); // reads must not insert absent devices
    DillAbiInfo abi{};
    require(get_api_version() == 0x00020000);
    require(get_abi_info(&abi, sizeof(abi)));
    require(abi.device_summary_size == sizeof(DeviceSummary) && abi.input_data_size == sizeof(JoystickInputData));
    require(!get_abi_info(&abi, sizeof(abi) - 1));
    require(get_last_error_code() == E_INVALIDARG);
    char message[256]{};
    get_last_error_message(message, sizeof(message));
    require(std::string(message).find("get_abi_info") != std::string::npos);
    size_t count = 0;
    std::uint64_t generation = 0;
    require(!get_device_snapshot(nullptr, 0, &count, &generation) && count == 1);
    require(get_last_error_code() == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
    std::vector<int> axes;
    const std::vector<int> warthog_axes{1, 2, 3, 6, 7};
    const size_t offsets[] = { offsetof(DIJOYSTATE2, lX), offsetof(DIJOYSTATE2, lY),
        offsetof(DIJOYSTATE2, lZ), offsetof(DIJOYSTATE2, lRx), offsetof(DIJOYSTATE2, lRy),
        offsetof(DIJOYSTATE2, lRz), offsetof(DIJOYSTATE2, rglSlider[0]), offsetof(DIJOYSTATE2, rglSlider[1]) };
    int queried = 0;
    auto shifted_native_offsets = [&](LPDIDEVICEOBJECTINSTANCE object, DWORD offset, DWORD how) -> HRESULT {
        require(how == DIPH_BYOFFSET && object->dwSize == sizeof(*object));
        ++queried;
        int axis = 0;
        for (int index = 0; index < 8; ++index) if (offset == offsets[index]) axis = index + 1;
        if (std::find(warthog_axes.begin(), warthog_axes.end(), axis) == warthog_axes.end()) return DIERR_OBJECTNOTFOUND;
        object->dwType = DIDFT_ABSAXIS;
        object->dwOfs = offset + 4; // raw HID offset differs from the configured format
        return DI_OK;
    };
    require(SUCCEEDED(formatted_axis_indices(shifted_native_offsets, axes)));
    require(queried == 8 && axes == warthog_axes);
    require(std::find(axes.begin(), axes.end(), 1) != axes.end());
    // Enumeration does not depend on an axis's value; a centered object is present.
    require(SUCCEEDED(formatted_axis_indices([](LPDIDEVICEOBJECTINSTANCE object, DWORD, DWORD) -> HRESULT {
        object->dwType = DIDFT_ABSAXIS;
        object->dwOfs = 999; // unusable native offsets cannot change configured IDs
        return DI_OK;
    }, axes)));
    require(axes == std::vector<int>({1, 2, 3, 4, 5, 6, 7, 8}));
    const auto previous_axes = axes;
    require(FAILED(formatted_axis_indices([](LPDIDEVICEOBJECTINSTANCE, DWORD, DWORD) -> HRESULT { return E_FAIL; }, axes)));
    require(axes == previous_axes); // failed discovery cannot publish a partial map
    require(report_poll_failure(key, E_FAIL, "test"));
    for (int i = 0; i < 10; ++i) require(!report_poll_failure(key, E_FAIL, "test"));
    require(g_poll_failures[key].suppressed == 10 && get_device_error(key) == E_FAIL);
    report_poll_recovery(key);
    require(g_poll_failures.empty() && get_device_error(key) == S_OK);
    require(shutdown());
    require(get_device_snapshot(nullptr, 0, &count, &generation) && count == 0);
    // Real Win32 worker/window lifecycle with a fake enumeration backend: no
    // driver initialization, device acquisition, installation or mutation.
    g_test_enumerate = []() -> HRESULT { return E_FAIL; };
    require(!init());
    require(get_last_error_code() == E_FAIL);
    require(!g_message_thread && !g_joystick_thread && !g_stop_event && !g_ready_event);
    static std::atomic<int> enumeration_calls{0};
    g_test_enumerate = []() -> HRESULT { ++enumeration_calls; g_initialization_done = true; return S_OK; };
    g_test_poll_startup = E_FAIL;
    require(!init());
    require(get_last_error_code() == E_FAIL && !is_initialized());
    require(!g_message_thread && !g_joystick_thread && !g_ready_event && !g_stop_event);
    g_test_poll_startup = S_OK;
    for (int cycle = 0; cycle < 5; ++cycle)
    {
        int before = enumeration_calls.load();
        require(init());
        require(is_initialized());
        require(init()); // idempotent: same threads and exactly one enumeration
        require(enumeration_calls == before + 1);
        if (!cycle)
        {
            DEV_BROADCAST_DEVICEINTERFACE notification{};
            notification.dbcc_size = sizeof(notification);
            notification.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
            HWND window = g_message_window.load();
            for (int index = 0; index < 20; ++index)
                SendMessage(window, WM_APP + 1, DBT_DEVICEARRIVAL, reinterpret_cast<LPARAM>(&notification));
            Sleep(30);
            require(enumeration_calls == before + 1);
            ULONGLONG deadline = GetTickCount64() + 2000;
            while (enumeration_calls == before + 1 && GetTickCount64() < deadline) Sleep(10);
            require(g_test_notifications == 20 && g_test_timer_calls == 1);
            require(enumeration_calls == before + 2);
        }
        require(shutdown());
        require(!is_initialized());
        require(shutdown());
        require(!g_message_thread && !g_joystick_thread && !g_stop_event && !g_ready_event && !g_message_window);
    }
    std::cout << "Native XInput, concurrency, snapshots, axes, diagnostics, startup rollback, hotplug debounce and restart checks passed.\n";
    return 0;
}
