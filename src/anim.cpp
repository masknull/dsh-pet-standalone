// anim.cpp — .pka loader/decoder (v1 + v2) + unified AnimUnit backend glue
// (webm dispatch lives here so anim.h stays free of libvpx headers).
#include "anim.h"
#include "webm.h"
#include "inflate.h"
#include "jsonc.h"
#include "util.h"
#include <cstring>
#include <algorithm>

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

// ---------------------------------------------------------------- AnimUnit

AnimUnit::~AnimUnit() = default;
AnimUnit::AnimUnit(AnimUnit&&) noexcept = default;
AnimUnit& AnimUnit::operator=(AnimUnit&&) noexcept = default;

AnimUnit AnimUnit::fromPka(std::string n, AnimPack p) {
    AnimUnit u;
    u.name = std::move(n);
    u.ok = p.ok;
    u.version = p.version;
    u.mode = p.mode;
    u.flags = p.flags;
    u.colors = p.colors;
    u.w = p.w;
    u.h = p.h;
    u.fpsX1000 = p.fpsX1000;
    u.frameCount = p.frameCount;
    u.pka = std::move(p);
    return u;
}

AnimUnit AnimUnit::fromWebm(std::string n, const uint8_t* data, size_t len) {
    return fromWebmPair(std::move(n), data, len, nullptr, 0);
}

AnimUnit AnimUnit::fromWebmPair(std::string n, const uint8_t* main, size_t mainLen,
                                const uint8_t* alpha, size_t alphaLen) {
    AnimUnit u;
    u.name = std::move(n);
#ifdef EMBED_SMALL
    (void)main;
    (void)mainLen;
    (void)alpha;
    (void)alphaLen;
    u.ok = false;  // webm backend compiled out of EMBED_SMALL builds
    return u;
#else
    auto v = std::make_unique<WebmVideo>();
    if (!v->open(main, mainLen)) {
        u.ok = false;
        return u;
    }
    u.ok = true;
    u.version = 8;  // distinct backend marker for diagnostics
    u.w = v->width();
    u.h = v->height();
    u.frameCount = v->frameCount();
    u.fpsX1000 = (uint32_t)(v->fps() * 1000.0 + 0.5);
    u.webm = std::move(v);
    if (alpha && alphaLen) {
        // partner alpha stream: separate ordinary VP9 stream (monochrome gray =
        // the alpha plane). Fully decodable per frame — no "hold" fallback.
        // Dims may differ (half-res mask is bilinear-upscaled in decodeFrame);
        // only the frame count must line up with the main timeline.
        auto va = std::make_unique<WebmVideo>();
        if (va->open(alpha, alphaLen)) {
            va->setGrayOnly(true);  // alpha mask: undefined chroma in libvpx
            if (va->frameCount() == u.frameCount) {
                u.webmAlpha = std::move(va);
            }
        }
    }
    return u;
#endif
}

