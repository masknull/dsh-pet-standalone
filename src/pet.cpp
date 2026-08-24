// pet.cpp — desktop-pet layered window + behaviour engine (C++ port of dsh-pet client logic).
#include "pet.h"
#include "util.h"
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <shellapi.h>
#include <windowsx.h>

namespace {

constexpr double kCanvasW = 640, kCanvasH = 360;   // thumb canvas geometry
constexpr double kHitX0 = 200, kHitY0 = 50;        // HIT_BOX (canvas px)
constexpr double kHitX1 = 440, kHitY1 = 335;
constexpr int kDragThreshold = 5;                  // px
const wchar_t kClassName[] = L"dsh-pet-standalone";
constexpr LRESULT kMsgUnhandled = (LRESULT)0xFFFFFFFF;

// The one PetWidget instance; the WH_MOUSE_LL hook proc (module-level, no userdata)
// needs it. Set in create(), cleared in the destructor.
PetWidget* g_petInstance = nullptr;

}  // namespace

bool dshHitBox(int clientX, int clientY, int winW, int winH) {
    if (winW <= 0 || winH <= 0) return false;
    int x0 = (int)(winW * (kHitX0 / kCanvasW));
    int x1 = (int)(winW * (kHitX1 / kCanvasW));
    int y0 = (int)(winH * (kHitY0 / kCanvasH));
    int y1 = (int)(winH * (kHitY1 / kCanvasH));
    return clientX >= x0 && clientX < x1 && clientY >= y0 && clientY < y1;
}

namespace {

bool inPool(const std::vector<std::string>& pool, const std::string& name) {
    for (const auto& n : pool)
        if (n == name) return true;
    return false;
}

const wchar_t kRunValueName[] = L"dsh-pet-standalone";

// ---- diagnostics (opt-in) -------------------------------------------------
// Everything here is silent by default; set env DSH_PET_DIAG=1 to get
//   * %TEMP%\dsh-pet-diag.log        tray/notification-API return values +
//                                     callback messages + playback switches
//   * pet-play-debug.txt (in CWD)     animation switches (used by gui-test.ps1)
// Normal single-exe runs therefore never write any file.
static bool diagEnabled() {
    static int cached = -1;
    if (cached < 0) {
        wchar_t v[8] = L"";
        GetEnvironmentVariableW(L"DSH_PET_DIAG", v, 8);
        // Lenient: ANY value except empty / "0" turns diagnostics on — so
        // "=1", "='1'" (cmd quoting) and "=yes" all work.
        cached = (v[0] == L'\0' || v[0] == L'0') ? 0 : 1;
    }
    return cached == 1;
}

static void diagLog(const char* fmt, ...) {
    if (!diagEnabled()) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
    wchar_t tmp[MAX_PATH] = L"";
    if (GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH) > 0) {
        std::wstring path = std::wstring(tmp) + L"\\dsh-pet-diag.log";
        FILE* f = nullptr;
        _wfopen_s(&f, path.c_str(), L"a");
        if (f) {
            fprintf(f, "[%.1f] %s\n", nowSec(), buf);
            fclose(f);
        }
    }
}

bool isAutoStartImpl(const std::wstring& exePath) {
    wchar_t buf[1024] = L"";
    DWORD sz = sizeof(buf);
    LONG rc = RegGetValueW(HKEY_CURRENT_USER,
                           L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                           kRunValueName, RRF_RT_REG_SZ, nullptr, buf, &sz);
    if (rc != ERROR_SUCCESS) return false;
    return std::wstring(buf) == L"\"" + exePath + L"\"";
}

void setAutoStartImpl(const std::wstring& exePath, bool on) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        if (on) {
            std::wstring val = L"\"" + exePath + L"\"";
            RegSetValueExW(key, kRunValueName, 0, REG_SZ, (const BYTE*)val.c_str(),
                           (DWORD)((val.size() + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(key, kRunValueName);
        }
        RegCloseKey(key);
    }
}

}  // namespace

