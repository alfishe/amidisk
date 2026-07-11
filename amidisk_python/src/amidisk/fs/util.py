"""Amiga filesystem helpers: timestamps, protection bits, name hashing."""

from datetime import datetime, timedelta

# AmigaDOS epoch
# Cache epoch to avoid constructing it repeatedly
EPOCH = datetime(1978, 1, 1)
TICKS_PER_SEC = 50

# Precomputed 256-byte translation tables for fast toupper folding at C-speed
UPPER_TABLE_STD = bytes(
    (c - 0x20) if (0x61 <= c <= 0x7A) else c
    for c in range(256)
)

UPPER_TABLE_INTL = bytes(
    (c - 0x20) if (0x61 <= c <= 0x7A) or (0xE0 <= c <= 0xFE and c != 0xF7) else c
    for c in range(256)
)


def amiga_to_datetime(days, mins, ticks):
    try:
        return EPOCH + timedelta(days=days, minutes=mins, seconds=ticks / TICKS_PER_SEC)
    except OverflowError:
        return EPOCH


def datetime_to_amiga(dt):
    delta = dt - EPOCH
    if delta.days < 0:
        return 0, 0, 0
    days = delta.days
    mins = delta.seconds // 60
    ticks = (delta.seconds % 60) * TICKS_PER_SEC + delta.microseconds * TICKS_PER_SEC // 1_000_000
    return days, mins, ticks


# protection bits: HSPARWED -- the low four (RWED) are *inverted* on disk
PROT_FLAGS = "hsparwed"


def protect_to_str(protect):
    out = []
    for i, ch in enumerate(PROT_FLAGS):
        bit = 1 << (7 - i)
        is_set = bool(protect & bit)
        if 7 - i <= 3:  # r/w/e/d inverted: bit set means NOT allowed
            out.append("-" if is_set else ch)
        else:
            out.append(ch if is_set else "-")
    return "".join(out)


def str_to_protect(s):
    """Parse 'hsparwed' style string (dashes for unset)."""
    protect = 0
    s = s.lower().ljust(8, "-")
    for i, ch in enumerate(PROT_FLAGS):
        bit = 1 << (7 - i)
        present = s[i] == ch
        if 7 - i <= 3:
            if not present:
                protect |= bit
        else:
            if present:
                protect |= bit
    return protect


def upper_char(c, intl):
    """AmigaDOS toupper used for name hashing/comparison."""
    if 0x61 <= c <= 0x7A:
        return c - 0x20
    if intl and 0xE0 <= c <= 0xFE and c != 0xF7:
        return c - 0x20
    return c


def hash_name(name, ht_size, intl):
    """AmigaDOS directory hash for a name (bytes)."""
    h = len(name)
    table = UPPER_TABLE_INTL if intl else UPPER_TABLE_STD
    for c in name.translate(table):
        h = (h * 13 + c) & 0x7FF
    return h % ht_size


def names_equal(a, b, intl):
    table = UPPER_TABLE_INTL if intl else UPPER_TABLE_STD
    return a.translate(table) == b.translate(table)
