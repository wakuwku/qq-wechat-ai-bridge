"""Package only reviewed source. Re-scan ZIP entries and verify every file hash."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SKIP_DIRS = {'.git', 'node_modules', 'state', '__pycache__', '.venv'}
DENY_NAMES = {'config.json', '.credentials.yaml', 'privacy-rules.json'}
DENY_SUFFIXES = {'.dll', '.exe', '.pdb', '.obj', '.lib', '.db', '.sqlite', '.log', '.pyc', '.zip'}


def files():
    for folder, dirs, names in os.walk(ROOT, followlinks=False):
        dirs[:] = [n for n in dirs if n not in SKIP_DIRS and not (Path(folder)/n).is_symlink()]
        for name in names:
            path = Path(folder)/name
            if path.is_symlink() or name in DENY_NAMES or name.startswith('.env') or '.bak' in name or path.suffix.lower() in DENY_SUFFIXES:
                continue
            yield path


def scan(paths):
    findings = []
    for path in paths:
        try: text = path.read_text(encoding='utf-8-sig')
        except UnicodeError: continue
        for number, line in enumerate(text.splitlines(), 1):
            rules = {
                'github-or-provider-token': r'(?:github_pat_|gh[pousr]_|sk-)[A-Za-z0-9_]{20,}',
                'non-example-wechat-id': r'wxid_[a-z0-9]{10,}',
                'credential-in-url': r'https?://[^\s/:]+:[^\s/@]+@',
                'private-key-material': r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----',
            }
            for rule, pattern in rules.items():
                if re.search(pattern, line):
                    # Explicit documentation examples are inspected individually.
                    if path.as_posix().endswith('docs/guides/PRIVACY.md') and rule in {'private-key-material', 'credential-in-url'}:
                        continue
                    if rule=='credential-in-url' and 'https://user:pass@' in line: continue
                    if rule=='credential-in-url' and path.as_posix().endswith('scripts/test-audit-security.mjs') and ('http://' + 'user:pass@public.test/') in line: continue
                    findings.append({'file':path.relative_to(ROOT).as_posix(),'line':number,'rule':rule})
    return findings


def digest(data): return hashlib.sha256(data).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args=parser.parse_args()
    paths=sorted(files())
    findings=scan(paths)
    if findings:
        print(json.dumps({'findings':findings},indent=2));return 1
    print(f'Public source scan: {len(paths)} files, 0 findings (limited patterns; not a security guarantee).')
    if not args.output:return 0
    args.output.mkdir(parents=True,exist_ok=True)
    hashes=[]
    for module,slug in [('qq-bridge','qq'),('wechat-bridge','wechat')]:
        target=args.output/f'dafeiyu-{slug}-v0.1.0.zip'
        selected=[p for p in paths if p.relative_to(ROOT).parts[0] not in {'qq-bridge','wechat-bridge'} or p.relative_to(ROOT).parts[0]==module]
        expected={f'dafeiyu-{slug}/'+p.relative_to(ROOT).as_posix():digest(p.read_bytes()) for p in selected}
        with zipfile.ZipFile(target,'x',zipfile.ZIP_DEFLATED) as archive:
            for p in selected: archive.write(p,f'dafeiyu-{slug}/'+p.relative_to(ROOT).as_posix())
        with zipfile.ZipFile(target) as archive:
            assert archive.testzip() is None
            assert set(archive.namelist())==set(expected)
            for name,sha in expected.items(): assert digest(archive.read(name))==sha
        hashes.append(f'{digest(target.read_bytes())}  {target.name}')
        print(f'{target.name}: {len(selected)} files, {target.stat().st_size} bytes; content hashes verified.')
    (args.output/'SHA256SUMS').write_text('\n'.join(hashes)+'\n',encoding='utf8')
    return 0


if __name__=='__main__': raise SystemExit(main())
