#!/usr/bin/env python3
"""Verify public graphics pins and ordered overlays without requiring a VPP HAP."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from prepare_runtime_sources import load, verify

ROOT=Path(__file__).resolve().parents[1]


def check(wine_source=None):
    lock=json.loads((ROOT/'docs/graphics/graphics-stack.lock.yaml').read_text())
    for name,pin in lock['submodules'].items():
        row=subprocess.check_output(['git','-C',str(ROOT),'ls-files','--stage','--','thirdparty/'+name],text=True)
        if not row.startswith('160000 '+pin+' 0\t'):
            raise ValueError('Graphics source pin differs: '+name)
    for rows in lock['managed_patches'].values():
        for row in rows:
            if hashlib.sha256((ROOT/row['path']).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=row['sha256']:
                raise ValueError('Graphics patch differs: '+row['path'])
    mesa=(ROOT/'scripts/apply_mesa_ohos_patches.sh').read_text()
    actual=re.findall(r'\$SCRIPT_DIR/\.\./(patches/[^"\s]+\.patch)',mesa)
    if actual!=lock['mesa_production_overlays']:
        raise ValueError('Mesa production overlay order differs')
    if any('0014-' in name for name in actual):
        raise ValueError('Diagnostic-only Mesa patch was enabled')
    protocol=(ROOT/'entry/src/main/cpp/graphics/virgl_ipc_protocol.h').read_text()
    for field,key in [('kProtocolVersion','whip_version'),('kHostConfigStringCount','host_config_strings')]:
        if int(re.search(field+r'\s*=\s*(\d+)',protocol)[1])!=lock['protocol'][key]:
            raise ValueError('Host/guest configuration protocol differs: '+key)
    for component in ('wine','fex'):
        recipe,_=load(component)
        if lock['runtime_recipes'][component]['commit']!=recipe['adaptedCommit'] or lock['runtime_recipes'][component]['tree']!=recipe['adaptedTree']:
            raise ValueError('Graphics runtime recipe differs: '+component)
    if wine_source:
        recipe,_=load('wine');verify(wine_source,recipe)
    print('Graphics public pins, patch hashes, Mesa production order, protocol and Proton/FEX recipes PASS; device acceptance pending')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wine-source',type=Path)
    check(parser.parse_args().wine_source)
