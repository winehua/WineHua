HGS_P0_TOOLS=/workspace/WineHua-root/.hgs-build-tools
export PATH="$HGS_P0_TOOLS/bin:$HGS_P0_TOOLS/local-packages/usr/bin:/apps/harmony/sdk/default/openharmony/native/build-tools/cmake/bin:$PATH"
export PYTHONPATH="$HGS_P0_TOOLS/local-packages/usr/lib/python3/dist-packages${PYTHONPATH:+:$PYTHONPATH}"
export LD_LIBRARY_PATH="$HGS_P0_TOOLS/local-packages/usr/lib/x86_64-linux-gnu:$HGS_P0_TOOLS/local-packages/usr/lib/x86_64-linux-gnu/blas:$HGS_P0_TOOLS/local-packages/usr/lib/x86_64-linux-gnu/lapack${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export M4="$HGS_P0_TOOLS/local-packages/usr/bin/m4"
export AC_MACRODIR="$HGS_P0_TOOLS/local-packages/usr/share/autoconf"
export autom4te_perllibdir="$AC_MACRODIR"
export AUTOM4TE="$HGS_P0_TOOLS/local-packages/usr/bin/autom4te"
export BISON_PKGDATADIR="$HGS_P0_TOOLS/local-packages/usr/share/bison"
export PKG_CONFIG_LIBDIR_FOR_BUILD=/workspace/WineHua-root/hgs-upstream-baseline/build/host-tools/lib/pkgconfig
export PKG_CONFIG_PATH_FOR_BUILD="$PKG_CONFIG_LIBDIR_FOR_BUILD"
export JOBS=8

export XKBCOMP_BIN="$HGS_P0_TOOLS/local-packages/usr/bin/xkbcomp"
