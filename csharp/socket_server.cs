using GENERIC = System.Collections.Generic;
using SOCKETS = System.Net.Sockets;
using Log = Net.Log;

namespace Net {

    class Const {
        public const ulong InvalidSocketId = 0;

        public static bool IsValidSocketId(ulong id) {
            return id != InvalidSocketId;
        }
    }

    enum SocketStatusEnum {
        Listening, // 服务器监听连接
        Accepted, // 服务器获得的客户端连接
        Connecting, // 客户端正在连接
        Connected, // 客户端已连接
    }

    class WriteBuffer {
        public byte[] Array;
        public int Offset; // 从第Offset个字节开始发送
        public int Size; // 一共发送Size个字节
    }

    public class SocketCloseReason {
        public const int MANUALLY_CLOSED = -1;
        public const int CONNECT_FAILED = -2;
        public const int CLOSED_BY_PEER = -3;
        public const int READ_FAILED = -4;
        public const int WRITE_FAILED = -5;
        public const int SERVER_DESTROY = -6;
    }

    public class SocketEvent {

        public void Clear() {
            Id = 0;
            Address = null;
            Array = null;
            Offset = 0;
            Size = 0;
            CloseReason = 0;
            return;
        }

        public SocketEvent Reset(ulong id, System.Net.IPEndPoint address) {
            Id = id;
            Address = Address;
            Array = null;
            Offset = 0;
            Size = 0;
            CloseReason = 0;
            return this;
        }

        public SocketEvent Reset(ulong id, System.Net.IPEndPoint address, byte[] array, int offset, int size) {
            Id = id;
            Address = address;
            Array = array;
            Offset = offset;
            Size = size;
            CloseReason = 0;
            return this;
        }

        public SocketEvent Reset(ulong id, System.Net.IPEndPoint address, int close_reason) {
            Id = id;
            Address = address;
            Array = null;
            Offset = 0;
            Size = 0;
            CloseReason = close_reason;
            return this;
        }

        public ulong Id { get; private set; }
        public System.Net.IPEndPoint Address { get; private set; }

        // 读数据专用
        public byte[] Array { get; set; }
        public int Offset { get; set; }
        public int Size { get; set; }
        public int CloseReason { get; set; }
    }

    public delegate void SocketEventCallback(SocketEvent evt);

    class Socket {
        public ulong Id;
        public SOCKETS.Socket Fd;
        public System.Net.IPEndPoint Address;
        public SocketStatusEnum Status;
        public GENERIC.Queue<WriteBuffer> WriteList = new GENERIC.Queue<WriteBuffer>();

        public SocketEventCallback OnOpen = DefaultOnOpen; // 客户端连接成功，或者服务器获得新连接
        public SocketEventCallback OnClosed = DefaultOnClosed; // 客户端连接断开，或者服务器连接断开
        public SocketEventCallback OnRead = DefaultOnRead; // 可读数据

        public int GetWriteBufferSize() {
            int ret = 0;
            foreach (WriteBuffer wb in WriteList) {
                ret += System.Math.Max(0, wb.Size);
            }
            return ret;
        }

        public override string ToString() {
            return string.Format("(socket id={0},addr={1},stat={2})", Id, Address, Status);
        }

        public static void DefaultOnOpen(SocketEvent evt) {
            Log.Logger.InfoFormat("socket is open (default cb): {0} {1}", evt.Id, evt.Address);
        }

        public static void DefaultOnClosed(SocketEvent evt) {
            Log.Logger.InfoFormat("socket is closed (default cb): {0} {1}", evt.Id, evt.Address);
        }

        public static void DefaultOnRead(SocketEvent evt) {
            Log.Logger.InfoFormat("socket read data (default cb): {0} {1}, {2}", evt.Id, evt.Address,
                System.BitConverter.ToString(evt.Array, evt.Offset, evt.Size));
        }
    }

    class IdGenerator {
        public static IdGenerator Instance = new IdGenerator();

        public ulong Next() {
            return ((ulong)System.DateTime.Now.Subtract(ts_zero).TotalSeconds << 32) + (ulong)(id++);
        }

        uint id;
        readonly System.DateTime ts_zero = new System.DateTime(1970, 1, 1);
    }

    public class SocketServer {

