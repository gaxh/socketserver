using GENERIC = System.Collections.Generic;
using SOCKETS = System.Net.Sockets;
using Log = global::Net.Log;

namespace Net
{

    class Const
    {
        public const ulong InvalidSocketId = 0;
    }

    enum SocketStatusEnum
    {
        Listening, // 服务器监听连接
        Accepted, // 服务器获得的客户端连接
        Connecting, // 客户端正在连接
        Connected, // 客户端已连接
        UDP_BIND, // udp已绑定
        UDP_CONNECT, // udp已连接
    }

    class WriteBuffer
    {
        public byte[] Array;
        public int Offset; // 从第Offset个字节开始发送
        public int Size; // 一共发送Size个字节
        public System.Net.EndPoint UdpAddr; // udp目标地址

        public int DataSize() {
            return Offset + Size;
        }
    }

    public class SocketCloseReason
    {
        public const int MANUALLY_CLOSED = -1;
        public const int CONNECT_FAILED = -2;
        public const int CLOSED_BY_PEER = -3;
        public const int READ_FAILED = -4;
        public const int WRITE_FAILED = -5;
        public const int SERVER_DESTROY = -6;
        public const int POLL_ERROR = -7;
    }

    public enum SocketEventEnum {
        OPEN,
        CLOSE,
        READ,
        WRITE_REPORT_THRESHOLD,
    }

    public class SocketEvent
    {

        public void Reset(SocketServer server, SocketEventEnum event_) {
            Server = server;
            Event = event_;
            Id = Const.InvalidSocketId;
            Address = null;
            Array = null;
            Offset = 0;
            Size = 0;
            CloseReason = 0;
            ListenerId = Const.InvalidSocketId;
            FromAddress = null;
            AboveThreshold = false;
        }

        public SocketServer Server;
        public SocketEventEnum Event;
        public ulong Id;
        public ulong ListenerId;
        public System.Net.IPEndPoint Address;

        // 读数据专用
        public byte[] Array;
        public int Offset;
        public int Size;

        // 连接关闭
        public int CloseReason;

        // udp
        public System.Net.IPEndPoint FromAddress;

        // 写缓冲是否超过设定的阈值
        public bool AboveThreshold;
    }

    public delegate void SocketEventCallback(SocketEvent evt);

    class Socket
    {
        public ulong Id;
        public SOCKETS.Socket Fd;
        public System.Net.IPEndPoint Address;
        public SocketStatusEnum Status;
        public GENERIC.Queue<WriteBuffer> WriteList = new GENERIC.Queue<WriteBuffer>();
        public int WriteDataSize = 0;

        public SocketEventCallback Cb;
        public bool Closed = false;
        public ulong ListenerId = Const.InvalidSocketId;
        public bool UDP = false;

        public int WriteReportThreshold = int.MaxValue;
        public int LastReportedWriteThreshold = 0;
        public bool LastReportedWriteAboveThreshold = false;

        public override string ToString() {
            return string.Format("(socket id={0},addr={1},stat={2})", Id, Address, Status);
        }

        void DefaultCallback(SocketEvent e) {
            Log.Logger.InfoFormat("default event: event={0}, id={1}, address={2}, size={3}, close_reason={4}",
                e.Event, e.Id, e.Address, e.Size, e.CloseReason);
        }

        public void Flush() {
            if (UDP) {
                while (WriteList.Count > 0) {
                    WriteBuffer wb = WriteList.Peek();

                    SocketHelper.SendTo(Fd, wb.Array, wb.Offset, wb.Size, wb.UdpAddr);

                    WriteDataSize -= wb.DataSize();
                    WriteList.Dequeue();
                }
            } else {
                while (WriteList.Count > 0) {
                    WriteBuffer wb = WriteList.Peek();

                    while (wb.Size > 0) {
                        int ret = SocketHelper.Send(Fd, wb.Array, wb.Offset, wb.Size);

                        if (ret <= 0) {
                            return;
                        }

                        wb.Offset += ret;
                        wb.Size -= ret;
                    }

                    WriteDataSize -= wb.DataSize();
                    WriteList.Dequeue();
                }
            }
        }

