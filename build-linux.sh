#!/bin/bash
set -e
# build the project on linux
# deveco command line tools is downloaded from:
# https://developer.huawei.com/consumer/cn/download/
# and extracted to any dir
#export TOOL_HOME=""

if [[ ! -n ${TOOL_HOME} ]]; then
  echo """\$TOOL_HOME IS NOT DEFINED, PLS SPECIFIY A CORRECT DIR!
  You can download HarmonyOS Commandline Tools form
  https://developer.huawei.com/consumer/cn/download/
       """
  exit 1
fi

export PROJ_BASE_HOME=$(dirname $(readlink -f "$0"))
export DEVECO_SDK_HOME=$TOOL_HOME/sdk
export OHOS_SDK_HOME=$TOOL_HOME/sdk/default/openharmony
export PATH=$TOOL_HOME/bin:$PATH
export PATH=$TOOL_HOME/tool/node/bin:$PATH

# ── 架构选择 ──
ARCH="${1:-arm64}"
case "$ARCH" in
    arm64)
        export OHOS_ARCH=aarch64
        export OHOS_ABI=arm64-v8a
        ;;
    x86_64)
        export OHOS_ARCH=x86_64
        export OHOS_ABI=x86_64
        ;;
    *)
        echo "用法: $0 [arm64|x86_64] [-b|-s|-p]"
        echo "  arm64   构建 arm64-v8a 目标 (默认)"
        echo "  x86_64  构建 x86_64 目标"
        exit 1
        ;;
esac

build_winehua_hap() {
	hvigorw assembleHap
	# add hnp, and sign manually
	pushd ${PROJ_BASE_HOME}/entry
		zip -r ../entry/build/default/outputs/default/entry-default-unsigned.hap hnp
	popd
}

build_winehua_hnps() {
	cd ${PROJ_BASE_HOME} && make -C build-hnp
}

sign_winehua() {
	pushd ${PROJ_BASE_HOME}
		python3 sign.py ./entry/build/default/outputs/default/entry-default-unsigned.hap ./entry/build/default/outputs/default/entry-default-signed.hap
	popd
}

helpusage() {
	echo "Usage: $(basename $0) [arch] [options]"
	echo "    arch: arm64 (默认) | x86_64"
	echo "    -b		Build WineHua HNPs and HAP"
	echo "    -s		Sign WineHua HAP Package, needed setup Key Signing in DevEco Studio"
	echo "    -p		Push WineHua HAP to device"
}

hdc_push() {
	"$OHOS_SDK_HOME/toolchains/hdc" file send ./entry/build/default/outputs/default/entry-default-signed.hap /data/local/tmp
	"$OHOS_SDK_HOME/toolchains/hdc" shell bm install -p /data/local/tmp/entry-default-signed.hap
	"$OHOS_SDK_HOME/toolchains/hdc" shell aa start -a EntryAbility -b $(jq ".app.bundleName" AppScope/app.json5)
}

build_winehua() {
	build_winehua_hnps
	build_winehua_hap
}

while getopts ":bsph:" optargs; do
	case ${optargs} in
		b)
			build_winehua
			;;
		s)
			sign_winehua
			;;
		p)
			hdc_push
			;;
		h)
			helpusage
			exit 0
			;;
		:)
			echo -e "  Option doesn't exist: '$OPTARG'"
			helpusage
			;;
	esac
done
