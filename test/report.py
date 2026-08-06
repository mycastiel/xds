#!/usr/bin/env python3
"""Generate and validate detailed XDS dual-kernel test reports."""

import argparse
import csv
import json
import re
import sys
from collections import Counter, defaultdict, deque
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Deque, Dict, Iterable, List, Optional, Tuple


CASE_RE = re.compile(
    r"api=(\S+)\s+case=(.+?)(?:\s+va=0x[0-9a-fA-F]+)?\s+"
    r"ret=(-?\d+)\s+expected=(-?\d+)"
)
CRC_PASS_RE = re.compile(
    r"crc-pass case=(\S+) crc32=0x([0-9a-fA-F]{8})"
)
KERNEL_BAD_RE = re.compile(
    r"(BUG:\s*KASAN|KASAN:|KernelAddressSanitizer:|BUG:|WARNING:|"
    r"Oops|kernel panic|use-after-free|refcount_t|suspicious RCU|"
    r"RCU stall|rcu_preempt detected stalls)",
    re.IGNORECASE,
)
EXPECTED_VARIANTS = {"kasan", "nokasan"}
STRESS_APIS = ("c", "python", "nds-c", "nds-python")
STRESS_MEMORY_MODES = ("normal", "registered")
STRESS_TOPOLOGIES = ("dm", "nvme", "raid0")
STRESS_ITERATIONS = 16
STRESS_WORKERS = 16


@dataclass
class KernelIdentity:
    uname: str = ""
    version: str = ""
    variant: str = ""
    kasan_enabled: bool = False
    kasan_config_source: str = ""
    kasan_mode: str = "none"
    cmdline: str = ""

    @property
    def kasan_config_enabled(self) -> bool:
        return "CONFIG_KASAN=y" in self.kasan_config_source


@dataclass
class TestCaseResult:
    case_id: str
    source_case_id: str
    occurrence: int
    suite: str
    topology: str
    api: str
    memory_mode: str
    io_mode: str
    expected: int
    actual: int
    passed: bool
    crc_match: Optional[bool] = None
    crc_expected: Optional[int] = None
    crc_actual: Optional[int] = None
    kernel_variant: str = ""
    kernel_version: str = ""
    kasan_enabled: bool = False


@dataclass
class KernelIssue:
    line: str
    context: str


@dataclass
class SuiteReport:
    name: str
    label: str
    kernel: KernelIdentity
    cases: List[TestCaseResult] = field(default_factory=list)
    kernel_issues: List[KernelIssue] = field(default_factory=list)
    status: str = "UNKNOWN"
    output_path: str = ""
    dmesg_path: str = ""


@dataclass
class FullReport:
    run_id: str
    kernel_variants: List[KernelIdentity] = field(default_factory=list)
    suites: List[SuiteReport] = field(default_factory=list)
    validation_errors: List[str] = field(default_factory=list)
    summary: Dict[str, Any] = field(default_factory=dict)


def parse_cases(text: str) -> List[TestCaseResult]:
    cases: List[TestCaseResult] = []
    occurrences: Counter[str] = Counter()

    for match in CASE_RE.finditer(text):
        source_case_id = match.group(2)
        occurrences[source_case_id] += 1
        occurrence = occurrences[source_case_id]
        case_id = source_case_id
        if occurrence > 1:
            case_id = f"{source_case_id}#{occurrence}"
        actual = int(match.group(3))
        expected = int(match.group(4))
        cases.append(
            TestCaseResult(
                case_id=case_id,
                source_case_id=source_case_id,
                occurrence=occurrence,
                suite="",
                topology="",
                api=match.group(1),
                memory_mode="",
                io_mode="",
                expected=expected,
                actual=actual,
                passed=actual == expected,
            )
        )
    return cases


def parse_crc_results(text: str) -> Dict[str, Deque[int]]:
    results: Dict[str, Deque[int]] = defaultdict(deque)
    for match in CRC_PASS_RE.finditer(text):
        results[match.group(1)].append(int(match.group(2), 16))
    return results


def scan_kernel_log(text: str) -> List[KernelIssue]:
    lines = text.splitlines()
    issues: List[KernelIssue] = []

    for index, line in enumerate(lines):
        if not KERNEL_BAD_RE.search(line):
            continue
        first = max(0, index - 3)
        last = min(len(lines), index + 8)
        issues.append(
            KernelIssue(
                line=line.strip(),
                context="\n".join(lines[first:last]),
            )
        )
    return issues


