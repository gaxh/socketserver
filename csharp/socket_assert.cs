
namespace Net
{

    public class Assert
    {
        public class AssertException : System.Exception
        {
            public AssertException(string msg) : base(msg) { }
        }

        public static void RuntimeAssert(bool value, string message) {
            if (!value) {
                throw new AssertException(message);
            }
        }

        static string SafeStringFormat(string format, params object[] args) {
            try {
                return string.Format(format, args);
            } catch (System.FormatException) {
                return "(format_failed)" + format;
            }
        }

        public static void RuntimeAssertFormat(bool value, string format, params object[] args) {
            if (!value) {
                throw new AssertException(SafeStringFormat(format, args));
            }
        }
    }


}


