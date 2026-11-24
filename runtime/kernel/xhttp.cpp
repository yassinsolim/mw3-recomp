// XHttp, the console's asynchronous HTTP client (WinHTTP's shape), for the one
// thing a title here uses it for: Advanced Warfare's multiplayer logs on to
// its publisher's DemonWare servers with one HTTPS request before it opens
// its lobby connection (online/demonware.h).
//
// Nothing here opens a socket. A request is answered in this process by the
// DemonWare stand-in, or fails as a request to a host that does not exist
// would; the bytes the title parses were written here.
//
// The calls and their order are the title's bdXHTTP (default_mp.xex, the
// request thread at 821BAA18 and its status callback at 821B9788): Open once,
// then for each request CrackUrl, Connect, OpenRequest, SetStatusCallback and
// SendRequest; the body by WriteData; ReceiveResponse; the status and the
// length by QueryHeaders; the body by ReadData. Each of those that completes
// later on the console completes here at the next DoWork, on the thread that
// calls it, which is where the console runs the callback too.
#include <ppc_recomp_shared.h>
#include "kernel.h"
#include "objects.h"
#include "../guest.h"
#include "../log.h"
#include "../online/demonware.h"
#include "../online/service.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

using namespace kernel;

namespace
{
    // The callback's status values (WINHTTP_CALLBACK_STATUS_*).
    constexpr uint32_t kHeadersAvailable = 0x00020000, kReadComplete = 0x00080000, kWriteComplete = 0x00100000,
                       kRequestError = 0x00200000, kSendRequestComplete = 0x00400000;
    // XHttpQueryHeaders' levels as the title passes them, both with the flag
    // that asks for a number.
    constexpr uint32_t kQueryStatusCode = 0x2000FFFE, kQueryContentLength = 0x20000009;
    constexpr uint32_t kSchemeHttp = 1, kSchemeHttps = 2;
    constexpr size_t kLargestBody = 0x10000;

    struct Session : Object
    {
        const char* TypeName() const override { return "http session"; }
    };

    struct Connection : Object
    {
        std::string host;
        const char* TypeName() const override { return "http connection"; }
    };

    struct Notice
    {
        uint32_t status = 0, information = 0, length = 0;
    };

    struct Request : Object
    {
        std::string host, path;
        uint32_t callback = 0, context = 0;
        // Two words of guest memory the callback's information points at: how
        // much was written, or a failed request's result and error.
        uint32_t scratch = 0;
        std::string body, reply;
        bool answered = false;
        size_t read = 0;
        std::deque<Notice> events;

        ~Request() override { if (scratch) FreeGuest(scratch); }
        const char* TypeName() const override { return "http request"; }
    };

    // The requests with a callback still to make, for DoWork.
    std::mutex g_lock;
    std::deque<std::shared_ptr<Request>> g_busy;

    void Queue(const std::shared_ptr<Request>& request, uint32_t status, uint32_t information = 0,
               uint32_t length = 0)
    {
        std::lock_guard g(g_lock);
        request->events.push_back({ status, information, length });
        if (std::find(g_busy.begin(), g_busy.end(), request) == g_busy.end()) g_busy.push_back(request);
    }

    std::string GuestText(uint32_t address, size_t most = 0x4000)
    {
        const auto* text = GuestPtr<char>(address);
        return text ? std::string(text, strnlen(text, most)) : std::string();
    }
}

PPC_FUNC(__imp__NetDll_XHttpStartup) { ctx.r3.u64 = 1; }
PPC_FUNC(__imp__NetDll_XHttpShutdown) { ctx.r3.u64 = 1; }

// (caller, agent, access type, proxy, proxy bypass, flags)
PPC_FUNC(__imp__NetDll_XHttpOpen)
{
    ctx.r3.u64 = InsertHandle(std::make_shared<Session>());
}

PPC_FUNC(__imp__NetDll_XHttpCloseHandle)
{
    if (auto request = LookupHandleAs<Request>(ctx.r4.u32))
    {
        std::lock_guard g(g_lock);
        std::erase(g_busy, request);
    }
    ctx.r3.u64 = CloseHandle(ctx.r4.u32) ? 1 : 0;
}