def parse_kernel_identity(path: Path) -> KernelIdentity:
    identity = KernelIdentity()

    for line in path.read_text(
        encoding="utf-8", errors="replace"
    ).splitlines():
        if line.startswith("UNAME="):
            identity.uname = line.removeprefix("UNAME=")
        elif line.startswith("VERSION="):
            identity.version = line.removeprefix("VERSION=")
        elif line.startswith("VARIANT="):
            identity.variant = line.removeprefix("VARIANT=")
        elif line.startswith("KASAN_ENABLED="):
            value = line.removeprefix("KASAN_ENABLED=")
            identity.kasan_enabled = value.strip().lower() == "true"
        elif line.startswith("KASAN_CONFIG="):
            identity.kasan_config_source = line.removeprefix(
                "KASAN_CONFIG="
            )
        elif line.startswith("KASAN_MODE="):
            identity.kasan_mode = line.removeprefix("KASAN_MODE=")
        elif line.startswith("CMDLINE="):
            identity.cmdline = line.removeprefix("CMDLINE=")

    if not identity.variant:
        if "xds-nokasan" in identity.version:
            identity.variant = "nokasan"
        elif "xds-kasan" in identity.version:
            identity.variant = "kasan"
    return identity


def suite_dimensions(label: str) -> Tuple[str, str, str, str]:
    parts = label.split(".")
    topology = parts[0]
    api = parts[-2] if len(parts) >= 3 else "unknown"
    io_mode = parts[-1] if len(parts) >= 2 else "unknown"
    registered_api = api.endswith("-registered")
    if registered_api:
        api = api.removesuffix("-registered")
    if label.startswith("stress-"):
        memory_mode = io_mode
        io_mode = "stress"
    elif label.startswith("mem-registration."):
        memory_mode = "registered"
        io_mode = "mixed"
    elif label.startswith("registered-mem."):
        memory_mode = "registered"
    elif registered_api:
        memory_mode = "registered"
    else:
        memory_mode = "normal"
    return topology, api, memory_mode, io_mode


def process_artifact_dir(
    artifact_dir: Path, identity: KernelIdentity
) -> Tuple[List[SuiteReport], List[str]]:
    suites: List[SuiteReport] = []
    errors: List[str] = []
    dmesg_by_label = {
        path.stem: path for path in artifact_dir.glob("*.dmesg")
    }

    for output_path in sorted(artifact_dir.glob("*.out")):
        label = output_path.stem
        topology, api, memory_mode, io_mode = suite_dimensions(label)
        text = output_path.read_text(
            encoding="utf-8", errors="replace"
        )
        cases = parse_cases(text)
        crc_results = parse_crc_results(text)

        for case in cases:
            case.suite = label
            case.topology = topology
            case.api = api
            case.memory_mode = memory_mode
            case.io_mode = io_mode
            case.kernel_variant = identity.variant
            case.kernel_version = identity.version
            case.kasan_enabled = identity.kasan_enabled
            values = crc_results.get(case.source_case_id)
            if values:
                value = values.popleft()
                case.crc_match = True
                case.crc_expected = value
                case.crc_actual = value

        unused_crc = sum(len(values) for values in crc_results.values())
        if unused_crc:
            errors.append(
                f"{identity.variant}/{label}: {unused_crc} CRC results "
                "have no matching case"
            )

        dmesg_path = dmesg_by_label.get(label)
        kernel_issues: List[KernelIssue] = []
        if dmesg_path:
            kernel_issues = scan_kernel_log(
                dmesg_path.read_text(
                    encoding="utf-8", errors="replace"
                )
            )
        else:
            errors.append(
                f"{identity.variant}/{label}: kernel log is missing"
            )
        suite = SuiteReport(
            name=f"{identity.variant}/{label}",
            label=label,
            kernel=identity,
            cases=cases,
            kernel_issues=kernel_issues,
            status=(
                "PASS"
                if cases and all(case.passed for case in cases)
                else "FAIL"
            ),
            output_path=str(output_path),
            dmesg_path=str(dmesg_path) if dmesg_path else "",
        )
        suites.append(suite)
    return suites, errors


