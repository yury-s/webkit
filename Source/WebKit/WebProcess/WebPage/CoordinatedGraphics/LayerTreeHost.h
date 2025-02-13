/*
 * Copyright (C) 2011, 2012 Apple Inc. All rights reserved.
 * Copyright (C) 2019 Igalia S.L.
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
#include "CallbackID.h"
#include "LayerTreeContext.h"
#include "ThreadedCompositor.h"
#include <WebCore/CoordinatedImageBackingStore.h>
#include <WebCore/CoordinatedPlatformLayer.h>
#include <WebCore/FloatPoint.h>
#include <WebCore/GraphicsLayerClient.h>
#include <WebCore/GraphicsLayerFactory.h>
#include <WebCore/PlatformScreen.h>
#include <wtf/CheckedRef.h>
#include <wtf/Forward.h>
#include <wtf/OptionSet.h>
#include <wtf/RunLoop.h>
#include <wtf/TZoneMalloc.h>

#if !HAVE(DISPLAY_LINK)
#include "ThreadedDisplayRefreshMonitor.h"
#endif

namespace WebCore {
class Damage;
class IntRect;
class IntSize;
class GraphicsLayer;
class GraphicsLayerFactory;
class NativeImage;
class SkiaPaintingEngine;
#if USE(CAIRO)
namespace Cairo {
class PaintingEngine;
}
#endif
}

namespace WebKit {
class LayerTreeHost;
}

namespace WTF {
template<typename T> struct IsDeprecatedTimerSmartPointerException;
template<> struct IsDeprecatedTimerSmartPointerException<WebKit::LayerTreeHost> : std::true_type { };
}

namespace WebKit {
class CoordinatedSceneState;
class WebPage;

class LayerTreeHost final : public CanMakeCheckedPtr<LayerTreeHost>, public WebCore::GraphicsLayerClient, public WebCore::GraphicsLayerFactory, public WebCore::CoordinatedPlatformLayer::Client
#if !HAVE(DISPLAY_LINK)
    , public ThreadedDisplayRefreshMonitor::Client
#endif
{
    WTF_MAKE_TZONE_ALLOCATED(LayerTreeHost);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(LayerTreeHost);
public:
#if HAVE(DISPLAY_LINK)
    explicit LayerTreeHost(WebPage&);
#else
    LayerTreeHost(WebPage&, WebCore::PlatformDisplayID);
#endif
    ~LayerTreeHost();

    WebPage& webPage() const { return m_webPage; }
    CoordinatedSceneState& sceneState() const { return m_sceneState.get(); }

    const LayerTreeContext& layerTreeContext() const { return m_layerTreeContext; }
    void setLayerTreeStateIsFrozen(bool);

    void scheduleLayerFlush();
    void cancelPendingLayerFlush();
    void setRootCompositingLayer(WebCore::GraphicsLayer*);
    void setViewOverlayRootLayer(WebCore::GraphicsLayer*);

    void forceRepaint();
    void forceRepaintAsync(CompletionHandler<void()>&&);
    void sizeDidChange(const WebCore::IntSize&);

    void pauseRendering();
    void resumeRendering();

    WebCore::GraphicsLayerFactory* graphicsLayerFactory();

    void backgroundColorDidChange();

    void willRenderFrame();
    void didRenderFrame();
#if HAVE(DISPLAY_LINK)
    void didComposite(uint32_t);
#endif

#if !HAVE(DISPLAY_LINK)
    RefPtr<WebCore::DisplayRefreshMonitor> createDisplayRefreshMonitor(WebCore::PlatformDisplayID);
    WebCore::PlatformDisplayID displayID() const { return m_displayID; }
#endif

#if PLATFORM(GTK)
    void adjustTransientZoom(double, WebCore::FloatPoint);
    void commitTransientZoom(double, WebCore::FloatPoint);
#endif

#if PLATFORM(GTK) || PLATFORM(WPE)
    void ensureDrawing();
#endif

#if PLATFORM(WPE) && USE(GBM) && ENABLE(WPE_PLATFORM)
    void preferredBufferFormatsDidChange();
#endif

private:
    void updateRootLayer();
    WebCore::FloatRect visibleContentsRect() const;
    void layerFlushTimerFired();
    void flushLayers();
    void commitSceneState();
#if !HAVE(DISPLAY_LINK)
    void renderNextFrame(bool);
#endif

    // CoordinatedPlatformLayer::Client
#if USE(CAIRO)
    WebCore::Cairo::PaintingEngine& paintingEngine() override;
#elif USE(SKIA)
    WebCore::SkiaPaintingEngine& paintingEngine() const override { return *m_skiaPaintingEngine.get(); }
#endif
    Ref<WebCore::CoordinatedImageBackingStore> imageBackingStore(Ref<WebCore::NativeImage>&&) override;

    void attachLayer(WebCore::CoordinatedPlatformLayer&) override;
    void detachLayer(WebCore::CoordinatedPlatformLayer&) override;
    void notifyCompositionRequired() override;
    bool isCompositionRequiredOrOngoing() const override;
    void requestComposition() override;
    RunLoop* compositingRunLoop() const override;

    // GraphicsLayerFactory
    Ref<WebCore::GraphicsLayer> createGraphicsLayer(WebCore::GraphicsLayer::Type, WebCore::GraphicsLayerClient&) override;

#if !HAVE(DISPLAY_LINK)
    // ThreadedDisplayRefreshMonitor::Client
    void requestDisplayRefreshMonitorUpdate() override;
    void handleDisplayRefreshMonitorUpdate(bool hasBeenRescheduled) override;
#endif

#if PLATFORM(GTK)
    WebCore::FloatPoint constrainTransientZoomOrigin(double, WebCore::FloatPoint) const;
    WebCore::CoordinatedPlatformLayer* layerForTransientZoom() const;
    void applyTransientZoomToLayers(double, WebCore::FloatPoint);
#endif

    WebPage& m_webPage;
    LayerTreeContext m_layerTreeContext;
    Ref<CoordinatedSceneState> m_sceneState;
    WebCore::GraphicsLayer* m_rootCompositingLayer { nullptr };
    WebCore::GraphicsLayer* m_overlayCompositingLayer { nullptr };
    HashSet<Ref<WebCore::CoordinatedPlatformLayer>> m_layers;
    bool m_didInitializeRootCompositingLayer { false };
    bool m_layerTreeStateIsFrozen { false };
    bool m_isPurgingBackingStores { false };
    bool m_pendingResize { false };
    bool m_pendingForceRepaint { false };
    bool m_isSuspended { false };
    bool m_isWaitingForRenderer { false };
    bool m_scheduledWhileWaitingForRenderer { false };
    bool m_forceFrameSync { false };
    bool m_compositionRequired { false };
#if ENABLE(SCROLLING_THREAD)
    bool m_compositionRequiredInScrollingThread { false };
#endif
    double m_lastAnimationServiceTime { 0 };
    RefPtr<ThreadedCompositor> m_compositor;
    struct {
        CompletionHandler<void()> callback;
#if HAVE(DISPLAY_LINK)
        std::optional<uint32_t> compositionRequestID;
#else
        bool needsFreshFlush { false };
#endif
    } m_forceRepaintAsync;
    RunLoop::Timer m_layerFlushTimer;
#if !HAVE(DISPLAY_LINK)
    WebCore::PlatformDisplayID m_displayID;
#endif
#if USE(CAIRO)
    std::unique_ptr<WebCore::Cairo::PaintingEngine> m_paintingEngine;
#elif USE(SKIA)
    std::unique_ptr<WebCore::SkiaPaintingEngine> m_skiaPaintingEngine;
#endif
    HashMap<uint64_t, Ref<WebCore::CoordinatedImageBackingStore>> m_imageBackingStores;

#if PLATFORM(GTK)
    bool m_transientZoom { false };
    double m_transientZoomScale { 1 };
    WebCore::FloatPoint m_transientZoomOrigin;
#endif

    uint32_t m_compositionRequestID { 0 };
};

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)