        public static SocketServer Instance = new SocketServer();

        public void Init() {
            for (int i = 0; i < poll_results.Length; ++i) {
                poll_results[i] = new SocketPoll.Result();
            }
            socket_poll.Init();
        }

        public void Destroy() {
            GENERIC.Dictionary<ulong, Socket> sockets_copied = new GENERIC.Dictionary<ulong, Socket>();
            foreach (var iter in sockets) {
                sockets_copied.Add(iter.Key, iter.Value);
            }

            foreach (var iter in sockets_copied) {
                Socket socket = iter.Value;

                Log.Logger.InfoFormat("socket is closed (server destroy): {0}", socket);
                socket.OnClosed(socket_event.Reset(socket.Id, socket.Address, SocketCloseReason.SERVER_DESTROY));
                socket_poll.Remove(socket.Fd);
                SocketHelper.Close(socket.Fd);
            }

            socket_poll.Destroy();
            sockets.Clear();
        }

        public int Update() {
            int n = socket_poll.Poll(poll_results, poll_results.Length);

            for(int i = 0; i < n; ++i) {
                SocketPoll.Result result = poll_results[i];

                Socket socket = GetSocketObject(result.Id);

                if (socket == null) {
                    continue;
                }

                if (result.Read) {
                    ProcessRead(socket);
                }

                if (result.Write) {
                    ProcessWrite(socket);
                }
            }

            return n;
        }

        void ProcessRead(Socket socket) {
            switch (socket.Status) {
                case SocketStatusEnum.Listening: {
                        // 正常获得新连接
                        SOCKETS.Socket fd = SocketHelper.Accept(socket.Fd);
                        if (fd == null) {
                            return;
                        }
                        fd.Blocking = false;

                        ulong id = IdGenerator.Instance.Next();

                        Socket client = new Socket() {
                            Id = id,
                            Fd = fd,
                            Address = (System.Net.IPEndPoint)fd.RemoteEndPoint,
                            Status = SocketStatusEnum.Accepted,
                            OnOpen = socket.OnOpen,
                            OnClosed = socket.OnClosed,
                            OnRead = socket.OnRead,
                        };

                        sockets.Add(id, client);
                        socket_poll.Add(fd, id, true, false);

                        Log.Logger.InfoFormat("server accept new socket: {0}", client);
                        // 调用 开始连接的回调
                        client.OnOpen(socket_event.Reset(id, client.Address));

                        return;
                    }
                case SocketStatusEnum.Connecting: {
                        // 正在连接的
                        // 有可能连接失败，需要测试一下连接
                        if (!socket.Fd.Connected) {
                            // 没连上，直接关闭连接
                            Log.Logger.InfoFormat("socket connecting failed: {0}", socket);
                            socket.OnClosed(socket_event.Reset(socket.Id, socket.Address, SocketCloseReason.CONNECT_FAILED));
                            ForceClose(socket);
                            return;
                        }
                        Log.Logger.ErrorFormat("process read failed, socket is connecting: {0}", socket);
                        return;
                    }
                case SocketStatusEnum.Connected:
                case SocketStatusEnum.Accepted: {
                        // 正常读数据
                        int ret = SocketHelper.Receive(socket.Fd, readbuffer, 0, readbuffer.Length);

                        if (ret < 0) {
                            // 连接挂了，直接强制关闭连接
                            Log.Logger.InfoFormat("socket is closed (read failed): {0}", socket);
                            socket.OnClosed(socket_event.Reset(socket.Id, socket.Address, SocketCloseReason.READ_FAILED));
                            ForceClose(socket);
                            return;
                        } else if (ret == 0) {
                            // 客户端关闭了连接，先发送剩余数据，再关闭连接
                            Log.Logger.InfoFormat("socket is closed by remote: {0}", socket);
                            socket.OnClosed(socket_event.Reset(socket.Id, socket.Address, SocketCloseReason.CLOSED_BY_PEER));
                            FlushAll(socket);
                            ForceClose(socket);
                            return;
                        } else {
                            socket.OnRead(socket_event.Reset(socket.Id, socket.Address, readbuffer, 0, ret));
                            return;
                        }
                    }
                default:
                    Log.Logger.ErrorFormat("process read failed, socket status {0} is invalid: {1}", socket.Status, socket);
                    return;

            }
        }

