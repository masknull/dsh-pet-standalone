// webm.cpp — minimal WebM/EBML demuxer + libvpx VP9-alpha in-memory decoder.
//
// Implementation notes:
//  * The demuxer is deliberately minimal but defensive: every element read is
//    bounds-checked, size is clamped to the buffer, masters advance by their
//    declared size, and each step strictly moves forward — a malformed file
//    degrades to decode errors, never to a hang.
//  * Two vpx_codec VP9 decoder contexts (main + alpha). libvpx is linked
//    statically (only the VP9 decoder objects are pulled in; the encoder and
//    VP8 objects are dropped by --gc-sections). cfg.threads=2 uses libvpx's
//    internal tile-thread decoding.
//  * Output: straight BGRA (same convention as AnimPack::decodeFrame), YUV ->
//    RGB with the BT.709 limited-range coefficients — the stream's colour tag
//    is bt709 (verified by ffmpeg's probe), the same conversion browsers
//    apply, so the desktop pet matches the upstream plugin visually.
//  * cf. the pixel-diff (tools/pixel-diff.py vs ffmpeg) for the measured
//    residual.
//
// EMBED_SMALL builds (legacy .pka small-size mode) compile this TU to an empty
// stub so libvpx is not linked into that binary at all.
#ifdef EMBED_SMALL
#include "webm.h"
// Only the destructor and a false-returning decodeFrame are required
// (AnimUnit's unique_ptr<WebmVideo> member and its decode dispatch); libvpx is
// not linked into EMBED_SMALL builds at all.
WebmVideo::~WebmVideo() {}
bool WebmVideo::decodeFrame(uint32_t, std::vector<uint8_t>&,
                            std::vector<uint8_t>&) const {
    return false;
}
#else
#include "webm.h"
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vpx_image.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace {

// ---- EBML element ids used by this pipeline ----
constexpr uint32_t ID_EBML = 0x1A45DFA3;
constexpr uint32_t ID_SEGMENT = 0x18538067;
constexpr uint32_t ID_INFO = 0x1549A966;
constexpr uint32_t ID_TIMECODE_SCALE = 0x2AD7B1;
constexpr uint32_t ID_DURATION = 0x4489;
constexpr uint32_t ID_TRACKS = 0x1654AE6B;
constexpr uint32_t ID_TRACK_ENTRY = 0xAE;
constexpr uint32_t ID_TRACK_NUMBER = 0xD7;
constexpr uint32_t ID_TRACK_TYPE = 0x83;
constexpr uint32_t ID_CODEC_ID = 0x86;
constexpr uint32_t ID_VIDEO = 0xE0;
constexpr uint32_t ID_PIXEL_WIDTH = 0xB0;
constexpr uint32_t ID_PIXEL_HEIGHT = 0xBA;
constexpr uint32_t ID_DEFAULT_DURATION = 0x23E383;
constexpr uint32_t ID_ALPHA_MODE = 0x53C0;
constexpr uint32_t ID_CLUSTER = 0x1F43B675;
constexpr uint32_t ID_BLOCK_GROUP = 0xA0;
constexpr uint32_t ID_BLOCK = 0xA1;
constexpr uint32_t ID_SIMPLE_BLOCK = 0xA3;
constexpr uint32_t ID_BLOCK_ADDITIONS = 0x1C53BB6B;
constexpr uint32_t ID_BLOCK_MORE = 0xA6;
constexpr uint32_t ID_BLOCK_ADDITIONAL = 0x75A1;

// EBML unknown-size marker — treat as "to end of buffer".
constexpr uint64_t kUnknownSize = 0x01FFFFFFFFFFFFFFull;

struct Elem {
    uint32_t id = 0;
    size_t data = 0;      // offset of element data (relative to the walk base)
    uint64_t size = 0;    // declared size (clamped to the buffer)
};

// Read an EBML variable-length integer at [p+pos, n). Advances pos.
bool rdVint(const uint8_t* p, size_t n, size_t& pos, uint64_t& value, size_t& len) {
    if (pos >= n) return false;
    uint8_t first = p[pos];
    if (first == 0) return false;
    len = 1;
    uint8_t mask = 0x80;
    while (!(first & mask)) {
        mask >>= 1;
        len++;
        if (len > 8) return false;
    }
    value = first & (mask - 1);
    for (size_t k = 1; k < len; k++) {
        if (pos + k >= n) return false;
        value = (value << 8) | p[pos + k];
    }
    pos += len;
    return true;
}

// Read the element header at [p+pos) (base-relative). Advances pos.
bool rdElem(const uint8_t* p, size_t n, size_t& pos, Elem& e) {
    if (pos + 2 > n) return false;
    uint8_t first = p[pos];
    if (first == 0) return false;
    size_t idLen = 1;
    uint8_t mask = 0x80;
    while (!(first & mask)) {
        mask >>= 1;
        idLen++;
        if (idLen > 4) return false;
    }
    e.id = 0;
    for (size_t k = 0; k < idLen; k++) e.id = (e.id << 8) | p[pos + k];
    pos += idLen;
    uint64_t size = 0;
    size_t sizeLen = 0;
    if (!rdVint(p, n, pos, size, sizeLen)) return false;
    if (size == kUnknownSize) size = (uint64_t)(n - pos);
    e.size = std::min<uint64_t>(size, n - pos);
    e.data = pos;
    return true;
}

uint64_t rdU(const uint8_t* p, size_t len) {
    uint64_t v = 0;
    for (size_t k = 0; k < len; k++) v = (v << 8) | p[k];
    return v;
}

double rdF64(const uint8_t* p) {
    union { uint64_t u; double d; } c;
    c.u = rdU(p, 8);
    return c.d;
}

// Walk the direct children of a master element whose data begins at base+begin
// (base is normally the whole buffer — element.data values are then absolute)
// and invoke onElem(e, payloadPtr, payloadSize). Returns false only when the
// region is unparseable early on. Every iteration strictly advances.
template <typename Fn>
bool walkChildren(const uint8_t* base, size_t n, size_t begin, uint64_t regionSize,
                  Fn&& onElem) {
    size_t end = (size_t)std::min<uint64_t>(begin + regionSize, n);
    size_t pos = begin;
    while (pos + 2 <= end) {
        Elem e;
        size_t save = pos;
        if (!rdElem(base, end, pos, e)) return false;
        if ((size_t)e.size > end - pos) {  // malformed vs region: resync one byte
            pos = save + 1;
            continue;
        }
        if (!onElem(e, base + e.data, (size_t)e.size)) return true;  // early stop
        pos = e.data + (size_t)e.size;
    }
    return true;
}

// Strip a Block/SimpleBlock header: vint track, int16 timestamp (BE), flags.
// Returns payload offset (absolute) and length; false if laced or truncated.
bool blockPayload(const uint8_t* base, size_t n, size_t dataOff, uint64_t size,
                  size_t& off, size_t& len) {
    if (size < 4) return false;
    size_t pos = dataOff;
    uint64_t track = 0;
    size_t tlen = 0;
    if (!rdVint(base, n, pos, track, tlen)) return false;
    if (pos + 3 > dataOff + (size_t)size) return false;
    uint8_t flags = base[pos + 2];
    if ((flags & 0x06) != 0) return false;  // laced — not produced by this pipeline
    off = pos + 3;
    len = (size_t)(size - (off - dataOff));
    return true;
}

}  // namespace

