#pragma once


#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_6.h>
#include "d3d12.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

#include <vector>
#include <algorithm>
#include <functional>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

#include "Jolt/Jolt.h"

#include "DirectXMath.h"
using namespace DirectX;

using u64 = uint64_t;
using u32 = uint32_t;
using u16 = uint16_t;
using u8 = uint8_t;
using i64 = int64_t;
using i32 = int32_t;
using i16 = int16_t;
using i8 = int8_t;
using usize = size_t;
using ssize = ptrdiff_t;
using f32 = float;
using f64 = double;

#define RETURN_IF_FAIL(hr) do { \
    if (FAILED(hr)) { \
        OutputDebugStringA("HRESULT error detected"); \
        assert(false); \
        return false; \
    } \
} while(0)

#define SAFE_RELEASE(obj) do { \
    if ((obj)) { \
        (obj)->Release(); \
        (obj) = nullptr; \
    } \
} while(0)

#define LOG(fmt, ...) do { \
    fprintf(stderr, (fmt), __VA_ARGS__); \
    fprintf(stderr, " (%s:%d)\n", __FILE__, __LINE__); \
} while(0)
