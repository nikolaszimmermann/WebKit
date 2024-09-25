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

#pragma once

#if USE(COORDINATED_GRAPHICS) && USE(SKIA)
#include "BitmapTexturePool.h"
#include "NicosiaBuffer.h"

#include <wtf/AutomaticThread.h>
#include <wtf/Deque.h>
#include <wtf/Function.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace WebCore {

class GraphicsLayer;
class TiledBackingStore;

namespace DisplayList {
class DisplayList;
}

class SkiaThreadedPaintingPool {
    WTF_MAKE_TZONE_ALLOCATED(SkiaThreadedPaintingPool);
    WTF_MAKE_NONCOPYABLE(SkiaThreadedPaintingPool);
public:
    SkiaThreadedPaintingPool();
    ~SkiaThreadedPaintingPool();

    static std::unique_ptr<SkiaThreadedPaintingPool> create()
    {
        return makeUnique<SkiaThreadedPaintingPool>();
    }

    using TaskFunction = Function<void(Nicosia::Buffer&, DisplayList::DisplayList&)>;
    Ref<Nicosia::Buffer> postPaintingTask(const TiledBackingStore&, const GraphicsLayer&, const IntRect& dirtyRect, SkiaThreadedPaintingPool::TaskFunction&&);

private:
    class Worker;
    friend class Worker;

private:
    unsigned numberOfPaintingThreads();
    unsigned minimumAreaForGPUPainting();

    void dumpStatisticsTimerFired();

    Ref<Nicosia::Buffer> createAcceleratedBuffer(const IntSize&, Nicosia::Buffer::Flags);
    Ref<Nicosia::Buffer> createUnacceleratedBuffer(const IntSize&, Nicosia::Buffer::Flags);

    std::unique_ptr<DisplayList::DisplayList> recordDisplayList(const TiledBackingStore&, const GraphicsLayer&, const IntRect& dirtyRect) const;

    struct Task {
        RefPtr<Nicosia::Buffer> buffer;
        std::unique_ptr<DisplayList::DisplayList> displayList;
        TaskFunction task;
    };

    Box<Lock> m_lock;
    Ref<AutomaticThreadCondition> m_condition;

    unsigned m_numberOfActiveWorkers { 0 };

    Vector<Ref<Worker>> m_workers;
    Deque<Task> m_tasks;

    BitmapTexturePool m_texturePool;
    RunLoop::Timer m_dumpStatisticsTimer;

};

} // namespace WebCore

#endif // USE(COORDINATED_GRAPHICS) && USE(SKIA)
