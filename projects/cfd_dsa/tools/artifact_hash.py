"""Compute artifact digests without loading the whole file into memory."""

import hashlib
from pathlib import Path


def stream_sha256(stream):
    digest = hashlib.sha256()
    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
        digest.update(chunk)
    return digest.hexdigest()


def file_sha256(path):
    with Path(path).open("rb") as source:
        return stream_sha256(source)