bool AnimUnit::decodeFrame(uint32_t i, std::vector<uint8_t>& scratch,
                           std::vector<uint8_t>& out) const {
    if (pka.ok) return pka.decodeFrame(i, scratch, out);
    if (webm && webm->ok()) {
        if (!webm->decodeFrame(i, scratch, out)) return false;
        if (webmAlpha && webmAlpha->ok()) {
            // alpha stream is monochrome gray: R==G==B==alpha. Compose into
            // byte 3 of the freshly decoded main frame (BGRA), so every frame
            // carries its own alpha — this is what kills the v7 dark shadow.
            if (!webmAlpha->decodeFrame(i, aScratch_, aFrame_)) return false;
            const uint32_t aw = webmAlpha->width(), ah = webmAlpha->height();
            if (aw == w && ah == h) {
                const size_t n = (size_t)w * h;
                for (size_t k = 0; k < n; k++) out[k * 4 + 3] = aFrame_[k * 4 + 0];
            } else {
                // half-res (or otherwise scaled) mask: bilinear upscale
                const float sx = (float)aw / (float)w, sy = (float)ah / (float)h;
                for (uint32_t y = 0; y < h; y++) {
                    const float fy = ((float)y + 0.5f) * sy - 0.5f;
                    int y0 = (int)fy;
                    if (y0 < 0) y0 = 0; else if (y0 > (int)ah - 1) y0 = ah - 1;
                    const int y1 = y0 + 1 < (int)ah ? y0 + 1 : y0;
                    const float ty = fy - (float)y0;
                    for (uint32_t x = 0; x < w; x++) {
                        const float fx = ((float)x + 0.5f) * sx - 0.5f;
                        int x0 = (int)fx;
                        if (x0 < 0) x0 = 0; else if (x0 > (int)aw - 1) x0 = aw - 1;
                        const int x1 = x0 + 1 < (int)aw ? x0 + 1 : x0;
                        const float tx = fx - (float)x0;
                        const size_t i00 = ((size_t)y0 * aw + x0) * 4;
                        const size_t i10 = ((size_t)y0 * aw + x1) * 4;
                        const size_t i01 = ((size_t)y1 * aw + x0) * 4;
                        const size_t i11 = ((size_t)y1 * aw + x1) * 4;
                        const float t0 = aFrame_[i00] + (aFrame_[i10] - aFrame_[i00]) * tx;
                        const float t1 = aFrame_[i01] + (aFrame_[i11] - aFrame_[i01]) * tx;
                        float v = t0 + (t1 - t0) * ty;
                        if (v < 0) v = 0; else if (v > 255) v = 255;
                        out[((size_t)y * w + x) * 4 + 3] = (uint8_t)v;
                    }
                }
            }
        }
        return true;
    }
    return false;
}

void AnimUnit::releaseDecoders() {
    if (webm) webm->releaseDecoders();
    if (webmAlpha) webmAlpha->releaseDecoders();
    // v9.1 memory fix: aScratch_/aFrame_ (each up to w*h*4) were kept at full
    // capacity for EVERY animation that ever played, so the set of played anims
    // permanently pinned 91*(0.92+0.92+alpha) MB of private memory. Released
    // buffers are rebuilt lazily by the next decodeFrame — zero cost when the
    // animation is not current.
    std::vector<uint8_t>().swap(aScratch_);
    std::vector<uint8_t>().swap(aFrame_);
}

bool AnimPack::parseHeader() {
    ok = false;
    palette.clear();
    idxPrev_.clear();
    outPrev_.clear();
    cacheFrame_ = UINT32_MAX;
    if (file.size() < 28) return false;
    if (memcmp(file.data(), "DSHPETPK", 8) != 0) return false;
    version = file[8];
    mode = file[9];
    flags = 0;
    w = rd32(file.data() + 12);
    h = rd32(file.data() + 16);
    fpsX1000 = rd32(file.data() + 20);
    frameCount = rd32(file.data() + 24);
    if (version != 1 && version != 2) return false;
    if (!w || !h || !frameCount || !fpsX1000) return false;
    if (version == 2) {
        // v2 header has one extra byte (reserved): colors@11 w@13 h@17 fps@21 count@25
        colors = rd16(file.data() + 11);
        w = rd32(file.data() + 13);
        h = rd32(file.data() + 17);
        fpsX1000 = rd32(file.data() + 21);
        frameCount = rd32(file.data() + 25);
    }

    if (version == 1) {
        colors = rd16(file.data() + 10);
        if (mode != 0 && mode != 1) return false;
        size_t pos = 28;
        frameMeta.clear();
        frameMeta.reserve(frameCount);
        for (uint32_t i = 0; i < frameCount; i++) {
            if (pos + 4 > file.size()) return false;
            uint32_t compLen = rd32(file.data() + pos);
            pos += 4;
            if (pos + compLen > file.size()) return false;
            frameMeta.emplace_back(compLen, (uint32_t)pos);
            pos += compLen;
            if (mode == 1) {
                if (pos + 2 > file.size()) return false;
                uint16_t palLen = rd16(file.data() + pos);
                pos += 2;
                if (palLen != colors * 4) return false;
                if (pos + palLen > file.size()) return false;
                pos += palLen;
            }
        }
        ok = true;
        return true;
    }

    // ---- v2 ----
    colors = rd16(file.data() + 11);  // [8]version [9]flags [10]reserved [11..12]colors
    flags = file[9];
    roiX = roiY = roiW = roiH = 0;
    size_t pos = 29;
    if (flags & 1) {
        if (pos + 8 > file.size()) return false;
        roiX = rd16(file.data() + pos);
        roiY = rd16(file.data() + pos + 2);
        roiW = rd16(file.data() + pos + 4);
        roiH = rd16(file.data() + pos + 6);
        pos += 8;
        if (!roiW || !roiH ||
            (uint32_t)roiX + roiW > w || (uint32_t)roiY + roiH > h) return false;
    }
    if (pos + 2 > file.size()) return false;
    uint16_t palLen = rd16(file.data() + pos);
    pos += 2;
    if (palLen != colors * 4) return false;
    if (pos + palLen > file.size()) return false;
    palette.assign(file.data() + pos, file.data() + pos + palLen);
    pos += palLen;

    frameMeta.clear();
    frameMeta.reserve(frameCount);
    for (uint32_t i = 0; i < frameCount; i++) {
        if (pos + 4 > file.size()) return false;
        uint32_t compLen = rd32(file.data() + pos);
        pos += 4;
        if (pos + compLen > file.size()) return false;
        frameMeta.emplace_back(compLen, (uint32_t)pos);
        pos += compLen;
    }
    ok = true;
    return true;
}

