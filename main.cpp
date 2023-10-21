#include "pch.h"
#include "cpp_hlsl_common.h"

extern "C" {
    __declspec(dllexport) extern const u32 D3D12SDKVersion = 611;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\d3d12\\";
}

#define WITH_D3D12_DEBUG_LAYER 1
#define WITH_D3D12_GPU_BASED_VALIDATION 0

#define GRC_ENABLE_VSYNC 1
#define GRC_MAX_BUFFERED_FRAMES 2
#define GRC_MAX_GPU_DESCRIPTORS (16 * 1024)
#define GRC_NUM_MSAA_SAMPLES 8
#define GRC_CLEAR_COLOR { 0.0f, 0.0f, 0.0f, 0.0f }

#define WINDOW_NAME "game"
#define WINDOW_MIN_WH 400

#define GPU_BUFFER_SIZE_STATIC (8 * 1024 * 1024)
#define GPU_BUFFER_SIZE_DYNAMIC (256 * 1024)

#define MESH_ROUND_RECT_100x100 0
#define MESH_CIRCLE_100 1
#define MESH_RECT_100x100 2
#define MESH_PATH_00 3
#define MESH_COUNT 4

#define NUM_GPU_PIPELINES 1

struct GraphicsContext {
    HWND window;
    i32 window_width;
    i32 window_height;

    IDXGIFactory7* dxgi_factory;
    IDXGIAdapter4* adapter;
    ID3D12Device12* device;

    ID3D12CommandQueue* command_queue;
    ID3D12CommandAllocator* command_allocators[GRC_MAX_BUFFERED_FRAMES];
    ID3D12GraphicsCommandList9* command_list;

#if WITH_D3D12_DEBUG_LAYER
    ID3D12Debug6* debug;
    ID3D12DebugDevice2* debug_device;
    ID3D12DebugCommandQueue1* debug_command_queue;
    ID3D12DebugCommandList3* debug_command_list;
    ID3D12InfoQueue1* debug_info_queue;
#endif

    IDXGISwapChain4* swap_chain;
    UINT swap_chain_flags;
    UINT swap_chain_present_interval;
    ID3D12Resource* swap_chain_buffers[GRC_MAX_BUFFERED_FRAMES];

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

struct StaticMesh {
    u32 first_vertex;
    u32 num_vertices;
};

struct Object {
    u32 mesh_index;
};

struct GameState {
    GraphicsContext gr;

    ID3D12Resource2* gpu_buffer_static;
    ID3D12Resource2* gpu_buffer_dynamic;
    ID3D12Resource2* upload_buffers[GRC_MAX_BUFFERED_FRAMES];
    u8* upload_buffer_bases[GRC_MAX_BUFFERED_FRAMES];

    ID3D12PipelineState* gpu_pipelines[NUM_GPU_PIPELINES];
    ID3D12RootSignature* gpu_root_signatures[NUM_GPU_PIPELINES];
    ID2D1Factory7* d2d_factory;

    bool is_window_minimized;

    std::vector<StaticMesh> meshes;

    std::vector<Object> objects;
    std::vector<CppHlsl_Object> cpp_hlsl_objects;

    struct {
        JPH::TempAllocatorImpl* temp_allocator;
        JPH::JobSystemThreadPool* job_system;
    } phy;
};

#define MAX_DYNAMIC_OBJECTS 1024

struct alignas(16) UploadData {
    CppHlsl_FrameState frame_state;
    CppHlsl_Object objects[MAX_DYNAMIC_OBJECTS];
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
    if ((gr->frame_fence_counter - gpu_frame_counter) >= GRC_MAX_BUFFERED_FRAMES) {
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
        .SampleDesc = { .Count = GRC_NUM_MSAA_SAMPLES },
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
    };
    const D3D12_CLEAR_VALUE clear_value = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        .Color = GRC_CLEAR_COLOR,
    };
    VHR(gr->device->CreateCommittedResource3(&heap_desc, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_RENDER_TARGET, &clear_value, nullptr, 0, nullptr, IID_PPV_ARGS(&gr->msaa_srgb_rt)));

    gr->device->CreateRenderTargetView(gr->msaa_srgb_rt, nullptr, { .ptr = gr->rtv_heap_start.ptr + GRC_MAX_BUFFERED_FRAMES * gr->rtv_heap_descriptor_size });

