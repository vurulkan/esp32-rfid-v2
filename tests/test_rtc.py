"""Execute the real rtc.cpp against a simulated DS3231 on a g++ host."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class RtcTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix="rfid-rtc-tests-")
        cls.binary = pathlib.Path(cls.work.name) / "rtc-tests"
        subprocess.run([
            "g++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I", str(ROOT / "tests/rtc_stubs"), "-I", str(ROOT / "src/esp32-rfid"),
            str(ROOT / "tests/rtc_test.cpp"), str(ROOT / "src/esp32-rfid/rtc.cpp"),
            "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def run_case(self, name):
        subprocess.run([str(self.binary), name], check=True)


def add_case(name):
    def test(self):
        self.run_case(name)
    setattr(RtcTests, "test_" + name, test)


for case in (
    "failed_reconfigure_preserves_requested_mode_and_latch",
    "rejects_readback_mismatch",
    "preserves_time_across_restart_and_toggle",
    "rejects_lost_power_until_adjusted",
    "recovers_from_bus_failures",
    "decodes_clock_modes_and_rejects_bad_registers",
    "validates_calendar_and_api_input",
    "enables_backup_without_erasing_loss_evidence",
    "failed_adjustment_preserves_loss_flag",
    "serializes_concurrent_clock_access",
):
    add_case(case)

if __name__ == "__main__":
    unittest.main(verbosity=2)