namespace {

// Map one index byte to BGRA straight-alpha pixel at out+px*4.
inline void palLookup(const std::vector<uint8_t>& pal, uint16_t colors,
                      uint8_t e, uint8_t* out) {
    if (e >= colors) {
        out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0;
    } else {
        out[0] = pal[e * 4 + 2];  // RGBA -> BGRA
        out[1] = pal[e * 4 + 1];
        out[2] = pal[e * 4 + 0];
        out[3] = pal[e * 4 + 3];
    }
}

// RLE packet stream: tag = (type<<7)|(len-1); type1 = len literal bytes follow,
// type0 = repeat: one value byte follows (run of len). Writes exactly outLen bytes.
static bool rleDecode(const uint8_t* in, size_t n, uint8_t* out, size_t outLen) {
    size_t k = 0, q = 0;
    while (q < n) {
        uint8_t tag = in[q++];
        size_t ln = (tag & 0x7F) + 1;
        if (tag & 0x80) {
            if (q >= n || k + ln > outLen) return false;
            memset(out + k, in[q++], ln);
            k += ln;
        } else {
            if (q + ln > n || k + ln > outLen) return false;
            memcpy(out + k, in + q, ln);
            q += ln;
            k += ln;
        }
    }
    return k == outLen;
}

// Per-frame transform modes (first byte of every v2 frame payload):
enum { PKA_MODE_RAW = 0, PKA_MODE_XOR = 1, PKA_MODE_RLE = 2, PKA_MODE_RLEXOR = 3 };

}  // namespace

