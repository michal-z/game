#include "pch.h"

extern "C" {
    __declspec(dllexport) extern const u32 D3D12SDKVersion = 610;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\d3d12\\";
}

constexpr const char* window_name = "my_game";
constexpr bool enable_debug_layer = true;
constexpr auto num_graphics_frames = 2;

struct GraphicsContext {
    IDXGIFactory7* dxgi_factory;
    IDXGIAdapter4* adapter;
    ID3D12Device12* device;

    ID3D12CommandQueue* command_queue;

    IDXGISwapChain4* swap_chain;
    ID3D12Resource* swap_chain_buffers[num_graphics_frames];

    ID3D12DescriptorHeap* rtv_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_heap_start;
    u32 rtv_heap_descriptor_size;

    ID3D12Fence* frame_fence;
    HANDLE frame_fence_event;
    u64 frame_fence_counter;
    u32 frame_fence_index;
};

static bool init_graphics_context(HWND window, GraphicsContext* gr)
{
    assert(gr && gr->device == nullptr);

    VHR(CreateDXGIFactory2(enable_debug_layer ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&gr->dxgi_factory)));

    VHR(gr->dxgi_factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&gr->adapter)));

    DXGI_ADAPTER_DESC3 adapter_desc = {};
    VHR(gr->adapter->GetDesc3(&adapter_desc));

    LOG("[graphics] Adapter: %S", adapter_desc.Description);

    if (enable_debug_layer) {
        ID3D12Debug6* debug = nullptr;
        D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
        if (debug) {
            debug->EnableDebugLayer();
            SAFE_RELEASE(debug);
        }
    }

    VHR(D3D12CreateDevice(gr->adapter, D3D_FEATURE_LEVEL_11_1, IID_PPV_ARGS(&gr->device)));

    LOG("[graphics] D3D12 device created");

    const D3D12_COMMAND_QUEUE_DESC command_queue_desc = {
        .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
        .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
        .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
        .NodeMask = 0,
    };
    VHR(gr->device->CreateCommandQueue(&command_queue_desc, IID_PPV_ARGS(&gr->command_queue)));

    LOG("[graphics] Command queue created");

    RECT rect = {};
    GetClientRect(window, &rect);

    const DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {
        .Width = static_cast<u32>(rect.right),
        .Height = static_cast<u32>(rect.bottom),
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .Stereo = FALSE,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = num_graphics_frames,
        .Scaling = DXGI_SCALING_NONE,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED,
        .Flags = 0,
    };
    IDXGISwapChain1* swap_chain1 = nullptr;
    VHR(gr->dxgi_factory->CreateSwapChainForHwnd(gr->command_queue, window, &swap_chain_desc, nullptr, nullptr, &swap_chain1));
    defer { SAFE_RELEASE(swap_chain1); };

    VHR(swap_chain1->QueryInterface(IID_PPV_ARGS(&gr->swap_chain)));

    VHR(gr->dxgi_factory->MakeWindowAssociation(window, DXGI_MWA_NO_WINDOW_CHANGES));

    for (i32 i = 0; i < num_graphics_frames; ++i) {
        VHR(gr->swap_chain->GetBuffer(i, IID_PPV_ARGS(&gr->swap_chain_buffers[i])));
    }

    LOG("[graphics] Swap chain created");

    const D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        .NumDescriptors = 1024,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
        .NodeMask = 0,
    };
    VHR(gr->device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&gr->rtv_heap)));
    gr->rtv_heap_start = gr->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    gr->rtv_heap_descriptor_size = gr->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (i32 i = 0; i < num_graphics_frames; ++i) {
        gr->device->CreateRenderTargetView(gr->swap_chain_buffers[i], nullptr, { .ptr = gr->rtv_heap_start.ptr + i * gr->rtv_heap_descriptor_size });
    }

    LOG("[graphics] Render target view (RTV) heap created");

    VHR(gr->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gr->frame_fence)));

    gr->frame_fence_event = CreateEventEx(nullptr, "frame_fence_event", 0, EVENT_ALL_ACCESS);
    if (gr->frame_fence_event == nullptr) return false;

    gr->frame_fence_counter = 0;
    gr->frame_fence_index = gr->swap_chain->GetCurrentBackBufferIndex();

    LOG("[graphics] Frame fence created");

    return true;
}

static void deinit_graphics_context(GraphicsContext* gr)
{
    assert(gr);
    if (gr->frame_fence_event) {
        CloseHandle(gr->frame_fence_event);
        gr->frame_fence_event = nullptr;
    }
    SAFE_RELEASE(gr->frame_fence);
    SAFE_RELEASE(gr->rtv_heap);
    for (i32 i = 0; i < num_graphics_frames; ++i) SAFE_RELEASE(gr->swap_chain_buffers[i]);
    SAFE_RELEASE(gr->swap_chain);
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
        } break;
        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        } break;
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = 400;
            info->ptMinTrackSize.y = 400;
            return 0;
        } break;
    }
    return DefWindowProc(window, message, wparam, lparam);
}

static HWND create_window(i32 width, i32 height)
{
    const WNDCLASSEXA winclass = {
        .cbSize = sizeof(winclass),
        .lpfnWndProc = process_window_message,
        .hInstance = GetModuleHandle(nullptr),
        .hCursor = LoadCursor(nullptr, IDC_ARROW),
        .lpszClassName = window_name,
    };
    if (!RegisterClassEx(&winclass)) return nullptr;

    LOG("[core] Window class registered");

    const DWORD style = WS_OVERLAPPEDWINDOW;

    RECT rect = { 0, 0, width, height };
    AdjustWindowRectEx(&rect, style, FALSE, 0);

    const HWND window = CreateWindowEx(0, window_name, window_name, style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, winclass.hInstance, nullptr);
    if (!window) return nullptr;

    LOG("[core] Window created");

    return window;
}

i32 main()
{
    ImGui_ImplWin32_EnableDpiAwareness();

    std::vector<i32> v;
    v.push_back(1);
    v.push_back(2);
    for (auto i = v.begin(); i != v.end(); ++i) {
        LOG("[test] %d", *i);
    }

    const HWND window = create_window(1200, 800);
    LOG("[graphics] Window DPI scale: %f", ImGui_ImplWin32_GetDpiScaleForHwnd(window));

    GraphicsContext gr = {};
    defer { deinit_graphics_context(&gr); };

    if (!init_graphics_context(window, &gr)) return 1;

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

    return 0;
}
