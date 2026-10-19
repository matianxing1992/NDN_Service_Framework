#!/usr/bin/env python3
"""The installed Boost pair required by each supported x86-64 host profile."""

import sys


_PROFILES = {
    "20.04": ("/usr/include", "/usr/lib/x86_64-linux-gnu", 107100, "1.71.0"),
    "26.04": ("/usr/include", "/usr/lib/x86_64-linux-gnu", 109000, "1.90.0"),
}


def select_profile(version):
    try:
        return _PROFILES[version]
    except KeyError as error:
        raise ValueError("Unsupported Ubuntu host profile: " + version) from error


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: host_profile.py UBUNTU_VERSION_ID")
    try:
        print("\t".join(map(str, select_profile(sys.argv[1]))))
    except ValueError as error:
        raise SystemExit(str(error)) from error
