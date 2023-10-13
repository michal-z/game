#include "cpp_hlsl_common.h"

#if defined(_S00)

#define ROOT_SIGNATURE "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED), " \
    "RootConstants(b0, num32BitConstants = 2)"

struct RootConst {
    uint first_vertex;
    uint object_index;
};
ConstantBuffer<RootConst> root_const : register(b0);

[RootSignature(ROOT_SIGNATURE)]
void s00_vs(
    uint vertex_index : SV_VertexID,
    out float4 out_position : SV_Position)
{
    StructuredBuffer<CppHlsl_PerFrameState> frame_state_buffer = ResourceDescriptorHeap[1];
    StructuredBuffer<CppHlsl_Vertex> vertex_buffer = ResourceDescriptorHeap[2];
    StructuredBuffer<CppHlsl_Object> object_buffer = ResourceDescriptorHeap[3];

    const uint first_vertex = root_const.first_vertex;
    const uint object_index = root_const.object_index;

    const CppHlsl_PerFrameState frame_state = frame_state_buffer[0];
    const CppHlsl_Vertex vertex = vertex_buffer[vertex_index + first_vertex];
    const CppHlsl_Object object = object_buffer[object_index];

    const float4x4 t = float4x4(
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        object.x, object.y, 0.0, 1.0);

    const float4 v = float4(vertex.x, vertex.y, 0.0, 1.0);
    out_position = mul(mul(v, t), frame_state.proj);
}

[RootSignature(ROOT_SIGNATURE)]
void s00_ps(
    float4 position : SV_Position,
    out float4 out_color : SV_Target0)
{
    out_color = float4(0.2, 0.8, 0.1, 1.0);
}

#endif
