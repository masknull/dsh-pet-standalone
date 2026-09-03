// bubble.cpp — 通知气泡窗口：独立分层窗口 + GDI+ 渲染。
// 白底圆角 + 底部小尾巴指向宠物头；单行文本超宽截断为 "...";
// GDI+ 输出 straight ARGB → 手工预乘 → UpdateLayeredWindow(ULW_ALPHA)。
#include "bubble.h"
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace Gdiplus;

std::wstring ellipsizeForWidth(const std::wstring& text, int maxW,
                               const std::function<int(const std::wstring&)>& widthOf) {
    if (text.empty() || maxW <= 0) return text;
    if (widthOf(text) <= maxW) return text;  // fits fully
    const std::wstring ell = L"...";
    int ellW = widthOf(ell);
    int budget = maxW - ellW;
    if (budget <= 0) return ell;  // nothing fits; show bare ellipsis
    // longest prefix whose width <= budget, scanning back from the tail
    std::wstring prefix = text;
    int end = (int)text.size();
    while (end > 0) {
        // avoid splitting a UTF-16 surrogate pair
        if (end >= 2 && text[end - 1] >= 0xDC00 && text[end - 1] <= 0xDFFF &&
            text[end - 2] >= 0xD800 && text[end - 2] <= 0xDBFF) {
            end -= 2;
        } else {
            end -= 1;
        }
        if (widthOf(text.substr(0, end)) <= budget) break;
    }
    prefix = text.substr(0, end);
    return prefix + ell;
}

namespace {

const wchar_t kBubbleClass[] = L"dsh-pet-bubble";

// font size / padding scale with the pet width (mirrors upstream 0.0455 / 0.03 / 0.022 / 0.035 / 0.017)
int fontPxFor(int petW) {
    return std::max(12, std::min(30, (int)std::lround(petW * 0.0455)));
}

}  // namespace

BubbleWindow::~BubbleWindow() {
    destroy();
}

bool BubbleWindow::create(HINSTANCE hInst, int petW, int screenW, int screenH) {
    if (created_) return true;
    hInst_ = hInst;
    petW_ = petW;
    screenW_ = screenW;
    screenH_ = screenH;
    fontPx_ = fontPxFor(petW_);
    padX_ = std::max(6, (int)std::lround(petW_ * 0.030));
    padY_ = std::max(4, (int)std::lround(petW_ * 0.022));
    radius_ = std::max(4, (int)std::lround(petW_ * 0.035));
    tailH_ = std::max(3, (int)std::lround(petW_ * 0.017));
    tailHalfW_ = tailH_;
    // 宽度上限：随宠物缩放，但不超过屏幕一半左右（防“撑死屏幕”）
    maxTextW_ = std::max(120, (int)std::lround(petW_ * 1.1));
    if (maxTextW_ > screenW_ - 64) maxTextW_ = std::max(120, screenW_ - 64);
    maxTextW_ = std::min(maxTextW_, 560);

    // GDI+（进程级 token，仅一次）
    if (gdiToken_ == 0) {
        GdiplusStartupInput in;
        if (GdiplusStartup(&gdiToken_, &in, nullptr) != Ok) gdiToken_ = 0;
    }

    // 字体：优先微软雅黑 UI / 微软雅黑，回退宋体 / Arial
    static const wchar_t* kFonts[] = {L"Microsoft YaHei UI", L"Microsoft YaHei", L"SimSun", L"Arial"};
    if (!font_) {
        for (auto* name : kFonts) {
            FontFamily ff(name);
            if (ff.IsAvailable()) {
                font_ = new Font(&ff, (REAL)fontPx_, FontStyleRegular, UnitPixel);
                break;
            }
        }
        if (!font_) font_ = new Font(L"Arial", (REAL)fontPx_, FontStyleRegular, UnitPixel);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &BubbleWindow::wndProc;
    wc.hInstance = hInst_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kBubbleClass;
    wc.hbrBackground = nullptr;
    if (!RegisterClassExW(&wc)) return false;

    DWORD ex = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    hwnd_ = CreateWindowExW(ex, kBubbleClass, L"", WS_POPUP, 0, 0, 8, 8,
                            nullptr, nullptr, hInst_, this);
    if (!hwnd_) return false;
    created_ = true;
    resizeSurface(2, 2);  // initial tiny DIB
    return true;
}

void BubbleWindow::destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
    dibBits_ = nullptr;
    w_ = h_ = 0;
    if (font_) { delete (Font*)font_; font_ = nullptr; }
    if (gdiToken_) {
        GdiplusShutdown(gdiToken_);
        gdiToken_ = 0;
    }
    created_ = false;
    visible_ = false;
}

