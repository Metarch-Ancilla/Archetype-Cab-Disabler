"""Lossless PNG payload compression shared by the optional ICO/ICNS tools."""
import io
from PIL import Image
import zopfli.png


def compress_png(payload, cache):
    if not payload.startswith(b'\x89PNG\r\n\x1a\n'):
        return payload
    if payload not in cache:
        compressed = zopfli.png.optimize(payload, lossy_transparent=False, lossy_8bit=False,
                                        keepchunks=['iCCP', 'sRGB', 'gAMA', 'cHRM', 'pHYs'])
        with Image.open(io.BytesIO(payload)) as before, Image.open(io.BytesIO(compressed)) as after:
            if before.size != after.size or before.convert('RGBA').tobytes() != after.convert('RGBA').tobytes():
                raise ValueError('Icon pixels changed')
        cache[payload] = min((payload, compressed), key=len)
    return cache[payload]
