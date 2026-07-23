#!/usr/bin/env python3

import csv
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPORT_PATH = SCRIPT_DIR / "report.py"
SPEC = importlib.util.spec_from_file_location("xds_report", REPORT_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load {REPORT_PATH}")
REPORT = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = REPORT
SPEC.loader.exec_module(REPORT)


def write_identity(directory: Path, variant: str) -> None:
    kasan = variant == "kasan"
    config = "CONFIG_KASAN=y" if kasan else "# CONFIG_KASAN is not set"
    contents = (
        f"UNAME=Linux test 6.6.0-xds-{variant} #1 SMP\n"
        f"VERSION=6.6.0-xds-{variant}\n"
        f"VARIANT={variant}\n"
        f"KASAN_ENABLED={'true' if kasan else 'false'}\n"
        f"KASAN_CONFIG={config}\n"
        f"KASAN_MODE={'generic' if kasan else 'none'}\n"
    )
    (directory / "kernel.info").write_text(
        contents, encoding="utf-8"
    )


def stress_output(api: str) -> str:
    lines = []
    for iteration in range(REPORT.STRESS_ITERATIONS):
        for worker in range(REPORT.STRESS_WORKERS):
            case_id = f"stress-i{iteration:04d}-w{worker:02d}"
            crc = (iteration << 16) | worker
            lines.append(
                f"api={api} case={case_id} va=0x0 ret=0 expected=0"
            )
            lines.append(
                f"crc-pass case={case_id} crc32=0x{crc:08x}"
            )
    return "\n".join(lines) + "\n"


def create_complete_fixture(base: Path) -> None:
    for variant in REPORT.EXPECTED_VARIANTS:
        directory = base / variant
        directory.mkdir()
        write_identity(directory, variant)
        (directory / "basic.c.single.out").write_text(
            "api=c case=basic ret=0 expected=0\n"
            "crc-pass case=basic crc32=0x12345678\n",
            encoding="utf-8",
        )
        (directory / "basic.c.single.dmesg").write_text(
            "", encoding="utf-8"
        )
        for api in REPORT.STRESS_APIS:
            for memory_mode in REPORT.STRESS_MEMORY_MODES:
                suffix = api
                topology = "direct-nsid-1"
                if memory_mode == "registered":
                    suffix += "-registered"
                    topology += "-registered"
                for operation in ("read", "write", "mixed"):
                    label = f"{topology}.{suffix}.rw-{operation}"
                    output = (
                        f"api={api} case={operation} ret=0 expected=0\n"
                    )
                    if operation != "write":
                        output += (
                            f"crc-pass case={operation} "
                            "crc32=0x12345678\n"
                        )
                    (directory / f"{label}.out").write_text(
                        output, encoding="utf-8"
                    )
                    (directory / f"{label}.dmesg").write_text(
                        "", encoding="utf-8"
                    )
        rejection_cases = {
            "c": (
                ("unknown operation", -22),
                ("unknown flags", -95),
                ("regular file write", -95),
                ("read-only block fd", -9),
                ("insufficient extent coverage", -7),
                ("out-of-capacity range", -27),
                ("userspace unknown operation", -22),
                ("userspace regular file write", -95),
            ),
            "python": (
                ("unknown operation", -22),
                ("regular file write", -95),
            ),
        }
        for api, cases in rejection_cases.items():
            label = f"rw-rejection.{api}.rejection"
            output = "".join(
                f"api={api} case={case} ret={result} "
                f"expected={result}\n"
                for case, result in cases
            )
            (directory / f"{label}.out").write_text(
                output, encoding="utf-8"
            )
            (directory / f"{label}.dmesg").write_text(
                "", encoding="utf-8"
            )
        for topology in REPORT.STRESS_TOPOLOGIES:
            for api in REPORT.STRESS_APIS:
                for memory_mode in REPORT.STRESS_MEMORY_MODES:
                    label = (
                        f"stress-{topology}.{api}.{memory_mode}"
                    )
                    (directory / f"{label}.out").write_text(
                        stress_output(api), encoding="utf-8"
                    )
                    (directory / f"{label}.dmesg").write_text(
                        "", encoding="utf-8"
                    )


class ReportTest(unittest.TestCase):
    def test_complete_dual_kernel_report(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            create_complete_fixture(base)
            json_path = base / "report.json"
            markdown_path = base / "report.md"
            tsv_path = base / "cases.tsv"

            result = subprocess.run(
                [
                    sys.executable,
                    str(REPORT_PATH),
                    "--artifact-dir",
                    str(base),
                    "--json-report",
                    str(json_path),
                    "--md-report",
                    str(markdown_path),
                    "--cases-tsv",
                    str(tsv_path),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(json_path.read_text(encoding="utf-8"))
            expected_per_kernel = (
                1
                + len(REPORT.STRESS_APIS)
                * len(REPORT.STRESS_MEMORY_MODES)
                * 3
                + 10
                + len(REPORT.expected_stress_labels())
                * REPORT.STRESS_ITERATIONS
                * REPORT.STRESS_WORKERS
            )
            expected_crc_per_kernel = (
                1
                + len(REPORT.STRESS_APIS)
                * len(REPORT.STRESS_MEMORY_MODES)
                * 2
                + len(REPORT.expected_stress_labels())
                * REPORT.STRESS_ITERATIONS
                * REPORT.STRESS_WORKERS
            )
            self.assertEqual(
                report["summary"]["total_cases"],
                expected_per_kernel * 2,
            )
            self.assertEqual(
                report["summary"]["crc_verified"],
                expected_crc_per_kernel * 2,
            )
            self.assertEqual(report["validation_errors"], [])

            with tsv_path.open(newline="", encoding="utf-8") as stream:
                rows = list(csv.DictReader(stream, delimiter="\t"))
            self.assertEqual(len(rows), expected_per_kernel * 2)
            self.assertEqual(
                {row["kernel_variant"] for row in rows},
                REPORT.EXPECTED_VARIANTS,
            )
            crc_rows = [
                row for row in rows if row["crc_match"]
            ]
            self.assertTrue(
                all(row["crc_match"] == "OK" for row in crc_rows)
            )
            basic_rows = [
                row for row in rows if row["source_case_id"] == "basic"
            ]
            self.assertTrue(
                all(row["memory_mode"] == "normal" for row in basic_rows)
            )
            self.assertTrue(
                all(row["io_mode"] == "single" for row in basic_rows)
            )
            markdown = markdown_path.read_text(encoding="utf-8")
            self.assertIn("## kasan/stress-dm.c.normal", markdown)
            self.assertIn("stress-i0015-w15", markdown)

    def test_duplicate_case_lines_are_not_dropped(self) -> None:
        cases = REPORT.parse_cases(
            "api=c case=registered read ret=0 expected=0\n"
            "api=c case=registered read ret=-2 expected=-2\n"
        )
        self.assertEqual(
            [case.case_id for case in cases],
            ["registered read", "registered read#2"],
        )

    def test_registered_memory_dimensions(self) -> None:
        self.assertEqual(
            REPORT.suite_dimensions("mem-registration.c.registered"),
            ("mem-registration", "c", "registered", "mixed"),
        )
        self.assertEqual(
            REPORT.suite_dimensions("registered-mem.python.queued"),
            ("registered-mem", "python", "registered", "queued"),
        )
        self.assertEqual(
            REPORT.suite_dimensions("stress-raid0.c.registered"),
            ("stress-raid0", "c", "registered", "stress"),
        )
        self.assertEqual(
            REPORT.suite_dimensions(
                "direct-nsid-1-registered.c-registered.rw-write"
            ),
            (
                "direct-nsid-1-registered",
                "c",
                "registered",
                "rw-write",
            ),
        )

    def test_missing_kernel_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            directory = base / "nokasan"
            directory.mkdir()
            write_identity(directory, "nokasan")
            (directory / "basic.c.single.out").write_text(
                "api=c case=basic ret=0 expected=0\n",
                encoding="utf-8",
            )
            report = REPORT.build_report(base)
            self.assertTrue(report.validation_errors)
            self.assertTrue(
                any(
                    "expected kernel variants" in error
                    for error in report.validation_errors
                )
            )

    def test_empty_suite_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            for variant in REPORT.EXPECTED_VARIANTS:
                directory = base / variant
                directory.mkdir()
                write_identity(directory, variant)
                (directory / "empty.c.single.out").write_text(
                    "", encoding="utf-8"
                )
            report = REPORT.build_report(base)
            self.assertTrue(
                any(
                    "suite contains no cases" in error
                    for error in report.validation_errors
                )
            )


if __name__ == "__main__":
    unittest.main()
