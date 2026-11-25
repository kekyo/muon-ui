#!/usr/bin/env bash
# Acquire all build dependencies before handing the archives to dpkg.
set -euo pipefail

dpkg --add-architecture i386
directory=$(mktemp -d)
trap 'rm -rf -- "$directory"' EXIT
chmod 755 "$directory"
. /etc/os-release

# The Ubuntu runner uses regional mirrors. Keep its release, security pocket,
# components, archive key and both architectures when falling back to Canonical.
cat > "$directory/ubuntu.sources" <<EOF
Types: deb
URIs: https://archive.ubuntu.com/ubuntu
Suites: ${VERSION_CODENAME:?} ${VERSION_CODENAME}-updates ${VERSION_CODENAME}-backports
Components: main restricted universe multiverse
Architectures: amd64 i386
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

Types: deb
URIs: https://security.ubuntu.com/ubuntu
Suites: ${VERSION_CODENAME}-security
Components: main restricted universe multiverse
Architectures: amd64 i386
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF
cat > "$directory/fallback.conf" <<EOF
Dir::Etc::sourcelist "$directory/ubuntu.sources";
Dir::Etc::sourceparts "-";
APT::Get::List-Cleanup "false";
EOF

deadline=${MUON_CI_APT_FETCH_TIMEOUT:-300s}
for mirror in runner ubuntu-archive; do
  if [[ $mirror == ubuntu-archive ]]; then
    export APT_CONFIG="$directory/fallback.conf"
  fi
  started=$SECONDS
  echo "Fetching from $mirror (deadline $deadline)"
  if timeout --kill-after=10s "$deadline" bash -ec '
    apt-get -o APT::Update::Error-Mode=any update
    apt-get -y --no-install-recommends --download-only install "$@"
  ' bash "$@"; then
    echo "Fetched from $mirror in $((SECONDS - started))s. Installing cached packages."
    apt-get -y --no-install-recommends --no-download install "$@"
    exit 0
  else
    status=$?
    if [[ $status == 124 || $status == 137 ]]; then
      echo "Fetch from $mirror timed out after $((SECONDS - started))s (exit $status)" >&2
    else
      echo "Fetch from $mirror failed after $((SECONDS - started))s (exit $status)" >&2
    fi
  fi
done
exit "$status"
