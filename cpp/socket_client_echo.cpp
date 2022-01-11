#include "socket_server.h"
#include <string.h>
#include <stdio.h>
#include <algorithm>

#define LOG(fmt, args...) printf("[%s:%d:%s]" fmt "\n", __FILE__, __LINE__, __FUNCTION__, ##args)

static void Event(const SocketServer::SOCKET_EVENT &e) {
    if(e.EVENT == SocketServer::SOCKET_EVENT_READ) {
        static char buffer[1024];
        static int n = 0;

        LOG("<ID=%llu> RECV size=%zu, data=(%s)", e.ID, e.SIZE, SocketServer::HexRepr(e.ARRAY, e.OFFSET, e.SIZE).c_str());

        snprintf(buffer, sizeof(buffer), "HELLO %d", n++);
        size_t size = strlen(buffer);
        LOG("<ID=%llu> SEND: size=%zu, data=(%s)", e.ID, size, SocketServer::HexRepr(buffer, 0, size).c_str());
        e.SERVER->SendCopy(e.ID, buffer, 0, strlen(buffer));
    }
}

int main() {
    SocketServer s;
    SocketServerLoop loop;
    loop.Init(&s);

    s.Init();
    SocketServer::SOCKET_ID s4 = s.Connect4("127.0.0.1", 12321, Event);
    SocketServer::SOCKET_ID s6 = s.Connect6("::1", 12322, Event);

    char buffer[1024];

    snprintf(buffer, sizeof(buffer), "BEGIN");
    
    s.SendCopy(s4, buffer, 0, strlen(buffer));
    s.SendCopy(s6, buffer, 0, strlen(buffer));

    loop.Loop();

    s.Destroy();
    loop.Destroy();

    return 0;
}
