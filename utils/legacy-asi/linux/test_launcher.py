#!/usr/bin/env python3
"""Source-only launcher tests: synthetic PE bytes and mocked process creation.

No Wine, Windows image, ASI, game, shell command or downloaded code is run.
"""

import contextlib
import importlib.machinery
import importlib.util
import io
import os
from pathlib import Path
import struct
import tempfile
import types
import unittest
from unittest import mock


LAUNCHER = Path(__file__).with_name("reVC-legacy-asi")
LOADER = importlib.machinery.SourceFileLoader("revc_legacy_wine_launcher", str(LAUNCHER))
SPEC = importlib.util.spec_from_loader(LOADER.name, LOADER)
launcher = importlib.util.module_from_spec(SPEC)
LOADER.exec_module(launcher)


def image_bytes(dll=False, canonical=False):
    """Construct inert PE headers/exports, deliberately not executable code."""
    data = bytearray(0xA00)
    data[:2] = b"MZ"
    nt = 0x80
    struct.pack_into("<I", data, 60, nt)
    data[nt:nt + 4] = b"PE\0\0"
    flags = 0x0102 | (0x2000 if dll else 0) | (1 if canonical else 0)
    sections = 3 if canonical else 1
    struct.pack_into("<HHIIIHH", data, nt + 4, 0x014C, sections, 0, 0, 0, 224, flags)
    optional = nt + 24
    struct.pack_into("<H", data, optional, 0x010B)
    struct.pack_into("<I", data, optional + 28, 0x00400000 if canonical else 0x20000000)
    struct.pack_into("<I", data, optional + 56, 0x630000 if canonical else 0x2000)
    struct.pack_into("<I", data, optional + 60, 0x400)
    struct.pack_into("<H", data, optional + 70, 0 if canonical else 0x40)
    struct.pack_into("<I", data, optional + 92, 16)
    table = optional + 224

    def section(index, name, size, rva, raw_size, raw_offset, protection):
        offset = table + index * 40
        data[offset:offset + 8] = name.ljust(8, b"\0")
        struct.pack_into("<IIII", data, offset + 8, size, rva, raw_size, raw_offset)
        struct.pack_into("<I", data, offset + 36, protection)

    section(0, b".text", 0x200, 0x1000, 0x200, 0x400, 0x60000020)
    if canonical:
        section(1, b".rdata", 0x400, 0x2000, 0x400, 0x600, 0x40000040)
        section(2, b".compat", 0x620000, 0x3000, 0, 0, 0xC0000080)
        struct.pack_into("<II", data, optional + 96, 0x2000, 0x100)
        # Export names/functions/ordinals, protocol marker, then ASCII names.
        struct.pack_into("<IIIII", data, 0x600 + 20, 2, 2, 0x2040, 0x2050, 0x2060)
        struct.pack_into("<II", data, 0x640, 0x2100, 0x1000)
        struct.pack_into("<II", data, 0x650, 0x2140, 0x2180)
        struct.pack_into("<HH", data, 0x660, 0, 1)
        struct.pack_into("<IIIII", data, 0x700, *launcher.PROTOCOL_MARKER)
        for offset, name in ((0x740, b"reVC_LegacyArenaProtocol\0"), (0x780, b"reVC_LegacyAsiPreInit\0")):
            data[offset:offset + len(name)] = name
    return data


