#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
if [[ $# != 1 || ! -f "$1" ]]; then
    echo 'usage: bash build.sh PATH_TO_GUI_PART3_PKG' >&2
    exit 2
fi
stock_ime="$(dirname -- "$1")/../gui/flash/ime.swf"
mkdir -p build dist
export FLEX_HOME="$(cygpath -aw ../tools/flex)"
export PATH="$(cygpath -au ../tools/jre/jdk-21.0.13+9-jre/bin):$PATH"
flex=../tools/flex/bin
python=../tools/python27/python.exe
cxx=${CXX:-'/d/Protable Files/mingw64/bin/g++.exe'}
"$cxx" -std=c++17 -Wall -Wextra -Werror -O2 -static -shared NativeIme.cpp \
    -DMS_WIN64 -isystem ../tools/python27/include -L../tools/python27/libs \
    -o build/_wot_native_ime.pyd -lpython27 -lole32 -loleaut32 -luuid -lcomctl32 -limm32
"$python" -B -S test_mod_paths.py
"$python" -B -S test_bridge_route.py
"$python" -S test_native_module.py
"$cxx" -std=c++17 -Wall -Wextra -Werror -O2 -static -DTEST_CANDIDATE_UI test_text_store.cpp NativeIme.cpp \
    -o build/test_text_store.exe -lole32 -loleaut32 -luuid -limm32
./build/test_text_store.exe
"$flex/compc" -source-path=extern -include-classes net.wg.infrastructure.base.AbstractView IME_fla.MainTimeline \
    -target-player=32.0 -output=build/extern.swc
"$flex/mxmlc" -compiler.external-library-path+=build/extern.swc -target-player=32.0 \
    -swf-version=17 -output=build/NativeImeBridge.swf NativeImeBridge.as
"$flex/mxmlc" -compiler.external-library-path+=build/extern.swc -target-player=32.0 \
    -swf-version=12 -compiler.omit-trace-statements=false -output=build/StockCandidatePosition.swf StockCandidatePosition.as
"$python" -B patch_stock_ime.py "$stock_ime" build/ime.swf
"$python" -B test_stock_ime.py "$stock_ime" build/ime.swf
"$python" -B pack.py "$@"
