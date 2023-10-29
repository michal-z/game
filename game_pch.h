#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "d3d12.h"
#include <dxgi1_6.h>
#include <d2d1_3.h>

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
#include "Jolt/RegisterTypes.h"
#include "Jolt/Core/Factory.h"
#include "Jolt/Core/TempAllocator.h"
#include "Jolt/Core/JobSystemThreadPool.h"
#include "Jolt/Physics/PhysicsSettings.h"
#include "Jolt/Physics/PhysicsSystem.h"
#include "Jolt/Physics/Collision/Shape/BoxShape.h"
#include "Jolt/Physics/Collision/Shape/SphereShape.h"
#include "Jolt/Physics/Body/BodyCreationSettings.h"
#include "Jolt/Physics/Body/BodyActivationListener.h"

#pragma warning(push)
#pragma warning(disable:4668)
#include "DirectXMath.h"
#pragma warning(pop)
using namespace DirectX;

#pragma warning(push)
#pragma warning(disable:4365)
#pragma warning(disable:4100)
#include "tracy/Tracy.hpp"
#include "tracy/TracyD3D12.hpp"
#pragma warning(pop)

using u64 = uint64_t;
using u32 = uint32_t;
using u16 = uint16_t;
using u8 = uint8_t;
using i64 = int64_t;
using i32 = int32_t;
using i16 = int16_t;
using i8 = int8_t;
using usize = size_t;
using isize = ptrdiff_t;
using f32 = float;
using f64 = double;

#define func static auto

#define LOG(fmt, ...) do \
{ \
    fprintf(stderr, (fmt), __VA_ARGS__); \
    fprintf(stderr, " (%s:%d)\n", __FILE__, __LINE__); \
} while(0)

#define VHR(r) do \
{ \
    if (FAILED(r)) { \
        LOG("[%s()] HRESULT error detected (0x%lX)", __FUNCTION__, r); \
        assert(false); \
        ExitProcess(1); \
    } \
} while(0)

#define SAFE_RELEASE(obj) do \
{ \
    if ((obj)) { \
        (obj)->Release(); \
        (obj) = nullptr; \
    } \
} while(0)

template<typename F> class DeferFinalizer
{
    F fn;
    bool moved;
public:
    template<typename T> DeferFinalizer(T&& f) : fn(std::forward<T>(f)), moved(false) {}

    DeferFinalizer(const DeferFinalizer&) = delete;
    DeferFinalizer& operator=(const DeferFinalizer&) = delete;

    DeferFinalizer(DeferFinalizer&& other) : fn(std::move(other.fn)), moved(other.moved) {
        other.moved = true;
    }

    ~DeferFinalizer() {
        if (!moved) fn();
    }
};

static struct
{
    template<typename F> DeferFinalizer<F> operator<<(F&& f) {
        return DeferFinalizer<F>(std::forward<F>(f));
    }
} __deferrer;

#define _TOKENPASTE(x, y) x ## y
#define _TOKENPASTE2(x, y) _TOKENPASTE(x, y)
#define defer auto _TOKENPASTE2(__deferred_lambda_call, __COUNTER__) = __deferrer << [&]
