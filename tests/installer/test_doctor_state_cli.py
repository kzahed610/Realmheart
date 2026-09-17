"""Fresh-process CLI regression using only an isolated installation."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from realmheart_maintenance.manifest import load_manifest


class DoctorStateCLITests(unittest.TestCase):
    def test_failure_repeat_and_verified_recovery(self):
        repo = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory(prefix='doctor-state-cli-') as temp:
            root = Path(temp)
            manifests = root / 'manifests'
            manifests.mkdir()
            (manifests / 'demo.toml').write_text('''schema_version = 1
        release_version = "0.7.8"
        [[components]]
        id = "demo"
        name = "Demo"
        category = "core"
        stage = "core_shell"
        component_version = "0.7.8"
        [[artifacts]]
        id = "artifact.demo"
        component_id = "demo"
        type = "executable"
        ownership = "release"
        managed = true
        path = "$PREFIX/bin/demo"
        required = true
        [[health_checks]]
        id = "check.demo"
        component_id = "demo"
        artifact_id = "artifact.demo"
        check = "artifact_exists"
        contexts = ["doctor_manual"]
        ''')
            from realmheart_maintenance.manifest import load_manifest
            load_manifest(manifests)
            prefix = root / 'prefix'
            (prefix / 'bin').mkdir(parents=True)
            state = root / 'state'
            command = [sys.executable, '-B', str(repo / 'tools/realmheart-doctor.py'),
                       'doctor', '--manifest-dir', str(manifests), '--prefix', str(prefix),
                       '--state-dir', str(state), '--json']
            def run(expected):
                result = subprocess.run(command, cwd=repo, capture_output=True, text=True, timeout=30)
                if result.returncode != expected:
                    raise AssertionError((result.returncode, result.stdout, result.stderr))
                assert not result.stderr, result.stderr
                return json.loads(result.stdout)
            first = run(2)
            second = run(2)
            assert first['state']['incident_ids'] == second['state']['incident_ids']
            incident_id = first['state']['incident_ids'][0]
            incident_path = state / 'incidents' / (incident_id + '.json')
            assert len(json.loads(incident_path.read_text())['timeline']) == 2
            executable = prefix / 'bin/demo'
            executable.write_text('#!/bin/sh\nexit 0\n')
            executable.chmod(0o755)
            healthy = run(0)
            assert healthy['overall'] == 'healthy'
            assert json.loads(incident_path.read_text())['resolution_state'] == 'resolved'
            assert (state / 'components/demo/last-healthy.json').is_file()
