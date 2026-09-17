"""Synthetic privacy fixtures; no real credentials are used."""
import unittest

from realmheart_doctor.redaction import sanitize_text


class RedactionTests(unittest.TestCase):
    def test_redacted_marker_does_not_hide_other_secrets_on_line(self):
        from realmheart_doctor.redaction import scan_secrets

        self.assertIn("provider_token", scan_secrets("token=[REDACTED] ghp_" + "x" * 36))
        self.assertEqual(scan_secrets("token=[REDACTED]\nbackend unavailable"), ())

    def test_residual_scanner_reports_categories_not_values(self):
        from realmheart_doctor.redaction import scan_secrets

        findings = scan_secrets("-----BEGIN PRIVATE KEY-----\nsynthetic\n-----END PRIVATE KEY-----")
        self.assertEqual(findings, ("private_key",))
        self.assertEqual(scan_secrets("token=[REDACTED]"), ())

    def test_sanitization_is_idempotent_and_preserves_versions(self):
        text = 'password="synthetic value" 0.7.8 gtk4-layer-shell 1.2.0-1'
        clean = sanitize_text(text)
        self.assertEqual(sanitize_text(clean), clean)
        self.assertIn("gtk4-layer-shell 1.2.0-1", clean)

    def test_connection_strings_and_private_keys_are_removed(self):
        source = ('postgresql://fixture:synthetic@db.invalid/app\n'
                  '-----BEGIN PRIVATE KEY-----\nsynthetic\n-----END PRIVATE KEY-----')
        clean = sanitize_text(source)
        self.assertNotIn("synthetic", clean)
        self.assertNotIn("fixture:", clean)

    def test_private_report_never_exposes_credentials(self):
        from dataclasses import replace
        from .test_doctor_state import _diagnosis
        from realmheart_doctor.diagnosis import ComponentHealth
        from realmheart_doctor.reports import render_report

        diagnosis = _diagnosis(ComponentHealth.FAILED)
        component = diagnosis.components[0]
        check = replace(component.checks[0], detail="password=synthetic-only")
        diagnosis = replace(diagnosis, components=(replace(component, checks=(check,)),))
        self.assertNotIn("synthetic-only", render_report(diagnosis, include_private=True)["text"])

    def test_public_report_preserves_check_with_home_path(self):
        from dataclasses import replace
        from .test_doctor_state import _diagnosis
        from realmheart_doctor.diagnosis import ComponentHealth
        from realmheart_doctor.reports import render_report

        diagnosis = _diagnosis(ComponentHealth.FAILED)
        component = diagnosis.components[0]
        check = replace(component.checks[0], detail="missing /home/fixture/file")
        diagnosis = replace(diagnosis, components=(replace(component, checks=(check,)),))
        self.assertIn("check.demo: fail", render_report(diagnosis)["text"])
        self.assertNotIn("/home/fixture", render_report(diagnosis)["text"])

    def test_scanner_failure_blocks_export(self):
        from unittest.mock import patch
        from .test_doctor_state import _diagnosis
        from realmheart_doctor.diagnosis import ComponentHealth
        from realmheart_doctor.reports import render_report

        with patch("realmheart_doctor.reports.scan_secrets", return_value=("private_key",)):
            report = render_report(_diagnosis(ComponentHealth.FAILED))
        self.assertFalse(report["export_allowed"])
        self.assertEqual(report["warnings"], ["private_key"])
        self.assertEqual(report["text"], "Report withheld: sensitive content requires local review.")


    def test_identity_network_and_credentials_golden(self):
        source = ('/home/alice/work alice on workstation\n'
                  '192.168.1.15 [2001:db8::1] fe80::1%eth0\n'
                  'SSID="Test Network"\nAuthorization: Bearer fixture-only\n'
                  'api_key="synthetic value"\npassword=fixture-only\n'
                  'ghp_' + 'x' * 36)
        expected = ('~/work [REDACTED] on [REDACTED]\n'
                    '[REDACTED] [[REDACTED]] [REDACTED]\n'
                    'SSID=[REDACTED]\nAuthorization: [REDACTED]\n'
                    'api_key=[REDACTED]\npassword=[REDACTED]\n[REDACTED]')
        self.assertEqual(sanitize_text(source, username="alice", hostname="workstation",
                                       home="/home/alice"), expected)
