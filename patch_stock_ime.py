# -*- coding: utf-8 -*-
"""Bind the styled, scale-aware candidate root to the original 2.4.2.0 movie."""
from __future__ import print_function
import hashlib
import io
import os
import struct
import sys
import zlib


STOCK_SHA256 = '3c1414ec12d46b07bcbcfb1efe40d4557ab23053e702dc9f07beadbde7cbda8e'
OLD_ROOT = b'\x00\x00IME_fla.MainTimeline\x00'
NEW_ROOT = b'\x00\x00StockCandidatePosition\x00'


def read_movie(data):
    if data[:4] != b'CWS\x0c':
        raise ValueError('Expected a compressed SWF 12 movie')
    body = zlib.decompress(data[8:])
    if len(body) + 8 != struct.unpack_from('<I', data, 4)[0]:
        raise ValueError('Incorrect SWF length')
    offset = (5 + 4 * (ord(body[:1]) >> 3) + 7) // 8 + 4  # RECT and frame rate/count
    header = body[:offset]
    tags = []
    while offset < len(body):
        start = offset
        kind, size = divmod(struct.unpack_from('<H', body, offset)[0], 64)
        offset += 2
        if size == 63:
            size = struct.unpack_from('<I', body, offset)[0]
            offset += 4
        end = offset + size
        if end > len(body):
            raise ValueError('Truncated SWF tag')
        tags.append((kind, body[offset:end], body[start:end]))
        offset = end
    if not tags or tags[-1][0] != 0:
        raise ValueError('Missing SWF end tag')
    return header, tags


def make_movie(stock, compiled):
    if hashlib.sha256(stock).hexdigest() != STOCK_SHA256:
        raise ValueError('Stock ime.swf differs from CN 2.4.2.0 #997; verify its ABI before building')
    header, original = read_movie(stock)
    _, subclass = read_movie(compiled)
    abc, = [raw for kind, _, raw in subclass if kind == 82]
    symbol, = [value for kind, value, _ in subclass if kind == 76]
    if symbol != b'\x01\x00' + NEW_ROOT:
        raise ValueError('Expected StockCandidatePosition as the compiled document class')
    chunks = [header]
    for kind, value, raw in original:
        if kind == 76:
            if value.count(OLD_ROOT) != 1:
                raise ValueError('Expected a single stock document class binding')
            value = value.replace(OLD_ROOT, NEW_ROOT)
            chunks.extend((abc, struct.pack('<HI', (76 << 6) | 63, len(value)), value))
        else:
            chunks.append(raw)
    body = b''.join(chunks)
    return stock[:4] + struct.pack('<I', len(body) + 8) + zlib.compress(body, 9)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit('Usage: patch_stock_ime.py STOCK_IME_SWF OUTPUT_IME_SWF')
    source, destination = sys.argv[1:]
    compiled = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'build', 'StockCandidatePosition.swf')
    if os.path.normcase(os.path.realpath(destination)) in [os.path.normcase(os.path.realpath(p)) for p in (source, compiled)]:
        sys.exit('Choose a separate output file')
    with io.open(source, 'rb') as original, io.open(compiled, 'rb') as subclass:
        result = make_movie(original.read(), subclass.read())
    with io.open(destination, 'wb') as output:
        output.write(result)
    print('Built original candidate movie with StockCandidatePosition root (%d bytes)' % len(result))
