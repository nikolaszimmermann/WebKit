/*
 * Copyright (C) 2017 Yusuke Suzuki <utatane.tea@gmail.com>.
 * Copyright (C) 2024 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "SkiaThreadedPaintingPool.h"

#if USE(COORDINATED_GRAPHICS) && USE(SKIA)
#include "DisplayListDrawingContext.h"
#include "GLContext.h"
#include "PlatformDisplay.h"
#include "ProcessCapabilities.h"
#include "TiledBackingStore.h"
#include <wtf/NumberOfCores.h>
#include <wtf/SystemTracing.h>
#include <wtf/text/StringToIntegerConversion.h>

constexpr bool dumpPaintTaskSettings = true;
constexpr bool collectPaintTaskStatistics = false;
constexpr Seconds dumpPaintTaskStatisticsInterval = 5_s;

static unsigned long s_numberOfPaintTasks;
static unsigned long s_numberOfPaintTasksDispatchedToCPU;
static unsigned long s_numberOfPaintTasksDispatchedToGPU;
static unsigned long s_numberOfPaintTasksAboveGPUSizeThreshold;

namespace WebCore {

class SkiaThreadedPaintingPool::Worker final : public AutomaticThread {
public:
    Worker(const AbstractLocker& locker, SkiaThreadedPaintingPool& pool, Box<Lock> lock, Ref<AutomaticThreadCondition>&& condition)
        : AutomaticThread(locker, lock, WTFMove(condition), ThreadType::Graphics, Seconds::infinity())
        , m_pool(pool)
    {
    }

    bool shouldSleep(const AbstractLocker&) final { return false; }
    ASCIILiteral name() const final { return "SkiaThreadedPaintingPool"_s; }

    PollResult poll(const AbstractLocker&) final
    {
        if (m_pool.m_tasks.isEmpty())
            return PollResult::Wait;

        m_task = m_pool.m_tasks.takeFirst();
        if (!m_task.task)
            return PollResult::Stop;

        return PollResult::Work;
    }

    WorkResult work() final
    {
        ASSERT(m_task.task);

        // The uses-accelerated-rendering setting is thread-local, be sure to set the correct value, when replaying the display lists.
        PlatformDisplay::sharedDisplay().setUsesAcceleratedRendering(m_task.buffer->isBackedByOpenGL());

        m_task.task(*m_task.buffer.get(), *m_task.displayList.get());
        m_task.buffer->completePainting();

        // Destruct display list on main thread.
        ensureOnMainThread([displayList = WTFMove(m_task.displayList)]() mutable {
            displayList = nullptr;
        });

        m_task = { };
        return WorkResult::Continue;
    }

    void threadDidStart() final
    {
        Locker locker { *m_pool.m_lock };
        m_pool.m_numberOfActiveWorkers++;
    }

    void threadIsStopping(const AbstractLocker&) final
    {
        m_pool.m_numberOfActiveWorkers--;
    }

    SkiaThreadedPaintingPool& m_pool;
    SkiaThreadedPaintingPool::Task m_task;
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(SkiaThreadedPaintingPool);

SkiaThreadedPaintingPool::SkiaThreadedPaintingPool()
    : m_lock(Box<Lock>::create())
    , m_condition(AutomaticThreadCondition::create())
    , m_dumpStatisticsTimer(RunLoop::current(), this, &SkiaThreadedPaintingPool::dumpStatisticsTimerFired)
{
    Locker locker { *m_lock };

    auto numberOfWorkers = numberOfPaintingThreads();
    for (unsigned i = 0; i < numberOfWorkers; ++i)
        m_workers.append(adoptRef(*new Worker(locker, *this, m_lock, m_condition.copyRef())));

    if (dumpPaintTaskSettings)
        WTFLogAlways("SkiaThreadedPaintingPool(), using %2u painting threads, min GPU area %7upx (GPU rendering active? %u)", numberOfWorkers, minimumAreaForGPUPainting(), ProcessCapabilities::canUseAcceleratedBuffers());

    if (collectPaintTaskStatistics)
        m_dumpStatisticsTimer.startOneShot(dumpPaintTaskStatisticsInterval);
}

SkiaThreadedPaintingPool::~SkiaThreadedPaintingPool()
{
    if (!m_workers.size())
        return;

    Locker locker { *m_lock };
    for (unsigned i = m_workers.size(); i--;)
        m_tasks.append({ }); // Use null task to indicate that we want the thread to terminate.
    m_condition->notifyAll(locker);

    for (auto& worker : m_workers)
        worker->join();
    ASSERT(!m_numberOfActiveWorkers);
}

void SkiaThreadedPaintingPool::dumpStatisticsTimerFired()
{
    if (!collectPaintTaskStatistics)
        return;

    WTFLogAlways("\n*** SkiaThreadedPaintingPool statistics ***\n");
    WTFLogAlways("Number of paint tasks (total): %lu\n", s_numberOfPaintTasks);
    WTFLogAlways("Number of paint tasks (->CPU): %lu (%.2f%%)\n", s_numberOfPaintTasksDispatchedToCPU, float(s_numberOfPaintTasksDispatchedToCPU) / float(s_numberOfPaintTasks) * 100.0f);
    WTFLogAlways("Number of paint tasks (->GPU): %lu (%.2f%%)\n", s_numberOfPaintTasksDispatchedToGPU, float(s_numberOfPaintTasksDispatchedToGPU) / float(s_numberOfPaintTasks) * 100.0f);
    WTFLogAlways("Number of p.t. above treshold: %lu (%.2f%%)\n", s_numberOfPaintTasksAboveGPUSizeThreshold, float(s_numberOfPaintTasksAboveGPUSizeThreshold) / float(s_numberOfPaintTasks) * 100.0f);

    m_dumpStatisticsTimer.startOneShot(dumpPaintTaskStatisticsInterval);
}

Ref<Nicosia::Buffer> SkiaThreadedPaintingPool::createUnacceleratedBuffer(const IntSize& size, Nicosia::Buffer::Flags flags)
{
    return Nicosia::UnacceleratedBuffer::create(size, flags);
}

Ref<Nicosia::Buffer> SkiaThreadedPaintingPool::createAcceleratedBuffer(const IntSize& size, Nicosia::Buffer::Flags flags)
{
    OptionSet<BitmapTexture::Flags> textureFlags;
    if (flags & Nicosia::Buffer::SupportsAlpha)
        textureFlags.add(BitmapTexture::Flags::SupportsAlpha);

    auto texture = m_texturePool.acquireTexture(size, textureFlags);
    return Nicosia::AcceleratedBuffer::create(WTFMove(texture));
}

std::unique_ptr<DisplayList::DisplayList> SkiaThreadedPaintingPool::recordDisplayList(const TiledBackingStore& tiledBackingStore, const GraphicsLayer& layer, const IntRect& dirtyRect) const
{
    auto paintIntoGraphicsContext = [&](GraphicsContext& context) {
        IntRect initialClip(IntPoint::zero(), dirtyRect.size());
        context.clip(initialClip);

        if (!layer.contentsOpaque()) {
            context.setCompositeOperation(CompositeOperator::Copy);
            context.fillRect(initialClip, Color::transparentBlack);
            context.setCompositeOperation(CompositeOperator::SourceOver);
        }

        context.translate(-dirtyRect.x(), -dirtyRect.y());
        context.scale(tiledBackingStore.contentsScale());

        layer.paintGraphicsLayerContents(context, tiledBackingStore.mapToContents(dirtyRect));
    };

    WTFBeginSignpost(this, RecordTile);

    auto displayList = makeUnique<DisplayList::DisplayList>();
    DisplayList::RecorderImpl recordingContext(*displayList, GraphicsContextState(), FloatRect({ }, dirtyRect.size()), AffineTransform());
    paintIntoGraphicsContext(recordingContext);

    WTFEndSignpost(this, RecordTile);
    return displayList;
}

Ref<Nicosia::Buffer> SkiaThreadedPaintingPool::postPaintingTask(const TiledBackingStore& tiledBackingStore, const GraphicsLayer& layer, const IntRect& dirtyRect, SkiaThreadedPaintingPool::TaskFunction&& task)
{
    auto minGPUArea = minimumAreaForGPUPainting();

    if (collectPaintTaskStatistics) {
        ++s_numberOfPaintTasks;
        if (dirtyRect.area() >= minGPUArea)
            ++s_numberOfPaintTasksAboveGPUSizeThreshold;
    }

    auto bufferFlags = Nicosia::Buffer::Flags { layer.contentsOpaque() ? Nicosia::Buffer::NoFlags : Nicosia::Buffer::SupportsAlpha };

    auto dispatchToCPU = [&](auto& locker) {
        if (collectPaintTaskStatistics)
            ++s_numberOfPaintTasksDispatchedToCPU;

        PlatformDisplay::sharedDisplay().setUsesAcceleratedRendering(false);
        auto displayList = recordDisplayList(tiledBackingStore, layer, dirtyRect);

        auto buffer = createUnacceleratedBuffer(dirtyRect.size(), bufferFlags);
        buffer->beginPainting();
        m_tasks.append({ buffer.copyRef(), WTFMove(displayList), WTFMove(task) });
        m_condition->notifyOne(locker);
        return buffer;
    };

    auto dispatchToGPU = [&](auto& locker) {
        if (!PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent())
            return dispatchToCPU(locker);

        if (collectPaintTaskStatistics)
            ++s_numberOfPaintTasksDispatchedToGPU;

        PlatformDisplay::sharedDisplay().setUsesAcceleratedRendering(true);
        auto displayList = recordDisplayList(tiledBackingStore, layer, dirtyRect);

        auto buffer = createAcceleratedBuffer(dirtyRect.size(), bufferFlags);
        buffer->beginPainting();
        m_tasks.append({ buffer.copyRef(), WTFMove(displayList), WTFMove(task) });
        m_condition->notifyOne(locker);
        return buffer;
    };

    Locker locker { *m_lock };

    // Large painting area request: painting area > GPU painting area threshold.
    if (ProcessCapabilities::canUseAcceleratedBuffers() && dirtyRect.area() >= minGPUArea)
        return dispatchToGPU(locker);

    // Small painting area request: painting area < GPU painting area threshold.
    return dispatchToCPU(locker);
}

unsigned SkiaThreadedPaintingPool::numberOfPaintingThreads()
{
    static std::once_flag onceFlag;
    static unsigned numberOfThreads = 0;

    std::call_once(onceFlag, [] {
        numberOfThreads = std::max(1, std::min(8, WTF::numberOfProcessorCores() / 2 + 1)); // By default, use half the CPU cores (plus one extra for the GPU), capped at 8.
        if (const char* envString = getenv("WEBKIT_SKIA_PAINTING_THREADS")) {
            auto newValue = parseInteger<unsigned>(StringView::fromLatin1(envString));
            if (newValue && (*newValue >= 1 && *newValue <= 8))
                numberOfThreads = *newValue;
            else
                WTFLogAlways("The number of Skia painting threads is not between 1 and 8. Using the default value %u\n", numberOfThreads);
        }
    });

    return numberOfThreads;
}

unsigned SkiaThreadedPaintingPool::minimumAreaForGPUPainting()
{
    static std::once_flag onceFlag;
    static unsigned areaThreshold = 0;

    std::call_once(onceFlag, [] {
        areaThreshold = 128 * 128; // Prefer GPU rendering above an area of 128x128px (by default).

        if (const char* envString = getenv("WEBKIT_SKIA_GPU_PAINTING_MIN_AREA")) {
            if (auto newValue = parseInteger<unsigned>(StringView::fromLatin1(envString)))
                areaThreshold = *newValue;
        }
    });

    return areaThreshold;
}

} // namespace WebCore

#endif // USE(COORDINATED_GRAPHICS) && USE(SKIA)
