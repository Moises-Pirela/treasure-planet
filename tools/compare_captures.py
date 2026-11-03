"""Compares two captures written by `treasure_planet --capture`, exiting 1 if they differ.

    python tools/compare_captures.py a.bmp b.bmp
"""
import struct
import sys


def read_bmp(path):
    with open(path, "rb") as f:
        data = f.read()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    return abs(width), abs(height), data[offset:]


def main():
    width_a, height_a, a = read_bmp(sys.argv[1])
    width_b, height_b, b = read_bmp(sys.argv[2])
    if (width_a, height_a) != (width_b, height_b):
        print(f"sizes differ: {width_a}x{height_a} and {width_b}x{height_b}")
        return 1
    different = 0
    worst = 0
    distinct = set()
    for i in range(0, len(a), 4):
        pixel_a, pixel_b = a[i:i + 3], b[i:i + 3]
        distinct.add(pixel_a)
        if pixel_a != pixel_b:
            different += 1
            worst = max(worst, *(abs(x - y) for x, y in zip(pixel_a, pixel_b)))
    total = width_a * height_a
    print(f"{width_a}x{height_a}: {different} of {total} pixels differ, worst channel delta {worst}, "
          f"{len(distinct)} distinct colours")
    return 1 if different else 0


if __name__ == "__main__":
    sys.exit(main())
