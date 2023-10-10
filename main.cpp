#include "pch.h"

extern "C" {
    __declspec(dllexport) extern const u32 D3D12SDKVersion = 610;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\d3d12\\";
}

#define ENBALE_D3D12_DEBUG_LAYER 1
#define ENBALE_D3D12_GPU_BASED_VALIDATION 0
#define ENBALE_D3D12_VSYNC 1

#define WINDOW_NAME "game"
#define WINDOW_MIN_WH 400

#define NUM_GPU_FRAMES 2
#define MAX_GPU_DESCRIPTORS (16 * 1024)
#define NUM_MSAA_SAMPLES 8
#define CLEAR_COLOR { 0.2f, 0.4f, 0.8f, 1.0 }

struct GraphicsContext {
    HWND window;
    i32 window_width;
    i32 window_height;

    IDXGIFactory7* dxgi_factory;
    IDXGIAdapter4* adapter;
    ID3D12Device12* device;

    ID3D12CommandQueue* command_queue;
    ID3D12CommandAllocator* command_allocators[NUM_GPU_FRAMES];
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
    ID3D12Resource* swap_chain_buffers[NUM_GPU_FRAMES];

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
    if ((gr->frame_fence_counter - gpu_frame_counter) >= NUM_GPU_FRAMES) {
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

static void create_msaa_srgb_render_target(GraphicsContext* gr)
{
    assert(gr && gr->device);

    const D3D12_HEAP_PROPERTIES heap_desc = { .Type = D3D12_HEAP_TYPE_DEFAULT };
    const D3D12_RESOURCE_DESC1 desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = static_cast<u64>(gr->window_width),
        .Height = static_cast<u32>(gr->window_height),
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        .SampleDesc = { .Count = NUM_MSAA_SAMPLES },
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
    };
    const D3D12_CLEAR_VALUE clear_value = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        .Color = CLEAR_COLOR,
    };
    VHR(gr->device->CreateCommittedResource3(&heap_desc, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_RENDER_TARGET, &clear_value, nullptr, 0, nullptr, IID_PPV_ARGS(&gr->msaa_srgb_rt)));

    gr->device->CreateRenderTargetView(gr->msaa_srgb_rt, nullptr, { .ptr = gr->rtv_heap_start.ptr + NUM_GPU_FRAMES * gr->rtv_heap_descriptor_size });

    LOG("[graphics] MSAAx%d SRGB render target created (%dx%d)", NUM_MSAA_SAMPLES, gr->window_width, gr->window_height);
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
        assert(current_rect.right >= WINDOW_MIN_WH / 2);
        assert(current_rect.bottom >= WINDOW_MIN_WH / 2);
        LOG("[graphics] Window resized to %dx%d", current_rect.right, current_rect.bottom);

        finish_gpu_commands(gr);

        for (i32 i = 0; i < NUM_GPU_FRAMES; ++i) SAFE_RELEASE(gr->swap_chain_buffers[i]);

        VHR(gr->swap_chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, gr->swap_chain_flags));

        for (i32 i = 0; i < NUM_GPU_FRAMES; ++i) {
            VHR(gr->swap_chain->GetBuffer(i, IID_PPV_ARGS(&gr->swap_chain_buffers[i])));
        }

        for (i32 i = 0; i < NUM_GPU_FRAMES; ++i) {
            gr->device->CreateRenderTargetView(gr->swap_chain_buffers[i], nullptr, { .ptr = gr->rtv_heap_start.ptr + i * gr->rtv_heap_descriptor_size });
        }

        gr->window_width = current_rect.right;
        gr->window_height = current_rect.bottom;
        gr->frame_index = gr->swap_chain->GetCurrentBackBufferIndex();

        SAFE_RELEASE(gr->msaa_srgb_rt);
        create_msaa_srgb_render_target(gr);
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
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&gr->debug)))) {
        LOG("[graphics] Failed to load D3D12 debug layer. Please rebuild with `ENBALE_D3D12_DEBUG_LAYER 0` and try again.");
        return false;
    }
    gr->debug->EnableDebugLayer();
    LOG("[graphics] D3D12 Debug Layer enabled");
#if ENBALE_D3D12_GPU_BASED_VALIDATION == 1
    gr->debug->SetEnableGPUBasedValidation(TRUE);
    LOG("[graphics] D3D12 GPU-Based Validation enabled");
