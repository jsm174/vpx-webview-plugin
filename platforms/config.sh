#!/bin/bash

set -e

VPINBALL_SHA=b8584785552ec0a046e50691d95845fca3418331
CEF_VERSION=156.0.28+g57389f0+chromium-156.0.8078.88

if [ -z "${BUILD_TYPE}" ]; then
   BUILD_TYPE="Release"
fi

echo "Build type: ${BUILD_TYPE}"
echo ""
