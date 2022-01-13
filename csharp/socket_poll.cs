using GENERIC = System.Collections.Generic;
using SOCKETS = System.Net.Sockets;

namespace Net
{
    public class SocketPoller<USERDATA_TYPE>
    {

        public class Result
        {
            public USERDATA_TYPE Userdata;
            public bool Read;
            public bool Write;
            public bool Error;
        }

        class PollEvent
        {
            public SOCKETS.Socket Fd;
            public USERDATA_TYPE Userdata;
            public bool Read;
            public bool Write;
        }

        public void Init() {

        }

        public void Destroy() {
            events.Clear();
        }

        public void Add(SOCKETS.Socket fd, USERDATA_TYPE userdata, bool read, bool write) {
            Assert.RuntimeAssert(fd != null, "fd is in events");

            events.Add(fd, new PollEvent() {
                Fd = fd,
                Userdata = userdata,
                Read = read,
                Write = write,
            });
        }

        public void Remove(SOCKETS.Socket fd) {
            Assert.RuntimeAssert(fd != null, "fd is not in events");

            events.Remove(fd);
        }

        public void Modify(SOCKETS.Socket fd, USERDATA_TYPE userdata, bool read, bool write) {
            PollEvent evt = GetPollEvent(fd);

            Assert.RuntimeAssert(evt != null, "fd is not in events");

            evt.Userdata = userdata;
            evt.Read = read;
            evt.Write = write;
        }

        public int Poll(Result[] results, int max) {


            foreach (var iter in events) {
                PollEvent evt = iter.Value;

                if (evt.Read) {
                    readlist.Add(evt.Fd);
                }


                if (evt.Write) {
                    writelist.Add(evt.Fd);
                }

                errorlist.Add(evt.Fd);
            }

            if (readlist.Count <= 0 && writelist.Count <= 0 && errorlist.Count <= 0) {
                return 0;
            }

            if (!SocketHelper.Select(readlist, writelist, errorlist)) {
                return -1;
            }

            int n = 0;

            for (int i = 0; i < readlist.Count && n < max; ++i) {
                PollEvent evt = GetPollEvent(readlist[i]);

                if (evt != null) {
                    Result result = results[n++];

                    result.Userdata = evt.Userdata;
                    result.Read = true;
                    result.Write = false;
                    result.Error = false;
                }
            }

            for (int i = 0; i < writelist.Count && n < max; ++i) {
                PollEvent evt = GetPollEvent(writelist[i]);

                if (evt != null) {
                    Result result = results[n++];

                    result.Userdata = evt.Userdata;
                    result.Read = false;
                    result.Write = true;
                    result.Error = false;
                }
            }

            for (int i = 0; i < errorlist.Count && n < max; ++i) {
                PollEvent evt = GetPollEvent(errorlist[i]);

                if (evt != null) {
                    Result result = results[n++];

                    result.Userdata = evt.Userdata;
                    result.Read = false;
                    result.Write = false;
                    result.Error = true;
                }
            }

            readlist.Clear();
            writelist.Clear();
            errorlist.Clear();

            return n;
        }

        PollEvent GetPollEvent(SOCKETS.Socket fd) {
            return events.TryGetValue(fd, out PollEvent e) ? e : null;
        }

        GENERIC.Dictionary<SOCKETS.Socket, PollEvent> events = new GENERIC.Dictionary<SOCKETS.Socket, PollEvent>();
        GENERIC.List<SOCKETS.Socket> readlist = new GENERIC.List<SOCKETS.Socket>();
        GENERIC.List<SOCKETS.Socket> writelist = new GENERIC.List<SOCKETS.Socket>();
        GENERIC.List<SOCKETS.Socket> errorlist = new GENERIC.List<SOCKETS.Socket>();
    }
}


