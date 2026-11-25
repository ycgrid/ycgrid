# SPDX-License-Identifier: Apache-2.0
"""Build src/engine/geo_data.inc (the region shapes of map charts) from Natural Earth (public domain).

    python scripts/build_map_data.py ne_50m_admin_0_countries.geojson ne_10m_admin_1_states_provinces.geojson

Get the two files from https://github.com/nvkelso/natural-earth-vector (geojson folder). Shapes are simplified
(Douglas-Peucker), coordinates are stored as 1/100 degree in int16 with delta coding, and each region carries the
names a sheet may use for it: English and local names, Chinese (simplified and traditional), Japanese, Korean,
German, French, Spanish, and ISO codes.

Format (little-endian): "YCGEO1", u32 region count, then per region:
  u8 level (0 country, 1 first-level subdivision), str country ISO A3, str ISO code,
  u8 name count, names (str each), i16 label lon, i16 label lat, u16 part count,
  per part: u16 point count, the first point (i16 lon, i16 lat), then (i8 dx, i8 dy) or the escape -128 + the point itself (i16 lon, i16 lat)
strings: u8 length + UTF-8 bytes.
"""
import json
import struct
import sys


def simplify(points, tol):
    """Douglas-Peucker on a ring (list of (lon, lat))."""
    if len(points) < 4:
        return points
    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        a, b = stack.pop()
        ax, ay = points[a]
        bx, by = points[b]
        dx, dy = bx - ax, by - ay
        norm = (dx * dx + dy * dy) ** 0.5
        best, idx = -1.0, -1
        for i in range(a + 1, b):
            px, py = points[i]
            d = abs(dy * (px - ax) - dx * (py - ay)) / norm if norm else ((px - ax) ** 2 + (py - ay) ** 2) ** 0.5
            if d > best:
                best, idx = d, i
        if best > tol and idx > 0:
            keep[idx] = True
            stack.append((a, idx))
            stack.append((idx, b))
    return [p for p, k in zip(points, keep) if k]


def rings(geom):
    if geom is None:
        return []
    if geom["type"] == "Polygon":
        polys = [geom["coordinates"]]
    elif geom["type"] == "MultiPolygon":
        polys = geom["coordinates"]
    else:
        return []
    out = []
    for poly in polys:
        if poly:
            out.append(poly[0])  # the outer ring (holes are left out: lakes don't change the look)
    return out


def ring_area(r):
    a = 0.0
    for i in range(len(r) - 1):
        a += r[i][0] * r[i + 1][1] - r[i + 1][0] * r[i][1]
    return abs(a) / 2


def put_str(b, s):
    data = (s or "").encode("utf-8")[:255]
    b += struct.pack("<B", len(data)) + data


def q(v):
    return max(-32767, min(32767, int(round(v * 100))))


def region(b, level, country, iso, names, label, geom, tol, min_area):
    parts = []
    for r in rings(geom):
        if ring_area(r) < min_area:
            continue  # tiny islands
        s = simplify([(float(x), float(y)) for x, y in r], tol)
        pts = []
        for x, y in s:
            p = (q(x), q(y))
            if not pts or pts[-1] != p:
                pts.append(p)
        if len(pts) >= 4:
            parts.append(pts)
    if not parts:
        return False
    b += struct.pack("<B", level)
    put_str(b, country)
    put_str(b, iso)
    uniq = []
    for n in names:
        n = (n or "").strip()
        if n and n not in uniq and n != "-99":
            uniq.append(n)
    b += struct.pack("<B", min(len(uniq), 255))
    for n in uniq[:255]:
        put_str(b, n)
    b += struct.pack("<hh", q(label[0]), q(label[1]))
    b += struct.pack("<H", len(parts))
    for pts in parts:
        b += struct.pack("<H", len(pts))
        b += struct.pack("<hh", *pts[0])
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            dx, dy = x1 - x0, y1 - y0
            if -127 <= dx <= 127 and -127 <= dy <= 127:
                b += struct.pack("<bb", dx, dy)
            else:
                b += struct.pack("<bhh", -128, x1, y1)  # a long jump: the point itself
    return True


def label_of(props, geom, kx, ky):
    if props.get(kx) not in (None, "") and props.get(ky) not in (None, ""):
        return float(props[kx]), float(props[ky])
    rs = rings(geom)
    if not rs:
        return 0.0, 0.0
    big = max(rs, key=ring_area)
    return sum(p[0] for p in big) / len(big), sum(p[1] for p in big) / len(big)


def main():
    countries = json.load(open(sys.argv[1], encoding="utf-8"))
    admin1 = json.load(open(sys.argv[2], encoding="utf-8"))
    b = bytearray()
    count = 0
    for f in countries["features"]:
        p = f["properties"]
        names = [p.get("NAME"), p.get("NAME_LONG"), p.get("ADMIN"), p.get("NAME_EN"), p.get("FORMAL_EN"), p.get("NAME_ALT"), p.get("NAME_ZH"),
                 p.get("NAME_ZHT"), p.get("NAME_JA"), p.get("NAME_KO"), p.get("NAME_DE"), p.get("NAME_FR"), p.get("NAME_ES"), p.get("ISO_A2_EH"),
                 p.get("ISO_A3_EH"), p.get("ADM0_A3")]
        a3 = p.get("ADM0_A3") or ""
        if region(b, 0, a3, p.get("ISO_A2_EH") or "", names, label_of(p, f["geometry"], "LABEL_X", "LABEL_Y"), f["geometry"], 0.08, 0.02):
            count += 1
    for f in admin1["features"]:
        p = f["properties"]
        iso = p.get("iso_3166_2") or ""
        names = [p.get("name"), p.get("name_en"), p.get("name_local"), p.get("woe_name"), p.get("gn_name"), p.get("name_zh"), p.get("name_zht"),
                 p.get("name_ja"), p.get("name_ko"), p.get("name_de"), p.get("name_fr"), p.get("name_es"), iso, iso.split("-")[-1] if "-" in iso else ""]
        alt = p.get("name_alt") or ""
        names += [a for a in alt.split("|") if a]
        if region(b, 1, p.get("adm0_a3") or "", iso, names, label_of(p, f["geometry"], "longitude", "latitude"), f["geometry"], 0.03, 0.0005):
            count += 1
    data = b"YCGEO1" + struct.pack("<I", count) + bytes(b)
    with open("src/engine/geo_data.inc", "w", encoding="utf-8", newline="\n") as out:
        out.write("// generated by scripts/build_map_data.py from Natural Earth (public domain); don't edit\n")
        out.write("// %d regions, %d bytes\n" % (count, len(data)))
        for i in range(0, len(data), 40):
            out.write(",".join(str(x) for x in data[i:i + 40]) + ",\n")
    print(count, "regions,", len(data), "bytes")


if __name__ == "__main__":
    main()
