#!/usr/bin/env python3
"""ftp_up.py — FTP upload to 3DS SD card with verification.

Extracted from dsdbg.py. The 3DS runs an anonymous FTP server on port 5000
(or 21) that serves the SD card. This module provides verified upload:

- Upload to /3ds/simcity.3dsx on SD card
- MD5 verification after upload
- Retry logic for transient failures
"""

import os
import subprocess
import hashlib
import tempfile
import time
from typing import Optional, Tuple


class FTPError(Exception):
    """Base exception for FTP errors."""
    pass


class FTPUploadError(FTPError):
    """Raised when upload fails."""
    pass


class FTPVerifyError(FTPError):
    """Raised when MD5 verification fails."""
    pass


def _curl(args, timeout):
    """Run curl with given args, return CompletedProcess."""
    return subprocess.run(["curl", "-sS", *args], capture_output=True, timeout=timeout)


def md5_file(path: str) -> str:
    """Compute MD5 of a local file."""
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            h.update(chunk)
    return h.hexdigest()


def ftp_url(host: str, port: int, path: str) -> str:
    """Construct FTP URL for anonymous access.

    The 3DS FTP server accepts anonymous login with any password.
    Path must be absolute (e.g., /3ds/simcity.3dsx).
    """
    return f"ftp://anonymous:.@{host}:{port}{path}"


def upload_file(
    host: str,
    local_path: str,
    remote_path: str,
    port: int = 5000,
    timeout: int = 120,
    retries: int = 3,
) -> bool:
    """Upload a file to the 3DS via FTP.

    Args:
        host: 3DS IP address
        local_path: Path to local file to upload
        remote_path: Remote path on 3DS (e.g., /3ds/simcity.3dsx)
        port: FTP port (default 5000)
        timeout: Curl timeout in seconds
        retries: Number of upload attempts

    Returns:
        True if upload succeeded

    Raises:
        FTPUploadError: If all retries fail
    """
    if not os.path.exists(local_path):
        raise FTPUploadError(f"Local file not found: {local_path}")

    last_err = ""
    for attempt in range(retries):
        try:
            r = _curl(["-T", local_path, ftp_url(host, port, remote_path)], timeout)
            if r.returncode == 0:
                return True
            last_err = r.stderr.decode(errors="replace")
        except subprocess.TimeoutExpired:
            last_err = f"timeout after {timeout}s"
        except Exception as e:
            last_err = str(e)

        if attempt < retries - 1:
            time.sleep(2.0 * (attempt + 1))

    raise FTPUploadError(f"Upload failed after {retries} attempts: {last_err}")


def fetch_file(
    host: str,
    remote_path: str,
    local_path: str,
    port: int = 5000,
    timeout: int = 90,
    retries: int = 5,
) -> int:
    """Download a file from the 3DS via FTP.

    Returns the size of the downloaded file.

    Raises:
        FTPError: If all retries fail
    """
    for attempt in range(retries):
        fd, tmp = tempfile.mkstemp(suffix=".ftp")
        os.close(fd)
        try:
            r = _curl(["-o", tmp, ftp_url(host, port, remote_path)], timeout)
            if r.returncode == 0 and os.path.exists(tmp) and os.path.getsize(tmp) > 0:
                os.replace(tmp, local_path)
                return os.path.getsize(local_path)
        finally:
            if os.path.exists(tmp):
                os.unlink(tmp)
        time.sleep(1.0)

    raise FTPError(f"Fetch {remote_path} failed after {retries} attempts")


def verify_remote_md5(
    host: str,
    remote_path: str,
    expected_md5: str,
    port: int = 5000,
    timeout: int = 90,
    retries: int = 3,
) -> bool:
    """Verify remote file MD5 by downloading to temp and comparing.

    This is the verification gate — the upload is not considered complete
    until the remote file's MD5 matches the local file's MD5.
    """
    for attempt in range(retries):
        fd, tmp = tempfile.mkstemp(suffix=".verify")
        os.close(fd)
        try:
            r = _curl(["-o", tmp, ftp_url(host, port, remote_path)], timeout)
            if r.returncode == 0 and os.path.exists(tmp):
                actual = md5_file(tmp)
                os.unlink(tmp)
                if actual == expected_md5:
                    return True
                else:
                    raise FTPVerifyError(
                        f"MD5 mismatch: expected {expected_md5}, got {actual}"
                    )
        finally:
            if os.path.exists(tmp):
                os.unlink(tmp)
        time.sleep(1.0)

    raise FTPVerifyError(f"MD5 verification failed after {retries} attempts")


def upload_and_verify(
    host: str,
    local_path: str,
    remote_path: str,
    port: int = 5000,
    upload_timeout: int = 120,
    verify_timeout: int = 90,
    retries: int = 3,
) -> Tuple[str, str]:
    """Upload file and verify MD5 in one atomic operation.

    Returns (local_md5, remote_md5) on success.

    Raises:
        FTPUploadError: If upload fails
        FTPVerifyError: If MD5 verification fails
    """
    local_md5 = md5_file(local_path)
    print(f"[ftp] Uploading {local_path} ({local_md5}) -> {host}:{port}{remote_path}")
    upload_file(host, local_path, remote_path, port, upload_timeout, retries)
    print(f"[ftp] Verifying MD5 on remote...")
    verify_remote_md5(host, remote_path, local_md5, port, verify_timeout, retries)
    print(f"[ftp] OK: remote MD5 matches {local_md5}")
    return local_md5, local_md5


if __name__ == "__main__":
    import sys
    if len(sys.argv) < 4:
        print("Usage: ftp_up.py <host> <local_path> <remote_path> [port]")
        sys.exit(1)
    host = sys.argv[1]
    local = sys.argv[2]
    remote = sys.argv[3]
    port = int(sys.argv[4]) if len(sys.argv) > 4 else 5000
    try:
        lm, rm = upload_and_verify(host, local, remote, port)
        print(f"SUCCESS: {lm} == {rm}")
        sys.exit(0)
    except FTPError as e:
        print(f"FAILED: {e}")
        sys.exit(1)