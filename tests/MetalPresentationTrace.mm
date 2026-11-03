// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: CC0-1.0

// Opt-in test instrumentation only. Never linked into or deployed with Kwi.
#import <Metal/Metal.h>
#import <QuartzCore/CAAnimation.h>
#import <QuartzCore/CAMetalLayer.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#import <objc/runtime.h>
#include <time.h>
#include <unistd.h>

namespace
{
int output{-1};
IMP originalNextDrawable{};
unsigned requestedDrawableCount{};
std::atomic<uint64_t> nextSequence{1};
thread_local uint64_t currentSequence{0};

uint64_t traceTime()
{
    timespec value{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &value);
    return uint64_t(value.tv_sec) * 1'000'000'000 + value.tv_nsec;
}

id<CAMetalDrawable> nextDrawable(id layer, SEL selector)
{
    CAMetalLayer *metalLayer = static_cast<CAMetalLayer *>(layer);
    if (requestedDrawableCount && metalLayer.maximumDrawableCount != requestedDrawableCount)
        metalLayer.maximumDrawableCount = requestedDrawableCount;
    const unsigned maximumDrawables = unsigned(metalLayer.maximumDrawableCount);
    const bool transaction = metalLayer.presentsWithTransaction;
    const bool displaySync = metalLayer.displaySyncEnabled;
    const auto layerId = reinterpret_cast<uintptr_t>(metalLayer);
    const auto original = reinterpret_cast<id<CAMetalDrawable> (*)(id, SEL)>(originalNextDrawable);
    id<CAMetalDrawable> drawable = original(layer, selector);
    if (!drawable) {
        currentSequence = 0;
        return drawable;
    }
    const auto sequence = nextSequence.fetch_add(1, std::memory_order_relaxed);
    currentSequence = sequence;
    const auto acquired = traceTime();
    [drawable addPresentedHandler:^(id<MTLDrawable> value) {
        const double hostNow = CACurrentMediaTime();
        const auto traceNow = traceTime();
        const double presented = value.presentedTime;
        char line[512];
        const int length = snprintf(line,
                                    sizeof(line),
                                    "{\"sequence\":%llu,\"drawable_id\":%llu,\"acquired_ns\":%llu,\"trace_now_ns\":%llu,"
                                    "\"ca_now_seconds\":%.9f,\"presented_seconds\":%.9f,\"maximum_drawables\":%u,"
                                    "\"transaction_presentation\":%d,\"display_sync_enabled\":%d,\"layer_id\":%llu}\n",
                                    static_cast<unsigned long long>(sequence),
                                    static_cast<unsigned long long>(value.drawableID),
                                    static_cast<unsigned long long>(acquired),
                                    static_cast<unsigned long long>(traceNow),
                                    hostNow,
                                    presented,
                                    maximumDrawables,
                                    int(transaction),
                                    int(displaySync),
                                    static_cast<unsigned long long>(layerId));
        if (length > 0 && size_t(length) < sizeof(line))
            (void)::write(output, line, size_t(length));
    }];
    return drawable;
}

__attribute__((constructor)) void install()
{
    const char *count = getenv("KWI_PRESENTATION_DRAWABLE_COUNT");
    if (count && (count[0] == '2' || count[0] == '3') && count[1] == '\0')
        requestedDrawableCount = unsigned(count[0] - '0');
    unsetenv("KWI_PRESENTATION_DRAWABLE_COUNT");
    const char *path = getenv("KWI_PRESENTATION_TRACE");
    if (path)
        output = ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0600);
    // Keep instrumentation out of child shells and interpreters.
    unsetenv("KWI_PRESENTATION_TRACE");
    unsetenv("DYLD_INSERT_LIBRARIES");
    if (output < 0)
        return;
    Method method = class_getInstanceMethod([CAMetalLayer class], @selector(nextDrawable));
    if (method)
        originalNextDrawable = method_setImplementation(method, reinterpret_cast<IMP>(nextDrawable));
}
}

extern "C" __attribute__((visibility("default"))) uint64_t kwi_current_drawable_sequence()
{
    return currentSequence;
}