bool AnimPack::decodeFrame(size_t i, std::vector<uint8_t>& scratch, std::vector<uint8_t>& out) const {
    if (!ok || i >= frameMeta.size()) return false;
    const size_t tex = (size_t)w * h;
    out.resize(tex * 4);

    if (version == 1) {
        size_t expected = (mode == 0) ? tex * 4 : tex;
        const auto& meta = frameMeta[i];
        const uint8_t* comp = file.data() + meta.second;
        scratch.resize(expected + 4);
        size_t actual = 0;
        int rc = inflateRaw(comp, meta.first, scratch.data(), expected, &actual);
        if (rc != 0 || actual != expected) return false;
        if (mode == 0) {
            memcpy(out.data(), scratch.data(), tex * 4);
            return true;
        }
        size_t palPos = meta.second + meta.first;
        if (palPos + 2 > file.size()) return false;
        uint16_t palLen = rd16(file.data() + palPos);
        if (palLen != colors * 4) return false;
        const uint8_t* pal = file.data() + palPos + 2;
        const uint8_t* idx = scratch.data();
        for (size_t px = 0; px < tex; px++) {
            uint8_t entry = idx[px];
            if (entry >= colors) {
                out[px * 4 + 0] = 0;
                out[px * 4 + 1] = 0;
                out[px * 4 + 2] = 0;
                out[px * 4 + 3] = 0;
            } else {
                out[px * 4 + 0] = pal[entry * 4 + 2];
                out[px * 4 + 1] = pal[entry * 4 + 1];
                out[px * 4 + 2] = pal[entry * 4 + 0];
                out[px * 4 + 3] = pal[entry * 4 + 3];
            }
        }
        return true;
    }

    // ---- v2 ----
    const size_t croW = (flags & 1) ? roiW : w;
    const size_t croH = (flags & 1) ? roiH : h;
    const size_t cro = croW * croH;
    const auto& meta = frameMeta[i];
    const uint8_t* comp = file.data() + meta.second;

    // Decompress (packet streams can be larger than cro; raw frames are cro + mode byte).
    size_t cap = std::max<size_t>(cro * 2 + 4096, 256 * 1024);
    size_t actual = 0;
    int rc = -4;
    while (rc == -4) {
        scratch.resize(cap);
        actual = 0;
        rc = inflateRaw(comp, meta.first, scratch.data(), cap, &actual);
        if (rc == -4) cap *= 2;
    }
    if (rc != 0 || actual < 1) return false;
    const uint8_t mode = scratch[0];
    const uint8_t* body = scratch.data() + 1;
    const size_t blen = actual - 1;

    auto mapRoi = [&](const uint8_t* idx) {
        // map the crop/full region; area outside ROI stays transparent (fresh buffer)
        if (flags & 1) {
            uint8_t* base = out.data();
            for (size_t ry = 0; ry < croH; ry++) {
                uint8_t* row = base + ((size_t)roiY + ry) * w * 4 + (size_t)roiX * 4;
                for (size_t rx = 0; rx < croW; rx++) {
                    palLookup(palette, colors, idx[ry * croW + rx], row + rx * 4);
                }
            }
        } else {
            for (size_t px = 0; px < cro; px++)
                palLookup(palette, colors, idx[px], out.data() + px * 4);
        }
    };

    auto decodeBody = [&](uint8_t m, const uint8_t* b, size_t bn, uint8_t* dst) -> bool {
        const uint8_t* prev = !idxPrev_.empty() ? idxPrev_.data() : nullptr;
        switch (m) {
            case PKA_MODE_RAW:
                if (bn != cro) return false;
                memcpy(dst, b, cro);
                return true;
            case PKA_MODE_XOR:
                if (bn != cro || !prev) return false;
                for (size_t k = 0; k < cro; k++) dst[k] = b[k] ^ prev[k];
                return true;
            case PKA_MODE_RLE:
                return rleDecode(b, bn, dst, cro);
            case PKA_MODE_RLEXOR: {
                if (!prev) return false;
                std::vector<uint8_t> tmp(cro);
                if (!rleDecode(b, bn, tmp.data(), cro)) return false;
                for (size_t k = 0; k < cro; k++) dst[k] = tmp[k] ^ prev[k];
                return true;
            }
            default:
                return false;
        }
    };

    if (i == 0 || !(flags & 2) || cacheFrame_ == i - 1) {
        // fast path: sequential chain (or single-frame packs)
        idxPrev_.resize(cro);
        if (!decodeBody(mode, body, blen, idxPrev_.data())) return false;
        mapRoi(idxPrev_.data());
        outPrev_.assign(out.begin(), out.end());
        cacheFrame_ = i;
        return true;
    }

    // out-of-order decode: rebuild the chain from frame 0 so xor references hold
    std::vector<uint8_t> chain(cro);
    {
        bool havePrev = false;
        for (uint32_t j = 0; j <= i; j++) {
            const auto& m = frameMeta[j];
            size_t jlen = 0;
            size_t jcap = std::max<size_t>(cro * 2 + 4096, 256 * 1024);
            int jrc = -4;
            while (jrc == -4) {
                scratch.resize(jcap);
                jlen = 0;
                jrc = inflateRaw(file.data() + m.second, m.first, scratch.data(),
                                 jcap, &jlen);
                if (jrc == -4) jcap *= 2;
            }
            if (jrc != 0 || jlen < 1) return false;
            uint8_t jm = scratch[0];
            const uint8_t* jb = scratch.data() + 1;
            size_t jbn = jlen - 1;
            std::vector<uint8_t> tmp(cro);
            uint8_t* dst = chain.data();
            if (havePrev) {
                // decode into tmp with chain acting as prev, then save
                std::vector<uint8_t> save = chain;
                const uint8_t* oldPrev = save.data();
                switch (jm) {
                    case PKA_MODE_RAW:
                        if (jbn != cro) return false;
                        memcpy(dst, jb, cro);
                        break;
                    case PKA_MODE_XOR:
                        if (jbn != cro) return false;
                        for (size_t k = 0; k < cro; k++) dst[k] = jb[k] ^ oldPrev[k];
                        break;
                    case PKA_MODE_RLE:
                        if (!rleDecode(jb, jbn, dst, cro)) return false;
                        break;
                    case PKA_MODE_RLEXOR:
                        if (!rleDecode(jb, jbn, tmp.data(), cro)) return false;
                        for (size_t k = 0; k < cro; k++) dst[k] = tmp[k] ^ oldPrev[k];
                        break;
                    default:
                        return false;
                }
            } else {
                if (jm != PKA_MODE_RAW || jbn != cro) return false;
                memcpy(dst, jb, cro);
                havePrev = (j == 0);
            }
        }
    }
    idxPrev_.swap(chain);
    mapRoi(idxPrev_.data());
    outPrev_.assign(out.begin(), out.end());
    cacheFrame_ = i;
    return true;
}

