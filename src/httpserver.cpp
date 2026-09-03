// httpserver.cpp — 桌宠 HTTP 通知监听（v10）。
// 后台线程（Winsock2）监听 127.0.0.1:53021，HTTP 请求解析集中在
// extractHttpText()——API 文档拿到后只需改这一处。
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "httpserver.h"
#include "jsonc.h"
#include "util.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <process.h>

// ----------------------------------------------------------------------------
// 百分比解码（URL-encoded -> UTF-8）
static std::string urlDecode(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size(); i++) {
        char c = src[i];
        if (c == '+' && i + 2 <= src.size()) { out.push_back(' '); continue; }
        if (c == '%' && i + 2 < src.size()) {
            char hi = src[i + 1], lo = src[i + 2];
            auto hex = [](char d) -> int {
                if (d >= '0' && d <= '9') return d - '0';
                if (d >= 'a' && d <= 'f') return d - 'a' + 10;
                if (d >= 'A' && d <= 'F') return d - 'A' + 10;
                return -1;
            };
            int nh = hex(hi), nl = hex(lo);
            if (nh >= 0 && nl >= 0) { out.push_back((char)((nh << 4) | nl)); i += 2; continue; }
        }
        out.push_back(c);
    }
    return out;
}

// ----------------------------------------------------------------------------
// 请求解析（单点）：API 文档到位后只改这里
// 当前契约：JSON body 取 "msg" 字段；无则取 query ?msg=；再否则原始 body 当文本。
std::string extractHttpText(const std::string& method, const std::string& path, const std::string& body) {
    (void)method;
    // 1) JSON body → {"msg":"..."}
    std::string b = trim(body);
    if (!b.empty() && (b[0] == '{' || b[0] == '[')) {
        Json j = parseJsonc(b);
        if (j.isObj()) {
            const Json* f = j.get("msg");
            if (f && f->type == Json::Type::Str && !f->str.empty()) return f->str;
            // JSON 对象但没有 msg 字段：不显示（避免暴露原始 JSON）
            return "";
        }
    }
    // 2) body 非空 → 直接当文本（trimmed）
    if (!b.empty()) return b;
    // 3) query param ?msg=...
    size_t q = path.find('?');
    if (q != std::string::npos) {
        std::string qs = path.substr(q + 1);
        size_t p = 0;
        while (p < qs.size()) {
            size_t amp = qs.find('&', p);
            if (amp == std::string::npos) amp = qs.size();
            std::string kv = qs.substr(p, amp - p);
            if (kv.size() > 4 && kv.substr(0, 4) == "msg=") {
                std::string val = urlDecode(kv.substr(4));
                if (!val.empty()) return val;
            }
            p = amp + 1;
        }
    }
    return "";
}

// 小 HTTP 响应
static void sendResponse(SOCKET s, int code, const char* mime, const std::string& body) {
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                     code, code == 200 ? "OK" : "Bad Request", mime, body.size());
    send(s, head, n, 0);
    send(s, body.data(), (int)body.size(), 0);
}

// 读取一个 HTTP 请求（method / path / body）
static bool readRequest(SOCKET c, std::string& method, std::string& path, std::string& body) {
    std::string buf;
    char tmp[4096];
    // 读头（直到 \r\n\r\n），上限 64 KB
    while (buf.size() < 65536) {
        int r = recv(c, tmp, (int)sizeof(tmp), 0);
        if (r <= 0) return false;
        buf.append(tmp, (size_t)r);
        if (buf.find("\r\n\r\n") != std::string::npos) break;
        if (buf.find("\n\n") != std::string::npos) break;
    }
    // 找头尾
    size_t hEnd = buf.find("\r\n\r\n");
    size_t hEndLen = 4;
    bool hasLfLf = false;
    if (hEnd == std::string::npos) {
        hEnd = buf.find("\n\n");
        hEndLen = 2;
        hasLfLf = true;
    }
    if (hEnd == std::string::npos) return false;
    std::string head = buf.substr(0, hEnd);
    // 请求行
    size_t sp1 = head.find(' ');
    if (sp1 == std::string::npos) return false;
    method = head.substr(0, sp1);
    size_t sp2 = head.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;
    path = head.substr(sp1 + 1, sp2 - sp1 - 1);
    body = buf.substr(hEnd + hEndLen);
    // 读 Content-Length 剩余
    size_t cl = 0;
    std::string lower = head;
    for (auto& ch : lower) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    size_t clp = lower.find("content-length:");
    if (clp != std::string::npos) {
        clp += 15;
        while (clp < lower.size() && (lower[clp] == ' ' || lower[clp] == '\t')) clp++;
        cl = (size_t)std::atol(lower.c_str() + clp);
    }
    if (cl > 0) {
        while (body.size() < cl) {
            int r = recv(c, tmp, (int)std::min((size_t)sizeof(tmp), cl - body.size()), 0);
            if (r <= 0) return false;
            body.append(tmp, (size_t)r);
        }
    }
    return true;
}

// ------------------------------------------------------------------ HttpServer
HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(int port, std::function<void(const std::string&)> onText) {
    stop();
    stop_ = 0;
    onText_ = std::move(onText);
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { WSACleanup(); return false; }
    sock_ = (void*)(intptr_t)s;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    SOCKADDR_IN addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((u_short)port);
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s); sock_ = nullptr; WSACleanup(); return false;
    }
    if (listen(s, 8) == SOCKET_ERROR) {
        closesocket(s); sock_ = nullptr; WSACleanup(); return false;
    }
    thread_ = (void*)_beginthreadex(nullptr, 0, threadProc, this, 0, nullptr);
    if (!thread_) {
        closesocket(s); sock_ = nullptr; WSACleanup(); return false;
    }
    return true;
}

void HttpServer::stop() {
    if (thread_) {
        InterlockedExchange(&stop_, 1);
        SOCKET s = (SOCKET)(intptr_t)sock_;
        if (s != INVALID_SOCKET) {
            closesocket(s);
            sock_ = nullptr;
        }
        WaitForSingleObject((HANDLE)thread_, 3000);
        CloseHandle((HANDLE)thread_);
        thread_ = nullptr;
        WSACleanup();
    }
}

unsigned __stdcall HttpServer::threadProc(void* self) {
    ((HttpServer*)self)->run();
    return 0;
}

void HttpServer::run() {
    SOCKET s = (SOCKET)(intptr_t)sock_;
    if (s == INVALID_SOCKET) return;
    while (!stop_) {
        // select 轮询 stop_，避免 closesocket 唤起后的竞态
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        TIMEVAL tv{0, 200000};  // 200 ms 轮询
        int sel = select(0, &rd, nullptr, nullptr, &tv);
        if (sel == SOCKET_ERROR) break;
        if (sel == 0) continue;  // timeout, re-check stop_
        SOCKET c = accept(s, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            if (stop_) break;
            continue;
        }
        // 处理单连接（串行；通知场景流量极低）
        std::string method, path, body;
        if (readRequest(c, method, path, body)) {
            std::string text = extractHttpText(method, path, body);
            if (!text.empty() && onText_) onText_(text);
            // 只有成功提取到文本并交付才 ok:true；否则 ok:false
            if (!text.empty()) {
                sendResponse(c, 200, "application/json", "{\"ok\":true}");
            } else {
                sendResponse(c, 200, "application/json", "{\"ok\":false}");
            }
        } else {
            sendResponse(c, 400, "application/json", "{\"ok\":false}");
        }
        closesocket(c);
    }
    if (s != INVALID_SOCKET) closesocket(s);
    onText_ = nullptr;
}