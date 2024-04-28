#include "socket_server.h"
#include "socket_server_loop.h"
#include <string.h>
#include <stdio.h>
#include <algorithm>

#define LOG(fmt, args...) printf("[%s:%d:%s]" fmt "\n", __FILE__, __LINE__, __FUNCTION__, ##args)

using namespace socketserver;

static void Event(const SocketEvent &e) {
    if(e.event == SocketEventType::READ) {
        static char buffer[1024];
        static int n = 0;

        LOG("<ID=%llu, LID=%llu> RECV size=%zu, data=(%s)", e.id, e.listener_id, e.read_event.size, HexRepr(e.read_event.data, 0, e.read_event.size).c_str());

        snprintf(buffer, sizeof(buffer), "HELLO %d", n++);
        size_t size = strlen(buffer);
        LOG("<ID=%llu, LID=%llu> SEND: size=%zu, data=(%s)", e.id, e.listener_id, size, HexRepr(buffer, 0, size).c_str());
        // e.server->SendCopy(e.id, buffer, strlen(buffer));
        e.server->SendtoCopy(e.id, *e.read_event.from_addr, buffer, strlen(buffer));
    }
}

int main() {
    std::unique_ptr<SocketServerInterface> s = CreateSocketServerObject();
    SocketServerLoop loop;
    loop.Init(s.get());

    s->Init(1024);

    SocketAddressNatural address;
    address.type = SocketAddressType::IPV4;
    strcpy(address.ipaddr4.ip, "127.0.0.1");
    address.ipaddr4.port = 12321;

    SocketId s4 = s->UdpConnect(ConvertSocketAddress(&address), Event);

    address.type = SocketAddressType::IPV6;
    strcpy(address.ipaddr6.ip, "::1");
    address.ipaddr6.port = 12322;
    address.ipaddr6.flow = 0;
    address.ipaddr6.scope = 0;

    SocketId s6 = s->UdpConnect(ConvertSocketAddress(&address), Event);

    char buffer[1024];

    snprintf(buffer, sizeof(buffer), "BEGIN");

    s->SendCopy(s4, buffer, strlen(buffer));
    s->SendCopy(s6, buffer, strlen(buffer));

    loop.Loop();

    loop.Destroy();
    s->Destroy();

    return 0;
}
