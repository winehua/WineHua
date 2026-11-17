"""Read Harmony project metadata with the SDK's JSON5 implementation."""
import json
import os
from pathlib import Path
import subprocess


def load(path):
    path = Path(path)
    try:
        return json.loads(path.read_text(encoding='utf-8'))
    except json.JSONDecodeError:
        tool = Path(os.environ.get('TOOL_HOME', '/apps/harmony'))
        return json.loads(subprocess.check_output([
            str(tool / 'tool/node/bin/node'), '-e',
            "const fs=require('fs'),j=require(process.argv[1]);"
            "process.stdout.write(JSON.stringify(j.parse(fs.readFileSync(process.argv[2],'utf8'))))",
            str(tool / 'hvigor/hvigor-ohos-plugin/node_modules/json5'), str(path)]))
