#include "socket_server.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <algorithm>

#define LOG(fmt, args...) printf("[%s:%d:%s]" fmt "\n", __FILE__, __LINE__, __FUNCTION__, ##args)

static void Event(const SocketServer::SOCKET_EVENT &e) {
    if(e.EVENT == SocketServer::SOCKET_EVENT_READ) {
        LOG("<ID=%llu, LID=%llu> RECV size=%zu, data=(%s), from_ip=%s, from_port=%hu, from_v6=%d", e.ID, e.LISTENER_ID, e.SIZE, SocketServer::HexRepr(e.ARRAY, e.OFFSET, e.SIZE).c_str(),
                e.FROM_ADDR->IP, e.FROM_ADDR->PORT, e.FROM_ADDR->V6);
    }
}

int main() {
    SocketServer s;
    SocketServerLoop loop;
    loop.Init(&s);

    s.Init();

    SocketServer::SOCKET_ADDRESS addr;
    strcpy(addr.IP, "0.0.0.0");
    addr.PORT = 12322;
    addr.V6 = false;
    SocketServer::SOCKET_ID s4 = s.UdpBind(addr, Event);
    loop.Loop();

    s.Destroy();
    loop.Destroy();
}

