OPTIMIZE := -O3

all : socket_server_echo

socket_server_echo : socket_server_echo.cpp socket_server.h socket_server.cpp
	g++ socket_server_echo.cpp socket_server.cpp -o $@ -g ${OPTIMIZE} -Wall

all : socket_client_echo

socket_client_echo : socket_client_echo.cpp socket_server.h socket_server.cpp
	g++ socket_client_echo.cpp socket_server.cpp -o $@ -g ${OPTIMIZE} -Wall

all : socket_server_udp_echo

socket_server_udp_echo : socket_server_udp_echo.cpp socket_server.h socket_server.cpp
	g++ socket_server_udp_echo.cpp socket_server.cpp -o $@ -g ${OPTIMIZE} -Wall

all : socket_client_udp_echo

socket_client_udp_echo : socket_client_udp_echo.cpp socket_server.h socket_server.cpp
	g++ socket_client_udp_echo.cpp socket_server.cpp -o $@ -g ${OPTIMIZE} -Wall

all : socket_client_stress

socket_client_stress : socket_client_stress.cpp socket_server.h socket_server.cpp
	g++ socket_client_stress.cpp socket_server.cpp -o $@ -g ${OPTIMIZE} -Wall

all : socket_client_udp_stress

socket_client_udp_stress : socket_client_udp_stress.cpp socket_server.h socket_server.cpp
	g++ socket_client_udp_stress.cpp socket_server.cpp -o $@ -g ${OPTIMIZE} -Wall

all : socket_client_udp_broadcast

socket_client_udp_broadcast : socket_client_udp_broadcast.cpp socket_server.h socket_server.cpp
	g++ socket_client_udp_broadcast.cpp socket_server.cpp -o $@ -g ${OPTIMIZE} -Wall

all : socket_server_udp_broadcast

socket_server_udp_broadcast : socket_server_udp_broadcast.cpp socket_server.h socket_server.cpp
	g++ socket_server_udp_broadcast.cpp socket_server.cpp -o $@ -g ${OPTIMIZE} -Wall

clean:
	rm -f socket_server_echo
	rm -f socket_client_echo
	rm -f socket_server_udp_echo
	rm -f socket_client_udp_echo
	rm -f socket_client_stress
	rm -f socket_client_udp_stress
	rm -f socket_client_udp_broadcast
	rm -f socket_server_udp_broadcast
