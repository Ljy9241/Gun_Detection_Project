"""Build a source-only, checksummed release; excludes credentials and runtime data."""
import hashlib
import io
import json
import re
import tarfile
from pathlib import Path
import argparse

ROOT = Path(__file__).resolve().parents[1]


def build(release):
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]{0,63}', release):
        raise ValueError('Invalid release identifier')
    output = ROOT / 'output' / 'deployment'
    output.mkdir(parents=True, exist_ok=True)
    selected = sorted((ROOT / 'cloud_backend').glob('*.py'))
    selected += [ROOT / 'cloud_backend' / 'requirements.txt']
    selected += sorted((ROOT / 'deploy' / 'ubuntu').glob('*'))
    manifest = {}
    archive = output / f'{release}.tar.gz'
    with tarfile.open(archive, 'w:gz') as bundle:
        for path in selected:
            data = path.read_bytes().replace(b'\r\n', b'\n')
            name = path.relative_to(ROOT).as_posix()
            manifest[name] = hashlib.sha256(data).hexdigest()
            member = tarfile.TarInfo(name)
            member.size, member.mode = len(data), 0o644
            bundle.addfile(member, io.BytesIO(data))
        data = json.dumps({'release': release, 'files': manifest}, indent=2).encode('utf-8')
        member = tarfile.TarInfo('MANIFEST.json')
        member.size, member.mode = len(data), 0o644
        bundle.addfile(member, io.BytesIO(data))
    checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix(archive.suffix + '.sha256').write_text(checksum + '\n', encoding='ascii')
    print(json.dumps({'archive': str(archive), 'sha256': checksum,
                      'files': len(manifest)}, ensure_ascii=False))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--release', required=True)
    build(parser.parse_args().release)