// (caller, url, length, flags, URL_COMPONENTS*). The structure is WinHTTP's:
// for each part a pointer and a length, where a null pointer with a length
// asks for a pointer into the URL and a pointer asks for a copy.
// { size, scheme, schemeLength, nScheme, host, hostLength, port, user,
//   userLength, password, passwordLength, path, pathLength, extra, extraLength }
PPC_FUNC(__imp__NetDll_XHttpCrackUrl)
{
    const uint32_t urlAt = ctx.r4.u32;
    auto* parts = GuestPtr<be32>(ctx.r7.u32);
    const std::string url = GuestText(urlAt).substr(0, ctx.r5.u32 ? ctx.r5.u32 : std::string::npos);
    const size_t schemeEnd = url.find("://");
    ctx.r3.u64 = 0;
    if (!parts || parts[0] < 0x3C || schemeEnd == std::string::npos) return;
    const std::string scheme = url.substr(0, schemeEnd);
    const bool secure = scheme == "https";
    if (!secure && scheme != "http") return;
    const size_t hostAt = schemeEnd + 3;
    const size_t pathAt = std::min(url.find('/', hostAt), url.size());
    std::string host = url.substr(hostAt, pathAt - hostAt);
    uint32_t port = secure ? 443 : 80;
    if (const size_t colon = host.find(':'); colon != std::string::npos)
    {
        port = uint32_t(std::strtoul(host.c_str() + colon + 1, nullptr, 10));
        host.resize(colon);
    }

    // One part: its pointer and length words, and where it is in the URL.
    const auto part = [&](uint32_t word, size_t at, size_t length) {
        if (uint32_t(parts[word]))
        {
            auto* out = GuestPtr<char>(uint32_t(parts[word]));
            if (!out || uint32_t(parts[word + 1]) <= length) return false;
            std::memcpy(out, url.data() + at, length);
            out[length] = 0;
        }
        else if (uint32_t(parts[word + 1])) parts[word] = urlAt + uint32_t(at);
        else return true;
        parts[word + 1] = uint32_t(length);
        return true;
    };
    if (!part(1, 0, schemeEnd) || !part(4, hostAt, host.size()) || !part(11, pathAt, url.size() - pathAt)) return;
    parts[3] = secure ? kSchemeHttps : kSchemeHttp;
    *reinterpret_cast<be16*>(parts + 6) = uint16_t(port);
    ctx.r3.u64 = 1;
}

// (caller, session, server name, port, flags)
PPC_FUNC(__imp__NetDll_XHttpConnect)
{
    auto connection = std::make_shared<Connection>();
    connection->host = GuestText(ctx.r5.u32, 0x180);
    ctx.r3.u64 = LookupHandleAs<Session>(ctx.r4.u32) ? InsertHandle(connection) : 0;
}

// (caller, connection, verb, object name, version, referrer, accept types, flags)
PPC_FUNC(__imp__NetDll_XHttpOpenRequest)
{
    auto connection = LookupHandleAs<Connection>(ctx.r4.u32);
    if (!connection) { ctx.r3.u64 = 0; return; }
    auto request = std::make_shared<Request>();
    request->host = connection->host;
    request->path = GuestText(ctx.r6.u32);
    request->scratch = AllocateGuest(8, 8);
    ctx.r3.u64 = request->scratch ? InsertHandle(request) : 0;
}

// (caller, handle, callback, notification flags, reserved): the callback
// before.
PPC_FUNC(__imp__NetDll_XHttpSetStatusCallback)
{
    auto request = LookupHandleAs<Request>(ctx.r4.u32);
    if (!request) { ctx.r3.u64 = 0; return; }
    const uint32_t before = request->callback;
    request->callback = ctx.r5.u32;
    ctx.r3.u64 = before;
}

