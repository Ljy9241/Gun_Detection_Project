"""Replay the original recordings through the compiled production C++ detector."""
import csv
from datetime import datetime
from itertools import groupby
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[3]
executable = Path(sys.argv[1]).resolve()
for name, expected in [('横向动作.csv', [30, 0, 0, 30]), ('竖向动作.csv', [0, 30, 0, 0])]:
    with (root / 'csv' / name).open(encoding='utf-8-sig', newline='') as f:
        rows = list(csv.DictReader(f))
    origin = datetime.fromisoformat(rows[0]['timestamp'])
    lines = []
    for stamp, batch in groupby(rows, key=lambda row: row['timestamp']):
        batch = list(batch)
        seconds = (datetime.fromisoformat(stamp) - origin).total_seconds()
        for index, row in enumerate(batch):
            micros = round((seconds + index / len(batch)) * 1000000)
            lines.append(f"{micros} {row['accel_x']} {row['accel_y']} {row['accel_z']}\n")
    result = subprocess.run([str(executable), 'replay'], input=''.join(lines),
                            text=True, capture_output=True, check=True)
    counts = list(map(int, result.stdout.split()))
    assert counts == expected, (name, counts, expected)
    print(f'{name}: horizontal/vertical/unknown/shots = {counts}')
