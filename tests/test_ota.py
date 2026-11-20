"""Run with python3 tests/test_ota.py on a host with g++ (including WSL).
The compiled harness executes callbacks extracted unchanged from production web.cpp.
No firmware is uploaded and no external device is used.
"""
import re
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class OtaRouteTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix="rfid-ota-tests-")
        cls.build = pathlib.Path(cls.work.name)
        source = (ROOT / "src/esp32-rfid/web.cpp").read_text()
        start = re.search(r'server\.on\(\s*"/firmware"\s*,\s*HTTP_POST\s*,', source).start()
        end = source.index('server.onNotFound(', start)
        (cls.build / "firmware_route.inc").write_text(source[start:end])
        start=source.index('server.on("/maintenance/format"')
        end=source.index('server.on("/maintenance/reboot"',start)
        (cls.build/'format_route.inc').write_text(source[start:end])
        cls.binary = cls.build / "ota-tests"
        subprocess.run([
            "g++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I", str(cls.build), str(ROOT / "tests/ota_route_test.cpp"),
            "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def run_case(self, name):
        subprocess.run([str(self.binary), name], check=True)

    def test_rejects_multiple_files_before_commit(self):
        self.run_case("rejects_multiple_files_before_commit")

    def test_preserves_another_maintenance_owner(self):
        self.run_case("preserves_another_maintenance_owner")

    def test_rejects_abort_after_file_end(self):
        self.run_case("rejects_abort_after_file_end")

    def test_rejects_unauthorized_or_maintenance_failure(self):
        self.run_case("rejects_unauthorized_or_maintenance_failure")

    def test_format_reboots_after_erasure_even_if_defaults_fail(self):
        self.run_case("format_reboots_after_erasure_even_if_defaults_fail")

    def test_rejects_incomplete_uploads(self):
        self.run_case("rejects_incomplete_uploads")

    def test_reboots_after_finalized_upload(self):
        self.run_case("reboots_after_finalized_upload")

    def test_recovers_after_failed_upload(self):
        self.run_case("recovers_after_failed_upload")


if __name__ == "__main__":
    unittest.main(verbosity=2)
