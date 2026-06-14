#!/bin/bash

if [[ ! -n ${TOOL_HOME} ]]; then
  echo """\$TOOL_HOME IS NOT DEFINED, PLS SPECIFIY A CORRECT DIR!
  You can download HarmonyOS Commandline Tools form
  https://developer.huawei.com/consumer/cn/download/
       """
  exit 1
fi

export PATH=$TOOL_HOME/bin:$PATH
export PATH=$TOOL_HOME/tool/node/bin:$PATH

# Check for required arguments
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <input-unsigned.hap> <output-signed.hap>"
    exit 1
fi

# Run the Python script with the provided arguments
python3 sign.py "$1" "$2"