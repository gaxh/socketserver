namespace Net
{
    public class Log
    {

        public class LoggerType
        {
            static string SafeStringFormat(string format, params object[] args) {
                try {
                    return string.Format(format, args);
                } catch (System.FormatException) {
                    return "(format_failed)" + format;
                }
            }

            public delegate void LogCallback(string format, params object[] args);

            public LogCallback InfoFormat = (format, args) => {
                System.Console.WriteLine("[INFO] " + SafeStringFormat(format, args));
            };

            public LogCallback ErrorFormat = (format, args) => {
                System.Console.WriteLine("[ERROR] " + SafeStringFormat(format, args));
            };
        }

        public static LoggerType Logger = new LoggerType();

    }
}

