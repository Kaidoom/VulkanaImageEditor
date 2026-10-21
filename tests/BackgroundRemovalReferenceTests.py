"""Opt-in Pillow reference comparison for the C++ model adapters.

Uses synthetic ONNX graphs, never research weights. Pillow is a test dependency
only. Run with the path to imageeditor_background_removal_probe.
"""

import json
import math
from pathlib import Path
import random
import subprocess
import sys
import tempfile

from PIL import Image


def pixels(image):
    return list(image.get_flattened_data()) if hasattr(image, "get_flattened_data") else list(image.getdata())


def varint(value):
    result = bytearray()
    while value > 127:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def number(field, value):
    return varint(field << 3) + varint(value)


def block(field, value):
    if isinstance(value, str):
        value = value.encode()
    return varint((field << 3) | 2) + varint(len(value)) + value


def tensor(name, shape):
    dimensions = b"".join(block(1, number(1, value)) for value in shape)
    return block(1, name) + block(2, block(1, number(1, 1) + block(2, dimensions)))


def model(width, height):
    axes = block(1, "axes") + number(8, 1) + number(20, 7)
    keepdims = block(1, "keepdims") + number(3, 1) + number(20, 2)
    node = (block(1, "image") + block(2, "coverage") + block(4, "ReduceMean")
            + block(5, axes) + block(5, keepdims))
    graph = (block(1, node) + block(2, "reference-comparison")
             + block(11, tensor("image", [1, 3, height, width]))
             + block(12, tensor("coverage", [1, 1, height, width])))
    return number(1, 9) + block(6, " " * 256) + block(7, graph) + block(8, number(2, 13))


def linear_resize(data, before, after):
    """Tensor bilinear resize, align_corners=False, independent of Pillow."""
    width, height = before
    out_width, out_height = after
    output = []
    for y in range(out_height):
        fy = max(0, (y + 0.5) * height / out_height - 0.5)
        y0, y1 = min(int(fy), height - 1), min(int(fy) + 1, height - 1)
        wy = fy - y0
        for x in range(out_width):
            fx = max(0, (x + 0.5) * width / out_width - 0.5)
            x0, x1 = min(int(fx), width - 1), min(int(fx) + 1, width - 1)
            wx = fx - x0
            top = data[y0 * width + x0] * (1 - wx) + data[y0 * width + x1] * wx
            bottom = data[y1 * width + x0] * (1 - wx) + data[y1 * width + x1] * wx
            output.append(top * (1 - wy) + bottom * wy)
    return output


def minmax(values):
    low, high = min(values), max(values)
    return [(value - low) / (high - low) for value in values]


def reference(source, size, profile):
    is_u2 = profile == "u2netp"
    scaled = source.resize(size, Image.Resampling.LANCZOS if is_u2
                           else Image.Resampling.BILINEAR)
    data = pixels(scaled)
    divisor = max(max(pixel) for pixel in data) if is_u2 else 255
    mean, deviation = (.485, .456, .406), (.229, .224, .225)
    values = [sum((pixel[c] / divisor - mean[c]) / deviation[c]
                  for c in range(3)) / 3 for pixel in data]
    if is_u2:
        small = Image.new("L", size)
        small.putdata([int(value * 255) for value in minmax(values)])
        return small.resize(source.size, Image.Resampling.LANCZOS)
    values = [1 / (1 + math.exp(-value)) for value in values]
    values = linear_resize(values, size, source.size)
    result = Image.new("L", source.size)
    result.putdata([int(max(0, min(1, value)) * 255) for value in values])
    return result


def main():
    probe = str(Path(sys.argv[1]).resolve())
    random_source = random.Random(3456)
    paths = {"u2netp": "u2netp.onnx",
             "birefnet-lite-int8": "birefnet-lite-512/onnx/model_int8.onnx"}
    with tempfile.TemporaryDirectory(prefix="vulkana-background-reference-") as temporary:
        root = Path(temporary)
        size = (41, 37)
        for path in paths.values():
            target = root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(model(*size))
        for width, height in [(19, 31), (91, 43), (43, 91)]:
            source = Image.new("RGB", (width, height))
            source.putdata([tuple(random_source.randrange(256) for _ in range(3))
                            for _ in range(width * height)])
            source.save(root / "source.png")
            for profile in paths:
                subprocess.run([probe, str(root), str(root / "source.png"), profile,
                                str(root / "out")], check=True, capture_output=True, text=True)
                actual = Image.open(root / "out" / f"{profile}.png")
                expected = reference(source, size, profile)
                differences = [abs(a - e) for a, e in zip(pixels(actual), pixels(expected))]
                maximum, average = max(differences), sum(differences) / len(differences)
                report = {"model": profile, "source": [width, height], "tensor": list(size),
                          "maxByteDifference": maximum, "meanByteDifference": average}
                print(json.dumps(report))
                # Pillow's fixed-point filter rounding and ONNX float32 differ
                # from reference float64 at at most a few quantization levels.
                assert maximum <= 3 and average <= 0.35, report


if __name__ == "__main__":
    main()
