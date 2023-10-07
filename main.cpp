#include "pch.h"

extern "C" {
    __declspec(dllexport) extern const u32 D3D12SDKVersion = 610;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\d3d12\\";
}

#define ENBALE_D3D12_DEBUG_LAYER 0
#define ENBALE_D3D12_VSYNC 1

constexpr const char* window_name = "game";
constexpr auto window_min_wh = 400;

constexpr auto num_gpu_frames = 2;
constexpr auto max_gpu_descriptors = 10000;

struct GraphicsContext {
    HWND window;
    i32 window_width;
    i32 window_height;

    IDXGIFactory7* dxgi_factory;
    IDXGIAdapter4* adapter;
    ID3D12Device12* device;

    ID3D12CommandQueue* command_queue;
    ID3D12CommandAllocator* command_allocators[num_gpu_frames];
    ID3D12GraphicsCommandList9* command_list;

#if ENBALE_D3D12_DEBUG_LAYER == 1
    ID3D12Debug6* debug;
    ID3D12DebugDevice2* debug_device;
    ID3D12DebugCommandQueue1* debug_command_queue;
    ID3D12DebugCommandList3* debug_command_list;
    ID3D12InfoQueue1* debug_info_queue;
#endif

    IDXGISwapChain4* swap_chain;
    UINT swap_chain_flags;
    UINT swap_chain_present_interval;
    ID3D12Resource* swap_chain_buffers[num_gpu_frames];

    ID3D12DescriptorHeap* rtv_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_heap_start;
    u32 rtv_heap_descriptor_size;

    ID3D12DescriptorHeap* gpu_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE gpu_heap_start_cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_heap_start_gpu;
    u32 gpu_heap_descriptor_size;

    ID3D12Fence* frame_fence;
    HANDLE frame_fence_event;
    u64 frame_fence_counter;
    u32 frame_index;

    ID3D12Resource2* msaa_srgb_rt;
};

static void present_gpu_frame(GraphicsContext* gr)
{
    assert(gr && gr->device);
    gr->frame_fence_counter += 1;

    UINT present_flags = 0;

    if (gr->swap_chain_present_interval == 0 && gr->swap_chain_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)
        present_flags |= DXGI_PRESENT_ALLOW_TEARING;

    VHR(gr->swap_chain->Present(gr->swap_chain_present_interval, present_flags));
    VHR(gr->command_queue->Signal(gr->frame_fence, gr->frame_fence_counter));

    const u64 gpu_frame_counter = gr->frame_fence->GetCompletedValue();
    if ((gr->frame_fence_counter - gpu_frame_counter) >= num_gpu_frames) {
        VHR(gr->frame_fence->SetEventOnCompletion(gpu_frame_counter + 1, gr->frame_fence_event));
        WaitForSingleObject(gr->frame_fence_event, INFINITE);
    }

    gr->frame_index = gr->swap_chain->GetCurrentBackBufferIndex();
}

static void finish_gpu_commands(GraphicsContext* gr)
{
    assert(gr && gr->device);
    gr->frame_fence_counter += 1;

    VHR(gr->command_queue->Signal(gr->frame_fence, gr->frame_fence_counter));
    VHR(gr->frame_fence->SetEventOnCompletion(gr->frame_fence_counter, gr->frame_fence_event));

    WaitForSingleObject(gr->frame_fence_event, INFINITE);
}

static bool handle_window_resize(GraphicsContext* gr)
{
    assert(gr && gr->device);

    RECT current_rect = {};
    GetClientRect(gr->window, &current_rect);

    if (current_rect.right == 0 && current_rect.bottom == 0) {
        // Window is minimized
        Sleep(10);
        return false; // Do not render.
    }

    if (current_rect.right != gr->window_width || current_rect.bottom != gr->window_height) {
        assert(current_rect.right >= window_min_wh / 2);
        assert(current_rect.bottom >= window_min_wh / 2);
        LOG("[graphics] Window resized to %dx%d", current_rect.right, current_rect.bottom);

        finish_gpu_commands(gr);

        for (i32 i = 0; i < num_gpu_frames; ++i) SAFE_RELEASE(gr->swap_chain_buffers[i]);

        VHR(gr->swap_chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, gr->swap_chain_flags));

        for (i32 i = 0; i < num_gpu_frames; ++i) {
            VHR(gr->swap_chain->GetBuffer(i, IID_PPV_ARGS(&gr->swap_chain_buffers[i])));
        }

        for (i32 i = 0; i < num_gpu_frames; ++i) {
            gr->device->CreateRenderTargetView(gr->swap_chain_buffers[i], nullptr, { .ptr = gr->rtv_heap_start.ptr + i * gr->rtv_heap_descriptor_size });
        }

        gr->window_width = current_rect.right;
        gr->window_height = current_rect.bottom;
        gr->frame_index = gr->swap_chain->GetCurrentBackBufferIndex();
    }

    return true; // Render normally.
}

