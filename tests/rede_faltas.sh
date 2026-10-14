#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 tests/rede_faltas_test.py
