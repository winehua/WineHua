import sys, json, subprocess, os, re
from pathlib import Path

inFile = sys.argv[1]
outFile = sys.argv[2]

# Read JSON5 file and convert to valid JSON
with open("build-profile.json5") as f:
    content = f.read()
# Remove trailing commas before } or ]
content = re.sub(r',\s*([}\]])', r'\1', content)
# Remove comments
content = re.sub(r'//.*$', '', content, flags=re.MULTILINE)
profile = json.loads(content)

config = profile["app"]["signingConfigs"][0]["material"]
basePath = Path(config["certpath"]).parent
sdkToolDir = os.environ['TOOL_HOME']

# Decrypt passwords using sign.js
keyPwd = subprocess.check_output(
    ["node", "sign.js", str(basePath.absolute()), config["keyPassword"]], encoding="utf-8"
).strip()
keystorePwd = subprocess.check_output(
    ["node", "sign.js", str(basePath.absolute()), config["storePassword"]], encoding="utf-8"
).strip()

# Build signing command
jar = f"{sdkToolDir}/sdk/default/openharmony/toolchains/lib/hap-sign-tool.jar"
cmd = (
    f"java -jar {jar} sign-app "
    f"-keyAlias {config['keyAlias']} "
    f"-signAlg {config['signAlg']} "
    f"-mode localSign "
    f"-appCertFile {config['certpath']} "
    f"-profileFile {config['profile']} "
    f"-inFile {inFile} "
    f"-keystoreFile {config['storeFile']} "
    f"-outFile {outFile} "
    f"-keyPwd {keyPwd} "
    f"-keystorePwd {keystorePwd}"
)
print(cmd)
os.system(cmd)