static bool init_graphics_context(HWND window, GraphicsContext* gr)
{
    assert(gr && gr->device == nullptr);

    RECT rect = {};
    GetClientRect(window, &rect);

    gr->window = window;
    gr->window_width = rect.right;
    gr->window_height = rect.bottom;

    //
    // Factory, adapater, device
    //
#if ENBALE_D3D12_DEBUG_LAYER == 1
    VHR(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&gr->dxgi_factory)));
#else
    VHR(CreateDXGIFactory2(0, IID_PPV_ARGS(&gr->dxgi_factory)));
#endif

    VHR(gr->dxgi_factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&gr->adapter)));

    DXGI_ADAPTER_DESC3 adapter_desc = {};
    VHR(gr->adapter->GetDesc3(&adapter_desc));

    LOG("[graphics] Adapter: %S", adapter_desc.Description);

#if ENBALE_D3D12_DEBUG_LAYER == 1
    VHR(D3D12GetDebugInterface(IID_PPV_ARGS(&gr->debug)));
    gr->debug->EnableDebugLayer();
#endif

    if (FAILED(D3D12CreateDevice(gr->adapter, D3D_FEATURE_LEVEL_11_1, IID_PPV_ARGS(&gr->device)))) return false;

#if ENBALE_D3D12_DEBUG_LAYER == 1
    VHR(gr->device->QueryInterface(IID_PPV_ARGS(&gr->debug_device)));
    VHR(gr->device->QueryInterface(IID_PPV_ARGS(&gr->debug_info_queue)));
    VHR(gr->debug_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE));
#endif

    LOG("[graphics] D3D12 device created");

    //
    // Check required features support
    //
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12 = {};
        D3D12_FEATURE_DATA_SHADER_MODEL shader_model = { .HighestShaderModel = D3D_HIGHEST_SHADER_MODEL };

        VHR(gr->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
        VHR(gr->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12)));
        VHR(gr->device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader_model, sizeof(shader_model)));

        if (options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3) {
            LOG("[graphics] Resource Binding Tier 3 is NOT SUPPORTED - please update your driver");
            return false;
        }
        LOG("[graphics] Resource Binding Tier 3 is SUPPORTED");

        if (options12.EnhancedBarriersSupported == FALSE) {
            LOG("[graphics] Enhanced Barriers API is NOT SUPPORTED - please update your driver");
            return false;
        }
        LOG("[graphics] Enhanced Barriers API is SUPPORTED");

        if (shader_model.HighestShaderModel < D3D_SHADER_MODEL_6_6) {
            LOG("[graphics] Shader Model 6.6 is NOT SUPPORTED - please update your driver");
            return false;
        }
        LOG("[graphics] Shader Model 6.6 is SUPPORTED");
    }

    //
    // Commands
    //
    const D3D12_COMMAND_QUEUE_DESC command_queue_desc = {
        .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
        .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
        .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
        .NodeMask = 0,
    };
    VHR(gr->device->CreateCommandQueue(&command_queue_desc, IID_PPV_ARGS(&gr->command_queue)));

#if ENBALE_D3D12_DEBUG_LAYER == 1
    VHR(gr->command_queue->QueryInterface(IID_PPV_ARGS(&gr->debug_command_queue)));
#endif

    LOG("[graphics] Command queue created");

    for (i32 i = 0; i < num_gpu_frames; ++i) {
        VHR(gr->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gr->command_allocators[i])));
    }

    LOG("[graphics] Command allocators created");

    VHR(gr->device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&gr->command_list)));

#if ENBALE_D3D12_DEBUG_LAYER == 1
    VHR(gr->command_list->QueryInterface(IID_PPV_ARGS(&gr->debug_command_list)));
