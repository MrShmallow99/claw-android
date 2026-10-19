"""Reads games/<name>/game.ini with the same rules as runtime/gamecfg.c:
"[section]", "key = value", '#'/';' comments (at line start or after a space),
repeated keys kept in order."""
import re


def load(path):
    """Returns {section: [(key, value), ...]} (sections and keys lower-case)."""
    out = {}
    section = ''
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = re.sub(r'(^|\s)[#;].*', '', line.rstrip('\n')).strip()
            if not line:
                continue
            if line.startswith('['):
                section = line[1:line.index(']')].strip().lower()
                out.setdefault(section, [])
                continue
            if '=' not in line:
                continue
            k, v = line.split('=', 1)
            out.setdefault(section, []).append((k.strip().lower(), v.strip()))
    return out


def get(cfg, section, key, default=None):
    vals = [v for k, v in cfg.get(section, []) if k == key]
    return vals[-1] if vals else default


def get_all(cfg, section, key):
    return [v for k, v in cfg.get(section, []) if k == key]


if __name__ == '__main__':
    # tools/gameini.py game.ini section key  -> prints the value (for shell scripts)
    import sys
    print(get(load(sys.argv[1]), sys.argv[2].lower(), sys.argv[3].lower(), ''))
