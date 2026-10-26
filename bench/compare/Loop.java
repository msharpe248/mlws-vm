// Loop.java - bench/loop.masm for the JVM. See docs/REGISTER_VS_STACK.md.
// Usage: javac Loop.java && java [-Xint] Loop [n]
public class Loop {
    static long loop(long n) {
        long acc = 0, i = 0;
        while (i < n) {
            acc = (acc + (i * i) % 7) ^ 0x55;
            i = i + 1;
        }
        return acc % 100000000;
    }
    public static void main(String[] a) {
        System.out.println(loop(a.length > 0 ? Long.parseLong(a[0]) : 30000000));
    }
}
