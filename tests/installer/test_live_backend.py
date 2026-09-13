from __future__ import annotations
import tempfile, unittest
from pathlib import Path
import os


from . import _bootstrap
from tests.installer.test_native_build import FakeBuildRunner, make_paths, make_snapshot, make_source
from realmheart_maintenance.manifest import load_manifest
from realmheart_installer.context import InstallContext
from realmheart_installer.live import LiveMutationBackend
from realmheart_installer.native_build import NativeBuildExecutor
from realmheart_installer.planning.planner import InstallationPlanner


class Phase16LiveBackendTests(unittest.TestCase):
    def _fixture(self, root: Path):
        paths=make_paths(root); source=make_source(root); registry=load_manifest(source/"components")
        runner=FakeBuildRunner(); snap=make_snapshot(paths,source,registry)
        plan=InstallationPlanner(paths=paths,source_root=source,snapshot=snap,registry=registry,runner=runner,transaction_id="RH-LIVE-TEST",prefix=root/"prefix",sysconf=root/"etc").build()
        runner.plan=plan
        report=NativeBuildExecutor(plan=plan,source_root=source,registry=registry,runner=runner,installer_cache=paths.installer_cache).run()
        self.assertTrue(report.ok,report.blockers)
        context=InstallContext.create(paths=paths,source_root=source,transaction_id=plan.transaction_id)
        backend=LiveMutationBackend(plan=plan,build_report=report,context=context,paths=paths,source_root=source,runner=runner,allow_unprivileged_system_commit=True,activate_user_services=False)
        return paths,source,registry,plan,report,context,backend

    def test_fake_prefix_artifact_commit_and_exact_rollback(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); *_,plan,report,context,backend=self._fixture(root)
            action=next(a for a in plan.artifact_actions if a.artifact_id=="core.binary")
            staged=next(a for a in report.artifacts if a.artifact_id=="core.binary")
            target=Path(action.target); target.parent.mkdir(parents=True,exist_ok=True); target.write_text("previous\n"); target.chmod(0o755)
            # Planning fingerprint was absence; use a fresh target instead to
            # prove the exact create/remove path.
            target.unlink()
            result=backend.commit_artifacts(next(c for c in plan.components if c.id=="realmheart-core"),(action,))
            self.assertTrue(result.ok,result.reason); self.assertTrue(target.is_file())
            rb=backend.rollback_component(next(c for c in plan.components if c.id=="realmheart-core"),result.operation_ids,())
            self.assertTrue(rb.ok,rb.reason); self.assertFalse(target.exists())


    def test_secure_privileged_parent_check_rejects_world_writable_directory(self):
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/"unsafe"
            path.mkdir()
            path.chmod(0o777)
            with self.assertRaises(Exception) as caught:
                LiveMutationBackend._assert_secure_privileged_directory(path, expected_uid=os.getuid())
            self.assertEqual(getattr(caught.exception, "code", None), "RH_PRIVILEGED_PARENT_UNSAFE")

    def test_fresh_directory_artifact_removes_created_parent_chain_on_rollback(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); *_,plan,report,context,backend=self._fixture(root)
            action=next(a for a in plan.artifact_actions if a.artifact_id=="core.assets")
            component=next(c for c in plan.components if c.id=="realmheart-core")
            target=Path(action.target)
            self.assertFalse(target.parent.exists())
            result=backend.commit_artifacts(component,(action,))
            self.assertTrue(result.ok,result.reason)
            self.assertTrue(target.is_dir())
            rb=backend.rollback_component(component,result.operation_ids,())
            self.assertTrue(rb.ok,rb.reason)
            self.assertFalse(target.exists())
            self.assertFalse((root/"prefix"/"share").exists())
            self.assertFalse((root/"prefix").exists())

    def test_production_system_target_cannot_use_unprivileged_test_escape_hatch(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); paths,source,registry,plan,report,context,backend=self._fixture(root)
            self.assertFalse(backend._test_system_target(Path("/usr/local/bin/realmheart")))
            self.assertFalse(backend._test_system_target(Path("/etc/pam.d/realmheart-lockscreen")))

if __name__ == "__main__": unittest.main()
