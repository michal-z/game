#include "game_pch.h"
#include "game_main.h"
#include "game_cpp_hlsl_common.h"
#include "game_misc.cpp"
#include "game_gpu_context.cpp"

extern "C" {
    __declspec(dllexport) extern const u32 D3D12SDKVersion = 611;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\d3d12\\";
}

enum StaticMeshType
{
    STATIC_MESH_ROUND_RECT_100x100,
    STATIC_MESH_CIRCLE_100,
    STATIC_MESH_RECT_100x100,
    STATIC_MESH_PATH_00,
    STATIC_MESH_NUM,
};

struct StaticMesh
{
    u32 first_vertex;
    u32 num_vertices;
};

struct Object
{
    u32 mesh_index;
};

struct alignas(16) UploadData
{
    CppHlsl_FrameState frame_state;
    CppHlsl_Object objects[1024];
};

static constexpr auto OBJECT_LAYER_NON_MOVING = JPH::ObjectLayer(0);
static constexpr auto OBJECT_LAYER_MOVING = JPH::ObjectLayer(1);
#define OBJECT_LAYER_NUM 2

static constexpr auto BROAD_PHASE_LAYER_NON_MOVING = JPH::BroadPhaseLayer(0);
static constexpr auto BROAD_PHASE_LAYER_MOVING = JPH::BroadPhaseLayer(1);
#define BROAD_PHASE_LAYER_NUM 2

struct ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter
{
    virtual bool ShouldCollide(JPH::ObjectLayer object1, JPH::ObjectLayer object2) const override {
        switch (object1) {
            case OBJECT_LAYER_NON_MOVING: return object2 == OBJECT_LAYER_MOVING;
            case OBJECT_LAYER_MOVING: return true;
            default: JPH_ASSERT(false); return false;
        }
    }
};

struct BroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface
{
    JPH::BroadPhaseLayer object_to_broad_phase[OBJECT_LAYER_NUM];

    BroadPhaseLayerInterface() {
        object_to_broad_phase[OBJECT_LAYER_NON_MOVING] = BROAD_PHASE_LAYER_NON_MOVING;
        object_to_broad_phase[OBJECT_LAYER_MOVING] = BROAD_PHASE_LAYER_MOVING;
    }

    virtual u32 GetNumBroadPhaseLayers() const override { return BROAD_PHASE_LAYER_NUM; }

    virtual JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        JPH_ASSERT(layer < OBJECT_LAYER_NUM);
        return object_to_broad_phase[layer];
    }
};

struct ObjectVsBroadPhaseLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
{
    virtual bool ShouldCollide(JPH::ObjectLayer layer1, JPH::BroadPhaseLayer layer2) const override {
        switch (layer1) {
            case OBJECT_LAYER_NON_MOVING: return layer2 == BROAD_PHASE_LAYER_MOVING;
            case OBJECT_LAYER_MOVING: return true;
            default: JPH_ASSERT(false); return false;
        }
    }
};