#endif
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

    for (i32 i = 0; i < NUM_GPU_FRAMES; ++i) {
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
        .BufferCount = NUM_GPU_FRAMES,
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

    for (i32 i = 0; i < NUM_GPU_FRAMES; ++i) {
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

    for (i32 i = 0; i < NUM_GPU_FRAMES; ++i) {
        gr->device->CreateRenderTargetView(gr->swap_chain_buffers[i], nullptr, { .ptr = gr->rtv_heap_start.ptr + i * gr->rtv_heap_descriptor_size });
    }

    LOG("[graphics] Render target view (RTV) descriptor heap created");

    //
    // CBV, SRV, UAV descriptor heap
    //
    const D3D12_DESCRIPTOR_HEAP_DESC gpu_heap_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = MAX_GPU_DESCRIPTORS,
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
    create_msaa_srgb_render_target(gr);

    return true;
}

static void shutdown_graphics_context(GraphicsContext* gr)
{
    assert(gr);

    SAFE_RELEASE(gr->msaa_srgb_rt);
    SAFE_RELEASE(gr->command_list);
    for (i32 i = 0; i < NUM_GPU_FRAMES; ++i) SAFE_RELEASE(gr->command_allocators[i]);
    if (gr->frame_fence_event) {
        CloseHandle(gr->frame_fence_event);
        gr->frame_fence_event = nullptr;
    }
    SAFE_RELEASE(gr->frame_fence);
    SAFE_RELEASE(gr->gpu_heap);
    SAFE_RELEASE(gr->rtv_heap);
    for (i32 i = 0; i < NUM_GPU_FRAMES; ++i) SAFE_RELEASE(gr->swap_chain_buffers[i]);
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
            info->ptMinTrackSize.x = WINDOW_MIN_WH;
            info->ptMinTrackSize.y = WINDOW_MIN_WH;
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
        .lpszClassName = WINDOW_NAME,
    };
    if (!RegisterClassEx(&winclass))
        VHR(HRESULT_FROM_WIN32(GetLastError()));

    LOG("[game] Window class registered");

    const DWORD style = WS_OVERLAPPEDWINDOW;

    RECT rect = { 0, 0, width, height };
    AdjustWindowRectEx(&rect, style, FALSE, 0);

    const HWND window = CreateWindowEx(0, WINDOW_NAME, WINDOW_NAME, style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, winclass.hInstance, nullptr);
    if (!window)
        VHR(HRESULT_FROM_WIN32(GetLastError()));

    LOG("[game] Window created");

    return window;
}

static f64 get_time()
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

static void update_frame_stats(HWND window, const char* name, f64* out_time, f32* out_delta_time)
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

static std::vector<u8> load_file(const char* filename)
{
    FILE* file = fopen(filename, "rb");
    assert(file);
    fseek(file, 0, SEEK_END);
    const i32 size_in_bytes = ftell(file);
    assert(size_in_bytes > 0);
    fseek(file, 0, SEEK_SET);
    std::vector<u8> data(size_in_bytes);
    const usize num_read_bytes = fread(&data[0], 1, size_in_bytes, file);
    fclose(file);
    assert(size_in_bytes == num_read_bytes);
    return data;
}

#define NUM_GPU_PIPELINES 1

struct GameState {
    GraphicsContext gr;
    ID3D12Resource2* vertex_buffer;
    ID3D12PipelineState* gpu_pipelines[NUM_GPU_PIPELINES];
    ID3D12RootSignature* gpu_root_signatures[NUM_GPU_PIPELINES];
    bool is_window_minimized;
};

static void draw(GameState* game_state);

