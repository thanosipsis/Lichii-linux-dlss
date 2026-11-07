#!/usr/bin/env python3
"""Fetch pinned NVIDIA Linux NGX build inputs, outside tracked source files."""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=Path('.cache/ngx'))
    args = parser.parse_args()
    manifest = json.loads(Path(__file__).with_name('linux-ngx-sdk.json').read_text())
    for entry in manifest['files']:
        target = args.directory / entry['path']
        if target.is_file() and hashlib.sha256(target.read_bytes()).hexdigest() == entry['sha256']:
            continue
        url = f"{manifest['repository']}/{manifest['commit']}/{entry['source']}"
        with urllib.request.urlopen(url, timeout=120) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != entry['sha256']:
            raise RuntimeError(f"SHA-256 mismatch: {entry['path']}")
        target.parent.mkdir(parents=True, exist_ok=True)
        partial = target.with_suffix(target.suffix + '.part')
        partial.write_bytes(data)
        partial.replace(target)
        print(f"Verified {entry['path']}")
    print(f"SDK/runtime: {args.directory.resolve()}")
    print(f"NVIDIA license: {(args.directory / 'lib/LICENSE.txt').resolve()}")


if __name__ == '__main__':
    main()
