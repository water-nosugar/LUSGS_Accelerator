"""Shared test paths; tests never infer the project root from their depth."""
from pathlib import Path
import sys

PROJECT_ROOT=Path(__file__).resolve().parents[1]
REPO_ROOT=PROJECT_ROOT.parents[1]
FIXTURES=Path(__file__).resolve().parent/'fixtures'
for path in (PROJECT_ROOT,PROJECT_ROOT/'tools',PROJECT_ROOT/'configs',PROJECT_ROOT/'configs/lusgs'):
    if str(path) not in sys.path:sys.path.insert(0,str(path))
