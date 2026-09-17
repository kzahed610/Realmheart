"""Pacman transaction correlation is evidence, never proof."""
import unittest

from realmheart_doctor.packages import correlate_package_changes


class DoctorPackageTests(unittest.TestCase):
    def test_relevant_upgrade_is_extracted_with_versions(self):
        log = (
            "[2026-09-17T11:00:00+0000] [ALPM] upgraded gtk4-layer-shell (1.1.0-1 -> 1.2.0-1)\n"
            "[2026-09-17T11:00:01+0000] [ALPM] upgraded firefox (140.0 -> 141.0)\n"
        )
        changes = correlate_package_changes(log, {"gtk4-layer-shell"},
                                            window=(1789642800, 1789646400))
        self.assertEqual(len(changes), 1)
        self.assertEqual(changes[0]["package"], "gtk4-layer-shell")
        self.assertEqual(changes[0]["previous"], "1.1.0-1")
        self.assertEqual(changes[0]["current"], "1.2.0-1")
        self.assertFalse(changes[0]["proves_causation"])

    def test_unrelated_packages_never_surface(self):
        log = "[2026-09-17T11:00:00+0000] [ALPM] upgraded firefox (140.0 -> 141.0)\n"
        self.assertEqual(correlate_package_changes(log, {"gtk4-layer-shell"},
                                                  window=(1789642800, 1789646400)), [])

    def test_malformed_lines_do_not_crash_correlation(self):
        log = "not a transaction line\n[2026-09-17T11:00:00+0000] [ALPM] upgraded (x -> y)\n"
        self.assertEqual(correlate_package_changes(log, {"anything"},
                                                   window=(1737098400, 1737100800)), [])


if __name__ == "__main__":
    unittest.main()
