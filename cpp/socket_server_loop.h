#ifndef __SOCKET_SERVER_LOOP_H__
#define __SOCKET_SERVER_LOOP_H__

#include "socket_server.h"

namespace socketserver {

class SocketServerLoop {
public:
    void Init(SocketServerInterface *);
    void Destroy();
    void Loop();
    void Exit();

private:
    SocketServerInterface *m_ss = 0;
    bool m_exit = true;
    static bool s_exit;
};

}

#endif
