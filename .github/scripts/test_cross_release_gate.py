"""Publishing cannot substitute branch/PR qualification for the exact release tag."""
import json
import unittest
from unittest.mock import patch
import require_cross_qualification as gate


class CrossReleaseGate(unittest.TestCase):
    def candidate(self, **changes):
        return dict(dict(head_sha='commit', event='push', head_branch='v0.99.1', status='completed', conclusion='success'), **changes)

    def check(self, runs):
        with patch.object(gate.subprocess, 'check_output', return_value=json.dumps(dict(workflow_runs=runs))), patch.object(gate.time, 'sleep'):
            gate.require('example/project', 'commit', 'v0.99.1')

    def test_accepts_only_exact_tag_success(self):
        self.check([self.candidate()])
        for changes in (dict(head_sha='other'), dict(event='pull_request'), dict(head_branch='develop')):
            with self.subTest(changes=changes), self.assertRaisesRegex(RuntimeError, 'Timed out'):
                self.check([self.candidate(**changes)])

    def test_failure_and_pending_runs_do_not_publish(self):
        with self.assertRaisesRegex(RuntimeError, 'failed'):
            self.check([self.candidate(conclusion='failure')])
        with self.assertRaisesRegex(RuntimeError, 'Timed out'):
            self.check([self.candidate(status='in_progress', conclusion=None)])