        void ForceClose(Socket socket) {
            socket_poll.Remove(socket.Fd);
            SocketHelper.Close(socket.Fd);
            sockets.Remove(socket.Id);
        }

        void FlushAll(Socket socket) {
            while (socket.WriteList.Count > 0) {
                WriteBuffer wb = socket.WriteList.Peek();

                while (wb.Size > 0) {
                    int ret = SocketHelper.Send(socket.Fd, wb.Array, wb.Offset, wb.Size);

                    if (ret <= 0) {
                        return;
                    }

                    wb.Offset += ret;
                    wb.Size -= ret;
                }

                socket.WriteList.Dequeue();
            }
        }

        void ProcessWrite(Socket socket) {
            switch (socket.Status) {
                case SocketStatusEnum.Listening: {
                        // 这是异常情况
                        Log.Logger.ErrorFormat("process write failed, socket is listening: {0}", socket);
                        socket_poll.Modify(socket.Fd, socket.Id, true, false);
                        return;
                    }
                case SocketStatusEnum.Accepted:
                case SocketStatusEnum.Connected: {
                        // 正常发数据
                        while (socket.WriteList.Count > 0) {
                            WriteBuffer wb = socket.WriteList.Peek();

                            while (wb.Size > 0) {
                                int ret = SocketHelper.Send(socket.Fd, wb.Array, wb.Offset, wb.Size);

                                if (ret < 0) {
                                    // 连接挂了，关闭连接
                                    Log.Logger.InfoFormat("socket is closed (write failed): {0}", socket);
                                    socket.OnClosed(socket_event.Reset(socket.Id, socket.Address, SocketCloseReason.WRITE_FAILED));
                                    ForceClose(socket);
                                    return;
                                } else if (ret == 0) {
                                    // 正常发送失败
                                    return;
                                } else {
                                    wb.Offset += ret;
                                    wb.Size -= ret;
                                }
                            }

                            socket.WriteList.Dequeue();
                        }
                        socket_poll.Modify(socket.Fd, socket.Id, true, false);
                        return;
                    }
                case SocketStatusEnum.Connecting: {
                        // 可能连接上了，也可能没连上
                        if (!socket.Fd.Connected) {
                            // 没连上，直接关闭连接
                            Log.Logger.InfoFormat("socket connecting failed: {0}", socket);
                            socket.OnClosed(socket_event.Reset(socket.Id, socket.Address, SocketCloseReason.CONNECT_FAILED));
                            ForceClose(socket);
                            return;
                        }
                        //连上了，标记为已连接

                        Log.Logger.InfoFormat("socket has connected: {0}", socket);
                        socket.Status = SocketStatusEnum.Connected;
                        socket.OnOpen(socket_event.Reset(socket.Id, socket.Address));

                        if (socket.WriteList.Count <= 0) {
                            socket_poll.Modify(socket.Fd, socket.Id, true, false);
                        }
                        return;
                    }
                default:
                    Log.Logger.ErrorFormat("process write failed, socket status {0} is invalid: {1}", socket.Status, socket);
                    return;
            }
        }

        public void SendCopy(ulong id, byte[] array, int offset, int size) {
            if (size <= 0) {
                return;
            }

            Socket socket = GetSocketObject(id);

            if (socket == null) {
                Log.Logger.ErrorFormat("Send failed: id {0} not exist", id);
                return;
            }

            switch (socket.Status) {
                case SocketStatusEnum.Listening: {
                        Log.Logger.ErrorFormat("send failed: {0} is listening", socket);
                        return;
                    }
                default: {
                        break;
                    }
            }

            //先尝试发送一次
            if (socket.WriteList.Count <= 0) {
                int ret = SocketHelper.Send(socket.Fd, array, offset, size);
                if (ret > 0) {
                    offset += ret;
                    size -= offset;

                    if (size <= 0) {
                        return;
                    }
                }
            }

            byte[] copied = new byte[size];
            System.Buffer.BlockCopy(array, offset, copied, 0, size);

            socket.WriteList.Enqueue(new WriteBuffer() {
                Array = copied,
                Offset = 0,
                Size = size,
            });

            socket_poll.Modify(socket.Fd, socket.Id, true, true);
        }

