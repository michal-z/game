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

#define LOG(fmt, ...) do { \
    fprintf(stderr, (fmt), __VA_ARGS__); \
    fprintf(stderr, " (%s:%d)\n", __FILE__, __LINE__); \
} while(0)

#define VHR(r) do { \
    if (FAILED(r)) { \
        LOG("[%s()] HRESULT error detected (0x%X)", __FUNCTION__, r); \
        return false; \
    } \
} while(0)

#define SAFE_RELEASE(obj) do { \
    if ((obj)) { \
        (obj)->Release(); \
        (obj) = nullptr; \
    } \
} while(0)

template<typename F> class defer_finalizer {
    F func;
    bool moved;
public:
    template<typename T> defer_finalizer(T&& f) : func(std::forward<T>(f)), moved(false) {}

    defer_finalizer(const defer_finalizer &) = delete;

    defer_finalizer(defer_finalizer&& other) : func(std::move(other.func)), moved(other.moved) {
        other.moved = true;
    }

    ~defer_finalizer() {
        if (!moved) func();
    }
};

static struct {
    template<typename F> defer_finalizer<F> operator<<(F&& f) {
        return defer_finalizer<F>(std::forward<F>(f));
    }
} __deferrer;

#define _TOKENPASTE(x, y) x ## y
#define _TOKENPASTE2(x, y) _TOKENPASTE(x, y)
#define defer auto _TOKENPASTE2(__deferred_lambda_call, __COUNTER__) = __deferrer << [&]