void BubbleWindow::resizeSurface(int nw, int nh) {
    if (memDC_) DeleteDC(memDC_);
    if (dib_) DeleteObject(dib_);
    memDC_ = nullptr;
    dib_ = nullptr;
    dibBits_ = nullptr;
    ZeroMemory(&bmi_, sizeof(bmi_));
    bmi_.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi_.bmiHeader.biWidth = nw;
    bmi_.bmiHeader.biHeight = -nh;  // top-down
    bmi_.bmiHeader.biPlanes = 1;
    bmi_.bmiHeader.biBitCount = 32;
    bmi_.bmiHeader.biCompression = BI_RGB;
    memDC_ = CreateCompatibleDC(nullptr);
    dib_ = CreateDIBSection(nullptr, &bmi_, DIB_RGB_COLORS, (void**)&dibBits_, nullptr, 0);
    if (memDC_ && dib_ && dibBits_) SelectObject(memDC_, dib_);
    w_ = nw;
    h_ = nh;
    if (hwnd_) SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, nw, nh, SWP_NOMOVE | SWP_NOACTIVATE);
}

int BubbleWindow::measureWidth(const std::wstring& s) {
    if (s.empty() || !memDC_ || !font_) return 0;
    Graphics g(memDC_);
    StringFormat sf(StringFormat::GenericTypographic());
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    RectF layout(0, 0, (REAL)maxTextW_, 2000);
    RectF box;
    Status st = g.MeasureString(s.c_str(), (INT)s.size(), (Font*)font_, layout, &sf, &box);
    if (st != Ok) return (int)(s.size() * fontPx_ * 0.9);
    return (int)(box.Width + 0.5f);
}

int BubbleWindow::measureHeight(const std::wstring& s) {
    if (s.empty() || !memDC_ || !font_) return fontPx_;
    Graphics g(memDC_);
    StringFormat sf(StringFormat::GenericTypographic());
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    RectF layout(0, 0, (REAL)maxTextW_, 2000);
    RectF box;
    Status st = g.MeasureString(s.c_str(), (INT)s.size(), (Font*)font_, layout, &sf, &box);
    if (st != Ok) return fontPx_;
    return (int)(box.Height + 0.5f);
}

void BubbleWindow::setText(const std::wstring& text) {
    text_ = text;
    if (text_.empty()) { hide(); return; }
    if (!created_ || !memDC_ || !dibBits_) return;
    auto widthOf = [this](const std::wstring& s) { return measureWidth(s); };
    text_ = ellipsizeForWidth(text_, maxTextW_, widthOf);
    int textW = std::max(measureWidth(text_), 4);
    int textH = std::max(measureHeight(text_), fontPx_);
    int nw = textW + padX_ * 2;
    int nh = textH + padY_ * 2 + tailH_;
    if (nw != w_ || nh != h_) resizeSurface(nw, nh);
    paint();
}

