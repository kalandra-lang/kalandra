"""Exercise selection against real CMake/CTest, including nested directories."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
RUNNER = REPO/'scripts/test-native.py'


class WorkflowTests(unittest.TestCase):
    def test_real_selection_and_unchanged_contracts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'child').mkdir()
            (root/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.28)
project(selection NONE)
enable_testing()
add_test(NAME frontend-syntax COMMAND "${CMAKE_COMMAND}" -E true)
set_tests_properties(frontend-syntax PROPERTIES LABELS existing TIMEOUT 12 RESOURCE_LOCK fixture ENVIRONMENT "TEST_VALUE=unchanged")
add_test(NAME native-loop-debug COMMAND "${CMAKE_COMMAND}" -E true)
add_subdirectory(child)
if(LABEL_TESTS)
  include("'''+(REPO/'native/cmake/TestWorkflow.cmake').as_posix()+'''")
  kalandra_label_tests("${CMAKE_CURRENT_SOURCE_DIR}")
endif()
''')
            (root/'child/CMakeLists.txt').write_text('''add_test(NAME native-loop-sanitized COMMAND "${CMAKE_COMMAND}" -E true)
add_test(NAME native-device-array-thread-sanitized COMMAND "${CMAKE_COMMAND}" -E true)
''')
            build = root/'build'
            def configure(enabled):
                subprocess.run(['cmake','-S',str(root),'-B',str(build),f'-DLABEL_TESTS={enabled}'],check=True,capture_output=True)
                return json.loads(subprocess.check_output(['ctest','--test-dir',str(build),'--show-only=json-v1']))['tests']
            before, after = configure('OFF'), configure('ON')
            def contracts(tests):
                return {t['name']: {'command':t['command'], 'properties':[p for p in t['properties'] if p['name']!='LABELS']} for t in tests}
            self.assertEqual(contracts(before), contracts(after))
            labels = {t['name']:next(p['value'] for p in t['properties'] if p['name']=='LABELS') for t in after}
            self.assertIn('existing', labels['frontend-syntax'])
            self.assertIn('feature-full', labels['native-device-array-thread-sanitized'])
            self.assertIn('variant-thread-sanitized', labels['native-device-array-thread-sanitized'])
            self.assertNotIn('variant-sanitized', labels['native-device-array-thread-sanitized'])
            def select(*options):
                report = root/'selected.json'
                result = subprocess.run([sys.executable,str(RUNNER),'--build-dir',str(build),'--report',str(report),*options],capture_output=True,text=True)
                return result, json.loads(report.read_text()) if report.exists() else None
            result, report = select('--list','--feature','frontend','--feature','control','--variant','sanitized')
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual([t['name'] for t in report['tests']], ['native-loop-sanitized'])
            result, _ = select('--feature','frontend','--junit',str(root/'result.xml'))
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertTrue((root/'result.xml').is_file())
            result, _ = select('--feature','frontend','--match','^missing$')
            self.assertEqual(result.returncode,2)
            self.assertIn('matched no tests',result.stderr)
            result, _ = select('--list')
            self.assertEqual(result.returncode,0)
            self.assertEqual(len(json.loads((root/'selected.json').read_text())['tests']),4)
            configure('OFF')
            result,_=select('--list')
            self.assertEqual(result.returncode,2)
            self.assertIn('rerun CMake',result.stderr)


if __name__=='__main__':
    unittest.main()