PetWidget::PetWidget(HINSTANCE hInst, AppConfig cfg, std::vector<LoadedAnim> anims,
                     const std::wstring& exePath, const std::wstring& assetsDir)
    : hInst_(hInst), cfg_(std::move(cfg)), anims_(std::move(anims)), exePath_(exePath), assetsDir_(assetsDir) {
    screenW_ = GetSystemMetrics(SM_CXSCREEN);
    screenH_ = GetSystemMetrics(SM_CYSCREEN);
    if (!cfg_.pets.empty()) {
        double size = cfg_.pets[0].size;
        if (size < 48) size = 48;  // sanity floor
        winW_ = (int)size;
        winH_ = (int)(size * 9.0 / 16.0 + 0.5);  // 16:9 rounded: default 350 -> 197
        if (winW_ > screenW_) {  // never exceed the screen
            winW_ = screenW_;
            winH_ = (int)(winW_ * 9.0 / 16.0 + 0.5);
        }
    }
}

PetWidget::~PetWidget() {
    removeTray();
    if (g_petInstance == this) g_petInstance = nullptr;
    if (memDC_) {
        if (dib_) SelectObject(memDC_, (HGDIOBJ)GetStockObject(DEFAULT_GUI_FONT));
        DeleteDC(memDC_);
    }
    if (dib_) DeleteObject(dib_);
    if (hwnd_) DestroyWindow(hwnd_);
}

