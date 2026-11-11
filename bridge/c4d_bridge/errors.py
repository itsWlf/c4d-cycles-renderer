# SPDX-License-Identifier: GPL-3.0-or-later
"""Errors shared by the bridge's modules."""


class RequestError(Exception):
    """Raised by a request handler to answer with an error reply (see protocol/protocol.md)."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
