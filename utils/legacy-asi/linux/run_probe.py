#!/usr/bin/env python3
"""Run only the source-owned canonical probe; keep diagnostics on every failure."""

import argparse
import os
from pathlib import Path
import re
import signal
import stat
import subprocess
import sys
import tempfile


REPORT_NAME = "canonical-probe-result.txt"
MAX_REPORT_BYTES = 64 * 1024
EXPECTED = {"status": "PASS", "stage": "complete", "canonical_image": "1",
            "modules": "3", "native_events": "2", "time_scale": "1.750"}


def parse_report(data):
    if len(data) > MAX_REPORT_BYTES:
        raise ValueError("report exceeds the size limit")
    text = data.decode("utf-8", errors="strict")
    if "\0" in text:
        raise ValueError("report contains NUL bytes")
    lines = [line for line in text.splitlines() if line]
    if not lines or lines[0] != "[canonical-wine-probe]":
        raise ValueError("report is missing its canonical probe header")
    values = {}
    for line in lines[1:]:
        if "=" not in line:
            raise ValueError("malformed report line: {!r}".format(line))
        key, value = line.split("=", 1)
        if not re.fullmatch(r"[a-z_]+", key) or key in values:
            raise ValueError("invalid or duplicate report field: {!r}".format(key))
        values[key] = value
    mismatches = ["{}={!r} (expected {!r})".format(key, values.get(key), value)
                  for key, value in EXPECTED.items() if values.get(key) != value]
    if mismatches:
        raise ValueError("; ".join(mismatches))
    return values


def read_report(path):
    # Do not block on a FIFO or follow an unexpected result-file symlink.
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0)
    descriptor = os.open(str(path), flags)
    with os.fdopen(descriptor, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_REPORT_BYTES:
            raise ValueError("report is not a bounded regular file")
        return stream.read(MAX_REPORT_BYTES + 1)


def stop_owned_group(process):
    """The group belongs only to our start_new_session child, never Wine globally."""
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(process.pid, sig)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=5)
            if sig == signal.SIGTERM:
                # The launcher may exit before a descendant handles TERM.
                # Finish only this still-owned group, not a shared wineserver.
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            return
        except subprocess.TimeoutExpired:
            continue
    raise RuntimeError("owned probe process group did not terminate")


def exit_status(code):
    if code < 0:
        code = 128 - code
    return code if 1 <= code <= 255 else 1


def run_probe(runtime, timeout):
    runtime = Path(runtime).resolve(strict=True)
    launcher = runtime / "reVC-legacy-asi"
    report = runtime / REPORT_NAME
    if not runtime.is_dir() or not launcher.is_file():
        raise ValueError("runtime must contain the colocated reVC-legacy-asi launcher")
    if report.exists() or report.is_symlink():
        raise ValueError("refusing a pre-existing report; use a fresh probe runtime (no retry or cleanup is performed)")
    code = 1
    with tempfile.TemporaryFile(mode="w+b", dir=str(runtime)) as output:
        try:
            process = subprocess.Popen([sys.executable, str(launcher)], cwd=str(runtime),
                                       stdin=subprocess.DEVNULL, stdout=output,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            try:
                code = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                code = 124
                print("Canonical Wine probe timed out; terminating only its owned process group.", file=sys.stderr)
                try:
                    stop_owned_group(process)
                except (OSError, RuntimeError) as error:
                    print("Probe cleanup failed: {}".format(error), file=sys.stderr)
        except OSError as error:
            print("Cannot start canonical probe: {}".format(error), file=sys.stderr)
        finally:
            print("--- canonical probe launcher output ---", flush=True)
            output.seek(0)
            for chunk in iter(lambda: output.read(65536), b""):
                print(chunk.decode("utf-8", errors="replace"), end="", flush=True)
            print("\n--- end launcher output; exit={} ---".format(code), flush=True)
    valid = False
    try:
        data = read_report(report)
        print("--- canonical probe report: {} ---".format(report), flush=True)
        print(data.decode("utf-8", errors="replace"), flush=True)
        parse_report(data)
        valid = True
    except (OSError, UnicodeError, ValueError) as error:
        print("Canonical probe report failure: {}".format(error), file=sys.stderr)
    if code != 0:
        print("Canonical probe launcher failed; its report cannot turn a failing exit into success.", file=sys.stderr)
        return exit_status(code)
    if not valid:
        return 1
    print("Canonical Wine probe passed; only source-owned fixtures ran.")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=180)
    options = parser.parse_args(argv)
    if not 0 < options.timeout <= 3600:
        parser.error("--timeout must be greater than zero and at most 3600 seconds")
    if not sys.platform.startswith("linux"):
        print("The Wine probe driver requires Linux.", file=sys.stderr)
        return 2
    try:
        return run_probe(options.runtime, options.timeout)
    except (OSError, ValueError) as error:
        print("Canonical probe setup failed: {}".format(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
