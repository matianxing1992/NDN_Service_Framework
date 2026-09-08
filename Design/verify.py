#!/usr/bin/env python3
"""Verify document artifacts and render contact sheets for visual review."""
from pathlib import Path
import hashlib, json, re, subprocess, sys, tarfile
from PIL import Image, ImageOps, ImageDraw
root = Path(__file__).resolve().parent.parent
run = root / sys.argv[1]
design = root / 'Design'
result = {'run': str(run.relative_to(root)), 'runtime_tests': 'not run'}
texts = []
for name in ('current-design', 'target-design'):
    pdf = design / (name + '.pdf')
    info = subprocess.check_output(['pdfinfo', str(pdf)], text=True)
    pages = int(re.search(r'Pages:\s+(\d+)', info)[1])
    log = (run / name / (name + '.log')).read_text()
    warnings = [l for l in log.splitlines() if re.search(r'Overfull|Missing character|Warning|^!', l)]
    fonts = subprocess.check_output(['pdffonts', str(pdf)], text=True)
    embedded = all(re.search(r'\s+yes\s+(?:yes|no)\s+(?:yes|no)\s+\d+\s+\d+\s*$', l) for l in fonts.splitlines()[2:])
    text = subprocess.check_output(['pdftotext', '-layout', str(pdf), '-'], text=True)
    texts.append(re.sub(r'\s+', '', '\n'.join(l for l in text.splitlines() if not ('NDNSF /' in l))))
    result[name] = dict(pages=pages, warnings=warnings, fonts_embedded=embedded, sha256=hashlib.sha256(pdf.read_bytes()).hexdigest())
    assert pages == 35 and not warnings and embedded
assert texts[0] == texts[1]
assert (design / 'current-content.tex').read_bytes() == (design / 'target-content.tex').read_bytes()
result['technical_body_equal'] = True
m = json.loads((design / 'source-baseline.json').read_text())
result['source_reconstruction'] = json.loads(subprocess.check_output([sys.executable, str(design / 'verify-source-baseline.py')], text=True))
if (root / m['local_source_archive']).exists():
    assert hashlib.sha256((root / m['local_source_archive']).read_bytes()).hexdigest() == m['archive_sha256']
    with tarfile.open(root / m['local_source_archive']) as tar:
        assert all(hashlib.sha256(tar.extractfile(p).read()).hexdigest() == meta['sha256'] for p, meta in m['files'].items())
    result['source_archive_verified'] = len(m['files'])
result['source_drift_after_snapshot'] = [p for p, meta in m['files'].items() if hashlib.sha256((root / p).read_bytes()).hexdigest() != meta['sha256']]
result['snapshot_commit'] = m['baseline_commit']
subprocess.run(['pdftoppm', '-scale-to', '1000', '-png', str(design / 'current-design.pdf'), str(run / 'page')], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
files = sorted(run.glob('page-*.png'))
for offset in range(0, len(files), 8):
    sheet = Image.new('RGB', (1600, 1160), '#b0b0b0')
    for i, p in enumerate(files[offset:offset+8]):
        im = Image.open(p); im.thumbnail((390, 545))
        x, y = (i % 4)*400, (i // 4)*580
        sheet.paste(im, (x, y+24))
        ImageDraw.Draw(sheet).text((x+10, y+4), p.stem, fill='black')
    sheet.save(run / ('contact-'+str(offset//8+1)+'.png'))
(run / 'verification.json').write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n')
print(json.dumps(result, ensure_ascii=False))