        public void Close() {
            SocketHelper.Close(Fd);
            Closed = true;
        }

        public void Callback(SocketEvent e) {
            if (Cb != null) {
                Cb(e);
            } else {
                DefaultCallback(e);
            }
        }
    }

    class IdGenerator
    {
        public ulong Next() {
            return ((ulong)System.DateTime.Now.Subtract(ts_zero).TotalSeconds << 32) + (ulong)(id++);
        }

        uint id;
        readonly System.DateTime ts_zero = new System.DateTime(1970, 1, 1);
    }

    public class SocketServer
    {

        public static SocketServer Instance = new SocketServer();

        public void Init() {
            for (int i = 0; i < poll_results.Length; ++i) {
                poll_results[i] = new SocketPoller<Socket>.Result();
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

                socket.Callback(MakeCloseEvent(socket, SocketCloseReason.SERVER_DESTROY));
                socket.Flush();
                ForceClose(socket);
            }

            socket_poll.Destroy();
            sockets.Clear();
        }

        public int Update() {
            int n = socket_poll.Poll(poll_results, poll_results.Length);

            for (int i = 0; i < n; ++i) {
                SocketPoller<Socket>.Result result = poll_results[i];

                Socket socket = result.Userdata;

                if (!socket.Closed && result.Read) {
                    ProcessRead(socket);
                }

                if (!socket.Closed && result.Write) {
                    ProcessWrite(socket);
                }

                if (!socket.Closed && result.Error) {
                    ProcessError(socket);
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

                        ulong id = id_generator.Next();

                        if (sockets.ContainsKey(id)) {
                            Log.Logger.ErrorFormat("accept failed, alloced id has been used: {0}", fd.RemoteEndPoint);
                            SocketHelper.Close(fd);
                            return;
                        }

                        Socket client = new Socket() {
                            Id = id,
                            Fd = fd,
                            Address = (System.Net.IPEndPoint)fd.RemoteEndPoint,
                            Status = SocketStatusEnum.Accepted,
                            Cb = socket.Cb,
                            ListenerId = socket.Id,
                        };

                        sockets.Add(id, client);
                        socket_poll.Add(fd, client, true, false);

                        Log.Logger.InfoFormat("server accept new socket: {0}", client);
                        // 调用 开始连接的回调
                        client.Cb(MakeOpenEvent(client));

                        return;
                    }
                case SocketStatusEnum.Connecting: {
                        Log.Logger.ErrorFormat("connecting status can not read, socket={0}", socket);
                        socket_poll.Modify(socket.Fd, socket, false, true);
                        return;
                    }
                case SocketStatusEnum.Connected:
                case SocketStatusEnum.Accepted: {
                        // 正常读数据
                        int ret = SocketHelper.Receive(socket.Fd, readbuffer, 0, readbuffer.Length);

                        if (ret < 0) {
                            // 连接挂了，直接强制关闭连接
                            Log.Logger.InfoFormat("socket is closed (read failed): {0}", socket);
                            socket.Callback(MakeCloseEvent(socket, SocketCloseReason.READ_FAILED));
                            ForceClose(socket);
                            return;
                        } else if (ret == 0) {
                            // 客户端关闭了连接，先发送剩余数据，再关闭连接
                            Log.Logger.InfoFormat("socket is closed by remote: {0}", socket);
                            socket.Callback(MakeCloseEvent(socket, SocketCloseReason.CLOSED_BY_PEER));
                            socket.Flush();
                            ForceClose(socket);
                            return;
                        } else {
                            socket.Callback(MakeReadEvent(socket, readbuffer, 0, ret));
                            return;
                        }
                    }
                case SocketStatusEnum.UDP_BIND:
                case SocketStatusEnum.UDP_CONNECT: {
                        System.Net.EndPoint from_addr = socket.Address.AddressFamily == SOCKETS.AddressFamily.InterNetworkV6 ?
                            new System.Net.IPEndPoint(System.Net.IPAddress.IPv6Any, 0) : new System.Net.IPEndPoint(System.Net.IPAddress.Any, 0);
                        int ret = SocketHelper.ReceiveFrom(socket.Fd, readbuffer, 0, readbuffer.Length, ref from_addr);

                        if (ret > 0 && from_addr is System.Net.IPEndPoint) {
                            socket.Callback(MakeUdpReadEvent(socket, readbuffer, 0, ret, (System.Net.IPEndPoint)from_addr));
                        }
                        return;
                    }
                default:
                    Log.Logger.ErrorFormat("process read failed, socket status {0} is invalid: {1}", socket.Status, socket);
                    return;

            }
        }

