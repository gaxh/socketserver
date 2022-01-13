using SOCKETS = System.Net.Sockets;

namespace Net
{
    public class SocketHelper
    {

        public static bool Bind(SOCKETS.Socket fd, System.Net.EndPoint address) {
            try {
                fd.Bind(address);
                return true;
            } catch (SOCKETS.SocketException e) {
                Log.Logger.ErrorFormat("call socket.bind failed ({0}): {1}\n{2}", e.ErrorCode, e.Message, e.StackTrace);
                return false;
            }
        }

        public static bool Listen(SOCKETS.Socket fd, int backlog) {
            try {
                fd.Listen(backlog);
                return true;
            } catch (SOCKETS.SocketException e) {
                Log.Logger.ErrorFormat("call socket.listen failed ({0}): {1}\n{2}", e.ErrorCode, e.Message, e.StackTrace);
                return false;
            }
        }

        public static bool Select(System.Collections.IList readlist, System.Collections.IList writelist, System.Collections.IList errorlist) {
            try {
                SOCKETS.Socket.Select(readlist, writelist, errorlist, 0);
                return true;
            } catch (SOCKETS.SocketException e) {
                Log.Logger.ErrorFormat("call socket.select failed ({0}): {1}\n{2}", e.ErrorCode, e.Message, e.StackTrace);
                return false;
            }
        }

        public static bool Connect(SOCKETS.Socket fd, System.Net.EndPoint address) {
            try {
                fd.Connect(address);
                return true;
            } catch (SOCKETS.SocketException e) {
                if (e.SocketErrorCode == SOCKETS.SocketError.WouldBlock || e.SocketErrorCode == SOCKETS.SocketError.InProgress) {
                    return true;
                }

                Log.Logger.ErrorFormat("call socket.connect failed ({0}): {1}\n{2}", e.ErrorCode, e.Message, e.StackTrace);
                return false;
            }
        }

        public static SOCKETS.Socket Accept(SOCKETS.Socket fd) {
            try {
                return fd.Accept();
            } catch (SOCKETS.SocketException e) {
                Log.Logger.ErrorFormat("call socket.accept failed ({0}): {1}\n{2}", e.ErrorCode, e.Message, e.StackTrace);
                return null;
            }
        }

        public static int Receive(SOCKETS.Socket fd, byte[] array, int offset, int size) {
            SOCKETS.SocketError error = 0;
            int ret = 0;
            try {
                ret = fd.Receive(array, offset, size, SOCKETS.SocketFlags.None, out error);
            } catch (SOCKETS.SocketException e) {
                if (e.SocketErrorCode == SOCKETS.SocketError.WouldBlock || e.SocketErrorCode == SOCKETS.SocketError.Interrupted) {
                    return 0;
                }
                Log.Logger.ErrorFormat("call socket.receive failed ({0}): {1}\n{2}", e.ErrorCode, e.Message, e.StackTrace);
                return -1;
            }

            if (error == SOCKETS.SocketError.WouldBlock || error == SOCKETS.SocketError.InProgress) {
                return 0;
            }

            return ret;
        }

        public static int Send(SOCKETS.Socket fd, byte[] array, int offset, int size) {
            SOCKETS.SocketError error = 0;
            int ret = 0;
            try {
                ret = fd.Send(array, offset, size, SOCKETS.SocketFlags.None, out error);
            } catch (SOCKETS.SocketException e) {
                if (e.SocketErrorCode == SOCKETS.SocketError.WouldBlock || e.SocketErrorCode == SOCKETS.SocketError.Interrupted) {
                    return 0;
                }
                Log.Logger.ErrorFormat("call socket.send failed ({0}): {1}\n{2}", e.ErrorCode, e.Message, e.StackTrace);
                return -1;
            }

            if (error == SOCKETS.SocketError.WouldBlock || error == SOCKETS.SocketError.InProgress) {
                return 0;
            }

            return ret;
        }

        public static void Shutdown(SOCKETS.Socket fd, SOCKETS.SocketShutdown how) {
            try {
                fd.Shutdown(how);
            } catch (SOCKETS.SocketException e) {
                Log.Logger.ErrorFormat("call socket.shutdown failed ({0}): {1}\n{2}", e.ErrorCode, e.Message, e.StackTrace);
            }
        }

        public static void Close(SOCKETS.Socket fd) {
            try {
                fd.Close();
            } catch (SOCKETS.SocketException e) {
                Log.Logger.ErrorFormat("call socket.shutdown failed ({0}): {1}\n{2}", e.ErrorCode, e.Message, e.StackTrace);
            }
        }

        public static int SendTo(SOCKETS.Socket fd, byte[] array, int offset, int size, System.Net.EndPoint addr) {
            try {
                return fd.SendTo(array, offset, size, SOCKETS.SocketFlags.None, addr != null ? addr : fd.RemoteEndPoint);
            } catch (SOCKETS.SocketException e) {
                if (e.SocketErrorCode == SOCKETS.SocketError.WouldBlock || e.SocketErrorCode == SOCKETS.SocketError.Interrupted) {
                    return 0;
                }
                Log.Logger.ErrorFormat("call socket.sendto failed ({0}): {1}\n{2}", e.ErrorCode, e.Message, e.StackTrace);
                return -1;
            }
        }

        public static int ReceiveFrom(SOCKETS.Socket fd, byte[] array, int offset, int size, ref System.Net.EndPoint addr) {
            try {
                return fd.ReceiveFrom(array, offset, size, SOCKETS.SocketFlags.None, ref addr);
            } catch (SOCKETS.SocketException e) {
                if (e.SocketErrorCode == SOCKETS.SocketError.WouldBlock || e.SocketErrorCode == SOCKETS.SocketError.Interrupted) {
                    return 0;
                }
                Log.Logger.ErrorFormat("call socket.receivefrom failed ({0}): {1}\n{2}", e.ErrorCode, e.Message, e.StackTrace);
                return -1;
            }
        }

        public static void UdpSetIgnoreError10054(SOCKETS.Socket fd) {
            try {
                const int SIP_UDP_CONNRESET = -1744830452;
                fd.IOControl(SIP_UDP_CONNRESET, new byte[] { 0, 0, 0, 0 }, null);
            } catch (System.Exception e) {
                Log.Logger.ErrorFormat("udp ignore error 10054 failed: {0}\n{1}", e.Message, e.StackTrace);
            }
        }
    }
}