    LOG("[graphics] MSAAx%d SRGB render target created (%dx%d)", GRC_NUM_MSAA_SAMPLES, gr->window_width, gr->window_height);
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

        for (i32 i = 0; i < GRC_MAX_BUFFERED_FRAMES; ++i) SAFE_RELEASE(gr->swap_chain_buffers[i]);

        VHR(gr->swap_chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, gr->swap_chain_flags));

        for (i32 i = 0; i < GRC_MAX_BUFFERED_FRAMES; ++i) {
            VHR(gr->swap_chain->GetBuffer(i, IID_PPV_ARGS(&gr->swap_chain_buffers[i])));
        }

        for (i32 i = 0; i < GRC_MAX_BUFFERED_FRAMES; ++i) {
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
#if WITH_D3D12_DEBUG_LAYER
    VHR(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&gr->dxgi_factory)));
#else
    VHR(CreateDXGIFactory2(0, IID_PPV_ARGS(&gr->dxgi_factory)));
#endif

    VHR(gr->dxgi_factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&gr->adapter)));

    DXGI_ADAPTER_DESC3 adapter_desc = {};
    VHR(gr->adapter->GetDesc3(&adapter_desc));

    LOG("[graphics] Adapter: %S", adapter_desc.Description);

#if WITH_D3D12_DEBUG_LAYER
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&gr->debug)))) {
        LOG("[graphics] Failed to load D3D12 debug layer. Please rebuild with `WITH_D3D12_DEBUG_LAYER 0` and try again.");
        return false;
    }
    gr->debug->EnableDebugLayer();
    LOG("[graphics] D3D12 Debug Layer enabled");
#if WITH_D3D12_GPU_BASED_VALIDATION
    gr->debug->SetEnableGPUBasedValidation(TRUE);
    LOG("[graphics] D3D12 GPU-Based Validation enabled");
#endif
#endif

    if (FAILED(D3D12CreateDevice(gr->adapter, D3D_FEATURE_LEVEL_11_1, IID_PPV_ARGS(&gr->device)))) return false;

#if WITH_D3D12_DEBUG_LAYER
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

#if WITH_D3D12_DEBUG_LAYER
    VHR(gr->command_queue->QueryInterface(IID_PPV_ARGS(&gr->debug_command_queue)));
#endif

    LOG("[graphics] Command queue created");

    for (i32 i = 0; i < GRC_MAX_BUFFERED_FRAMES; ++i) {
        VHR(gr->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gr->command_allocators[i])));
    }

    LOG("[graphics] Command allocators created");

    VHR(gr->device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&gr->command_list)));

#if WITH_D3D12_DEBUG_LAYER
    VHR(gr->command_list->QueryInterface(IID_PPV_ARGS(&gr->debug_command_list)));
#endif

    LOG("[graphics] Command list created");

    //
    // Swap chain
    //
    /* Swap chain flags */ {
        gr->swap_chain_flags = 0;
        gr->swap_chain_present_interval = GRC_ENABLE_VSYNC;

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
        .BufferCount = GRC_MAX_BUFFERED_FRAMES,
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

    for (i32 i = 0; i < GRC_MAX_BUFFERED_FRAMES; ++i) {
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

    for (i32 i = 0; i < GRC_MAX_BUFFERED_FRAMES; ++i) {
        gr->device->CreateRenderTargetView(gr->swap_chain_buffers[i], nullptr, { .ptr = gr->rtv_heap_start.ptr + i * gr->rtv_heap_descriptor_size });
    }

    LOG("[graphics] Render target view (RTV) descriptor heap created");

    //
    // CBV, SRV, UAV descriptor heap
    //
    const D3D12_DESCRIPTOR_HEAP_DESC gpu_heap_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = GRC_MAX_GPU_DESCRIPTORS,
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
    for (i32 i = 0; i < GRC_MAX_BUFFERED_FRAMES; ++i) SAFE_RELEASE(gr->command_allocators[i]);
    if (gr->frame_fence_event) {
        CloseHandle(gr->frame_fence_event);
        gr->frame_fence_event = nullptr;
    }
    SAFE_RELEASE(gr->frame_fence);
    SAFE_RELEASE(gr->gpu_heap);
    SAFE_RELEASE(gr->rtv_heap);
    for (i32 i = 0; i < GRC_MAX_BUFFERED_FRAMES; ++i) SAFE_RELEASE(gr->swap_chain_buffers[i]);
    SAFE_RELEASE(gr->swap_chain);
    SAFE_RELEASE(gr->command_queue);
    SAFE_RELEASE(gr->device);
    SAFE_RELEASE(gr->adapter);
    SAFE_RELEASE(gr->dxgi_factory);

#if WITH_D3D12_DEBUG_LAYER
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
    (void)num_read_bytes;
    fclose(file);
    assert(size_in_bytes == num_read_bytes);
    return data;
}

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
            .ptr = gr->rtv_heap_start.ptr + GRC_MAX_BUFFERED_FRAMES * gr->rtv_heap_descriptor_size
        };
        const f32 clear_color[] = GRC_CLEAR_COLOR;

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