#endif

    LOG("[graphics] Command list created");

    //
    // Swap chain
    //
    /* Swap chain flags */ {
        gr->swap_chain_flags = 0;
        gr->swap_chain_present_interval = ENBALE_D3D12_VSYNC;

        BOOL allow_tearing = FALSE;
        const HRESULT hr = gr->dxgi_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing));

        if (SUCCEEDED(hr) && allow_tearing == TRUE) {
            gr->swap_chain_flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        }
    }

    const DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {
        .Width = static_cast<u32>(gr->window_width),
        .Height = static_cast<u32>(gr->window_height),
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .Stereo = FALSE,
        .SampleDesc = { .Count = 1 },
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = num_gpu_frames,
        .Scaling = DXGI_SCALING_NONE,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED,
        .Flags = gr->swap_chain_flags,
    };
    IDXGISwapChain1* swap_chain1 = nullptr;
    VHR(gr->dxgi_factory->CreateSwapChainForHwnd(gr->command_queue, window, &swap_chain_desc, nullptr, nullptr, &swap_chain1));
    defer { SAFE_RELEASE(swap_chain1); };

    VHR(swap_chain1->QueryInterface(IID_PPV_ARGS(&gr->swap_chain)));

    VHR(gr->dxgi_factory->MakeWindowAssociation(window, DXGI_MWA_NO_WINDOW_CHANGES));

    for (i32 i = 0; i < num_gpu_frames; ++i) {
        VHR(gr->swap_chain->GetBuffer(i, IID_PPV_ARGS(&gr->swap_chain_buffers[i])));
    }

    LOG("[graphics] Swap chain created");

    //
    // RTV descriptor heap
    //
    const D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        .NumDescriptors = 1024,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
        .NodeMask = 0,
    };
    VHR(gr->device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&gr->rtv_heap)));
    gr->rtv_heap_start = gr->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    gr->rtv_heap_descriptor_size = gr->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (i32 i = 0; i < num_gpu_frames; ++i) {
        gr->device->CreateRenderTargetView(gr->swap_chain_buffers[i], nullptr, { .ptr = gr->rtv_heap_start.ptr + i * gr->rtv_heap_descriptor_size });
    }

    LOG("[graphics] Render target view (RTV) descriptor heap created");

    //
    // CBV, SRV, UAV descriptor heap
    //
    const D3D12_DESCRIPTOR_HEAP_DESC gpu_heap_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = max_gpu_descriptors,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
        .NodeMask = 0,
    };
    VHR(gr->device->CreateDescriptorHeap(&gpu_heap_desc, IID_PPV_ARGS(&gr->gpu_heap)));
    gr->gpu_heap_start_cpu = gr->gpu_heap->GetCPUDescriptorHandleForHeapStart();
    gr->gpu_heap_start_gpu = gr->gpu_heap->GetGPUDescriptorHandleForHeapStart();
    gr->gpu_heap_descriptor_size = gr->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    LOG("[graphics] GPU descriptor heap (CBV_SRV_UAV, SHADER_VISIBLE) created");

    //
    // Frame fence
    //
    VHR(gr->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gr->frame_fence)));

    gr->frame_fence_event = CreateEventEx(nullptr, "frame_fence_event", 0, EVENT_ALL_ACCESS);
    if (gr->frame_fence_event == nullptr) VHR(HRESULT_FROM_WIN32(GetLastError()));

    gr->frame_fence_counter = 0;
    gr->frame_index = gr->swap_chain->GetCurrentBackBufferIndex();

    LOG("[graphics] Frame fence created");

    //
    // MSAA, SRGB render target
    //
    {
        const D3D12_HEAP_PROPERTIES heap_desc = { .Type = D3D12_HEAP_TYPE_DEFAULT };
        const D3D12_RESOURCE_DESC1 desc = {
            .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            .Width = static_cast<u64>(gr->window_width),
            .Height = static_cast<u32>(gr->window_height),
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            .SampleDesc = { .Count = 8 },
            .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        };
        VHR(gr->device->CreateCommittedResource3(&heap_desc, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_RENDER_TARGET, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&gr->msaa_srgb_rt)));

        gr->device->CreateRenderTargetView(gr->msaa_srgb_rt, nullptr, { .ptr = gr->rtv_heap_start.ptr + num_gpu_frames * gr->rtv_heap_descriptor_size });
    }

