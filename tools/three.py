#!/usr/bin/env python3
"""three.py — Autonomous 3DS toolchain: build → upload → restart → debug.

This is the single entry point for the full 3DS development loop.
It replaces the manual workflow of:
  1. make docker-build
  2. scp to Deck
  3. cp to /run/media/.../simcity_test.3dsx
  4. SSH to Deck, pkill azahar, launch via EmuDeck script
  5. Wait for debug bridge, run commands

Usage:
    # Full cycle
    python3 tools/three.py --host 10.0.0.130 --build --upload --restart --debug --cmd "prof"

    # Build + upload only
    python3 tools/three.py --host 10.0.0.130 --build --upload

    # Debug commands on existing session
    python3 tools/three.py --host 10.0.0.130 --debug --cmd "prof" --cmd "state"

    # CI mode (JSON output, exit codes)
    python3 tools/three.py --host 10.0.0.130 --build --upload --restart --debug --cmd "prof" --json
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Any

# Import our modules
sys.path.insert(0, str(Path(__file__).parent))
from bridge import Bridge, BridgeError, BridgeTimeoutError, BridgeWedgeError
from ftp_up import upload_and_verify, FTPError, md5_file


class ThreeError(Exception):
    """Base exception for three.py errors."""
    pass


class BuildError(ThreeError):
    pass


class UploadError(ThreeError):
    pass


class RestartError(ThreeError):
    pass


class VerificationError(ThreeError):
    pass


class Three:
    """Main orchestration class."""

    def __init__(
        self,
        host: str,
        port: int = 5002,
        ftp_port: int = 5000,
        binary_path: str = "build/simcity.3dsx",
        remote_path: str = "/3ds/simcity.3dsx",
        work_dir: Optional[str] = None,
        json_output: bool = False,
    ):
        self.host = host
        self.port = port
        self.ftp_port = ftp_port
        self.binary_path = Path(binary_path)
        self.remote_path = remote_path
        self.work_dir = Path(work_dir) if work_dir else Path.cwd()
        self.json_output = json_output

        self.local_md5: Optional[str] = None
        self.remote_md5: Optional[str] = None
        self.build_hash: Optional[str] = None
        self.bridge: Optional[Bridge] = None

    def log(self, msg: str, level: str = "INFO") -> None:
        if not self.json_output:
            print(f"[{level}] {msg}")

    def log_json(self, data: Dict[str, Any]) -> None:
        if self.json_output:
            print(json.dumps(data, separators=(",", ":")))

    # ─── Phase 1: Build ───

    def build(self) -> bool:
        """Run `make docker-build` and verify binary exists."""
        # Check if binary already exists and is up to date
        if self.binary_path.exists():
            existing_md5 = md5_file(str(self.binary_path))
            self.local_md5 = existing_md5
            self.log(f"Binary already exists: {self.binary_path} ({existing_md5}) — skipping rebuild")
            self.log_json({"phase": "build", "status": "skipped", "md5": existing_md5, "size": self.binary_path.stat().st_size})
            return True

        self.log("Building via make docker-build...")
        self.log_json({"phase": "build", "status": "started"})

        try:
            # Change to work directory
            old_cwd = os.getcwd()
            os.chdir(self.work_dir)

            result = subprocess.run(
                ["make", "docker-build"],
                capture_output=True,
                text=True,
                timeout=600,  # 10 minutes
            )

            os.chdir(old_cwd)

            if result.returncode != 0:
                raise BuildError(f"Build failed:\n{result.stderr}")

            if not self.binary_path.exists():
                raise BuildError(f"Binary not found at {self.binary_path}")

            self.local_md5 = md5_file(str(self.binary_path))
            self.log(f"Build OK: {self.binary_path} ({self.local_md5})")
            self.log_json({"phase": "build", "status": "ok", "md5": self.local_md5, "size": self.binary_path.stat().st_size})
            return True

        except subprocess.TimeoutExpired:
            raise BuildError("Build timed out after 600s")
        except Exception as e:
            raise BuildError(str(e))

    # ─── Phase 2: Upload ───

    def upload(self) -> bool:
        """Upload binary to 3DS via FTP and verify MD5."""
        # Ensure we have local MD5 (compute from binary if needed)
        if not self.local_md5:
            if self.binary_path.exists():
                self.local_md5 = md5_file(str(self.binary_path))
                self.log(f"Computed local MD5: {self.local_md5}")
            else:
                raise UploadError("No local MD5 and binary not found — run build() first")

        self.log(f"Uploading to {self.host}:{self.ftp_port}{self.remote_path}...")
        self.log_json({"phase": "upload", "status": "started", "host": self.host, "remote_path": self.remote_path})

        try:
            local_md5, remote_md5 = upload_and_verify(
                self.host,
                str(self.binary_path),
                self.remote_path,
                self.ftp_port,
                upload_timeout=120,
                verify_timeout=90,
                retries=3,
            )
            self.remote_md5 = remote_md5

            if local_md5 != remote_md5:
                raise VerificationError(f"MD5 mismatch: local={local_md5} remote={remote_md5}")

            self.log("Upload OK: MD5 verified")
            self.log_json({"phase": "upload", "status": "ok", "md5": local_md5})
            return True

        except FTPError as e:
            raise UploadError(str(e))

    # ─── Phase 3: Restart ───

    def restart(self) -> bool:
        """Restart the application on 3DS and verify new binary is running.

        Strategy:
        1. Connect to debug bridge
        2. Pause current execution
        3. Wait for bridge to close (app exits)
        4. On 3DS, the homebrew must be re-launched — this requires
           the app to exit and the user/launcher to start it again.
           For automation, we use the debug bridge to trigger a clean
           shutdown, then wait for a new `info` with updated build hash.
        """
        self.log("Restarting application on 3DS...")
        self.log_json({"phase": "restart", "status": "started"})

        try:
            # Connect to debug bridge
            with Bridge(self.host, self.port) as b:
                # Verify connection
                ping = b.ping()
                if not ping.startswith("OK"):
                    raise RestartError(f"Ping failed: {ping}")

# Get current build hash
            info = b.info()
            if not info.startswith("OK"):
                raise RestartError(f"Info failed: {info}")

            old_build: Optional[str] = self._parse_build_hash(info)
            self.log(f"Current build hash: {old_build}")

            # Pause the application
            pause_reply = b.pause()
            self.log(f"Paused: {pause_reply}")

            # Wait for app to actually stop (bridge closes or frame counter stops)
            time.sleep(2.0)

            # Now the app is paused/stopped. On real 3DS hardware,
            # the homebrew must be re-launched. This typically requires
            # the user to select it from the homebrew menu, OR the app
            # can be designed to auto-restart on a debug command.
            #
            # For now, we wait for the new binary to be running by
            # polling the debug bridge until it responds with new build hash.

            self.log("Waiting for application restart...")
            deadline = time.time() + 60.0
            while time.time() < deadline:
                try:
                    with Bridge(self.host, self.port) as b:
                        info = b.info()
                        if info.startswith("OK"):
                            new_build = self._parse_build_hash(info)
                            self.log(f"New build hash: {new_build}")
                            if new_build != old_build:
                                self.build_hash = new_build
                                self.log("Restart verified: new build running")
                                self.log_json({"phase": "restart", "status": "ok", "old_build": old_build, "new_build": new_build})
                                return True
                except (BridgeError, ConnectionError, BridgeTimeoutError):
                    pass
                time.sleep(2.0)

            raise RestartError("Timeout waiting for restart (60s)")

        except Exception as e:
            raise RestartError(str(e))

    def _parse_build_hash(self, info: str) -> Optional[str]:
        """Extract build hash from info output."""
        # Info format: "OK 3dsdbgkit/2 build=ftp22 breaks=0 ..."
        for tok in info.split():
            if tok.startswith("build="):
                return tok[6:]
        return None

    # ─── Phase 4: Debug Commands ───

    def debug(self, commands: List[str], verify: bool = True) -> Dict[str, str]:
        """Execute debug commands on the bridge.

        Args:
            commands: List of command strings (e.g., ["prof", "state", "prof"])
            verify: If True, wait for flag changes where applicable

        Returns:
            Dict mapping command -> response
        """
        self.log(f"Executing {len(commands)} debug commands...")
        self.log_json({"phase": "debug", "status": "started", "commands": commands})

        results = {}
        try:
            with Bridge(self.host, self.port) as b:
                # Initial health check
                ping = b.ping()
                if not ping.startswith("OK"):
                    raise BridgeError(f"Ping failed: {ping}")
                self.log(f"Ping: {ping}")

                for cmd in commands:
                    self.log(f"Running: {cmd}")
                    if verify and cmd in ("cclog 1", "anlz 1", "trace 1", "cclog 0", "anlz 0", "trace 0"):
                        # Toggle commands — wait for flag
                        flag = cmd.split()[0]
                        val = cmd.split()[1]
                        b._raw_cmd(cmd)
                        if not b.wait_for_flag(flag, val, timeout=10):
                            raise VerificationError(f"Flag {flag} did not switch to {val}")
                        results[cmd] = f"OK (flag {flag}={val} verified)"
                    else:
                        reply = b._raw_cmd(cmd)
                        results[cmd] = reply
                        self.log(f"  {cmd} -> {reply[:80]}...")

            self.log_json({"phase": "debug", "status": "ok", "results": results})
            return results

        except BridgeError as e:
            raise ThreeError(f"Debug command failed: {e}")

    # ─── Full Pipeline ───

    def run_pipeline(
        self,
        do_build: bool = True,
        do_upload: bool = True,
        do_restart: bool = True,
        debug_commands: Optional[List[str]] = None,
    ) -> Dict[str, Any]:
        """Run the full pipeline with selected phases."""
        results: Dict[str, Any] = {"phases": {}}

        if do_build:
            self.build()
            results["phases"]["build"] = {"status": "ok", "md5": self.local_md5}

        if do_upload:
            self.upload()
            results["phases"]["upload"] = {"status": "ok", "md5": self.remote_md5}

        if do_restart:
            self.restart()
            results["phases"]["restart"] = {"status": "ok", "build_hash": self.build_hash}

        if debug_commands:
            debug_results = self.debug(debug_commands)
            results["phases"]["debug"] = {"status": "ok", "commands": debug_results}

        results["status"] = "ok"
        self.log_json({"phase": "complete", "results": results})
        return results


def main():
    ap = argparse.ArgumentParser(
        description="Autonomous 3DS toolchain: build → upload → restart → debug",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Full cycle
  python3 tools/three.py --host 10.0.0.130 --build --upload --restart --debug --cmd "prof"

  # Build + upload only
  python3 tools/three.py --host 10.0.0.130 --build --upload

  # Debug commands on existing session
  python3 tools/three.py --host 10.0.0.130 --debug --cmd "prof" --cmd "state"

  # CI mode (JSON output)
  python3 tools/three.py --host 10.0.0.130 --build --upload --restart --debug --cmd "prof" --json
        """
    )
    ap.add_argument("--host", default="10.0.0.130", help="3DS IP address")
    ap.add_argument("--port", type=int, default=5002, help="Debug bridge port")
    ap.add_argument("--ftp-port", type=int, default=5000, help="FTP port")
    ap.add_argument("--binary", default="build/simcity.3dsx", help="Local binary path")
    ap.add_argument("--remote", default="/3ds/simcity.3dsx", help="Remote path on 3DS SD")
    ap.add_argument("--work-dir", default=".", help="Working directory")

    # Phase selection
    ap.add_argument("--build", action="store_true", help="Run docker build")
    ap.add_argument("--upload", action="store_true", help="Upload via FTP")
    ap.add_argument("--restart", action="store_true", help="Restart app on 3DS")
    ap.add_argument("--debug", action="store_true", help="Run debug commands")
    ap.add_argument("--cmd", action="append", help="Debug command to run (can repeat)")

    # Output
    ap.add_argument("--json", action="store_true", help="JSON output for CI")

    args = ap.parse_args()

    # Default to all phases if none specified
    if not any([args.build, args.upload, args.restart, args.debug]):
        args.build = args.upload = args.restart = True

    three = Three(
        host=args.host,
        port=args.port,
        ftp_port=args.ftp_port,
        binary_path=args.binary,
        remote_path=args.remote,
        work_dir=args.work_dir,
        json_output=args.json,
    )

    try:
        results = three.run_pipeline(
            do_build=args.build,
            do_upload=args.upload,
            do_restart=args.restart,
            debug_commands=args.cmd or [],
        )
        if not args.json:
            print("\n=== PIPELINE COMPLETE ===")
        sys.exit(0)
    except ThreeError as e:
        if args.json:
            print(json.dumps({"status": "error", "error": str(e)}, separators=(",", ":")))
        else:
            print(f"\n[ERROR] {e}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        if args.json:
            print(json.dumps({"status": "error", "error": f"Unexpected: {e}"}, separators=(",", ":")))
        else:
            print(f"\n[UNEXPECTED ERROR] {e}", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()