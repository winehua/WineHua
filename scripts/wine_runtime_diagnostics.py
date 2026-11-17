#!/usr/bin/env python3
"""Make inherited temporary Unix diagnostics opt-in without changing fault routing.

Some historical effective Wine sources contain probes outside the ordered patch
chain. Normalize those optional probes as well as the normal OHOS diagnostics.
Unknown source layouts fail rather than silently shipping an unguarded probe.
"""
import argparse
import json
from pathlib import Path
import re

HEADER = 'ohos_runtime_diagnostics.h'


def replace_once(text, old, new, label):
    if new in text:
        return text
    if text.count(old) != 1:
        raise ValueError('unsupported Wine diagnostic context: ' + label)
    return text.replace(old, new, 1)


def apply(source):
    root = source / 'dlls/ntdll/unix'
    header = Path(__file__).with_suffix('.h').read_bytes()
    updates = {}
    for name in ('ohos_virtual.c', 'virtual.c', 'thread.c', 'signal_arm64.c'):
        path = root / name
        text = path.read_text()
        original = text
        if name == 'ohos_virtual.c':
            text = replace_once(text, 'static int ohos_diag_quiet;',
                'volatile sig_atomic_t ohos_runtime_diagnostics_enabled;\n'
                'static int ohos_diag_quiet = 1;', 'diagnostic state')
            text = replace_once(text, '    ohos_diag_quiet = quiet && quiet[0] == \'1\';',
                '    /* Read the environment once, before registering signal handlers. */\n'
                '    ohos_runtime_diagnostics_enabled =\n'
                '        !(quiet && quiet[0] == \'1\') &&\n'
                '        ohos_diagnostics_flag( getenv( "WINEHUA_RUNTIME_DIAGNOSTICS" ) );\n'
                '    ohos_diag_quiet = (quiet && quiet[0] == \'1\') ||\n'
                '        (!ohos_runtime_diagnostics_enabled && !ohos_diagnostics_flag( rpgxp_diagnostics ));',
                'initialization')
            if '[FAULT-MIN]' in text:
                text = replace_once(text, '        if (fault_seq <= 65536)',
                    '        if (ohos_runtime_diagnostics_enabled && fault_seq <= 65536)', 'fault probe')
                if text.count('[FAULT-MIN]') != 1:
                    raise ValueError('unknown FAULT-MIN instrumentation')
        elif name == 'virtual.c' and '[TEB-PROBE]' in text:
            text = replace_once(text, '    if (++count > 8192) return;',
                '    if (!ohos_runtime_diagnostics_enabled) return;\n'
                '    if (++count > 8192) return;', 'TEB probe')
            if text.count('[TEB-PROBE]') != 1:
                raise ValueError('unknown TEB instrumentation')
        elif name in ('thread.c', 'signal_arm64.c'):
            text = re.sub(r'(?m)^(\s*)write(\( 2, "\[QUIT-PROBE\][^\n]+);$',
                          r'\1ohos_runtime_diagnostics_write\2;', text)
            for line in text.splitlines():
                if '[QUIT-PROBE]' in line and 'ohos_runtime_diagnostics_write(' not in line:
                    raise ValueError('unknown QUIT instrumentation: ' + name)
        if name == 'ohos_virtual.c' or text != original or '#include "' + HEADER + '"' in text:
            text = replace_once(text, '#include "unix_private.h"',
                                '#include "unix_private.h"\n#include "' + HEADER + '"', name + ' include')
        if text != original:
            updates[path] = text.encode()
    # Validate the entire transformation before publishing any file.
    updates[root / HEADER] = header
    changed = []
    for path, data in updates.items():
        if not path.exists() or path.read_bytes() != data:
            path.write_bytes(data)
            changed.append(str(path.relative_to(source)))
    return changed


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps({'runtimeDiagnostics': 'opt-in-v1', 'changed': apply(args.source)}))
