# loop.py - bench/loop.masm in Python. The loop is inside a function, so its variables
# are fast locals (at module level they would be dictionary entries, about twice as slow).
# See docs/REGISTER_VS_STACK.md. Usage: python3 loop.py [n]
import sys
def loop(N):
    acc = 0
    i = 0
    while i < N:
        acc = (acc + (i * i) % 7) ^ 0x55
        i += 1
    return acc % 100000000
if __name__ == "__main__":
    print(loop(int(sys.argv[1]) if len(sys.argv) > 1 else 30000000))
