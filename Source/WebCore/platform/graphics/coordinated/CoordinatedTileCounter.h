/*
 * Copyright (C) 2026 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if USE(COORDINATED_GRAPHICS)
#include <atomic>
#include <wtf/SharedTask.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

// Counts the tiles of one rendering update that the painting threads still have to paint.
class CoordinatedTileCounter final : public ThreadSafeRefCounted<CoordinatedTileCounter> {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(CoordinatedTileCounter);
public:
    using DidPaintAllTilesTask = SharedTask<void()>;

    static Ref<CoordinatedTileCounter> create(RefPtr<DidPaintAllTilesTask>&& didPaintAllTiles)
    {
        return adoptRef(*new CoordinatedTileCounter(WTF::move(didPaintAllTiles)));
    }

    void willPaintTile() { ++m_pendingTiles; }

    // Called from the painting threads. The last tile to finish runs the task.
    void didPaintTile()
    {
        ASSERT(m_pendingTiles.load());
        if (--m_pendingTiles)
            return;

        if (m_didPaintAllTiles)
            m_didPaintAllTiles->run();
    }

    bool hasPendingTiles() const { return !!m_pendingTiles.load(); }

private:
    explicit CoordinatedTileCounter(RefPtr<DidPaintAllTilesTask>&& didPaintAllTiles)
        : m_didPaintAllTiles(WTF::move(didPaintAllTiles))
    {
    }

    std::atomic<unsigned> m_pendingTiles { 0 };
    const RefPtr<DidPaintAllTilesTask> m_didPaintAllTiles;
};

} // namespace WebCore

#endif // USE(COORDINATED_GRAPHICS)
