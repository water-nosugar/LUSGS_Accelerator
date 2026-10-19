#!/usr/bin/env python3
"""Bundle the local design prototype without third-party dependencies."""
from pathlib import Path
import json
import subprocess


def main():
    root = Path(__file__).resolve().parent
    document = (root / 'index.html').read_text(encoding='utf-8')
    for name in ('styles.css', 'architecture-poster.css'):
        css = (root / name).read_text(encoding='utf-8')
        document = document.replace(f'<link rel="stylesheet" href="{name}">', '<style>\n' + css + '\n</style>')
    for name in ('model.js', 'detail-architecture.js', 'demo-session.js', 'app.js', 'poster-layout.js', 'architecture-poster.js'):
        code = (root / name).read_text(encoding='utf-8')
        if '</script' in code.lower():
            raise ValueError(f'{name} contains an unsafe inline script terminator')
        document = document.replace(f'<script src="{name}"></script>', '<script>\n' + code + '\n</script>')
    target = root / 'prototype.html'
    target.write_text(document, encoding='utf-8')
    result = subprocess.run(
        ['node', '-e', "process.stdout.write(JSON.stringify(require('./demo-session.js'),null,2))"],
        cwd=root, check=True, text=True, capture_output=True,
    )
    session = json.loads(result.stdout)
    (root / 'demo-session.json').write_text(json.dumps(session, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(f'STANDALONE_BUILD_PASS: {target.name} ({target.stat().st_size} bytes); {len(session["events"])} illustrative events')


if __name__ == '__main__':
    main()
