#!/bin/bash

set -e

source ./platforms/config.sh

CEF_PLATFORM=linux64
CEF_SHA1=6bd0b932a37503fb0206ac77bf628ebd0496f120

echo "Building external libraries..."
echo "  VPINBALL_SHA: ${VPINBALL_SHA}"
echo "  CEF_VERSION: ${CEF_VERSION}"
echo ""

NUM_PROCS=$(nproc)

mkdir -p "external/linux-x64/${BUILD_TYPE}"
cd "external/linux-x64/${BUILD_TYPE}"

#
# VPX plugin SDK headers
#

if [ -n "${VPINBALL_SOURCE_DIR}" ]; then
   echo "Using local vpinball source: ${VPINBALL_SOURCE_DIR} (unpinned)"
   VPINBALL_PLUGINS="${VPINBALL_SOURCE_DIR}/plugins/plugins"
else
   VPINBALL_FOUND_SHA="$([ -f vpinball/cache.txt ] && cat vpinball/cache.txt || echo "")"

   if [ "${VPINBALL_SHA}" != "${VPINBALL_FOUND_SHA}" ]; then
      echo "Downloading vpinball plugin SDK. Expected: ${VPINBALL_SHA}, Found: ${VPINBALL_FOUND_SHA}"

      rm -rf vpinball
      mkdir vpinball
      cd vpinball
      curl -sL https://github.com/vpinball/vpinball/archive/${VPINBALL_SHA}.tar.gz -o vpinball-${VPINBALL_SHA}.tar.gz
      tar xzf vpinball-${VPINBALL_SHA}.tar.gz "vpinball-${VPINBALL_SHA}/plugins/plugins"
      rm vpinball-${VPINBALL_SHA}.tar.gz
      echo "${VPINBALL_SHA}" > cache.txt
      cd ..
   fi
   VPINBALL_PLUGINS="vpinball/vpinball-${VPINBALL_SHA}/plugins/plugins"
fi

rm -rf ../../../third-party/include/plugins
mkdir -p ../../../third-party/include/plugins
cp "${VPINBALL_PLUGINS}"/*.h ../../../third-party/include/plugins/

#
# CEF binary distribution and libcef_dll_wrapper
#

CEF_EXPECTED="${CEF_VERSION}-${BUILD_TYPE}"
CEF_FOUND="$([ -f cef/cache.txt ] && cat cef/cache.txt || echo "")"

if [ "${CEF_EXPECTED}" != "${CEF_FOUND}" ]; then
   echo "Building CEF. Expected: ${CEF_EXPECTED}, Found: ${CEF_FOUND}"

   CEF_NAME="cef_binary_${CEF_VERSION}_${CEF_PLATFORM}_minimal"
   rm -rf cef "${CEF_NAME}"
   curl -sfL "https://cef-builds.spotifycdn.com/${CEF_NAME//+/%2B}.tar.bz2" -o cef.tar.bz2
   echo "${CEF_SHA1}  cef.tar.bz2" | sha1sum -c -
   tar xjf cef.tar.bz2
   rm cef.tar.bz2
   mv "${CEF_NAME}" cef
   cmake -S cef -B cef/build -DCMAKE_BUILD_TYPE=${BUILD_TYPE}
   cmake --build cef/build --target libcef_dll_wrapper -- -j${NUM_PROCS}
   echo "${CEF_EXPECTED}" > cef/cache.txt
fi
