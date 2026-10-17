#!/usr/bin/env python3
"""Generate text diffs; preserve table geometry and deleted-figure references."""
import hashlib
from pathlib import Path
import re
import subprocess
import sys
import tempfile

baseline, current, preamble, output = map(Path, sys.argv[1:])
input_re = re.compile(r'(?m)^[ \t]*\\input\{([^}]+)\}[ \t]*(?:%[^\n]*)?$')
table_re = re.compile(r'\\begin\{(table\*?)\}.*?\\end\{\1\}', re.S)
minipage_re = re.compile(r'\\begin\{minipage\}.*?\\end\{minipage\}', re.S)
figure_re = re.compile(r'\\begin\{(figure\*?)\}.*?\\end\{\1\}', re.S)

def flatten(path, root, stack=()):
    path = path.resolve()
    if path in stack:
        raise ValueError(f'Recursive input: {path}')
    text = path.read_text()
    # Do not turn commented example tables into active floats on expansion.
    text = re.sub(r'(?m)^[ \t]*%[^\n]*\n?', '', text)
    def include(match):
        child = root / match[1]
        if not child.suffix:
            child = child.with_suffix('.tex')
        return flatten(child, root, (*stack, path))
    return input_re.sub(include, text)

tables = {}
def mask(text):
    def replace(match):
        table = match[0]
        # Placement alone is not a text change; retain the revised placement.
        identity = re.sub(r'^(\\begin\{(?:table|figure)\*?\})(?:\[[^\]]*\])?',
                          r'\1', table)
        token = 'NDNSFTABLE' + hashlib.sha256(identity.encode()).hexdigest().upper()
        tables[token] = table
        return token
    text = table_re.sub(replace, text)
    # Non-floating tables need the same whole-table markup as floating tables.
    text = minipage_re.sub(
        lambda m: replace(m) if r'\captionof{table}' in m[0] else m[0], text)
    # Retain deleted figures in red so references in deleted prose still resolve.
    def deleted_figure(match):
        labels = re.findall(r'\\label\{([^}]+)\}', match[0])
        if labels and all(label not in new_labels for label in labels):
            return replace(match)
        return match[0]
    return figure_re.sub(deleted_figure, text)

old = flatten(baseline, baseline.parent)
new = flatten(current, current.parent)
new_labels = set(re.findall(r'\\label\{([^}]+)\}', new))
old_masked, new_masked = mask(old), mask(new)
with tempfile.TemporaryDirectory(prefix='ndnsf-latexdiff-') as tmp:
    old_file, new_file = Path(tmp) / 'old.tex', Path(tmp) / 'new.tex'
    old_file.write_text(old_masked)
    new_file.write_text(new_masked)
    diff = subprocess.check_output([
        'latexdiff', '--encoding=utf8', '--graphics-markup=none',
        '--preamble=' + str(preamble.resolve()), str(old_file), str(new_file)
    ], text=True)

removed_labels = set()
for token, table in tables.items():
    if token in old_masked and token not in new_masked:
        removed_labels.update(re.findall(r'\\label\{([^}]+)\}', table))
        table = re.sub(r'\\label\{([^}]+)\}', r'\\label{removed:\1}', table)
        color = 'NDNSFDeleted'
    elif token in new_masked and token not in old_masked:
        color = 'NDNSFAdded'
    else:
        color = None
        # A moved, unchanged table appears only at its revised location.
        diff = re.sub(r'\\DIFdel(?:FL)?\{\s*' + token + r'\s*\}', '', diff)
        diff = re.sub(r'\\DIFadd(?:FL)?\{\s*' + token + r'\s*\}', token, diff)
    if color:
        # The caption package resets text color, so color its text and label too.
        color_setup = ('\n\\color{' + color + '}\n\\captionsetup{'
                       'textfont+={color=' + color + '},'
                       'labelfont+={color=' + color + '}}')
        table = re.sub(
            r'(\\begin\{(?:table|figure)\*?\}(?:\[[^\]]*\])?'
            r'|\\begin\{minipage\}(?:\[[^\]]*\])?\{[^}]*\})',
            lambda m: m[0] + color_setup, table, count=1)
    diff = diff.replace(token, '\n' + table + '\n')
if 'NDNSFTABLE' in diff:
    raise ValueError('Unexpanded table marker')
for label in removed_labels - new_labels:
    diff = diff.replace('\\ref{' + label + '}', '\\ref{removed:' + label + '}')
# TeX ignores the source space between adjacent diff macros; retain a visible
# separator when deleted and added phrases would otherwise run together.
for suffix in ('', 'FL'):
    diff = diff.replace(
        rf'\DIFdelend{suffix} \DIFaddbegin{suffix} \DIFadd{suffix}',
        rf'\DIFdelend{suffix}\ \DIFaddbegin{suffix} \DIFadd{suffix}')
output.write_text(diff)
