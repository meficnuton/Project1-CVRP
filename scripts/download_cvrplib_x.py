"""Download and independently validate all 100 CVRPLib Set X instances (.vrp + .sol BKS).

Uses the official CVRPLib instances page (PUC-Rio) to discover Set X entries,
downloads each .vrp and .sol pair, validates 100/100 pairs with `cvrp validate --ids cvrplib-x`,
and writes a reproducible `manifest.csv` with SHA-256 hashes and BKS metadata.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import csv
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request

BASE_URL = "https://galgos.inf.puc-rio.br"
INSTANCES_URL = f"{BASE_URL}/cvrplib/index.php/en/cvrp/instances"
USER_AGENT = "cvrp-project-i-downloader/1.0 (academic benchmark fetch)"

ROW_RE = re.compile(
    r'href="(?P<vrp_path>/cvrplib/index\.php/en/download/instance/(?P<id>\d+))"[^>]*>\s*'
    r'(?P<name>X-n\d+-k\d+)\s*</a>.*?</td>\s*'
    r'<td[^>]*>\$(?P<customers>[\d{},]+)\$</td>\s*'
    r'<td[^>]*>\$(?P<k_nom>[\d{},]+)\$</td>\s*'
    r'<td[^>]*>\$(?P<capacity>[\d{},]+)\$</td>\s*'
    r'<td[^>]*>.*?href="(?P<sol_path>/cvrplib/index\.php/en/download/bks/\d+)"[^>]*>.*?'
    r'\$(?P<bks>[\d{},]+)\$.*?</td>\s*'
    r'<td[^>]*>.*?<span[^>]*>\s*(?P<opt>yes|no)\s*</span>',
    re.DOTALL,
)

VALID_OUT_RE = re.compile(r"^VALID cost=(\d+) routes=(\d+)\s*$")


@dataclass(frozen=True)
class Entry:
    name: str
    cvrplib_id: int
    customers: int
    k_nominal: int
    capacity: int
    web_bks: int
    proven_optimal: bool
    vrp_url: str
    sol_url: str


def parse_int(token: str) -> int:
    cleaned = token.replace("{,}", "").replace(",", "").strip()
    return int(cleaned)


def size_group(customers: int) -> str:
    if customers <= 300:
        return "small"
    if customers <= 600:
        return "medium"
    return "large"


def fetch_bytes(url: str, retries: int = 4, timeout: float = 45.0) -> bytes:
    last_err: Exception | None = None
    for attempt in range(1, retries + 1):
        req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                data = resp.read()
                if not data:
                    raise RuntimeError(f"Empty response from {url}")
                return data
        except (urllib.error.URLError, TimeoutError, RuntimeError) as exc:
            last_err = exc
            if attempt < retries:
                time.sleep(min(2.0 ** (attempt - 1), 8.0))
    raise RuntimeError(f"Failed to download {url} after {retries} attempts: {last_err}")


def download_to_file(url: str, dest: Path, force: bool = False) -> bool:
    if dest.is_file() and dest.stat().st_size > 0 and not force:
        return False
    data = fetch_bytes(url)
    tmp = dest.with_suffix(dest.suffix + ".part")
    tmp.write_bytes(data)
    tmp.replace(dest)
    return True


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_set_x_entries(html: str) -> list[Entry]:
    entries: list[Entry] = []
    seen: set[str] = set()
    for m in ROW_RE.finditer(html):
        name = m.group("name")
        if name in seen:
            raise RuntimeError(f"Duplicate Set X entry in HTML: {name}")
        seen.add(name)
        entries.append(
            Entry(
                name=name,
                cvrplib_id=int(m.group("id")),
                customers=parse_int(m.group("customers")),
                k_nominal=parse_int(m.group("k_nom")),
                capacity=parse_int(m.group("capacity")),
                web_bks=parse_int(m.group("bks")),
                proven_optimal=(m.group("opt").lower() == "yes"),
                vrp_url=BASE_URL + m.group("vrp_path"),
                sol_url=BASE_URL + m.group("sol_path"),
            )
        )
    if len(entries) != 100:
        raise RuntimeError(f"Expected 100 Set X entries in CVRPLib HTML, found {len(entries)}")
    entries.sort(key=lambda e: (e.customers, e.name))
    return entries


def load_candidates(path: Path) -> dict[str, tuple[str, str]]:
    if not path.is_file():
        return {}
    mapping: dict[str, tuple[str, str]] = {}
    with path.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            mapping[row["instance"]] = (row["split"], row["group"])
    return mapping


def validate_pair(cvrp_exe: Path, vrp: Path, sol: Path) -> tuple[int, int]:
    proc = subprocess.run(
        [str(cvrp_exe), "validate", str(vrp), str(sol), "--ids", "cvrplib-x"],
        capture_output=True,
        text=True,
        timeout=30,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"Validation failed for {vrp.name}: {proc.stderr.strip()}")
    match = VALID_OUT_RE.match(proc.stdout.strip())
    if not match:
        raise RuntimeError(f"Unexpected validator output for {vrp.name}: {proc.stdout!r}")
    return int(match.group(1)), int(match.group(2))


def default_cvrp_exe() -> Path:
    return Path("build/cvrp.exe") if os.name == "nt" else Path("build/cvrp")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", type=Path, default=Path("data/cvrplib-x"))
    parser.add_argument(
        "--candidates", type=Path, default=Path("docs/project-i/01-benchmark-candidates.csv")
    )
    parser.add_argument("--cvrp-exe", type=Path, default=default_cvrp_exe())
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--refresh-html", action="store_true")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--skip-validate", action="store_true")
    args = parser.parse_args()

    out_dir: Path = args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    html_path = out_dir / "cvrp-instances.html"
    if args.refresh_html or not html_path.is_file() or html_path.stat().st_size == 0:
        print(f"Fetching CVRPLib catalog: {INSTANCES_URL}")
        download_to_file(INSTANCES_URL, html_path, force=True)
    else:
        print(f"Using cached CVRPLib catalog: {html_path}")

    html = html_path.read_text(encoding="utf-8", errors="replace")
    entries = parse_set_x_entries(html)
    candidates = load_candidates(args.candidates)

    entry_names = {e.name for e in entries}
    missing_candidates = set(candidates) - entry_names
    if missing_candidates:
        raise RuntimeError(f"Candidates missing from CVRPLib Set X: {sorted(missing_candidates)}")

    if not args.skip_validate and not args.cvrp_exe.is_file():
        raise RuntimeError(
            f"Solver binary not found at {args.cvrp_exe}. Build Release first or pass --cvrp-exe."
        )

    def fetch_pair(entry: Entry) -> tuple[str, int]:
        vrp_path = out_dir / f"{entry.name}.vrp"
        sol_path = out_dir / f"{entry.name}.sol"
        downloaded = 0
        if download_to_file(entry.vrp_url, vrp_path, force=args.force):
            downloaded += 1
        if download_to_file(entry.sol_url, sol_path, force=args.force):
            downloaded += 1
        return entry.name, downloaded

    print(f"Downloading 100 Set X pairs into {out_dir} (workers={args.workers})...")
    newly_downloaded = 0
    done_pairs = 0
    with ThreadPoolExecutor(max_workers=max(1, args.workers)) as pool:
        futures = {pool.submit(fetch_pair, e): e for e in entries}
        for fut in as_completed(futures):
            _, cnt = fut.result()
            newly_downloaded += cnt
            done_pairs += 1
            if done_pairs % 20 == 0 or done_pairs == len(entries):
                print(f"  progress: {done_pairs}/{len(entries)} pairs ready")

    timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    manifest_rows: list[dict[str, str | int]] = []
    validated_count = 0
    optimal_count = 0
    split_counts = {"pilot": 0, "test": 0, "extra": 0}

    print("Validating pairs and computing SHA-256 hashes...")
    for entry in entries:
        vrp_path = out_dir / f"{entry.name}.vrp"
        sol_path = out_dir / f"{entry.name}.sol"
        split, group = candidates.get(entry.name, ("extra", size_group(entry.customers)))
        split_counts[split] += 1
        if entry.proven_optimal:
            optimal_count += 1

        if args.skip_validate:
            bks_cost = entry.web_bks
            bks_routes = -1
            validated = "false"
        else:
            bks_cost, bks_routes = validate_pair(args.cvrp_exe, vrp_path, sol_path)
            if bks_cost != entry.web_bks:
                raise RuntimeError(
                    f"BKS mismatch for {entry.name}: validated={bks_cost}, web={entry.web_bks}"
                )
            validated = "true"
            validated_count += 1

        manifest_rows.append(
            {
                "instance": entry.name,
                "cvrplib_id": entry.cvrplib_id,
                "customers": entry.customers,
                "k_nominal": entry.k_nominal,
                "capacity": entry.capacity,
                "split": split,
                "group": group,
                "bks_cost": bks_cost,
                "bks_routes": bks_routes,
                "proven_optimal": "true" if entry.proven_optimal else "false",
                "vrp_sha256": sha256_file(vrp_path),
                "sol_sha256": sha256_file(sol_path),
                "vrp_url": entry.vrp_url,
                "sol_url": entry.sol_url,
                "downloaded_at": timestamp,
                "validated": validated,
            }
        )

    manifest_path = out_dir / "manifest.csv"
    fieldnames = list(manifest_rows[0].keys())
    with manifest_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(manifest_rows)

    print(
        f"Done: 100 Set X instances ({newly_downloaded} files newly downloaded), "
        f"validated={validated_count}/100, proven_optimal={optimal_count}/100, "
        f"splits={split_counts}."
    )
    print(f"Manifest written to {manifest_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