def discover_kernel_dirs(base: Path) -> List[Tuple[Path, Path]]:
    result: List[Tuple[Path, Path]] = []

    for child in sorted(base.iterdir()):
        identity_path = child / "kernel.info"
        if child.is_dir() and identity_path.is_file():
            result.append((child, identity_path))
    return result


def expected_stress_labels() -> set[str]:
    return {
        f"stress-{topology}.{api}.{memory_mode}"
        for topology in STRESS_TOPOLOGIES
        for api in STRESS_APIS
        for memory_mode in STRESS_MEMORY_MODES
    }


def expected_stress_case_ids() -> set[str]:
    return {
        f"stress-i{iteration:04d}-w{worker:02d}"
        for iteration in range(STRESS_ITERATIONS)
        for worker in range(STRESS_WORKERS)
    }


def case_signature(case: TestCaseResult) -> Tuple[Any, ...]:
    return (
        case.source_case_id,
        case.occurrence,
        case.api,
        case.memory_mode,
        case.io_mode,
        case.expected,
    )


def validate_kernel_identity(identity: KernelIdentity) -> List[str]:
    errors: List[str] = []
    prefix = identity.variant or identity.version or "unknown"

    if not identity.uname or not identity.version:
        errors.append(f"{prefix}: incomplete kernel identity")
    if identity.variant == "kasan":
        if not identity.kasan_enabled:
            errors.append("kasan: KASAN runtime is not active")
        if not identity.kasan_config_enabled:
            errors.append("kasan: CONFIG_KASAN is not enabled")
    elif identity.variant == "nokasan":
        if identity.kasan_enabled:
            errors.append("nokasan: KASAN runtime is active")
        if identity.kasan_config_enabled:
            errors.append("nokasan: CONFIG_KASAN is enabled")
    return errors


def validate_stress_suites(
    variant: str, suites: Iterable[SuiteReport]
) -> List[str]:
    errors: List[str] = []
    suites_by_label = {
        suite.label: suite
        for suite in suites
        if suite.label.startswith("stress-")
    }
    expected_labels = expected_stress_labels()
    actual_labels = set(suites_by_label)

    if actual_labels != expected_labels:
        errors.append(
            f"{variant}: stress suites differ: "
            f"missing={sorted(expected_labels - actual_labels)} "
            f"extra={sorted(actual_labels - expected_labels)}"
        )
    expected_ids = expected_stress_case_ids()
    for label, suite in suites_by_label.items():
        actual_ids = {case.source_case_id for case in suite.cases}
        if len(suite.cases) != len(expected_ids):
            errors.append(
                f"{variant}/{label}: expected {len(expected_ids)} "
                f"cases, got {len(suite.cases)}"
            )
        if actual_ids != expected_ids:
            errors.append(
                f"{variant}/{label}: stress case IDs do not cover "
                f"{STRESS_ITERATIONS} iterations and {STRESS_WORKERS} workers"
            )
        missing_crc = [
            case.case_id
            for case in suite.cases
            if case.crc_match is not True
        ]
        if missing_crc:
            errors.append(
                f"{variant}/{label}: {len(missing_crc)} cases lack "
                "CRC verification"
            )
    return errors


def validate_report(report: FullReport) -> List[str]:
    errors: List[str] = []
    variants = {item.variant for item in report.kernel_variants}

    if variants != EXPECTED_VARIANTS:
        errors.append(
            f"expected kernel variants {sorted(EXPECTED_VARIANTS)}, "
            f"got {sorted(variants)}"
        )
    if len(report.kernel_variants) != len(variants):
        errors.append("duplicate kernel identity entries")

    suites_by_variant: Dict[str, List[SuiteReport]] = defaultdict(list)
    for identity in report.kernel_variants:
        errors.extend(validate_kernel_identity(identity))
    for suite in report.suites:
        suites_by_variant[suite.kernel.variant].append(suite)
        if not suite.cases:
            errors.append(f"{suite.name}: suite contains no cases")
        if suite.status != "PASS":
            errors.append(f"{suite.name}: suite status is {suite.status}")
        if suite.kernel_issues:
            errors.append(
                f"{suite.name}: {len(suite.kernel_issues)} kernel issues"
            )
        failed = [case for case in suite.cases if not case.passed]
        if failed:
            errors.append(
                f"{suite.name}: {len(failed)} test cases failed"
            )

    for variant in EXPECTED_VARIANTS:
        suites = suites_by_variant.get(variant, [])
        if not suites:
            errors.append(f"{variant}: no suites found")
            continue
        errors.extend(validate_stress_suites(variant, suites))

    if all(suites_by_variant.get(item) for item in EXPECTED_VARIANTS):
        kasan_suites = {
            suite.label: suite for suite in suites_by_variant["kasan"]
        }
        nokasan_suites = {
            suite.label: suite for suite in suites_by_variant["nokasan"]
        }
        if set(kasan_suites) != set(nokasan_suites):
            errors.append("suite labels differ between kernel variants")
        for label in sorted(set(kasan_suites) & set(nokasan_suites)):
            kasan_cases = Counter(
                case_signature(case)
                for case in kasan_suites[label].cases
            )
            nokasan_cases = Counter(
                case_signature(case)
                for case in nokasan_suites[label].cases
            )
            if kasan_cases != nokasan_cases:
                errors.append(
                    f"{label}: case set differs between kernel variants"
                )
    return errors


