# SPDX-License-Identifier: GPL-3.0-or-later
"""Writer of the shared-memory frame block (protocol/protocol.md, "Frame block").

A small state block with a fixed name says where the pixels are and how far the render is; the
pixels live in a second block that is replaced whenever the image size changes. Free of bpy.
"""

import ctypes
import mmap
import struct
import sys
import time

import numpy as np

STATE_MAGIC = 0x54535243  # "CRST"
STATE_LAYOUT = 1
STATE_SIZE = 256

STOPPED, RENDERING, PAUSED, DONE = 0, 1, 2, 3

_HEADER_SEQ = 8
_GEOMETRY = struct.Struct("<IIII")       # at 16: generation, width, height, latest
_BUFFER_SEQ = (32, 40)
_PROGRESS = struct.Struct("<QIIII")      # at 48: frame id, state, sample, samples, reserved
_TIMES = struct.Struct("<dd")            # at 72: start time, end time (Unix seconds)
_STATUS_OFFSET, _STATUS_SIZE = 128, 128


class FrameBlock:
    def __init__(self, token):
        self.prefix = f"cyclesrender_{token}"
        self.state_name = self.prefix + "_state"
        self.state = mmap.mmap(-1, STATE_SIZE, tagname=self.state_name)
        struct.pack_into("<II", self.state, 0, STATE_MAGIC, STATE_LAYOUT)
        self.generation = 0
        self.size = (0, 0)
        self.buffers = (None, None)   # two float32 arrays of width * height * 4 values, rows bottom-up
        self.latest = 0
        self.frame_id = 0
        self._header_seq = 0
        self._seq = [0, 0]
        self._mappings = []           # the current pixel block and the one before it
        self.set_progress(STOPPED, 0, 0, "")
        self.set_times(0.0, 0.0)
        # Raised after every published frame, so the reader need not poll.
        self._signal = None
        if sys.platform == "win32":
            kernel32 = ctypes.windll.kernel32
            kernel32.CreateEventW.restype = ctypes.c_void_p
            kernel32.CreateEventW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_wchar_p]
            kernel32.SetEvent.argtypes = [ctypes.c_void_p]
            self._signal = kernel32.CreateEventW(None, False, False, self.prefix + "_frame")
            self._set_event = kernel32.SetEvent

    def _resize(self, width, height):
        self.generation += 1
        values = width * height * 4
        mapping = mmap.mmap(-1, values * 4 * 2, tagname=f"{self.prefix}_px{self.generation}")
        array = np.frombuffer(mapping, dtype=np.float32)
        # A reader may still be copying from the previous block, so that one is kept alive.
        self._mappings = self._mappings[-1:] + [mapping]
        self.buffers = (array[:values], array[values:])
        self.size = (width, height)
        self.latest = 0
        self._seq = [0, 0]
        self._header_seq += 1
        struct.pack_into("<Q", self.state, _HEADER_SEQ, self._header_seq)        # odd: changing
        _GEOMETRY.pack_into(self.state, 16, self.generation, width, height, 0)
        struct.pack_into("<QQ", self.state, _BUFFER_SEQ[0], 0, 0)
        self._header_seq += 1
        struct.pack_into("<Q", self.state, _HEADER_SEQ, self._header_seq)

    def begin(self, width, height):
        """Returns (index, array) of the buffer to write the next frame into."""
        if self.size != (width, height):
            self._resize(width, height)
        index = 1 - self.latest
        self._seq[index] += 1
        struct.pack_into("<Q", self.state, _BUFFER_SEQ[index], self._seq[index])  # odd: being written
        return index, self.buffers[index]

    def end(self, index):
        """Publishes the frame written into buffer `index`."""
        self._seq[index] += 1
        struct.pack_into("<Q", self.state, _BUFFER_SEQ[index], self._seq[index])
        self.latest = index
        self.frame_id += 1
        struct.pack_into("<I", self.state, 28, index)
        struct.pack_into("<Q", self.state, 48, self.frame_id)
        if self._signal:
            self._set_event(self._signal)

    def set_progress(self, state, sample, samples, status):
        struct.pack_into("<IIII", self.state, 56, state, sample, samples, 0)
        text = status.encode("utf-8")[:_STATUS_SIZE - 1]
        self.state[_STATUS_OFFSET:_STATUS_OFFSET + _STATUS_SIZE] = text.ljust(_STATUS_SIZE, b"\0")

    def set_times(self, start, end):
        _TIMES.pack_into(self.state, 72, start, end)

    def write_test_pattern(self, width, height):
        """Publishes a frame whose value number i (in storage order) is ((i * 2654435761) mod 2^32) >> 8."""
        index, array = self.begin(width, height)
        numbers = np.arange(width * height * 4, dtype=np.uint64)
        array[:] = (((numbers * 2654435761) & 0xFFFFFFFF) >> 8).astype(np.float32)
        self.end(index)
        self.set_times(time.time(), 0.0)
