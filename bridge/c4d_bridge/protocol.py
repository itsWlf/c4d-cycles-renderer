# SPDX-License-Identifier: GPL-3.0-or-later
"""Wire format of the bridge protocol (see protocol/protocol.md).

Pure standard library and free of bpy, so the tests and any other client can use it too.
"""

import json
import struct

MAGIC = b"C4DB"
VERSION = 0
MAX_HEADER_BYTES = 16 * 1024 * 1024
MAX_BLOBS = 4096

_PREFIX = struct.Struct("<4sHHII")  # magic, version, flags, header length, blob count


class ProtocolError(Exception):
    """The peer sent something that is not a valid frame."""


class ConnectionClosed(Exception):
    """The peer closed the connection."""


def send_message(sock, header, blobs=()):
    """Send one frame. Blobs may be any bytes-like objects and are sent without copying."""
    payload = json.dumps(header, separators=(",", ":")).encode("utf-8")
    views = [memoryview(blob).cast("B") for blob in blobs]
    prefix = _PREFIX.pack(MAGIC, VERSION, 0, len(payload), len(views))
    lengths = struct.pack(f"<{len(views)}Q", *(view.nbytes for view in views))
    sock.sendall(prefix + lengths + payload)
    for view in views:
        sock.sendall(view)


def _receive_exactly(sock, count):
    buffer = bytearray(count)
    view = memoryview(buffer)
    received = 0
    while received < count:
        chunk = sock.recv_into(view[received:], count - received)
        if chunk == 0:
            raise ConnectionClosed()
        received += chunk
    return buffer


def receive_message(sock):
    """Block until one frame has arrived. Returns (header dict, list of bytearray blobs)."""
    magic, version, _flags, header_length, blob_count = _PREFIX.unpack(_receive_exactly(sock, _PREFIX.size))
    if magic != MAGIC:
        raise ProtocolError(f"bad magic {bytes(magic)!r}")
    if version != VERSION:
        raise ProtocolError(f"protocol version {version}, expected {VERSION}")
    if header_length > MAX_HEADER_BYTES or blob_count > MAX_BLOBS:
        raise ProtocolError("frame exceeds the protocol limits")
    lengths = struct.unpack(f"<{blob_count}Q", _receive_exactly(sock, 8 * blob_count))
    try:
        header = json.loads(bytes(_receive_exactly(sock, header_length)).decode("utf-8"))
    except ValueError as exc:
        raise ProtocolError(f"header is not valid JSON: {exc}") from exc
    if not isinstance(header, dict):
        raise ProtocolError("header is not a JSON object")
    blobs = [_receive_exactly(sock, length) for length in lengths]
    return header, blobs