static void jolt_trace(const char* fmt, ...)
{
    va_list list;
    va_start(list, fmt);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, list);
    va_end(list);

    LOG("[physics] %s", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
static bool jolt_assert_failed(const char* expression, const char* message, const char* file, u32 line)
{
    LOG("[physics] Assert failed (%s): (%s:%d) %s", expression, file, line, message ? message : "");

    // Breakpoint
    return true;
}
#endif

static constexpr auto OBJECT_LAYER_NON_MOVING = JPH::ObjectLayer(0);
static constexpr auto OBJECT_LAYER_MOVING = JPH::ObjectLayer(1);
static constexpr u32 OBJECT_LAYER_COUNT = 2;

static constexpr auto BROAD_PHASE_LAYER_NON_MOVING = JPH::BroadPhaseLayer(0);
static constexpr auto BROAD_PHASE_LAYER_MOVING = JPH::BroadPhaseLayer(1);
static constexpr u32 BROAD_PHASE_LAYER_COUNT = 2;

struct ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
    virtual bool ShouldCollide(JPH::ObjectLayer object1, JPH::ObjectLayer object2) const override {
        switch (object1) {
            case OBJECT_LAYER_NON_MOVING: return object2 == OBJECT_LAYER_MOVING;
            case OBJECT_LAYER_MOVING: return true;
            default: JPH_ASSERT(false); return false;
        }
    }
};

struct BroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface {
    JPH::BroadPhaseLayer object_to_broad_phase[OBJECT_LAYER_COUNT];

    BroadPhaseLayerInterface() {
        object_to_broad_phase[OBJECT_LAYER_NON_MOVING] = BROAD_PHASE_LAYER_NON_MOVING;
        object_to_broad_phase[OBJECT_LAYER_MOVING] = BROAD_PHASE_LAYER_MOVING;
    }

    virtual u32 GetNumBroadPhaseLayers() const override { return BROAD_PHASE_LAYER_COUNT; }

    virtual JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        JPH_ASSERT(layer < OBJECT_LAYER_COUNT);
        return object_to_broad_phase[layer];
    }
};

struct ObjectVsBroadPhaseLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
    virtual bool ShouldCollide(JPH::ObjectLayer layer1, JPH::BroadPhaseLayer layer2) const override {
        switch (layer1) {
            case OBJECT_LAYER_NON_MOVING: return layer2 == BROAD_PHASE_LAYER_MOVING;
            case OBJECT_LAYER_MOVING: return true;
            default: JPH_ASSERT(false); return false;
        }
    }
};

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
    if (!ImGui_ImplDX12_Init(gr->device, GRC_MAX_BUFFERED_FRAMES, DXGI_FORMAT_R8G8B8A8_UNORM, gr->gpu_heap, gr->gpu_heap_start_cpu, gr->gpu_heap_start_gpu)) VHR(E_FAIL);

    ImGui::GetStyle().ScaleAllSizes(dpi_scale);

    {
        const D2D1_FACTORY_OPTIONS options = { .debugLevel = D2D1_DEBUG_LEVEL_INFORMATION };
        VHR(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(game_state->d2d_factory), &options, reinterpret_cast<void**>(&game_state->d2d_factory)));
    }

    JPH::RegisterDefaultAllocator();

    JPH::Trace = jolt_trace;
	JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = jolt_assert_failed;);

    JPH::Factory::sInstance = new JPH::Factory();

    JPH::RegisterTypes();

    game_state->phy.temp_allocator = new JPH::TempAllocatorImpl(10 * 1024 * 1024);
    game_state->phy.job_system = new JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers);

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
                //.FillMode = D3D12_FILL_MODE_WIREFRAME,
                .CullMode = D3D12_CULL_MODE_NONE,
            },
            .PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
            .NumRenderTargets = 1,
            .RTVFormats = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB },
            .SampleDesc = { .Count = GRC_NUM_MSAA_SAMPLES },
        };

        VHR(gr->device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&game_state->gpu_pipelines[0])));
        VHR(gr->device->CreateRootSignature(0, vs.data(), vs.size(), IID_PPV_ARGS(&game_state->gpu_root_signatures[0])));
    }

    // Upload buffers
    for (i32 i = 0; i < GRC_MAX_BUFFERED_FRAMES; ++i) {
        const D3D12_HEAP_PROPERTIES heap_desc = { .Type = D3D12_HEAP_TYPE_UPLOAD };
        const D3D12_RESOURCE_DESC1 desc = {
            .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Width = GPU_BUFFER_SIZE_DYNAMIC,
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .SampleDesc = { .Count = 1 },
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        };
        VHR(gr->device->CreateCommittedResource3(&heap_desc, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&game_state->upload_buffers[i])));

        const D3D12_RANGE range = { .Begin = 0, .End = 0 };
        VHR(game_state->upload_buffers[i]->Map(0, &range, reinterpret_cast<void**>(&game_state->upload_buffer_bases[i])));
    }

    // Static buffer
    {
        const D3D12_HEAP_PROPERTIES heap_desc = { .Type = D3D12_HEAP_TYPE_DEFAULT };
        const D3D12_RESOURCE_DESC1 desc = {
            .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Width = GPU_BUFFER_SIZE_STATIC,
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .SampleDesc = { .Count = 1 },
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        };
        VHR(gr->device->CreateCommittedResource3(&heap_desc, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&game_state->gpu_buffer_static)));
    }

    // Dynamic buffer
    {
        const D3D12_HEAP_PROPERTIES heap_desc = { .Type = D3D12_HEAP_TYPE_DEFAULT };
        const D3D12_RESOURCE_DESC1 desc = {
            .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Width = GPU_BUFFER_SIZE_DYNAMIC,
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .SampleDesc = { .Count = 1 },
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        };
        VHR(gr->device->CreateCommittedResource3(&heap_desc, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&game_state->gpu_buffer_dynamic)));
    }

    // Create static meshes and store them in the upload buffer
    {
        game_state->meshes.resize(MESH_COUNT);

        struct TessellationSink : public ID2D1TessellationSink {
            std::vector<CppHlsl_Vertex> vertices;

            virtual void AddTriangles(const D2D1_TRIANGLE* triangles, u32 num_triangles) override {
                for (u32 i = 0; i < num_triangles; ++i) {
                    const D2D1_TRIANGLE* tri = &triangles[i];
                    vertices.push_back({ tri->point1.x, tri->point1.y });
                    vertices.push_back({ tri->point2.x, tri->point2.y });
                    vertices.push_back({ tri->point3.x, tri->point3.y });
                }
            }

            virtual HRESULT QueryInterface(const IID&, void**) override { return S_OK; }
            virtual ULONG AddRef() override { return 0; }
            virtual ULONG Release() override { return 0; }
            virtual HRESULT Close() override { return S_OK; }
        } tess_sink;

        {
            const D2D1_ROUNDED_RECT shape = {
                .rect = { -50.0f, -50.0f, 50.0f, 50.0f }, .radiusX = 25.0f, .radiusY = 25.0f,
            };
            ID2D1RoundedRectangleGeometry* geo = nullptr;
            VHR(game_state->d2d_factory->CreateRoundedRectangleGeometry(&shape, &geo));
            defer { SAFE_RELEASE(geo); };

            const u32 first_vertex = static_cast<u32>(tess_sink.vertices.size());
            VHR(geo->Tessellate(nullptr, D2D1_DEFAULT_FLATTENING_TOLERANCE, &tess_sink));
            const u32 num_vertices = static_cast<u32>(tess_sink.vertices.size()) - first_vertex;

            game_state->meshes[MESH_ROUND_RECT_100x100] = { first_vertex, num_vertices };
        }
        {
            const D2D1_ELLIPSE shape = {
                .point = { 0.0f, 0.0f }, .radiusX = 100.0f, .radiusY = 100.0f,
            };
            ID2D1EllipseGeometry* geo = nullptr;
            VHR(game_state->d2d_factory->CreateEllipseGeometry(&shape, &geo));
            defer { SAFE_RELEASE(geo); };

            const u32 first_vertex = static_cast<u32>(tess_sink.vertices.size());
            VHR(geo->Tessellate(nullptr, D2D1_DEFAULT_FLATTENING_TOLERANCE, &tess_sink));
            const u32 num_vertices = static_cast<u32>(tess_sink.vertices.size()) - first_vertex;

            game_state->meshes[MESH_CIRCLE_100] = { first_vertex, num_vertices };
        }
        {
            const D2D1_RECT_F shape = { -50.0f, -50.0f, 50.0f, 50.0f };
            ID2D1RectangleGeometry* geo = nullptr;
            VHR(game_state->d2d_factory->CreateRectangleGeometry(&shape, &geo));
            defer { SAFE_RELEASE(geo); };

            const u32 first_vertex = static_cast<u32>(tess_sink.vertices.size());
            VHR(geo->Tessellate(nullptr, D2D1_DEFAULT_FLATTENING_TOLERANCE, &tess_sink));
            const u32 num_vertices = static_cast<u32>(tess_sink.vertices.size()) - first_vertex;

            game_state->meshes[MESH_RECT_100x100] = { first_vertex, num_vertices };
        }
        {
            ID2D1PathGeometry* geo = nullptr;
            VHR(game_state->d2d_factory->CreatePathGeometry(&geo));
            defer { SAFE_RELEASE(geo); };

            ID2D1GeometrySink* sink = nullptr;
            VHR(geo->Open(&sink));
            defer { SAFE_RELEASE(sink); };

            sink->BeginFigure({ 0.0f, 200.0f }, D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine({ 200.0f, 0.0f });
            sink->AddLine({ 0.0f, -200.0f });
            sink->AddLine({ -200.0f, 0.0f });
            sink->EndFigure(D2D1_FIGURE_END_OPEN);

            sink->BeginFigure({ -400.0f, 0.0f }, D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddBezier({ { -400.0f, -400.0f }, { -100.0f, -300.0f }, { 0.0f, -400.0f } });
            sink->AddBezier({ { 100.0f, -300.0f }, { 400.0f, -400.0f }, { 400.0f, 0.0f } });
            sink->EndFigure(D2D1_FIGURE_END_OPEN);

            VHR(sink->Close());

            ID2D1PathGeometry* geo1 = nullptr;
            VHR(game_state->d2d_factory->CreatePathGeometry(&geo1));
            defer { SAFE_RELEASE(geo1); };

            ID2D1GeometrySink* sink1 = nullptr;
            VHR(geo1->Open(&sink1));
            defer { SAFE_RELEASE(sink1); };
            VHR(geo->Widen(50.0f, nullptr, nullptr, D2D1_DEFAULT_FLATTENING_TOLERANCE, sink1));
            VHR(sink1->Close());

            const u32 first_vertex = static_cast<u32>(tess_sink.vertices.size());
            VHR(geo1->Tessellate(nullptr, D2D1_DEFAULT_FLATTENING_TOLERANCE, &tess_sink));
            const u32 num_vertices = static_cast<u32>(tess_sink.vertices.size()) - first_vertex;

            game_state->meshes[MESH_PATH_00] = { first_vertex, num_vertices };
        }

        auto* ptr = reinterpret_cast<CppHlsl_Vertex*>(game_state->upload_buffer_bases[0]);
        memcpy(ptr, tess_sink.vertices.data(), sizeof(CppHlsl_Vertex) * tess_sink.vertices.size());

        {
            const D3D12_SHADER_RESOURCE_VIEW_DESC desc = {
                .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
                .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                .Buffer = {
                    .FirstElement = 0,
                    .NumElements = static_cast<u32>(tess_sink.vertices.size()),
                    .StructureByteStride = sizeof(CppHlsl_Vertex),
                },
            };
            gr->device->CreateShaderResourceView(game_state->gpu_buffer_static, &desc, { .ptr = gr->gpu_heap_start_cpu.ptr + RDH_VERTEX_BUFFER_STATIC * gr->gpu_heap_descriptor_size });
        }
    }

    // Copy upload buffer to the static buffer
    {
        VHR(gr->command_allocators[0]->Reset());
        VHR(gr->command_list->Reset(gr->command_allocators[0], nullptr));

        {
            const D3D12_BUFFER_BARRIER buffer_barriers[] = {
                {
                    .SyncBefore = D3D12_BARRIER_SYNC_NONE,
                    .SyncAfter = D3D12_BARRIER_SYNC_COPY,
                    .AccessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS,
                    .AccessAfter = D3D12_BARRIER_ACCESS_COPY_SOURCE,
                    .pResource = game_state->upload_buffers[0],
                    .Size = UINT64_MAX,
                }, {
                    .SyncBefore = D3D12_BARRIER_SYNC_NONE,
                    .SyncAfter = D3D12_BARRIER_SYNC_COPY,
                    .AccessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS,
                    .AccessAfter = D3D12_BARRIER_ACCESS_COPY_DEST,
                    .pResource = game_state->gpu_buffer_static,
                    .Size = UINT64_MAX,
                },
            };
            const D3D12_BARRIER_GROUP barrier_group = {
                .Type = D3D12_BARRIER_TYPE_BUFFER,
                .NumBarriers = ARRAYSIZE(buffer_barriers),
                .pBufferBarriers = buffer_barriers,
            };
            gr->command_list->Barrier(1, &barrier_group);
        }

        gr->command_list->CopyBufferRegion(game_state->gpu_buffer_static, 0, game_state->upload_buffers[0], 0, GPU_BUFFER_SIZE_DYNAMIC);

        VHR(gr->command_list->Close());

        gr->command_queue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList**>(&gr->command_list));

        finish_gpu_commands(gr);
    }

    game_state->objects.push_back({ .mesh_index = MESH_ROUND_RECT_100x100 });
    game_state->cpp_hlsl_objects.push_back({ .x = -400.0f, .y = 400.0f });

    game_state->objects.push_back({ .mesh_index = MESH_RECT_100x100 });
    game_state->cpp_hlsl_objects.push_back({ .x = 600.0f, .y = -200.0f });

    game_state->objects.push_back({ .mesh_index = MESH_CIRCLE_100 });
    game_state->cpp_hlsl_objects.push_back({ .x = 0.0f, .y = 0.0f });

    game_state->objects.push_back({ .mesh_index = MESH_PATH_00 });
    game_state->cpp_hlsl_objects.push_back({ .x = 0.0f, .y = 0.0f });

    {
        const D3D12_SHADER_RESOURCE_VIEW_DESC desc = {
            .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Buffer = {
                .FirstElement = 0,
                .NumElements = 1,
                .StructureByteStride = sizeof(CppHlsl_FrameState),
            },
        };
        gr->device->CreateShaderResourceView(game_state->gpu_buffer_dynamic, &desc, { .ptr = gr->gpu_heap_start_cpu.ptr + RDH_FRAME_STATE * gr->gpu_heap_descriptor_size });
    }
    {
        const D3D12_SHADER_RESOURCE_VIEW_DESC desc = {
            .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Buffer = {
                .FirstElement = sizeof(CppHlsl_FrameState) / sizeof(CppHlsl_Object),
                .NumElements = static_cast<u32>(game_state->cpp_hlsl_objects.size()),
                .StructureByteStride = sizeof(CppHlsl_Object),
            },
        };
        gr->device->CreateShaderResourceView(game_state->gpu_buffer_dynamic, &desc, { .ptr = gr->gpu_heap_start_cpu.ptr + RDH_OBJECTS_DYNAMIC * gr->gpu_heap_descriptor_size });
    }
}

