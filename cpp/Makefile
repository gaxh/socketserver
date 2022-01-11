all : socket_server_echo

socket_server_echo : socket_server_echo.cpp socket_server.h socket_server.cpp
	g++ socket_server_echo.cpp socket_server.cpp -o $@ -g -O2 -Wall

all : socket_client_echo

socket_client_echo : socket_client_echo.cpp socket_server.h socket_server.cpp
	g++ socket_client_echo.cpp socket_server.cpp -o $@ -g -O2 -Wall

clean:
	rm -f socket_server_echo
	rm -f socket_client_echo

