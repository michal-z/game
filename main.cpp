#include "pch.h"
#include "cpp_hlsl_common.h"

extern "C" {
    __declspec(dllexport) extern const u32 D3D12SDKVersion = 611;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\d3d12\\";
}

#define WITH_D3D12_DEBUG_LAYER 1
#define WITH_D3D12_GPU_BASED_VALIDATION 0
//--------------------------------------------------------------------------------------------------
struct GpuContext
{
    static constexpr auto ENABLE_VSYNC = true;
    static constexpr auto MAX_BUFFERED_FRAMES = 2;
    static constexpr auto MAX_GPU_DESCRIPTORS = 16 * 1024;
    static constexpr auto NUM_MSAA_SAMPLES = 8;
    static constexpr auto CLEAR_COLOR = XMVECTORF32{ 0.0f, 0.0f, 0.0f, 0.0f };

    HWND window;
    i32 window_width;
    i32 window_height;

    IDXGIFactory7* dxgi_factory;
    IDXGIAdapter4* adapter;
    ID3D12Device13* device;

    ID3D12CommandQueue* command_queue;
    ID3D12CommandAllocator* command_allocators[MAX_BUFFERED_FRAMES];
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
    ID3D12Resource* swap_chain_buffers[MAX_BUFFERED_FRAMES];

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
//--------------------------------------------------------------------------------------------------
struct StaticMesh
{
    static constexpr auto ROUND_RECT_100x100 = 0;
    static constexpr auto CIRCLE_100 = 1;
    static constexpr auto RECT_100x100 = 2;
    static constexpr auto PATH_00 = 3;
    static constexpr auto NUM = 4;

    u32 first_vertex;
    u32 num_vertices;
};
//--------------------------------------------------------------------------------------------------
struct Object
{
    u32 mesh_index;
};
//--------------------------------------------------------------------------------------------------
struct GameState
{
    static constexpr auto WINDOW_NAME = "game";
    static constexpr auto WINDOW_WIDTH = 1200;
    static constexpr auto WINDOW_HEIGHT = 800;
    static constexpr auto WINDOW_MIN_WH = 400;
    static constexpr auto NUM_GPU_PIPELINES = 1;
    static constexpr auto GPU_BUFFER_SIZE_STATIC = 8 * 1024 * 1024;
    static constexpr auto GPU_BUFFER_SIZE_DYNAMIC = 256 * 1024;

    struct {
        GpuContext* gc;
        ID2D1Factory7* d2d_factory;
        ID3D12Resource2* buffer_static;
        ID3D12Resource2* buffer_dynamic;
        ID3D12Resource2* upload_buffers[GpuContext::MAX_BUFFERED_FRAMES];
        u8* upload_buffer_bases[GpuContext::MAX_BUFFERED_FRAMES];
        ID3D12PipelineState* pipelines[NUM_GPU_PIPELINES];
        ID3D12RootSignature* root_signatures[NUM_GPU_PIPELINES];
    } gpu;

    bool is_window_minimized;

    std::vector<StaticMesh> meshes;

    std::vector<Object> objects;
    std::vector<CppHlsl_Object> cpp_hlsl_objects;

    struct {
        JPH::TempAllocatorImpl* temp_allocator;
        JPH::JobSystemThreadPool* job_system;
    } phy;
};
//--------------------------------------------------------------------------------------------------
struct alignas(16) UploadData
{
    static constexpr auto MAX_DYNAMIC_OBJECTS = 1024;

    CppHlsl_FrameState frame_state;
    CppHlsl_Object objects[MAX_DYNAMIC_OBJECTS];
};
//--------------------------------------------------------------------------------------------------
struct ObjectLayers
{
    static constexpr auto NON_MOVING = JPH::ObjectLayer(0);
    static constexpr auto MOVING = JPH::ObjectLayer(1);
    static constexpr auto NUM = 2;
};
//--------------------------------------------------------------------------------------------------
struct BroadPhaseLayers
{
    static constexpr auto NON_MOVING = JPH::BroadPhaseLayer(0);
    static constexpr auto MOVING = JPH::BroadPhaseLayer(1);
    static constexpr auto NUM = 2;
};
//--------------------------------------------------------------------------------------------------
struct ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter
{
    virtual bool ShouldCollide(JPH::ObjectLayer object1, JPH::ObjectLayer object2) const override {
        switch (object1) {
            case ObjectLayers::NON_MOVING: return object2 == ObjectLayers::MOVING;
            case ObjectLayers::MOVING: return true;
            default: JPH_ASSERT(false); return false;
        }
    }
};
//--------------------------------------------------------------------------------------------------
struct BroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface
{
    JPH::BroadPhaseLayer object_to_broad_phase[ObjectLayers::NUM];

    BroadPhaseLayerInterface() {
        object_to_broad_phase[ObjectLayers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        object_to_broad_phase[ObjectLayers::MOVING] = BroadPhaseLayers::MOVING;
    }

    virtual u32 GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM; }