        SocketEvent MakeCloseEvent(Socket socket, int close_reason) {
            socket_event.Reset(this, SocketEventEnum.CLOSE);
            socket_event.Id = socket.Id;
            socket_event.Address = socket.Address;
            socket_event.ListenerId = socket.ListenerId;
            socket_event.CloseReason = close_reason;
            return socket_event;
        }

        SocketEvent MakeOpenEvent(Socket socket) {
            socket_event.Reset(this, SocketEventEnum.OPEN);
            socket_event.Id = socket.Id;
            socket_event.Address = socket.Address;
            socket_event.ListenerId = socket.ListenerId;
            return socket_event;
        }

        SocketEvent MakeReadEvent(Socket socket, byte[] array, int offset, int size) {
            socket_event.Reset(this, SocketEventEnum.READ);
            socket_event.Id = socket.Id;
            socket_event.Address = socket.Address;
            socket_event.ListenerId = socket.ListenerId;
            socket_event.Array = array;
            socket_event.Offset = offset;
            socket_event.Size = size;
            return socket_event;
        }

        SocketEvent MakeUdpReadEvent(Socket socket, byte[] array, int offset, int size, System.Net.IPEndPoint from_addr) {
            socket_event.Reset(this, SocketEventEnum.READ);
            socket_event.Id = socket.Id;
            socket_event.Address = socket.Address;
            socket_event.ListenerId = socket.ListenerId;
            socket_event.Array = array;
            socket_event.Offset = offset;
            socket_event.Size = size;
            socket_event.FromAddress = from_addr;
            return socket_event;
        }

        SocketEvent MakeWriteReportThresholdEvent(Socket socket, bool above_threshold) {
            socket_event.Reset(this, SocketEventEnum.WRITE_REPORT_THRESHOLD);
            socket_event.Id = socket.Id;
            socket_event.Address = socket.Address;
            socket_event.ListenerId = socket.ListenerId;
            socket_event.AboveThreshold = above_threshold;
            return socket_event;
        }

        void ForceClose(Socket socket) {
            socket_poll.Remove(socket.Fd);
            socket.Close();
            sockets.Remove(socket.Id);
        }

        void ProcessWrite(Socket socket) {
            switch (socket.Status) {
                case SocketStatusEnum.Listening: {
                        // 这是异常情况
                        Log.Logger.ErrorFormat("process write failed, socket is listening: {0}", socket);
                        socket_poll.Modify(socket.Fd, socket, true, false);
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
                                    socket.Callback(MakeCloseEvent(socket, SocketCloseReason.WRITE_FAILED));
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

                            socket.WriteDataSize -= wb.DataSize();
                            socket.WriteList.Dequeue();
                        }
                        socket_poll.Modify(socket.Fd, socket, true, false);

                        TriggerWriteReport(socket);
                        return;
                    }
                case SocketStatusEnum.Connecting: {
                        // 可能连接上了，也可能没连上
                        if (!socket.Fd.Connected) {
                            // 没连上，直接关闭连接
                            Log.Logger.InfoFormat("socket connecting failed: {0}", socket);
                            socket.Callback(MakeCloseEvent(socket, SocketCloseReason.CONNECT_FAILED));
                            ForceClose(socket);
                            return;
                        }
                        //连上了，标记为已连接

                        Log.Logger.InfoFormat("socket has connected: {0}", socket);
                        socket_poll.Modify(socket.Fd, socket, true, socket.WriteList.Count > 0);
                        socket.Status = SocketStatusEnum.Connected;
                        socket.Callback(MakeOpenEvent(socket));

                        return;
                    }
                case SocketStatusEnum.UDP_BIND:
                case SocketStatusEnum.UDP_CONNECT: {
                        while (socket.WriteList.Count > 0) {
                            WriteBuffer wb = socket.WriteList.Peek();

                            int ret = SocketHelper.SendTo(socket.Fd, wb.Array, wb.Offset, wb.Size, wb.UdpAddr);

                            if (ret == 0) {
                                return;
                            }

                            socket.WriteDataSize -= wb.DataSize();
                            socket.WriteList.Dequeue();
                        }

                        socket_poll.Modify(socket.Fd, socket, true, false);

                        TriggerWriteReport(socket);
                        return;
                    }
                default:
                    Log.Logger.ErrorFormat("process write failed, socket status {0} is invalid: {1}", socket.Status, socket);
                    return;
            }
        }

