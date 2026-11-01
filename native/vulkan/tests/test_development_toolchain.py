import pathlib
import subprocess
import tempfile
import unittest

MODULE = pathlib.Path(__file__).resolve().parents[2] / 'cmake/DevelopmentToolchain.cmake'
class DevelopmentToolchainTests(unittest.TestCase):
    def configure(self, host, requested):
        with tempfile.TemporaryDirectory() as temporary:
            script = pathlib.Path(temporary) / 'check.cmake'
            script.write_text(f'''set(CMAKE_HOST_SYSTEM_NAME "{host}")
set(KALANDRA_LLVM_VERSION "23.1.3")
set(KALANDRA_DEVELOPMENT_LLVM_VERSION "{requested}" CACHE STRING "")
include("{MODULE.as_posix()}")
if(KALANDRA_DEVELOPMENT_LLVM_VERSION)
  if(NOT KALANDRA_LLVM_VERSION STREQUAL "23.1.4" OR NOT CMAKE_SKIP_INSTALL_RULES)
    message(FATAL_ERROR "development configuration did not retain release isolation")
  endif()
elseif(NOT KALANDRA_LLVM_VERSION STREQUAL "23.1.3" OR CMAKE_SKIP_INSTALL_RULES)
  message(FATAL_ERROR "default release configuration changed")
endif()
''')
            return subprocess.run(['cmake','-P',str(script)], capture_output=True, text=True)
    def test_release_default_unchanged(self):
        self.assertEqual(self.configure('Linux','').returncode, 0)
    def test_matching_linux_development_profile(self):
        self.assertEqual(self.configure('Linux','23.1.4').returncode, 0)
    def test_other_versions_and_platforms_rejected(self):
        for host,version in [('Linux','23.1.5'),('Darwin','23.1.4'),('Windows','23.1.4')]:
            with self.subTest(host=host,version=version):
                self.assertNotEqual(self.configure(host,version).returncode,0)
if __name__=='__main__': unittest.main()