static void draw_frame(GameState* game_state)
{
    assert(game_state);

    if (game_state->is_window_minimized)
        return;

    GraphicsContext* gr = &game_state->gr;

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
            .MaxDepth = 1.0f,
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

    {
        const D3D12_CPU_DESCRIPTOR_HANDLE rt_descriptor = {
            .ptr = gr->rtv_heap_start.ptr + NUM_GPU_FRAMES * gr->rtv_heap_descriptor_size
        };
        const f32 clear_color[] = CLEAR_COLOR;

        gr->command_list->OMSetRenderTargets(1, &rt_descriptor, TRUE, nullptr);
        gr->command_list->ClearRenderTargetView(rt_descriptor, clear_color, 0, nullptr);
    }

    draw(game_state);

    {
        const D3D12_TEXTURE_BARRIER texture_barriers[] = {
            {
                .SyncBefore = D3D12_BARRIER_SYNC_RENDER_TARGET,
                .SyncAfter = D3D12_BARRIER_SYNC_RESOLVE,
                .AccessBefore = D3D12_BARRIER_ACCESS_RENDER_TARGET,
                .AccessAfter = D3D12_BARRIER_ACCESS_RESOLVE_SOURCE,
                .LayoutBefore = D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                .LayoutAfter = D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,
                .pResource = gr->msaa_srgb_rt,
                .Subresources = { .IndexOrFirstMipLevel = 0xffffffff },
            }, {
                .SyncBefore = D3D12_BARRIER_SYNC_NONE,
                .SyncAfter = D3D12_BARRIER_SYNC_RESOLVE,
                .AccessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS,
                .AccessAfter = D3D12_BARRIER_ACCESS_RESOLVE_DEST,
                .LayoutBefore = D3D12_BARRIER_LAYOUT_PRESENT,
                .LayoutAfter = D3D12_BARRIER_LAYOUT_RESOLVE_DEST,
                .pResource = gr->swap_chain_buffers[gr->frame_index],
                .Subresources = { .IndexOrFirstMipLevel = 0xffffffff },
            },
        };
        const D3D12_BARRIER_GROUP barrier_group = {
            .Type = D3D12_BARRIER_TYPE_TEXTURE,
            .NumBarriers = ARRAYSIZE(texture_barriers),
            .pTextureBarriers = texture_barriers,
        };
        gr->command_list->Barrier(1, &barrier_group);
    }

    gr->command_list->ResolveSubresource(gr->swap_chain_buffers[gr->frame_index], 0, gr->msaa_srgb_rt, 0, DXGI_FORMAT_R8G8B8A8_UNORM);

    {
        const D3D12_TEXTURE_BARRIER texture_barrier = {
            .SyncBefore = D3D12_BARRIER_SYNC_RESOLVE,
            .SyncAfter = D3D12_BARRIER_SYNC_RENDER_TARGET,
            .AccessBefore = D3D12_BARRIER_ACCESS_RESOLVE_DEST,
            .AccessAfter = D3D12_BARRIER_ACCESS_RENDER_TARGET,
            .LayoutBefore = D3D12_BARRIER_LAYOUT_RESOLVE_DEST,
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

    {
        const D3D12_CPU_DESCRIPTOR_HANDLE back_buffer_descriptor = {
            .ptr = gr->rtv_heap_start.ptr + gr->frame_index * gr->rtv_heap_descriptor_size
        };
        gr->command_list->OMSetRenderTargets(1, &back_buffer_descriptor, TRUE, nullptr);
    }

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), gr->command_list);

    {
        const D3D12_TEXTURE_BARRIER texture_barriers[] = {
            {
                .SyncBefore = D3D12_BARRIER_SYNC_RENDER_TARGET,
                .SyncAfter = D3D12_BARRIER_SYNC_NONE,
                .AccessBefore = D3D12_BARRIER_ACCESS_RENDER_TARGET,
                .AccessAfter = D3D12_BARRIER_ACCESS_NO_ACCESS,
                .LayoutBefore = D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                .LayoutAfter = D3D12_BARRIER_LAYOUT_PRESENT,
                .pResource = gr->swap_chain_buffers[gr->frame_index],
                .Subresources = { .IndexOrFirstMipLevel = 0xffffffff },
            }, {
                .SyncBefore = D3D12_BARRIER_SYNC_RESOLVE,
                .SyncAfter = D3D12_BARRIER_SYNC_NONE,
                .AccessBefore = D3D12_BARRIER_ACCESS_RESOLVE_SOURCE,
                .AccessAfter = D3D12_BARRIER_ACCESS_NO_ACCESS,
                .LayoutBefore = D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,
                .LayoutAfter = D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                .pResource = gr->msaa_srgb_rt,
                .Subresources = { .IndexOrFirstMipLevel = 0xffffffff },
            },
        };
        const D3D12_BARRIER_GROUP barrier_group = {
            .Type = D3D12_BARRIER_TYPE_TEXTURE,
            .NumBarriers = ARRAYSIZE(texture_barriers),
            .pTextureBarriers = texture_barriers,
        };
        gr->command_list->Barrier(1, &barrier_group);
    }

    VHR(gr->command_list->Close());

    gr->command_queue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList**>(&gr->command_list));

    present_gpu_frame(gr);
}