        void ProcessError(Socket socket) {
            switch (socket.Status) {
                case SocketStatusEnum.Connecting: {
                        if (!socket.Fd.Connected) {
                            // 没连上，直接关闭连接
                            Log.Logger.InfoFormat("socket connecting failed: {0}", socket);
                            socket.Callback(MakeCloseEvent(socket, SocketCloseReason.CONNECT_FAILED));
                            ForceClose(socket);
                            return;
                        }
                        //连上了，标记为已连接

                        Log.Logger.InfoFormat("socket has connected: {0}", socket);
                        socket_poll.Modify(socket.Fd, socket, true, socket.WriteList.Count > 0);
                        socket.Status = SocketStatusEnum.Connected;
                        socket.Callback(MakeOpenEvent(socket));
                    }
                    return;
                case SocketStatusEnum.Accepted:
                case SocketStatusEnum.Connected: {
                        Log.Logger.InfoFormat("socket is closed (poll error): {0}", socket);
                        socket.Callback(MakeCloseEvent(socket, SocketCloseReason.POLL_ERROR));
                        ForceClose(socket);
                        return;
                    }
                default:
                    Log.Logger.ErrorFormat("socket get error event: {0}", socket);
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

            byte[] copied = new byte[size];
            System.Buffer.BlockCopy(array, offset, copied, 0, size);
            WriteBuffer wb = new WriteBuffer() {
                Array = copied,
                Offset = 0,
                Size = size,
            };

            SendBuffer(socket, wb);
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

            WriteBuffer wb = new WriteBuffer() {
                Array = array,
                Offset = offset,
                Size = size,
            };

            SendBuffer(socket, wb);
        }

        void SendBuffer(Socket socket, WriteBuffer wb) {
            if (wb.Size <= 0) {
                return;
            }

            if (socket.Status == SocketStatusEnum.Listening) {
                Log.Logger.ErrorFormat("send failed: {0} is listening", socket);
                return;
            }

            if (socket.WriteList.Count <= 0) {
                if (socket.UDP) {
                    int ret = SocketHelper.SendTo(socket.Fd, wb.Array, wb.Offset, wb.Size, wb.UdpAddr);

                    if (ret > 0) {
                        return;
                    }
                } else {
                    int ret = SocketHelper.Send(socket.Fd, wb.Array, wb.Offset, wb.Size);

                    if (ret > 0) {
                        wb.Offset += ret;
                        wb.Size -= ret;

                        if (wb.Size <= 0) {
                            return;
                        }
                    }
                }
            }

            socket.WriteDataSize += wb.DataSize();
            socket.WriteList.Enqueue(wb);
            socket_poll.Modify(socket.Fd, socket, true, true);

            TriggerWriteReport(socket);
        }

        public void Close(ulong id, bool call_cb = true, int close_reason = SocketCloseReason.MANUALLY_CLOSED) {
            Socket socket = GetSocketObject(id);

            if (socket == null) {
                Log.Logger.ErrorFormat("failed to close socket, get socket failed: {0}", id);
                return;
            }

            Log.Logger.InfoFormat("socket is closed manully: {0}", socket);

            if (call_cb) {
                socket.Callback(MakeCloseEvent(socket, close_reason));
            }
            socket.Flush();
            ForceClose(socket);
        }

        // as client
        public ulong Connect(System.Net.IPEndPoint remote, SocketEventCallback cb) {
            SOCKETS.Socket fd = new SOCKETS.Socket(remote.AddressFamily, SOCKETS.SocketType.Stream, SOCKETS.ProtocolType.Tcp);
            fd.Blocking = false;

            if (!SocketHelper.Connect(fd, remote)) {
                SocketHelper.Close(fd);
                return Const.InvalidSocketId;
            }

            ulong id = id_generator.Next();

            if (sockets.ContainsKey(id)) {
                Log.Logger.ErrorFormat("connect failed, alloced id has been used: {0}", fd.RemoteEndPoint);
                SocketHelper.Close(fd);
                return Const.InvalidSocketId;
            }

            Socket socket = new Socket() {
                Id = id,
                Fd = fd,
                Address = remote,
                Status = SocketStatusEnum.Connecting,
                Cb = cb,
            };

            sockets.Add(id, socket);
            socket_poll.Add(fd, socket, false, true);

            Log.Logger.InfoFormat("socket is connecting: {0}", socket);

            return id;
        }

        // as server
        public ulong Listen(System.Net.IPEndPoint local, SocketEventCallback cb) {
            SOCKETS.Socket fd = new SOCKETS.Socket(local.AddressFamily, SOCKETS.SocketType.Stream, SOCKETS.ProtocolType.Tcp);
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

            ulong id = id_generator.Next();

            if (sockets.ContainsKey(id)) {
                Log.Logger.ErrorFormat("listen failed, alloced is has been used: {0}", fd.RemoteEndPoint);
                SocketHelper.Close(fd);
                return Const.InvalidSocketId;
            }

            Socket socket = new Socket() {
                Id = id,
                Fd = fd,
                Address = local,
                Status = SocketStatusEnum.Listening,
                Cb = cb,
            };

            sockets.Add(id, socket);
            socket_poll.Add(fd, socket, true, false);

            Log.Logger.InfoFormat("socket is listening: {0}", socket);

            return id;
        }

        public ulong UdpBind(System.Net.IPEndPoint local, SocketEventCallback cb) {
            SOCKETS.Socket fd = new SOCKETS.Socket(local.AddressFamily, SOCKETS.SocketType.Dgram, SOCKETS.ProtocolType.Udp);
            fd.SetSocketOption(SOCKETS.SocketOptionLevel.Socket, SOCKETS.SocketOptionName.ReuseAddress, true);
            fd.Blocking = false;
            SocketHelper.UdpSetIgnoreError10054(fd);

            if (!SocketHelper.Bind(fd, local)) {
                SocketHelper.Close(fd);
                return Const.InvalidSocketId;
            }

            ulong id = id_generator.Next();

            if (sockets.ContainsKey(id)) {
                Log.Logger.ErrorFormat("udp bind failed, alloced is has been used: {0}", fd.RemoteEndPoint);
                SocketHelper.Close(fd);
                return Const.InvalidSocketId;
            }

            Socket socket = new Socket() {
                Id = id,
                Fd = fd,
                Address = local,
                Status = SocketStatusEnum.UDP_BIND,
                Cb = cb,
                UDP = true,
            };

            sockets.Add(id, socket);
            socket_poll.Add(fd, socket, true, false);

            return id;
        }

        public ulong UdpConnect(System.Net.IPEndPoint remote, SocketEventCallback cb) {
            SOCKETS.Socket fd = new SOCKETS.Socket(remote.AddressFamily, SOCKETS.SocketType.Dgram, SOCKETS.ProtocolType.Udp);
            fd.Blocking = false;
            SocketHelper.UdpSetIgnoreError10054(fd);

            if (!SocketHelper.Connect(fd, remote)) {
                SocketHelper.Close(fd);
                return Const.InvalidSocketId;
            }

            ulong id = id_generator.Next();

            if (sockets.ContainsKey(id)) {
                Log.Logger.ErrorFormat("udp connect failed, alloced id has been used: {0}", fd.RemoteEndPoint);
                SocketHelper.Close(fd);
                return Const.InvalidSocketId;
            }

            Socket socket = new Socket() {
                Id = id,
                Fd = fd,
                Address = remote,
                Status = SocketStatusEnum.UDP_CONNECT,
                Cb = cb,
                UDP = true,
            };

            sockets.Add(id, socket);
            socket_poll.Add(fd, socket, true, false);

            return id;
        }

        public void SendUdpCopy(ulong id, System.Net.IPEndPoint to_addr, byte[] array, int offset, int size) {
            if (size <= 0) {
                return;
            }

            Socket socket = GetSocketObject(id);

            if (socket == null) {
                Log.Logger.ErrorFormat("SendUdpCopy failed: id {0} not exist", id);
                return;
            }

            byte[] copied = new byte[size];
            System.Buffer.BlockCopy(array, offset, copied, 0, size);
            WriteBuffer wb = new WriteBuffer() {
                Array = copied,
                Offset = 0,
                Size = size,
                UdpAddr = to_addr,
            };

            SendBuffer(socket, wb);
        }

        public void SendUdpNocopy(ulong id, System.Net.IPEndPoint to_addr, byte[] array, int offset, int size) {
            if (size <= 0) {
                return;
            }

            Socket socket = GetSocketObject(id);

            if (socket == null) {
                Log.Logger.ErrorFormat("SendUdpNocopy failed: id {0} not exist", id);
                return;
            }

            WriteBuffer wb = new WriteBuffer() {
                Array = array,
                Offset = offset,
                Size = size,
                UdpAddr = to_addr,
            };

            SendBuffer(socket, wb);
        }

        public void SetWriteReportThreshold(ulong id, int threshold) {
            Socket socket = GetSocketObject(id);

            if (socket == null) {
                Log.Logger.ErrorFormat("SetWriteReportThreshold failed: id {0} not exist", id);
                return;
            }

            socket.WriteReportThreshold = threshold;
        }

        void TriggerWriteReport(Socket socket) {
            if (socket.LastReportedWriteThreshold == socket.WriteReportThreshold) {
                if (!socket.LastReportedWriteAboveThreshold && socket.WriteDataSize < socket.WriteReportThreshold) {
                    return;
                } else if (socket.LastReportedWriteAboveThreshold && socket.WriteDataSize >= socket.WriteReportThreshold) {
                    return;
                }
            }

            socket.LastReportedWriteThreshold = socket.WriteReportThreshold;

            if (socket.WriteDataSize < socket.WriteReportThreshold) {
                socket.LastReportedWriteAboveThreshold = false;

                socket.Callback(MakeWriteReportThresholdEvent(socket, false));
            } else {
                socket.LastReportedWriteAboveThreshold = true;

                socket.Callback(MakeWriteReportThresholdEvent(socket, true));
            }
        }

        Socket GetSocketObject(ulong id) {
            return sockets.TryGetValue(id, out Socket obj) ? obj : null;
        }

        GENERIC.Dictionary<ulong, Socket> sockets = new GENERIC.Dictionary<ulong, Socket>();

        SocketPoller<Socket>.Result[] poll_results = new SocketPoller<Socket>.Result[128];
        byte[] readbuffer = new byte[1024 * 1024 * 10];
        SocketPoller<Socket> socket_poll = new SocketPoller<Socket>();
        SocketEvent socket_event = new SocketEvent();
        IdGenerator id_generator = new IdGenerator();
    }

    public class SocketServerLoop {
        public void Init(SocketServer server) {
            this.server = server;
            exit = false;
            s_exit = false;

            System.Console.CancelKeyPress += Console_CancelKeyPress;
        }
        private void Console_CancelKeyPress(object sender, System.ConsoleCancelEventArgs e) {
            exit = true;
        }

        public void Destroy() {
            this.server = null;
            exit = true;
            s_exit = true;

            System.Console.CancelKeyPress -= Console_CancelKeyPress;
        }

        public void Loop() {
            while (!exit && !s_exit) {
                int n = server.Update();

                if (m_loop_cb != null) {
                    n += m_loop_cb();
                }

                if (n <= 0) {
                    System.Threading.Thread.Sleep(10);
                }
            }
        }

        public void Exit() {
            exit = true;
        }

        public void LoopCall(LoopCallback cb) {
            m_loop_cb = cb;
        }

        SocketServer server;
        bool exit = true;
        static bool s_exit = true;

        public delegate int LoopCallback();
        LoopCallback m_loop_cb;
    }
}





