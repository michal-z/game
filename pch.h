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
    fprintf(stderr, "%s(%d) LOG: ", __FILE__, __LINE__); \
    fprintf(stderr, (fmt), __VA_ARGS__); \
    fprintf(stderr, "\n"); \
} while(0)
