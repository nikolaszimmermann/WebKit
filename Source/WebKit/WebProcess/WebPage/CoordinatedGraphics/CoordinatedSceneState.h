/*
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if USE(COORDINATED_GRAPHICS)
#include <WebCore/CoordinatedCompositionReason.h>
#include <WebCore/CoordinatedTileCounter.h>
#include <atomic>
#include <wtf/Deque.h>
#include <wtf/HashSet.h>
#include <wtf/Vector.h>
#include <wtf/Lock.h>
#include <wtf/ThreadSafeRefCounted.h>

namespace WebCore {
class CoordinatedPlatformLayer;
}

namespace WebKit {

class CoordinatedSceneState final : public ThreadSafeRefCounted<CoordinatedSceneState> {
    WTF_MAKE_TZONE_ALLOCATED(CoordinatedSceneState);
public:
    static Ref<CoordinatedSceneState> create()
    {
        return adoptRef(*new CoordinatedSceneState());
    }
    ~CoordinatedSceneState();

    WebCore::CoordinatedPlatformLayer& rootLayer() const { return m_rootLayer.get(); }

    void setRootLayerChildren(Vector<Ref<WebCore::CoordinatedPlatformLayer>>&&);
    void addLayer(WebCore::CoordinatedPlatformLayer&);
    void removeLayer(WebCore::CoordinatedPlatformLayer&);

    bool flush();
    void flushPendingState();

    // Applying only the oldest transaction per composition shows every rendering update.
    enum class ApplyTransactions : bool { OldestReady, AllReady };
    using LayersWithPendingTileUpdates = Vector<Ref<WebCore::CoordinatedPlatformLayer>, 16>;
    LayersWithPendingTileUpdates applyLayerState(const OptionSet<WebCore::CompositionReason>&, ApplyTransactions);
    void processPendingTileUpdates(LayersWithPendingTileUpdates&&);
    // Applies all the transactions that are ready.
    void flushCompositingState(const OptionSet<WebCore::CompositionReason>&);
    void invalidate();

    void invalidateCommittedLayers();

    bool layersDidChange() const { return m_didChangeLayers; }

    void waitUntilPaintingComplete();

    void setDidPaintAllTilesTask(Ref<WebCore::CoordinatedTileCounter::DidPaintAllTilesTask>&&);
    Ref<WebCore::CoordinatedTileCounter> willPaintTile();
    bool hasPendingTiles() const;

private:
    CoordinatedSceneState();

    // Everything the main thread committed with one rendering update. flush() builds it on the main thread, and the
    // compositor applies it once all the tiles painted for it are done.
    struct Transaction {
        uint64_t id { 0 };
        RefPtr<WebCore::CoordinatedTileCounter> tileCounter;
        std::optional<HashSet<Ref<WebCore::CoordinatedPlatformLayer>>> layers;
        HashSet<Ref<WebCore::CoordinatedPlatformLayer>> layersToRemove;
        Vector<Ref<WebCore::CoordinatedPlatformLayer>> changedLayers;
    };
    bool firstTransactionHasPendingTiles() const WTF_REQUIRES_LOCK(m_transactionsLock);
    std::optional<Transaction> takeFirstReadyTransaction();
    void applyLayerSetChanges(Transaction&);
    void applyTransaction(Transaction&&);

    const Ref<WebCore::CoordinatedPlatformLayer> m_rootLayer;
    Lock m_layersLock;
    HashSet<Ref<WebCore::CoordinatedPlatformLayer>> m_layers WTF_GUARDED_BY_LOCK(m_layersLock);
    HashSet<Ref<WebCore::CoordinatedPlatformLayer>> m_layersToRemove;
    std::atomic<bool> m_didChangeLayers { false };
    HashSet<Ref<WebCore::CoordinatedPlatformLayer>> m_committedLayers;
    Lock m_stateLock;

    uint64_t m_lastTransactionID { 0 };
    mutable Lock m_transactionsLock;
    Deque<Transaction> m_transactions WTF_GUARDED_BY_LOCK(m_transactionsLock);

    // Tiles are counted per rendering update. m_tileCounter belongs to the update being built on the main thread,
    // and flush() hands it over with the transaction.
    RefPtr<WebCore::CoordinatedTileCounter::DidPaintAllTilesTask> m_didPaintAllTilesTask;
    Ref<WebCore::CoordinatedTileCounter> m_tileCounter;
    RefPtr<WebCore::CoordinatedTileCounter> m_lastCommittedTileCounter;
};

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)

