// anim.h — animation backends for dsh-pet-standalone.
//
// Two sources produce identical "AnimeUnit" surfaces for the pet renderer:
//   * AnimPack  (.pka v1/v2 lossy packs, kept as the optional EMBED_SMALL mode)
//   * WebmVideo (original upstream VP9-alpha WebM, decoded in memory at full
//                quality 640x360@24 — the v7 default)
// AnimUnit dispatches to whichever backend is loaded; pet.cpp, the tray, the
// hit-box logic and the window code never see the difference.
//
// .pka v2 container (little-endian), one file per animation:
//   u8[8]  magic "DSHPETPK"
//   u8     version (2)
//   u8     flags (bit0 ROI crop, bit1 delta payload, bit2 RLE framing)
//   u8     reserved (0)
//   u16    colors (RGBA palette entries, palette stored ONCE for the animation)
//   u32    full_width, full_height, fps_x1000, frame_count
//   [flags&1] u16 roiX, roiY, roiW, roiH     crop region inside the full canvas
//   u16    palette_len + palette bytes (RGBA; slot 0 = fully transparent)
//   per frame:
//     u32    comp_len
//     comp_len bytes raw-DEFLATE of the transformed payload:
//       delta (flags&2): frame 0 raw index bytes; later frames a packet stream:
//         u8 tag = (type<<7)|(len-1); type1 = len literal changed bytes follow,
//         type0 = skip len pixels (copy from previous frame). Max run 128.
//       rle (flags&4):   packet stream: type0 = repeat: one value byte follows
//                        (run of len), type1 = len literal bytes.
//       neither:         raw index bytes (one u8 per pixel; 0 = transparent).
//
// .pka v1 container (still supported for older external packs):
//   version=1, mode=0 (BGRA lossless) | mode=1 (palette8, palette PER FRAME).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class WebmVideo;  // defined in webm.h/webm.cpp (libvpx VP9-alpha decoder)

struct AnimPack {
    bool ok = false;
    uint8_t version = 0;
    uint8_t mode = 0;          // v1 only
    uint8_t flags = 0;         // v2 only
    uint16_t colors = 0;
    uint32_t w = 0, h = 0;     // full-canvas geometry (placement)
    uint32_t fpsX1000 = 0;
    uint32_t frameCount = 0;
    uint16_t roiX = 0, roiY = 0, roiW = 0, roiH = 0;  // v2 crop region (0 sized = none)
    std::vector<uint8_t> palette;                     // v2: shared RGBA palette
    std::vector<uint8_t> file;                        // whole file bytes
    std::vector<std::pair<uint32_t, uint32_t>> frameMeta;  // (compLen, dataOffset)

    double fps() const { return fpsX1000 ? fpsX1000 / 1000.0 : 24.0; }
    double duration() const { return fps() > 0 ? frameCount / fps() : 0.0; }

    bool parseHeader();
    // Decode frame i into out as straight-alpha BGRA (w*h*4 bytes). scratch is temp
    // storage. When the pack uses inter-frame delta, a persistent cache inside the
    // pack keeps the previous frame; sequential decoding is O(changed pixels).
    bool decodeFrame(size_t i, std::vector<uint8_t>& scratch, std::vector<uint8_t>& out) const;

private:
    // mutable decode caches for delta chains (single-threaded UI use)
    mutable std::vector<uint8_t> idxPrev_;    // previous frame's (crop) index buffer
    mutable std::vector<uint8_t> outPrev_;    // previous decoded BGRA full canvas
    mutable uint32_t cacheFrame_ = UINT32_MAX;
};

// Unified animation slot: exactly one backend is active.
struct AnimUnit {
    std::string name;          // UTF-8 identifier (identical in both modes)
    bool ok = false;
    uint8_t version = 0, mode = 0, flags = 0;   // pka passthrough (webm: zeros)
    uint16_t colors = 0;
    uint32_t w = 0, h = 0;                      // full canvas 640x360 (webm) / pka w/h
    uint32_t fpsX1000 = 0;                      // 24000 for webm thumbs
    uint32_t frameCount = 0;
    AnimPack pka;                               // backend A (ok when pka.ok)
    std::unique_ptr<WebmVideo> webm;            // backend B (ok when webm && webm->ok())
    // Dual-stream mode (v8): webm = main video (no alpha), webmAlpha = the same
    // timeline's alpha plane encoded as a monochrome VP9 stream. decodeFrame
    // composes the alpha into byte 3, so every frame gets a FRESH alpha plane
    // (the v7 upstream single-stream alpha could only be decoded on frame 0,
    // everything else had to be "held" — that produced a fixed dark silhouette).
    std::unique_ptr<WebmVideo> webmAlpha;       // backend B2 (optional partner stream)

    AnimUnit() = default;
    ~AnimUnit();                                // out-of-line (webm.h is in anim.cpp)
    AnimUnit(AnimUnit&&) noexcept;
    AnimUnit& operator=(AnimUnit&&) noexcept;
    AnimUnit(const AnimUnit&) = delete;
    AnimUnit& operator=(const AnimUnit&) = delete;

    double fps() const { return fpsX1000 ? fpsX1000 / 1000.0 : 24.0; }
    double duration() const { return fps() > 0 ? frameCount / fps() : 0.0; }
    // Mirrors AnimPack::decodeFrame (const, with mutable decode caches).
    bool decodeFrame(uint32_t i, std::vector<uint8_t>& scratch,
                     std::vector<uint8_t>& out) const;

    // Builders (fill name + one backend + the shared fields).
    static AnimUnit fromPka(std::string n, AnimPack p);
    static AnimUnit fromWebm(std::string n, const uint8_t* data, size_t len);
    // Dual-stream: main webm + partner alpha webm (alpha may be null for
    // plain single-stream webm, which keeps the v7 fallback behaviour).
    static AnimUnit fromWebmPair(std::string n, const uint8_t* main, size_t mainLen,
                                 const uint8_t* alpha, size_t alphaLen);

private:
    // shared compose scratch (mutable: decodeFrame is const) — second stream
    // decode reuses our own buffers so the caller's scratch stays untouched.
    mutable std::vector<uint8_t> aScratch_;
    mutable std::vector<uint8_t> aFrame_;
};

struct LoadedAnim {
    std::string name;  // UTF-8, file base name / manifest name
    AnimUnit pack;
};

// Scan a directory for *.pka and load each (external override path).
std::vector<LoadedAnim> loadAnimDir(const std::wstring& dir);