// main.cpp — entry point, CLI parsing, self-test mode, GUI startup.
// winsock2 必须在 windows.h（经 pet.h）之前，避免 winsock.h 与 winsock2.h 冲突。
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "pet.h"
#include "config.h"
#include "anim.h"
#include "webm.h"
#include "inflate.h"
#include "jsonc.h"
#include "resources.h"
#include "util.h"
#include "selftest_vectors.h"
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <atomic>
#include <process.h>

// ------------------------------------------------------------------ selftest

static uint64_t frameSumBytes(const uint8_t* p, size_t n) {
    uint64_t total = 0;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v = 0;
        for (int k = 7; k >= 0; k--) v = (v << 8) | p[i + k];
        total += v;
    }
    if (i < n) {
        uint64_t v = 0;
        int k = 0;
        for (; i < n; i++, k++) v |= (uint64_t)p[i] << (8 * k);
        total += v;
    }
    return total;
}

static bool g_selftestOk = true;
static FILE* g_log = nullptr;

static void logLine(const char* s) {
    fputs(s, stdout);
    fputc('\n', stdout);
    if (g_log) {
        fputs(s, g_log);
        fputc('\n', g_log);
        fflush(g_log);
    }
}

static void check(bool ok, const char* what) {
    char buf[512];
    snprintf(buf, sizeof(buf), "[%s] %s", ok ? "PASS" : "FAIL", what);
    logLine(buf);
    if (!ok) g_selftestOk = false;
}

static void runInflateVectors() {
    logLine("== inflate vectors (raw DEFLATE) ==");
    for (size_t i = 0; i < kSelftestVectorCount; i++) {
        const SelftestVector& v = kSelftestVectors[i];
        std::vector<uint8_t> out(v.rawLen);
        size_t actual = 0;
        int rc = inflateRaw(v.defl, v.deflLen, out.data(), v.rawLen, &actual);
        bool ok = (rc == 0) && (actual == v.rawLen) && (memcmp(out.data(), v.raw, v.rawLen) == 0);
        char buf[256];
        snprintf(buf, sizeof(buf), "vector '%s': rc=%d len=%zu/%zu", v.name, rc, actual, v.rawLen);
        check(ok, buf);
    }
}

// Right-click/left-click hit-box semantics: whole-window HTCLIENT means the mouse
// never pierces the window any more; the HIT_BOX (200,50)-(440,335) on the 640x360
// canvas now only gates the LEFT-click reaction and drag/click reporting. These
// vectors pin the shared mapping (main.cpp selftest + pet.cpp runtime use the same
// dshHitBox function). Expected values hand-computed from 350x197:
//   x0=(int)(350*200/640)=109  x1=(int)(350*440/640)=240
//   y0=(int)(197*50/360)=27    y1=(int)(197*335/360)=183
//   hit  => 109 <= x < 240  &&  27 <= y < 183
struct HitBoxVec { int cx, cy, winW, winH; bool expect; };
static void runHitBoxTest() {
    logLine("== right-click hit-box mapping (dshHitBox, 350x197 window) ==");
    static const HitBoxVec kVec[] = {
        {109, 27, 350, 197, true},    // left/top edge inclusive
        {175, 100, 350, 197, true},   // pet centre
        {239, 182, 350, 197, true},   // inside, near the right/bottom edge
        {240, 100, 350, 197, false},  // boundary: right edge exclusive (transparent)
        {175, 183, 350, 197, false},  // boundary: bottom edge exclusive
        {0, 0, 350, 197, false},      // transparent top-left corner
        {349, 196, 350, 197, false},  // transparent bottom-right corner
        {10, 10, 350, 197, false},    // transparent top margin
        {300, 190, 350, 197, false},  // transparent bottom margin
        {-1, 5, 350, 197, false},     // out of window
        {5, -1, 350, 197, false},     // out of window
        {0, 0, 0, 0, false},          // degenerate window
    };
    bool all = true;
    for (const auto& v : kVec) {
        bool got = dshHitBox(v.cx, v.cy, v.winW, v.winH);
        char buf[192];
        snprintf(buf, sizeof(buf), "hitbox(%d,%d)@%dx%d expect %d got %d",
                 v.cx, v.cy, v.winW, v.winH, v.expect ? 1 : 0, got ? 1 : 0);
        bool ok = (got == v.expect);
        if (!ok) all = false;
        check(ok, buf);
    }
    check(all, "all dshHitBox vector cases match the HIT_BOX mapping");
}

