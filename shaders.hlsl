#if defined(_S00)

struct Vertex {
    float2 position;
};
#define ROOT_SIGNATURE "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)"
    //Texture2D srv_ao_texture = ResourceDescriptorHeap[draw_const.ao_index];

//#define ROOT_SIGNATURE "RootFlags(0)"

[RootSignature(ROOT_SIGNATURE)]
void s00_vs(
    uint vertex_id : SV_VertexID,
    out float4 out_position : SV_Position)
{
    StructuredBuffer<Vertex> vertex_buffer = ResourceDescriptorHeap[1];

    //const float2 verts[] = { float2(-0.9, -0.9), float2(0.0, 0.9), float2(0.9, -0.9) };
    //out_position = float4(verts[vertex_id], 0.0, 1.0);

    out_position = float4(vertex_buffer[vertex_id], 0.0, 1.0);
}

[RootSignature(ROOT_SIGNATURE)]
void s00_ps(
    float4 position : SV_Position,
    out float4 out_color : SV_Target0)
{
    out_color = float4(0.75, 0.0, 0.0, 1.0);
}

#endif
