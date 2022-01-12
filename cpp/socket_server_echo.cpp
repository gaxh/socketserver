#include "socket_server.h"
#include <stdio.h>

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
            LOG("Received: id=%llu, ip=%s, port=%hu, v6=%d, size=%zu, data=(%s), lid=%llu", e.ID, e.ADDR->IP, e.ADDR->PORT, e.ADDR->V6, e.SIZE, SocketServer::HexRepr(e.ARRAY, e.OFFSET, e.SIZE).c_str(), e.LISTENER_ID);
            e.SERVER->SendCopy(e.ID, e.ARRAY, e.OFFSET, e.SIZE);
            break;
    }
}

int main() {
    SocketServer s;
    SocketServerLoop loop;
    loop.Init(&s);

    s.Init();
    s.Listen4("127.0.0.1", 12321, Event);
    s.Listen6("::1", 12322, Event);

    loop.Loop();

    s.Destroy();
    loop.Destroy();

    return 0;
}
