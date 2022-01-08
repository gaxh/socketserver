all : socket_server_echo

socket_server_echo : socket_server_echo.cpp socket_server.h
	g++ socket_server_echo.cpp -o $@ -g -O2 -Wall

clean:
	rm -f socket_server_echo