#if ENBALE_D3D12_DEBUG_LAYER == 1
    gr->debug_command_queue->AssertResourceAccess(gr->msaa_srgb_rt, 0, D3D12_BARRIER_ACCESS_COMMON);
    gr->debug_command_queue->AssertTextureLayout(gr->msaa_srgb_rt, 0, D3D12_BARRIER_LAYOUT_RENDER_TARGET);
#endif

    return true;
}

static void deinit_graphics_context(GraphicsContext* gr)
{
    assert(gr);
    finish_gpu_commands(gr);

    SAFE_RELEASE(gr->msaa_srgb_rt);
    SAFE_RELEASE(gr->command_list);
    for (i32 i = 0; i < num_gpu_frames; ++i) SAFE_RELEASE(gr->command_allocators[i]);
    if (gr->frame_fence_event) {
        CloseHandle(gr->frame_fence_event);
        gr->frame_fence_event = nullptr;
    }
    SAFE_RELEASE(gr->frame_fence);
    SAFE_RELEASE(gr->gpu_heap);
    SAFE_RELEASE(gr->rtv_heap);
    for (i32 i = 0; i < num_gpu_frames; ++i) SAFE_RELEASE(gr->swap_chain_buffers[i]);
    SAFE_RELEASE(gr->swap_chain);
    SAFE_RELEASE(gr->command_queue);
    SAFE_RELEASE(gr->device);
    SAFE_RELEASE(gr->adapter);
    SAFE_RELEASE(gr->dxgi_factory);

#if ENBALE_D3D12_DEBUG_LAYER == 1
    SAFE_RELEASE(gr->debug_command_list);
    SAFE_RELEASE(gr->debug_command_queue);
    SAFE_RELEASE(gr->debug_info_queue);
    SAFE_RELEASE(gr->debug);

    if (gr->debug_device) {
        VHR(gr->debug_device->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL));

        const auto refcount = gr->debug_device->Release();
        assert(refcount == 0);

        gr->debug_device = nullptr;
    }
#endif
}

static LRESULT CALLBACK process_window_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    const LRESULT imgui_result = ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);
    if (imgui_result != 0)
        return imgui_result;

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
            info->ptMinTrackSize.x = window_min_wh;
            info->ptMinTrackSize.y = window_min_wh;
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
    if (!RegisterClassEx(&winclass))
        VHR(HRESULT_FROM_WIN32(GetLastError()));

    LOG("[core] Window class registered");

    const DWORD style = WS_OVERLAPPEDWINDOW;

    RECT rect = { 0, 0, width, height };
    AdjustWindowRectEx(&rect, style, FALSE, 0);

    const HWND window = CreateWindowEx(0, window_name, window_name, style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, winclass.hInstance, nullptr);
    if (!window)
        VHR(HRESULT_FROM_WIN32(GetLastError()));

    LOG("[core] Window created");

    return window;
}

