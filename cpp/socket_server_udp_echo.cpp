#include "socket_server.h"
#include <stdio.h>
#include <string.h>

#define LOG(fmt, args...) printf("[%s:%d:%s]" fmt "\n", __FILE__, __LINE__, __FUNCTION__, ##args)

static void Event(const SocketServer::SOCKET_EVENT &e) {
    switch(e.EVENT) {
        case SocketServer::SOCKET_EVENT_OPEN:
            LOG("Connected: id=%llu, ip=%s, port=%hu, v6=%d, lid=%llu", e.ID, e.ADDR->IP, e.ADDR->PORT, e.ADDR->V6, e.LISTENER_ID);
            break;
        case SocketServer::SOCKET_EVENT_CLOSE:
            LOG("Disconnected: id=%llu, ip=%s, port=%hu, v6=%d, reason=%d, lid=%llu", e.ID, e.ADDR->IP, e.ADDR->PORT, e.ADDR->V6, e.CLOSE_REASON, e.LISTENER_ID);
            break;
        case SocketServer::SOCKET_EVENT_READ:
            LOG("Received: id=%llu, ip=%s, port=%hu, v6=%d, size=%zu, data=(%s), lid=%llu, from_ip=%s, from_port=%hu, from_v6=%d", e.ID, e.ADDR->IP, e.ADDR->PORT, e.ADDR->V6, e.SIZE, e.SIZE < 50 ? SocketServer::HexRepr(e.ARRAY, e.OFFSET, e.SIZE).c_str() : "<IGNORED>", e.LISTENER_ID, e.FROM_ADDR->IP, e.FROM_ADDR->PORT, e.FROM_ADDR->V6);
            //e.SERVER->SendUdpCopy(e.ID, *e.FROM_ADDR, e.ARRAY, e.OFFSET, e.SIZE);
            e.SERVER->SendUdpCopy(e.ID, e.FROM_UDP_ID, e.ARRAY, e.OFFSET, e.SIZE);
            break;
        case SocketServer::SOCKET_EVENT_WRITE_REPORT_THRESHOLD:
            LOG("WriteReportThreshold: id=%llu, ip=%s, port=%hu, v6=%d, lid=%llu, above=%d", e.ID, e.ADDR->IP, e.ADDR->PORT, e.ADDR->V6, e.LISTENER_ID, e.ABOVE_THRESHOLD);
            break;

        default:
            break;
    }
}

int main() {
    SocketServer s;
    SocketServerLoop loop;
    loop.Init(&s);

    s.Init();
    SocketServer::SOCKET_ADDRESS addr;
    strcpy(addr.IP, "0.0.0.0");
    addr.PORT = 12321;
    addr.V6 = false;
    SocketServer::SOCKET_ID u4 = s.UdpBind(addr, Event);
    strcpy(addr.IP, "::");
    addr.PORT = 12322;
    addr.V6 = true;
    SocketServer::SOCKET_ID u6 = s.UdpBind(addr, Event);

    s.SetWriteReportThreshold(u4, 2000);
    s.SetWriteReportThreshold(u6, 2000);

    loop.Loop();

    s.Destroy();
    loop.Destroy();

    return 0;
}