static void init(GameState* game_state)
{
    assert(game_state);

    ImGui_ImplWin32_EnableDpiAwareness();
    const f32 dpi_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(nullptr);
    LOG("[game] Window DPI scale: %f", dpi_scale);

    const HWND window = create_window(static_cast<i32>(1200 * dpi_scale), static_cast<i32>(800 * dpi_scale));

    if (!init_graphics_context(window, &game_state->gr)) {
        // TODO: Display message box in release mode.
        VHR(E_FAIL);
    }

    GraphicsContext* gr = &game_state->gr;

    ImGui::CreateContext();
    ImGui::GetIO().Fonts->AddFontFromFileTTF("assets/Roboto-Medium.ttf", floor(16.0f * dpi_scale));

    if (!ImGui_ImplWin32_Init(window)) VHR(E_FAIL);
    if (!ImGui_ImplDX12_Init(gr->device, NUM_GPU_FRAMES, DXGI_FORMAT_R8G8B8A8_UNORM, gr->gpu_heap, gr->gpu_heap_start_cpu, gr->gpu_heap_start_gpu)) VHR(E_FAIL);

    ImGui::GetStyle().ScaleAllSizes(dpi_scale);

    {
        const std::vector<u8> vs = load_file("assets/s00_vs.cso");
        const std::vector<u8> ps = load_file("assets/s00_ps.cso");

        const D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {
            .VS = { vs.data(), vs.size() },
            .PS = { ps.data(), ps.size() },
            .BlendState = {
                .RenderTarget = {
                    { .RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL },
                },
            },
            .SampleMask = 0xffffffff,
            .RasterizerState = {
                .FillMode = D3D12_FILL_MODE_SOLID,
                .CullMode = D3D12_CULL_MODE_NONE,
            },
            .PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
            .NumRenderTargets = 1,
            .RTVFormats = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB },
            .SampleDesc = { .Count = NUM_MSAA_SAMPLES },
        };

        VHR(gr->device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&game_state->gpu_pipelines[0])));
        VHR(gr->device->CreateRootSignature(0, vs.data(), vs.size(), IID_PPV_ARGS(&game_state->gpu_root_signatures[0])));
    }

    {
        const D3D12_HEAP_PROPERTIES heap_desc = { .Type = D3D12_HEAP_TYPE_UPLOAD };
        const D3D12_RESOURCE_DESC1 desc = {
            .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Width = 1024, // TODO
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .SampleDesc = { .Count = 1 },
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        };
        VHR(gr->device->CreateCommittedResource3(&heap_desc, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&game_state->vertex_buffer)));

        const D3D12_RANGE range = { .Begin = 0, .End = 0 };
        XMFLOAT2* ptr = nullptr;
        VHR(game_state->vertex_buffer->Map(0, &range, reinterpret_cast<void**>(&ptr)));
        *ptr++ = XMFLOAT2(-0.9f, -0.7f);
        *ptr++ = XMFLOAT2(0.0f, 0.9f);
        *ptr++ = XMFLOAT2(0.9f, -0.9f);
        game_state->vertex_buffer->Unmap(0, nullptr);

        const D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {
            .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Buffer = {
                .FirstElement = 0,
                .NumElements = 3,
                .StructureByteStride = sizeof(XMFLOAT2), // TODO: Vertex
            },
        };
        gr->device->CreateShaderResourceView(game_state->vertex_buffer, &srv_desc, { .ptr = gr->gpu_heap_start_cpu.ptr + 1 * gr->gpu_heap_descriptor_size});
    }
}

static void shutdown(GameState* game_state)
{
    assert(game_state);

    finish_gpu_commands(&game_state->gr);

    SAFE_RELEASE(game_state->vertex_buffer);
    for (i32 i = 0; i < ARRAYSIZE(game_state->gpu_pipelines); ++i) {
        SAFE_RELEASE(game_state->gpu_pipelines[i]);
        SAFE_RELEASE(game_state->gpu_root_signatures[i]);
    }

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    shutdown_graphics_context(&game_state->gr);
}

static void update(GameState* game_state)
{
    assert(game_state);

    game_state->is_window_minimized = !handle_window_resize(&game_state->gr);
    if (game_state->is_window_minimized)
        return;

    f64 time;
    f32 delta_time;
    update_frame_stats(game_state->gr.window, WINDOW_NAME, &time, &delta_time);

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::ShowDemoWindow();

    ImGui::Render();
}

static void draw(GameState* game_state)
{
    assert(game_state);

    GraphicsContext* gr = &game_state->gr;

    gr->command_list->SetPipelineState(game_state->gpu_pipelines[0]);
    gr->command_list->SetGraphicsRootSignature(game_state->gpu_root_signatures[0]);
    gr->command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gr->command_list->DrawInstanced(3, 1, 0, 0);
}

i32 main()
{
    GameState game_state = {};
    init(&game_state);

    while (true) {
        MSG msg = {};
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) break;
        } else {
            update(&game_state);
            draw_frame(&game_state);
        }
    }

    shutdown(&game_state);

    return 0;
}
