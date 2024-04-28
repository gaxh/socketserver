
HEADER_FILES := $(wildcard *.h)

%.o : %.cpp ${HEADER_FILES}
	${CXX} -c -o $@ $< -g -O2 -Wall

all : socket_server.a

socket_server.a : socket_server.o socket_server_loop.o
	ar rcs $@ $^

%.exe : %.o socket_server.a ${HEADER_FILES}
	${CXX} -o $@ $< socket_server.a

all : socket_client_echo.exe

all : socket_server_echo.exe

all : socket_client_stress.exe

all : socket_server_udp_echo.exe

all : socket_client_udp_echo.exe

all : socket_client_udp_stress.exe

all : socket_server_unix_echo.exe

all : socket_client_unix_echo.exe

clean:
	rm -f *.o
	rm -f *.a
	rm -f *.exe
