#!/bin/sh
# Builds ect-zopfli.mjs: ECT's zopfli fork (the deflate engine behind `ect -zip`)
# compiled to a single-file WASM ES module usable from browsers and Node.
# Needs an activated emsdk (emcc on PATH).
# Sources in src/ come from
# https://github.com/fhanau/Efficient-Compression-Tool @ f0b38f7f8b750099f14d4976beff6a107d6119ac
# (the revision bundled with ect-bin 1.4.1), with small js13k-zip hooks marked
# "js13k-zip" (seedable randomization, per-block output recording, emitting
# the result of every split/compress pass and stopping once passes repeat).
set -e
cd "$(dirname "$0")"
emcc ${EMFLAGS:--O3 -flto -msimd128 -mbulk-memory -mnontrapping-fptoint} -DNDEBUG -DNOMULTI=1 -Isrc ectz.cpp \
  src/zopfli/blocksplitter.c src/zopfli/deflate.cpp src/zopfli/katajainen.cpp \
  src/zopfli/lz77.c src/zopfli/squeeze.c src/zopfli/util.c src/LzFind.c \
  -sSTACK_SIZE=8MB -sINITIAL_MEMORY=16MB -sALLOW_MEMORY_GROWTH=1 \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,worker,node -sSINGLE_FILE=1 \
  -sEXPORTED_FUNCTIONS=_ect_deflate,_ect_output,_ect_block_count,_ect_blocks,_malloc,_free \
  -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32 \
  -o ${OUT:-ect-zopfli.mjs}
