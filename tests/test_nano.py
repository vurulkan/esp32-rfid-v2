import pathlib
import subprocess
import tempfile
import unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class NanoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work=tempfile.TemporaryDirectory(prefix='rfid-nano-')
        cls.binary=pathlib.Path(cls.work.name)/'nano-tests'
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
            '-I',str(ROOT/'tests/host_stubs'),str(ROOT/'tests/nano_test.cpp'),'-o',str(cls.binary)],check=True)
    @classmethod
    def tearDownClass(cls):cls.work.cleanup()
    def run_case(self,name):subprocess.run([str(self.binary),name],check=True)
def add_case(name):
    def test(self):self.run_case(name)
    setattr(NanoTests,'test_'+name,test)
for name in ('accepts_valid_parity_and_rejects_corruption','flush_is_atomic_wrap_safe_and_discards_overflow','feedback_and_uart_overflow_recover'):
    add_case(name)
if __name__=='__main__':unittest.main(verbosity=2)