func jolt_trace(const char* fmt, ...) -> void
{
    va_list list;
    va_start(list, fmt);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, list);
    va_end(list);

    LOG("[physics] %s", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
func jolt_assert_failed(const char* expression, const char* message, const char* file, u32 line) -> bool
{
    LOG("[physics] Assert failed (%s): (%s:%d) %s", expression, file, line, message ? message : "");

    return true; // breakpoint
}
#endif

#define WINDOW_NAME "game"
#define WINDOW_WIDTH 1200
#define WINDOW_HEIGHT 800
#define NUM_GPU_PIPELINES 1
#define GPU_BUFFER_SIZE_STATIC (8 * 1024 * 1024)
#define GPU_BUFFER_SIZE_DYNAMIC (256 * 1024)

struct GameState
{
    struct {
        GpuContext* gc;
        ID2D1Factory7* d2d_factory;
        ID3D12Resource2* buffer_static;
        ID3D12Resource2* buffer_dynamic;
        ID3D12Resource2* upload_buffers[GPU_MAX_BUFFERED_FRAMES];
        u8* upload_buffer_bases[GPU_MAX_BUFFERED_FRAMES];
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
        ObjectLayerPairFilter* object_layer_pair_filter;
        BroadPhaseLayerInterface* broad_phase_layer_interface;
        ObjectVsBroadPhaseLayerFilter* object_vs_broad_phase_layer_filter;
    } phy;
};

func init(GameState* game_state) -> void
{
    assert(game_state);

    ImGui_ImplWin32_EnableDpiAwareness();
    const f32 dpi_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(nullptr);
    LOG("[game] Window DPI scale: %f", dpi_scale);

    const HWND window = create_window(WINDOW_NAME, static_cast<i32>(WINDOW_WIDTH * dpi_scale), static_cast<i32>(WINDOW_HEIGHT * dpi_scale));

    game_state->gpu.gc = new GpuContext();
    memset(game_state->gpu.gc, 0, sizeof(GpuContext));

    if (!init_gpu_context(game_state->gpu.gc, window)) {
        // TODO: Display message box in release mode.
        VHR(E_FAIL);
    }

    GpuContext* gc = game_state->gpu.gc;

    ImGui::CreateContext();
    ImGui::GetIO().Fonts->AddFontFromFileTTF("assets/Roboto-Medium.ttf", floor(16.0f * dpi_scale));

    if (!ImGui_ImplWin32_Init(window)) VHR(E_FAIL);
    if (!ImGui_ImplDX12_Init(gc->device, GPU_MAX_BUFFERED_FRAMES, DXGI_FORMAT_R8G8B8A8_UNORM, gc->gpu_heap, gc->gpu_heap_start_cpu, gc->gpu_heap_start_gpu)) VHR(E_FAIL);

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
    game_state->phy.object_layer_pair_filter = new ObjectLayerPairFilter();
    game_state->phy.broad_phase_layer_interface = new BroadPhaseLayerInterface();
    game_state->phy.object_vs_broad_phase_layer_filter = new ObjectVsBroadPhaseLayerFilter();

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
            .SampleDesc = { .Count = GPU_NUM_MSAA_SAMPLES },
        };

        VHR(gc->device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&game_state->gpu.pipelines[0])));
        VHR(gc->device->CreateRootSignature(0, vs.data(), vs.size(), IID_PPV_ARGS(&game_state->gpu.root_signatures[0])));
    }

    // Upload buffers
    for (i32 i = 0; i < GPU_MAX_BUFFERED_FRAMES; ++i) {
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
        VHR(gc->device->CreateCommittedResource3(&heap_desc, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&game_state->gpu.upload_buffers[i])));

        const D3D12_RANGE range = { .Begin = 0, .End = 0 };
        VHR(game_state->gpu.upload_buffers[i]->Map(0, &range, reinterpret_cast<void**>(&game_state->gpu.upload_buffer_bases[i])));
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
        VHR(gc->device->CreateCommittedResource3(&heap_desc, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&game_state->gpu.buffer_static)));
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
        VHR(gc->device->CreateCommittedResource3(&heap_desc, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&game_state->gpu.buffer_dynamic)));
    }

    // Create static meshes and store them in the upload buffer
    {
        game_state->meshes.resize(STATIC_MESH_NUM);

        struct TessellationSink final : public ID2D1TessellationSink {
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

            game_state->meshes[STATIC_MESH_ROUND_RECT_100x100] = { first_vertex, num_vertices };
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

            game_state->meshes[STATIC_MESH_CIRCLE_100] = { first_vertex, num_vertices };
        }
        {
            const D2D1_RECT_F shape = { -50.0f, -50.0f, 50.0f, 50.0f };
            ID2D1RectangleGeometry* geo = nullptr;
            VHR(game_state->gpu.d2d_factory->CreateRectangleGeometry(&shape, &geo));
            defer { SAFE_RELEASE(geo); };

            const u32 first_vertex = static_cast<u32>(tess_sink.vertices.size());
            VHR(geo->Tessellate(nullptr, D2D1_DEFAULT_FLATTENING_TOLERANCE, &tess_sink));
            const u32 num_vertices = static_cast<u32>(tess_sink.vertices.size()) - first_vertex;

            game_state->meshes[STATIC_MESH_RECT_100x100] = { first_vertex, num_vertices };
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

            game_state->meshes[STATIC_MESH_PATH_00] = { first_vertex, num_vertices };
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

        gc->command_list->CopyBufferRegion(game_state->gpu.buffer_static, 0, game_state->gpu.upload_buffers[0], 0, GPU_BUFFER_SIZE_DYNAMIC);

        VHR(gc->command_list->Close());

        gc->command_queue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList**>(&gc->command_list));

        finish_gpu_commands(gc);
    }

    game_state->objects.push_back({ .mesh_index = STATIC_MESH_ROUND_RECT_100x100 });
    game_state->cpp_hlsl_objects.push_back({ .x = -400.0f, .y = 400.0f });

    game_state->objects.push_back({ .mesh_index = STATIC_MESH_RECT_100x100 });
    game_state->cpp_hlsl_objects.push_back({ .x = 600.0f, .y = -200.0f });

    game_state->objects.push_back({ .mesh_index = STATIC_MESH_CIRCLE_100 });
    game_state->cpp_hlsl_objects.push_back({ .x = 0.0f, .y = 0.0f });

    game_state->objects.push_back({ .mesh_index = STATIC_MESH_PATH_00 });
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

