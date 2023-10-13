#pragma once

#ifdef __cplusplus
#define float4x4 XMFLOAT4X4
#endif

struct CppHlsl_Vertex {
    float x, y;
};

struct CppHlsl_Object {
    float x, y;
};

struct CppHlsl_PerFrameState {
    float4x4 proj;
};

#ifdef __cplusplus
static_assert(sizeof(CppHlsl_PerFrameState) % sizeof(CppHlsl_Object) == 0); // TODO
#endif
