func CALLBACK process_window_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT
{
    extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    const LRESULT imgui_result = ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);
    if (imgui_result != 0)
        return imgui_result;

    switch (message) {
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                PostQuitMessage(0);
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
            break;
        case WM_GETMINMAXINFO:
            {
                auto info = reinterpret_cast<MINMAXINFO*>(lparam);
                info->ptMinTrackSize.x = 400;
                info->ptMinTrackSize.y = 400;
                return 0;
            }
    }
    return DefWindowProc(window, message, wparam, lparam);
}

func create_window(const char* name, i32 width, i32 height) -> HWND
{
    const WNDCLASSEXA winclass = {
        .cbSize = sizeof(winclass),
        .lpfnWndProc = process_window_message,
        .hInstance = GetModuleHandle(nullptr),
        .hCursor = LoadCursor(nullptr, IDC_ARROW),
        .lpszClassName = name,
    };
    if (!RegisterClassEx(&winclass))
        VHR(HRESULT_FROM_WIN32(GetLastError()));

    LOG("[game] Window class registered");

    const DWORD style = WS_OVERLAPPEDWINDOW;

    RECT rect = { 0, 0, width, height };
    AdjustWindowRectEx(&rect, style, FALSE, 0);

    const HWND window = CreateWindowEx(0, name, name, style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, winclass.hInstance, nullptr);
    if (!window)
        VHR(HRESULT_FROM_WIN32(GetLastError()));

    LOG("[game] Window created");

    return window;
}

func get_time() -> f64
{
    static LARGE_INTEGER start_counter;
    static LARGE_INTEGER frequency;
    if (start_counter.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start_counter);
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (counter.QuadPart - start_counter.QuadPart) / static_cast<f64>(frequency.QuadPart);
}

func update_frame_stats(HWND window, const char* name, f64* out_time, f32* out_delta_time) -> void
{
    static f64 previous_time = -1.0;
    static f64 header_refresh_time = 0.0;
    static u32 num_frames = 0;

    if (previous_time < 0.0) {
        previous_time = get_time();
        header_refresh_time = previous_time;
    }

    *out_time = get_time();
    *out_delta_time = static_cast<f32>(*out_time - previous_time);
    previous_time = *out_time;

    if ((*out_time - header_refresh_time) >= 1.0) {
        const f64 fps = num_frames / (*out_time - header_refresh_time);
        const f64 ms = (1.0 / fps) * 1000.0;
        char header[128];
        snprintf(header, sizeof(header), "[%.1f fps  %.3f ms] %s", fps, ms, name);
        SetWindowText(window, header);
        header_refresh_time = *out_time;
        num_frames = 0;
    }
    num_frames++;
}

func load_file(const char* filename) -> std::vector<u8>
{
    FILE* file = fopen(filename, "rb");
    assert(file);
    fseek(file, 0, SEEK_END);
    const i32 size_in_bytes = ftell(file);
    assert(size_in_bytes > 0);
    fseek(file, 0, SEEK_SET);
    std::vector<u8> data(size_in_bytes);
    const usize num_read_bytes = fread(&data[0], 1, size_in_bytes, file);
    (void)num_read_bytes;
    fclose(file);
    assert(size_in_bytes == num_read_bytes);
    return data;
}
