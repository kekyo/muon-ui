#!/bin/bash
set -euo pipefail

npm install
npm run test
npm run build:dist --workspace muon-ui

ctest --test-dir muon-core/.build/dist/linux64/release --output-on-failure

ctest --test-dir muon-core/.build/dist/windows32/release --output-on-failure

ctest --test-dir muon-core/.build/dist/windows64/release --output-on-failure
