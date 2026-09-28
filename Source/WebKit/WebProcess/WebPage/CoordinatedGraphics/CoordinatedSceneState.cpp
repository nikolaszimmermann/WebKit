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

#include "config.h"
#include "CoordinatedSceneState.h"

#if USE(COORDINATED_GRAPHICS)
#include <WebCore/CoordinatedPlatformLayer.h>
#include <wtf/MainThread.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(CoordinatedSceneState);

CoordinatedSceneState::CoordinatedSceneState()
    : m_rootLayer(CoordinatedPlatformLayer::create())
    , m_tileCounter(CoordinatedTileCounter::create(nullptr))
{
    ASSERT(isMainRunLoop());
}

CoordinatedSceneState::~CoordinatedSceneState()
{
    ASSERT(m_layers.isEmpty());
    ASSERT(m_committedLayers.isEmpty());
}

void CoordinatedSceneState::setRootLayerChildren(Vector<Ref<CoordinatedPlatformLayer>>&& children)
{
    ASSERT(isMainRunLoop());

    {
        Locker locker { m_rootLayer->lock() };
        m_rootLayer->setChildren(WTF::move(children));
    }
    m_didChangeLayers = true;
}

void CoordinatedSceneState::addLayer(CoordinatedPlatformLayer& layer)
{
    ASSERT(isMainRunLoop());
    {
        Locker locker { m_layersLock };
        m_layers.add(layer);
    }
    m_didChangeLayers = true;
}

void CoordinatedSceneState::removeLayer(CoordinatedPlatformLayer& layer)
{
    ASSERT(isMainRunLoop());
    {
        Locker locker { m_layersLock };
        m_layers.remove(layer);
    }
    m_layersToRemove.add(layer);
    m_didChangeLayers = true;
}

std::optional<uint64_t> CoordinatedSceneState::flush(ForceTransaction forceTransaction)
{
    ASSERT(isMainRunLoop());

    Transaction transaction;
    transaction.id = ++m_lastTransactionID;

    bool didChangeLayers = m_didChangeLayers.exchange(false);
    if (didChangeLayers) {
        {
            Locker locker { m_layersLock };
            transaction.layers = m_layers;
        }
        transaction.layersToRemove = std::exchange(m_layersToRemove, { });
    }

    auto layers = this->layers();
    flushPendingState(layers);

#if !USE(TEXTURE_MAPPER)
    if (m_rootLayer->commitChanges(transaction.id))
        transaction.changedLayers.append(m_rootLayer);
    for (auto& layer : layers) {
        if (layer->commitChanges(transaction.id))
            transaction.changedLayers.append(layer);
    }
#endif

    transaction.viewportSize = std::exchange(m_viewportSize, std::nullopt);

    // Without the Skia compositor the layer changes aren't part of the transaction, but the compositor still has to wait
    // for the tiles painted during this rendering update.
    bool hasChanges = didChangeLayers || !transaction.changedLayers.isEmpty() || transaction.viewportSize || m_tileCounter->hasPendingTiles();
    if (!hasChanges && forceTransaction == ForceTransaction::No)
        return std::nullopt;

    if (hasChanges) {
        m_lastCommittedTileCounter = std::exchange(m_tileCounter, CoordinatedTileCounter::create(m_didPaintAllTilesTask.copyRef()));
        transaction.tileCounter = m_lastCommittedTileCounter;
    }

    Locker locker { m_transactionsLock };
    m_transactions.append(WTF::move(transaction));
    return m_lastTransactionID;
}

void CoordinatedSceneState::setViewportSize(const IntSize& size, float deviceScaleFactor)
{
    ASSERT(isMainRunLoop());
    m_viewportSize = ViewportSize { size, deviceScaleFactor };
}

Vector<Ref<CoordinatedPlatformLayer>> CoordinatedSceneState::layers() const
{
    Locker locker { m_layersLock };
    return copyToVector(m_layers);
}

void CoordinatedSceneState::flushPendingState()
{
    flushPendingState(layers());
}

void CoordinatedSceneState::flushPendingState(const Vector<Ref<CoordinatedPlatformLayer>>& layers)
{
    Locker stateLock { m_stateLock };
    for (auto& layer : layers)
        layer->flushPendingState();
}

void CoordinatedSceneState::applyLayerSetChanges(Transaction& transaction)
{
    for (auto& layer : transaction.layersToRemove)
        layer->invalidateTarget();

    if (transaction.layers)
        m_committedLayers = WTF::move(*transaction.layers);
}

void CoordinatedSceneState::applyTransaction(Transaction&& transaction)
{
    ASSERT(!isMainRunLoop());
    applyLayerSetChanges(transaction);

#if !USE(TEXTURE_MAPPER)
    for (auto& layer : transaction.changedLayers)
        layer->applyCommittedChanges(transaction.id);
#endif
}

