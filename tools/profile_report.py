"""Usage: profile_report.py <binary> <samples-file> -- top functions by sample count."""
import sys, subprocess, bisect, collections
binary, samples = sys.argv[1], sys.argv[2]
syms = []
for line in subprocess.run(['nm', '-C', binary], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) >= 3 and p[1] in 'tTwW':
        syms.append((int(p[0], 16), p[2]))
syms.sort()
addrs = [s[0] for s in syms]
c = collections.Counter()
lines = open(samples).read().split()[2:]
for l in lines:
    a = int(l, 16)
    i = bisect.bisect_right(addrs, a) - 1
    c[syms[i][1] if i >= 0 else '?'] += 1
total = sum(c.values())
for name, n in c.most_common(30):
    print('%6.2f%%  %s' % (100.0 * n / total, name))
print('samples', total)