static bool readFileBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    if (sz.QuadPart <= 0 || sz.QuadPart > (1024LL * 1024 * 1024)) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD read = 0;
    BOOL ok = ReadFile(h, out.data(), (DWORD)out.size(), &read, nullptr);
    CloseHandle(h);
    return ok && read == out.size();
}

// Read an optional manifest.json ({"animations":[{"name":"..."}]}) that lists the pack
// deterministically. Falls back to directory enumeration when absent.
static std::vector<std::wstring> manifestNames(const std::wstring& dir) {
    std::vector<std::wstring> out;
    std::vector<uint8_t> bytes;
    if (!readFileBytes(dir + L"\\manifest.json", bytes)) return out;
    std::string text(bytes.begin(), bytes.end());
    Json j = parseJsonc(text);
    const Json* anims = j.get("animations");
    if (!anims || !anims->isArr()) return out;
    for (const auto& e : anims->arr) {
        const Json* name = e.get("name");
        if (name && name->type == Json::Type::Str) out.push_back(utf8ToWide(name->str));
    }
    return out;
}

std::vector<LoadedAnim> loadAnimDir(const std::wstring& dir) {
    std::vector<LoadedAnim> out;
    // 1) manifest-driven listing (deterministic, primary)
    for (const auto& nm : manifestNames(dir)) {
        LoadedAnim la;
        la.name = wideToUtf8(nm);
        AnimPack p;
        if (readFileBytes(dir + L"\\" + nm + L".pka", p.file) && p.parseHeader()) {
            la.pack = AnimUnit::fromPka(la.name, std::move(p));
            out.push_back(std::move(la));
        }
    }
    if (!out.empty()) return out;
    // 2) directory enumeration fallback
    std::wstring pattern = dir + L"\\*.pka";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring full = dir + L"\\" + fd.cFileName;
        std::wstring base = fd.cFileName;
        size_t dot = base.find_last_of(L'.');
        if (dot != std::wstring::npos) base.erase(dot);
        AnimPack p;
        LoadedAnim la;
        la.name = wideToUtf8(base);
        if (readFileBytes(full, p.file) && p.parseHeader()) {
            la.pack = AnimUnit::fromPka(la.name, std::move(p));
            out.push_back(std::move(la));
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    return out;
}