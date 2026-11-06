#!/usr/bin/env python3
"""Data-only report tests and mocked processes; no Wine, PE or ASI execution."""

import contextlib
import importlib.util
import io
from pathlib import Path
import signal
import subprocess
import tempfile
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location("revc_probe_runner", Path(__file__).with_name("run_probe.py"))
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)
VALID = ("[canonical-wine-probe]\nstatus=PASS\nstage=complete\ncanonical_image=1\n"
         "modules=3\nnative_events=2\ntime_scale=1.750\nmemory_error=\nloader_error=\n").encode()


class ReportTests(unittest.TestCase):
    def test_valid_report_and_crlf(self):
        self.assertEqual(runner.parse_report(VALID)["status"], "PASS")
        self.assertEqual(runner.parse_report(VALID.replace(b"\n", b"\r\n"))["modules"], "3")

    def test_each_required_field_must_match(self):
        for key, value in runner.EXPECTED.items():
            with self.subTest(key=key):
                data = VALID.replace((key + "=" + value).encode(), (key + "=wrong").encode())
                with self.assertRaises(ValueError):
                    runner.parse_report(data)

    def test_duplicate_or_malformed_report_rejected(self):
        for data in (VALID + b"status=PASS\n", VALID + b"not a field\n",
                     VALID + b"bad key=value\n", VALID.replace(b"modules=3\n", b""),
                     VALID.replace(b"[canonical-wine-probe]", b"[other]"),
                     VALID + b"\0", b"\xff", b"x" * (runner.MAX_REPORT_BYTES + 1)):
            with self.subTest(data=data[:30]):
                with self.assertRaises((ValueError, UnicodeError)):
                    runner.parse_report(data)

    def test_signal_and_large_exit_codes_fail_closed(self):
        self.assertEqual(runner.exit_status(-15), 143)
        self.assertEqual(runner.exit_status(256), 1)
        self.assertEqual(runner.exit_status(1), 1)


