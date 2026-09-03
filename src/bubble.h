// bubble.h — 通知气泡窗口（v10）。
// 独立分层窗口（WS_EX_LAYERED | TOPMOST | TOOLWINDOW | TRANSPARENT | NOACTIVATE），
// 悬浮于宠物头顶上方：白底圆角 + 底部小尾巴指向宠物，单行文本。
// 文本超宽时尾部截断并追加 "..."（ellipsizeForWidth，纯逻辑可单测）。
// 与动画系统无耦合：宠物照常播放，本窗口只负责展示。
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <functional>
#include <cstdint>

// 纯函数：按 widthOf 测像素宽，超 maxW 时截断到最长前缀并追加 L"..."。
// 自动避免在 UTF-16 代理对中间截断。widthOf 由调用方注入（GDI+ 测量 / 假测量器）。
std::wstring ellipsizeForWidth(const std::wstring& text, int maxW,
                               const std::function<int(const std::wstring&)>& widthOf);

class BubbleWindow {
public:
    BubbleWindow() = default;
    ~BubbleWindow();
    BubbleWindow(const BubbleWindow&) = delete;
    BubbleWindow& operator=(const BubbleWindow&) = delete;

    // 注册类 + 创建隐藏分层窗口 + 初始化 GDI+。petW = 宠物窗口宽（布局缩放基准）。
    bool create(HINSTANCE hInst, int petW, int screenW, int screenH);
    void destroy();

    void setText(const std::wstring& text);  // 测量/省略/重排/重绘（仍保持当前显隐）
    void show();                             // ShowWindow + 提交表面
    void hide();
    bool visible() const { return visible_; }
    const std::wstring& text() const { return text_; }

    // 以宠物窗口为锚点定位：气泡窗口底部（尾巴尖）对准宠物头部上方，水平居中于宠物。
    void positionNear(double petX, double petY, int petW, int petH, int screenW, int screenH);

    static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l);

private:
    int measureWidth(const std::wstring& s);
    int measureHeight(const std::wstring& s);
    void resizeSurface(int nw, int nh);
    void paint();

    HINSTANCE hInst_ = nullptr;
    HWND hwnd_ = nullptr;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    uint8_t* dibBits_ = nullptr;
    int w_ = 0, h_ = 0;
    BITMAPINFO bmi_{};
    BLENDFUNCTION blend_{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

    std::wstring text_;
    bool visible_ = false;
    bool created_ = false;
    int petW_ = 350;
    int screenW_ = 1920, screenH_ = 1080;
    int fontPx_ = 16;
    int padX_ = 10, padY_ = 8, radius_ = 12, tailH_ = 6, tailHalfW_ = 6;
    int maxTextW_ = 420;

    void* font_ = nullptr;      // Gdiplus::Font*
    ULONG_PTR gdiToken_ = 0;    // GDI+ startup token
};