#!/usr/bin/env python3
"""What linking one Petari object pulls in, as an archive link would: the objects it needs and the symbols nothing defines.

Reads the symbols.txt that tools/petari-link-needs.sh leaves in its output directory.
usage: petari-closure.py OUT_DIR Game_Player_MarioActor.o [more root objects]
"""
import collections, subprocess, sys
from pathlib import Path

# Toolchain and C/C++ runtime symbols the 3DS link provides.
RUNTIME = ('_Z', '__aeabi', '__cxa', '__gxx', '_Unwind', '__stack_chk', '__sync', '__dso_handle')
LIBC = {'memcpy', 'memset', 'memmove', 'memcmp', 'strcmp', 'strncmp', 'strlen', 'strcpy', 'strncpy', 'strcat', 'strchr',
        'strrchr', 'strstr', 'sprintf', 'snprintf', 'vsnprintf', 'printf', 'abort', 'sqrt', 'sqrtf', 'sin', 'sinf', 'cos',
        'cosf', 'tan', 'tanf', 'atan', 'atanf', 'atan2', 'atan2f', 'acos', 'acosf', 'asin', 'asinf', 'fmod', 'fmodf', 'pow',
        'powf', 'floor', 'floorf', 'ceil', 'ceilf', 'fabs', 'fabsf', 'strtol', 'strtoul', 'atoi', 'tolower', 'toupper',
        'wcslen', 'vsprintf', 'sscanf', 'exp', 'expf', 'log', 'logf', 'log10', 'modf', 'frexp', 'ldexp', 'strncat'}
STD = ('_ZSt', '_ZNSt', '_ZNKSt', '_Znwj', '_Znaj', '_ZdlPv', '_ZdaPv', '_ZTVN10__cxxabiv', '_ZnwjPv')


def provided(symbol):
    return symbol in LIBC or symbol.startswith(STD) or (symbol.startswith(RUNTIME) and not symbol.startswith('_Z'))


def main():
    out, roots = Path(sys.argv[1]), sys.argv[2:]
    defines, needs, owner, libraries = collections.defaultdict(set), collections.defaultdict(set), {}, set()
    for line in (out / 'symbols.txt').read_text().splitlines():
        parts = line.rsplit(' ', 2)
        if len(parts) != 3: continue
        where, kind, symbol = parts
        where = where.rsplit(':', 1)[0].strip()
        library = where.startswith('/build/')
        if kind == 'U':
            if not library: needs[where].add(symbol)
        elif kind in 'TWVDBRCSu':  # global definitions only; lowercase ones are local to their object
            if library: libraries.add(symbol)
            else:
                defines[where].add(symbol)
                owner.setdefault(symbol, where)
    closure, queue, missing = set(roots), list(roots), collections.defaultdict(set)
    while queue:
        current = queue.pop()
        for symbol in needs[current]:
            if symbol in libraries or provided(symbol): continue
            source = owner.get(symbol)
            if source is None: missing[symbol].add(current)
            elif source not in closure:
                closure.add(source)
                queue.append(source)
    names = subprocess.run(['c++filt'], input='\n'.join(missing), capture_output=True, text=True).stdout.splitlines()
    readable = dict(zip(missing, names))
    directories = collections.Counter('/'.join(o.split('_')[:2]) for o in closure)
    print(f'{len(closure)} objects in the closure of {" ".join(roots)}; {len(missing)} symbols nothing defines')
    for directory, count in directories.most_common(): print(f'  {count:4} {directory}')
    (out / 'closure-objects.txt').write_text('\n'.join(sorted(closure)) + '\n')
    (out / 'closure-missing.txt').write_text(''.join(f'{readable[s]}\t{" ".join(sorted(missing[s]))}\n'
                                                     for s in sorted(missing, key=lambda s: readable[s])))


if __name__ == '__main__':
    main()