def serialize(value: Any) -> Any:
    if hasattr(value, "__dataclass_fields__"):
        return {
            key: serialize(item)
            for key, item in asdict(value).items()
        }
    if isinstance(value, Path):
        return str(value)
    return value


def write_json(report: FullReport, path: Path) -> None:
    path.write_text(
        json.dumps(serialize(report), indent=2),
        encoding="utf-8",
    )


def markdown_cell(value: Any) -> str:
    return str(value).replace("|", r"\|").replace("\n", "<br>")


def crc_status(case: TestCaseResult) -> str:
    if case.crc_match is True:
        return "OK"
    if case.crc_match is False:
        return "FAIL"
    return ""


def write_markdown(report: FullReport, path: Path) -> None:
    lines = [
        "# XDS Dual-Kernel Test Report",
        "",
        f"Run ID: `{report.run_id}`",
        "",
        "## Kernel summary",
        "",
        "| Variant | Version | KASAN runtime | KASAN config | Suites | "
        "Cases | CRC verified | Failed |",
        "| --- | --- | --- | --- | ---: | ---: | ---: | ---: |",
    ]

    for identity in sorted(
        report.kernel_variants, key=lambda item: item.variant
    ):
        suites = [
            suite
            for suite in report.suites
            if suite.kernel.variant == identity.variant
        ]
        cases = [case for suite in suites for case in suite.cases]
        crc_count = sum(case.crc_match is True for case in cases)
        failed = sum(not case.passed for case in cases)
        lines.append(
            f"| {identity.variant} | {identity.version} | "
            f"{'active' if identity.kasan_enabled else 'inactive'} | "
            f"{'enabled' if identity.kasan_config_enabled else 'disabled'} | "
            f"{len(suites)} | {len(cases)} | {crc_count} | {failed} |"
        )

    lines.extend(
        [
            "",
            "## Validation",
            "",
        ]
    )
    if report.validation_errors:
        lines.extend(
            f"- FAIL: {markdown_cell(error)}"
            for error in report.validation_errors
        )
    else:
        lines.append("- PASS: complete and consistent dual-kernel matrix")

    for suite in sorted(report.suites, key=lambda item: item.name):
        lines.extend(
            [
                "",
                f"## {suite.name}",
                "",
                f"- Status: `{suite.status}`",
                f"- Kernel: `{suite.kernel.uname}`",
                f"- Output: `{suite.output_path}`",
                f"- Kernel log: `{suite.dmesg_path or 'not captured'}`",
                "",
                "| Case | API | Memory | I/O mode | Expected | Actual | "
                "Result | CRC | CRC expected | CRC actual |",
                "| --- | --- | --- | --- | ---: | ---: | --- | --- | "
                "--- | --- |",
            ]
        )
        for case in suite.cases:
            crc_expected = (
                f"0x{case.crc_expected:08x}"
                if case.crc_expected is not None
                else ""
            )
            crc_actual = (
                f"0x{case.crc_actual:08x}"
                if case.crc_actual is not None
                else ""
            )
            lines.append(
                f"| {markdown_cell(case.case_id)} | {case.api} | "
                f"{case.memory_mode} | {case.io_mode} | {case.expected} | "
                f"{case.actual} | {'PASS' if case.passed else 'FAIL'} | "
                f"{crc_status(case)} | {crc_expected} | {crc_actual} |"
            )

    total = sum(len(suite.cases) for suite in report.suites)
    passed = sum(
        case.passed
        for suite in report.suites
        for case in suite.cases
    )
    lines.extend(
        [
            "",
            "## Grand summary",
            "",
            f"- Total suites: {len(report.suites)}",
            f"- Total cases: {total}",
            f"- Passed: {passed}",
            f"- Failed: {total - passed}",
            "",
        ]
    )
    path.write_text("\n".join(lines), encoding="utf-8")