void BubbleWindow::paint() {
    if (!created_ || !hwnd_ || !memDC_ || !dibBits_ || w_ <= 0 || h_ <= 0) return;
    // clear to transparent
    ZeroMemory(dibBits_, (size_t)w_ * h_ * 4);
    {
        Bitmap bmp(w_, h_, w_ * 4, PixelFormat32bppARGB, dibBits_);
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.SetCompositingMode(CompositingModeSourceOver);

        int bodyH = h_ - tailH_;  // body region above the tail
        REAL r = (REAL)radius_;
        REAL bw = (REAL)w_, bh = (REAL)bodyH;
        REAL zr = 0, rtwo = r * 2;
        // rounded-rect body
        GraphicsPath body;
        body.AddArc(zr, zr, rtwo, rtwo, 180.0f, 90.0f);
        body.AddArc(bw - rtwo, zr, rtwo, rtwo, 270.0f, 90.0f);
        body.AddArc(bw - rtwo, bh - rtwo, rtwo, rtwo, 0.0f, 90.0f);
        body.AddArc(zr, bh - rtwo, rtwo, rtwo, 90.0f, 90.0f);
        body.CloseFigure();
        // tail triangle (pointing down at the pet head)
        REAL cx = w_ / 2.0f, tw = (REAL)tailHalfW_;
        PointF tri[3] = {
            PointF(cx - tw, (REAL)bodyH),
            PointF(cx + tw, (REAL)bodyH),
            PointF(cx, (REAL)h_),
        };
        // 纯白在白色软件窗口上不显眼 → 用接近不透明白 + 1px 灰边勾勒轮廓
        SolidBrush fill(Color(250, 255, 255, 255));  // 几乎不透明白
        g.FillPath(&fill, &body);
        g.FillPolygon(&fill, tri, 3);
        // 1px 淡灰描边（路径内缩 0.5px，避免右/下边缘被 DIB 裁剪）
        Pen border(Color(120, 170, 170, 170), 1.0f);
        GraphicsPath bodyInset;
        REAL hi = 0.5f, bwi = bw - 1, bhi = bh - 1, rtwoi = (std::max)(rtwo - 1, 0.0f);
        bodyInset.AddArc(hi, hi, rtwoi, rtwoi, 180.0f, 90.0f);
        bodyInset.AddArc(bwi - rtwoi, hi, rtwoi, rtwoi, 270.0f, 90.0f);
        bodyInset.AddArc(bwi - rtwoi, bhi - rtwoi, rtwoi, rtwoi, 0.0f, 90.0f);
        bodyInset.AddArc(hi, bhi - rtwoi, rtwoi, rtwoi, 90.0f, 90.0f);
        bodyInset.CloseFigure();
        g.DrawPath(&border, &bodyInset);

        // text, centered in the body（近黑，保证对比度）
        SolidBrush ink(Color(255, 20, 20, 20));
        StringFormat sf(StringFormat::GenericTypographic());
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        RectF tr(0, 0, (REAL)w_, (REAL)bodyH);
        g.DrawString(text_.c_str(), (INT)text_.size(), (Font*)font_, tr, &sf, &ink);
    }
    // GDI+ 输出 straight ARGB → 手工预乘（UpdateLayeredWindow 需要 premultiplied）
    for (int y = 0; y < h_; y++) {
        uint8_t* p = dibBits_ + (size_t)y * w_ * 4;
        for (int x = 0; x < w_; x++, p += 4) {
            uint8_t a = p[3];
            if (a == 0) { p[0] = p[1] = p[2] = 0; continue; }
            p[0] = (uint8_t)(((int)p[0] * a + 127) / 255);
            p[1] = (uint8_t)(((int)p[1] * a + 127) / 255);
            p[2] = (uint8_t)(((int)p[2] * a + 127) / 255);
        }
    }
    RECT rc;
    GetWindowRect(hwnd_, &rc);
    POINT pt{rc.left, rc.top};
    SIZE sz{w_, h_};
    POINT src{0, 0};
    UpdateLayeredWindow(hwnd_, nullptr, &pt, &sz, memDC_, &src, 0, &blend_, ULW_ALPHA);
}

void BubbleWindow::show() {
    if (!created_) return;
    if (!visible_) {
        visible_ = true;
        ShowWindow(hwnd_, SW_SHOW);
    }
    paint();
}

void BubbleWindow::hide() {
    if (!created_) return;
    visible_ = false;
    ShowWindow(hwnd_, SW_HIDE);
}

void BubbleWindow::positionNear(double petX, double petY, int petW, int petH, int screenW, int screenH) {
    if (!created_) return;  // 即使窗口尚未显示，也允许定位（修复首次/超时重显位置错误）
    (void)screenW;
    (void)screenH;
    int centerX = (int)(petX + petW / 2.0);
    // 尾巴尖对准宠物头部上方：头部约在画布 y≈50/360 → 窗口顶往下 winH*0.18 处
    int headY = (int)(petY + petH * 0.18);
    int bx = centerX - w_ / 2;
    int by = headY - h_;  // 窗口底部=尾巴尖=头部上方
    // 限制在宠物所在显示器的工作区内
    POINT pt{centerX, (int)petY};
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        LONG lx = bx, ly = by;
        lx = std::max(mi.rcWork.left, std::min(lx, mi.rcWork.right - (LONG)w_));
        ly = std::max(mi.rcWork.top, std::min(ly, mi.rcWork.bottom - (LONG)h_));
        bx = (int)lx;
        by = (int)ly;
    } else {
        bx = std::max(0, std::min(bx, screenW_ - w_));
        by = std::max(0, std::min(by, screenH_ - h_));
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, bx, by, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

LRESULT CALLBACK BubbleWindow::wndProc(HWND h, UINT m, WPARAM wParam, LPARAM lParam) {
    if (m == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return TRUE;
    }
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, m, wParam, lParam);
}