class LauncherTests(unittest.TestCase):
    def setUp(self):
        fixture_root = Path(__file__).resolve().parents[3] / "build" / "legacy-asi-launcher-python-tests"
        fixture_root.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="source-only-", dir=str(fixture_root))
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.runtime = self.base / "runtime with spaces"
        self.runtime.mkdir()
        (self.runtime / "reVC.exe").write_bytes(image_bytes())
        (self.runtime / "reVC-game.exe").write_bytes(image_bytes(canonical=True))
        (self.runtime / "reVC-core.dll").write_bytes(image_bytes(dll=True))
        (self.runtime / "legacy-asi").mkdir()
        (self.runtime / "legacy-asi" / "enabled.ini").write_text("; Nothing enabled\n[Modules]\n", encoding="utf-8")
        self.environment = {"HOME": str(self.base / "home"), "PATH": str(self.base / "path with spaces")}
        self.wine = str(self.base / "wine executable with spaces")
        self.stdout = io.StringIO()
        self.stderr = io.StringIO()

    def invoke(self, arguments=None, code=0, which=True, extra_environment=None):
        environment = dict(self.environment)
        if extra_environment:
            environment.update(extra_environment)
        with mock.patch.dict(os.environ, environment, clear=True), \
                mock.patch.object(launcher.sys, "platform", "linux"), \
                mock.patch.object(launcher.platform, "machine", return_value="x86_64"), \
                mock.patch.object(launcher.shutil, "which", return_value=self.wine if which else None) as lookup, \
                mock.patch.object(launcher.subprocess, "run", return_value=types.SimpleNamespace(returncode=code)) as run, \
                contextlib.redirect_stdout(self.stdout), contextlib.redirect_stderr(self.stderr):
            result = launcher.main([] if arguments is None else arguments, self.runtime)
        return result, run, lookup

    def mutate_bootstrap(self, offset, format_string, *values):
        path = self.runtime / "reVC-game.exe"
        data = bytearray(path.read_bytes())
        struct.pack_into(format_string, data, offset, *values)
        path.write_bytes(data)

    def test_check_is_nonmutating_and_executes_nothing(self):
        before = {str(path.relative_to(self.base)): path.read_bytes() for path in self.base.rglob("*") if path.is_file()}
        result, run, _ = self.invoke(["--check"])
        self.assertEqual(result, 0, self.stderr.getvalue())
        run.assert_not_called()
        self.assertFalse((self.base / "home").exists())
        after = {str(path.relative_to(self.base)): path.read_bytes() for path in self.base.rglob("*") if path.is_file()}
        self.assertEqual(before, after)
        self.assertIn("does not certify", self.stdout.getvalue())

    def test_launch_preserves_argv_and_child_exit(self):
        arguments = ["-windowed", "path with spaces", 'quote"and;$(nothing)', ""]
        result, run, _ = self.invoke(["--"] + arguments, code=37)
        self.assertEqual(result, 37)
        call = run.call_args
        self.assertEqual(call.args[0], [str(Path(self.wine).resolve()), str(self.runtime / "reVC.exe")] + arguments)
        self.assertEqual(call.kwargs["cwd"], str(self.runtime))
        self.assertFalse(call.kwargs["check"])
        self.assertNotIn("shell", call.kwargs)
        prefix = self.base / "home" / ".local" / "share" / "reVC" / "legacy-asi-wine"
        self.assertEqual(call.kwargs["env"]["WINEPREFIX"], str(prefix))
        self.assertTrue(prefix.parent.is_dir())
        self.assertFalse(prefix.exists(), "Only Wine may initialize the prefix")
        self.assertNotIn("WINEARCH", call.kwargs["env"])

    def test_signal_exit_has_shell_convention(self):
        result, _, _ = self.invoke(code=-15)
        self.assertEqual(result, 143)

    def test_existing_custom_prefix_and_winearch_preserved(self):
        prefix = self.base / "existing prefix"
        prefix.mkdir()
        marker = prefix / "user.reg"
        marker.write_bytes(b"user owned content")
        result, run, _ = self.invoke(extra_environment={"WINEPREFIX": str(prefix), "WINEARCH": "win64"})
        self.assertEqual(result, 0)
        self.assertEqual(run.call_args.kwargs["env"]["WINEPREFIX"], str(prefix))
        self.assertEqual(run.call_args.kwargs["env"]["WINEARCH"], "win64")
        self.assertEqual(marker.read_bytes(), b"user owned content")

    def test_custom_wine_is_one_executable_not_shell_words(self):
        result, _, lookup = self.invoke(["--check"], extra_environment={"REVC_WINE": "wine --must-not-be-a-command"})
        self.assertEqual(result, 0)
        self.assertEqual(lookup.call_args.args[0], "wine --must-not-be-a-command")

    def test_xdg_prefix(self):
        xdg = self.base / "xdg data"
        result, run, _ = self.invoke(extra_environment={"XDG_DATA_HOME": str(xdg)})
        self.assertEqual(result, 0)
        self.assertEqual(run.call_args.kwargs["env"]["WINEPREFIX"], str(xdg / "reVC" / "legacy-asi-wine"))
        self.assertFalse((xdg / "reVC" / "legacy-asi-wine").exists())

    def test_invalid_prefix_configuration(self):
        for key, value in (("WINEPREFIX", "relative-prefix"), ("WINEPREFIX", ""), ("XDG_DATA_HOME", "relative-data")):
            with self.subTest(key=key, value=value):
                result, run, _ = self.invoke(extra_environment={key: value})
                self.assertEqual(result, 2)
                run.assert_not_called()

    def test_missing_custom_prefix_parent_is_not_created(self):
        missing = self.base / "user parent" / "prefix"
        result, run, _ = self.invoke(extra_environment={"WINEPREFIX": str(missing)})
        self.assertEqual(result, 2)
        run.assert_not_called()
        self.assertFalse(missing.parent.exists())

    def test_file_prefix_rejected(self):
        prefix = self.base / "not a directory"
        prefix.write_bytes(b"user data")
        result, run, _ = self.invoke(extra_environment={"WINEPREFIX": str(prefix)})
        self.assertEqual(result, 2)
        run.assert_not_called()
        self.assertEqual(prefix.read_bytes(), b"user data")

    def test_missing_wine_does_not_create_default_parent(self):
        result, run, _ = self.invoke(which=False)
        self.assertEqual(result, 127)
        run.assert_not_called()
        self.assertIn("32-bit Windows", self.stderr.getvalue())
        self.assertFalse((self.base / "home").exists())

    def test_empty_wine_override_rejected(self):
        result, run, lookup = self.invoke(extra_environment={"REVC_WINE": ""})
        self.assertEqual(result, 2)
        run.assert_not_called()
        lookup.assert_not_called()

    def test_missing_runtime_files_rejected(self):
        for name in ("reVC.exe", "reVC-game.exe", "reVC-core.dll", "legacy-asi/enabled.ini"):
            with self.subTest(name=name):
                path = self.runtime / name
                data = path.read_bytes()
                path.unlink()
                result, run, _ = self.invoke()
                self.assertEqual(result, 2)
                run.assert_not_called()
                path.write_bytes(data)

    def test_native_linux_elf_rejected(self):
        (self.runtime / "reVC.exe").write_bytes(b"\x7fELF" + b"\0" * 124)
        result, run, _ = self.invoke()
        self.assertEqual(result, 2)
        self.assertIn("native Linux ELF", self.stderr.getvalue())
        run.assert_not_called()

    def test_native_x64_and_wrong_module_types_rejected(self):
        for name, offset, value in (("reVC.exe", 0x84, 0x8664), ("reVC-game.exe", 0x96, 0x2103), ("reVC-core.dll", 0x96, 0x0102)):
            with self.subTest(name=name):
                path = self.runtime / name
                original = path.read_bytes()
                data = bytearray(original)
                struct.pack_into("<H", data, offset, value)
                path.write_bytes(data)
                result, run, _ = self.invoke()
                self.assertEqual(result, 2)
                run.assert_not_called()
                path.write_bytes(original)

    def test_truncated_and_bounds_corruption_rejected(self):
        path = self.runtime / "reVC-game.exe"
        original = path.read_bytes()
        for data in (original[:63], original[:0x90], b"MZ" + b"\0" * 61 + b"\xff"):
            with self.subTest(size=len(data)):
                path.write_bytes(data)
                result, run, _ = self.invoke()
                self.assertEqual(result, 2)
                run.assert_not_called()
        path.write_bytes(original)
        self.mutate_bootstrap(60, "<I", 0xFFFFFFF0)
        result, run, _ = self.invoke()
        self.assertEqual(result, 2)
        run.assert_not_called()

    def test_canonical_bootstrap_contract_rejected_on_changes(self):
        optional = 0x80 + 24
        table = optional + 224
        mutations = (
            (optional + 28, "<I", 0x500000),  # wrong image base
            (optional + 70, "<H", 0x40),  # ASLR bootstrap cannot own fixed addresses
            (0x96, "<H", 0x0102),  # relocations not stripped
            (table + 80 + 36, "<I", 0xE0000080),  # .compat executable
            (table + 80 + 36, "<I", 0x40000080),  # .compat read-only
            (table + 80 + 8, "<I", 0x1000),  # .compat too small
            (table + 80 + 12, "<I", 0x6B000),  # .compat misses early ABI addresses
            (0x704, "<I", 999),  # incompatible protocol version
            (0x644, "<I", 0x2050),  # callback forwarded into export directory
            (0x644, "<I", 0x2100),  # callback points into non-executable .rdata
            (0x600 + 24, "<I", 0xFFFFFFFF),  # export count oversized
            (0x650, "<I", 0xFFFFFFF0),  # export name is out of bounds
        )
        path = self.runtime / "reVC-game.exe"
        original = path.read_bytes()
        for offset, format_string, value in mutations:
            with self.subTest(offset=offset, value=value):
                path.write_bytes(original)
                self.mutate_bootstrap(offset, format_string, value)
                result, run, _ = self.invoke()
                self.assertEqual(result, 2, self.stdout.getvalue())
                run.assert_not_called()

    def test_host_checks(self):
        with mock.patch.object(launcher.sys, "platform", "win32"), contextlib.redirect_stderr(self.stderr):
            self.assertEqual(launcher.main([], self.runtime), 2)
        with mock.patch.object(launcher.sys, "platform", "linux"), \
                mock.patch.object(launcher.platform, "machine", return_value="aarch64"), contextlib.redirect_stderr(self.stderr):
            self.assertEqual(launcher.main([], self.runtime), 2)

    def test_startup_oserror_is_reported_without_shell_retry(self):
        with mock.patch.dict(os.environ, self.environment, clear=True), \
                mock.patch.object(launcher.sys, "platform", "linux"), \
                mock.patch.object(launcher.platform, "machine", return_value="x86_64"), \
                mock.patch.object(launcher.shutil, "which", return_value=self.wine), \
                mock.patch.object(launcher.subprocess, "run", side_effect=OSError("owned test failure")) as run, \
                contextlib.redirect_stderr(self.stderr):
            self.assertEqual(launcher.main([], self.runtime), 1)
        self.assertEqual(run.call_count, 1)
        self.assertIn("owned test failure", self.stderr.getvalue())

    def test_unknown_launcher_arguments_require_separator(self):
        with self.assertRaises(SystemExit) as error, contextlib.redirect_stderr(self.stderr):
            launcher.main(["-windowed"], self.runtime)
        self.assertEqual(error.exception.code, 2)

    def test_check_cannot_forward_arguments(self):
        with self.assertRaises(SystemExit) as error, contextlib.redirect_stderr(self.stderr):
            launcher.main(["--check", "--", "-windowed"], self.runtime)
        self.assertEqual(error.exception.code, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