// Synthetic .pka v2 decode vectors (crop + delta, and RLE) with exact expected pixels.
static void runPka2VectorTest() {
    logLine("== .pka v2 decoder vectors (synthetic crop/delta + RLE packs) ==");
    bool all = true;
    for (size_t i = 0; i < kPka2VectorCount; i++) {
        const Pka2Vector& v = kPka2Vectors[i];
        AnimPack p;
        p.file.assign(v.pack, v.pack + v.packLen);
        bool hdr = p.parseHeader();
        bool framesOk = hdr && p.w == v.w && p.h == v.h &&
                        p.fpsX1000 == v.fpsX1000 && p.frameCount == v.frameCount;
        std::vector<uint8_t> scratch, frame;
        for (uint32_t fi = 0; fi < v.frameCount && framesOk; fi++) {
            if (!p.decodeFrame(fi, scratch, frame)) {
                framesOk = false;
                break;
            }
            if (frame.size() != v.expectLen ||
                memcmp(frame.data(), v.expect[fi], v.expectLen) != 0) {
                framesOk = false;
            }
        }
        char buf[256];
        snprintf(buf, sizeof(buf), "pka2 vector '%s' %ux%u frames=%u (header=%d) (%s)",
                 v.name, v.w, v.h, v.frameCount, hdr ? 1 : 0,
                 framesOk ? "matches" : "MISMATCH");
        check(framesOk, buf);
        if (!framesOk) all = false;
    }
    check(all, "all pka v2 vectors decode to the exact expected pixels");
}

// Embedded default config (RCDATA "CFG"): parse prize + geometry assertions.
static void runConfigTest() {
    logLine("== embedded default config (RCDATA CFG) ==");
    std::string text = loadEmbeddedConfigText();
    check(!text.empty(), "RCDATA CFG (default config JSONC) present");
    if (text.empty()) return;
    Json root = parseJsonc(text);
    bool hasPets = root.isObj() && root.get("pets") != nullptr && root.get("pets")->isArr() &&
                   !root.get("pets")->arr.empty();
    bool hasIdle = false;
    const Json* a = root.get("animations");
    if (a && a->isObj() && a->get("idle") && a->get("idle")->isArr() && !a->get("idle")->arr.empty())
        hasIdle = true;
    check(hasPets && hasIdle, "parseJsonc(embedded config) pets+idle");

    std::string err;
    AppConfig cfg;
    bool okCfg = loadConfig(L"", L"", cfg, &err);
    double sz = !cfg.pets.empty() ? cfg.pets[0].size : 0;
    int winW = (int)sz, winH = (int)(sz * 9.0 / 16.0 + 0.5);
    char buf[256];
    snprintf(buf, sizeof(buf), "default display size=%.0f -> window %dx%d", sz, winW, winH);
    logLine(buf);
    check(okCfg && sz == 350, "default pet size = 350 (was 462)");
    check(winW == 350 && winH == 197, "16:9 rounded window geometry = 350x197");
}

