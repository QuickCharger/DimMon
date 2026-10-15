/* ===========================================================================
 *  httpd.h — 极简 HTTP/1.1 服务器（Winsock2，零第三方依赖）
 *  ---------------------------------------------------------------------------
 *  设计目标：只服务本机回环上的一个配置页面 + 几个 JSON 接口。
 *  因此刻意不支持：TLS、HTTP/2、chunked、keep-alive、multipart。
 *  =========================================================================== */
#ifndef HTTPD_H
#define HTTPD_H

#include <string>
#include <functional>

struct HttpRequest {
    std::string method;      // GET / POST
    std::string path;        // 不含 query
    std::string query;       // ? 之后（不含 ?）
    std::string body;        // 已按 Content-Length 读全
    std::string token;       // X-Token 头，或 ?token= 参数
    std::string origin;      // Origin 头（浏览器跨域请求才有）
    std::string host;        // Host 头
    std::string clientIp;
};

struct HttpResponse {
    int         status = 200;
    std::string contentType = "text/plain; charset=utf-8";
    std::string body;
    std::string extraHeaders;   // 每行以 \r\n 结尾

    void SetJson(const std::string &json) {
        status = 200;
        contentType = "application/json; charset=utf-8";
        body = json;
    }
    void SetHtml(const std::string &html) {
        status = 200;
        contentType = "text/html; charset=utf-8";
        body = html;
    }
    void SetText(int code, const std::string &text) {
        status = code;
        contentType = "text/plain; charset=utf-8";
        body = text;
    }
};

typedef std::function<HttpResponse(const HttpRequest &)> HttpHandler;

/* 启动服务：绑定 127.0.0.1。port=0 表示由系统分配随机端口。
 * 成功返回实际监听端口（1-65535），失败返回 0。 */
unsigned short HttpServeStart(HttpHandler handler, unsigned short port = 0);

/* 停止服务（关闭监听套接字，accept 线程退出） */
void HttpServeStop(void);

/* 取最后一次错误描述（启动失败时用） */
const char *HttpServeLastError(void);

#endif /* HTTPD_H */
