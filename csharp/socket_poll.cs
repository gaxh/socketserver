using GENERIC = System.Collections.Generic;
using SOCKETS = System.Net.Sockets;

namespace Net {

    public class SocketPoll {

        public class Result {
            public ulong Id;
            public bool Read;
            public bool Write;
        }

        class PollEvent {
            public SOCKETS.Socket Fd;
            public ulong Id;
            public bool Read;
            public bool Write;
        }

        public void Init() {

        }

        public void Destroy() {
            events.Clear();
        }

        public void Add(SOCKETS.Socket fd, ulong id, bool read, bool write) {
            Assert.RuntimeAssert(fd != null, "fd is in events");

            events.Add(fd, new PollEvent() {
                Fd = fd,
                Id = id,
                Read = read,
                Write = write,
            });
        }

        public void Remove(SOCKETS.Socket fd) {
            Assert.RuntimeAssert(fd != null, "fd is not in events");

            events.Remove(fd);
        }

        public void Modify(SOCKETS.Socket fd, ulong id, bool read, bool write) {
            PollEvent evt = GetPollEvent(fd);

            Assert.RuntimeAssert(evt != null, "fd is not in events");

            evt.Id = id;
            evt.Read = read;
            evt.Write = write;
        }

        public int Poll(Result[] results, int max) {
            GENERIC.List<SOCKETS.Socket> readlist = new GENERIC.List<SOCKETS.Socket>();
            GENERIC.List<SOCKETS.Socket> writelist = new GENERIC.List<SOCKETS.Socket>();

            foreach (var iter in events) {
                PollEvent evt = iter.Value;

                if (evt.Read) {
                    readlist.Add(evt.Fd);
                }

                if (evt.Write) {
                    writelist.Add(evt.Fd);
                }
            }

            if (readlist.Count <= 0 && writelist.Count <= 0) {
                return 0;
            }

            if (!SocketHelper.Select(readlist, writelist)) {
                return -1;
            }

            int n = 0;

            for (int i = 0; i < readlist.Count && n < max; ++i) {
                PollEvent evt = GetPollEvent(readlist[i]);

                if (evt != null) {
                    Result result = results[n++];

                    result.Id = evt.Id;
                    result.Read = true;
                    result.Write = false;
                }
            }

            for (int i = 0; i < writelist.Count && n < max; ++i) {
                PollEvent evt = GetPollEvent(writelist[i]);

                if (evt != null) {
                    Result result = results[n++];

                    result.Id = evt.Id;
                    result.Read = false;
                    result.Write = true;
                }
            }

            return n;
        }

        PollEvent GetPollEvent(SOCKETS.Socket fd) {
            return events.TryGetValue(fd, out PollEvent e) ? e : null;
        }

        GENERIC.Dictionary<SOCKETS.Socket, PollEvent> events = new GENERIC.Dictionary<SOCKETS.Socket, PollEvent>();
    }

}