// Embedded animation packs (RCDATA EMB + PACK000..): decode every frame.
// In v7 full-quality mode every anim is an original upstream WebM (VP9-alpha);
// the same loop verifies 640x360@24fps, alpha transparency at the corners, and
// measures the aggregate all-91 decode throughput. In EMBED_SMALL mode the
// embedded set is .pka v2 and the v6 assertions below still apply.
static void runEmbeddedAnimsTest() {
    logLine("== embedded animations (RCDATA, no external assets) ==");
    std::vector<LoadedAnim> anims = loadEmbeddedAnims();
    char hdr[256];
    snprintf(hdr, sizeof(hdr), "embedded manifest + %d anims loadable", (int)anims.size());
    check(!anims.empty(), hdr);
    bool allOk = !anims.empty();
    bool anyWebm = false, allWebm = true;
    uint64_t totalDecoded = 0;
    uint32_t webmCount = 0;
    double t0 = nowSec();
    for (const auto& la : anims) {
        const AnimUnit& p = la.pack;
        bool isWebm = p.webm && p.webm->ok();
        anyWebm = anyWebm || isWebm;
        allWebm = allWebm && isWebm;
        if (isWebm) webmCount++;
        std::vector<uint8_t> scratch, frame;
        bool ok = p.ok;
        uint64_t sum = 0;
        bool cornersOk = true;
        bool firstFramesNonZero = true;
        uint32_t zeroSumFrames = 0;
        // alpha-liveness probe: sample frames; the composed alpha plane must
        // CHANGE across the clip (fresh per-frame). A constant plane means the
        // old hold-the-first-silhouette bug (the "fixed dark shadow").
        uint32_t alphaProbe[8] = {0};
        uint32_t alphaProbeN = 0;
        uint32_t step = p.frameCount > 6 ? p.frameCount / 6 : 1;
        for (uint32_t i = 0; i < p.frameCount && ok; i++) {
            if (!p.decodeFrame(i, scratch, frame)) {
                ok = false;
                break;
            }
            totalDecoded++;
            uint64_t fs = frameSumBytes(frame.data(), frame.size());
            sum += fs;
            if (i < 3 && fs == 0) firstFramesNonZero = false;
            if (isWebm && (i == 0 || i + 1 == p.frameCount ||
                           i == p.frameCount / 2)) {
                // corners must be fully transparent (rounded pet on transparent
                // canvas); upstream thumbs have a 1-LSB AA fringe, so a<=1.
                const uint32_t w = p.w, h = p.h;
                const uint32_t corners[4][2] = {
                    {0, 0}, {w - 1, 0}, {0, h - 1}, {w - 1, h - 1}};
                for (auto& c : corners)
                    if (frame[((size_t)c[1] * w + c[0]) * 4 + 3] > 1) cornersOk = false;
            }
            if (isWebm && fs == 0) zeroSumFrames++;
            if (isWebm && (i % step == 0 || i + 1 == p.frameCount) &&
                alphaProbeN < 8) {
                uint32_t op = 0;
                const uint32_t w = p.w, h = p.h;
                const uint8_t* fp = frame.data();
                for (uint32_t k = 0; k < w * h; k++)
                    if (fp[k * 4 + 3] > 128) op++;
                alphaProbe[alphaProbeN++] = op;
            }
        }
        if (!ok) allOk = false;
        if (isWebm) {
            const WebmVideo* v = p.webm.get();
            // frame count must agree with the container duration @ fps
            uint32_t expect = (uint32_t)(v->durationMs() / 1000.0 * v->fps() + 0.5);
            bool countOk = p.frameCount == v->frames().size() &&
                           (p.frameCount == expect || p.frameCount == expect + 1);
            if (!countOk) allOk = false;
            // dual-stream alpha liveness: >=2 sampled frames with different
            // opaque counts prove the alpha plane is fresh every frame
            bool alphaAlive = false;
            for (uint32_t k = 1; k < alphaProbeN; k++)
                if (alphaProbe[k] != alphaProbe[0]) alphaAlive = true;
            char buf[600];
            snprintf(buf, sizeof(buf),
                     "webm '%s' %ux%u fps=%.2f frames=%u(sum=%u) alpha_corners=%d "
                     "first3_nonzero=%d zero_frames=%d count_vs_container=%s "
                     "alpha_alive=%d probes=[%u,%u,%u,%u,%u,%u,%u,%u] (%s)",
                     la.name.c_str(), p.w, p.h, v->fps(), p.frameCount,
                     (unsigned)(sum ? 1 : 0), cornersOk ? 1 : 0,
                     firstFramesNonZero ? 1 : 0, zeroSumFrames,
                     countOk ? "ok" : "MISMATCH", alphaAlive ? 1 : 0,
                     alphaProbe[0], alphaProbe[1], alphaProbe[2], alphaProbe[3],
                     alphaProbe[4], alphaProbe[5], alphaProbe[6], alphaProbe[7],
                     ok ? "decodes" : "DECODE FAIL");
            check(ok && cornersOk && firstFramesNonZero && countOk && alphaAlive, buf);
        } else {
            char buf[512];
            snprintf(buf, sizeof(buf), "pack '%s' %ux%u mode=%u fps=%d frames=%u sum=%016llx (%s)",
                     la.name.c_str(), p.w, p.h, p.mode, (int)(p.fps() + 0.5), p.frameCount,
                     (unsigned long long)sum, ok ? "decodes" : "DECODE FAIL");
            check(ok, buf);
        }
    }
    double sec = nowSec() - t0;
    char perf[256];
    if (sec > 0 && totalDecoded > 0)
        snprintf(perf, sizeof(perf),
                 "embedded decode perf: %llu frames in %.3f s -> %.1f fps aggregate "
                 "(91 anims -> %.2f s per full pass)",
                 (unsigned long long)totalDecoded, sec, totalDecoded / sec,
                 sec / (webmCount ? webmCount : 1));
    else
        snprintf(perf, sizeof(perf), "embedded decode perf: n/a");
    logLine(perf);
    check(allOk, "embedded anims: every frame of every anim decodes");
    // v8 dual-stream mode: all embedded anims must be webm-backed
    if (anyWebm) {
        char m[160];
        snprintf(m, sizeof(m), "dual-stream mode: %d/%d embedded anims",
                 webmCount, (int)anims.size());
        check(allWebm && webmCount == (int)anims.size(), m);
        check(!anims.empty(), "startup popup suppressed (embedded anims available)");
    }
}

