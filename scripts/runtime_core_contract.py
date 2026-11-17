"""Pin the preserved GL, Vulkan, input, network and FEX binary compatibility base.

Intentional upgrades require reviewing the lock alongside their regression
evidence. This gate does not infer compatibility from a self-reported version.
"""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'docs/graphics/runtime-core.lock.json'


def validate_core(read, section='libraries', candidate=None):
    lock = json.loads(LOCK.read_text())
    if lock.get('schemaVersion') != 1 or not lock.get(section):
        raise ValueError('Missing core runtime compatibility lock: ' + section)
    for name, expected in lock[section].items():
        if candidate is not None:
            from source_build_contract import validate_member
            validate_member(candidate, section, name, read(name))
            continue
        if hashlib.sha256(read(name)).hexdigest() != expected:
            raise ValueError('Unreviewed core runtime replacement: ' + name)
