/* ===========================================================================
 *  httpd.cpp — 极简 HTTP/1.1 服务器实现（Winsock2）
 * =========================================================================== */
#include "httpd.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>

static HttpHandler        g_handler;
static SOCKET             g_listen   = INVALID_SOCKET;
static std::atomic<bool>  g_stop(false);
static std::string        g_lastErr;

/* --------------------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------------------ */
static std::string LowerAscii(const std::string &s)
{
    std::string r = s;
    for (size_t i = 0; i < r.size(); ++i)
        if (r[i] >= 'A' && r[i] <= 'Z') r[i] = (char)(r[i] - 'A' + 'a');
    return r;
}

static std::string Trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r' || s[b-1] == '\n')) --b;
    return s.substr(a, b - a);
}

static int HexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* %XX 与 + 解码 */
static std::string UrlDecode(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int h = HexVal(s[i+1]), l = HexVal(s[i+2]);
            if (h >= 0 && l >= 0) { out.push_back((char)(h * 16 + l)); i += 2; continue; }
        }
        if (s[i] == '+') { out.push_back(' '); continue; }
        out.push_back(s[i]);
    }
    return out;
}

/* 从 "a=1&b=2" 里取键值 */
static std::string QueryGet(const std::string &query, const std::string &key)
{
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        std::string kv = query.substr(pos, amp - pos);
        size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            if (UrlDecode(kv.substr(0, eq)) == key)
                return UrlDecode(kv.substr(eq + 1));
        }
        pos = amp + 1;
    }
    return std::string();
}

static bool SendAll(SOCKET c, const char *p, size_t n)
{
    while (n > 0) {
        int sent = send(c, p, (int)n, 0);
        if (sent <= 0) return false;
        p += sent;
        n -= (size_t)sent;
    }
    return true;
}

static const char *Reason(int code)
{
    switch (code) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default:  return "OK";
    }
}

/* --------------------------------------------------------------------------
 * 请求解析
 * ------------------------------------------------------------------------ */
static bool ReadRequest(SOCKET c, HttpRequest &req)
{
    std::string buf;
    char tmp[8192];
    size_t hdrEnd = std::string::npos;

    while (buf.size() < 65536) {
        int n = recv(c, tmp, (int)sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, (size_t)n);
        hdrEnd = buf.find("\r\n\r\n");
        if (hdrEnd != std::string::npos) break;
    }
    if (hdrEnd == std::string::npos) return false;

    std::string head = buf.substr(0, hdrEnd);
    std::string rest = buf.substr(hdrEnd + 4);

    size_t eol = head.find("\r\n");
    std::string reqLine = (eol == std::string::npos) ? head : head.substr(0, eol);
    std::string hdrPart = (eol == std::string::npos) ? std::string() : head.substr(eol + 2);

    /* 请求行: METHOD SP target SP VERSION */
    size_t s1 = reqLine.find(' ');
    if (s1 == std::string::npos) return false;
    size_t s2 = reqLine.find(' ', s1 + 1);
    req.method = reqLine.substr(0, s1);
    std::string target = reqLine.substr(s1 + 1,
                        (s2 == std::string::npos ? reqLine.size() : s2) - s1 - 1);

    size_t qm = target.find('?');
    if (qm == std::string::npos) {
        req.path  = UrlDecode(target);
    } else {
        req.path  = UrlDecode(target.substr(0, qm));
        req.query = target.substr(qm + 1);
    }

    /* 头字段 */
    std::string contentLength = "0";
    size_t pos = 0;
    while (pos < hdrPart.size()) {
        size_t nl = hdrPart.find("\r\n", pos);
        if (nl == std::string::npos) nl = hdrPart.size();
        std::string line = hdrPart.substr(pos, nl - pos);
        pos = nl + 2;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = LowerAscii(Trim(line.substr(0, colon)));
        std::string val  = Trim(line.substr(colon + 1));
        if      (name == "host")           req.host   = val;
        else if (name == "origin")         req.origin = val;
        else if (name == "x-token")        req.token  = val;
        else if (name == "content-length") contentLength = val;
    }

    /* token 也允许放在 query 里 */
    if (req.token.empty() && !req.query.empty())
        req.token = QueryGet(req.query, "token");

    /* 请求体 */
    long cl = atol(contentLength.c_str());
    if (cl < 0) cl = 0;
    if (cl > 1024 * 1024) return false;
    while ((long)rest.size() < cl) {
        int n = recv(c, tmp, (int)sizeof(tmp), 0);
        if (n <= 0) break;
        rest.append(tmp, (size_t)n);
    }
    req.body = rest.substr(0, (size_t)cl);
    return true;
}

static void SendResponse(SOCKET c, const HttpResponse &r)
{
    std::string h;
    h.reserve(256 + r.body.size());
    h += "HTTP/1.1 ";
    h += std::to_string(r.status);
    h += " ";
    h += Reason(r.status);
    h += "\r\nContent-Type: ";
    h += r.contentType;
    h += "\r\nContent-Length: ";
    h += std::to_string(r.body.size());
    h += "\r\nCache-Control: no-store\r\nConnection: close\r\n";
    h += r.extraHeaders;
    h += "\r\n";

    if (!SendAll(c, h.data(), h.size())) return;
    if (!r.body.empty()) SendAll(c, r.body.data(), r.body.size());
}

/* --------------------------------------------------------------------------
 * 单连接处理
 * ------------------------------------------------------------------------ */
static void HandleConn(SOCKET c)
{
    DWORD tv = 10000;   /* 10s，防止半开连接把线程吊死 */
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));

    HttpRequest req;
    if (ReadRequest(c, req)) {
        HttpResponse r;
        if (g_handler) {
            r = g_handler(req);
        } else {
            r.SetText(500, "no handler");
        }
        SendResponse(c, r);
    }
    shutdown(c, SD_BOTH);
    closesocket(c);
}

/* --------------------------------------------------------------------------
 * accept 循环
 * ------------------------------------------------------------------------ */
static void AcceptLoop(void)
{
    while (!g_stop.load()) {
        SOCKET c = accept(g_listen, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        std::thread(HandleConn, c).detach();
    }
}

unsigned short HttpServeStart(HttpHandler handler, unsigned short port)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        g_lastErr = "WSAStartup 失败";
        return 0;
    }

    g_handler = handler;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        g_lastErr = "创建 socket 失败";
        WSACleanup();
        return 0;
    }

    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&yes, sizeof(yes));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* 只绑 127.0.0.1 */

    if (bind(s, (sockaddr *)&addr, sizeof(addr)) != 0) {
        g_lastErr = "bind 127.0.0.1 失败";
        closesocket(s);
        WSACleanup();
        return 0;
    }
    if (listen(s, 32) != 0) {
        g_lastErr = "listen 失败";
        closesocket(s);
        WSACleanup();
        return 0;
    }

    int alen = (int)sizeof(addr);
    if (getsockname(s, (sockaddr *)&addr, &alen) != 0) {
        g_lastErr = "getsockname 失败";
        closesocket(s);
        WSACleanup();
        return 0;
    }

    g_listen = s;
    g_stop.store(false);
    std::thread(AcceptLoop).detach();

    return ntohs(addr.sin_port);
}

void HttpServeStop(void)
{
    g_stop.store(true);
    if (g_listen != INVALID_SOCKET) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
    }
    WSACleanup();
}

const char *HttpServeLastError(void)
{
    return g_lastErr.c_str();
}