        public void SendNocopy(ulong id, byte[] array, int offset, int size) {
            if (size <= 0) {
                return;
            }

            Socket socket = GetSocketObject(id);

            if (socket == null) {
                Log.Logger.ErrorFormat("Send failed: id {0} not exist", id);
                return;
            }

            switch (socket.Status) {
                case SocketStatusEnum.Listening: {
                        Log.Logger.ErrorFormat("send failed: {0} is listening", socket);
                        return;
                    }
                default: {
                        break;
                    }
            }

            //先尝试发送一次
            if (socket.WriteList.Count <= 0) {
                int ret = SocketHelper.Send(socket.Fd, array, offset, size);
                if (ret > 0) {
                    offset += ret;
                    size -= offset;

                    if (size <= 0) {
                        return;
                    }
                }
            }

            socket.WriteList.Enqueue(new WriteBuffer() {
                Array = array,
                Offset = offset,
                Size = size,
            });

            socket_poll.Modify(socket.Fd, socket.Id, true, true);
        }

        public void Close(ulong id) {
            Socket socket = GetSocketObject(id);

            if (socket == null) {
                return;
            }

            Log.Logger.InfoFormat("socket is closed manully: {0}", socket);
            socket.OnClosed(socket_event.Reset(socket.Id, socket.Address, SocketCloseReason.MANUALLY_CLOSED));
            FlushAll(socket);
            ForceClose(socket);
        }

        // as client
        public ulong Connect(System.Net.IPEndPoint remote, SocketEventCallback on_open, SocketEventCallback on_close, SocketEventCallback on_read) {
            SOCKETS.Socket fd = new SOCKETS.Socket(SOCKETS.AddressFamily.InterNetwork, SOCKETS.SocketType.Stream, SOCKETS.ProtocolType.Tcp);
            fd.Blocking = false;

            if (!SocketHelper.Connect(fd, remote)) {
                SocketHelper.Close(fd);
                return Const.InvalidSocketId;
            }

            ulong id = IdGenerator.Instance.Next();

            Socket socket = new Socket() {
                Id = id,
                Fd = fd,
                Address = remote,
                Status = SocketStatusEnum.Connecting,
                OnOpen = on_open,
                OnClosed = on_close,
                OnRead = on_read,
            };

            sockets.Add(id, socket);
            socket_poll.Add(fd, id, true, true);

            Log.Logger.InfoFormat("socket is connecting: {0}", socket);

            return id;
        }

        // as server
        public ulong Listen(System.Net.IPEndPoint local, SocketEventCallback on_open, SocketEventCallback on_close, SocketEventCallback on_read) {
            SOCKETS.Socket fd = new SOCKETS.Socket(SOCKETS.AddressFamily.InterNetwork, SOCKETS.SocketType.Stream, SOCKETS.ProtocolType.Tcp);
            fd.SetSocketOption(SOCKETS.SocketOptionLevel.Socket, SOCKETS.SocketOptionName.ReuseAddress, true);
            fd.Blocking = false;

            if (!SocketHelper.Bind(fd, local)) {
                SocketHelper.Close(fd);
                return Const.InvalidSocketId;
            }

            if (!SocketHelper.Listen(fd, 20)) {
                SocketHelper.Close(fd);
                return Const.InvalidSocketId;
            }

            ulong id = IdGenerator.Instance.Next();

            Socket socket = new Socket() {
                Id = id,
                Fd = fd,
                Address = local,
                Status = SocketStatusEnum.Listening,
                OnOpen = on_open,
                OnClosed = on_close,
                OnRead = on_read,
            };

            sockets.Add(id, socket);
            socket_poll.Add(fd, id, true, false);

            Log.Logger.InfoFormat("socket is listening: {0}", socket);

            return id;
        }

        Socket GetSocketObject(ulong id) {
            return sockets.TryGetValue(id, out Socket obj) ? obj : null;
        }

        GENERIC.Dictionary<ulong, Socket> sockets = new GENERIC.Dictionary<ulong, Socket>();

        SocketPoll.Result[] poll_results = new SocketPoll.Result[128];
        byte[] readbuffer = new byte[1024 * 1024 * 10];
        SocketPoll socket_poll = new SocketPoll();
        SocketEvent socket_event = new SocketEvent();
    }

}


