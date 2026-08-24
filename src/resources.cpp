// resources.cpp — Win32 RCDATA resource accessor.
//
// The strict single-exe requirement means the default config and the default
// animation set must live INSIDE the exe. Build pipeline (rcgen.py) embeds:
//   "CFG"      default config JSONC text               (RCDATA)
//   "EMB"      embedded-manifest.json (ordered names)  (RCDATA)
//   "PACK000".."PACK00N"  one asset per manifest entry  (RCDATA)
//
// Asset format depends on the build mode:
//   * default (v7 FULL QUALITY): the ORIGINAL upstream thumb WebM files
//     (VP9-alpha, 640x360@24fps) — parsed and decoded in memory, zero copy.
//   * EMBED_SMALL=1 (legacy v2): .pka lossy packs (256x144@10fps/pal96),
//     decoded by AnimPack exactly as in v6.
// Resource names are ASCII identifiers, so no numeric-id collisions with the
// application icon (IDI_APP = 1 in resource.h) are possible.
#include "resources.h"
#include "anim.h"
#include "webm.h"
#include "jsonc.h"
#include <windows.h>
#include <cstdio>

bool loadResourceBytes(const wchar_t* name, std::vector<uint8_t>& out) {
    HRSRC h = FindResourceW(nullptr, name, (LPCWSTR)RT_RCDATA);
    if (!h) return false;
    HGLOBAL g = LoadResource(nullptr, h);
    if (!g) return false;
    const uint8_t* p = (const uint8_t*)LockResource(g);
    DWORD n = SizeofResource(nullptr, h);
    if (!p || n == 0) return false;
    out.assign(p, p + n);
    return true;
}

// Zero-copy variant: the resource pointer stays valid for the whole process
// lifetime (we never FreeResource), so WebmVideo can point straight into the
// mapped PE image instead of duplicating ~46 MB of animation bytes.
bool loadResourcePtr(const wchar_t* name, const uint8_t** out, size_t* len) {
    HRSRC h = FindResourceW(nullptr, name, (LPCWSTR)RT_RCDATA);
    if (!h) return false;
    HGLOBAL g = LoadResource(nullptr, h);
    if (!g) return false;
    *out = (const uint8_t*)LockResource(g);
    *len = (size_t)SizeofResource(nullptr, h);
    return *out && *len;
}

std::string loadEmbeddedConfigText() {
    std::vector<uint8_t> b;
    if (!loadResourceBytes(L"CFG", b)) return "";
    return std::string(b.begin(), b.end());
}

std::vector<LoadedAnim> loadEmbeddedAnims() {
    std::vector<LoadedAnim> out;
    std::vector<uint8_t> mb;
    if (!loadResourceBytes(L"EMB", mb)) return out;
    Json j = parseJsonc(std::string(mb.begin(), mb.end()));
    const Json* anims = j.get("animations");
    if (!anims || !anims->isArr()) return out;
    int i = 0;
    for (const auto& e : anims->arr) {
        const Json* name = e.get("name");
        if (name && name->type == Json::Type::Str && !name->str.empty()) {
            wchar_t rname[16];
            swprintf(rname, 16, L"PACK%03d", i);
            LoadedAnim la;
            la.name = name->str;  // UTF-8, identical to external base names
#ifdef EMBED_SMALL
            std::vector<uint8_t> bytes;
            if (loadResourceBytes(rname, bytes)) {
                AnimPack p;
                p.file = std::move(bytes);
                if (p.parseHeader()) {
                    la.pack = AnimUnit::fromPka(la.name, std::move(p));
                    out.push_back(std::move(la));
                }
            }
#else
            const uint8_t* ptr = nullptr;
            size_t len = 0;
            if (loadResourcePtr(rname, &ptr, &len)) {
                // dual-stream (v8): optional partner alpha stream PACK%03dA
                wchar_t ralpha[20];
                swprintf(ralpha, 20, L"PACK%03dA", i);
                const uint8_t* ap = nullptr;
                size_t alen = 0;
                loadResourcePtr(ralpha, &ap, &alen);  // optional
                AnimUnit u = AnimUnit::fromWebmPair(la.name, ptr, len, ap, alen);
                if (u.ok) {
                    la.pack = std::move(u);
                    out.push_back(std::move(la));
                }
            }
#endif
        }
        i++;
    }
    return out;
}