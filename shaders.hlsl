struct Transform {
    float4x4 m;
};

struct Vertex {
    float2 position;
};

#if defined(_S00)

#define ROOT_SIGNATURE "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED), " \
    "RootConstants(b0, num32BitConstants = 1)"

struct RootConst {
    uint first_vertex;
};
ConstantBuffer<RootConst> root_const : register(b0);

[RootSignature(ROOT_SIGNATURE)]
void s00_vs(
    uint vertex_id : SV_VertexID,
    out float4 out_position : SV_Position)
{
    StructuredBuffer<Transform> xform = ResourceDescriptorHeap[1];
    StructuredBuffer<Vertex> vertex_buffer = ResourceDescriptorHeap[2];

    const uint first_vertex = root_const.first_vertex;

    out_position = mul(float4(vertex_buffer[vertex_id + first_vertex], 0.0, 1.0), xform[0].m);
}

[RootSignature(ROOT_SIGNATURE)]
void s00_ps(
    float4 position : SV_Position,
    out float4 out_color : SV_Target0)
{
    out_color = float4(0.2, 0.8, 0.1, 1.0);
}

#endif
