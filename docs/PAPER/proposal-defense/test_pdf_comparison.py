#!/usr/bin/env python3
"""Focused text-matching checks; not an academic or runtime-protocol test."""
from build_pdf_comparison import changed_indices


def words(text):
    return [{"text": item, "page": 0} for item in text.split()]


def main():
    for old, new, expected in [
        ("a b c", "a b c", (set(), set())),
        ("a old c", "a new c", ({1}, {1})),
        ("a c", "a b c", (set(), {1})),
        ("a b c", "a c", ({1}, set())),
    ]:
        assert changed_indices(words(old), words(new))[:2] == expected
    a = " ".join("x" + str(i) for i in range(20))
    b = " ".join("y" + str(i) for i in range(22))
    assert changed_indices(words(a + " " + b), words(b + " " + a))[:2] == (set(), set())
    removed, added, _ = changed_indices(words(a), words(a + " " + a))
    assert not removed and len(added) == 20
    print("PASS 6/6: identical, replacement, insertion, deletion, reordering, duplicated passage")


if __name__ == "__main__":
    main()