bool CoordinatedSceneState::firstTransactionHasPendingTiles() const
{
    if (m_transactions.isEmpty())
        return false;

    auto& tileCounter = m_transactions.first().tileCounter;
    return tileCounter && tileCounter->hasPendingTiles();
}

auto CoordinatedSceneState::takeFirstReadyTransaction() -> std::optional<Transaction>
{
    // A transaction can only be applied once all the tiles painted for it are done, and the ones after it have to wait.
    Locker locker { m_transactionsLock };
    if (m_transactions.isEmpty() || firstTransactionHasPendingTiles())
        return std::nullopt;
    return m_transactions.takeFirst();
}

auto CoordinatedSceneState::applyLayerState(const OptionSet<CompositionReason>& reasons, ApplyTransactions applyTransactions) -> AppliedLayerState
{
    // Animations run on the compositor's own layers, so there is nothing to apply for them.
    if (reasons.hasExactlyOneBitSet() && reasons.contains(CompositionReason::Animation))
        return { };

    AppliedLayerState state;

    // Rendering updates apply the transactions whose tiles are painted, and the other compositions only apply the
    // changes made off the main thread.
    if (reasons.contains(CompositionReason::RenderingUpdate)) {
        while (auto transaction = takeFirstReadyTransaction()) {
            state.lastAppliedTransactionID = transaction->id;
            if (transaction->viewportSize)
                state.viewportSize = transaction->viewportSize;
            applyTransaction(WTF::move(*transaction));
            if (applyTransactions == ApplyTransactions::OldestReady)
                break;
        }
    }

    {
        Locker stateLock { m_stateLock };
        m_rootLayer->flushPositionChanges(reasons);
        for (auto& layer : m_committedLayers)
            layer->flushPositionChanges(reasons);
    }

    m_rootLayer->flushCompositingState(reasons);
    for (auto& layer : m_committedLayers) {
        layer->flushCompositingState(reasons);
        if (layer->hasPendingBackingStoreTileUpdates())
            state.layersWithPendingTileUpdates.append(Ref { layer });
    }
    return state;
}

void CoordinatedSceneState::processPendingTileUpdates(LayersWithPendingTileUpdates&& layers)
{
    for (auto& layer : layers)
        layer->processPendingBackingStoreTileUpdates();
}

void CoordinatedSceneState::flushCompositingState(const OptionSet<CompositionReason>& reasons)
{
    processPendingTileUpdates(applyLayerState(reasons, ApplyTransactions::AllReady).layersWithPendingTileUpdates);
}

void CoordinatedSceneState::invalidateCommittedLayers()
{
    ASSERT(!isMainRunLoop());
    Deque<Transaction> transactions;
    {
        Locker locker { m_transactionsLock };
        transactions = std::exchange(m_transactions, { });
    }
    for (auto& transaction : transactions)
        applyLayerSetChanges(transaction);

    m_rootLayer->invalidateTarget();
    while (!m_committedLayers.isEmpty()) {
        auto layer = m_committedLayers.takeAny();
        layer->invalidateTarget();
    }
}

void CoordinatedSceneState::invalidate()
{
    ASSERT(isMainRunLoop());
    // Root layer doesn't have client nor backing stores to invalidate.
    HashSet<Ref<CoordinatedPlatformLayer>> layers;
    {
        Locker locker { m_layersLock };
        layers = WTF::move(m_layers);
    }
    for (Ref layer : layers)
        layer->invalidateClient();

    Locker locker { m_transactionsLock };
    m_transactions.clear();
}

void CoordinatedSceneState::waitUntilPaintingComplete()
{
    ASSERT(isMainRunLoop());
#if USE(TEXTURE_MAPPER)
    for (auto& layer : layers())
        layer->waitUntilPaintingComplete();
#else
    if (m_lastCommittedTileCounter)
        m_lastCommittedTileCounter->waitUntilAllTilesArePainted();
#endif
}

void CoordinatedSceneState::setDidPaintAllTilesTask(Ref<CoordinatedTileCounter::DidPaintAllTilesTask>&& task)
{
    ASSERT(isMainRunLoop());
    ASSERT(!m_tileCounter->hasPendingTiles());
    m_didPaintAllTilesTask = WTF::move(task);
    m_tileCounter = CoordinatedTileCounter::create(m_didPaintAllTilesTask.copyRef());
}

Ref<CoordinatedTileCounter> CoordinatedSceneState::willPaintTile()
{
    ASSERT(isMainRunLoop());
    m_tileCounter->willPaintTile();
    return m_tileCounter;
}

bool CoordinatedSceneState::hasPendingTiles() const
{
    Locker locker { m_transactionsLock };
    return firstTransactionHasPendingTiles();
}

bool CoordinatedSceneState::hasQueuedTransactions() const
{
    Locker locker { m_transactionsLock };
    return !m_transactions.isEmpty();
}

bool CoordinatedSceneState::willApplyLastTransactionNext() const
{
    Locker locker { m_transactionsLock };
    return m_transactions.size() == 1 && !firstTransactionHasPendingTiles();
}

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)
