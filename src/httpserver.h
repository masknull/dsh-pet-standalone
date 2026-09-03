// httpserver.h — 桌宠 HTTP 通知监听（v10）。
// 后台线程监听 127.0.0.1:<port>（默认 53021），解析请求 → 提取展示文本（UTF-8）
// → 回调（PetWidget 收到后 PostMessage 到 UI 线程 → 气泡显示）。
// 协议解析集中在 extractHttpText()：后续拿到 API 接口文档时只需改这一个函数。
#pragma once
#include <string>
#include <functional>

// 从请求中提取要在气泡里显示的文本（UTF-8）。空串 = 不显示。
// method/path/body 为原始 HTTP 请求要素。API 文档到位后在此按新契约解析。
std::string extractHttpText(const std::string& method, const std::string& path, const std::string& body);

class HttpServer {
public:
    HttpServer() = default;
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // 启动监听。onText 在后台线程调用（UTF-8）；需自行切到 UI 线程。
    // 端口被占用/启动失败时返回 false（不崩溃、不阻塞）。
    bool start(int port, std::function<void(const std::string&)> onText);
    void stop();  // 同步收尾并 join 后台线程
    bool running() const { return thread_ != nullptr; }

private:
    static unsigned __stdcall threadProc(void* self);
    void run();

    void* thread_ = nullptr;  // HANDLE
    void* sock_ = nullptr;    // SOCKET (listening)
    volatile long stop_ = 0;
    std::function<void(const std::string&)> onText_;
};
