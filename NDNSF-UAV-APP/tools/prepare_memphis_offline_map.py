#!/usr/bin/env python3
"""Prepare a small offline OpenStreetMap tile cache for the UAV demo.

The cache is intentionally tiny: by default it downloads the 3x3 tile window
around the University of Memphis for zoom levels 14, 15, and 16. The
ground-station GUI reads these tiles first, then falls back to /tmp cache, then
network.
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path
import sys
import urllib.request


DEFAULT_LAT = 35.1186
DEFAULT_LON = -89.9375
DEFAULT_ZOOM = 15
DEFAULT_ZOOMS = "14,15,16"


def deg2tile(lat: float, lon: float, zoom: int) -> tuple[int, int]:
    lat_rad = math.radians(lat)
    n = 2.0 ** zoom
    x = int(math.floor((lon + 180.0) / 360.0 * n))
    y = int(math.floor((1.0 - math.asinh(math.tan(lat_rad)) / math.pi) / 2.0 * n))
    return x, y


def download_tile(zoom: int, x: int, y: int, output_root: Path, force: bool) -> bool:
    path = output_root / str(zoom) / str(x) / f"{y}.png"
    if path.exists() and path.stat().st_size > 0 and not force:
        print(f"[OK] cached {zoom}/{x}/{y}")
        return True

    path.parent.mkdir(parents=True, exist_ok=True)
    url = f"https://tile.openstreetmap.org/{zoom}/{x}/{y}.png"
    print(f"[GET] {url}")
    try:
        request = urllib.request.Request(url, headers={"User-Agent": "ndnsf-uav-app"})
        with urllib.request.urlopen(request, timeout=10) as response:
            path.write_bytes(response.read())
    except Exception as exc:
        print(f"[FAIL] {url}: {exc}", file=sys.stderr)
        return False
    return True


def main() -> int:
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description="Prepare offline Memphis OSM tiles")
    parser.add_argument("--lat", type=float, default=DEFAULT_LAT)
    parser.add_argument("--lon", type=float, default=DEFAULT_LON)
    parser.add_argument("--zoom", type=int, default=DEFAULT_ZOOM,
                        help="Single zoom level to fetch. Kept for compatibility.")
    parser.add_argument("--zooms", default=DEFAULT_ZOOMS,
                        help="Comma-separated zoom levels to fetch, default 14,15,16.")
    parser.add_argument("--radius", type=int, default=1,
                        help="Tile radius around the center tile; 1 means 3x3 tiles.")
    parser.add_argument("--output-root", default=str(repo / "NDNSF-UAV-APP/maps/osm"))
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    output_root = Path(args.output_root).resolve()
    ok = True
    zooms = [int(item) for item in args.zooms.split(",") if item.strip()]
    if not zooms:
        zooms = [args.zoom]
    for zoom in zooms:
        center_x, center_y = deg2tile(args.lat, args.lon, zoom)
        for x in range(center_x - args.radius, center_x + args.radius + 1):
          for y in range(center_y - args.radius, center_y + args.radius + 1):
            ok = download_tile(zoom, x, y, output_root, args.force) and ok

    if ok:
        print(f"[OK] offline map cache ready: {output_root}")
        return 0
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
