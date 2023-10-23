#define WITH_D3D12_DEBUG_LAYER 1
#define WITH_D3D12_GPU_BASED_VALIDATION 0

struct GpuContext
{
    let ENABLE_VSYNC = true;
    let MAX_BUFFERED_FRAMES = 2;
    let MAX_GPU_DESCRIPTORS = 16 * 1024;
    let NUM_MSAA_SAMPLES = 8;
    let CLEAR_COLOR = XMVECTORF32{ 0.0f, 0.0f, 0.0f, 0.0f };

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

    fn init(GpuContext* gc, HWND window) -> bool {
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

        for (i32 i = 0; i < MAX_BUFFERED_FRAMES; ++i) {
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
            gc->swap_chain_present_interval = ENABLE_VSYNC;

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
            .BufferCount = MAX_BUFFERED_FRAMES,
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

        for (i32 i = 0; i < MAX_BUFFERED_FRAMES; ++i) {
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

        for (i32 i = 0; i < MAX_BUFFERED_FRAMES; ++i) {
            gc->device->CreateRenderTargetView(gc->swap_chain_buffers[i], nullptr, { .ptr = gc->rtv_heap_start.ptr + i * gc->rtv_heap_descriptor_size });
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

    fn deinit(GpuContext* gc) {
        assert(gc);

        SAFE_RELEASE(gc->msaa_srgb_rt);
        SAFE_RELEASE(gc->command_list);
        for (i32 i = 0; i < MAX_BUFFERED_FRAMES; ++i) SAFE_RELEASE(gc->command_allocators[i]);
        if (gc->frame_fence_event) {
            CloseHandle(gc->frame_fence_event);
            gc->frame_fence_event = nullptr;
        }
        SAFE_RELEASE(gc->frame_fence);
        SAFE_RELEASE(gc->gpu_heap);
        SAFE_RELEASE(gc->rtv_heap);
        for (i32 i = 0; i < MAX_BUFFERED_FRAMES; ++i) SAFE_RELEASE(gc->swap_chain_buffers[i]);
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

    fn handle_window_resize(GpuContext* gc) -> bool {
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

            finish(gc);

            for (i32 i = 0; i < MAX_BUFFERED_FRAMES; ++i) SAFE_RELEASE(gc->swap_chain_buffers[i]);

            VHR(gc->swap_chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, gc->swap_chain_flags));

            for (i32 i = 0; i < MAX_BUFFERED_FRAMES; ++i) {
                VHR(gc->swap_chain->GetBuffer(i, IID_PPV_ARGS(&gc->swap_chain_buffers[i])));
            }

            for (i32 i = 0; i < MAX_BUFFERED_FRAMES; ++i) {
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

    fn finish(GpuContext* gc) -> void {
        assert(gc && gc->device);
        gc->frame_fence_counter += 1;

        VHR(gc->command_queue->Signal(gc->frame_fence, gc->frame_fence_counter));
        VHR(gc->frame_fence->SetEventOnCompletion(gc->frame_fence_counter, gc->frame_fence_event));

        WaitForSingleObject(gc->frame_fence_event, INFINITE);
    }

    fn create_msaa_srgb_render_target(GpuContext* gc) -> void {
        assert(gc && gc->device);

        const D3D12_HEAP_PROPERTIES heap_desc = { .Type = D3D12_HEAP_TYPE_DEFAULT };
        const D3D12_RESOURCE_DESC1 desc = {
            .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            .Width = static_cast<u64>(gc->window_width),
            .Height = static_cast<u32>(gc->window_height),
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            .SampleDesc = { .Count = NUM_MSAA_SAMPLES },
            .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        };
        const D3D12_CLEAR_VALUE clear_value = {
            .Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            .Color = { CLEAR_COLOR[0], CLEAR_COLOR[1], CLEAR_COLOR[2], CLEAR_COLOR[3] },
        };
        VHR(gc->device->CreateCommittedResource3(&heap_desc, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_RENDER_TARGET, &clear_value, nullptr, 0, nullptr, IID_PPV_ARGS(&gc->msaa_srgb_rt)));

        gc->device->CreateRenderTargetView(gc->msaa_srgb_rt, nullptr, { .ptr = gc->rtv_heap_start.ptr + MAX_BUFFERED_FRAMES * gc->rtv_heap_descriptor_size });

        LOG("[graphics] MSAAx%d SRGB render target created (%dx%d)", NUM_MSAA_SAMPLES, gc->window_width, gc->window_height);
    }

    fn present(GpuContext* gc) -> void {
        assert(gc && gc->device);
        gc->frame_fence_counter += 1;

        UINT present_flags = 0;

        if (gc->swap_chain_present_interval == 0 && gc->swap_chain_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)
            present_flags |= DXGI_PRESENT_ALLOW_TEARING;

        VHR(gc->swap_chain->Present(gc->swap_chain_present_interval, present_flags));
        VHR(gc->command_queue->Signal(gc->frame_fence, gc->frame_fence_counter));

        const u64 gpu_frame_counter = gc->frame_fence->GetCompletedValue();
        if ((gc->frame_fence_counter - gpu_frame_counter) >= MAX_BUFFERED_FRAMES) {
            VHR(gc->frame_fence->SetEventOnCompletion(gpu_frame_counter + 1, gc->frame_fence_event));
            WaitForSingleObject(gc->frame_fence_event, INFINITE);
        }

        gc->frame_index = gc->swap_chain->GetCurrentBackBufferIndex();
    }
};
