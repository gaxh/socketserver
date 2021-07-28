namespace Net {
    public class Log {

        public class LoggerType {
            public void InfoFormat(string format, params object[] args) {
                System.Console.WriteLine("[INFO] " + SafeStringFormat(format, args));
            }

            public void ErrorFormat(string format, params object[] args) {
                System.Console.WriteLine("[ERROR] " + SafeStringFormat(format, args));
            }

            static string SafeStringFormat(string format, params object[] args) {
                try {
                    return string.Format(format, args);
                } catch (System.FormatException) {
                    return "(format_failed)" + format;
                }
            }
        }

        public static LoggerType Logger = new LoggerType();

    }
}
