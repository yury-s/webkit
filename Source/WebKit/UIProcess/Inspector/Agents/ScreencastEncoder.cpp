/*
 * Copyright (c) 2010, The WebM Project authors. All rights reserved.
 * Copyright (c) 2013 The Chromium Authors. All rights reserved.
 * Copyright (C) 2020 Microsoft Corporation.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ScreencastEncoder.h"

#include "WebMFileWriter.h"
#include <algorithm>
#include <libyuv.h>
#include <vpx/vp8.h>
#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>
#include <wtf/Compiler.h>
#include <wtf/RunLoop.h>
#include <wtf/UniqueArray.h>
#include <wtf/WorkQueue.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringConcatenateNumbers.h>

#if USE(SKIA) && !PLATFORM(GTK)
#include <skia/core/SkBitmap.h>
#include <skia/core/SkCanvas.h>
#include <skia/core/SkData.h>
#include <skia/core/SkImage.h>
#endif

#if USE(CAIRO) || PLATFORM(GTK)
#include <WebCore/RefPtrCairo.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

using namespace WebCore;

namespace WebKit {

namespace {

struct VpxCodecDeleter {
  void operator()(vpx_codec_ctx_t* codec) {
    if (codec) {
        vpx_codec_err_t ret = vpx_codec_destroy(codec);
        if (ret != VPX_CODEC_OK)
            fprintf(stderr, "Failed to encode frame: %s\n", vpx_codec_error(codec));
    }
  }
};

using ScopedVpxCodec = std::unique_ptr<vpx_codec_ctx_t, VpxCodecDeleter>;

// Number of timebase unints per one frame.
constexpr int timeScale = 1000;

// Defines the dimension of a macro block. This is used to compute the active
// map for the encoder.
const int kMacroBlockSize = 16;

void createImage(unsigned int width, unsigned int height,
                 std::unique_ptr<vpx_image_t>& out_image,
                 std::unique_ptr<uint8_t[]>& out_image_buffer) {
  std::unique_ptr<vpx_image_t> image(new vpx_image_t());
  memset(image.get(), 0, sizeof(vpx_image_t));

  // libvpx seems to require both to be assigned.
  image->d_w = width;
  image->w = width;
  image->d_h = height;
  image->h = height;

  // I420
  image->fmt = VPX_IMG_FMT_YV12;
  image->x_chroma_shift = 1;
  image->y_chroma_shift = 1;

  // libyuv's fast-path requires 16-byte aligned pointers and strides, so pad
  // the Y, U and V planes' strides to multiples of 16 bytes.
  const int y_stride = ((image->w - 1) & ~15) + 16;
  const int uv_unaligned_stride = y_stride >> image->x_chroma_shift;
  const int uv_stride = ((uv_unaligned_stride - 1) & ~15) + 16;

  // libvpx accesses the source image in macro blocks, and will over-read
  // if the image is not padded out to the next macroblock: crbug.com/119633.
  // Pad the Y, U and V planes' height out to compensate.
  // Assuming macroblocks are 16x16, aligning the planes' strides above also
  // macroblock aligned them.
  static_assert(kMacroBlockSize == 16, "macroblock_size_not_16");
  const int y_rows = ((image->h - 1) & ~(kMacroBlockSize-1)) + kMacroBlockSize;
  const int uv_rows = y_rows >> image->y_chroma_shift;

  // Allocate a YUV buffer large enough for the aligned data & padding.
  const int buffer_size = y_stride * y_rows + 2*uv_stride * uv_rows;
  std::unique_ptr<uint8_t[]> image_buffer(new uint8_t[buffer_size]);

  // Reset image value to 128 so we just need to fill in the y plane.
  memset(image_buffer.get(), 128, buffer_size);

  // Fill in the information for |image_|.
  unsigned char* uchar_buffer =
      reinterpret_cast<unsigned char*>(image_buffer.get());
  image->planes[0] = uchar_buffer;
  image->planes[1] = image->planes[0] + y_stride * y_rows;
  image->planes[2] = image->planes[1] + uv_stride * uv_rows;
  image->stride[0] = y_stride;
  image->stride[1] = uv_stride;
  image->stride[2] = uv_stride;

  out_image = std::move(image);
  out_image_buffer = std::move(image_buffer);
}

} // namespace

class ScreencastEncoder::VPXFrame {
    WTF_MAKE_NONCOPYABLE(VPXFrame);
    WTF_MAKE_FAST_ALLOCATED;
public:
#if USE(SKIA) && !PLATFORM(GTK)
    explicit VPXFrame(sk_sp<SkImage>&& surface)
        : m_surface(WTFMove(surface))
    { }
#elif USE(CAIRO) || PLATFORM(GTK)
    explicit VPXFrame(RefPtr<cairo_surface_t>&& surface)
        : m_surface(WTFMove(surface))
    { }
#elif PLATFORM(MAC)
    VPXFrame(RetainPtr<CGImageRef> windowImage, int offsetTop)
        : m_windowImage(WTFMove(windowImage))
        , m_offsetTop(offsetTop)
    { }
#endif

    void setDuration(Seconds duration) { m_duration = duration; }
    Seconds duration() const { return m_duration; }

    void convertToVpxImage(vpx_image_t* image)
    {
#if USE(SKIA) && !PLATFORM(GTK)
        // Convert the updated region to YUV ready for encoding.
        SkImageInfo info = SkImageInfo::Make(m_surface->width(), m_surface->height(), kN32_SkColorType, kPremul_SkAlphaType);
        int argb_stride = info.minRowBytes();
        size_t bufferSize = info.computeByteSize(argb_stride);
        UniqueArray<uint8_t> buffer = makeUniqueArray<uint8_t>(bufferSize);
        uint8_t* argb_data = buffer.get();
        if (!m_surface->readPixels(info, argb_data, argb_stride, 0, 0))
            fprintf(stderr, "Read SkImage to ARGB buffer\n");
#elif USE(CAIRO) || PLATFORM(GTK)
        // Convert the updated region to YUV ready for encoding.
        const uint8_t* argb_data = cairo_image_surface_get_data(m_surface.get());
        int argb_stride = cairo_image_surface_get_stride(m_surface.get());
#elif PLATFORM(MAC)
        int argb_stride = image->w * 4;
        UniqueArray<uint8_t> buffer = makeUniqueArray<uint8_t>(argb_stride * image->h);
        uint8_t* argb_data = buffer.get();
        ScreencastEncoder::imageToARGB(m_windowImage.get(), argb_data, image->w, image->h, m_offsetTop);
#endif
        const int y_stride = image->stride[0];
        ASSERT(image->stride[1] == image->stride[2]);
        const int uv_stride = image->stride[1];
        uint8_t* y_data = image->planes[0];
        uint8_t* u_data = image->planes[1];
        uint8_t* v_data = image->planes[2];

        // TODO: redraw only damaged regions?
        libyuv::ARGBToI420(argb_data, argb_stride,
                            y_data, y_stride,
                            u_data, uv_stride,
                            v_data, uv_stride,
                            image->w, image->h);
    }

private:
#if USE(SKIA) && !PLATFORM(GTK)
    sk_sp<SkImage> m_surface;
#elif USE(CAIRO) || PLATFORM(GTK)
    RefPtr<cairo_surface_t> m_surface;
#elif PLATFORM(MAC)
    RetainPtr<CGImageRef> m_windowImage;
    int m_offsetTop { 0 };
#endif
    Seconds m_duration;
};


class ScreencastEncoder::VPXCodec {
public:
    VPXCodec(ScopedVpxCodec codec, vpx_codec_enc_cfg_t cfg, FILE* file)
        : m_encoderQueue(WorkQueue::create("Screencast encoder"_s))
        , m_codec(WTFMove(codec))
        , m_cfg(cfg)
        , m_file(file)
        , m_writer(new WebMFileWriter(file, &m_cfg))
    {
        createImage(cfg.g_w, cfg.g_h, m_image, m_imageBuffer);
    }

    void encodeFrameAsync(std::unique_ptr<VPXFrame>&& frame)
    {
        m_encoderQueue->dispatch([this, frame = WTFMove(frame)] {
            frame->convertToVpxImage(m_image.get());
            double frameCount = frame->duration().seconds() * fps;
            // For long duration repeat frame at 1 fps to ensure last frame duration is short enough.
            // TODO: figure out why simply passing duration doesn't work well.
            for (;frameCount > 1.5; frameCount -= 1) {
                encodeFrame(m_image.get(), timeScale);
            }
            encodeFrame(m_image.get(), std::max<int>(1, frameCount * timeScale));
        });
    }

    void finishAsync(Function<void()>&& callback)
    {
        m_encoderQueue->dispatch([this, callback = WTFMove(callback)] {
            finish();
            callback();
        });
    }

private:
    bool encodeFrame(vpx_image_t *img, int duration)
    {
        vpx_codec_iter_t iter = nullptr;
        const vpx_codec_cx_pkt_t *pkt = nullptr;
        int flags = 0;
        const vpx_codec_err_t res = vpx_codec_encode(m_codec.get(), img, m_pts, duration, flags, VPX_DL_REALTIME);
        if (res != VPX_CODEC_OK) {
            fprintf(stderr, "Failed to encode frame: %s\n", vpx_codec_error(m_codec.get()));
            return false;
        }

        bool gotPkts = false;
        while ((pkt = vpx_codec_get_cx_data(m_codec.get(), &iter)) != nullptr) {
            gotPkts = true;

            if (pkt->kind == VPX_CODEC_CX_FRAME_PKT) {
                if (!m_writer->writeFrame(pkt)) {
                    fprintf(stderr, "Failed to write compressed frame\n");
                    return false;
                }
                ++m_frameCount;
                m_pts += pkt->data.frame.duration;
            }
        }

        return gotPkts;
    }

    void finish()
    {
        // Flush encoder.
        while (encodeFrame(nullptr, 1))
            ++m_frameCount;

        m_writer->finish();
        fclose(m_file);
    }

    Ref<WorkQueue> m_encoderQueue;
    ScopedVpxCodec m_codec;
    vpx_codec_enc_cfg_t m_cfg;
    FILE* m_file { nullptr };
    std::unique_ptr<WebMFileWriter> m_writer;
    int m_frameCount { 0 };
    int64_t m_pts { 0 };
    std::unique_ptr<uint8_t[]> m_imageBuffer;
    std::unique_ptr<vpx_image_t> m_image;
};

ScreencastEncoder::ScreencastEncoder(std::unique_ptr<VPXCodec>&& vpxCodec, IntSize size)
    : m_vpxCodec(WTFMove(vpxCodec))
    , m_size(size)
{
    ASSERT(!size.isZero());
}

ScreencastEncoder::~ScreencastEncoder()
{
}

RefPtr<ScreencastEncoder> ScreencastEncoder::create(String& errorString, const String& filePath, IntSize size)
{
    vpx_codec_iface_t* codec_interface = vpx_codec_vp8_cx();
    if (!codec_interface) {
        errorString = "Codec not found."_s;
        return nullptr;
    }

    if (size.width() <= 0 || size.height() <= 0 || (size.width() % 2) != 0 || (size.height() % 2) != 0) {
        errorString = makeString("Invalid frame size: "_s, size.width(), "x"_s, size.height());
        return nullptr;
    }

    vpx_codec_enc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    vpx_codec_err_t error = vpx_codec_enc_config_default(codec_interface, &cfg, 0);
    if (error) {
        errorString = makeString("Failed to get default codec config: "_s, span(vpx_codec_err_to_string(error)));
        return nullptr;
    }

    cfg.g_w = size.width();
    cfg.g_h = size.height();
    cfg.g_timebase.num = 1;
    cfg.g_timebase.den = fps * timeScale;
    cfg.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;

    ScopedVpxCodec codec(new vpx_codec_ctx_t);
    if (vpx_codec_enc_init(codec.get(), codec_interface, &cfg, 0)) {
        errorString = makeString("Failed to initialize encoder: "_s, span(vpx_codec_error(codec.get())));
        return nullptr;
    }

    FILE* file = fopen(filePath.utf8().data(), "wb");
    if (!file) {
        errorString = makeString("Failed to open file '"_s, filePath, "' for writing: "_s, span(strerror(errno)));
        return nullptr;
    }

    std::unique_ptr<VPXCodec> vpxCodec(new VPXCodec(WTFMove(codec), cfg, file));
    return adoptRef(new ScreencastEncoder(WTFMove(vpxCodec), size));
}

void ScreencastEncoder::flushLastFrame()
{
    MonotonicTime now = MonotonicTime::now();
    if (m_lastFrameTimestamp) {
        // If previous frame encoding failed for some rason leave the timestampt intact.
        if (!m_lastFrame)
            return;

        Seconds seconds = now - m_lastFrameTimestamp;
        m_lastFrame->setDuration(seconds);
        m_vpxCodec->encodeFrameAsync(WTFMove(m_lastFrame));
    }
    m_lastFrameTimestamp = now;
}

#if USE(SKIA) && !PLATFORM(GTK)
void ScreencastEncoder::encodeFrame(sk_sp<SkImage>&& image, IntSize size)
{
    flushLastFrame();
    // Note that in WPE drawing area size is updated asynchronously and may differ from acutal
    // size of the surface.
    if (size.isZero()) {
        return;
    }
    SkBitmap surface;
    surface.allocPixels(SkImageInfo::Make(m_size.width(), m_size.height(), kBGRA_8888_SkColorType, kPremul_SkAlphaType));
    SkCanvas canvas(surface);
    SkMatrix transform;
    if (size.width() > m_size.width() || size.height() > m_size.height()) {
        // If no scale is specified shrink to fit the frame.
        double scale = std::min(static_cast<double>(m_size.width()) / size.width(),
                                static_cast<double>(m_size.height()) / size.height());
        transform.setScale(scale, scale);
        canvas.setMatrix(transform);
    }
    // Record top left part of the drawing area that fits into the frame.
    canvas.drawImage(image, 0, 0);
    m_lastFrame = makeUnique<VPXFrame>(surface.asImage());
}
#elif USE(CAIRO) || PLATFORM(GTK)
void ScreencastEncoder::encodeFrame(cairo_surface_t* drawingAreaSurface, IntSize size)
{
    flushLastFrame();
    // Note that in WPE drawing area size is updated asynchronously and may differ from acutal
    // size of the surface.
    if (size.isZero()) {
        return;
    }

    RefPtr<cairo_surface_t> surface = adoptRef(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, m_size.width(), m_size.height()));
    {
        RefPtr<cairo_t> cr = adoptRef(cairo_create(surface.get()));

        cairo_matrix_t transform;
        if (size.width() > m_size.width() || size.height() > m_size.height()) {
            // If no scale is specified shrink to fit the frame.
            double scale = std::min(static_cast<double>(m_size.width()) / size.width(),
                                    static_cast<double>(m_size.height()) / size.height());
            cairo_matrix_init_scale(&transform, scale, scale);
            cairo_transform(cr.get(), &transform);
        }

        // Record top left part of the drawing area that fits into the frame.
        cairo_set_source_surface(cr.get(), drawingAreaSurface, 0, 0);
        cairo_paint(cr.get());
    }
    cairo_surface_flush(surface.get());

    m_lastFrame = makeUnique<VPXFrame>(WTFMove(surface));
}
#elif PLATFORM(MAC)
void ScreencastEncoder::encodeFrame(RetainPtr<CGImageRef>&& windowImage)
{
    flushLastFrame();

    m_lastFrame = makeUnique<VPXFrame>(WTFMove(windowImage), m_offsetTop);
}
#endif

void ScreencastEncoder::finish(Function<void()>&& callback)
{
    if (!m_vpxCodec) {
        callback();
        return;
    }

    flushLastFrame();
    m_vpxCodec->finishAsync([protectRef = Ref { *this }, callback = WTFMove(callback)] () mutable {
        RunLoop::main().dispatch([callback = WTFMove(callback)] {
            callback();
        });
    });
}


} // namespace WebKit

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
