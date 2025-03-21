/*
 * Copyright (C) 2024, 2025 Igalia S.L.
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
#include <skia/core/SkPictureRecorder.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/WorkerPool.h>

class SkImage;

namespace WebCore {

class BitmapTexturePool;
class CoordinatedTileBuffer;
class GraphicsContext;
class GraphicsLayer;
class IntRect;
class IntSize;
enum class RenderingMode : uint8_t;

class SkiaPaintingEngine {
    WTF_MAKE_TZONE_ALLOCATED(SkiaPaintingEngine);
    WTF_MAKE_NONCOPYABLE(SkiaPaintingEngine);
public:
    SkiaPaintingEngine(unsigned numberOfCPUThreads, unsigned numberOfGPUThreads);
    ~SkiaPaintingEngine();

    static std::unique_ptr<SkiaPaintingEngine> create();

    enum class HybridPaintingStrategy {
        PreferCPUIfIdle,
        PreferGPUIfIdle,
        PreferGPUAboveMinimumArea,
        MinimumFractionOfTasksUsingGPU,
        CPUAffineRendering,
        GPUAffineRendering
    };

    static unsigned numberOfCPUPaintingThreads();
    static unsigned numberOfGPUPaintingThreads();
    static unsigned minimumAreaForGPUPainting();
    static float minimumFractionOfTasksUsingGPUPainting();
    static HybridPaintingStrategy hybridPaintingStrategy();

    Ref<CoordinatedTileBuffer> paintLayer(const GraphicsLayer&, const IntRect& dirtyRect, bool contentsOpaque, float contentsScale);

private:
    Ref<CoordinatedTileBuffer> createBuffer(RenderingMode, const IntSize&, bool contentsOpaque) const;
    void paintIntoGraphicsContext(const GraphicsLayer&, GraphicsContext&, const IntRect&, bool contentsOpaque, float contentsScale) const;
    bool paintGraphicsLayerIntoBuffer(Ref<CoordinatedTileBuffer>&, const GraphicsLayer&, const IntRect& dirtyRect, bool contentsOpaque, float contentsScale) const;

    // Threaded rendering
    Ref<CoordinatedTileBuffer> postPaintingTask(const GraphicsLayer&, RenderingMode, const IntRect& dirtyRect, bool contentsOpaque, float contentsScale);

    // Main thread rendering
    Ref<CoordinatedTileBuffer> performPaintingTask(const GraphicsLayer&, RenderingMode, const IntRect& dirtyRect, bool contentsOpaque, float contentsScale);

    bool isHybridMode() const;
    RenderingMode decideHybridRenderingMode(const IntRect& dirtyRect, float contentsScale) const;

    RefPtr<WorkerPool> m_cpuWorkerPool;
    RefPtr<WorkerPool> m_gpuWorkerPool;
    std::unique_ptr<BitmapTexturePool> m_texturePool;
    SkPictureRecorder m_pictureRecorder;
};

} // namespace WebCore

#endif // USE(COORDINATED_GRAPHICS) && USE(SKIA)