    virtual JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        JPH_ASSERT(layer < ObjectLayers::NUM);
        return object_to_broad_phase[layer];
    }
};
//--------------------------------------------------------------------------------------------------
struct ObjectVsBroadPhaseLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
{
    virtual bool ShouldCollide(JPH::ObjectLayer layer1, JPH::BroadPhaseLayer layer2) const override {
        switch (layer1) {
            case ObjectLayers::NON_MOVING: return layer2 == BroadPhaseLayers::MOVING;
            case ObjectLayers::MOVING: return true;
            default: JPH_ASSERT(false); return false;
        }
    }
};
//--------------------------------------------------------------------------------------------------
static void present_gpu_frame(GpuContext* gc)
{
    assert(gc && gc->device);
    gc->frame_fence_counter += 1;

    UINT present_flags = 0;

    if (gc->swap_chain_present_interval == 0 && gc->swap_chain_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)
        present_flags |= DXGI_PRESENT_ALLOW_TEARING;

    VHR(gc->swap_chain->Present(gc->swap_chain_present_interval, present_flags));
    VHR(gc->command_queue->Signal(gc->frame_fence, gc->frame_fence_counter));

    const u64 gpu_frame_counter = gc->frame_fence->GetCompletedValue();
    if ((gc->frame_fence_counter - gpu_frame_counter) >= GpuContext::MAX_BUFFERED_FRAMES) {
        VHR(gc->frame_fence->SetEventOnCompletion(gpu_frame_counter + 1, gc->frame_fence_event));
        WaitForSingleObject(gc->frame_fence_event, INFINITE);
    }

    gc->frame_index = gc->swap_chain->GetCurrentBackBufferIndex();
}
//--------------------------------------------------------------------------------------------------
static void finish_gpu_commands(GpuContext* gc)
{
    assert(gc && gc->device);
    gc->frame_fence_counter += 1;

    VHR(gc->command_queue->Signal(gc->frame_fence, gc->frame_fence_counter));
    VHR(gc->frame_fence->SetEventOnCompletion(gc->frame_fence_counter, gc->frame_fence_event));

    WaitForSingleObject(gc->frame_fence_event, INFINITE);
}
//--------------------------------------------------------------------------------------------------
static void create_msaa_srgb_render_target(GpuContext* gc)
{
    assert(gc && gc->device);

    const D3D12_HEAP_PROPERTIES heap_desc = { .Type = D3D12_HEAP_TYPE_DEFAULT };
    const D3D12_RESOURCE_DESC1 desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = static_cast<u64>(gc->window_width),
        .Height = static_cast<u32>(gc->window_height),
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        .SampleDesc = { .Count = GpuContext::NUM_MSAA_SAMPLES },
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
    };
    const D3D12_CLEAR_VALUE clear_value = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        .Color = { GpuContext::CLEAR_COLOR[0], GpuContext::CLEAR_COLOR[1], GpuContext::CLEAR_COLOR[2], GpuContext::CLEAR_COLOR[3] },
    };
    VHR(gc->device->CreateCommittedResource3(&heap_desc, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_RENDER_TARGET, &clear_value, nullptr, 0, nullptr, IID_PPV_ARGS(&gc->msaa_srgb_rt)));

    gc->device->CreateRenderTargetView(gc->msaa_srgb_rt, nullptr, { .ptr = gc->rtv_heap_start.ptr + GpuContext::MAX_BUFFERED_FRAMES * gc->rtv_heap_descriptor_size });

    LOG("[graphics] MSAAx%d SRGB render target created (%dx%d)", GpuContext::NUM_MSAA_SAMPLES, gc->window_width, gc->window_height);
}
//--------------------------------------------------------------------------------------------------
static bool handle_window_resize(GpuContext* gc)
{
    assert(gc && gc->device);

    RECT current_rect = {};
    GetClientRect(gc->window, &current_rect);

    if (current_rect.right == 0 && current_rect.bottom == 0) {
        // Window is minimized
        Sleep(10);
        return false; // Do not render.
    }

    if (current_rect.right != gc->window_width || current_rect.bottom != gc->window_height) {
        LOG("[graphics] Window resized to %dx%d", current_rect.right, current_rect.bottom);

        finish_gpu_commands(gc);

        for (i32 i = 0; i < GpuContext::MAX_BUFFERED_FRAMES; ++i) SAFE_RELEASE(gc->swap_chain_buffers[i]);

        VHR(gc->swap_chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, gc->swap_chain_flags));

        for (i32 i = 0; i < GpuContext::MAX_BUFFERED_FRAMES; ++i) {
            VHR(gc->swap_chain->GetBuffer(i, IID_PPV_ARGS(&gc->swap_chain_buffers[i])));
        }

        for (i32 i = 0; i < GpuContext::MAX_BUFFERED_FRAMES; ++i) {
            gc->device->CreateRenderTargetView(gc->swap_chain_buffers[i], nullptr, { .ptr = gc->rtv_heap_start.ptr + i * gc->rtv_heap_descriptor_size });
        }

        gc->window_width = current_rect.right;
        gc->window_height = current_rect.bottom;
        gc->frame_index = gc->swap_chain->GetCurrentBackBufferIndex();

        SAFE_RELEASE(gc->msaa_srgb_rt);
        create_msaa_srgb_render_target(gc);
    }

    return true; // Render normally.
}
//--------------------------------------------------------------------------------------------------
static bool init_gpu_context(HWND window, GpuContext* gc)
{
    assert(gc && gc->device == nullptr);

    RECT rect = {};
    GetClientRect(window, &rect);

    gc->window = window;
    gc->window_width = rect.right;
    gc->window_height = rect.bottom;

    //
    // Factory, adapater, device
    //
#if WITH_D3D12_DEBUG_LAYER
    VHR(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&gc->dxgi_factory)));
