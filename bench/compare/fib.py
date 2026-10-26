# fib.py - bench/fib.masm in Python (CPython is a stack machine). See docs/REGISTER_VS_STACK.md.
# Usage: python3 fib.py [n]
import sys
def fib(n):
    if n < 2: return n
    return fib(n - 1) + fib(n - 2)
if __name__ == "__main__":
    print(fib(int(sys.argv[1]) if len(sys.argv) > 1 else 32))
