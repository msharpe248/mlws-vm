// Fib.java - bench/fib.masm for the JVM (a stack machine). `long` matches MLWSVM's i64.
// See docs/REGISTER_VS_STACK.md. Usage: javac Fib.java && java [-Xint] Fib [n]
public class Fib {
    static long fib(long n) {
        if (n < 2) return n;
        return fib(n - 1) + fib(n - 2);
    }
    public static void main(String[] a) {
        System.out.println(fib(a.length > 0 ? Long.parseLong(a[0]) : 32));
    }
}