static LRESULT CALLBACK trayTestWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

// Shell_NotifyIcon API check: NIM_ADD + NIM_SETVERSION(NOTIFYICON_VERSION_4) on a
// hidden window, exactly like addTray() does, then NIM_DELETE. Prints raw returns.
static void runTrayApiTest() {
    logLine("== tray API check (Shell_NotifyIcon NIM_ADD/SETVERSION/DELETE) ==");
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &trayTestWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"dsh-pet-tray-selftest";
    if (!RegisterClassExW(&wc)) {
        check(false, "register tray test window class");
        return;
    }
    HWND hw = CreateWindowExW(0, L"dsh-pet-tray-selftest", L"", WS_OVERLAPPED, 0, 0, 0, 0,
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (!hw) {
        check(false, "create tray test window");
        return;
    }
    NOTIFYICONDATAW nid{};
#ifdef NOTIFYICONDATAW_V2_SIZE
    nid.cbSize = NOTIFYICONDATAW_V2_SIZE;
#else
    nid.cbSize = sizeof(NOTIFYICONDATAW);
#endif
    nid.hWnd = hw;
    nid.uID = 42;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP + 2;
    nid.hIcon = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    wcscpy(nid.szTip, L"dsh-pet-selftest");
    BOOL okAdd = Shell_NotifyIconW(NIM_ADD, &nid);
    char buf[256];
    snprintf(buf, sizeof(buf), "  NIM_ADD -> %d (GLE=%lu)", okAdd ? 1 : 0, (unsigned long)GetLastError());
    logLine(buf);
    BOOL okVer = FALSE;
    if (okAdd) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        okVer = Shell_NotifyIconW(NIM_SETVERSION, &nid);
        snprintf(buf, sizeof(buf), "  NIM_SETVERSION(4) -> %d (GLE=%lu)", okVer ? 1 : 0, (unsigned long)GetLastError());
        logLine(buf);
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
    DestroyWindow(hw);
    check(okAdd && okVer, "Shell_NotifyIcon NIM_ADD + NIM_SETVERSION(4) succeed (tray API path)");
}

static void runPkaDirTest(const std::wstring& dir) {
    logLine("== pka decode roundtrip (external dir) ==");
    std::vector<LoadedAnim> anims = loadAnimDir(dir);
    check(!anims.empty(), "loadAnimDir found *.pka files");
    // sidecar checksums: <dir>\..\checksums.txt   (name hex + space + utf8 filename.pka)
    // Optional: absent sidecar (e.g. CI full-assets zip) degrades to decode-only.
    std::wstring sidecar = dir.substr(0, dir.find_last_of(L'\\')) + L"\\checksums.txt";
    std::ifstream f(sidecar.c_str());
    bool haveSidecar = (bool)f;
    if (haveSidecar)
        check(true, "checksums.txt sidecar readable");
    else
        logLine("  (no checksums.txt sidecar — decode-only verification)");
    for (const auto& la : anims) {
        const AnimUnit& p = la.pack;
        std::vector<uint8_t> scratch, frame;
        uint64_t sum = 0;
        bool allOk = true;
        for (uint32_t i = 0; i < p.frameCount && allOk; i++) {
            if (!p.decodeFrame(i, scratch, frame)) allOk = false;
            sum += frameSumBytes(frame.data(), frame.size());
        }
        char buf[512];
        std::string line = la.name + " (" + std::to_string(p.w) + "x" + std::to_string(p.h) +
                           ", mode=" + std::to_string(p.mode) + ", fps=" + std::to_string((int)(p.fps() + 0.5)) +
                           ", frames=" + std::to_string(p.frameCount) + ")";
        bool ok = allOk;
        std::string expected;
        if (haveSidecar) {
            std::string want;
            // find line whose suffix is "<name>.pka"
            std::string needle = la.name + ".pka";
            std::string lineTxt;
            std::ifstream f2(sidecar.c_str());
            while (std::getline(f2, lineTxt)) {
                if (lineTxt.size() >= needle.size() + 17 &&
                    lineTxt.compare(lineTxt.size() - needle.size(), needle.size(), needle) == 0) {
                    want = lineTxt.substr(0, 16);
                    break;
                }
            }
            if (!want.empty()) {
                uint64_t wantSum = strtoull(want.c_str(), nullptr, 16);
                ok = ok && (sum == wantSum);
                expected = " sum=" + want;
            }
        }
        char buf2[600];
        snprintf(buf2, sizeof(buf2), "%s%s (local sum=%016llx)", line.c_str(), expected.c_str(), (unsigned long long)sum);
        check(ok, buf2);
    }
}

// v10: 气泡文本省略（纯函数，注入假宽度测量器）。
static void runBubbleTextTest() {
    logLine("== bubble text ellipsis (ellipsizeForWidth) ==");
    // 假测量器：每 wchar 10px。
    auto widthOf = [](const std::wstring& s) { return (int)(s.size() * 10); };
    // 短文本：不截断
    check(ellipsizeForWidth(L"你好", 200, widthOf) == L"你好", "short text not truncated");
    // 超长：截断到 <= maxW，尾部追加 "..."
    std::wstring longText(30, L'A');  // 300px
    std::wstring r = ellipsizeForWidth(longText, 100, widthOf);
    check(r.size() == (size_t)10 && r.substr(7) == L"...", "long text ellipsized (7 chars + ...)");
    // 前缀宽度 <= 100 - ellipsis(30) = 70 → 7 字符
    check(widthOf(r) <= 100, "ellipsized width <= maxW");
    // 无空格空间：只剩省略号
    check(ellipsizeForWidth(longText, 20, widthOf) == L"...", "tiny budget -> bare ellipsis");
    // 空串
    check(ellipsizeForWidth(L"", 100, widthOf) == L"", "empty text passthrough");
}

// v10: extractHttpText 解析（纯函数，不联网）。
static void runHttpExtractTest() {
    logLine("== http text extraction (extractHttpText) ==");
    // JSON body {"msg":"..."}
    check(extractHttpText("POST", "/", "{\"msg\":\"你好\"}") == "你好", "json body msg field");
    // JSON 对象无 msg 字段 → 空（不显示原始 JSON）
    check(extractHttpText("POST", "/", "{\"foo\":1}").empty(), "json body without msg -> empty");
    // 原始 body 文本（trimmed）
    check(extractHttpText("POST", "/", "直接文本") == "直接文本", "raw body as text");
    // query param ?msg=
    check(extractHttpText("GET", "/?msg=hello%20world", "") == "hello world", "query param url-decoded");
    // 空 body、无 query → 空
    check(extractHttpText("GET", "/", "").empty(), "nothing -> empty");
}

// v10: HTTP 监听器回环（真实启动/连接/回调/停止）。
static void runHttpLoopbackTest() {
    logLine("== http listener loopback (127.0.0.1:<test port>) ==");
    const int kTestPort = 53099;  // 避开产品端口 53021，防止与运行中的实例冲突
    static std::atomic<bool> got{false};
    static std::string gotText;
    HttpServer srv;
    auto onText = [](const std::string& t) { gotText = t; got.store(true); };
    bool startOk = srv.start(kTestPort, onText);
    check(startOk, "HttpServer::start(testPort)");
    if (!startOk) return;

    WSADATA wsa;
    bool clientOk = false;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
        SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (c != INVALID_SOCKET) {
            SOCKADDR_IN a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = htons((u_short)kTestPort);
            if (connect(c, (sockaddr*)&a, sizeof(a)) == 0) {
                const char* body = "{\"msg\":\"hello-from-test\"}";
                char req[256];
                snprintf(req, sizeof(req),
                         "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                         "Content-Type: application/json\r\nContent-Length: %zu\r\n"
                         "Connection: close\r\n\r\n%s",
                         strlen(body), body);
                send(c, req, (int)strlen(req), 0);
                char rbuf[512];
                int rn = recv(c, rbuf, sizeof(rbuf), 0);
                clientOk = rn > 0 && strstr(rbuf, "200") != nullptr;
            }
            closesocket(c);
        }
        WSACleanup();
    }
    check(clientOk, "POST received HTTP 200");
    // 等回调异步触发
    for (int i = 0; i < 100 && !got.load(); i++) Sleep(10);
    check(got.load() && gotText == "hello-from-test", "onText callback fired with text");
    srv.stop();
    check(!srv.running(), "HttpServer stopped cleanly");
}

