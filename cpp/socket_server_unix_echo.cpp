#include "socket_server.h"
#include "socket_server_loop.h"
#include <stdio.h>
#include <string.h>

using namespace socketserver;

#define LOG(fmt, args...) printf("[%s:%d:%s] " fmt "\n", __FILE__, __LINE__, __FUNCTION__, ##args)

static void Event(const SocketEvent &e) {
    switch(e.event) {
        case SocketEventType::OPEN:
            LOG("Connected: id=%llu, addr=%s, lid=%llu", e.id, DumpSocketAddress(e.addr).c_str(), e.listener_id);
            break;
        case SocketEventType::CLOSE:
            LOG("Disconnected: id=%llu, addr=%s, reason=%d, lid=%llu", e.id, DumpSocketAddress(e.addr).c_str(), e.close_event.close_reason, e.listener_id);
            break;
        case SocketEventType::READ:
            LOG("Received: id=%llu, addr=%s, size=%zu, data=(%s), lid=%llu", e.id, DumpSocketAddress(e.addr).c_str(), e.read_event.size, e.read_event.size < 50 ? HexRepr(e.read_event.data, 0, e.read_event.size).c_str() : "<IGNORED>", e.listener_id);
            e.server->SendCopy(e.id, e.read_event.data, e.read_event.size);
            break;
        case SocketEventType::WRITE_REPORT:
            LOG("WriteReportThreshold: id=%llu, addr=%s, lid=%llu, above=%d", e.id, DumpSocketAddress(e.addr).c_str(), e.listener_id, e.write_report_event.above_threshold);
            break;
        default:
            break;
    }
}

int main() {
    std::unique_ptr<SocketServerInterface> s = CreateSocketServerObject();

    loop::Init(s.get());

    s->Init(1024);

    SocketAddressNatural address;
    address.type = SocketAddressType::UNIX;
    strcpy(address.unixaddr.path, "/tmp/socket_server_unix_echo.socket");

    SocketId l4 = s->Listen(ConvertSocketAddress(&address), Event);

    s->SetWriteReportThreshold(l4, 1000);

    loop::Loop();

    loop::Destroy();
    s->Destroy();

    return 0;
}