static void draw_frame(GraphicsContext* gr)
{
    ID3D12CommandAllocator* command_allocator = gr->command_allocators[gr->frame_index];
    VHR(command_allocator->Reset());
    VHR(gr->command_list->Reset(command_allocator, nullptr));

    gr->command_list->SetDescriptorHeaps(1, &gr->gpu_heap);

    /* Viewport */ {
        const D3D12_VIEWPORT viewport = {
            .TopLeftX = 0.0f,
            .TopLeftY = 0.0f,
            .Width = static_cast<f32>(gr->window_width),
            .Height = static_cast<f32>(gr->window_height),
            .MinDepth = 0.0f,
            .MaxDepth = 0.0f,
        };
        gr->command_list->RSSetViewports(1, &viewport);
    }
    /* Scissor */ {
        const D3D12_RECT scissor_rect = {
            .left = 0,
            .top = 0,
            .right = gr->window_width,
            .bottom = gr->window_height,
        };
        gr->command_list->RSSetScissorRects(1, &scissor_rect);
    }

    /* D3D12_BARRIER_LAYOUT_PRESENT -> D3D12_BARRIER_LAYOUT_RENDER_TARGET */ {
        const D3D12_TEXTURE_BARRIER texture_barrier = {
            .SyncBefore = D3D12_BARRIER_SYNC_RENDER_TARGET,
            .SyncAfter = D3D12_BARRIER_SYNC_RENDER_TARGET,
            .AccessBefore = D3D12_BARRIER_ACCESS_COMMON,
            .AccessAfter = D3D12_BARRIER_ACCESS_RENDER_TARGET,
            .LayoutBefore = D3D12_BARRIER_LAYOUT_PRESENT,
            .LayoutAfter = D3D12_BARRIER_LAYOUT_RENDER_TARGET,
            .pResource = gr->swap_chain_buffers[gr->frame_index],
            .Subresources = { .IndexOrFirstMipLevel = 0xffffffff },
        };
        const D3D12_BARRIER_GROUP barrier_group = {
            .Type = D3D12_BARRIER_TYPE_TEXTURE,
            .NumBarriers = 1,
            .pTextureBarriers = &texture_barrier,
        };
        gr->command_list->Barrier(1, &barrier_group);
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE back_buffer_descriptor = {
        .ptr = gr->rtv_heap_start.ptr + gr->frame_index * gr->rtv_heap_descriptor_size
    };

    gr->command_list->OMSetRenderTargets(1, &back_buffer_descriptor, TRUE, nullptr);
    gr->command_list->ClearRenderTargetView(back_buffer_descriptor, XMVECTORF32{ 0.2f, 0.4f, 0.8f, 1.0 }, 0, nullptr);

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), gr->command_list);

    /* D3D12_BARRIER_LAYOUT_RENDER_TARGET -> D3D12_BARRIER_LAYOUT_PRESENT */ {
        const D3D12_TEXTURE_BARRIER texture_barrier = {
            .SyncBefore = D3D12_BARRIER_SYNC_RENDER_TARGET,
            .SyncAfter = D3D12_BARRIER_SYNC_RENDER_TARGET,
            .AccessBefore = D3D12_BARRIER_ACCESS_RENDER_TARGET,
            .AccessAfter = D3D12_BARRIER_ACCESS_COMMON,
            .LayoutBefore = D3D12_BARRIER_LAYOUT_RENDER_TARGET,
            .LayoutAfter = D3D12_BARRIER_LAYOUT_PRESENT,
            .pResource = gr->swap_chain_buffers[gr->frame_index],
            .Subresources = { .IndexOrFirstMipLevel = 0xffffffff },
        };
        const D3D12_BARRIER_GROUP barrier_group = {
            .Type = D3D12_BARRIER_TYPE_TEXTURE,
            .NumBarriers = 1,
            .pTextureBarriers = &texture_barrier,
        };
        gr->command_list->Barrier(1, &barrier_group);
    }

    VHR(gr->command_list->Close());

    gr->command_queue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList**>(&gr->command_list));

    present_gpu_frame(gr);
}

static f64 get_time() {
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

static void update_frame_stats(HWND window, const char* name, f64* out_time, f32* out_delta_time) {
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

i32 main()
{
    ImGui_ImplWin32_EnableDpiAwareness();
    const f32 dpi_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(nullptr);
    LOG("[graphics] Window DPI scale: %f", dpi_scale);

    const HWND window = create_window(static_cast<i32>(1200 * dpi_scale), static_cast<i32>(800 * dpi_scale));

    GraphicsContext gr = {};
    defer { deinit_graphics_context(&gr); };

    if (!init_graphics_context(window, &gr)) {
        // TODO: Display message box in release mode.
        return 1;
    }

    ImGui::CreateContext();
    defer { ImGui::DestroyContext(); };

    ImGui::GetIO().Fonts->AddFontFromFileTTF("Roboto-Medium.ttf", floor(16.0f * dpi_scale));

    if (!ImGui_ImplWin32_Init(window)) return 1;
    defer { ImGui_ImplWin32_Shutdown(); };

    if (!ImGui_ImplDX12_Init(gr.device, num_gpu_frames, DXGI_FORMAT_R8G8B8A8_UNORM, gr.gpu_heap, gr.gpu_heap_start_cpu, gr.gpu_heap_start_gpu)) return 1;
    defer {
        finish_gpu_commands(&gr);
        ImGui_ImplDX12_Shutdown();
    };

    ImGui::GetStyle().ScaleAllSizes(dpi_scale);

    while (true) {
        MSG msg = {};
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) break;
        } else {
            if (!handle_window_resize(&gr)) continue;

            f64 time;
            f32 delta_time;
            update_frame_stats(gr.window, window_name, &time, &delta_time);

            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            ImGui::ShowDemoWindow();

            ImGui::Render();

            draw_frame(&gr);
        }
    }

    return 0;
}
