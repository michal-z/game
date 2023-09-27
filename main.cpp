#include "pch.h"

extern "C" {
    __declspec(dllexport) extern const UINT D3D12SDKVersion = 610;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\d3d12\\";
}

constexpr const char* window_name = "my_game";
constexpr bool enable_debug_layer = true;

struct GraphicsContext {
    IDXGIFactory7* dxgi_factory;
    IDXGIAdapter4* adapter;
    ID3D12Device12* device;
    ID3D12CommandQueue* command_queue;
};

static bool init_graphics_context(GraphicsContext* gr)
{
    assert(gr && gr->device == nullptr);

    RETURN_IF_FAIL(CreateDXGIFactory2(enable_debug_layer ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&gr->dxgi_factory)));

    RETURN_IF_FAIL(gr->dxgi_factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&gr->adapter)));

    DXGI_ADAPTER_DESC3 adapter_desc = {};
    RETURN_IF_FAIL(gr->adapter->GetDesc3(&adapter_desc));

    LOG("Graphics adapter: %S", adapter_desc.Description);

    if (enable_debug_layer) {
        ID3D12Debug6* debug = nullptr;
        D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
        if (debug) {
            debug->EnableDebugLayer();
            SAFE_RELEASE(debug);
        }
    }

    RETURN_IF_FAIL(D3D12CreateDevice(gr->adapter, D3D_FEATURE_LEVEL_11_1, IID_PPV_ARGS(&gr->device)));

    LOG("D3D12 device created");

    const D3D12_COMMAND_QUEUE_DESC command_queue_desc = {
        .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
        .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
        .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
        .NodeMask = 0,
    };
    RETURN_IF_FAIL(gr->device->CreateCommandQueue(&command_queue_desc, IID_PPV_ARGS(&gr->command_queue)));

    LOG("Command queue created");

    return true;
}

static void deinit_graphics_context(GraphicsContext* gr)
{
    assert(gr);
    SAFE_RELEASE(gr->command_queue);
    SAFE_RELEASE(gr->device);
    SAFE_RELEASE(gr->adapter);
    SAFE_RELEASE(gr->dxgi_factory);
}

static LRESULT CALLBACK process_window_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
        case WM_KEYDOWN: {
            if (wparam == VK_ESCAPE) {
                PostQuitMessage(0);
                return 0;
            }
            break;
        }
        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

static HWND create_window(int width, int height)
{
    const WNDCLASSEXA winclass = {
        .cbSize = sizeof(winclass),
        .lpfnWndProc = process_window_message,
        .hInstance = GetModuleHandle(nullptr),
        .hCursor = LoadCursor(nullptr, IDC_ARROW),
        .lpszClassName = window_name,
    };
    if (!RegisterClassExA(&winclass)) assert(false);

    const DWORD style = WS_OVERLAPPEDWINDOW;

    RECT rect = { 0, 0, width, height };
    AdjustWindowRectEx(&rect, style, FALSE, 0);

    return CreateWindowExA(0, window_name, window_name, style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, winclass.hInstance, nullptr);
}

int main()
{
    ImGui_ImplWin32_EnableDpiAwareness();

    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    for (auto i = std::begin(v); i != std::end(v); ++i) {
        LOG("%d", *i);
    }

    const HWND window = create_window(1200, 800);
    LOG("Window DPI scale: %f", ImGui_ImplWin32_GetDpiScaleForHwnd(window));

    GraphicsContext gr = {};
    if (!init_graphics_context(&gr)) {
        deinit_graphics_context(&gr);
        return 1;
    }

    while (true) {
        MSG msg = {};
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                break;
            }
        } else {
            Sleep(1);
        }
    }

    deinit_graphics_context(&gr);
    return 0;
}
