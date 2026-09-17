"""Privacy-filtered reports keep diagnosis usable without leaking host detail."""
import unittest

from .test_doctor_state import _diagnosis
from realmheart_doctor.diagnosis import ComponentHealth
from realmheart_doctor.reports import render_report


class DoctorReportTests(unittest.TestCase):
    def test_public_report_redacts_ip_in_check_detail(self):
        from dataclasses import replace

        diagnosis = _diagnosis(ComponentHealth.FAILED)
        component = diagnosis.components[0]
        check = replace(component.checks[0], detail="backend unavailable at 192.168.1.15")
        diagnosis = replace(diagnosis, components=(replace(component, checks=(check,)),))
        text = render_report(diagnosis)["text"]
        self.assertNotIn("192.168.1.15", text)
        self.assertIn("backend unavailable", text)
        self.assertIn("[REDACTED]", text)

    def test_public_report_excludes_local_paths_and_raw_output(self):
        report = render_report(_diagnosis(ComponentHealth.FAILED), include_private=False)
        text = report["text"]
        self.assertNotIn("/home/", text)
        self.assertNotIn("stdout", text)
        self.assertNotIn("stderr", text)
        self.assertIn("demo", text)
        self.assertIn("failed", text)

    def test_private_report_is_explicitly_requested_and_marked(self):
        report = render_report(_diagnosis(ComponentHealth.FAILED), include_private=True)
        self.assertTrue(report["private"])
        self.assertFalse(render_report(_diagnosis(ComponentHealth.FAILED), include_private=False)["private"])

    def test_unknown_results_stay_unknown_in_reports(self):
        report = render_report(_diagnosis(ComponentHealth.UNKNOWN), include_private=False)
        self.assertIn("unknown", report["text"])