class RunnerTests(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parents[3] / "build" / "legacy-asi-probe-runner-tests"
        root.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="source-only-", dir=str(root))
        self.addCleanup(self.temporary.cleanup)
        self.runtime = Path(self.temporary.name) / "runtime with spaces"
        self.runtime.mkdir()
        self.launcher = self.runtime / "reVC-legacy-asi"
        self.launcher.write_text("Inert source fixture; never executed.\n", encoding="utf-8")
        self.stdout, self.stderr = io.StringIO(), io.StringIO()

    def invoke(self, code=0, data=VALID, timeout=False, start_error=None):
        process = mock.Mock(pid=45678)

        def start(*args, **kwargs):
            if start_error:
                raise start_error
            self.process_call = (args, kwargs)
            kwargs["stdout"].write(b"Own launcher diagnostic\n")
            if data is not None:
                (self.runtime / runner.REPORT_NAME).write_bytes(data)
            return process

        process.wait.side_effect = [subprocess.TimeoutExpired("own fixture", 1), code] if timeout else None
        process.wait.return_value = code
        with mock.patch.object(runner.subprocess, "Popen", side_effect=start) as popen, \
                mock.patch.object(runner.os, "killpg", create=True) as killpg, \
                mock.patch.object(runner.signal, "SIGKILL", 9, create=True), \
                contextlib.redirect_stdout(self.stdout), contextlib.redirect_stderr(self.stderr):
            result = runner.run_probe(self.runtime, 1)
        return result, popen, process, killpg

    def test_success_prints_output_and_report(self):
        result, popen, process, killpg = self.invoke()
        self.assertEqual(result, 0)
        self.assertIn("Own launcher diagnostic", self.stdout.getvalue())
        self.assertIn("status=PASS", self.stdout.getvalue())
        args, kwargs = self.process_call
        self.assertEqual(args[0], [runner.sys.executable, str(self.launcher)])
        self.assertEqual(kwargs["cwd"], str(self.runtime))
        self.assertTrue(kwargs["start_new_session"])
        self.assertNotIn("shell", kwargs)
        process.wait.assert_called_once_with(timeout=1)
        killpg.assert_not_called()

    def test_failed_exit_always_prints_failing_report(self):
        data = VALID.replace(b"status=PASS", b"status=FAIL").replace(b"stage=complete", b"stage=map-and-finalize")
        result, _, _, _ = self.invoke(code=1, data=data)
        self.assertEqual(result, 1)
        self.assertIn("Own launcher diagnostic", self.stdout.getvalue())
        self.assertIn("stage=map-and-finalize", self.stdout.getvalue())
        self.assertIn("launcher failed", self.stderr.getvalue())

    def test_valid_report_does_not_override_nonzero_exit(self):
        result, _, _, _ = self.invoke(code=37)
        self.assertEqual(result, 37)
        self.assertIn("status=PASS", self.stdout.getvalue())

    def test_exit_zero_requires_report(self):
        result, _, _, _ = self.invoke(data=None)
        self.assertEqual(result, 1)
        self.assertIn("report failure", self.stderr.getvalue())

    def test_exit_zero_requires_valid_report(self):
        result, _, _, _ = self.invoke(data=VALID + b"status=PASS\n")
        self.assertEqual(result, 1)
        self.assertIn("duplicate", self.stderr.getvalue())

    def test_nonzero_exit_with_missing_report_still_prints_output(self):
        result, _, _, _ = self.invoke(code=1, data=None)
        self.assertEqual(result, 1)
        self.assertIn("Own launcher diagnostic", self.stdout.getvalue())
        self.assertIn("report failure", self.stderr.getvalue())

    def test_timeout_kills_only_owned_group_and_fails_even_if_report_passes(self):
        result, popen, _, killpg = self.invoke(timeout=True)
        self.assertEqual(result, 124)
        self.assertEqual(killpg.call_args_list, [mock.call(45678, signal.SIGTERM), mock.call(45678, 9)])
        self.assertEqual(popen.call_count, 1)
        self.assertIn("status=PASS", self.stdout.getvalue())
        self.assertIn("timed out", self.stderr.getvalue())

    def test_timeout_escalates_only_same_group(self):
        process = mock.Mock(pid=12345)
        process.wait.side_effect = [subprocess.TimeoutExpired("own", 5), 0]
        with mock.patch.object(runner.os, "killpg", create=True) as killpg, \
                mock.patch.object(runner.signal, "SIGKILL", 9, create=True):
            runner.stop_owned_group(process)
        self.assertEqual(killpg.call_args_list, [mock.call(12345, signal.SIGTERM), mock.call(12345, 9)])

    def test_start_failure_still_reports_missing_report(self):
        result, popen, _, killpg = self.invoke(start_error=OSError("owned start failure"))
        self.assertEqual(result, 1)
        self.assertEqual(popen.call_count, 1)
        killpg.assert_not_called()
        self.assertIn("owned start failure", self.stderr.getvalue())
        self.assertIn("report failure", self.stderr.getvalue())

    def test_existing_report_is_preserved_and_not_reused(self):
        path = self.runtime / runner.REPORT_NAME
        path.write_bytes(VALID)
        with mock.patch.object(runner.subprocess, "Popen") as popen:
            with self.assertRaisesRegex(ValueError, "pre-existing"):
                runner.run_probe(self.runtime, 1)
        popen.assert_not_called()
        self.assertEqual(path.read_bytes(), VALID)

    def test_missing_launcher_does_not_start_anything(self):
        self.launcher.unlink()
        with mock.patch.object(runner.subprocess, "Popen") as popen:
            with self.assertRaisesRegex(ValueError, "colocated"):
                runner.run_probe(self.runtime, 1)
        popen.assert_not_called()

    def test_nonregular_and_oversized_reports_rejected(self):
        path = self.runtime / runner.REPORT_NAME
        path.mkdir()
        with self.assertRaises((OSError, ValueError)):
            runner.read_report(path)
        path.rmdir()
        path.write_bytes(b"x" * (runner.MAX_REPORT_BYTES + 1))
        with self.assertRaises(ValueError):
            runner.read_report(path)

    def test_cli_requires_linux_and_bounded_timeout(self):
        with mock.patch.object(runner.sys, "platform", "win32"), contextlib.redirect_stderr(self.stderr):
            self.assertEqual(runner.main(["--runtime", str(self.runtime)]), 2)
        for value in ("0", "-1", "3601", "nan", "inf"):
            with self.subTest(timeout=value), contextlib.redirect_stderr(self.stderr), self.assertRaises(SystemExit):
                runner.main(["--runtime", str(self.runtime), "--timeout", value])


if __name__ == "__main__":
    unittest.main(verbosity=2)
