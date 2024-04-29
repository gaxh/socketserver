#ifndef __SOCKET_SERVER_LOOP_H__
#define __SOCKET_SERVER_LOOP_H__

#include "socket_server.h"

namespace socketserver {

namespace loop {

void Init(SocketServerInterface *);
void Destroy();
void Loop();
void Exit();

}

}

#endif
