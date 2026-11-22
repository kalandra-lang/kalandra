#!/usr/bin/env bash
# Install the compiler's exact header-only dependency in the runner's workspace.
set -euo pipefail
work=$(mktemp -d "${RUNNER_TEMP:?}/tomlplusplus.XXXXXX")
cd "$work"
curl --fail --silent --show-error --location --retry 3 \
  https://codeload.github.com/marzer/tomlplusplus/tar.gz/refs/tags/v3.4.0 -o source.tar.gz
echo '8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155  source.tar.gz' | shasum -a 256 -c -
tar -xzf source.tar.gz
cmake -S tomlplusplus-3.4.0 -B build -G Ninja -DCMAKE_INSTALL_PREFIX="$work/install" -DCMAKE_INSTALL_LIBDIR=lib
cmake --install build
echo "KALANDRA_TOML_CMAKE_DIR=$work/install/lib/cmake/tomlplusplus" >> "${GITHUB_ENV:?}"
