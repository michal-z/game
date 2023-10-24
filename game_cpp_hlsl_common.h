#pragma once

#ifdef __cplusplus
#define float4x4 XMFLOAT4X4
#endif

#define RDH_FRAME_STATE 1
#define RDH_VERTEX_BUFFER_STATIC 2
#define RDH_OBJECTS_DYNAMIC 3

struct CppHlsl_Vertex
{
    float x, y;
};

struct CppHlsl_Object
{
    float x, y;
    float scalex, scaley;
    float rotation_in_radians;
    unsigned int color;
    float _padding[2];
};

struct CppHlsl_FrameState
{
    float4x4 proj;
    float _padding[112];
};

#ifdef __cplusplus
static_assert(sizeof(CppHlsl_Object) == 32);
static_assert(sizeof(CppHlsl_FrameState) == 512);
static_assert((sizeof(CppHlsl_FrameState) % sizeof(CppHlsl_Object)) == 0);
static_assert((sizeof(CppHlsl_FrameState) / sizeof(CppHlsl_Object)) == 16);
#endif