func shutdown(GameState* game_state) -> void
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
    if (game_state->phy.object_layer_pair_filter) {
        delete game_state->phy.object_layer_pair_filter;
        game_state->phy.object_layer_pair_filter = nullptr;
    }
    if (game_state->phy.broad_phase_layer_interface) {
        delete game_state->phy.broad_phase_layer_interface;
        game_state->phy.broad_phase_layer_interface = nullptr;
    }
    if (game_state->phy.object_vs_broad_phase_layer_filter) {
        delete game_state->phy.object_vs_broad_phase_layer_filter;
        game_state->phy.object_vs_broad_phase_layer_filter = nullptr;
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

func draw(GameState* game_state) -> void;

func draw_frame(GameState* game_state) -> void
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
            .ptr = gc->rtv_heap_start.ptr + GPU_MAX_BUFFERED_FRAMES * gc->rtv_heap_descriptor_size
        };
        const f32 clear_color[] = GPU_CLEAR_COLOR;

        gc->command_list->OMSetRenderTargets(1, &rt_descriptor, TRUE, nullptr);
        gc->command_list->ClearRenderTargetView(rt_descriptor, clear_color, 0, nullptr);
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

    present_frame(gc);
}

func update(GameState* game_state) -> void
{
    assert(game_state);

    game_state->is_window_minimized = !handle_window_resize(game_state->gpu.gc);
    if (game_state->is_window_minimized)
        return;

    f64 time;
    f32 delta_time;
    update_frame_stats(game_state->gpu.gc->window, WINDOW_NAME, &time, &delta_time);

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::ShowDemoWindow();

    ImGui::Render();
}

func draw(GameState* game_state) -> void
{
    assert(game_state);

    GpuContext* gc = game_state->gpu.gc;

    {
        const f32 r = 500.0f;
        XMMATRIX xform;
        if (gc->window_width >= gc->window_height) {
            const float aspect = static_cast<f32>(gc->window_width) / static_cast<f32>(gc->window_height);
            xform = XMMatrixOrthographicOffCenterLH(-r * aspect, r * aspect, -r, r, -1.0f, 1.0f);
        } else {
            const float aspect = static_cast<f32>(gc->window_height) / static_cast<f32>(gc->window_width);
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

        const u32 root_consts[] = {
            game_state->meshes[mesh_index].first_vertex,
            static_cast<u32>(i), // object index
        };

        gc->command_list->SetGraphicsRoot32BitConstants(0, ARRAYSIZE(root_consts), &root_consts, 0);
        gc->command_list->DrawInstanced(game_state->meshes[mesh_index].num_vertices, 1, 0, 0);
    }
}

func main() -> i32
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
