"""Do not accept a universal package whose Intel dependencies drop High Sierra."""
import unittest
from scripts.cross.macos_targets import validate_deployment


class MacDeploymentTests(unittest.TestCase):
    def test_per_architecture_minimums(self):
        self.assertEqual(validate_deployment(0x1000007, [0x000a0d00], 'Intel'), '10.13.0')
        self.assertEqual(validate_deployment(0x100000c, [0x000b0000], 'Apple Silicon'), '11.0.0')

    def test_newer_intel_dependency_is_rejected_even_if_arm64_allows_it(self):
        for minimum in (0x000a0d01, 0x000a0e00, 0x000b0000):
            with self.subTest(minimum=minimum), self.assertRaises(ValueError):
                validate_deployment(0x1000007, [minimum], 'dependency.dylib')
        with self.assertRaises(ValueError):
            validate_deployment(0x100000c, [0x000c0000], 'dependency.dylib')

    def test_unknown_or_ambiguous_deployment_is_rejected(self):
        for minima in ([], [0x000a0d00, 0x000b0000]):
            with self.subTest(minima=minima), self.assertRaises(ValueError):
                validate_deployment(0x1000007, minima, 'dependency.dylib')
        with self.assertRaises(ValueError):
            validate_deployment(0x1000000, [0x000a0d00], 'unknown CPU')