bool PetWidget::create() {
    g_petInstance = this;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &PetWidget::wndProc;
    wc.hInstance = hInst_;
    wc.hIcon = LoadIconW(hInst_, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    wc.hbrBackground = nullptr;
    if (!RegisterClassExW(&wc)) return false;

    DWORD exStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    hwnd_ = CreateWindowExW(exStyle, kClassName, L"dsh-pet-standalone", WS_POPUP,
                            (int)winX_, (int)winY_, winW_, winH_, nullptr, nullptr, hInst_, this);
    if (!hwnd_) return false;

    // top-down 32bpp DIB for UpdateLayeredWindow
    ZeroMemory(&bmi_, sizeof(bmi_));
    bmi_.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi_.bmiHeader.biWidth = winW_;
    bmi_.bmiHeader.biHeight = -winH_;  // top-down
    bmi_.bmiHeader.biPlanes = 1;
    bmi_.bmiHeader.biBitCount = 32;
    bmi_.bmiHeader.biCompression = BI_RGB;
    memDC_ = CreateCompatibleDC(nullptr);
    dib_ = CreateDIBSection(nullptr, &bmi_, DIB_RGB_COLORS, (void**)&dibBits_, nullptr, 0);
    if (!memDC_ || !dib_ || !dibBits_) return false;
    SelectObject(memDC_, dib_);

    diagLog("app: %s pid=%lu embedded_anims=%d", kAppVersionDesc,
            (unsigned long)GetCurrentProcessId(), (int)anims_.size());
    SetTimer(hwnd_, 1, 8, nullptr);
    addTray(false);

    // initial corner placement
    if (!cfg_.pets.empty()) {
        const PositionCfg& pos = cfg_.pets[0].pos;
        double x = 0, y = 0;
        if (pos.corner == "top-left") { x = pos.marginX; y = pos.marginY; }
        else if (pos.corner == "top-right") { x = screenW_ - winW_ - pos.marginX; y = pos.marginY; }
        else if (pos.corner == "bottom-left") { x = pos.marginX; y = screenH_ - winH_ - pos.marginY; }
        else { x = screenW_ - winW_ - pos.marginX; y = screenH_ - winH_ - pos.marginY; }  // bottom-right
        placeAt(x, y);
    }
    play(firstIdle(), true);
    ShowWindow(hwnd_, SW_SHOW);
    return true;
}

int PetWidget::run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

void PetWidget::quit() {
    removeTray();
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

// ---------------------------------------------------------------- behaviour

double PetWidget::animDuration(int idx) const {
    if (idx < 0 || idx >= (int)anims_.size()) return 0;
    return anims_[idx].pack.duration();
}

int PetWidget::findAnim(const std::string& name) const {
    for (int i = 0; i < (int)anims_.size(); i++)
        if (anims_[i].name == name) return i;
    return -1;
}

int PetWidget::firstIdle() const {
    for (const auto& n : cfg_.animations.idle) {
        int idx = findAnim(n);
        if (idx >= 0) return idx;
    }
    return firstAvailable();
}

int PetWidget::firstAvailable() const {
    return anims_.empty() ? -1 : 0;
}

int PetWidget::pickFromNames(const std::vector<std::string>& names, const std::string& excludeName) {
    // Uniform pick from the pool of name indices, excluding excludeName (falls back to full pool).
    std::vector<int> pool;
    for (const auto& n : names) {
        int idx = findAnim(n);
        if (idx >= 0 && !(excludeName != "" && anims_[idx].name == excludeName)) pool.push_back(idx);
    }
    if (pool.empty()) {
        for (const auto& n : names) {
            int idx = findAnim(n);
            if (idx >= 0) pool.push_back(idx);
        }
    }
    if (pool.empty()) return -1;
    return pool[rndInt(0, (int)pool.size())];
}

int PetWidget::pickCategoryAction(const std::string& excludeName) {
    // port of pickWeightedCategory + pickCategoryAction
    std::vector<const CategoryCfg*> cats;
    for (const auto& c : cfg_.animations.categories)
        if (!c.actions.empty()) cats.push_back(&c);
    if (cats.empty()) return pickFromNames(cfg_.animations.idle, excludeName);

    std::vector<const CategoryCfg*> filtered;
    for (auto* c : cats)
        if (!(c->noMirror && facingRight_)) filtered.push_back(c);
    std::vector<const CategoryCfg*>& eligible = filtered.empty() ? cats : filtered;

    double totalW = 0;
    for (auto* c : eligible) totalW += c->weight;
    if (totalW <= 0) totalW = 1;
    double t = rnd01() * totalW;
    const CategoryCfg* chosen = eligible.back();
    for (auto* c : eligible) {
        t -= c->weight;
        if (t <= 0) { chosen = c; break; }
    }
    // pick action name excluding current
    std::vector<int> pool;
    for (const auto& n : chosen->actions) {
        int idx = findAnim(n);
        if (idx >= 0 && anims_[idx].name != excludeName) pool.push_back(idx);
    }
    if (pool.empty()) {
        for (const auto& n : chosen->actions) {
            int idx = findAnim(n);
            if (idx >= 0) pool.push_back(idx);
        }
    }
    if (!pool.empty()) return pool[rndInt(0, (int)pool.size())];
    return firstIdle();  // FALLBACK (plugin picks from idle pool)
}

void PetWidget::play(int animIdx, bool onceFlag) {
    if (animIdx < 0 || animIdx >= (int)anims_.size()) return;
    // Release previous animation's vpx decoders to free memory and threads
    if (curAnim_ >= 0 && curAnim_ != animIdx && curAnim_ < (int)anims_.size()) {
        auto& old = anims_[curAnim_].pack;
        if (old.webm) old.webm->releaseDecoders();
        if (old.webmAlpha) old.webmAlpha->releaseDecoders();
    }
    // switchTo dedupe: identical (anim, once) while still playing -> keep playing
    if (curAnim_ == animIdx && once_ == onceFlag && !endedHandled_) return;
    curAnim_ = animIdx;
    once_ = onceFlag;
    animStart_ = nowSec();
    endedHandled_ = false;
    curFrame_ = 0;
    FILE* dbg = nullptr;
    if (diagEnabled()) {
        fopen_s(&dbg, "pet-play-debug.txt", "a");
        if (dbg) {
            fprintf(dbg, "[%.1f] play anim=%d name=%s once=%d\n", nowSec(), animIdx, anims_[animIdx].name.c_str(), onceFlag ? 1 : 0);
            fclose(dbg);
        }
    }
    diagLog("play anim=%d name=%s once=%d", animIdx, anims_[animIdx].name.c_str(), onceFlag ? 1 : 0);
}

void PetWidget::stopMove() { move_.active = false; }

void PetWidget::handleEnded() {
    if (dragActive_ && dragging_) return;
    if (curAnim_ < 0) return;
    const std::string& cur = anims_[curAnim_].name;
    if (inPool(cfg_.animations.turn, cur)) {
        facingRight_ = !facingRight_;
    }
    if (inPool(cfg_.animations.drag, cur) || inPool(cfg_.animations.clicks, cur)) {
        play(pickFromNames(cfg_.animations.idle, cur), true);
        return;
    }
    pickNext();
}

void PetWidget::pickNext() {
    const std::string& cur = curAnim_ >= 0 ? anims_[curAnim_].name : "";
    double roll = rnd01();
    double idleW = cfg_.weights.idle, turnW = cfg_.weights.turn, moveW = cfg_.weights.move;
    double topEnd = (idleW + turnW + moveW) / 100.0;
    if (roll < idleW / 100.0) {
        play(pickFromNames(cfg_.animations.idle, cur), true);
    } else if (roll < (idleW + turnW) / 100.0) {
        play(pickFromNames(cfg_.animations.turn, cur), true);
    } else if (roll < topEnd) {
        if (!tryMove()) {
            play(pickCategoryAction(cur), true);
        }
    } else {
        play(pickCategoryAction(cur), true);
    }
}

bool PetWidget::tryMove() {
    // port of tryMove + planMove
    if (move_.active) return true;
    if (cfg_.animations.moves.actions.empty()) return false;
    const MoveActionCfg& act = cfg_.animations.moves.actions[rndInt(0, (int)cfg_.animations.moves.actions.size())];
    double minD = cfg_.animations.moves.minDist, maxD = cfg_.animations.moves.maxDist;
    double margin = cfg_.animations.moves.margin;
    double lead = cfg_.animations.moves.leadSec, tail = cfg_.animations.moves.tailSec;
    if (act.minDist >= 0) minD = act.minDist;
    if (act.maxDist >= 0) maxD = act.maxDist;
    if (act.margin >= 0) margin = act.margin;
    if (act.leadSec >= 0) lead = act.leadSec;
    if (act.tailSec >= 0) tail = act.tailSec;

    const std::string& cur = curAnim_ >= 0 ? anims_[curAnim_].name : "";
    bool isTurn = inPool(cfg_.animations.turn, cur);
    int dir = ((facingRight_ ? 1 : 0) != (isTurn ? 1 : 0)) ? 1 : -1;

    double halfW = winW_ / 2.0;
    double cx = winX_ + winW_ / 2.0;
    double cy = winY_ + winH_ / 2.0;
    double dist = rndInt((int)minD, (int)maxD);
    double target = cx + (double)dir * dist;
    double leftBound = margin + halfW;
    double rightBound = (double)screenW_ - margin - halfW;
    if (target < leftBound || target > rightBound) return false;

    int animIdx = findAnim(act.name);
    if (animIdx < 0) return false;
    play(animIdx, true);
    move_.active = true;
    move_.startX = cx;
    move_.startY = cy;
    move_.targetX = target;
    move_.dir = dir;
    move_.totalPx = std::abs(target - cx);
    move_.leadSec = lead;
    move_.tailSec = tail;
    move_.duration = anims_[animIdx].pack.duration();
    move_.finished = false;
    return true;
}

void PetWidget::onPointerDown(int sx, int sy) {
    stopMove();
    offX_ = sx - winX_ - winW_ / 2.0;
    offY_ = sy - winY_ - winH_ / 2.0;
    pressX_ = sx;
    pressY_ = sy;
    dragActive_ = true;
    dragging_ = false;
    SetCapture(hwnd_);
}

void PetWidget::onPointerMove(int sx, int sy) {
    if (!dragActive_) return;
    double dx = sx - pressX_, dy = sy - pressY_;
    if (!dragging_) {
        if (dx * dx + dy * dy < (double)(kDragThreshold * kDragThreshold)) return;
        dragging_ = true;
        once_ = true;
        play(pickFromNames(cfg_.animations.drag, ""), true);
    }
    placeAt(sx - offX_ - winW_ / 2.0, sy - offY_ - winH_ / 2.0);
}

void PetWidget::onPointerUp(int sx, int sy) {
    bool wasDragging = dragging_;
    dragActive_ = false;
    dragging_ = false;
    ReleaseCapture();
    if (wasDragging) {
        justDragged_ = true;
        justDraggedUntil_ = nowSec() + 0.12;
        // No clamping — allow free placement across all monitors.
        // SetWindowPos accepts negative/multi-monitor coords natively.
        const std::string& cur = curAnim_ >= 0 ? anims_[curAnim_].name : "";
        play(pickFromNames(cfg_.animations.idle, cur), true);  // one-shot, auto-switch
    }
}

void PetWidget::onClick() {
    const std::string& cur = curAnim_ >= 0 ? anims_[curAnim_].name : "";
    if (once_ && !inPool(cfg_.animations.idle, cur)) return;  // ignore clicks mid one-shot anim
    stopMove();
    once_ = true;
    play(pickFromNames(cfg_.animations.clicks, cur), true);
}

// ---------------------------------------------------------------- window loop

void PetWidget::tick() {
    if (!visible_ || curAnim_ < 0) return;
    const AnimUnit& p = anims_[curAnim_].pack;
    double now = nowSec();
    double t = now - animStart_;

    if (once_) {
        if (!endedHandled_ && t >= p.duration() && p.duration() > 0) {
            endedHandled_ = true;
            // freeze on last frame (plugin video with loop=false holds final frame)
            // but only play held frame if we're not dragging; handleEnded may react
            handleEnded();
            t = now - animStart_;  // reflect possible switch
            if (curAnim_ >= 0) {
                const AnimUnit& p2 = anims_[curAnim_].pack;
                double tf = now - animStart_;
                curFrame_ = (uint32_t)std::min((double)(p2.frameCount - 1), tf * p2.fps());
            }
        } else {
            curFrame_ = (uint32_t)std::min((double)(p.frameCount - 1), t * p.fps());
            if (t >= p.duration() && p.duration() <= 0) curFrame_ = p.frameCount - 1;
        }
    } else {
        // looping mode (once_=false): after ONE full cycle, switch to next animation.
        // Without this check the pet loops the same clip forever (never calls handleEnded).
        if (t >= p.duration() && p.duration() > 0) {
            handleEnded();
            return;  // next tick renders new animation
        }
        curFrame_ = ((uint32_t)(t * p.fps())) % p.frameCount;
    }

    if (move_.active && !dragging_) {
        double mt = now - animStart_;
        double x;
        if (mt <= move_.leadSec) {
            x = move_.startX;
        } else if (mt >= move_.duration - move_.tailSec) {
            x = move_.targetX;
        } else {
            double win = move_.duration - move_.leadSec - move_.tailSec;
            if (win <= 0) x = move_.targetX;
            else x = move_.startX + (double)move_.dir * move_.totalPx * (mt - move_.leadSec) / win;
        }
        placeAt(x - winW_ / 2.0, move_.startY - winH_ / 2.0);
    }
    paint();
}

void PetWidget::paint() {
    if (curAnim_ < 0 || !visible_) return;
    const AnimUnit& p = anims_[curAnim_].pack;
    static std::vector<uint8_t> scratch, frame;
    if (curFrame_ != lastFrame_ || facingRight_ != lastFacing_) {
        if (!p.decodeFrame(curFrame_, scratch, frame)) return;
        lastFrame_ = curFrame_;
        lastFacing_ = facingRight_;
        // scale + premultiply + optional mirror into the DIB (dibBits_ is premultiplied BGRA)
        const uint8_t* src = frame.data();
        uint32_t pw = p.w, ph = p.h;
        for (int dy = 0; dy < winH_; dy++) {
            double sy = ((double)dy + 0.5) * (double)ph / winH_ - 0.5;
            if (sy < 0) sy = 0;
            if (sy > ph - 1) sy = ph - 1;
            int y0 = (int)sy;
            double fy = sy - y0;
            int y1 = y0 + 1 < (int)ph ? y0 + 1 : y0;
            uint8_t* dstRow = dibBits_ + (size_t)dy * winW_ * 4;
            for (int dx = 0; dx < winW_; dx++) {
                double sx = ((double)dx + 0.5) * (double)pw / winW_ - 0.5;
                if (facingRight_) sx = (double)(pw - 1) - sx;  // mirror
                if (sx < 0) sx = 0;
                if (sx > pw - 1) sx = pw - 1;
                int x0 = (int)sx;
                double fx = sx - x0;
                int x1 = x0 + 1 < (int)pw ? x0 + 1 : x0;
                // bilinear + premultiply (straight BGRA -> premultiplied BGRA)
                size_t i00 = ((size_t)y0 * pw + x0) * 4;
                size_t i01 = ((size_t)y0 * pw + x1) * 4;
                size_t i10 = ((size_t)y1 * pw + x0) * 4;
                size_t i11 = ((size_t)y1 * pw + x1) * 4;
                unsigned a[4];
                for (int c = 0; c < 4; c++) {
                    double v = (1 - fx) * ((1 - fy) * src[i00 + c] + fy * src[i10 + c]) +
                               fx * ((1 - fy) * src[i01 + c] + fy * src[i11 + c]);
                    a[c] = (unsigned)(v + 0.5);
                    if (a[c] > 255) a[c] = 255;
                }
                unsigned alpha = a[3];
                dstRow[dx * 4 + 0] = (uint8_t)((a[0] * alpha + 127) / 255);  // B
                dstRow[dx * 4 + 1] = (uint8_t)((a[1] * alpha + 127) / 255);  // G
                dstRow[dx * 4 + 2] = (uint8_t)((a[2] * alpha + 127) / 255);  // R
                dstRow[dx * 4 + 3] = (uint8_t)alpha;
            }
        }
    }
    POINT pt{(int)winX_, (int)winY_};
    SIZE sz{winW_, winH_};
    POINT src{0, 0};
    UpdateLayeredWindow(hwnd_, nullptr, &pt, &sz, memDC_, &src, 0, &blend_, ULW_ALPHA);
}

void PetWidget::placeAt(double px, double py) {
    winX_ = px;
    winY_ = py;
    SetWindowPos(hwnd_, HWND_TOPMOST, (int)px, (int)py, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------- tray / menus

void PetWidget::addTray(bool forceReadd) {
    if (trayAdded_ && !forceReadd) return;
    NOTIFYICONDATAW nid{};
    // NOTIFYICON_VERSION_4 requires the v2 struct size (docs); sizeof() would also
    // work on modern systems but the mandated value keeps older shells happy.
#ifdef NOTIFYICONDATAW_V2_SIZE
    nid.cbSize = NOTIFYICONDATAW_V2_SIZE;
#else
    nid.cbSize = sizeof(NOTIFYICONDATAW);
#endif
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_GUID;
    static const GUID kPetTrayGuid = {
        0xd8a13b2e, 0x5e94, 0x4c6a,
        {0x9f, 0x11, 0x5a, 0x3c, 0xe1, 0x4b, 0x09, 0x7d}};
    nid.guidItem = kPetTrayGuid;
    nid.uCallbackMessage = WM_APP + 1;
    // Load the 16x16 app icon exactly at tray size; fall back to the standard
    // application icon so the entry is ALWAYS visible and right-clickable.
    nid.hIcon = (HICON)LoadImageW(hInst_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                  LR_DEFAULTCOLOR);
    if (!nid.hIcon) {  // defensive: never register an icon-less tray entry
        nid.hIcon = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
        diagLog("tray: LoadImageW(IDI_APP) failed, fell back to IDI_APPLICATION (GLE=%lu)",
                (unsigned long)GetLastError());
    }
    wchar_t tip[128];
    swprintf(tip, 128, L"dsh-pet-standalone v%s", kAppVersion);
    wcscpy(nid.szTip, tip);
    if (trayAdded_) Shell_NotifyIconW(NIM_DELETE, &nid);
    trayAdded_ = false;
    BOOL okAdd = Shell_NotifyIconW(NIM_ADD, &nid);
    diagLog("tray: NIM_ADD -> %d (GLE=%lu, hIcon=%p, guid=yes)", okAdd ? 1 : 0,
            (unsigned long)GetLastError(), (void*)nid.hIcon);
    if (okAdd) {
        nid.uVersion = NOTIFYICON_VERSION_4;  // v4: right-click arrives as WM_CONTEXTMENU
        BOOL okVer = Shell_NotifyIconW(NIM_SETVERSION, &nid);
        diagLog("tray: NIM_SETVERSION(4) -> %d (GLE=%lu)", okVer ? 1 : 0, (unsigned long)GetLastError());
        trayAdded_ = true;
        // v9+: on success, actively re-assert visibility once (some Windows 11
        // builds swallow the first NIM_ADD after Explorer churn).
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

void PetWidget::removeTray() {
    if (!trayAdded_) return;
    NOTIFYICONDATAW nid{};
#ifdef NOTIFYICONDATAW_V2_SIZE
    nid.cbSize = NOTIFYICONDATAW_V2_SIZE;
#else
    nid.cbSize = sizeof(NOTIFYICONDATAW);
#endif
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_GUID;  // match the GUID used by NIM_ADD
    static const GUID kPetTrayGuid = {
        0xd8a13b2e, 0x5e94, 0x4c6a,
        {0x9f, 0x11, 0x5a, 0x3c, 0xe1, 0x4b, 0x09, 0x7d}};
    nid.guidItem = kPetTrayGuid;
    BOOL ok = Shell_NotifyIconW(NIM_DELETE, &nid);
    diagLog("tray: NIM_DELETE -> %d (GLE=%lu)", ok ? 1 : 0, (unsigned long)GetLastError());
    trayAdded_ = false;
}

void PetWidget::applyPassthrough() {
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (passthrough_) {
        // TRUE passthrough: the window is invisible to the mouse — every click
        // (left AND right) goes to the window directly below the pet, as if the
        // pet did not exist. The pet's own context menu is NOT reachable by
        // right-click while passthrough is on (use the tray icon instead).
        ex |= WS_EX_TRANSPARENT;
        diagLog("menu: passthrough ON (WS_EX_TRANSPARENT, whole window mouse-transparent)");
    } else {
        ex &= ~WS_EX_TRANSPARENT;
        diagLog("menu: passthrough OFF");
    }
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
}

// ---------------------------------------------------------------- wndproc

void PetWidget::showContextMenu(int x, int y) {
    // Re-entry guard: one physical right-click can arrive as MULTIPLE messages
    // (tray v4 sends WM_CONTEXTMENU; the window path may get WM_RBUTTONUP and/or
    // a DefWindowProc-synthesised WM_CONTEXTMENU; a keyboard Shift+F10 emits
    // WM_CONTEXTMENU too). At most one menu within 300 ms — never a double menu.
    double now = nowSec();
    if (now - lastMenuMs_ < 0.3) {
        diagLog("menu: suppressed duplicate open (%.3f s since last)", now - lastMenuMs_);
        return;
    }
    lastMenuMs_ = now;

    // Identical menu for the pet window (right-click) and the tray icon.
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_SHOWHIDE, visible_ ? L"隐藏" : L"显示");
    AppendMenuW(m, MF_STRING | (passthrough_ ? MF_CHECKED : 0), IDM_PASSTHROUGH, L"鼠标穿透");
    bool on = isAutoStartImpl(exePath_);
    AppendMenuW(m, MF_STRING | (on ? MF_CHECKED : 0), IDM_AUTOSTART, L"开机启动");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_RESTART, L"重启");
    AppendMenuW(m, MF_STRING, IDM_QUIT, L"退出");
    // Correct menu protocol (MS docs): activate the window before tracking so
    // the menu can be dismissed. A WS_EX_TOOLWINDOW layered window may be denied
    // foreground rights (another process holds the foreground lock) — that is
    // ALWAYS the case for tray-originated right-clicks, so attach our input
    // queue to the current foreground thread (the standard tray-menu fix),
    // then nudge topmost and retry once before giving up.
    BOOL fg = SetForegroundWindow(hwnd_);
    if (!fg) {
        DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        DWORD myThread = GetCurrentThreadId();
        if (fgThread && fgThread != myThread) {
            AttachThreadInput(myThread, fgThread, TRUE);
            fg = SetForegroundWindow(hwnd_);
            AttachThreadInput(myThread, fgThread, FALSE);
        }
        if (!fg) {
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            fg = SetForegroundWindow(hwnd_);
        }
        diagLog("menu: SetForegroundWindow retried (fgThread=%lu GLE=%lu) -> %d",
                (unsigned long)fgThread, (unsigned long)GetLastError(), fg ? 1 : 0);
    }
    diagLog("menu: SetForegroundWindow=%d", fg ? 1 : 0);
    diagLog("menu: invoking TrackPopupMenu at %d,%d", x, y);  // pre-call: headless-safe invoke marker
    UINT cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, x, y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    diagLog("menu: TrackPopupMenu done at %d,%d -> cmd=%u", x, y, cmd);
    DestroyMenu(m);
    if (cmd) handleCommand(cmd);
}

void PetWidget::handleCommand(UINT id) {
    switch (id) {
        case IDM_SHOWHIDE:
            visible_ = !visible_;
            ShowWindow(hwnd_, visible_ ? SW_SHOW : SW_HIDE);
            break;
        case IDM_PASSTHROUGH:
            passthrough_ = !passthrough_;
            applyPassthrough();
            break;
                case IDM_AUTOSTART:
            setAutoStartImpl(exePath_, !isAutoStartImpl(exePath_));
            break;
        case IDM_RESTART: {
            ShellExecuteW(nullptr, L"open", exePath_.c_str(), L"--restarted", nullptr, SW_SHOWNORMAL);
            quit();
            break;
        }
        case IDM_QUIT:
            quit();
            break;
    }
}

// ---------------------------------------------------------------- wndproc

LRESULT CALLBACK PetWidget::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PetWidget* self = (PetWidget*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    static UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == taskbarCreated && self) {
        self->addTray(true);
        return 0;
    }
    if (self && msg != WM_NCCREATE) {
        LRESULT r = self->handleMessage(msg, wParam, lParam);
        if (r != kMsgUnhandled) return r;
    }
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return TRUE;
    }
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT PetWidget::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST: {
            // Whole window is client area (QQ-pet behaviour): the transparent margin
            // no longer pierces the mouse, so a right-click ANYWHERE on the window
            // (even over ~70% transparent pixels) reaches us and pops the menu.
            // Left-click semantics moved to explicit hit-box checks in WM_LBUTTONDOWN.
            return HTCLIENT;
        }
        case WM_LBUTTONDOWN: {
            POINT c{(short)LOWORD(lParam), (short)HIWORD(lParam)};
            pressHitInside_ = dshHitBox(c.x, c.y, winW_, winH_);
            diagLog("input: lbutton down client=%d,%d hitbox=%d",
                    c.x, c.y, pressHitInside_ ? 1 : 0);
            POINT p = c;
            ClientToScreen(hwnd_, &p);
            // Whole window is draggable (QQ-pet style, including the transparent
            // margin); the hit-box only gates the click REACTION below.
            onPointerDown(p.x, p.y);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (wParam & MK_LBUTTON) {
                POINT p{(short)LOWORD(lParam), (short)HIWORD(lParam)};
                ClientToScreen(hwnd_, &p);
                onPointerMove(p.x, p.y);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT p{(short)LOWORD(lParam), (short)HIWORD(lParam)};
            ClientToScreen(hwnd_, &p);
            if (dragActive_) {
                onPointerUp(p.x, p.y);  // clears dragActive_; drags also suppress the click
            }
            // Click reaction only when the press actually landed on the pet body
            // (transparent-area clicks are ignored: the old click-through equivalent,
            // now inside the window instead of on the desktop behind it).
            if (pressHitInside_ && !(justDragged_ && nowSec() < justDraggedUntil_)) {
                onClick();
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            POINT c{(short)LOWORD(lParam), (short)HIWORD(lParam)};
            bool across = c.x >= 0 && c.x < winW_ && c.y >= 0 && c.y < winH_;
            diagLog("menu: rbutton client=%d,%d in_window=%d hitbox=%d -> showContextMenu",
                    c.x, c.y, across ? 1 : 0, dshHitBox(c.x, c.y, winW_, winH_) ? 1 : 0);
            POINT p;
            GetCursorPos(&p);
            showContextMenu(p.x, p.y);
            return 0;
        }
        case WM_CONTEXTMENU: {
            // Window path for the keyboard Invoke-Context-Menu key / Shift+F10
            // (DefWindowProc also synthesises this from WM_RBUTTONUP when we do
            // NOT consume it; we consume WM_RBUTTONUP above, so this case only
            // fires for genuine keyboard/intent deliveries). showContextMenu's
            // re-entry guard prevents any duplicate menu.
            POINT p;
            GetCursorPos(&p);
            showContextMenu(p.x, p.y);
            return 0;
        }
        case WM_TIMER:
            tick();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_DISPLAYCHANGE:
            screenW_ = GetSystemMetrics(SM_CXSCREEN);
            screenH_ = GetSystemMetrics(SM_CYSCREEN);
            return 0;
        case WM_CLOSE:
            removeTray();
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_APP + 1: {
            // Tray callback. NOTIFYICON_VERSION_4 delivers the message in the
            // LOW 16 bits of lParam; the HIGH 16 bits carry flags (often 0x10000
            // on Win10/11) — comparing the raw value against WM_CONTEXTMENU
            // (0x7B) always failed, which is why tray right/left clicks did
            // nothing. Mask to the low word first.
            UINT tmsgFull = (UINT)lParam;
            UINT tmsg = tmsgFull & 0xFFFF;
            diagLog("tray: callback msg=%#x (low=%#x)", tmsgFull, tmsg);
            if (tmsg == WM_CONTEXTMENU || tmsg == WM_RBUTTONUP || tmsg == WM_LBUTTONUP) {
                POINT p;
                GetCursorPos(&p);
                showContextMenu(p.x, p.y);
            } else if (tmsg == WM_LBUTTONDBLCLK) {
                handleCommand(IDM_SHOWHIDE);
            }
            return 0;
        }
        default:
            return kMsgUnhandled;
    }
}