#!/usr/bin/env python3
"""Exercise basic matrix selection without devices, mounts or kernel modules."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


RUNNER = Path(__file__).with_name("basic_test.sh")
BLOCK_LABELS = (
    "direct-nsid-1", "direct-nsid-2", "nvme-part", "linear", "linear-part",
    "raid0", "raid0-slot-order", "raid0-part", "raid0-same-ns",
)
FS_LABELS = ("direct", "nvme-part", "linear", "linear-part", "raid0", "raid0-part")
SCRIPT = r'''
source "$1"
validate_basic_profile
WORK_DIR=$2
MOUNT_DIR=$2/mnt
blockdev() { printf '67108864\n'; }
generate_pattern() { :; }
sync() { :; }
run_write_pipeline() { printf 'WRITE %s %s\n' "$2" "$5"; }
run_manifest() { printf 'READ %s %s %s\n' "$5" "$1" "$3"; }
for label in direct-nsid-1 direct-nsid-2 nvme-part linear linear-part raid0 raid0-slot-order raid0-part raid0-same-ns; do
    case $label in
        direct-nsid-1 | direct-nsid-2 | nvme-part) boundary=0 ;;
        linear) boundary=$((63 * 1024 * 1024)) ;;
        linear-part) boundary=$((59 * 1024 * 1024)) ;;
        *) boundary=65536 ;;
    esac
    run_block_matrix /mock/device "$label" 100 "$boundary"
done
for label in direct nvme-part linear linear-part raid0 raid0-part; do
    for size in $(basic_fs_sizes "$label"); do
        run_filesystem_matrix /mock/device "$label-ext4-$size" "$size" 100
    done
done
'''


def collect(profile: str):
    with tempfile.TemporaryDirectory() as tmp:
        result = subprocess.run(
            ["bash", "-c", SCRIPT, "test", str(RUNNER), tmp],
            env=dict(os.environ, XDS_BASIC_PROFILE=profile),
            text=True, capture_output=True, check=True,
        )
        manifests = {
            p.name: p.read_text().replace(tmp, "/work")
            for p in Path(tmp).glob("*.tsv")
        }
        writes = {line for line in result.stdout.splitlines() if line.startswith("WRITE ")}
        return manifests, writes


class BasicProfileTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.quick, cls.quick_writes = collect("quick")
        cls.full, cls.full_writes = collect("full")

    def test_full_preserves_exhaustive_matrix(self):
        expected = {
            f"{label}.{api}.single.tsv"
            for label in BLOCK_LABELS for api in ("c", "python", "nds-c", "nds-python")
        } | {
            f"{label}-ext4-{size}.{api}.{mode}.tsv"
            for label in FS_LABELS for size in (1024, 2048, 4096)
            for api in ("c", "python", "nds-c", "nds-python")
            for mode in ("single", "queued", "threaded")
        }
        self.assertEqual(set(self.full), expected)
        self.assertEqual(len(self.full_writes), 18)
        self.assertEqual(sum(len(v.splitlines()) for v in self.full.values()), 1320)

    def test_quick_preserves_topologies_and_api_families(self):
        for label in BLOCK_LABELS:
            for api in ("c", "nds-c"):
                self.assertIn(f"{label}.{api}.single.tsv", self.quick)
        for label in FS_LABELS:
            for api in ("c", "nds-c"):
                self.assertIn(f"{label}-ext4-4096.{api}.single.tsv", self.quick)
        for size in (1024, 2048):
            for api in ("c", "nds-c"):
                self.assertIn(f"direct-ext4-{size}.{api}.single.tsv", self.quick)
        for api in ("c", "python", "nds-c", "nds-python"):
            for label in ("direct-nsid-1", "raid0"):
                self.assertIn(f"{label}.{api}.single.tsv", self.quick)
            for mode in ("single", "queued", "threaded"):
                self.assertIn(f"direct-ext4-4096.{api}.{mode}.tsv", self.quick)
        self.assertEqual(len(self.quick), 48)
        self.assertEqual(len(self.quick_writes), 11)
        self.assertEqual(sum(len(v.splitlines()) for v in self.quick.values()), 304)

    def test_kept_manifests_preserve_every_boundary_case(self):
        for name, contents in self.quick.items():
            with self.subTest(name=name):
                self.assertEqual(contents, self.full[name])
        self.assertTrue(self.quick_writes <= self.full_writes)

    def test_bad_profile_fails_before_preflight(self):
        result = subprocess.run(
            ["bash", "-c", 'source "$1"; preflight() { echo BAD; }; main',
             "test", str(RUNNER)],
            env=dict(os.environ, XDS_BASIC_PROFILE="invalid"),
            text=True, capture_output=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("XDS_BASIC_PROFILE must be quick or full", result.stderr)
        self.assertNotIn("BAD", result.stdout)


if __name__ == "__main__":
    unittest.main()