static void shutdown(GameState* game_state)
{
    assert(game_state);

    finish_gpu_commands(&game_state->gr);

    SAFE_RELEASE(game_state->d2d_factory);
    SAFE_RELEASE(game_state->gpu_buffer_static);
    SAFE_RELEASE(game_state->gpu_buffer_dynamic);
    for (i32 i = 0; i < ARRAYSIZE(game_state->upload_buffers); ++i) {
        SAFE_RELEASE(game_state->upload_buffers[i]);
    }
    for (i32 i = 0; i < ARRAYSIZE(game_state->gpu_pipelines); ++i) {
        SAFE_RELEASE(game_state->gpu_pipelines[i]);
        SAFE_RELEASE(game_state->gpu_root_signatures[i]);
    }

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (game_state->phy.job_system) {
        delete game_state->phy.job_system;
        game_state->phy.job_system = nullptr;
    }
    if (game_state->phy.temp_allocator) {
        delete game_state->phy.temp_allocator;
        game_state->phy.temp_allocator = nullptr;
    }

    JPH::UnregisterTypes();

    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;

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

    {
        const f32 r = 500.0f;
        XMMATRIX xform;
        if (gr->window_width >= gr->window_height) {
            const float aspect = static_cast<f32>(gr->window_width) / gr->window_height;
            xform = XMMatrixOrthographicOffCenterLH(-r * aspect, r * aspect, -r, r, -1.0f, 1.0f);
        } else {
            const float aspect = static_cast<f32>(gr->window_height) / gr->window_width;
            xform = XMMatrixOrthographicOffCenterLH(-r, r, -r * aspect, r * aspect, -1.0f, 1.0f);
        }

        auto* ptr = reinterpret_cast<UploadData*>(game_state->upload_buffer_bases[gr->frame_index]);
        memset(ptr, 0, sizeof(UploadData));

        XMStoreFloat4x4(&ptr->frame_state.proj, XMMatrixTranspose(xform));

        memcpy(ptr->objects, game_state->cpp_hlsl_objects.data(), game_state->cpp_hlsl_objects.size() * sizeof(CppHlsl_Object));
    }

    {
        const D3D12_BUFFER_BARRIER buffer_barriers[] = {
            {
                .SyncBefore = D3D12_BARRIER_SYNC_NONE,
                .SyncAfter = D3D12_BARRIER_SYNC_COPY,
                .AccessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS,
                .AccessAfter = D3D12_BARRIER_ACCESS_COPY_SOURCE,
                .pResource = game_state->upload_buffers[gr->frame_index],
                .Size = UINT64_MAX,
            }, {
                .SyncBefore = D3D12_BARRIER_SYNC_NONE,
                .SyncAfter = D3D12_BARRIER_SYNC_COPY,
                .AccessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS,
                .AccessAfter = D3D12_BARRIER_ACCESS_COPY_DEST,
                .pResource = game_state->gpu_buffer_dynamic,
                .Size = UINT64_MAX,
            },
        };
        const D3D12_BARRIER_GROUP barrier_group = {
            .Type = D3D12_BARRIER_TYPE_BUFFER,
            .NumBarriers = ARRAYSIZE(buffer_barriers),
            .pBufferBarriers = buffer_barriers,
        };
        gr->command_list->Barrier(1, &barrier_group);
    }

    gr->command_list->CopyBufferRegion(game_state->gpu_buffer_dynamic, 0, game_state->upload_buffers[gr->frame_index], 0, sizeof(UploadData));

    {
        const D3D12_BUFFER_BARRIER buffer_barriers[] = {
            {
                .SyncBefore = D3D12_BARRIER_SYNC_NONE,
                .SyncAfter = D3D12_BARRIER_SYNC_DRAW,
                .AccessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS,
                .AccessAfter = D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                .pResource = game_state->gpu_buffer_static,
                .Size = UINT64_MAX,
            }, {
                .SyncBefore = D3D12_BARRIER_SYNC_COPY,
                .SyncAfter = D3D12_BARRIER_SYNC_DRAW,
                .AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST,
                .AccessAfter = D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                .pResource = game_state->gpu_buffer_dynamic,
                .Size = UINT64_MAX,
            },
        };
        const D3D12_BARRIER_GROUP barrier_group = {
            .Type = D3D12_BARRIER_TYPE_BUFFER,
            .NumBarriers = ARRAYSIZE(buffer_barriers),
            .pBufferBarriers = buffer_barriers,
        };
        gr->command_list->Barrier(1, &barrier_group);
    }

    gr->command_list->SetPipelineState(game_state->gpu_pipelines[0]);
    gr->command_list->SetGraphicsRootSignature(game_state->gpu_root_signatures[0]);
    gr->command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (usize i = 0; i < game_state->objects.size(); ++i) {
        const Object* obj = &game_state->objects[i];
        const u32 mesh_index = obj->mesh_index;

        const u32 root_consts[2] = {
            game_state->meshes[mesh_index].first_vertex,
            static_cast<u32>(i), // object index
        };

        gr->command_list->SetGraphicsRoot32BitConstants(0, ARRAYSIZE(root_consts), &root_consts, 0);
        gr->command_list->DrawInstanced(game_state->meshes[mesh_index].num_vertices, 1, 0, 0);
    }
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