#else
    VHR(CreateDXGIFactory2(0, IID_PPV_ARGS(&gc->dxgi_factory)));
#endif

    VHR(gc->dxgi_factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&gc->adapter)));

    DXGI_ADAPTER_DESC3 adapter_desc = {};
    VHR(gc->adapter->GetDesc3(&adapter_desc));

    LOG("[graphics] Adapter: %S", adapter_desc.Description);

#if WITH_D3D12_DEBUG_LAYER
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&gc->debug)))) {
        LOG("[graphics] Failed to load D3D12 debug layer. Please rebuild with `WITH_D3D12_DEBUG_LAYER 0` and try again.");
        return false;
    }
    gc->debug->EnableDebugLayer();
    LOG("[graphics] D3D12 Debug Layer enabled");
#if WITH_D3D12_GPU_BASED_VALIDATION
    gc->debug->SetEnableGPUBasedValidation(TRUE);
    LOG("[graphics] D3D12 GPU-Based Validation enabled");
#endif
#endif

    if (FAILED(D3D12CreateDevice(gc->adapter, D3D_FEATURE_LEVEL_11_1, IID_PPV_ARGS(&gc->device)))) return false;

#if WITH_D3D12_DEBUG_LAYER
    VHR(gc->device->QueryInterface(IID_PPV_ARGS(&gc->debug_device)));
    VHR(gc->device->QueryInterface(IID_PPV_ARGS(&gc->debug_info_queue)));
    VHR(gc->debug_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE));
#endif

    LOG("[graphics] D3D12 device created");

    //
    // Check required features support
    //
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12 = {};
        D3D12_FEATURE_DATA_SHADER_MODEL shader_model = { .HighestShaderModel = D3D_HIGHEST_SHADER_MODEL };

        VHR(gc->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
        VHR(gc->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12)));
        VHR(gc->device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader_model, sizeof(shader_model)));

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
    VHR(gc->device->CreateCommandQueue(&command_queue_desc, IID_PPV_ARGS(&gc->command_queue)));

#if WITH_D3D12_DEBUG_LAYER
    VHR(gc->command_queue->QueryInterface(IID_PPV_ARGS(&gc->debug_command_queue)));
#endif

    LOG("[graphics] Command queue created");

    for (i32 i = 0; i < GpuContext::MAX_BUFFERED_FRAMES; ++i) {
        VHR(gc->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gc->command_allocators[i])));
    }

    LOG("[graphics] Command allocators created");

    VHR(gc->device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&gc->command_list)));

#if WITH_D3D12_DEBUG_LAYER
    VHR(gc->command_list->QueryInterface(IID_PPV_ARGS(&gc->debug_command_list)));