def write_tsv(report: FullReport, path: Path) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(
            stream, delimiter="\t", lineterminator="\n"
        )
        writer.writerow(
            [
                "case_id",
                "source_case_id",
                "occurrence",
                "suite",
                "topology",
                "api",
                "memory_mode",
                "io_mode",
                "expected",
                "actual",
                "passed",
                "crc_match",
                "crc_expected",
                "crc_actual",
                "kernel_variant",
                "kernel_version",
                "kasan_enabled",
            ]
        )
        cases = [
            case
            for suite in report.suites
            for case in suite.cases
        ]
        for case in sorted(
            cases,
            key=lambda item: (
                item.kernel_variant,
                item.suite,
                item.case_id,
            ),
        ):
            writer.writerow(
                [
                    case.case_id,
                    case.source_case_id,
                    case.occurrence,
                    case.suite,
                    case.topology,
                    case.api,
                    case.memory_mode,
                    case.io_mode,
                    case.expected,
                    case.actual,
                    "PASS" if case.passed else "FAIL",
                    crc_status(case),
                    (
                        f"0x{case.crc_expected:08x}"
                        if case.crc_expected is not None
                        else ""
                    ),
                    (
                        f"0x{case.crc_actual:08x}"
                        if case.crc_actual is not None
                        else ""
                    ),
                    case.kernel_variant,
                    case.kernel_version,
                    str(case.kasan_enabled).lower(),
                ]
            )


def build_report(base: Path) -> FullReport:
    report = FullReport(run_id=base.name)
    processing_errors: List[str] = []

    for artifact_dir, identity_path in discover_kernel_dirs(base):
        identity = parse_kernel_identity(identity_path)
        report.kernel_variants.append(identity)
        suites, errors = process_artifact_dir(
            artifact_dir, identity
        )
        report.suites.extend(suites)
        processing_errors.extend(errors)

    report.validation_errors = processing_errors
    report.validation_errors.extend(validate_report(report))
    all_cases = [
        case for suite in report.suites for case in suite.cases
    ]
    report.summary = {
        "total_kernels": len(report.kernel_variants),
        "total_suites": len(report.suites),
        "total_cases": len(all_cases),
        "passed": sum(case.passed for case in all_cases),
        "failed": sum(not case.passed for case in all_cases),
        "crc_verified": sum(
            case.crc_match is True for case in all_cases
        ),
    }
    return report


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate detailed dual-kernel XDS test reports"
    )
    parser.add_argument("--artifact-dir", type=Path, required=True)
    parser.add_argument(
        "--json-report",
        type=Path,
        help="optional JSON output path",
    )
    parser.add_argument(
        "--md-report", type=Path, default=Path("report.md")
    )
    parser.add_argument(
        "--cases-tsv",
        type=Path,
        help="optional per-case TSV output path",
    )
    args = parser.parse_args()

    if not args.artifact_dir.is_dir():
        parser.error(
            f"artifact directory not found: {args.artifact_dir}"
        )

    report = build_report(args.artifact_dir)
    if args.json_report is not None:
        write_json(report, args.json_report)
    write_markdown(report, args.md_report)
    if args.cases_tsv is not None:
        write_tsv(report, args.cases_tsv)

    print(f"Markdown report: {args.md_report}")
    if args.json_report is not None:
        print(f"JSON report: {args.json_report}")
    if args.cases_tsv is not None:
        print(f"Cases TSV: {args.cases_tsv}")
    print(
        f"Kernels: {report.summary['total_kernels']}, "
        f"suites: {report.summary['total_suites']}, "
        f"cases: {report.summary['total_cases']}, "
        f"passed: {report.summary['passed']}, "
        f"failed: {report.summary['failed']}, "
        f"CRC verified: {report.summary['crc_verified']}"
    )
    if report.validation_errors:
        for error in report.validation_errors:
            print(f"VALIDATION ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"report: {error}", file=sys.stderr)
        sys.exit(1)
