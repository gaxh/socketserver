#include "socket_server.h"
#include "socket_server_loop.h"
#include <string.h>
#include <stdio.h>
#include <algorithm>
#include <sys/time.h>

#include <vector>

#define LOG(fmt, args...) printf("[%s:%d:%s]" fmt "\n", __FILE__, __LINE__, __FUNCTION__, ##args)

using namespace socketserver;

static void Event(const SocketEvent &e) {
    if(e.event == SocketEventType::READ) {
        LOG("<ID=%llu, LID=%llu> RECV size=%zu", e.id, e.listener_id, e.read_event.size);
    } else if(e.event == SocketEventType::WRITE_REPORT) {
        LOG("<ID=%llu, LID=%llu> WRITE REPORT above=%d", e.id, e.listener_id, e.write_report_event.above_threshold);
    }
}

static void StressSend(SocketServerInterface *s, std::vector<SocketId> socket_ids, std::shared_ptr<int> stress_count) {
    char random[1000];

    for(int i = 0; i < 100; ++i) {
        for(size_t j = 0; j < socket_ids.size(); ++j) {
            s->SendCopy(socket_ids[j], random, sizeof(random));
        }
    }

    if((*stress_count)-- > 0) {
        s->SetTimeout(1000, std::bind(StressSend, s, socket_ids, stress_count));
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

    s->SetWriteReportThreshold(s4, 1000);
    s->SetWriteReportThreshold(s6, 1000);

    std::vector<SocketId> socket_ids;
    socket_ids.emplace_back(s4);
    socket_ids.emplace_back(s6);

    std::shared_ptr<int> stress_count = std::make_shared<int>(60);

    s->SetTimeout(1000, std::bind(StressSend, s.get(), socket_ids, stress_count));

    loop.Loop();
    loop.Destroy();
    s->Destroy();

    return 0;
}

