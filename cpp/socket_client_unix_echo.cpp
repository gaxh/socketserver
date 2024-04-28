#include "socket_server.h"
#include "socket_server_loop.h"
#include <string.h>
#include <stdio.h>
#include <algorithm>

#define LOG(fmt, args...) printf("[%s:%d:%s] " fmt "\n", __FILE__, __LINE__, __FUNCTION__, ##args)

using namespace socketserver;

static void Event(const SocketEvent &e) {
    if(e.event == SocketEventType::READ) {
        static char buffer[1024];
        static int n = 0;

        LOG("<ID=%llu, LID=%llu> RECV size=%zu, data=(%s)", e.id, e.listener_id, e.read_event.size, HexRepr(e.read_event.data, 0, e.read_event.size).c_str());

        snprintf(buffer, sizeof(buffer), "HELLO %d", n++);
        size_t size = strlen(buffer);
        LOG("<ID=%llu, LID=%llu> SEND: size=%zu, data=(%s)", e.id, e.listener_id, size, HexRepr(buffer, 0, size).c_str());
        e.server->SendCopy(e.id, buffer, strlen(buffer));
    }
}

int main() {
    std::unique_ptr<SocketServerInterface> s = CreateSocketServerObject();
    SocketServerLoop loop;
    loop.Init(s.get());

    s->Init(1024);

    SocketAddressNatural address;
    address.type = SocketAddressType::UNIX;
    strcpy(address.unixaddr.path, "/tmp/socket_server_unix_echo.socket");

    SocketId s4 = s->Connect(ConvertSocketAddress(&address), Event);

    char buffer[1024];

    snprintf(buffer, sizeof(buffer), "BEGIN");

    s->SendCopy(s4, buffer, strlen(buffer));

    loop.Loop();

    loop.Destroy();
    s->Destroy();

    return 0;
}