#endif

    LOG("[graphics] Command list created");

    //
    // Swap chain
    //
    /* Swap chain flags */ {
        gc->swap_chain_flags = 0;
        gc->swap_chain_present_interval = GpuContext::ENABLE_VSYNC;

        BOOL allow_tearing = FALSE;
        const HRESULT hr = gc->dxgi_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing));

        if (SUCCEEDED(hr) && allow_tearing == TRUE) {
            gc->swap_chain_flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        }
    }

    const DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {
        .Width = static_cast<u32>(gc->window_width),
        .Height = static_cast<u32>(gc->window_height),
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .Stereo = FALSE,
        .SampleDesc = { .Count = 1 },
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = GpuContext::MAX_BUFFERED_FRAMES,
        .Scaling = DXGI_SCALING_NONE,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED,
        .Flags = gc->swap_chain_flags,
    };
    IDXGISwapChain1* swap_chain1 = nullptr;
    VHR(gc->dxgi_factory->CreateSwapChainForHwnd(gc->command_queue, window, &swap_chain_desc, nullptr, nullptr, &swap_chain1));
    defer { SAFE_RELEASE(swap_chain1); };

    VHR(swap_chain1->QueryInterface(IID_PPV_ARGS(&gc->swap_chain)));

    VHR(gc->dxgi_factory->MakeWindowAssociation(window, DXGI_MWA_NO_WINDOW_CHANGES));

    for (i32 i = 0; i < GpuContext::MAX_BUFFERED_FRAMES; ++i) {
        VHR(gc->swap_chain->GetBuffer(i, IID_PPV_ARGS(&gc->swap_chain_buffers[i])));
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
    VHR(gc->device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&gc->rtv_heap)));
    gc->rtv_heap_start = gc->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    gc->rtv_heap_descriptor_size = gc->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (i32 i = 0; i < GpuContext::MAX_BUFFERED_FRAMES; ++i) {
        gc->device->CreateRenderTargetView(gc->swap_chain_buffers[i], nullptr, { .ptr = gc->rtv_heap_start.ptr + i * gc->rtv_heap_descriptor_size });
    }

    LOG("[graphics] Render target view (RTV) descriptor heap created");

    //
    // CBV, SRV, UAV descriptor heap
    //
    const D3D12_DESCRIPTOR_HEAP_DESC gpu_heap_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = GpuContext::MAX_GPU_DESCRIPTORS,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
        .NodeMask = 0,
    };
    VHR(gc->device->CreateDescriptorHeap(&gpu_heap_desc, IID_PPV_ARGS(&gc->gpu_heap)));
    gc->gpu_heap_start_cpu = gc->gpu_heap->GetCPUDescriptorHandleForHeapStart();
    gc->gpu_heap_start_gpu = gc->gpu_heap->GetGPUDescriptorHandleForHeapStart();
    gc->gpu_heap_descriptor_size = gc->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    LOG("[graphics] GPU descriptor heap (CBV_SRV_UAV, SHADER_VISIBLE) created");

    //
    // Frame fence
    //
    VHR(gc->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gc->frame_fence)));

    gc->frame_fence_event = CreateEventEx(nullptr, "frame_fence_event", 0, EVENT_ALL_ACCESS);
    if (gc->frame_fence_event == nullptr) VHR(HRESULT_FROM_WIN32(GetLastError()));

    gc->frame_fence_counter = 0;
    gc->frame_index = gc->swap_chain->GetCurrentBackBufferIndex();

    LOG("[graphics] Frame fence created");

    //
    // MSAA, SRGB render target
    //
    create_msaa_srgb_render_target(gc);

    return true;
}
//--------------------------------------------------------------------------------------------------
static void shutdown_gpu_context(GpuContext* gc)
{
    assert(gc);

    SAFE_RELEASE(gc->msaa_srgb_rt);
    SAFE_RELEASE(gc->command_list);
    for (i32 i = 0; i < GpuContext::MAX_BUFFERED_FRAMES; ++i) SAFE_RELEASE(gc->command_allocators[i]);
    if (gc->frame_fence_event) {
        CloseHandle(gc->frame_fence_event);
        gc->frame_fence_event = nullptr;
    }
    SAFE_RELEASE(gc->frame_fence);
    SAFE_RELEASE(gc->gpu_heap);
    SAFE_RELEASE(gc->rtv_heap);
    for (i32 i = 0; i < GpuContext::MAX_BUFFERED_FRAMES; ++i) SAFE_RELEASE(gc->swap_chain_buffers[i]);
    SAFE_RELEASE(gc->swap_chain);
    SAFE_RELEASE(gc->command_queue);
    SAFE_RELEASE(gc->device);
    SAFE_RELEASE(gc->adapter);
    SAFE_RELEASE(gc->dxgi_factory);

#if WITH_D3D12_DEBUG_LAYER
    SAFE_RELEASE(gc->debug_command_list);
    SAFE_RELEASE(gc->debug_command_queue);
    SAFE_RELEASE(gc->debug_info_queue);
    SAFE_RELEASE(gc->debug);

    if (gc->debug_device) {
        VHR(gc->debug_device->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL));

        const auto refcount = gc->debug_device->Release();
        assert(refcount == 0);
        (void)refcount;

        gc->debug_device = nullptr;
    }
#endif
}
//--------------------------------------------------------------------------------------------------
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
            info->ptMinTrackSize.x = GameState::WINDOW_MIN_WH;
            info->ptMinTrackSize.y = GameState::WINDOW_MIN_WH;
            return 0;
        } break;
    }
    return DefWindowProc(window, message, wparam, lparam);
}
//--------------------------------------------------------------------------------------------------
static HWND create_window(const char* name, i32 width, i32 height)
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
//--------------------------------------------------------------------------------------------------
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
//--------------------------------------------------------------------------------------------------
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
//--------------------------------------------------------------------------------------------------
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
//--------------------------------------------------------------------------------------------------
static void draw(GameState* game_state);