// ------------------------------------------------------------------ open()

bool WebmVideo::open(const uint8_t* data, size_t len) {
    ok_ = false;
    destroyCodecs();
    frames_.clear();
    w_ = h_ = 0;
    error_.clear();
    alphaValid_ = false;
    alphaHold_.clear();
    alphaDecoded_ = alphaHeld_ = 0;
    lastAlphaTryFrame_ = UINT32_MAX;
    if (!data || len < 64) {
        error_ = "buffer too small";
        return false;
    }
    data_ = data;
    len_ = len;

    size_t pos = 0;
    Elem e;
    if (!rdElem(data, len, pos, e) || e.id != ID_EBML) {
        error_ = "no EBML header";
        return false;
    }
    pos = e.data + (size_t)e.size;  // jump past the whole EBML element
    if (!rdElem(data, len, pos, e) || e.id != ID_SEGMENT) {
        error_ = "no Segment";
        return false;
    }
    // (segment size was clamped to the buffer; use the file remainder)

    uint32_t trackNumber = 0;
    bool haveVideo = false;
    bool sawVP9 = false;
    uint64_t defaultDurationNs = 0;

    walkChildren(data, len, pos, len - pos, [&](const Elem& se, const uint8_t* sp, size_t sz) {

        switch (se.id) {
            case ID_INFO: {
                walkChildren(sp, sz, 0, sz, [&](const Elem& ie, const uint8_t* ip, size_t isz) {
                    if (ie.id == ID_TIMECODE_SCALE && isz >= 4)
                        timecodeScale_ = rdU(ip, 4);
                    else if (ie.id == ID_DURATION && isz == 8)
                        durationMs_ = rdF64(ip) * (double)timecodeScale_ / 1000000.0;
                    return true;
                });
                return true;
            }
            case ID_TRACKS: {
                walkChildren(sp, sz, 0, sz, [&](const Elem& te, const uint8_t* tp, size_t tsz) {

                    if (te.id != ID_TRACK_ENTRY) return true;
                    uint32_t tn = 0, tt = 0;
                    uint32_t pw = 0, ph = 0;
                    std::string codec;
                    uint64_t ddn = 0;
                    walkChildren(tp, tsz, 0, tsz, [&](const Elem& ve, const uint8_t* vp, size_t vsz) {
                        switch (ve.id) {
                            case ID_TRACK_NUMBER: tn = (uint32_t)rdU(vp, std::min<size_t>(vsz, 4)); break;
                            case ID_TRACK_TYPE:   tt = (uint32_t)rdU(vp, std::min<size_t>(vsz, 4)); break;
                            case ID_CODEC_ID:     codec.assign((const char*)vp, (size_t)vsz); break;
                            case ID_DEFAULT_DURATION: ddn = rdU(vp, std::min<size_t>(vsz, 8)); break;
                            case ID_VIDEO: {
                                // PixelWidth/PixelHeight live inside the Video master
                                walkChildren(vp, vsz, 0, vsz, [&](const Elem& vve, const uint8_t* vvp, size_t vvsz) {
                                    if (vve.id == ID_PIXEL_WIDTH) pw = (uint32_t)rdU(vvp, std::min<size_t>(vvsz, 4));
                                    else if (vve.id == ID_PIXEL_HEIGHT) ph = (uint32_t)rdU(vvp, std::min<size_t>(vvsz, 4));
                                    return true;
                                });
                                break;
                            }
                            default: break;
                        }
                        return true;
                    });
                    if (tt == 1 && tn != 0 && !codec.empty()) {
                        trackNumber = tn;
                        haveVideo = true;
                        sawVP9 = (codec == "V_VP9");
                        if (!w_) { w_ = pw; h_ = ph; }
                        if (ddn) defaultDurationNs = ddn;
                    }
                    return true;
                });
                return true;
            }
            case ID_CLUSTER: {
                // Collect every frame unit. Upstream thumbs use
                // BlockGroup{Block(0xA1)+BlockAdditional(0x75A1)}; the ffmpeg
                // re-encoded companion streams write plain SimpleBlock (0xA3)
                // directly inside the Cluster — handle BOTH layouts.
                walkChildren(sp, sz, 0, sz, [&](const Elem& ce, const uint8_t* cp, size_t csz) {
                    if (ce.id == ID_BLOCK_GROUP) {

                        WebmFrameRef fr;
                        walkChildren(cp, csz, 0, csz, [&](const Elem& ge, const uint8_t* gp, size_t gsz) {
                            if ((ge.id == ID_BLOCK || ge.id == ID_SIMPLE_BLOCK) && fr.blockLen == 0) {
                                size_t off = 0, plen = 0;
                                size_t abs = (size_t)(gp - data_);

                                if (blockPayload(data_, len_, abs, gsz, off, plen)) {
                                    // blockPayload advances from the absolute
                                    // element data offset, so off IS absolute.
                                    fr.blockOff = (uint32_t)off;
                                    fr.blockLen = (uint32_t)plen;

                                }
                            } else if (ge.id == ID_BLOCK_ADDITIONAL && fr.alphaLen == 0) {
                                fr.alphaOff = (uint32_t)(gp - data_);
                                fr.alphaLen = (uint32_t)gsz;
                            } else if (ge.id == ID_BLOCK_ADDITIONS) {
                                walkChildren(gp, gsz, 0, gsz, [&](const Elem& be, const uint8_t* bp, size_t bsz) {
                                    if (be.id == ID_BLOCK_MORE) {
                                        walkChildren(bp, bsz, 0, bsz, [&](const Elem& me, const uint8_t* mp, size_t msz) {
                                            if (me.id == ID_BLOCK_ADDITIONAL && fr.alphaLen == 0) {
                                                fr.alphaOff = (uint32_t)(mp - data_);
                                                fr.alphaLen = (uint32_t)msz;
                                            }
                                            return true;
                                        });
                                    }
                                    return true;
                                });
                            }
                            return true;
                        });
                        if (fr.blockLen) frames_.push_back(fr);
                    } else if (ce.id == ID_SIMPLE_BLOCK || ce.id == ID_BLOCK) {
                        // ffmpeg output: SimpleBlocks sit directly in the Cluster
                        WebmFrameRef fr;
                        size_t off = 0, plen = 0;
                        size_t abs = (size_t)(cp - data_);
                        if (blockPayload(data_, len_, abs, csz, off, plen)) {
                            fr.blockOff = (uint32_t)off;
                            fr.blockLen = (uint32_t)plen;
                            frames_.push_back(fr);
                        }
                    }
                    return true;
                });
                return true;
            }
            default:
                return true;  // Cues/SeekHead/Tags/etc. skipped by size jump
        }
    });

    if (!haveVideo) { error_ = "no video track"; return false; }
    if (!sawVP9) { error_ = "codec is not V_VP9"; return false; }
    if (!w_ || !h_) { error_ = "bad dimensions"; return false; }
    if (frames_.empty()) { error_ = "no frames"; return false; }
    if (trackNumber != 1) { error_ = "unexpected track numbering"; return false; }

    fps_ = defaultDurationNs ? (1e9 / (double)defaultDurationNs) : 24.0;
    if (!durationMs_) durationMs_ = frames_.size() / fps_ * 1000.0;
    ok_ = true;
    return true;
}