static int runSelfTest(const std::wstring& dir) {
    AllocConsole();
    FILE* con = nullptr;
    freopen_s(&con, "CONOUT$", "w", stdout);
    freopen_s(&con, "CONOUT$", "w", stderr);
    // GUI-subsystem processes cannot be captured through the parent's stdout pipe,
    // so also mirror all output to ./selftest-result.txt (UTF-8).
    FILE* fileLog = nullptr;
    fopen_s(&fileLog, "selftest-result.txt", "w");
    g_log = fileLog ? fileLog : stdout;
    logLine(kAppVersionDesc);
    logLine(("selftest dir arg: " + wideToUtf8(dir)).c_str());

    runInflateVectors();
    runHitBoxTest();        // right-click/left-click hit-box mapping (HTCLIENT whole window)
    runPka2VectorTest();    // synthetic .pka v2 packs (crop/delta/RLE) exact pixel decode
    runConfigTest();        // embedded default config + 350x197 geometry
    runEmbeddedAnimsTest(); // embedded anims decode without any external assets
    runTrayApiTest();       // Shell_NotifyIcon NIM_ADD/SETVERSION(4) returns
    runBubbleTextTest();    // v10: bubble text ellipsis
    runHttpExtractTest();   // v10: http text extraction (pure)
    runHttpLoopbackTest();  // v10: http listener round-trip (real socket)
    if (!dir.empty()) {
        runPkaDirTest(dir);
    } else {
        logLine("(no external asset dir given; embedded-only path verified above)");
    }
    logLine(g_selftestOk ? "== ALL SELFTESTS PASSED ==" : "== SELFTESTS FAILED ==");
    logLine("结果已写入 ./selftest-result.txt");
    if (g_log && g_log != stdout) fclose(g_log);
    return g_selftestOk ? 0 : 1;
}