static void draw_frame(GameState* game_state)
{
    assert(game_state);

    if (game_state->is_window_minimized)
        return;

    GpuContext* gc = game_state->gpu.gc;

    ID3D12CommandAllocator* command_allocator = gc->command_allocators[gc->frame_index];
    VHR(command_allocator->Reset());
    VHR(gc->command_list->Reset(command_allocator, nullptr));

    gc->command_list->SetDescriptorHeaps(1, &gc->gpu_heap);

    /* Viewport */ {
        const D3D12_VIEWPORT viewport = {
            .TopLeftX = 0.0f,
            .TopLeftY = 0.0f,
            .Width = static_cast<f32>(gc->window_width),
            .Height = static_cast<f32>(gc->window_height),
            .MinDepth = 0.0f,
            .MaxDepth = 1.0f,
        };
        gc->command_list->RSSetViewports(1, &viewport);
    }
    /* Scissor */ {
        const D3D12_RECT scissor_rect = {
            .left = 0,
            .top = 0,
            .right = gc->window_width,
            .bottom = gc->window_height,
        };
        gc->command_list->RSSetScissorRects(1, &scissor_rect);
    }

    {
        const D3D12_CPU_DESCRIPTOR_HANDLE rt_descriptor = {
            .ptr = gc->rtv_heap_start.ptr + GpuContext::MAX_BUFFERED_FRAMES * gc->rtv_heap_descriptor_size
        };

        gc->command_list->OMSetRenderTargets(1, &rt_descriptor, TRUE, nullptr);
        gc->command_list->ClearRenderTargetView(rt_descriptor, GpuContext::CLEAR_COLOR, 0, nullptr);
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
                .pResource = gc->msaa_srgb_rt,
                .Subresources = { .IndexOrFirstMipLevel = 0xffffffff },
            }, {
                .SyncBefore = D3D12_BARRIER_SYNC_NONE,
                .SyncAfter = D3D12_BARRIER_SYNC_RESOLVE,
                .AccessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS,
                .AccessAfter = D3D12_BARRIER_ACCESS_RESOLVE_DEST,
                .LayoutBefore = D3D12_BARRIER_LAYOUT_PRESENT,
                .LayoutAfter = D3D12_BARRIER_LAYOUT_RESOLVE_DEST,
                .pResource = gc->swap_chain_buffers[gc->frame_index],
                .Subresources = { .IndexOrFirstMipLevel = 0xffffffff },
            },
        };
        const D3D12_BARRIER_GROUP barrier_group = {
            .Type = D3D12_BARRIER_TYPE_TEXTURE,
            .NumBarriers = ARRAYSIZE(texture_barriers),
            .pTextureBarriers = texture_barriers,
        };
        gc->command_list->Barrier(1, &barrier_group);
    }

    gc->command_list->ResolveSubresource(gc->swap_chain_buffers[gc->frame_index], 0, gc->msaa_srgb_rt, 0, DXGI_FORMAT_R8G8B8A8_UNORM);

    {
        const D3D12_TEXTURE_BARRIER texture_barrier = {
            .SyncBefore = D3D12_BARRIER_SYNC_RESOLVE,
            .SyncAfter = D3D12_BARRIER_SYNC_RENDER_TARGET,
            .AccessBefore = D3D12_BARRIER_ACCESS_RESOLVE_DEST,
            .AccessAfter = D3D12_BARRIER_ACCESS_RENDER_TARGET,
            .LayoutBefore = D3D12_BARRIER_LAYOUT_RESOLVE_DEST,
            .LayoutAfter = D3D12_BARRIER_LAYOUT_RENDER_TARGET,
            .pResource = gc->swap_chain_buffers[gc->frame_index],
            .Subresources = { .IndexOrFirstMipLevel = 0xffffffff },
        };
        const D3D12_BARRIER_GROUP barrier_group = {
            .Type = D3D12_BARRIER_TYPE_TEXTURE,
            .NumBarriers = 1,
            .pTextureBarriers = &texture_barrier,
        };
        gc->command_list->Barrier(1, &barrier_group);
    }

    {
        const D3D12_CPU_DESCRIPTOR_HANDLE back_buffer_descriptor = {
            .ptr = gc->rtv_heap_start.ptr + gc->frame_index * gc->rtv_heap_descriptor_size
        };
        gc->command_list->OMSetRenderTargets(1, &back_buffer_descriptor, TRUE, nullptr);
    }

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), gc->command_list);

    {
        const D3D12_TEXTURE_BARRIER texture_barriers[] = {
            {
                .SyncBefore = D3D12_BARRIER_SYNC_RENDER_TARGET,
                .SyncAfter = D3D12_BARRIER_SYNC_NONE,
                .AccessBefore = D3D12_BARRIER_ACCESS_RENDER_TARGET,
                .AccessAfter = D3D12_BARRIER_ACCESS_NO_ACCESS,
                .LayoutBefore = D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                .LayoutAfter = D3D12_BARRIER_LAYOUT_PRESENT,
                .pResource = gc->swap_chain_buffers[gc->frame_index],
                .Subresources = { .IndexOrFirstMipLevel = 0xffffffff },
            }, {
                .SyncBefore = D3D12_BARRIER_SYNC_RESOLVE,
                .SyncAfter = D3D12_BARRIER_SYNC_NONE,
                .AccessBefore = D3D12_BARRIER_ACCESS_RESOLVE_SOURCE,
                .AccessAfter = D3D12_BARRIER_ACCESS_NO_ACCESS,
                .LayoutBefore = D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,
                .LayoutAfter = D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                .pResource = gc->msaa_srgb_rt,
                .Subresources = { .IndexOrFirstMipLevel = 0xffffffff },
            },
        };
        const D3D12_BARRIER_GROUP barrier_group = {
            .Type = D3D12_BARRIER_TYPE_TEXTURE,
            .NumBarriers = ARRAYSIZE(texture_barriers),
            .pTextureBarriers = texture_barriers,
        };
        gc->command_list->Barrier(1, &barrier_group);
    }

    VHR(gc->command_list->Close());

    gc->command_queue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList**>(&gc->command_list));

    present_gpu_frame(gc);
}
//--------------------------------------------------------------------------------------------------
static void jolt_trace(const char* fmt, ...)
{
    va_list list;
    va_start(list, fmt);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, list);
    va_end(list);

    LOG("[physics] %s", buffer);
}
//--------------------------------------------------------------------------------------------------
#ifdef JPH_ENABLE_ASSERTS
static bool jolt_assert_failed(const char* expression, const char* message, const char* file, u32 line)
{
    LOG("[physics] Assert failed (%s): (%s:%d) %s", expression, file, line, message ? message : "");

    // Breakpoint
    return true;
}
#endif
//--------------------------------------------------------------------------------------------------
static void init(GameState* game_state)
{
    assert(game_state);

    ImGui_ImplWin32_EnableDpiAwareness();
    const f32 dpi_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(nullptr);
    LOG("[game] Window DPI scale: %f", dpi_scale);

    const HWND window = create_window(GameState::WINDOW_NAME, static_cast<i32>(GameState::WINDOW_WIDTH * dpi_scale), static_cast<i32>(GameState::WINDOW_HEIGHT * dpi_scale));

    game_state->gpu.gc = new GpuContext();
    memset(game_state->gpu.gc, 0, sizeof(GpuContext));

    if (!init_gpu_context(window, game_state->gpu.gc)) {
        // TODO: Display message box in release mode.
        VHR(E_FAIL);
    }

    GpuContext* gc = game_state->gpu.gc;

    ImGui::CreateContext();
    ImGui::GetIO().Fonts->AddFontFromFileTTF("assets/Roboto-Medium.ttf", floor(16.0f * dpi_scale));

    if (!ImGui_ImplWin32_Init(window)) VHR(E_FAIL);
    if (!ImGui_ImplDX12_Init(gc->device, GpuContext::MAX_BUFFERED_FRAMES, DXGI_FORMAT_R8G8B8A8_UNORM, gc->gpu_heap, gc->gpu_heap_start_cpu, gc->gpu_heap_start_gpu)) VHR(E_FAIL);

    ImGui::GetStyle().ScaleAllSizes(dpi_scale);

    {
        const D2D1_FACTORY_OPTIONS options = { .debugLevel = D2D1_DEBUG_LEVEL_INFORMATION };
        VHR(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(game_state->gpu.d2d_factory), &options, reinterpret_cast<void**>(&game_state->gpu.d2d_factory)));
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
            .SampleDesc = { .Count = GpuContext::NUM_MSAA_SAMPLES },
        };

        VHR(gc->device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&game_state->gpu.pipelines[0])));
        VHR(gc->device->CreateRootSignature(0, vs.data(), vs.size(), IID_PPV_ARGS(&game_state->gpu.root_signatures[0])));
    }

    // Upload buffers
    for (i32 i = 0; i < GpuContext::MAX_BUFFERED_FRAMES; ++i) {
        const D3D12_HEAP_PROPERTIES heap_desc = { .Type = D3D12_HEAP_TYPE_UPLOAD };
        const D3D12_RESOURCE_DESC1 desc = {
            .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Width = GameState::GPU_BUFFER_SIZE_DYNAMIC,
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .SampleDesc = { .Count = 1 },
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        };
        VHR(gc->device->CreateCommittedResource3(&heap_desc, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&game_state->gpu.upload_buffers[i])));

        const D3D12_RANGE range = { .Begin = 0, .End = 0 };
        VHR(game_state->gpu.upload_buffers[i]->Map(0, &range, reinterpret_cast<void**>(&game_state->gpu.upload_buffer_bases[i])));
    }

    // Static buffer
    {
        const D3D12_HEAP_PROPERTIES heap_desc = { .Type = D3D12_HEAP_TYPE_DEFAULT };
        const D3D12_RESOURCE_DESC1 desc = {
            .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Width = GameState::GPU_BUFFER_SIZE_STATIC,
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .SampleDesc = { .Count = 1 },
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        };
        VHR(gc->device->CreateCommittedResource3(&heap_desc, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&game_state->gpu.buffer_static)));
    }

    // Dynamic buffer
    {
        const D3D12_HEAP_PROPERTIES heap_desc = { .Type = D3D12_HEAP_TYPE_DEFAULT };
        const D3D12_RESOURCE_DESC1 desc = {
            .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Width = GameState::GPU_BUFFER_SIZE_DYNAMIC,
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .SampleDesc = { .Count = 1 },
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        };
        VHR(gc->device->CreateCommittedResource3(&heap_desc, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&game_state->gpu.buffer_dynamic)));
    }

    // Create static meshes and store them in the upload buffer
    {
        game_state->meshes.resize(StaticMesh::NUM);

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
            VHR(game_state->gpu.d2d_factory->CreateRoundedRectangleGeometry(&shape, &geo));
            defer { SAFE_RELEASE(geo); };

            const u32 first_vertex = static_cast<u32>(tess_sink.vertices.size());
            VHR(geo->Tessellate(nullptr, D2D1_DEFAULT_FLATTENING_TOLERANCE, &tess_sink));
            const u32 num_vertices = static_cast<u32>(tess_sink.vertices.size()) - first_vertex;

            game_state->meshes[StaticMesh::ROUND_RECT_100x100] = { first_vertex, num_vertices };
        }
        {
            const D2D1_ELLIPSE shape = {
                .point = { 0.0f, 0.0f }, .radiusX = 100.0f, .radiusY = 100.0f,
            };
            ID2D1EllipseGeometry* geo = nullptr;
            VHR(game_state->gpu.d2d_factory->CreateEllipseGeometry(&shape, &geo));
            defer { SAFE_RELEASE(geo); };

            const u32 first_vertex = static_cast<u32>(tess_sink.vertices.size());
            VHR(geo->Tessellate(nullptr, D2D1_DEFAULT_FLATTENING_TOLERANCE, &tess_sink));
            const u32 num_vertices = static_cast<u32>(tess_sink.vertices.size()) - first_vertex;

            game_state->meshes[StaticMesh::CIRCLE_100] = { first_vertex, num_vertices };
        }
        {
            const D2D1_RECT_F shape = { -50.0f, -50.0f, 50.0f, 50.0f };
            ID2D1RectangleGeometry* geo = nullptr;
            VHR(game_state->gpu.d2d_factory->CreateRectangleGeometry(&shape, &geo));
            defer { SAFE_RELEASE(geo); };

            const u32 first_vertex = static_cast<u32>(tess_sink.vertices.size());
            VHR(geo->Tessellate(nullptr, D2D1_DEFAULT_FLATTENING_TOLERANCE, &tess_sink));
            const u32 num_vertices = static_cast<u32>(tess_sink.vertices.size()) - first_vertex;

            game_state->meshes[StaticMesh::RECT_100x100] = { first_vertex, num_vertices };
        }
        {
            ID2D1PathGeometry* geo = nullptr;
            VHR(game_state->gpu.d2d_factory->CreatePathGeometry(&geo));
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
            VHR(game_state->gpu.d2d_factory->CreatePathGeometry(&geo1));
            defer { SAFE_RELEASE(geo1); };

            ID2D1GeometrySink* sink1 = nullptr;
            VHR(geo1->Open(&sink1));
            defer { SAFE_RELEASE(sink1); };
            VHR(geo->Widen(50.0f, nullptr, nullptr, D2D1_DEFAULT_FLATTENING_TOLERANCE, sink1));
            VHR(sink1->Close());

            const u32 first_vertex = static_cast<u32>(tess_sink.vertices.size());
            VHR(geo1->Tessellate(nullptr, D2D1_DEFAULT_FLATTENING_TOLERANCE, &tess_sink));
            const u32 num_vertices = static_cast<u32>(tess_sink.vertices.size()) - first_vertex;

            game_state->meshes[StaticMesh::PATH_00] = { first_vertex, num_vertices };
        }

        auto* ptr = reinterpret_cast<CppHlsl_Vertex*>(game_state->gpu.upload_buffer_bases[0]);
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
            gc->device->CreateShaderResourceView(game_state->gpu.buffer_static, &desc, { .ptr = gc->gpu_heap_start_cpu.ptr + RDH_VERTEX_BUFFER_STATIC * gc->gpu_heap_descriptor_size });
        }
    }

    // Copy upload buffer to the static buffer
    {
        VHR(gc->command_allocators[0]->Reset());
        VHR(gc->command_list->Reset(gc->command_allocators[0], nullptr));

        {
            const D3D12_BUFFER_BARRIER buffer_barriers[] = {
                {
                    .SyncBefore = D3D12_BARRIER_SYNC_NONE,
                    .SyncAfter = D3D12_BARRIER_SYNC_COPY,
                    .AccessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS,
                    .AccessAfter = D3D12_BARRIER_ACCESS_COPY_SOURCE,
                    .pResource = game_state->gpu.upload_buffers[0],
                    .Size = UINT64_MAX,
                }, {
                    .SyncBefore = D3D12_BARRIER_SYNC_NONE,
                    .SyncAfter = D3D12_BARRIER_SYNC_COPY,
                    .AccessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS,
                    .AccessAfter = D3D12_BARRIER_ACCESS_COPY_DEST,
                    .pResource = game_state->gpu.buffer_static,
                    .Size = UINT64_MAX,
                },
            };
            const D3D12_BARRIER_GROUP barrier_group = {
                .Type = D3D12_BARRIER_TYPE_BUFFER,
                .NumBarriers = ARRAYSIZE(buffer_barriers),
                .pBufferBarriers = buffer_barriers,
            };
            gc->command_list->Barrier(1, &barrier_group);
        }

        gc->command_list->CopyBufferRegion(game_state->gpu.buffer_static, 0, game_state->gpu.upload_buffers[0], 0, GameState::GPU_BUFFER_SIZE_DYNAMIC);

        VHR(gc->command_list->Close());

        gc->command_queue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList**>(&gc->command_list));

        finish_gpu_commands(gc);
    }

    game_state->objects.push_back({ .mesh_index = StaticMesh::ROUND_RECT_100x100 });
    game_state->cpp_hlsl_objects.push_back({ .x = -400.0f, .y = 400.0f });

    game_state->objects.push_back({ .mesh_index = StaticMesh::RECT_100x100 });
    game_state->cpp_hlsl_objects.push_back({ .x = 600.0f, .y = -200.0f });

    game_state->objects.push_back({ .mesh_index = StaticMesh::CIRCLE_100 });
    game_state->cpp_hlsl_objects.push_back({ .x = 0.0f, .y = 0.0f });

    game_state->objects.push_back({ .mesh_index = StaticMesh::PATH_00 });
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
        gc->device->CreateShaderResourceView(game_state->gpu.buffer_dynamic, &desc, { .ptr = gc->gpu_heap_start_cpu.ptr + RDH_FRAME_STATE * gc->gpu_heap_descriptor_size });
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
        gc->device->CreateShaderResourceView(game_state->gpu.buffer_dynamic, &desc, { .ptr = gc->gpu_heap_start_cpu.ptr + RDH_OBJECTS_DYNAMIC * gc->gpu_heap_descriptor_size });
    }
}
//--------------------------------------------------------------------------------------------------
static void shutdown(GameState* game_state)
{
    assert(game_state);

    if (game_state->gpu.gc) finish_gpu_commands(game_state->gpu.gc);

    SAFE_RELEASE(game_state->gpu.d2d_factory);
    SAFE_RELEASE(game_state->gpu.buffer_static);
    SAFE_RELEASE(game_state->gpu.buffer_dynamic);
    for (i32 i = 0; i < ARRAYSIZE(game_state->gpu.upload_buffers); ++i) {
        SAFE_RELEASE(game_state->gpu.upload_buffers[i]);
    }
    for (i32 i = 0; i < ARRAYSIZE(game_state->gpu.pipelines); ++i) {
        SAFE_RELEASE(game_state->gpu.pipelines[i]);
        SAFE_RELEASE(game_state->gpu.root_signatures[i]);
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

    if (game_state->gpu.gc) {
        shutdown_gpu_context(game_state->gpu.gc);
        delete game_state->gpu.gc;
        game_state->gpu.gc = nullptr;
    }
}
//--------------------------------------------------------------------------------------------------
static void update(GameState* game_state)
{
    assert(game_state);

    game_state->is_window_minimized = !handle_window_resize(game_state->gpu.gc);
    if (game_state->is_window_minimized)
        return;

    f64 time;
    f32 delta_time;
    update_frame_stats(game_state->gpu.gc->window, GameState::WINDOW_NAME, &time, &delta_time);

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::ShowDemoWindow();

    ImGui::Render();
}
//--------------------------------------------------------------------------------------------------
static void draw(GameState* game_state)
{
    assert(game_state);

    GpuContext* gc = game_state->gpu.gc;

    {
        const f32 r = 500.0f;
        XMMATRIX xform;
        if (gc->window_width >= gc->window_height) {
            const float aspect = static_cast<f32>(gc->window_width) / gc->window_height;
            xform = XMMatrixOrthographicOffCenterLH(-r * aspect, r * aspect, -r, r, -1.0f, 1.0f);
        } else {
            const float aspect = static_cast<f32>(gc->window_height) / gc->window_width;
            xform = XMMatrixOrthographicOffCenterLH(-r, r, -r * aspect, r * aspect, -1.0f, 1.0f);
        }

        auto* ptr = reinterpret_cast<UploadData*>(game_state->gpu.upload_buffer_bases[gc->frame_index]);
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
                .pResource = game_state->gpu.upload_buffers[gc->frame_index],
                .Size = UINT64_MAX,
            }, {
                .SyncBefore = D3D12_BARRIER_SYNC_NONE,
                .SyncAfter = D3D12_BARRIER_SYNC_COPY,
                .AccessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS,
                .AccessAfter = D3D12_BARRIER_ACCESS_COPY_DEST,
                .pResource = game_state->gpu.buffer_dynamic,
                .Size = UINT64_MAX,
            },
        };
        const D3D12_BARRIER_GROUP barrier_group = {
            .Type = D3D12_BARRIER_TYPE_BUFFER,
            .NumBarriers = ARRAYSIZE(buffer_barriers),
            .pBufferBarriers = buffer_barriers,
        };
        gc->command_list->Barrier(1, &barrier_group);
    }

    gc->command_list->CopyBufferRegion(game_state->gpu.buffer_dynamic, 0, game_state->gpu.upload_buffers[gc->frame_index], 0, sizeof(UploadData));

    {
        const D3D12_BUFFER_BARRIER buffer_barriers[] = {
            {
                .SyncBefore = D3D12_BARRIER_SYNC_NONE,
                .SyncAfter = D3D12_BARRIER_SYNC_DRAW,
                .AccessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS,
                .AccessAfter = D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                .pResource = game_state->gpu.buffer_static,
                .Size = UINT64_MAX,
            }, {
                .SyncBefore = D3D12_BARRIER_SYNC_COPY,
                .SyncAfter = D3D12_BARRIER_SYNC_DRAW,
                .AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST,
                .AccessAfter = D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                .pResource = game_state->gpu.buffer_dynamic,
                .Size = UINT64_MAX,
            },
        };
        const D3D12_BARRIER_GROUP barrier_group = {
            .Type = D3D12_BARRIER_TYPE_BUFFER,
            .NumBarriers = ARRAYSIZE(buffer_barriers),
            .pBufferBarriers = buffer_barriers,
        };
        gc->command_list->Barrier(1, &barrier_group);
    }

    gc->command_list->SetPipelineState(game_state->gpu.pipelines[0]);
    gc->command_list->SetGraphicsRootSignature(game_state->gpu.root_signatures[0]);
    gc->command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (usize i = 0; i < game_state->objects.size(); ++i) {
        const Object* obj = &game_state->objects[i];
        const u32 mesh_index = obj->mesh_index;

        const u32 root_consts[2] = {
            game_state->meshes[mesh_index].first_vertex,
            static_cast<u32>(i), // object index
        };

        gc->command_list->SetGraphicsRoot32BitConstants(0, ARRAYSIZE(root_consts), &root_consts, 0);
        gc->command_list->DrawInstanced(game_state->meshes[mesh_index].num_vertices, 1, 0, 0);
    }
}
//--------------------------------------------------------------------------------------------------
i32 main()
{
    auto game_state = new GameState();
    memset(game_state, 0, sizeof(GameState));
    defer { delete game_state; };

    init(game_state);
    defer { shutdown(game_state); };

    while (true) {
        MSG msg = {};
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) break;
        } else {
            update(game_state);
            draw_frame(game_state);
        }
    }

    return 0;
}
//--------------------------------------------------------------------------------------------------
