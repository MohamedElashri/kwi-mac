// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: CC0-1.0
#include "MacMetalTiming.h"
#import <Metal/Metal.h>
#include <QDir>
#include <QQuickWindow>
#include <QSGRendererInterface>
#import <QuartzCore/CAAnimation.h>
#include <atomic>
#include <cstdio>
#include <fcntl.h>
#include <memory>
#include <time.h>
#include <unistd.h>

extern "C" uint64_t kwi_current_drawable_sequence();

namespace
{
uint64_t traceTime()
{
    timespec value{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &value);
    return uint64_t(value.tv_sec) * 1'000'000'000 + value.tv_nsec;
}

struct Output {
    int fd{-1};
    ~Output()
    {
        if (fd >= 0)
            ::close(fd);
    }
};
}

bool watchMacMetalTiming(QQuickWindow *window)
{
    if (!qEnvironmentVariableIsSet("KWI_GPU_TRACE"))
        return true;
    if (window->rendererInterface()->graphicsApi() != QSGRendererInterface::Metal)
        return false;
    const auto output = std::make_shared<Output>();
    const auto path = QDir{qEnvironmentVariable("KWI_PERF_OUTPUT")}.filePath(QStringLiteral("gpu.jsonl")).toLocal8Bit();
    output->fd = ::open(path.constData(), O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0600);
    if (output->fd < 0)
        return false;
    QObject::connect(
        window,
        &QQuickWindow::afterRendering,
        window,
        [window, output] {
            auto *resource = window->rendererInterface()->getResource(window, QSGRendererInterface::CommandListResource);
            if (!resource)
                return;
            // The Metal backend returns the object itself (unlike Vulkan's
            // pointer to a handle); see QSGRhiSupport::rifResource.
            id<MTLCommandBuffer> command = static_cast<id<MTLCommandBuffer>>(resource);
            const auto sequence = kwi_current_drawable_sequence();
            if (!command || !sequence)
                return;
            const auto encoded = traceTime();
            const auto scheduled = std::make_shared<std::atomic<uint64_t>>(0);
            [command addScheduledHandler:^(id<MTLCommandBuffer>) { scheduled->store(traceTime(), std::memory_order_relaxed); }];
            [command addCompletedHandler:^(id<MTLCommandBuffer> value) {
                const double hostNow = CACurrentMediaTime();
                const auto completed = traceTime();
                char line[512];
                const int length = snprintf(line,
                                            sizeof(line),
                                            "{\"sequence\":%llu,\"encoded_ns\":%llu,\"scheduled_ns\":%llu,\"completed_ns\":%llu,"
                                            "\"ca_now_seconds\":%.9f,\"gpu_start_seconds\":%.9f,\"gpu_end_seconds\":%.9f,\"status\":%ld}\n",
                                            static_cast<unsigned long long>(sequence),
                                            static_cast<unsigned long long>(encoded),
                                            static_cast<unsigned long long>(scheduled->load(std::memory_order_relaxed)),
                                            static_cast<unsigned long long>(completed),
                                            hostNow,
                                            value.GPUStartTime,
                                            value.GPUEndTime,
                                            long(value.status));
                if (length > 0 && size_t(length) < sizeof(line))
                    (void)::write(output->fd, line, size_t(length));
            }];
        },
        Qt::DirectConnection);
    return true;
}
