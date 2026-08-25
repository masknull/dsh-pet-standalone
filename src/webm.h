// webm.h — pristine-quality animation backend: upstream thumb WebM (VP9 + alpha)
// embedded as raw bytes, demuxed in memory and decoded per-frame with libvpx.
//
// Why this exists (v7): the user rejected the v2 .pka lossy downscale
// (256x144@10fps/pal96). The original dsh-pet thumbnails are 640x360 VP9-alpha
// WebM at 24 fps, ~46 MB for all 91. We embed the ORIGINAL bytes (no re-encode,
// no resample — the "zip in memory" model the user asked for) and decode them
// at runtime with a statically-linked libvpx. Only the current frame's RGBA
// buffer (~0.92 MB) is retained between frames; decoding is sequential and the
// decoders are reset and replayed on loop wrap.
//
// Container (verified against the actual 91 upstream files):
//   EBML header -> Segment -> Info(TimecodeScale, Duration)
//                           -> Tracks(TrackEntry: CodecID=V_VP9, PixelWidth=640,
//                                     PixelHeight=360, DefaultDuration=41666666ns,
//                                     AlphaMode=1)
//                           -> Cluster(Timecode) -> BlockGroup
//                               -> Block (0xA1, VP9 bitstream, main image)
//                               -> BlockAdditional (0x75A1, VP9 bitstream,
//                                                    alpha plane image)
//   AlphaMode=1 means the BlockAdditional with BlockAddID 1 is the alpha plane
//   (same signalling Chromium uses for <video> alpha WebM).
//
// Decode: two vpx_codec VP9 decoder contexts (main + alpha). Each decoded
// "alpha" frame is a full-res video frame whose Y plane IS the alpha channel
// (U/V ignored). The main frame is YUV420 -> BT.601 limited range -> RGBA,
// alpha composited, output as straight BGRA (identical convention to
// AnimPack::decodeFrame, so the pet renderer is unchanged).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct WebmFrameRef {
    uint32_t blockOff = 0;   // absolute offset of the VP9 bitstream (main image)
    uint32_t blockLen = 0;
    uint32_t alphaOff = 0;   // absolute offset of the alpha-plane bitstream (0 = none)
    uint32_t alphaLen = 0;
};

class WebmVideo {
public:
    WebmVideo() = default;
    ~WebmVideo();                       // destroys both decoder contexts
    WebmVideo(const WebmVideo&) = delete;
    WebmVideo& operator=(const WebmVideo&) = delete;

    // Parse a WebM held at [data, data+len). Keeps a POINTER (no copy) — the
    // caller guarantees the buffer outlives this object (RCDATA resource or an
    // owned vector). Idempotent: a second open() replaces the previous state.
    bool open(const uint8_t* data, size_t len);
    bool ok() const { return ok_; }

    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    uint32_t frameCount() const { return (uint32_t)frames_.size(); }
    double fps() const { return fps_; }                 // 24.0 for upstream thumbs
    double durationMs() const { return durationMs_; }   // from container Duration
    double duration() const { return fps_ > 0 ? frameCount() / fps_ : 0.0; }
    const char* error() const { return error_.c_str(); }
    const std::vector<WebmFrameRef>& frames() const { return frames_; }

    // Decode frame i (sequential; random access restarts from the first frame).
    // out receives STRAIGHT BGRA, w*h*4 bytes (alpha in byte 3). scratch is
    // unused by this backend (kept for interface parity with AnimPack).
    // Const like AnimPack::decodeFrame: the decode cache is mutable and the
    // caller must not go through the same instance concurrently.
    bool decodeFrame(uint32_t i, std::vector<uint8_t>& scratch,
                     std::vector<uint8_t>& out) const;

    // Either decode() stamp that reported and matched (i == pos_+1), or the
    // number of frames re-decoded after a seek/wrap since the last successful
    // decode — used by the selftest to measure real decode throughput.
    uint32_t lastDecodedSpan() const { return lastDecodedSpan_; }
    uint32_t alphaDecodedFrames() const { return alphaDecoded_; }  // fresh alpha planes decoded
    uint32_t alphaHeldFrames() const { return alphaHeld_; }        // alpha planes held from previous

    // Monochrome gray source (the dual-stream alpha masks): libvpx reports
    // UNDEFINED chroma content for plain-gray VP9, so decodeOne must force
    // neutral U/V and take RGB straight from Y. Set on the alpha companion
    // stream only — colour main streams keep full BT.709 conversion.
    void setGrayOnly(bool g) { grayOnly_ = g; }
    bool grayOnly() const { return grayOnly_; }

    // Release all decoder resources (vpx contexts, threads) AND the per-anim
    // scratch buffers. Called when switching away from this animation to free
    // memory and reduce thread count. v9.1: also drops cur_/alphaHold_
    // capacity — keeping them pinned for all 91 anims was the linear private
    // memory growth (each vector holds up to w*h*4 bytes after playing).
    void releaseDecoders() {
        destroyCodecs(); pos_ = UINT32_MAX; lastDecodedSpan_ = 0;
        alphaValid_ = false;
        std::vector<uint8_t>().swap(alphaHold_);
        std::vector<uint8_t>().swap(cur_);
    }

private:
    const uint8_t* data_ = nullptr;
    size_t len_ = 0;
    bool ok_ = false;
    bool grayOnly_ = false;
    uint32_t w_ = 0, h_ = 0;
    double fps_ = 24.0;
    double durationMs_ = 0;
    uint64_t timecodeScale_ = 1000000;
    std::string error_;
    std::vector<WebmFrameRef> frames_;

    // decoder state (main + alpha) — mutable like AnimPack's decode caches
    mutable void* dec_[2] = {nullptr, nullptr};     // vpx_codec_ctx_t*
    mutable uint32_t pos_ = UINT32_MAX;             // last decoded frame index
    mutable uint32_t lastDecodedSpan_ = 0;
    mutable std::vector<uint8_t> cur_;              // current decoded frame (RGBA scratch)
    mutable std::vector<uint8_t> alphaHold_;        // last good alpha plane (w*h bytes)
    mutable bool alphaValid_ = false;
    mutable uint32_t alphaDecoded_ = 0;             // diagnostics: fresh vs held
    mutable uint32_t alphaHeld_ = 0;
    mutable uint32_t lastAlphaTryFrame_ = UINT32_MAX;

    bool ensureCodecs() const;
    void destroyCodecs() const;
    bool rebuildAlpha() const;
    bool decodeOne(uint32_t i, std::vector<uint8_t>& rgba) const;  // writes rgba
};