#!/usr/bin/env python3
import argparse
import base64
import hashlib
import pathlib
import re
import sys
import time

import serial


PROMPT = "IPC_LITE_PROMPT# "
SECONDARY_PROMPT_PATTERN = r"> "
UPLOAD_CHUNK_BYTES = 512


class SerialShell:
    def __init__(self, port: str, baudrate: int, timeout: float) -> None:
        self.ser = serial.Serial(port, baudrate=baudrate, timeout=timeout)
        self.buffer = ""

    def close(self) -> None:
        self.ser.close()

    def _read_until(self, pattern: str, timeout: float) -> str:
        deadline = time.time() + timeout
        regex = re.compile(pattern, re.S)
        while time.time() < deadline:
            chunk = self.ser.read(4096).decode(errors="ignore")
            if chunk:
                self.buffer += chunk
                if regex.search(self.buffer):
                    data = self.buffer
                    self.buffer = ""
                    return data
            else:
                time.sleep(0.05)
        raise TimeoutError(f"timeout waiting for pattern: {pattern}")

    def _write(self, text: str) -> None:
        self.ser.write(text.encode())
        self.ser.flush()

    def login(self, username: str, password: str) -> None:
        self._write("\n")
        data = self._read_until(r"(login:|# |> )", 10)
        if "> " in data:
            self._write("__IPC_LITE_EOF__\n")
            data = self._read_until(r"(login:|# )", 10)
        if "login:" in data:
            self._write(f"{username}\n")
            self._read_until(r"Password:", 10)
            self._write(f"{password}\n")
            self._read_until(r"# ", 10)

        self.run(f"export PS1='{PROMPT}'")

    def run(self, command: str, timeout: float = 30.0) -> str:
        self._write(command + "\n")
        return self._read_until(re.escape(PROMPT), timeout)

    def upload_file(self, source: pathlib.Path, destination: str) -> None:
        encoded = base64.b64encode(source.read_bytes()).decode()
        temp_path = f"{destination}.b64"
        total_chunks = (len(encoded) + UPLOAD_CHUNK_BYTES - 1) // UPLOAD_CHUNK_BYTES

        self.run(f"rm -f '{temp_path}' '{destination}'", timeout=10)
        self._write(f"cat > '{temp_path}' <<'__IPC_LITE_EOF__'\n")
        self._read_until(SECONDARY_PROMPT_PATTERN, 10)
        for index, offset in enumerate(range(0, len(encoded), UPLOAD_CHUNK_BYTES), start=1):
            self._write(encoded[offset : offset + UPLOAD_CHUNK_BYTES] + "\n")
            if index == 1 or index == total_chunks or index % 200 == 0:
                print(
                    f"  chunk {index}/{total_chunks} for {source.name}",
                    flush=True,
                )
            self._read_until(SECONDARY_PROMPT_PATTERN, 10)
        self._write("__IPC_LITE_EOF__\n")
        self._read_until(re.escape(PROMPT), 300)
        self.run(
            f"base64 -d '{temp_path}' > '{destination}' && rm -f '{temp_path}'",
            timeout=300,
        )


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    digest.update(path.read_bytes())
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/ttyUSB0")
    parser.add_argument("--baudrate", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=0.2)
    parser.add_argument("--username", default="root")
    parser.add_argument("--password", default="luckfox")
    parser.add_argument("--remote-dir", default="/root/ipc_lite")
    parser.add_argument("--run", action="store_true")
    args = parser.parse_args()

    base_dir = pathlib.Path(__file__).resolve().parent / "out" / "ipc_lite"
    files = [
        base_dir / "ipc_lite",
        base_dir / "ipc_lite.ini",
        base_dir / "run.sh",
    ]

    missing = [str(path) for path in files if not path.exists()]
    if missing:
        print("missing build outputs:", file=sys.stderr)
        for path in missing:
            print(f"  {path}", file=sys.stderr)
        return 1

    shell = SerialShell(args.port, args.baudrate, args.timeout)
    try:
        shell.login(args.username, args.password)
        shell.run(f"mkdir -p '{args.remote_dir}'", timeout=10)
        for source in files:
            destination = f"{args.remote_dir}/{source.name}"
            print(f"upload {source.name} -> {destination}", flush=True)
            shell.upload_file(source, destination)
            local_sha = sha256(source)
            output = shell.run(f"sha256sum '{destination}'", timeout=20)
            if local_sha not in output:
                print(f"sha256 mismatch for {source.name}", file=sys.stderr)
                return 1

        shell.run(
            f"chmod +x '{args.remote_dir}/ipc_lite' '{args.remote_dir}/run.sh'",
            timeout=10,
        )

        if args.run:
            print(shell.run(f"cd '{args.remote_dir}' && ./run.sh", timeout=10))
        else:
            print(shell.run(f"ls -l '{args.remote_dir}'", timeout=10))
    finally:
        shell.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
