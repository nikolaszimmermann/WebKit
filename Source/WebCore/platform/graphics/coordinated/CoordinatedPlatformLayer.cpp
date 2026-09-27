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
#include "CoordinatedPlatformLayer.h"

#if USE(COORDINATED_GRAPHICS)
#include "CoordinatedAnimatedBackingStoreClient.h"
#include "CoordinatedBackingStoreProxy.h"
#include "CoordinatedImageBackingStore.h"
#include "CoordinatedPlatformLayerBuffer.h"
#include "CoordinatedPlatformLayerBufferHolePunch.h"
#include "CoordinatedPlatformLayerBufferVideo.h"
#include "CoordinatedTileBuffer.h"
#include "CoordinatedTileCounter.h"
#include "GraphicsContext.h"
#include "GraphicsLayerCoordinated.h"
#include "NativeImage.h"
#include <wtf/MainThread.h>

#if USE(TEXTURE_MAPPER)
#include "CoordinatedBackingStore.h"
#include "TextureMapperLayer.h"
#endif

#if USE(SKIA)
#include "SkiaCompositingLayer.h"
#include "SkiaPaintingEngine.h"
#endif

namespace WebCore {

#if !USE(TEXTURE_MAPPER)
// The layer changes committed with one rendering update, see commitChanges().
struct CoordinatedPlatformLayer::CommittedChanges {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(CommittedChanges);
public:
    uint64_t transactionID { 0 };
    EnumSet<Change> changes;
    PositionUpdates positionUpdates;
    // Most rendering updates only repaint a layer, so its other properties are only copied when one of them changed.
    std::unique_ptr<CommittedProperties> properties;
    std::optional<CoordinatedBackingStoreProxy::Update> tileUpdate;
    float contentsScale { 1 };
    std::unique_ptr<CoordinatedPlatformLayerBuffer> contentsBuffer;
#if ENABLE(DAMAGE_TRACKING)
    std::optional<Damage> damage;
#endif
};

struct CoordinatedPlatformLayer::CommittedProperties {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(CommittedProperties);
public:
    FloatPoint3D anchorPoint;
    FloatSize size;
    TransformationMatrix transform;
    TransformationMatrix childrenTransform;
    bool masksToBounds { false };
    bool preserves3D { false };
    bool backfaceVisibility { true };
    Color backgroundColor;
    float opacity { 1 };
    BlendMode blendMode { BlendMode::Normal };
    bool contentsVisible { true };
    bool contentsOpaque { false };
    FloatRect contentsRect;
    bool contentsRectClipsDescendants { false };
    FloatRoundedRect contentsClippingRect;
    Path contentsClipShapePath;
    Color contentsColor;
    FloatSize contentsTileSize;
    FloatSize contentsTilePhase;
    RefPtr<CoordinatedImageBackingStore> imageBackingStore;
    Path clipPath;
    WindRule clipPathWindRule { WindRule::NonZero };
    FilterOperations filters;
    RefPtr<CoordinatedPlatformLayer> mask;
    RefPtr<CoordinatedPlatformLayer> replica;
    FilterOperations backdropFilters;
    FloatRoundedRect backdropRect;
    Path backdropShapePath;
    bool isBackdropRoot { false };
    AcceleratedAnimations animations;
    Color debugBorderColor;
    float debugBorderWidth { 0 };
    int repaintCount { -1 };
    Vector<Ref<CoordinatedPlatformLayer>> children;
    bool hasBackingStore { false };
    RefPtr<CoordinatedAnimatedBackingStoreClient> animatedBackingStoreClient;
};
#endif

Ref<CoordinatedPlatformLayer> CoordinatedPlatformLayer::create(Client& client)
{
    return adoptRef(*new CoordinatedPlatformLayer(&client));
}

Ref<CoordinatedPlatformLayer> CoordinatedPlatformLayer::create()
{
    return adoptRef(*new CoordinatedPlatformLayer(nullptr));
}

CoordinatedPlatformLayer::CoordinatedPlatformLayer(Client* client)
    : m_client(client)
    , m_id(PlatformLayerIdentifier::generate())
#if !USE(TEXTURE_MAPPER)
    , m_threadSafeGrContext(m_client ? m_client->paintingEngine().threadSafeGrContext() : nullptr)
#endif
{
    ASSERT(isMainThread());
}

CoordinatedPlatformLayer::~CoordinatedPlatformLayer() = default;

void CoordinatedPlatformLayer::setOwner(GraphicsLayerCoordinated* owner)
{
    assertIsMainThread();
    if (m_owner == owner)
        return;

    m_owner = owner;
    if (!m_client)
        return;

    if (m_owner)
        m_client->attachLayer(*this);
    else {
        purgeBackingStores();
        m_client->detachLayer(*this);
    }
}

GraphicsLayerCoordinated* CoordinatedPlatformLayer::owner() const
{
    assertIsMainThread();
    return m_owner;
}

#if USE(TEXTURE_MAPPER)
TextureMapperLayer& CoordinatedPlatformLayer::ensureTarget()
{
    ASSERT(!isMainThread());
    if (!m_target) {
        m_target = makeUnique<TextureMapperLayer>();
#if ENABLE(DAMAGE_TRACKING)
        m_target->setDamagePropagationEnabled(m_damagePropagationEnabled);
        if (m_damagePropagationEnabled)
            m_target->setDamageInGlobalCoordinateSpace(m_damageInGlobalCoordinateSpace);
#endif
    }
    return *m_target;
}

#else

SkiaCompositingLayer& CoordinatedPlatformLayer::ensureTarget()
{
    ASSERT(!isMainThread());
    if (!m_target)
        m_target = SkiaCompositingLayer::create();
#if ENABLE(DAMAGE_TRACKING)
    m_target->setDamagePropagationEnabled(m_damagePropagationEnabled);
#endif
    return *m_target;
}
#endif

static bool shouldReleaseBuffer(CoordinatedPlatformLayerBuffer* buffer)
{
#if ENABLE(VIDEO)
    // Do not release hole punch buffers early. See https://bugs.webkit.org/show_bug.cgi?id=267322.
    if (is<CoordinatedPlatformLayerBufferHolePunch>(buffer))
        return false;
#else
    UNUSED_PARAM(buffer);
#endif

    return true;
}

void CoordinatedPlatformLayer::invalidateTarget()
{
    ASSERT(!isMainThread());
    {
        Locker locker { m_lock };
#if USE(TEXTURE_MAPPER)
        m_backingStore = nullptr;
#endif
        m_imageBackingStore.committed = nullptr;
        if (m_target && shouldReleaseBuffer(m_contentsBuffer.committed.get()))
            m_contentsBuffer.committed = nullptr;
#if !USE(TEXTURE_MAPPER)
        if (m_target && !shouldReleaseBuffer(m_target->contentsBuffer()))
            m_contentsBuffer.committed = m_target->takeContentsBuffer();
#endif
        m_contentsBuffer.hasCommitted = false;
#if !USE(TEXTURE_MAPPER)
        m_committedChanges.clear();
#endif
    }
#if USE(TEXTURE_MAPPER)
    m_target = nullptr;
#else
    if (m_target) {
        m_target->invalidate();
        m_target = nullptr;
    }
#endif
}

void CoordinatedPlatformLayer::invalidateClient()
{
    ASSERT(isMainThread());
    purgeBackingStores();
    m_client = nullptr;
}

void CoordinatedPlatformLayer::notifyCompositionRequired()
{
    if (!m_client)
        return;
    m_client->notifyCompositionRequired();
}

auto CoordinatedPlatformLayer::makePositionUpdate(const FloatPoint& value) -> PositionUpdate
{
    assertIsHeld(m_lock);
    return { value, ++m_lastPositionUpdateGeneration, isMainThread() };
}

void CoordinatedPlatformLayer::setPosition(FloatPoint&& position)
{
    assertIsHeld(m_lock);
    m_pendingState.position = makePositionUpdate(position);
}

void CoordinatedPlatformLayer::setPositionForScrolling(const FloatPoint& position)
{
    Locker locker { m_lock };
    m_pendingState.positionForScrolling = makePositionUpdate(position);
}

const FloatPoint& CoordinatedPlatformLayer::position() const
{
    assertIsHeld(m_lock);
    return m_position;
}

void CoordinatedPlatformLayer::setTopLeftPositionForScrolling(const FloatPoint& position)
{
    FloatPoint newPosition;
    {
        Locker locker { m_lock };
        newPosition = { position.x() + m_anchorPoint.x() * m_size.width(), position.y() + m_anchorPoint.y() * m_size.height() };
    }
    setPositionForScrolling(newPosition);
}

FloatPoint CoordinatedPlatformLayer::topLeftPositionForScrolling()
{
    Locker locker { m_lock };
    return m_position - toFloatSize(m_anchorPoint.xy()) * m_size;
}

void CoordinatedPlatformLayer::setBoundsOrigin(const FloatPoint& origin)
{
    assertIsHeld(m_lock);
    m_pendingState.boundsOrigin = makePositionUpdate(origin);
}

void CoordinatedPlatformLayer::setBoundsOriginForScrolling(const FloatPoint& origin)
{
    Locker locker { m_lock };
    m_pendingState.boundsOriginForScrolling = makePositionUpdate(origin);
}

const FloatPoint& CoordinatedPlatformLayer::boundsOrigin() const
{
    assertIsHeld(m_lock);
    return m_boundsOrigin;
}

void CoordinatedPlatformLayer::setAnchorPoint(FloatPoint3D&& point)
{
    assertIsHeld(m_lock);
    if (m_anchorPoint == point)
        return;

    m_anchorPoint = WTF::move(point);
    m_pendingChanges.add(Change::AnchorPoint);
    notifyCompositionRequired();
}

const FloatPoint3D& CoordinatedPlatformLayer::anchorPoint() const
{
    assertIsHeld(m_lock);
    return m_anchorPoint;
}

void CoordinatedPlatformLayer::setSize(FloatSize&& size)
{
    assertIsHeld(m_lock);
    if (m_size == size)
        return;

    m_size = WTF::move(size);
    m_pendingChanges.add(Change::Size);
    notifyCompositionRequired();
}

const FloatSize& CoordinatedPlatformLayer::size() const
{
    assertIsHeld(m_lock);
    return m_size;
}

FloatRect CoordinatedPlatformLayer::bounds() const
{
    assertIsHeld(m_lock);
    return FloatRect({ }, m_size);
}

void CoordinatedPlatformLayer::setTransform(const TransformationMatrix& matrix)
{
    assertIsHeld(m_lock);
    if (m_transform == matrix)
        return;

    m_transform = matrix;
    m_pendingChanges.add(Change::Transform);
    notifyCompositionRequired();
}

const TransformationMatrix& CoordinatedPlatformLayer::transform() const
{
    assertIsHeld(m_lock);
    return m_transform;
}

void CoordinatedPlatformLayer::setChildrenTransform(const TransformationMatrix& matrix)
{
    assertIsHeld(m_lock);
    if (m_childrenTransform == matrix)
        return;

    m_childrenTransform = matrix;
    m_pendingChanges.add(Change::ChildrenTransform);
    notifyCompositionRequired();
}

const TransformationMatrix& CoordinatedPlatformLayer::childrenTransform() const
{
    assertIsHeld(m_lock);
    return m_childrenTransform;
}

void CoordinatedPlatformLayer::didUpdateLayerTransform()
{
    assertIsMainThread();
    m_needsTilesUpdate = true;
}

void CoordinatedPlatformLayer::setVisibleRect(const FloatRect& visibleRect)
{
    assertIsMainThread();
    if (m_visibleRect == visibleRect)
        return;

    m_visibleRect = visibleRect;
}

void CoordinatedPlatformLayer::setTransformedVisibleRect(IntRect&& transformedVisibleRect)
{
    assertIsMainThread();
    if (m_transformedVisibleRect == transformedVisibleRect)
        return;

    m_transformedVisibleRect = WTF::move(transformedVisibleRect);
    m_needsTilesUpdate = true;
}

#if ENABLE(SCROLLING_THREAD)
void CoordinatedPlatformLayer::setScrollingNodeID(std::optional<ScrollingNodeID> nodeID)
{
    assertIsHeld(m_lock);
    m_scrollingNodeID = nodeID;
}

const Markable<ScrollingNodeID>& CoordinatedPlatformLayer::scrollingNodeID() const
{
    assertIsHeld(m_lock);
    return m_scrollingNodeID;
}
#endif

void CoordinatedPlatformLayer::setDrawsContent(bool drawsContent)
{
    assertIsMainThread();
    m_drawsContent = drawsContent;
}

void CoordinatedPlatformLayer::setMasksToBounds(bool masksToBounds)
{
    assertIsHeld(m_lock);
    if (m_masksToBounds == masksToBounds)
        return;

    m_masksToBounds = masksToBounds;
    m_pendingChanges.add(Change::MasksToBounds);
    damageWholeLayer();
    notifyCompositionRequired();
}

bool CoordinatedPlatformLayer::masksToBounds() const
{
    assertIsHeld(m_lock);
    return m_masksToBounds;
}

void CoordinatedPlatformLayer::setPreserves3D(bool preserves3D)
{
    assertIsHeld(m_lock);
    if (m_preserves3D == preserves3D)
        return;

    m_preserves3D = preserves3D;
    m_pendingChanges.add(Change::Preserves3D);
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setBackfaceVisibility(bool backfaceVisibility)
{
    assertIsHeld(m_lock);
    if (m_backfaceVisibility == backfaceVisibility)
        return;

    m_backfaceVisibility = backfaceVisibility;
    m_pendingChanges.add(Change::BackfaceVisibility);
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setBackgroundColor(const Color& backgroundColor)
{
    assertIsHeld(m_lock);
    if (m_backgroundColor == backgroundColor)
        return;

    m_backgroundColor = backgroundColor;
    m_pendingChanges.add(Change::BackgroundColor);
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setOpacity(float opacity)
{
    assertIsHeld(m_lock);
    if (m_opacity == opacity)
        return;

    m_opacity = opacity;
    m_pendingChanges.add(Change::Opacity);
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setBlendMode(BlendMode blendMode)
{
    assertIsHeld(m_lock);
    if (m_blendMode == blendMode)
        return;

    m_blendMode = blendMode;
    m_pendingChanges.add(Change::BlendMode);
    damageWholeLayer();
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setContentsVisible(bool contentsVisible)
{
    assertIsHeld(m_lock);
    if (m_contentsVisible == contentsVisible)
        return;

    m_contentsVisible = contentsVisible;
    m_pendingChanges.add(Change::ContentsVisible);
    damageWholeLayer();
    notifyCompositionRequired();
}

bool CoordinatedPlatformLayer::contentsVisible() const
{
    assertIsHeld(m_lock);
    return m_contentsVisible;
}

void CoordinatedPlatformLayer::setContentsOpaque(bool contentsOpaque)
{
    assertIsHeld(m_lock);
    if (m_contentsOpaque == contentsOpaque)
        return;

    m_contentsOpaque = contentsOpaque;
    m_pendingChanges.add(Change::ContentsOpaque);
    // FIXME: request a full repaint?
    damageWholeLayer();
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setContentsRect(const FloatRect& contentsRect)
{
    assertIsHeld(m_lock);
    assertCanChangeContentsForRenderingUpdate();
    if (m_contentsRect == contentsRect)
        return;

    m_contentsRect = contentsRect;
    m_pendingChanges.add(Change::ContentsRect);
    damageWholeLayer();
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setContentsRectClipsDescendants(bool contentsRectClipsDescendants)
{
    assertIsHeld(m_lock);
    if (m_contentsRectClipsDescendants == contentsRectClipsDescendants)
        return;

    m_contentsRectClipsDescendants = contentsRectClipsDescendants;
    m_pendingChanges.add(Change::ContentsRectClipsDescendants);
    damageWholeLayer();
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setContentsClippingRect(const FloatRoundedRect& contentsClippingRect)
{
    assertIsHeld(m_lock);
    assertCanChangeContentsForRenderingUpdate();
    if (m_contentsClippingRect == contentsClippingRect)
        return;

    m_contentsClippingRect = contentsClippingRect;
    m_pendingChanges.add(Change::ContentsClippingRect);
    damageWholeLayer();
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setContentsClipShapePath(const Path& path)
{
    assertIsHeld(m_lock);
    if (m_contentsClipShapePath.definitelyEqual(path))
        return;

    m_contentsClipShapePath = path;
    m_pendingChanges.add(Change::ContentsClipShapePath);
    damageWholeLayer();
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setContentsScale(float contentsScale)
{
    assertIsMainThread();
    assertIsHeld(m_lock);
    if (m_contentsScale == contentsScale)
        return;

    m_contentsScale = contentsScale;
    m_needsTilesUpdate = true;
    notifyCompositionRequired();
}

float CoordinatedPlatformLayer::contentsScale() const
{
    assertIsHeld(m_lock);
    return m_contentsScale;
}

void CoordinatedPlatformLayer::setContentsBuffer(std::unique_ptr<CoordinatedPlatformLayerBuffer>&& buffer, std::optional<Damage>&& dirtyRegion, RequireComposition requireComposition)
{
    assertIsHeld(m_lock);
    assertCanChangeContentsForRenderingUpdate();
#if !USE(TEXTURE_MAPPER)
    if (!buffer && !m_contentsBuffer.pending && !m_contentsBuffer.hasCommitted && !m_asyncState.contentsBuffer)
        return;

    // A video frame that is still waiting to be applied is older than this buffer, so it must not replace it.
    dropAsyncContentsBuffer();
#else
    if (!buffer && !m_contentsBuffer.pending && !m_contentsBuffer.hasCommitted)
        return;
#endif

    m_contentsBuffer.pending = WTF::move(buffer);
    m_pendingChanges.add(Change::ContentsBuffer);
#if ENABLE(DAMAGE_TRACKING)
    if (dirtyRegion)
        addDamage(WTF::move(*dirtyRegion));
    else
        damageWholeLayer();
#else
    UNUSED_PARAM(dirtyRegion);
#endif
    if (requireComposition == RequireComposition::Yes)
        notifyCompositionRequired();
}

#if USE(TEXTURE_MAPPER)
void CoordinatedPlatformLayer::setAsyncContentsRects(const FloatRect& contentsRect, const FloatRoundedRect& contentsClippingRect)
{
    // The TextureMapper compositor already applies these with the next composition.
    setContentsRect(contentsRect);
    setContentsClippingRect(contentsClippingRect);
}

void CoordinatedPlatformLayer::setAsyncContentsBuffer(std::unique_ptr<CoordinatedPlatformLayerBuffer>&& buffer, RequireComposition requireComposition)
{
    setContentsBuffer(WTF::move(buffer), std::nullopt, requireComposition);
}
#else
void CoordinatedPlatformLayer::setAsyncContentsRects(const FloatRect& contentsRect, const FloatRoundedRect& contentsClippingRect)
{
    assertIsHeld(m_lock);
    if (m_asyncState.contentsRect == contentsRect && m_asyncState.contentsClippingRect == contentsClippingRect)
        return;

    if (m_asyncState.contentsRect != contentsRect) {
        m_asyncState.contentsRect = contentsRect;
        m_asyncState.changes.add(Change::ContentsRect);
    }

    if (m_asyncState.contentsClippingRect != contentsClippingRect) {
        m_asyncState.contentsClippingRect = contentsClippingRect;
        m_asyncState.changes.add(Change::ContentsClippingRect);
    }

    damageWholeLayerAsync();
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setAsyncContentsBuffer(std::unique_ptr<CoordinatedPlatformLayerBuffer>&& buffer, RequireComposition requireComposition)
{
    assertIsHeld(m_lock);
    if (!buffer && !m_asyncState.contentsBuffer && !m_contentsBuffer.hasCommitted)
        return;

    m_asyncState.contentsBuffer = WTF::move(buffer);
    m_asyncState.changes.add(Change::ContentsBuffer);
    damageWholeLayerAsync();

    if (requireComposition == RequireComposition::Yes)
        notifyCompositionRequired();
}

void CoordinatedPlatformLayer::dropAsyncContentsBuffer()
{
    assertIsHeld(m_lock);
    m_asyncState.contentsBuffer = nullptr;
    m_asyncState.changes.remove(Change::ContentsBuffer);
}
#endif

#if ENABLE(VIDEO) && USE(GSTREAMER_GL)
void CoordinatedPlatformLayer::replaceCurrentContentsBufferWithCopy()
{
    Locker locker { m_lock };
    if (!m_contentsBuffer.hasCommitted)
        return;

    m_contentsBuffer.pending = nullptr;

#if USE(TEXTURE_MAPPER)
    if (is<CoordinatedPlatformLayerBufferVideo>(*m_contentsBuffer.committed))
        m_contentsBuffer.pending = downcast<CoordinatedPlatformLayerBufferVideo>(*m_contentsBuffer.committed).copyBuffer();
    m_contentsBuffer.committed = WTF::move(m_contentsBuffer.pending);
    m_contentsBuffer.hasCommitted = !!m_contentsBuffer.committed;
    ensureTarget().setContentsLayer(m_contentsBuffer.committed.get());
#else
    dropAsyncContentsBuffer();

    if (!m_target)
        return;

    if (auto* buffer = m_target->contentsBuffer()) {
        if (is<CoordinatedPlatformLayerBufferVideo>(*buffer))
            m_contentsBuffer.pending = downcast<CoordinatedPlatformLayerBufferVideo>(*buffer).copyBuffer();
        m_contentsBuffer.hasCommitted = !!m_contentsBuffer.pending;
        m_target->setContentsBuffer(WTF::move(m_contentsBuffer.pending));
    }
#endif
}
#endif

void CoordinatedPlatformLayer::setContentsImage(NativeImage* image)
{
    assertIsHeld(m_lock);
    if (image) {
        if (m_imageBackingStore.current && m_imageBackingStore.current->isSameNativeImage(*image))
            return;

        ASSERT(m_client);
        m_imageBackingStore.current = m_client->imageBackingStore(Ref { *image });
    } else {
        if (!m_imageBackingStore.current)
            return;
        m_imageBackingStore.current = nullptr;
    }
    m_pendingChanges.add(Change::ContentsImage);
    damageWholeLayer();
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setContentsColor(const Color& color)
{
    assertIsHeld(m_lock);
    if (m_contentsColor == color)
        return;

    m_contentsColor = color;
    m_pendingChanges.add(Change::ContentsColor);
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setContentsTileSize(const FloatSize& contentsTileSize)
{
    assertIsHeld(m_lock);
    if (m_contentsTileSize == contentsTileSize)
        return;

    m_contentsTileSize = contentsTileSize;
    m_pendingChanges.add(Change::ContentsTiling);
    damageWholeLayer();
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setContentsTilePhase(const FloatSize& contentsTilePhase)
{
    assertIsHeld(m_lock);
    if (m_contentsTilePhase == contentsTilePhase)
        return;

    m_contentsTilePhase = contentsTilePhase;
    m_pendingChanges.add(Change::ContentsTiling);
    damageWholeLayer();
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setDirtyRegion(Damage&& damage)
{
    assertIsMainThread();
    assertIsHeld(m_lock);
    auto dirtyRegion = damage.rects();
    if (m_dirtyRegion != dirtyRegion) {
        m_dirtyRegion = WTF::move(dirtyRegion);
        notifyCompositionRequired();
    }

#if ENABLE(DAMAGE_TRACKING)
    addDamage(WTF::move(damage));
#endif
}

void CoordinatedPlatformLayer::assertCanChangeContentsForRenderingUpdate() const
{
#if !USE(TEXTURE_MAPPER)
    // Contents changed off the main thread must use the async setters.
    ASSERT(isMainThread());
#endif
}

#if ENABLE(DAMAGE_TRACKING)
static void accumulateDamage(std::optional<Damage>& accumulatedDamage, Damage&& damage)
{
    if (!accumulatedDamage)
        accumulatedDamage = WTF::move(damage);
    else
        accumulatedDamage->add(damage);
}

void CoordinatedPlatformLayer::addDamage(Damage&& damage)
{
    assertIsHeld(m_lock);
    assertCanChangeContentsForRenderingUpdate();
    accumulateDamage(m_damage, WTF::move(damage));
    m_pendingChanges.add(Change::Damage);
}

std::optional<Damage> CoordinatedPlatformLayer::wholeLayerDamage() const
{
    assertIsHeld(m_lock);
    // An empty Damage rejects everything added to it later, so it must never become the layer's damage.
    if (!m_damagePropagationEnabled || m_size.isEmpty())
        return std::nullopt;

    return Damage { m_size, Damage::Mode::Full };
}
#endif

void CoordinatedPlatformLayer::damageWholeLayer()
{
#if ENABLE(DAMAGE_TRACKING)
    assertIsHeld(m_lock);
    if (auto damage = wholeLayerDamage())
        addDamage(WTF::move(*damage));
#endif
}

#if !USE(TEXTURE_MAPPER)
void CoordinatedPlatformLayer::damageWholeLayerAsync()
{
#if ENABLE(DAMAGE_TRACKING)
    assertIsHeld(m_lock);
    if (auto damage = wholeLayerDamage()) {
        accumulateDamage(m_asyncState.damage, WTF::move(*damage));
        m_asyncState.changes.add(Change::Damage);
    }
#endif
}
#endif

void CoordinatedPlatformLayer::setFilters(const FilterOperations& filters)
{
    assertIsHeld(m_lock);
    if (m_filters == filters)
        return;

    m_filters = filters;
    m_pendingChanges.add(Change::Filters);
    damageWholeLayer();
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setMask(CoordinatedPlatformLayer* mask)
{
    assertIsHeld(m_lock);
    if (m_mask == mask)
        return;

    m_mask = mask;
    m_pendingChanges.add(Change::Mask);
    damageWholeLayer();
    notifyCompositionRequired();
}

CoordinatedPlatformLayer* CoordinatedPlatformLayer::mask() const
{
    assertIsHeld(m_lock);
    return m_mask;
}

void CoordinatedPlatformLayer::setReplica(CoordinatedPlatformLayer* replica)
{
    assertIsHeld(m_lock);
    if (m_replica == replica)
        return;

    m_replica = replica;
    m_pendingChanges.add(Change::Replica);
    damageWholeLayer();
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setBackdrop(CoordinatedPlatformLayer* backdrop)
{
    assertIsHeld(m_lock);
    if (m_backdrop == backdrop)
        return;

    m_backdrop = backdrop;
    notifyBackdropFiltersChanged();
}

void CoordinatedPlatformLayer::notifyBackdropFiltersChanged()
{
    assertIsHeld(m_lock);
    m_pendingChanges.add(Change::Backdrop);
    damageWholeLayer();
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setBackdropRect(const FloatRoundedRect& backdropRect)
{
    assertIsHeld(m_lock);
    if (m_backdropRect == backdropRect)
        return;

    m_backdropRect = backdropRect;
    m_pendingChanges.add(Change::BackdropRect);
    damageWholeLayer();
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setBackdropShapePath(const Path& path)
{
    assertIsHeld(m_lock);
    if (m_backdropShapePath.definitelyEqual(path))
        return;

    m_backdropShapePath = path;
    m_pendingChanges.add(Change::BackdropShapePath);
    damageWholeLayer();
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setIsBackdropRoot(bool isBackdropRoot)
{
    assertIsHeld(m_lock);
    if (m_isBackdropRoot == isBackdropRoot)
        return;

    m_isBackdropRoot = isBackdropRoot;
    m_pendingChanges.add(Change::BackdropRoot);
    notifyCompositionRequired();
}

#if USE(TEXTURE_MAPPER)
void CoordinatedPlatformLayer::setAnimations(const TextureMapperAnimations& animations)
#else
void CoordinatedPlatformLayer::setAnimations(const AcceleratedAnimations& animations)
#endif
{
    assertIsHeld(m_lock);
    m_animations = animations;
    m_pendingChanges.add(Change::Animations);
    notifyCompositionRequired();
}

RefPtr<CoordinatedPlatformLayer> CoordinatedPlatformLayer::parent() const
{
    assertIsHeld(m_lock);
    return m_parent;
}

void CoordinatedPlatformLayer::setChildren(Vector<Ref<CoordinatedPlatformLayer>>&& children)
{
    assertIsHeld(m_lock);
    if (m_children == children)
        return;

    while (!m_children.isEmpty()) {
        auto child = m_children.takeLast();
        Locker childLocker { child->m_lock };
        child->m_parent = nullptr;
    }

    m_children = WTF::move(children);

    for (auto& child : m_children) {
        Locker childLocker { child->m_lock };
        child->removeFromParent();
        child->m_parent = this;
    }

    m_pendingChanges.add(Change::Children);
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::removeFromParent()
{
    assertIsHeld(m_lock);
    RefPtr parent = std::exchange(m_parent, nullptr);
    if (!parent)
        return;

    Locker parentLocker { parent->m_lock };

    parent->m_children.removeFirstMatching([this](auto& layer) {
        return layer.ptr() == this;
    });
}

const Vector<Ref<CoordinatedPlatformLayer>>& CoordinatedPlatformLayer::children() const
{
    assertIsHeld(m_lock);
    return m_children;
}

void CoordinatedPlatformLayer::setEventRegion(const EventRegion& eventRegion)
{
    assertIsHeld(m_lock);
    m_eventRegion = eventRegion;
}

const EventRegion& CoordinatedPlatformLayer::eventRegion() const
{
    assertIsHeld(m_lock);
    return m_eventRegion;
}

void CoordinatedPlatformLayer::setClipPath(const Path& path, WindRule windRule)
{
    assertIsHeld(m_lock);
    m_clipPath.path = path;
    m_clipPath.windRule = windRule;
    m_pendingChanges.add(Change::ClipPath);
    damageWholeLayer();
}

void CoordinatedPlatformLayer::setDebugBorder(Color&& borderColor, float borderWidth)
{
    assertIsHeld(m_lock);
    if (m_debugBorderColor == borderColor && m_debugBorderWidth == borderWidth)
        return;

    m_debugBorderColor = WTF::move(borderColor);
    m_debugBorderWidth = borderWidth;
    m_pendingChanges.add(Change::DebugIndicators);
    notifyCompositionRequired();
}

void CoordinatedPlatformLayer::setShowRepaintCounter(bool showRepaintCounter)
{
    assertIsMainThread();
    assertIsHeld(m_lock);
    if ((m_repaintCount != -1 && showRepaintCounter) || (m_repaintCount == -1 && !showRepaintCounter))
        return;

    m_repaintCount = showRepaintCounter ? m_owner->repaintCount() : -1;
    m_pendingChanges.add(Change::DebugIndicators);
    notifyCompositionRequired();
}

bool CoordinatedPlatformLayer::needsBackingStore() const
{
    assertIsMainThread();
    assertIsHeld(m_lock);
    if (!m_owner)
        return false;

    if (!m_drawsContent || !m_contentsVisible || m_size.isEmpty())
        return false;

    // If the CSS opacity value is 0 and there's no animation over the opacity property, the layer is invisible.
    if (!m_opacity && !m_animations.hasActiveAnimationsOfType(AnimatedProperty::Opacity))
        return false;

    // Check if there's a filter that sets the opacity to zero.
    bool hasOpacityZeroFilter = std::ranges::any_of(m_filters, [](auto& operation) {
        return operation->type() == FilterOperation::Type::Opacity && !downcast<BasicComponentTransferFilterOperation>(operation.get()).amount();
    });

    return !hasOpacityZeroFilter;
}

void CoordinatedPlatformLayer::updateBackingStore()
{
    assertIsMainThread();

    if (m_dirtyRegion.isEmpty() && !m_pendingTilesCreation && !m_needsTilesUpdate)
        return;

    FloatSize size;
    float contentsScale;
    bool contentsOpaque;
    RefPtr<CoordinatedBackingStoreProxy> backingStoreProxy;
    {
        Locker locker { m_lock };
        if (!m_backingStoreProxy)
            return;

        size = m_size;
        contentsScale = m_contentsScale;
        contentsOpaque = m_contentsOpaque;
        backingStoreProxy = m_backingStoreProxy;
    }

    Damage damage(size, Damage::Mode::Rectangles);
    auto updateResult = backingStoreProxy->updateIfNeeded(m_transformedVisibleRect, size, m_visibleRect, contentsScale, contentsOpaque, m_pendingTilesCreation || m_needsTilesUpdate, m_dirtyRegion, damage, *this);
    m_dirtyRegion.clear();
    m_needsTilesUpdate = false;
    m_pendingTilesCreation = updateResult.contains(CoordinatedBackingStoreProxy::UpdateResult::TilesPending);

    bool tilesChanged = updateResult.contains(CoordinatedBackingStoreProxy::UpdateResult::TilesChanged);
    {
        Locker locker { m_lock };
#if ENABLE(DAMAGE_TRACKING)
        addDamage(WTF::move(damage));
#endif

        if (tilesChanged) {
            if (m_repaintCount != -1 && updateResult.contains(CoordinatedBackingStoreProxy::UpdateResult::BuffersChanged)) {
                m_repaintCount = m_owner->incrementRepaintCount();
                m_pendingChanges.add(Change::DebugIndicators);
            }
        }
    }

    if (tilesChanged)
        notifyCompositionRequired();
}

void CoordinatedPlatformLayer::updateContents(bool affectedByTransformAnimation)
{
    assertIsMainThread();
    assertIsHeld(m_lock);

    if (needsBackingStore()) {
        if (!m_backingStoreProxy) {
            m_backingStoreProxy = CoordinatedBackingStoreProxy::create();
            m_backingStoreProxy->setAffectedByTransformAnimation(affectedByTransformAnimation);
            m_needsTilesUpdate = true;
            m_pendingChanges.add(Change::BackingStore);
        } else {
            bool wasAffectedByTransformAnimation = !!m_backingStoreProxy->animatedBackingStoreClient();
            if (wasAffectedByTransformAnimation != affectedByTransformAnimation) {
                m_backingStoreProxy->setAffectedByTransformAnimation(affectedByTransformAnimation);
                m_pendingChanges.add(Change::BackingStore);
            }
        }
    } else {
        if (m_backingStoreProxy) {
            m_backingStoreProxy->invalidate();
            m_backingStoreProxy = nullptr;
            m_pendingChanges.add(Change::BackingStore);
        }
    }

    if (m_backdrop) {
        Locker locker { m_backdrop->lock() };
        m_backdrop->updateContents(affectedByTransformAnimation);
    }
}

void CoordinatedPlatformLayer::purgeBackingStores()
{
    Locker locker { m_lock };
    if (m_backingStoreProxy) {
        m_backingStoreProxy->invalidate();
        m_backingStoreProxy = nullptr;
    }
    m_imageBackingStore.current = nullptr;
    if (shouldReleaseBuffer(m_contentsBuffer.pending.get()))
        m_contentsBuffer.pending = nullptr;
#if !USE(TEXTURE_MAPPER)
    if (shouldReleaseBuffer(m_asyncState.contentsBuffer.get()))
        m_asyncState.contentsBuffer = nullptr;
#endif
}

bool CoordinatedPlatformLayer::isCompositionRequiredOrOngoing() const
{
    return m_client ? m_client->isCompositionRequiredOrOngoing() : false;
}

void CoordinatedPlatformLayer::requestComposition(CompositionReason reason)
{
    if (m_client)
        m_client->requestComposition(reason);
}

RunLoop* CoordinatedPlatformLayer::compositingRunLoop() const
{
    return m_client ? m_client->compositingRunLoop() : nullptr;
}

int CoordinatedPlatformLayer::maxTextureSize() const
{
    return m_client ? m_client->maxTextureSize() : 0;
}

Ref<CoordinatedTileCounter> CoordinatedPlatformLayer::willPaintTile()
{
    ASSERT(isMainThread());
    ASSERT(m_client);
    return m_client->willPaintTile();
}

void CoordinatedPlatformLayer::waitUntilPaintingComplete()
{
    Locker locker { m_lock };
    if (m_backingStoreProxy)
        m_backingStoreProxy->waitUntilPaintingComplete();
}

void CoordinatedPlatformLayer::flushPendingState()
{
    Locker locker { m_lock };
    if (!m_pendingState.position && !m_pendingState.boundsOrigin && !m_pendingState.positionForScrolling && !m_pendingState.boundsOriginForScrolling)
        return;

    // Positions set on the main thread belong to its current rendering update, so only the main thread takes them, and
    // they are applied together with the rest of that rendering update. Positions set by the scrolling thread are
    // applied with the next composition, no matter which thread takes them.
    bool canTakePositionsForRenderingUpdate = isMainThread();
    bool requiresComposition = false;
    auto takeUpdate = [&](std::optional<PositionUpdate>& pendingUpdate, FloatPoint& value, uint64_t& generation, std::optional<PositionUpdate>& updateForRenderingUpdate, std::optional<PositionUpdate>& updateForScrolling) {
        if (!pendingUpdate || (pendingUpdate->isForRenderingUpdate && !canTakePositionsForRenderingUpdate))
            return;

        auto update = *std::exchange(pendingUpdate, std::nullopt);
        if (update.generation <= generation)
            return;

        // Replace a waiting update even when the value didn't change, so that an older position can't be applied after it.
        generation = update.generation;
        (update.isForRenderingUpdate ? updateForRenderingUpdate : updateForScrolling) = update;
        if (value != update.value) {
            value = update.value;
            requiresComposition = true;
        }
    };
    auto takeUpdates = [&](std::optional<PositionUpdate>& update, std::optional<PositionUpdate>& updateForScrolling, FloatPoint& value, uint64_t& generation, std::optional<PositionUpdate>& updateForRenderingUpdate, std::optional<PositionUpdate>& updateForScrollingComposition) {
        // Take the older one first, so that the newer one ends up as the latest known value.
        bool updateIsOlder = update && (!updateForScrolling || update->generation < updateForScrolling->generation);
        takeUpdate(updateIsOlder ? update : updateForScrolling, value, generation, updateForRenderingUpdate, updateForScrollingComposition);
        takeUpdate(updateIsOlder ? updateForScrolling : update, value, generation, updateForRenderingUpdate, updateForScrollingComposition);
    };

    takeUpdates(m_pendingState.position, m_pendingState.positionForScrolling, m_position, m_positionGeneration, m_positionUpdatesForRenderingUpdate.position, m_positionUpdatesForScrolling.position);
    takeUpdates(m_pendingState.boundsOrigin, m_pendingState.boundsOriginForScrolling, m_boundsOrigin, m_boundsOriginGeneration, m_positionUpdatesForRenderingUpdate.boundsOrigin, m_positionUpdatesForScrolling.boundsOrigin);
    if (requiresComposition)
        notifyCompositionRequired();
}

void CoordinatedPlatformLayer::flushPositionChanges(const OptionSet<CompositionReason>& reasons)
{
    ASSERT(!isMainThread());
    if (!reasons.containsAny({ CompositionReason::RenderingUpdate, CompositionReason::AsyncScrolling }))
        return;

    PositionUpdates positionUpdates;
    {
        Locker locker { m_lock };
#if USE(TEXTURE_MAPPER)
        if (reasons.contains(CompositionReason::RenderingUpdate))
            applyPositionUpdates(std::exchange(m_positionUpdatesForRenderingUpdate, { }));
#endif
        positionUpdates = std::exchange(m_positionUpdatesForScrolling, { });
    }
    applyPositionUpdates(WTF::move(positionUpdates));
}

void CoordinatedPlatformLayer::applyPositionUpdates(PositionUpdates&& positionUpdates)
{
    ASSERT(!isMainThread());
    if (positionUpdates.position && positionUpdates.position->generation > m_appliedPositionGeneration) {
        ensureTarget().setPosition(positionUpdates.position->value);
        m_appliedPositionGeneration = positionUpdates.position->generation;
    }

    if (positionUpdates.boundsOrigin && positionUpdates.boundsOrigin->generation > m_appliedBoundsOriginGeneration) {
        ensureTarget().setBoundsOrigin(positionUpdates.boundsOrigin->value);
        m_appliedBoundsOriginGeneration = positionUpdates.boundsOrigin->generation;
    }
}

void CoordinatedPlatformLayer::flushCompositingState(const OptionSet<CompositionReason>& reasons)
{
    ASSERT(!isMainThread());
    Locker locker { m_lock };
#if USE(TEXTURE_MAPPER)
    if (m_pendingChanges.isEmpty() && (!reasons.contains(CompositionReason::RenderingUpdate) || !m_backingStoreProxy))
        return;

    flushCompositingStateOnTarget(reasons, ensureTarget());
#else
    // The main thread's changes are applied with the rendering update that committed them, see applyCommittedChanges().
    // While a rendering update waits for its tiles, the reasons can be empty, and then the changes made off the main thread wait too.
    if (m_asyncState.changes.isEmpty() || !reasons.containsAny({ CompositionReason::RenderingUpdate, CompositionReason::VideoFrame, CompositionReason::AsyncScrolling }))
        return;

    applyAsyncChanges(ensureTarget());
#endif
}

#if USE(TEXTURE_MAPPER)
void CoordinatedPlatformLayer::flushCompositingStateOnTarget(const OptionSet<CompositionReason>& reasons, TextureMapperLayer& layer)
{
    assertIsHeld(m_lock);
    if (reasons.containsAny({ CompositionReason::RenderingUpdate, CompositionReason::AsyncScrolling })) {
        if (m_pendingChanges.contains(Change::ContentsRect)) {
            layer.setContentsRect(m_contentsRect);
            m_pendingChanges.remove(Change::ContentsRect);
        }

        if (m_pendingChanges.contains(Change::ContentsClippingRect)) {
            layer.setContentsClippingRect(m_contentsClippingRect);
            m_pendingChanges.remove(Change::ContentsClippingRect);
        }

        // FIXME: clip the contents to the corner-shape contour here too. TextureMapper::beginClip()
        // takes a ClipPath, a triangulated vertex buffer, so this needs a Path tessellation step that
        // does not exist yet; until then a non-round corner shape on composited contents is clipped
        // only by the rect above.
        m_pendingChanges.remove(Change::ContentsClipShapePath);
    }

    if (reasons.contains(CompositionReason::RenderingUpdate)) {
        if (m_pendingChanges.contains(Change::AnchorPoint)) {
            layer.setAnchorPoint(m_anchorPoint);
            m_pendingChanges.remove(Change::AnchorPoint);
        }

        if (m_pendingChanges.contains(Change::Size)) {
            layer.setSize(m_size);
            m_pendingChanges.remove(Change::Size);
        }

        if (m_pendingChanges.contains(Change::Transform)) {
            layer.setTransform(m_transform);
            m_pendingChanges.remove(Change::Transform);
        }

        if (m_pendingChanges.contains(Change::ChildrenTransform)) {
            layer.setChildrenTransform(m_childrenTransform);
            m_pendingChanges.remove(Change::ChildrenTransform);
        }

        if (m_pendingChanges.contains(Change::Preserves3D)) {
            layer.setPreserves3D(m_preserves3D);
            m_pendingChanges.remove(Change::Preserves3D);
        }

        if (m_pendingChanges.contains(Change::MasksToBounds)) {
            layer.setMasksToBounds(m_masksToBounds);
            m_pendingChanges.remove(Change::MasksToBounds);
        }

        if (m_pendingChanges.contains(Change::BackfaceVisibility)) {
            layer.setBackfaceVisibility(m_backfaceVisibility);
            m_pendingChanges.remove(Change::BackfaceVisibility);
        }

        if (m_pendingChanges.contains(Change::BackgroundColor)) {
            layer.setBackgroundColor(m_backgroundColor);
            m_pendingChanges.remove(Change::BackgroundColor);
        }

        if (m_pendingChanges.contains(Change::Opacity)) {
            layer.setOpacity(m_opacity);
            m_pendingChanges.remove(Change::Opacity);
        }

        if (m_pendingChanges.contains(Change::BackingStore)) {
            if (m_backingStoreProxy) {
                if (!m_backingStore)
                    m_backingStore = CoordinatedBackingStore::create();
                layer.setBackingStore(m_backingStore.get());
                layer.setBackgroundColor({ });

                if (auto* animatedBackingStoreClient = m_backingStoreProxy->animatedBackingStoreClient())
                    layer.setAnimatedBackingStoreClient(animatedBackingStoreClient);
            } else {
                layer.setBackingStore(nullptr);
                layer.setAnimatedBackingStoreClient(nullptr);
                m_backingStore = nullptr;
            }
            m_pendingChanges.remove(Change::BackingStore);
        }

        if (m_pendingChanges.contains(Change::ContentsImage)) {
            m_imageBackingStore.committed = m_imageBackingStore.current;
            m_pendingChanges.remove(Change::ContentsImage);
        }

        if (m_pendingChanges.contains(Change::ContentsVisible)) {
            layer.setContentsVisible(m_contentsVisible);
            m_pendingChanges.remove(Change::ContentsVisible);
        }

        if (m_pendingChanges.contains(Change::ContentsOpaque)) {
            layer.setContentsOpaque(m_contentsOpaque);
            m_pendingChanges.remove(Change::ContentsOpaque);
        }

        if (m_pendingChanges.contains(Change::ContentsRectClipsDescendants)) {
            layer.setContentsRectClipsDescendants(m_contentsRectClipsDescendants);
            m_pendingChanges.remove(Change::ContentsRectClipsDescendants);
        }

        if (m_pendingChanges.contains(Change::ContentsTiling)) {
            layer.setContentsTileSize(m_contentsTileSize);
            layer.setContentsTilePhase(m_contentsTilePhase);
            m_pendingChanges.remove(Change::ContentsTiling);
        }

        if (m_pendingChanges.contains(Change::ContentsColor)) {
            layer.setSolidColor(m_contentsColor);
            m_pendingChanges.remove(Change::ContentsColor);
        }

#if ENABLE(DAMAGE_TRACKING)
        if (m_pendingChanges.contains(Change::Damage)) {
            ASSERT(m_damage.has_value());
            layer.setDamage(*std::exchange(m_damage, std::nullopt));
            m_pendingChanges.remove(Change::Damage);
        }
#endif

        if (m_pendingChanges.contains(Change::Filters)) {
            layer.setFilters(m_filters);
            m_pendingChanges.remove(Change::Filters);
        }

        if (m_pendingChanges.contains(Change::Mask)) {
            layer.setMaskLayer(m_mask ? &m_mask->ensureTarget() : nullptr);
            m_pendingChanges.remove(Change::Mask);
        }

        if (m_pendingChanges.contains(Change::Replica)) {
            layer.setReplicaLayer(m_replica ? &m_replica->ensureTarget() : nullptr);
            m_pendingChanges.remove(Change::Replica);
        }

        if (m_pendingChanges.contains(Change::Backdrop)) {
            layer.setBackdropLayer(m_backdrop ? &m_backdrop->ensureTarget() : nullptr);
            m_pendingChanges.remove(Change::Backdrop);
        }

        if (m_pendingChanges.contains(Change::BackdropRect)) {
            layer.setBackdropFiltersRect(m_backdropRect);
            m_pendingChanges.remove(Change::BackdropRect);
        }

        // FIXME: clip the backdrop to the corner-shape contour here too. TextureMapper::beginClip()
        // takes a ClipPath, a triangulated vertex buffer, so this needs a Path tessellation step that
        // does not exist yet; until then a non-round corner shape on a backdrop filter is clipped only
        // by the rounded rect above. The Skia compositor, which GTK and WPE use by default, clips to
        // the contour.
        m_pendingChanges.remove(Change::BackdropShapePath);

        if (m_pendingChanges.contains(Change::Animations)) {
            layer.setAnimations(m_animations);
            m_pendingChanges.remove(Change::Animations);
        }

        if (m_pendingChanges.contains(Change::DebugIndicators)) {
            layer.setShowRepaintCounter(m_repaintCount != -1);
            layer.setRepaintCount(m_repaintCount);

            layer.setShowDebugBorder(m_debugBorderColor.isVisible());
            layer.setDebugBorderColor(m_debugBorderColor);
            layer.setDebugBorderWidth(m_debugBorderWidth);
            m_pendingChanges.remove(Change::DebugIndicators);
        }

        if (m_pendingChanges.contains(Change::Children)) {
            layer.setChildren(WTF::map(m_children, [](auto& child) {
                return &child->ensureTarget();
            }));
            m_pendingChanges.remove(Change::Children);
        }

        if (m_backingStoreProxy) {
            m_backingStore->resize(layer.size(), m_contentsScale);

            auto update = m_backingStoreProxy->takePendingUpdate();
            for (auto tileID : update.tilesToCreate())
                m_backingStore->createTile(tileID);
            for (auto tileID : update.tilesToRemove())
                m_backingStore->removeTile(tileID);
            for (const auto& tileUpdate : update.tilesToUpdate())
                m_backingStore->updateTile(tileUpdate.tileID, tileUpdate.dirtyRect, tileUpdate.tileRect, tileUpdate.buffer.copyRef(), { });
        }
    }

    if (reasons.containsAny({ CompositionReason::RenderingUpdate, CompositionReason::VideoFrame, CompositionReason::AsyncScrolling })) {
        if (m_pendingChanges.contains(Change::ContentsBuffer)) {
            m_contentsBuffer.committed = WTF::move(m_contentsBuffer.pending);
            m_contentsBuffer.hasCommitted = !!m_contentsBuffer.committed;
            m_pendingChanges.remove(Change::ContentsBuffer);
        }

        if (m_contentsBuffer.committed)
            layer.setContentsLayer(m_contentsBuffer.committed.get());
        else if (m_imageBackingStore.committed) {
            if (reasons.containsAny({ CompositionReason::RenderingUpdate, CompositionReason::AsyncScrolling }))
                layer.setContentsLayer(m_imageBackingStore.committed->buffer());
        } else
            layer.setContentsLayer(nullptr);
    }
}

#else

void CoordinatedPlatformLayer::applyAsyncChanges(SkiaCompositingLayer& layer)
{
    assertIsHeld(m_lock);
    auto asyncChanges = std::exchange(m_asyncState.changes, { });
    if (asyncChanges.contains(Change::ContentsRect))
        layer.setContentsRect(m_asyncState.contentsRect);

    if (asyncChanges.contains(Change::ContentsClippingRect))
        layer.setContentsClippingRect(m_asyncState.contentsClippingRect);

#if ENABLE(DAMAGE_TRACKING)
    if (asyncChanges.contains(Change::Damage)) {
        ASSERT(m_asyncState.damage.has_value());
        layer.addDamage(*std::exchange(m_asyncState.damage, std::nullopt));
    }
#endif

    if (asyncChanges.contains(Change::ContentsBuffer)) {
        m_contentsBuffer.hasCommitted = !!m_asyncState.contentsBuffer;
        layer.setContentsBuffer(WTF::move(m_asyncState.contentsBuffer));
    }
}

bool CoordinatedPlatformLayer::commitChanges(uint64_t transactionID)
{
    ASSERT(isMainThread());
    Locker locker { m_lock };

    std::optional<CoordinatedBackingStoreProxy::Update> tileUpdate;
    if (m_backingStoreProxy)
        tileUpdate = m_backingStoreProxy->takePendingUpdate();

    bool hasPositionUpdates = m_positionUpdatesForRenderingUpdate.position || m_positionUpdatesForRenderingUpdate.boundsOrigin;
    if (m_pendingChanges.isEmpty() && !hasPositionUpdates && (!tileUpdate || tileUpdate->isEmpty()))
        return false;

    auto committedChanges = makeUnique<CommittedChanges>();
    committedChanges->transactionID = transactionID;
    auto changes = std::exchange(m_pendingChanges, { });
    committedChanges->changes = changes;
    committedChanges->positionUpdates = std::exchange(m_positionUpdatesForRenderingUpdate, { });
    committedChanges->properties = commitProperties(changes);

    if (tileUpdate) {
        committedChanges->tileUpdate = WTF::move(tileUpdate);
        committedChanges->contentsScale = m_contentsScale;
    }
    if (changes.contains(Change::ContentsBuffer)) {
        m_contentsBuffer.hasCommitted = !!m_contentsBuffer.pending;
        committedChanges->contentsBuffer = WTF::move(m_contentsBuffer.pending);
    }
#if ENABLE(DAMAGE_TRACKING)
    if (changes.contains(Change::Damage))
        committedChanges->damage = std::exchange(m_damage, std::nullopt);
#endif

    m_committedChanges.append(WTF::move(committedChanges));
    return true;
}

auto CoordinatedPlatformLayer::commitProperties(EnumSet<Change> changes) -> std::unique_ptr<CommittedProperties>
{
    static constexpr EnumSet<Change> changesWithoutProperties {
        Change::ContentsBuffer,
#if ENABLE(DAMAGE_TRACKING)
        Change::Damage,
#endif
    };
    if (changes.containsOnly(changesWithoutProperties))
        return nullptr;

    auto properties = makeUnique<CommittedProperties>();
    if (changes.contains(Change::AnchorPoint))
        properties->anchorPoint = m_anchorPoint;
    if (changes.contains(Change::Size))
        properties->size = m_size;
    if (changes.contains(Change::Transform))
        properties->transform = m_transform;
    if (changes.contains(Change::ChildrenTransform))
        properties->childrenTransform = m_childrenTransform;
    if (changes.contains(Change::MasksToBounds))
        properties->masksToBounds = m_masksToBounds;
    if (changes.contains(Change::Preserves3D))
        properties->preserves3D = m_preserves3D;
    if (changes.contains(Change::BackfaceVisibility))
        properties->backfaceVisibility = m_backfaceVisibility;
    if (changes.contains(Change::BackgroundColor))
        properties->backgroundColor = m_backgroundColor;
    if (changes.contains(Change::Opacity))
        properties->opacity = m_opacity;
    if (changes.contains(Change::BlendMode))
        properties->blendMode = m_blendMode;
    if (changes.contains(Change::ContentsVisible))
        properties->contentsVisible = m_contentsVisible;
    if (changes.contains(Change::ContentsOpaque))
        properties->contentsOpaque = m_contentsOpaque;
    if (changes.contains(Change::ContentsRect))
        properties->contentsRect = m_contentsRect;
    if (changes.contains(Change::ContentsRectClipsDescendants))
        properties->contentsRectClipsDescendants = m_contentsRectClipsDescendants;
    if (changes.contains(Change::ContentsClippingRect))
        properties->contentsClippingRect = m_contentsClippingRect;
    if (changes.contains(Change::ContentsClipShapePath))
        properties->contentsClipShapePath = m_contentsClipShapePath;
    if (changes.contains(Change::ContentsColor))
        properties->contentsColor = m_contentsColor;
    if (changes.contains(Change::ContentsTiling)) {
        properties->contentsTileSize = m_contentsTileSize;
        properties->contentsTilePhase = m_contentsTilePhase;
    }
    if (changes.contains(Change::ContentsImage))
        properties->imageBackingStore = m_imageBackingStore.current;
    if (changes.contains(Change::ClipPath)) {
        properties->clipPath = m_clipPath.path;
        properties->clipPathWindRule = m_clipPath.windRule;
    }
    if (changes.contains(Change::Filters))
        properties->filters = m_filters;
    if (changes.contains(Change::Mask))
        properties->mask = m_mask;
    if (changes.contains(Change::Replica))
        properties->replica = m_replica;
    // The owner is told about every change of the backdrop filters, see notifyBackdropFiltersChanged().
    if (changes.contains(Change::Backdrop) && m_backdrop) {
        Locker backdropLocker { m_backdrop->m_lock };
        properties->backdropFilters = m_backdrop->m_filters;
    }
    if (changes.contains(Change::BackdropRect))
        properties->backdropRect = m_backdropRect;
    if (changes.contains(Change::BackdropShapePath))
        properties->backdropShapePath = m_backdropShapePath;
    if (changes.contains(Change::BackdropRoot))
        properties->isBackdropRoot = m_isBackdropRoot;
    if (changes.contains(Change::Animations))
        properties->animations = m_animations;
    if (changes.contains(Change::DebugIndicators)) {
        properties->debugBorderColor = m_debugBorderColor;
        properties->debugBorderWidth = m_debugBorderWidth;
        properties->repaintCount = m_repaintCount;
    }
    if (changes.contains(Change::Children))
        properties->children = m_children;
    if (changes.contains(Change::BackingStore)) {
        properties->hasBackingStore = !!m_backingStoreProxy;
        properties->animatedBackingStoreClient = m_backingStoreProxy ? m_backingStoreProxy->animatedBackingStoreClient() : nullptr;
    }
    return properties;
}

void CoordinatedPlatformLayer::applyCommittedChanges(uint64_t transactionID)
{
    ASSERT(!isMainThread());
    std::unique_ptr<CommittedChanges> committedChanges;
    {
        Locker locker { m_lock };
        if (m_committedChanges.isEmpty())
            return;

        ASSERT(m_committedChanges.first()->transactionID >= transactionID);
        if (m_committedChanges.first()->transactionID != transactionID)
            return;

        committedChanges = m_committedChanges.takeFirst();
    }

    // Everything needed was copied at commit time, so the main thread can go on changing the layer meanwhile.
    applyPositionUpdates(WTF::move(committedChanges->positionUpdates));

    auto& layer = ensureTarget();
    const auto& changes = committedChanges->changes;
    if (committedChanges->properties)
        applyCommittedProperties(changes, WTF::move(*committedChanges->properties), layer);

    if (committedChanges->tileUpdate)
        layer.updateBackingStore(WTF::move(*committedChanges->tileUpdate), committedChanges->contentsScale);

#if ENABLE(DAMAGE_TRACKING)
    if (changes.contains(Change::Damage)) {
        ASSERT(committedChanges->damage.has_value());
        layer.addDamage(WTF::move(*committedChanges->damage));
    }
#endif

    if (changes.contains(Change::ContentsBuffer))
        layer.setContentsBuffer(WTF::move(committedChanges->contentsBuffer));
}

// An empty shape means that there is no clip.
static std::optional<SkPath> shapeClipPath(const Path& shape)
{
    if (shape.isEmpty())
        return std::nullopt;

    auto clipPath = *shape.platformPath();
    clipPath.setFillType(SkPathFillType::kWinding);
    return clipPath;
}

void CoordinatedPlatformLayer::applyCommittedProperties(EnumSet<Change> changes, CommittedProperties&& properties, SkiaCompositingLayer& layer)
{
    if (changes.contains(Change::ContentsRect))
        layer.setContentsRect(properties.contentsRect);

    if (changes.contains(Change::ContentsClippingRect))
        layer.setContentsClippingRect(properties.contentsClippingRect);

    if (changes.contains(Change::ContentsClipShapePath))
        layer.setContentsClipPath(shapeClipPath(properties.contentsClipShapePath));

    if (changes.contains(Change::ContentsImage))
        layer.setImageBackingStore(properties.imageBackingStore.get());

    if (changes.contains(Change::AnchorPoint))
        layer.setAnchorPoint(properties.anchorPoint);

    if (changes.contains(Change::Size))
        layer.setSize(properties.size);

    if (changes.contains(Change::Transform))
        layer.setTransform(properties.transform);

    if (changes.contains(Change::ChildrenTransform))
        layer.setChildrenTransform(properties.childrenTransform);

    if (changes.contains(Change::Preserves3D))
        layer.setPreserves3D(properties.preserves3D);

    if (changes.contains(Change::MasksToBounds))
        layer.setMasksToBounds(properties.masksToBounds);

    if (changes.contains(Change::BackfaceVisibility))
        layer.setBackfaceVisibility(properties.backfaceVisibility);

    if (changes.contains(Change::BackgroundColor))
        layer.setBackgroundColor(properties.backgroundColor);

    if (changes.contains(Change::Opacity))
        layer.setOpacity(properties.opacity);

    if (changes.contains(Change::BlendMode))
        layer.setBlendMode(properties.blendMode);

    if (changes.contains(Change::BackingStore))
        layer.setUseBackingStore(properties.hasBackingStore, properties.animatedBackingStoreClient.get());

    if (changes.contains(Change::ContentsVisible))
        layer.setContentsVisible(properties.contentsVisible);

    if (changes.contains(Change::ContentsOpaque))
        layer.setContentsOpaque(properties.contentsOpaque);

    if (changes.contains(Change::ContentsRectClipsDescendants))
        layer.setContentsRectClipsDescendants(properties.contentsRectClipsDescendants);

    if (changes.contains(Change::ContentsTiling))
        layer.setContentsTiling(properties.contentsTileSize, properties.contentsTilePhase);

    if (changes.contains(Change::ContentsColor))
        layer.setContentsSolidColor(properties.contentsColor);

    if (changes.contains(Change::ClipPath)) {
        auto clipPath = *properties.clipPath.platformPath();
        clipPath.setFillType(properties.clipPathWindRule == WindRule::EvenOdd ? SkPathFillType::kEvenOdd : SkPathFillType::kWinding);
        layer.setClipPath(WTF::move(clipPath));
    }

    if (changes.contains(Change::Filters))
        layer.setFilters(properties.filters);

    if (changes.contains(Change::Mask))
        layer.setMask(properties.mask ? RefPtr { &properties.mask->ensureTarget() } : nullptr);

    if (changes.contains(Change::Replica))
        layer.setReplica(properties.replica ? RefPtr { &properties.replica->ensureTarget() } : nullptr);

    // FIXME: stop creating a layer for backdrop filters when switching to SkiaCompositingLayer.
    if (changes.contains(Change::Backdrop))
        layer.setBackdropFilters(properties.backdropFilters);

    if (changes.contains(Change::BackdropRect))
        layer.setBackdropFiltersRect(properties.backdropRect);

    if (changes.contains(Change::BackdropShapePath))
        layer.setBackdropFiltersClipPath(shapeClipPath(properties.backdropShapePath));

    if (changes.contains(Change::BackdropRoot))
        layer.setIsBackdropRoot(properties.isBackdropRoot);

    if (changes.contains(Change::Animations))
        layer.setAnimations(WTF::move(properties.animations));

    if (changes.contains(Change::DebugIndicators)) {
        Color color;
        std::optional<float> width;
        if (properties.debugBorderColor.isVisible()) {
            color = properties.debugBorderColor;
            width = properties.debugBorderWidth;
        }
        std::optional<unsigned> repaintCount;
        if (properties.repaintCount != -1)
            repaintCount = properties.repaintCount;

        layer.setDebugIndicators(WTF::move(color), width, repaintCount);
    }

    if (changes.contains(Change::Children)) {
        layer.setChildren(WTF::map(properties.children, [](auto& child) {
            return Ref { child->ensureTarget() };
        }));
    }
}
#endif

bool CoordinatedPlatformLayer::hasPendingBackingStoreTileUpdates() const
{
    ASSERT(!isMainThread());

#if USE(TEXTURE_MAPPER)
    Locker locker { m_lock };
    if (m_backingStore)
        return m_backingStore->hasPendingUpdates();
#else
    if (m_target)
        return m_target->hasPendingBackingStoreTileUpdates();
#endif

    return false;
}

void CoordinatedPlatformLayer::processPendingBackingStoreTileUpdates()
{
    ASSERT(!isMainThread());

#if USE(TEXTURE_MAPPER)
    Locker locker { m_lock };
    if (m_backingStore)
        m_backingStore->processPendingUpdates();
#else
    if (m_target) {
        m_target->processPendingTileUpdates();
        return;
    }
#endif
}

} // namespace WebCore

#endif // USE(COORDINATED_GRAPHICS)