// (caller, request, headers, their length, optional data, its length, the
// body's whole length, context)
PPC_FUNC(__imp__NetDll_XHttpSendRequest)
{
    auto request = LookupHandleAs<Request>(ctx.r4.u32);
    const auto* optional = GuestPtr<char>(ctx.r7.u32);
    if (!request || ctx.r8.u32 > kLargestBody) { ctx.r3.u64 = 0; return; }
    request->context = ctx.r10.u32;
    if (optional) request->body.assign(optional, ctx.r8.u32);
    Queue(request, kSendRequestComplete);
    ctx.r3.u64 = 1;
}

// (caller, request, buffer, length, written out)
PPC_FUNC(__imp__NetDll_XHttpWriteData)
{
    auto request = LookupHandleAs<Request>(ctx.r4.u32);
    const auto* bytes = GuestPtr<char>(ctx.r5.u32);
    if (!request || !bytes || request->body.size() + ctx.r6.u32 > kLargestBody) { ctx.r3.u64 = 0; return; }
    request->body.append(bytes, ctx.r6.u32);
    *GuestPtr<be32>(request->scratch) = ctx.r6.u32;
    Queue(request, kWriteComplete, request->scratch, 4);
    ctx.r3.u64 = 1;
}

// (caller, request, reserved): the request is whole, and is answered.
PPC_FUNC(__imp__NetDll_XHttpReceiveResponse)
{
    auto request = LookupHandleAs<Request>(ctx.r4.u32);
    if (!request) { ctx.r3.u64 = 0; return; }
    request->answered = online::demonware::Http(request->host, request->path, request->body, LocalLiveXuid(),
                                                request->reply);
    if (online::Trace())
        LOGI("net-trace: http %s%s, %zu bytes: %s", request->host.c_str(), request->path.c_str(),
             request->body.size(), request->answered ? "answered" : "no such server");
    if (request->answered) Queue(request, kHeadersAvailable);
    else
    {
        constexpr uint32_t kReceiveResponse = 2, kCannotConnect = 12029;
        auto* failure = GuestPtr<be32>(request->scratch);
        failure[0] = kReceiveResponse;
        failure[1] = kCannotConnect;
        Queue(request, kRequestError, request->scratch, 8);
    }
    ctx.r3.u64 = 1;
}

// (caller, request, level, name, buffer, its size in and out, index)
PPC_FUNC(__imp__NetDll_XHttpQueryHeaders)
{
    auto request = LookupHandleAs<Request>(ctx.r4.u32);
    auto* out = GuestPtr<be32>(ctx.r7.u32);
    auto* size = GuestPtr<be32>(ctx.r8.u32);
    ctx.r3.u64 = 0;
    if (!request || !request->answered || !out || !size || *size < 4) return;
    if (ctx.r5.u32 == kQueryStatusCode) *out = 200;
    else if (ctx.r5.u32 == kQueryContentLength) *out = uint32_t(request->reply.size());
    else return;
    *size = 4;
    ctx.r3.u64 = 1;
}

// (caller, request, buffer, length, read out)
PPC_FUNC(__imp__NetDll_XHttpReadData)
{
    auto request = LookupHandleAs<Request>(ctx.r4.u32);
    auto* out = GuestPtr<char>(ctx.r5.u32);
    if (!request || !request->answered || !out) { ctx.r3.u64 = 0; return; }
    const size_t length = std::min(size_t(ctx.r6.u32), request->reply.size() - request->read);
    std::memcpy(out, request->reply.data() + request->read, length);
    request->read += length;
    Queue(request, kReadComplete, ctx.r5.u32, uint32_t(length));
    ctx.r3.u64 = 1;
}

// (caller, session, timeout): the callbacks that are due, on this thread.
PPC_FUNC(__imp__NetDll_XHttpDoWork)
{
    for (;;)
    {
        std::shared_ptr<Request> request;
        Notice event;
        {
            std::lock_guard g(g_lock);
            if (g_busy.empty()) break;
            request = g_busy.front();
            event = request->events.front();
            request->events.pop_front();
            if (request->events.empty()) g_busy.pop_front();
        }
        if (request->callback)
            CallGuest(ctx, request->callback, request->handle, request->context, event.status, event.information,
                      event.length);
    }
    ctx.r3.u64 = 1;
}