// ------------------------------------------------------------------ CLI

static std::vector<std::wstring> splitArgs(const std::wstring& cmd) {
    std::vector<std::wstring> out;
    std::wstring cur;
    bool inQ = false;
    for (wchar_t c : cmd) {
        if (c == L'"') {
            inQ = !inQ;
        } else if (c == L' ' && !inQ) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static void enableDpiAwareness() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        auto fn = (BOOL(WINAPI*)(void*))GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (fn) {
            fn((void*)-4);  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            return;
        }
    }
    SetProcessDPIAware();
}

// ------------------------------------------------------------------ render dump

// Render one animation frame through the same scale pipeline as paint()
// (straight alpha, optional mirror) and write a top-down 32bpp BMP.
// Exists for headless verification of the render path.
static int renderFrameToBmp(const std::wstring& assetsDir, const std::string& animName,
                            uint32_t frameIdx, const std::wstring& outBmp, int winW, int winH, bool mirror) {
    std::vector<LoadedAnim> anims = assetsDir.empty() ? loadEmbeddedAnims() : loadAnimDir(assetsDir);
    int idx = -1;
    for (size_t i = 0; i < anims.size(); i++) {
        if (anims[i].name == animName) { idx = (int)i; break; }
    }
    if (idx < 0) return 2;
    const AnimUnit& p = anims[idx].pack;
    if (!p.ok || frameIdx >= p.frameCount) return 3;
    std::vector<uint8_t> scratch, frame;
    if (!p.decodeFrame(frameIdx, scratch, frame)) return 4;
    const uint8_t* src = frame.data();
    uint32_t pw = p.w, ph = p.h;
    // 32bpp top-down BGRA BMP
    uint32_t rowBytes = winW * 4;
    std::vector<uint8_t> data((size_t)winH * rowBytes);
    for (int dy = 0; dy < winH; dy++) {
        double sy = ((double)dy + 0.5) * (double)ph / winH - 0.5;
        if (sy < 0) sy = 0;
        if (sy > ph - 1) sy = ph - 1;
        int y0 = (int)sy;
        double fy = sy - y0;
        int y1 = y0 + 1 < (int)ph ? y0 + 1 : y0;
        uint8_t* dstRow = data.data() + (size_t)dy * rowBytes;
        for (int dx = 0; dx < winW; dx++) {
            double sx = ((double)dx + 0.5) * (double)pw / winW - 0.5;
            if (mirror) sx = (double)(pw - 1) - sx;
            if (sx < 0) sx = 0;
            if (sx > pw - 1) sx = pw - 1;
            int x0 = (int)sx;
            double fx = sx - x0;
            int x1 = x0 + 1 < (int)pw ? x0 + 1 : x0;
            size_t i00 = ((size_t)y0 * pw + x0) * 4;
            size_t i01 = ((size_t)y0 * pw + x1) * 4;
            size_t i10 = ((size_t)y1 * pw + x0) * 4;
            size_t i11 = ((size_t)y1 * pw + x1) * 4;
            for (int c = 0; c < 4; c++) {
                double v = (1 - fx) * ((1 - fy) * src[i00 + c] + fy * src[i10 + c]) +
                           fx * ((1 - fy) * src[i01 + c] + fy * src[i11 + c]);
                if (v > 255) v = 255;
                if (v < 0) v = 0;
                dstRow[dx * 4 + c] = (uint8_t)(v + 0.5);
            }
        }
    }
    // BMP header (BITMAPINFOHEADER, top-down)
    std::vector<uint8_t> bmp(54 + data.size());
    uint32_t fsz = (uint32_t)bmp.size();
    bmp[0] = 'B'; bmp[1] = 'M';
    bmp[2] = (uint8_t)fsz; bmp[3] = (uint8_t)(fsz >> 8); bmp[4] = (uint8_t)(fsz >> 16); bmp[5] = (uint8_t)(fsz >> 24);
    bmp[10] = 54;
    auto put32 = [&](size_t o, uint32_t v) {
        bmp[o] = (uint8_t)v; bmp[o + 1] = (uint8_t)(v >> 8); bmp[o + 2] = (uint8_t)(v >> 16); bmp[o + 3] = (uint8_t)(v >> 24);
    };
    put32(14, 40);
    put32(18, (uint32_t)winW);
    put32(22, ~(uint32_t)winH + 1);  // negative -> top-down
    bmp[26] = 1; bmp[27] = 0;
    bmp[28] = 32; bmp[29] = 0;
    put32(30, 0);
    put32(34, (uint32_t)data.size());
    memcpy(bmp.data() + 54, data.data(), data.size());
    FILE* f = nullptr;
    _wfopen_s(&f, outBmp.c_str(), L"wb");
    if (!f) return 5;
    fwrite(bmp.data(), 1, bmp.size(), f);
    fclose(f);
    return 0;
}

