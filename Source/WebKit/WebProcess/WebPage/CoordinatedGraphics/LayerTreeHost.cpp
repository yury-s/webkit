/*
 * Copyright (C) 2011 Apple Inc. All rights reserved.
 * Copyright (C) 2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2012 Company 100, Inc.
 * Copyright (C) 2014-2019 Igalia S.L.
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
#include "LayerTreeHost.h"

#if USE(COORDINATED_GRAPHICS)
#include "CoordinatedSceneState.h"
#include "DrawingArea.h"
#include "WebPageInlines.h"
#include "WebPageProxyMessages.h"
#include "WebProcess.h"
#include <WebCore/AsyncScrollingCoordinator.h>
#include <WebCore/Chrome.h>
#include <WebCore/Damage.h>
#include <WebCore/GraphicsLayerCoordinated.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameView.h>
#include <WebCore/NativeImage.h>
#include <WebCore/PageOverlayController.h>
#include <WebCore/RenderLayerBacking.h>
#include <WebCore/RenderView.h>
#include <WebCore/ScrollingThread.h>
#include <WebCore/Settings.h>
#include <WebCore/ThreadedScrollingTree.h>
#include <wtf/SetForScope.h>
#include <wtf/SystemTracing.h>
#include <wtf/TZoneMallocInlines.h>

#if USE(CAIRO)
#include <WebCore/CairoPaintingEngine.h>
#elif USE(SKIA)
#include <WebCore/SkiaPaintingEngine.h>
#endif

#if USE(GLIB_EVENT_LOOP)
#include <wtf/glib/RunLoopSourcePriority.h>
#endif

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(LayerTreeHost);

#if HAVE(DISPLAY_LINK)
LayerTreeHost::LayerTreeHost(WebPage& webPage)
#else
LayerTreeHost::LayerTreeHost(WebPage& webPage, WebCore::PlatformDisplayID displayID)
#endif
    : m_webPage(webPage)
    , m_sceneState(CoordinatedSceneState::create())
    , m_layerFlushTimer(RunLoop::main(), this, &LayerTreeHost::layerFlushTimerFired)
#if !HAVE(DISPLAY_LINK)
    , m_displayID(displayID)
#endif
#if USE(CAIRO)
    , m_paintingEngine(Cairo::PaintingEngine::create())
#elif USE(SKIA)
    , m_skiaPaintingEngine(SkiaPaintingEngine::create())
#endif
{
    {
        auto& rootLayer = m_sceneState->rootLayer();
#if ENABLE(DAMAGE_TRACKING)
        rootLayer.setDamagePropagation(webPage.corePage()->settings().propagateDamagingInformation());
#endif
        Locker locker { rootLayer.lock() };
        rootLayer.setAnchorPoint(FloatPoint3D(0, 0, 0));
        rootLayer.setSize(m_webPage.size());
    }

#if USE(GLIB_EVENT_LOOP)
    m_layerFlushTimer.setPriority(RunLoopSourcePriority::LayerFlushTimer);
    m_layerFlushTimer.setName("[WebKit] LayerTreeHost"_s);
#endif
    scheduleLayerFlush();

#if HAVE(DISPLAY_LINK)
    m_compositor = ThreadedCompositor::create(*this);
#else
    m_compositor = ThreadedCompositor::create(*this, *this, displayID);
#endif
#if ENABLE(DAMAGE_TRACKING)
    auto damagePropagation = ([](const Settings& settings) {
        if (!settings.propagateDamagingInformation())
            return Damage::Propagation::None;
        if (settings.unifyDamagedRegions())
            return Damage::Propagation::Unified;
        return Damage::Propagation::Region;
    })(webPage.corePage()->settings());
    m_compositor->setDamagePropagation(damagePropagation);
#endif
    m_layerTreeContext.contextID = m_compositor->surfaceID();
}

LayerTreeHost::~LayerTreeHost()
{
    if (m_forceRepaintAsync.callback)
        m_forceRepaintAsync.callback();

    cancelPendingLayerFlush();

    m_sceneState->invalidate();

#if USE(SKIA)
    m_skiaPaintingEngine = nullptr;
#endif

    m_compositor->invalidate();
}

void LayerTreeHost::setLayerTreeStateIsFrozen(bool isFrozen)
{
    if (m_layerTreeStateIsFrozen == isFrozen)
        return;

    m_layerTreeStateIsFrozen = isFrozen;

    if (m_layerTreeStateIsFrozen)
        cancelPendingLayerFlush();
    else
        scheduleLayerFlush();
}

void LayerTreeHost::scheduleLayerFlush()
{
    WTFEmitSignpost(this, ScheduleLayerFlush, "isWaitingForRenderer %i", m_isWaitingForRenderer);

    if (m_layerTreeStateIsFrozen)
        return;

    if (m_webPage.size().isEmpty())
        return;

    if (m_isWaitingForRenderer) {
        m_scheduledWhileWaitingForRenderer = true;
        return;
    }

    if (!m_layerFlushTimer.isActive())
        m_layerFlushTimer.startOneShot(0_s);
}

void LayerTreeHost::cancelPendingLayerFlush()
{
    m_layerFlushTimer.stop();
}

void LayerTreeHost::flushLayers()
{
    if (m_layerTreeStateIsFrozen)
        return;

#if PLATFORM(GTK) || PLATFORM(WPE)
    TraceScope traceScope(FlushPendingLayerChangesStart, FlushPendingLayerChangesEnd);
#endif

    Ref page { m_webPage };
    page->updateRendering();
    page->flushPendingEditorStateUpdate();

    if (m_overlayCompositingLayer)
        m_overlayCompositingLayer->flushCompositingState(visibleContentsRect());

    OptionSet<FinalizeRenderingUpdateFlags> flags;
#if PLATFORM(GTK)
    if (!m_transientZoom)
        flags.add(FinalizeRenderingUpdateFlags::ApplyScrollingTreeLayerPositions);
#else
    flags.add(FinalizeRenderingUpdateFlags::ApplyScrollingTreeLayerPositions);
#endif
    page->finalizeRenderingUpdate(flags);

    if (m_pendingResize) {
        auto& rootLayer = m_sceneState->rootLayer();
        Locker locker { rootLayer.lock() };
        rootLayer.setSize(page->size());
    }

#if PLATFORM(GTK)
    // If we have an active transient zoom, we want the zoom to win over any changes
    // that WebCore makes to the relevant layers, so re-apply our changes after flushing.
    if (m_transientZoom)
        applyTransientZoomToLayers(m_transientZoomScale, m_transientZoomOrigin);
#endif

#if PLATFORM(GTK) || PLATFORM(WPE)
    if (auto* drawingArea = m_webPage.drawingArea())
        drawingArea->dispatchPendingCallbacksAfterEnsuringDrawing();
#endif

    bool didChangeSceneState = m_sceneState->flush();
    if (m_compositionRequired || m_pendingResize || m_forceFrameSync || didChangeSceneState)
        commitSceneState();

    m_compositionRequired = false;
    m_pendingResize = false;
    m_forceFrameSync = false;

    page->didUpdateRendering();

    // Eject any backing stores whose only reference is held in the HashMap cache.
    m_imageBackingStores.removeIf([](auto& it) {
        return it.value->hasOneRef();
    });
}

void LayerTreeHost::layerFlushTimerFired()
{
    WTFBeginSignpost(this, LayerFlushTimerFired, "isWaitingForRenderer %i", m_isWaitingForRenderer);

    if (m_isSuspended) {
        WTFEndSignpost(this, LayerFlushTimerFired);
        return;
    }

    if (m_isWaitingForRenderer) {
        WTFEndSignpost(this, LayerFlushTimerFired);
        return;
    }

#if !HAVE(DISPLAY_LINK)
    // If a force-repaint callback was registered, we should force a 'frame sync' that
    // will guarantee us a call to renderNextFrame() once the update is complete.
    if (m_forceRepaintAsync.callback)
        m_forceFrameSync = true;
#endif

    flushLayers();

    WTFEndSignpost(this, LayerFlushTimerFired);
}

void LayerTreeHost::updateRootLayer()
{
    Vector<Ref<CoordinatedPlatformLayer>> children;
    if (m_rootCompositingLayer) {
        children.append(downcast<GraphicsLayerCoordinated>(m_rootCompositingLayer)->coordinatedPlatformLayer());
        if (m_overlayCompositingLayer)
            children.append(downcast<GraphicsLayerCoordinated>(m_overlayCompositingLayer)->coordinatedPlatformLayer());
    }

    m_sceneState->setRootLayerChildren(WTFMove(children));
}

void LayerTreeHost::setRootCompositingLayer(GraphicsLayer* graphicsLayer)
{
    if (m_rootCompositingLayer == graphicsLayer)
        return;

    m_rootCompositingLayer = graphicsLayer;
    updateRootLayer();
}

void LayerTreeHost::setViewOverlayRootLayer(GraphicsLayer* graphicsLayer)
{
    if (m_overlayCompositingLayer == graphicsLayer)
        return;

    m_overlayCompositingLayer = graphicsLayer;
    updateRootLayer();
}

void LayerTreeHost::forceRepaint()
{
#if !HAVE(DISPLAY_LINK)
    // This is necessary for running layout tests. Since in this case we are not waiting for a UIProcess to reply nicely.
    // Instead we are just triggering forceRepaint. But we still want to have the scripted animation callbacks being executed.
    if (auto* frameView = m_webPage.localMainFrameView())
        frameView->updateLayoutAndStyleIfNeededRecursive();

    // We need to schedule another flush, otherwise the forced paint might cancel a later expected flush.
    m_forceFrameSync = true;
    scheduleLayerFlush();

    if (!m_isWaitingForRenderer)
        flushLayers();
#else
    if (m_isWaitingForRenderer) {
        m_pendingForceRepaint = true;
        return;
    }

    m_pendingForceRepaint = false;
    m_webPage.corePage()->forceRepaintAllFrames();
    m_forceFrameSync = true;
    cancelPendingLayerFlush();
    flushLayers();
    m_sceneState->waitUntilPaintingComplete();
#endif
}

void LayerTreeHost::forceRepaintAsync(CompletionHandler<void()>&& callback)
{
#if !HAVE(DISPLAY_LINK)
    scheduleLayerFlush();

    // We want a clean repaint, meaning that if we're currently waiting for the renderer
    // to finish an update, we'll have to schedule another flush when it's done.
    ASSERT(!m_forceRepaintAsync.callback);
    m_forceRepaintAsync.callback = WTFMove(callback);
    m_forceRepaintAsync.needsFreshFlush = m_scheduledWhileWaitingForRenderer;
#else
    ASSERT(!m_forceRepaintAsync.callback);
    m_forceRepaintAsync.callback = WTFMove(callback);
    forceRepaint();
    if (m_pendingForceRepaint)
        m_forceRepaintAsync.compositionRequestID = std::nullopt;
    else
        m_forceRepaintAsync.compositionRequestID = m_compositionRequestID;
#endif
}

#if PLATFORM(GTK) || PLATFORM(WPE)
void LayerTreeHost::ensureDrawing()
{
    m_forceFrameSync = true;
    scheduleLayerFlush();
}
#endif

void LayerTreeHost::sizeDidChange(const IntSize& size)
{
    m_pendingResize = true;
    if (m_isWaitingForRenderer)
        scheduleLayerFlush();
    else {
        cancelPendingLayerFlush();
        flushLayers();
    }
}

void LayerTreeHost::pauseRendering()
{
    m_isSuspended = true;
    m_compositor->suspend();
}

void LayerTreeHost::resumeRendering()
{
    m_isSuspended = false;
    m_compositor->resume();
    scheduleLayerFlush();
}

GraphicsLayerFactory* LayerTreeHost::graphicsLayerFactory()
{
    return this;
}

FloatRect LayerTreeHost::visibleContentsRect() const
{
    if (auto* localMainFrameView = m_webPage.localMainFrameView())
        return FloatRect({ }, localMainFrameView->sizeForVisibleContent(ScrollableArea::VisibleContentRectIncludesScrollbars::Yes));
    return m_webPage.bounds();
}

void LayerTreeHost::backgroundColorDidChange()
{
    m_compositor->backgroundColorDidChange();
}

void LayerTreeHost::attachLayer(CoordinatedPlatformLayer& layer)
{
#if ENABLE(DAMAGE_TRACKING)
    layer.setDamagePropagation(webPage().corePage()->settings().propagateDamagingInformation());
#endif
    m_sceneState->addLayer(layer);
}

void LayerTreeHost::detachLayer(CoordinatedPlatformLayer& layer)
{
    m_sceneState->removeLayer(layer);
}

void LayerTreeHost::notifyCompositionRequired()
{
#if ENABLE(SCROLLING_THREAD)
    if (ScrollingThread::isCurrentThread()) {
        m_compositionRequiredInScrollingThread = true;
        return;
    }
#endif
    m_compositionRequired = true;
}

bool LayerTreeHost::isCompositionRequiredOrOngoing() const
{
    return m_compositionRequired || m_forceFrameSync || m_compositor->isActive();
}

void LayerTreeHost::requestComposition()
{
#if ENABLE(SCROLLING_THREAD)
    if (ScrollingThread::isCurrentThread()) {
        if (!m_compositionRequiredInScrollingThread)
            return;
        m_compositionRequiredInScrollingThread = false;
    }
#endif

    m_compositor->updateScene();
}

#if USE(CAIRO)
Cairo::PaintingEngine& LayerTreeHost::paintingEngine()
{
    return *m_paintingEngine;
}
#endif

Ref<CoordinatedImageBackingStore> LayerTreeHost::imageBackingStore(Ref<NativeImage>&& nativeImage)
{
    auto nativeImageID = CoordinatedImageBackingStore::uniqueIDForNativeImage(nativeImage.get());
    auto addResult = m_imageBackingStores.ensure(nativeImageID, [&] {
        return CoordinatedImageBackingStore::create(WTFMove(nativeImage));
    });
    return addResult.iterator->value;
}

Ref<GraphicsLayer> LayerTreeHost::createGraphicsLayer(GraphicsLayer::Type layerType, GraphicsLayerClient& client)
{
    return adoptRef(*new GraphicsLayerCoordinated(layerType, client, CoordinatedPlatformLayer::create(*this)));
}

#if !HAVE(DISPLAY_LINK)
RefPtr<DisplayRefreshMonitor> LayerTreeHost::createDisplayRefreshMonitor(PlatformDisplayID displayID)
{
    ASSERT(m_displayID == displayID);
    return Ref { m_compositor->displayRefreshMonitor() };
}

void LayerTreeHost::requestDisplayRefreshMonitorUpdate()
{
    // Flush layers to cause a repaint. If m_isWaitingForRenderer was true at this point, the layer
    // flush won't do anything, but that means there's a painting ongoing that will send the
    // display refresh notification when it's done.
    m_forceFrameSync = true;
    scheduleLayerFlush();
}

void LayerTreeHost::handleDisplayRefreshMonitorUpdate(bool hasBeenRescheduled)
{
    // Call renderNextFrame. If hasBeenRescheduled is true, the layer flush will force a repaint
    // that will cause the display refresh notification to come.
    renderNextFrame(hasBeenRescheduled);
}
#endif

void LayerTreeHost::willRenderFrame()
{
    if (auto* drawingArea = m_webPage.drawingArea())
        drawingArea->willStartRenderingUpdateDisplay();
}

void LayerTreeHost::didRenderFrame()
{
    if (auto* drawingArea = m_webPage.drawingArea())
        drawingArea->didCompleteRenderingUpdateDisplay();
}

#if HAVE(DISPLAY_LINK)
void LayerTreeHost::didComposite(uint32_t compositionResponseID)
{
    WTFBeginSignpost(this, DidComposite, "compositionRequestID %i, compositionResponseID %i", m_compositionRequestID, compositionResponseID);

    if (m_forceRepaintAsync.callback && m_forceRepaintAsync.compositionRequestID && compositionResponseID >= *m_forceRepaintAsync.compositionRequestID) {
        m_forceRepaintAsync.callback();
        m_forceRepaintAsync.compositionRequestID = std::nullopt;
    }

    if (!m_isWaitingForRenderer || m_compositionRequestID == compositionResponseID) {
        m_isWaitingForRenderer = false;
        bool scheduledWhileWaitingForRenderer = std::exchange(m_scheduledWhileWaitingForRenderer, false);
        if (m_pendingForceRepaint) {
            if (m_layerTreeStateIsFrozen) {
                if (m_forceRepaintAsync.callback) {
                    m_forceRepaintAsync.callback();
                    m_forceRepaintAsync.compositionRequestID = std::nullopt;
                }
            } else {
                forceRepaint();
                if (m_forceRepaintAsync.callback)
                    m_forceRepaintAsync.compositionRequestID = m_compositionRequestID;
            }
        } else if (!m_isSuspended && !m_layerTreeStateIsFrozen && (scheduledWhileWaitingForRenderer || m_layerFlushTimer.isActive())) {
            cancelPendingLayerFlush();
            flushLayers();
        }
    }
    WTFEndSignpost(this, DidComposite);
}
#endif

void LayerTreeHost::commitSceneState()
{
    m_isWaitingForRenderer = true;
    m_compositionRequestID = m_compositor->requestComposition();
    WTFEmitSignpost(this, CommitSceneState, "compositionRequestID %i", m_compositionRequestID);
}

#if !HAVE(DISPLAY_LINK)
void LayerTreeHost::renderNextFrame(bool forceRepaint)
{
    WTFBeginSignpost(this, RenderNextFrame);

    m_isWaitingForRenderer = false;
    bool scheduledWhileWaitingForRenderer = std::exchange(m_scheduledWhileWaitingForRenderer, false);

    if (m_forceRepaintAsync.callback) {
        // If the asynchronous force-repaint needs a separate fresh flush, it was due to
        // the force-repaint request being registered while CoordinatedLayerTreeHost was
        // waiting for the renderer to finish an update.
        ASSERT(!m_forceRepaintAsync.needsFreshFlush || scheduledWhileWaitingForRenderer);

        // Execute the callback if another layer flush and the subsequent state update
        // aren't needed. If they are, the callback will be executed when this function
        // is called after the next update.
        if (!m_forceRepaintAsync.needsFreshFlush)
            m_forceRepaintAsync.callback();
        m_forceRepaintAsync.needsFreshFlush = false;
    }

    if (scheduledWhileWaitingForRenderer || m_layerFlushTimer.isActive() || forceRepaint) {
        m_layerFlushTimer.stop();
        if (forceRepaint)
            m_forceFrameSync = true;
        layerFlushTimerFired();
    }

    WTFEndSignpost(this, RenderNextFrame);
}
#endif

#if PLATFORM(GTK)
FloatPoint LayerTreeHost::constrainTransientZoomOrigin(double scale, FloatPoint origin) const
{
    auto* frameView = m_webPage.localMainFrameView();
    if (!frameView)
        return origin;

    FloatRect visibleContentRect = frameView->visibleContentRectIncludingScrollbars();

    FloatPoint constrainedOrigin = visibleContentRect.location();
    constrainedOrigin.moveBy(-origin);

    IntSize scaledTotalContentsSize = frameView->totalContentsSize();
    scaledTotalContentsSize.scale(scale * m_webPage.viewScaleFactor() / m_webPage.totalScaleFactor());

    // Scaling may have exposed the overhang area, so we need to constrain the final
    // layer position exactly like scrolling will once it's committed, to ensure that
    // scrolling doesn't make the view jump.
    constrainedOrigin = ScrollableArea::constrainScrollPositionForOverhang(roundedIntRect(visibleContentRect),
        scaledTotalContentsSize, roundedIntPoint(constrainedOrigin), frameView->scrollOrigin(),
        frameView->headerHeight(), frameView->footerHeight());
    constrainedOrigin.moveBy(-visibleContentRect.location());
    constrainedOrigin = -constrainedOrigin;

    return constrainedOrigin;
}

CoordinatedPlatformLayer* LayerTreeHost::layerForTransientZoom() const
{
    auto* frameView = m_webPage.localMainFrameView();
    if (!frameView)
        return nullptr;

    RenderLayerBacking* renderViewBacking = frameView->renderView()->layer()->backing();
    if (!renderViewBacking)
        return nullptr;

    auto* scaledLayer = renderViewBacking->contentsContainmentLayer();
    if (!scaledLayer)
        scaledLayer = renderViewBacking->graphicsLayer();
    ASSERT(scaledLayer);
    return &downcast<GraphicsLayerCoordinated>(*scaledLayer).coordinatedPlatformLayer();
}

void LayerTreeHost::applyTransientZoomToLayers(double scale, FloatPoint origin)
{
    // FIXME: Scrollbars should stay in-place and change height while zooming.
    FloatPoint constrainedOrigin = constrainTransientZoomOrigin(scale, origin);
    auto* zoomLayer = layerForTransientZoom();

    TransformationMatrix transform;
    transform.translate(constrainedOrigin.x(), constrainedOrigin.y());
    transform.scale(scale);

    zoomLayer->setTransform(transform);
    zoomLayer->setAnchorPoint(FloatPoint3D());
    zoomLayer->setPosition(FloatPoint());
}

void LayerTreeHost::adjustTransientZoom(double scale, FloatPoint origin)
{
    m_transientZoom = true;
    m_transientZoomScale = scale;
    m_transientZoomOrigin = origin;

    applyTransientZoomToLayers(m_transientZoomScale, m_transientZoomOrigin);
}

void LayerTreeHost::commitTransientZoom(double scale, FloatPoint origin)
{
    if (m_transientZoomScale == scale) {
        // If the page scale is already the target scale, setPageScaleFactor() will short-circuit
        // and not apply the transform, so we can't depend on it to do so.
        TransformationMatrix finalTransform;
        finalTransform.scale(scale);

        layerForTransientZoom()->setTransform(finalTransform);
    }

    m_transientZoom = false;
    m_transientZoomScale = 1;
    m_transientZoomOrigin = FloatPoint();
}
#endif

#if PLATFORM(WPE) && USE(GBM) && ENABLE(WPE_PLATFORM)
void LayerTreeHost::preferredBufferFormatsDidChange()
{
    m_compositor->preferredBufferFormatsDidChange();
}
#endif

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)
