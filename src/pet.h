// pet.h — desktop-pet layered window + behaviour engine.
// A faithful C++ port of the plugin logic in dsh-pet src/client (pickers.ts, motion.ts, pet.ts),
// adapted to a single topmost per-pixel-alpha Win32 window per process.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define IDI_APP 1   // prevent Windows min/max macros from clashing with std::min/max
#include "config.h"
#include "anim.h"
#include "bubble.h"
#include "httpserver.h"
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

// Version marker — printed by --version / --help, in the selftest header and in
// the tray tooltip, so a user can tell at a glance whether the running binary
// is the current build (old 1.x exes predate the real-asset embedding and the
// tray v4 menu fix; v5 covered only the hit-box right-click; v6 embedded the
// .pka v2 lossy packs; v7 embeds the ORIGINAL VP9-alpha thumb WebM and decodes
// them in memory at full 640x360@24 quality; v8 converted the assets ONCE to a
// dual-stream pair (main + alpha-as-gray VP9) so EVERY frame gets a fresh alpha
// plane — the "held silhouette" dark-shadow bug is gone; v9 fixes tray
// right-click (AttachThreadInput foreground fix) and makes passthrough a TRUE
// whole-window mouse-transparency (no right-click bridge — use the tray menu).
// v10 adds the HTTP notification bubble: listens on 127.0.0.1:53021 and shows
// pushed text in a floating bubble above the pet head (10s, click-through,
// tray toggleable)).
inline const wchar_t kAppVersion[] = L"10.0";
inline const char kAppVersionDesc[] =
    "dsh-pet-standalone v10.0 (91 animations, dual-stream VP9, per-frame alpha, "
    "tray menu + true passthrough, single exe; HTTP notification bubble "
    "127.0.0.1:53021; perf: lazy ULW submit, 62Hz tick, scaled-source lookup "
    "tables, decoder/scratch buffer release on switch)";

// Canonical pet-body hit test, shared by the window (WM_LBUTTONDOWN), the diag
// logging and the selftest: HIT_BOX (200,50)-(440,335) on the 640x360 thumb
// canvas maps to fractions of the window's actual size. Right edge inclusive
// semantics match the original single-file hit test exactly.
bool dshHitBox(int clientX, int clientY, int winW, int winH);

// Menu command ids (shared by window context menu and tray menu: identical menus).
enum : UINT {
    IDM_SHOWHIDE = 1,
    IDM_PASSTHROUGH = 2,
    IDM_AUTOSTART = 3,
    IDM_RESTART = 4,
    IDM_QUIT = 5,
    IDM_BUBBLE = 6,  // v10: 通知气泡 on/off
};

class PetWidget {
public:
    PetWidget(HINSTANCE hInst, AppConfig cfg, std::vector<LoadedAnim> anims,
              const std::wstring& exePath, const std::wstring& assetsDir);
    ~PetWidget();

    bool create();   // register class, create layered window, tray
    int run();       // message loop
    void quit();
    int animCount() const { return (int)anims_.size(); }

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    // ---- platform ----
    HINSTANCE hInst_ = nullptr;
    HWND hwnd_ = nullptr;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    uint8_t* dibBits_ = nullptr;
    int winW_ = 0, winH_ = 0;
    BITMAPINFO bmi_{};
    BLENDFUNCTION blend_{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    int screenW_ = 0, screenH_ = 0;
    std::wstring exePath_, assetsDir_;
    bool visible_ = true;
    bool passthrough_ = false;
    bool trayAdded_ = false;

    // ---- config / assets ----
    AppConfig cfg_;
    std::vector<LoadedAnim> anims_;

    // ---- pet state (port of PetCard) ----
    int facingRight_ = 0;          // 0 = left
    bool dragging_ = false;        // threshold passed
    bool dragActive_ = false;
    int pressX_ = 0, pressY_ = 0;
    double offX_ = 0, offY_ = 0;
    bool justDragged_ = false;
    double justDraggedUntil_ = 0;
    bool once_ = true;             // current animation plays once (vs loops)
    int curAnim_ = -1;             // index into anims_
    double animStart_ = 0;         // seconds (nowSec)
    bool endedHandled_ = false;
    uint32_t curFrame_ = 0;
    uint32_t lastFrame_ = UINT32_MAX;
    int lastFacing_ = -1;
    bool firstPaint_ = true;         // v9.1: first paint always submits the surface
    double winX_ = 0, winY_ = 0;   // window top-left (px)
    double lastMenuMs_ = 0;        // context-menu re-entry guard (nowSec)
    bool pressHitInside_ = false;  // WM_LBUTTONDOWN landed inside the pet hit box

    struct MovePlan {
        bool active = false;
        double startX = 0, startY = 0, targetX = 0;
        int dir = 1;
        double totalPx = 0, leadSec = 0, tailSec = 0, duration = 0;
        bool finished = false;
    } move_;

    // ---- internals ----
    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void tick();
    void paint();
    void computeDrawnFrame();
    void placeAt(double px, double py);
    void handleEnded();
    void pickNext();
    bool tryMove();
    void stopMove();
    void onPointerDown(int x, int y);
    void onPointerMove(int x, int y);
    void onPointerUp(int x, int y);
    void onClick();
    void play(int animIdx, bool onceFlag);
    int findAnim(const std::string& name) const;
    int firstIdle() const;
    int firstAvailable() const;
    int pickInPoolIdx(const std::vector<int>& poolIdx, int excludeIdx);
    int pickFromNames(const std::vector<std::string>& names, const std::string& excludeName);
    int pickCategoryAction(const std::string& excludeName);
    void showContextMenu(int x, int y);
    void handleCommand(UINT id);
    void addTray(bool forceReadd);
    void removeTray();
    void applyPassthrough();
    void onHttpText(const std::string& utf8);  // HTTP thread → 主线程气泡
    double animDuration(int idx) const;

    // ---- v10: HTTP notification bubble ----
    BubbleWindow bubble_;
    HttpServer http_;
    bool bubbleOn_ = true;            // 菜单开关（默认开）
    bool bubbleShowing_ = false;      // 当前有气泡在显示
    double bubbleShownAt_ = 0;        // nowSec 时间戳（10s 自动消失）
    std::string httpPending_;         // HTTP 线程交付的文本（UTF-8）
    CRITICAL_SECTION httpLock_{};     // 保护 httpPending_
};