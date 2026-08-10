S=/data/build/aptdev/sysroot
export PKG_CONFIG_PATH="$S/usr/lib/x86_64-linux-gnu/pkgconfig:$S/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$S"
export CPATH="$S/usr/include:$S/usr/include/x86_64-linux-gnu:$S/usr/include/freetype2:$S/usr/include/x86_64-linux-gnu/openblas-pthread"
export LIBRARY_PATH="$S/usr/lib/x86_64-linux-gnu"
export CMAKE_PREFIX_PATH="$S/usr"
export LD_LIBRARY_PATH="$S/usr/lib/x86_64-linux-gnu:$S/usr/lib/x86_64-linux-gnu/openblas-pthread:$S/usr/lib/x86_64-linux-gnu/lapack:$S/usr/lib/x86_64-linux-gnu/blas${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