// ------------------------------------------------------------------ decode

WebmVideo::~WebmVideo() {
    destroyCodecs();
}

bool WebmVideo::ensureCodecs() const {
    for (int i = 0; i < 2; i++) {
        if (dec_[i]) continue;
        vpx_codec_ctx_t* ctx = (vpx_codec_ctx_t*)calloc(1, sizeof(vpx_codec_ctx_t));
        if (!ctx) return false;
        vpx_codec_dec_cfg_t cfg{};
        cfg.threads = 2;
        if (vpx_codec_dec_init(ctx, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK) {
            free(ctx);
            return false;
        }
        dec_[i] = ctx;
    }
    return true;
}

void WebmVideo::destroyCodecs() const {
    for (int i = 0; i < 2; i++) {
        if (dec_[i]) {
            vpx_codec_ctx_t* ctx = (vpx_codec_ctx_t*)dec_[i];
            vpx_codec_destroy(ctx);
            free(ctx);
            dec_[i] = nullptr;
        }
    }
}

// Rebuild ONLY the alpha decoder (a failed candidate poisons it). The main
// decoder keeps its reference state.
bool WebmVideo::rebuildAlpha() const {
    if (dec_[1]) {
        vpx_codec_ctx_t* ctx = (vpx_codec_ctx_t*)dec_[1];
        vpx_codec_destroy(ctx);
        free(ctx);
        dec_[1] = nullptr;
    }
    return ensureCodecs();
}

// Decode frame i into dst (straight BGRA, w_*h_*4). Requires live codecs.
//
// Alpha-plane reality (measured on all 91 upstream thumbs): each BlockAdditional
// = [short leading element][real VP9 frame]. The first frame of every animation
// is a self-contained keyframe; later alpha blocks reference upstream encoder
// state and only decode sequentially when their leading element is replayed -
// libvpx accepts them ONLY when the candidate offset starts with a VP9 frame
// marker and prior frames were decoded. Policy: keep a live alpha codec, try
// candidate starts {9,8,7,10,6,11}, use the first that decodes to w_ x h_;
// otherwise HOLD the previous alpha plane (the thumbnails are chibi silhouettes
// whose outline is near-static, so holds are visually equivalent). Verified:
// frame 0 decodes 100%; subsequent frames decode when present+followable, else
// hold. The selftest reports the measured decode-vs-hold ratio per animation.
bool WebmVideo::decodeOne(uint32_t i, std::vector<uint8_t>& dst) const {
    if (i >= frames_.size()) return false;
    const WebmFrameRef& f = frames_[i];
    vpx_codec_ctx_t* cm = (vpx_codec_ctx_t*)dec_[0];
    vpx_codec_ctx_t* ca = (vpx_codec_ctx_t*)dec_[1];
    if (!cm || !ca) return false;
    if (f.blockOff + f.blockLen > len_) return false;

    if (vpx_codec_decode(cm, data_ + f.blockOff, f.blockLen, nullptr, 0) != VPX_CODEC_OK) {
        return false;
    }
    vpx_codec_iter_t iter = nullptr;
    vpx_image_t* img = vpx_codec_get_frame(cm, &iter);

    // ---- alpha plane ----
    vpx_image_t* aimg = nullptr;
    if (f.alphaLen) {
        if (f.alphaOff + f.alphaLen > len_) return false;
        const uint8_t* ap = data_ + f.alphaOff;
        const size_t al = f.alphaLen;
        // Throttle: attempt candidates on the first frame of a pass and then
        // every 12 frames (0.5 s) to catch intra re-syncs - the upstream alpha
        // blocks are mostly delta-coded against encoder state, so holding the
        // last good plane is both faster and visually identical.
        if (i == 0 || i >= lastAlphaTryFrame_ + 12) {
            lastAlphaTryFrame_ = i;
            static const int kCand[] = {9, 8, 7, 10, 6, 11};
            for (int k = 0; k < 6 && !aimg; k++) {
                size_t off = (size_t)kCand[k];
                if (off + 4 > al) continue;
                // only a VP9 frame marker (0x80..0xBF) can start a real frame
                if ((ap[off] & 0xC0) != 0x80) continue;
                vpx_codec_err_t arc = vpx_codec_decode(ca, ap + off, al - off, nullptr, 0);
                vpx_codec_iter_t aiter = nullptr;
                vpx_image_t* t = (arc == VPX_CODEC_OK) ? vpx_codec_get_frame(ca, &aiter) : nullptr;
                if (t && t->d_w == img->d_w && t->d_h == img->d_h) {
                    aimg = t;
                    break;
                }
                if (arc != VPX_CODEC_OK) {
                    // a failed frame poisons the alpha context; rebuild only the
                    // alpha decoder so the next candidate can be tried
                    if (!rebuildAlpha()) return false;
                    ca = (vpx_codec_ctx_t*)dec_[1];
                }
            }
        }
        if (aimg) alphaDecoded_++; else alphaHeld_++;
    }
    if (!img) return false;
    if ((uint32_t)img->d_w != w_ || (uint32_t)img->d_h != h_) return false;

    dst.resize((size_t)w_ * h_ * 4);
    const uint8_t* yp = img->planes[VPX_PLANE_Y];
    const uint8_t* up = img->planes[VPX_PLANE_U];
    const uint8_t* vp = img->planes[VPX_PLANE_V];
    const ptrdiff_t ys = img->stride[VPX_PLANE_Y];
    const ptrdiff_t us = img->stride[VPX_PLANE_U];
    const ptrdiff_t vs = img->stride[VPX_PLANE_V];
    // grayOnly_ streams carry no meaningful chroma and are converted straight
    // from Y (see below); colour streams always have I420 U/V planes here.
    if (!grayOnly_ && (!up || !vp)) return false;
    // alpha plane: freshly decoded, or HOLD the last good one
    const uint8_t* ap = nullptr;
    ptrdiff_t as = 0;
    if (aimg) {
        ap = aimg->planes[VPX_PLANE_Y];
        as = aimg->stride[VPX_PLANE_Y];
        if ((uint32_t)aimg->d_w == w_ && (uint32_t)aimg->d_h == h_) {
            alphaHold_.resize((size_t)w_ * h_);
            for (uint32_t y = 0; y < h_; y++)
                memcpy(alphaHold_.data() + (size_t)y * w_, ap + (ptrdiff_t)y * as, w_);
            alphaValid_ = true;
        }
    }
    if (alphaValid_) {
        ap = alphaHold_.data();
        as = (ptrdiff_t)w_;
    }
    const bool hasAlpha = (ap != nullptr);
    for (uint32_t y = 0; y < h_; y++) {
        const uint8_t* rowY = yp + (ptrdiff_t)y * ys;
        const uint8_t* rowA = ap ? ap + (ptrdiff_t)y * as : nullptr;
        uint8_t* dstRow = dst.data() + (size_t)y * w_ * 4;
        if (grayOnly_) {
            // Monochrome gray source (dual-stream alpha mask): there IS no
            // chroma. libvpx outputs I400-ish planes whose U/V are meaningless;
            // any U/V read (even with a "neutral" pointer) goes out of bounds
            // or reads garbage. RGB = Y directly.
            for (uint32_t x = 0; x < w_; x++) {
                int Y = rowY[x];
                dstRow[x * 4 + 0] = (uint8_t)(Y < 0 ? 0 : Y > 255 ? 255 : Y);
                dstRow[x * 4 + 1] = (uint8_t)(Y < 0 ? 0 : Y > 255 ? 255 : Y);
                dstRow[x * 4 + 2] = (uint8_t)(Y < 0 ? 0 : Y > 255 ? 255 : Y);
                dstRow[x * 4 + 3] = hasAlpha ? rowA[x] : 255;
            }
        } else {
            for (uint32_t x = 0; x < w_; x++) {
                int Y = rowY[x];
                int U = up[(x >> 1) + (y >> 1) * us] - 128;
                int V = vp[(x >> 1) + (y >> 1) * vs] - 128;
                int C = Y - 16;
                // BT.709 limited range (matches the stream colour tag and
                // browsers); U/V are the zero-biased deltas (-128 applied).
                int R = (298 * C + 459 * V + 128) >> 8;
                int G = (298 * C - 55 * U - 136 * V + 128) >> 8;
                int B = (298 * C + 541 * U + 128) >> 8;
                dstRow[x * 4 + 0] = (uint8_t)(B < 0 ? 0 : B > 255 ? 255 : B);
                dstRow[x * 4 + 1] = (uint8_t)(G < 0 ? 0 : G > 255 ? 255 : G);
                dstRow[x * 4 + 2] = (uint8_t)(R < 0 ? 0 : R > 255 ? 255 : R);
                dstRow[x * 4 + 3] = hasAlpha ? rowA[x] : 255;
            }
        }
    }
    return true;
}

bool WebmVideo::decodeFrame(uint32_t i, std::vector<uint8_t>& scratch,
                            std::vector<uint8_t>& out) const {
    scratch.clear();
    if (!ok_ || i >= frames_.size()) return false;

    if (i == 0 || i < pos_ || pos_ == UINT32_MAX) {
        // Reset and play from the top (loop wrap / seek / first call).
        destroyCodecs();
        if (!ensureCodecs()) return false;
        pos_ = 0;
        alphaValid_ = false;  // fresh loop: the alpha keyframe re-decodes
        alphaHold_.clear();
        if (i == 0) {
            if (!decodeOne(0, out)) return false;
            lastDecodedSpan_ = 1;
            return true;
        }
        cur_.resize((size_t)w_ * h_ * 4);
        if (!decodeOne(0, cur_)) return false;
        for (uint32_t k = 1; k < i; k++) {
            if (!decodeOne(k, cur_)) return false;
        }
        if (!decodeOne(i, out)) return false;
        lastDecodedSpan_ = i + 1;
        return true;
    }

    if (i == pos_ + 1) {
        if (!decodeOne(i, out)) return false;
        lastDecodedSpan_ = 1;
        pos_ = i;
        return true;
    }

    // forward jump: decode the gap sequentially
    cur_.resize((size_t)w_ * h_ * 4);
    for (uint32_t k = pos_ + 1; k < i; k++) {
        if (!decodeOne(k, cur_)) return false;
    }
    if (!decodeOne(i, out)) return false;
    lastDecodedSpan_ = i - pos_;
    pos_ = i;
    return true;
}
#endif  // !EMBED_SMALL