// Replace control characters (e.g. 0x09 from path mix-ups) so the popup text can
// never render a mangled path.
static std::wstring sanitizePathForDisplay(const std::wstring& in) {
    std::wstring out = in;
    for (auto& c : out)
        if (c >= 0 && c < 0x20) c = L'?';
    return out;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    enableDpiAwareness();
    srand((unsigned)(nowSec() * 1000.0) ^ (unsigned)GetCurrentProcessId());

    std::vector<std::wstring> args = splitArgs(GetCommandLineW());
    std::wstring selftestDir, assetsDir, renderOut;
    std::string renderAnim;
    double sizeOverride = 0;
    uint32_t renderFrame = 0;
    int renderW = 0, renderH = 0;
    bool wantHelp = false;
    bool wantVersion = false;
    for (size_t i = 1; i < args.size(); i++) {
        const std::wstring& a = args[i];
        if (a == L"--help" || a == L"-h") wantHelp = true;
        else if (a == L"--version" || a == L"-v") wantVersion = true;
        else if (a == L"--selftest") {
            if (i + 1 < args.size() && args[i + 1][0] != L'-') selftestDir = args[++i];
        } else if (a == L"--assets") {
            if (i + 1 < args.size()) assetsDir = args[++i];
        } else if (a == L"--size") {
            if (i + 1 < args.size()) sizeOverride = _wtof(args[++i].c_str());
        } else if (a == L"--render-frame") {
            // --render-frame <animName> <frameIdx> <out.bmp> [--render-size WxH] [--mirror]
            if (i + 3 < args.size()) {
                renderAnim = wideToUtf8(args[++i]);
                renderFrame = (uint32_t)_wtoi(args[++i].c_str());
                renderOut = args[++i];
            }
        } else if (a == L"--render-size") {
            if (i + 1 < args.size()) {
                std::wstring sz = args[++i];
                int x = _wtoi(sz.c_str());
                size_t sep = sz.find(L'x');
                renderW = x;
                renderH = sep != std::wstring::npos ? _wtoi(sz.substr(sep + 1).c_str()) : 0;
            }
        } else if (a == L"--mirror") {
            // handled implicitly below via --render-frame set
        }
    }

    if (!renderAnim.empty()) {
        int w = renderW > 0 ? renderW : 350;
        int h = renderH > 0 ? renderH : (int)(w * 9 / 16 + 0.5);
        bool mirror = std::find(args.begin() + 1, args.end(), L"--mirror") != args.end();
        int rc = renderFrameToBmp(assetsDir, renderAnim, renderFrame, renderOut, w, h, mirror);
        return rc;
    }

    if (wantVersion) {
        std::wstring v = L"dsh-pet-standalone v" + std::wstring(kAppVersion);
        v += L"\n" + utf8ToWide(kAppVersionDesc);
        v += L"\n默认尺寸 350×197；内嵌 91 个上游原始动画（VP9-alpha WebM，640×360@24fps，运行时内存解码，零落盘）；\n右键任意位置弹菜单（含透明区）；托盘 v4（WM_CONTEXTMENU）。\nv10 新增：HTTP 通知气泡（监听 127.0.0.1:53021，推送 {\\\"msg\\\":\\\"...\\\"} 在头顶显示 6s，右键可开关）。\n旧版 1.x/5.x/6.x exe（素材少、画质糊或右键命中受限）——请用本版本覆盖。";
        MessageBoxW(nullptr, v.c_str(), L"dsh-pet-standalone --version", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    if (wantHelp) {
        std::wstring helpText =
            std::wstring(L"dsh-pet-standalone v") + kAppVersion +
            L" — 独立桌面宠物（单 exe，零外部文件，全画质）\n\n"
            L"用法:\n"
            L"  dsh-pet-standalone.exe                      启动宠物（动画/配置已内嵌）\n"
            L"  dsh-pet-standalone.exe --size 320          指定宽度(高按16:9)\n"
            L"  dsh-pet-standalone.exe --assets <dir>      外置 .pka 动画目录(可选覆盖)\n"
            L"  dsh-pet-standalone.exe --selftest [目录]   自检：内嵌资源解码/几何/托盘API\n"
            L"  dsh-pet-standalone.exe --version           版本信息\n"
            L"  dsh-pet-standalone.exe --help              本帮助\n\n"
            L"默认配置与动画集以内嵌资源(RCDATA)打包进 exe，双击即可播放；\n"
            L"exe 同目录的 dsh-pet-standalone.jsonc / --assets 仅作为可选覆盖。\n\n"
            L"通知气泡：程序监听 127.0.0.1:53021。向它推送文本即可在宠物头顶显示气泡。\n"
            L"  例: curl -X POST http://127.0.0.1:53021/ -d \"{\\\"msg\\\":\\\"你好\\\"}\"\n"
            L"  或: curl \"http://127.0.0.1:53021/?msg=你好\"\n"
            L"  气泡单行超长自动截断为 ...，显示约 6 秒消失；新消息覆盖旧消息；\n"
            L"  右键菜单「通知气泡」可开关。\n";
        MessageBoxW(nullptr, helpText.c_str(), L"dsh-pet-standalone", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    if (std::find(args.begin() + 1, args.end(), L"--selftest") != args.end()) {
        return runSelfTest(selftestDir);
    }

    wchar_t exeBuf[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    std::wstring exePath = exeBuf;
    size_t slash = exePath.find_last_of(L'\\');
    std::wstring exeDir = slash == std::wstring::npos ? L"." : exePath.substr(0, slash);

    std::string cfgErr;
    AppConfig cfg;
    std::wstring cfgPath = exeDir + L"\\dsh-pet-standalone.jsonc";
    if (!loadConfig(exeDir, cfgPath, cfg, &cfgErr)) {
        std::wstring msg = utf8ToWide("配置加载失败: " + cfgErr);
        MessageBoxW(nullptr, msg.c_str(), L"dsh-pet-standalone", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (sizeOverride > 0 && !cfg.pets.empty()) cfg.pets[0].size = sizeOverride;

    // resolve external animation dir (optional override)
    std::wstring animDir;
    if (!assetsDir.empty()) {
        animDir = assetsDir;
    } else {
        std::vector<std::wstring> candidates = {
            exeDir + L"\\assets\\packed",
            exeDir + L"\\packed",
        };
        wchar_t appData[MAX_PATH] = L"";
        if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH) > 0) {
            candidates.push_back(std::wstring(appData) + L"\\dsh-pet-standalone\\packed");
        }
        for (const auto& c : candidates) {
            if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) {
                animDir = c;
                break;
            }
        }
    }

    // Load order: external .pka (optional override) -> embedded RCDATA packs.
    std::vector<LoadedAnim> anims = animDir.empty() ? std::vector<LoadedAnim>() : loadAnimDir(animDir);
    if (anims.empty()) {
        anims = loadEmbeddedAnims();
    }
    // The popup appears ONLY when BOTH embedded and external animations are gone.
    if (anims.empty()) {
        std::wstring suggest = animDir.empty() ? (exeDir + L"\\assets\\packed") : animDir;
        std::wstring msg = L"未找到动画文件。\n\n内嵌动画与外部素材均不可用。\n请在以下位置放置动画包：\n" +
                           sanitizePathForDisplay(suggest) +
                           L"\n\n正常情况下内嵌动画总是可用；请检查打包是否完整。\n程序仍会启动（托盘可退出）。";
        MessageBoxW(nullptr, msg.c_str(), L"dsh-pet-standalone", MB_OK | MB_ICONWARNING);
    }

    PetWidget pet(hInst, std::move(cfg), std::move(anims), exePath, animDir);
    if (!pet.create()) {
        MessageBoxW(nullptr, L"窗口创建失败", L"dsh-pet-standalone", MB_OK | MB_ICONERROR);
        return 1;
    }
    return pet.run();
}