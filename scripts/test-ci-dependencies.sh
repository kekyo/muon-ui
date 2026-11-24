#!/usr/bin/env bash
# Exercise CI acquisition failures without changing the runner's APT state.
set -euo pipefail
script="$(dirname "$0")/install-ci-dependencies.sh"
fixture=$(mktemp -d)
trap 'rm -rf -- "$fixture"' EXIT
export fixture PATH="$fixture:$PATH" MUON_CI_APT_FETCH_TIMEOUT=1s
cat > "$fixture/dpkg" <<'EOF'
#!/usr/bin/env bash
[[ $* == '--add-architecture i386' ]]
EOF
cat > "$fixture/apt-get" <<'EOF'
#!/usr/bin/env bash
set -eu
mirror=runner
if [[ -n ${APT_CONFIG:-} ]]; then mirror=ubuntu-archive; fi
case " $* " in
  *" update "*)
    echo "$mirror update" >> "$fixture/events"
    if [[ $scenario == both-unavailable ]]; then exit 100; fi
    ;;
  *" --download-only "*)
    echo "$mirror fetch" >> "$fixture/events"
    if [[ $scenario == stalled && $mirror == runner ]]; then sleep 4; exit 100; fi
    [[ " $* " == *" wine32:i386 "* ]]
    touch "$fixture/archives"
    ;;
  *" --no-download "*)
    echo "$mirror install" >> "$fixture/events"
    [[ -f $fixture/archives ]]
    if [[ $scenario == install-failed ]]; then exit 42; fi
    # Installation must be allowed to outlive the acquisition deadline.
    sleep 2
    ;;
  *) exit 1 ;;
esac
EOF
chmod +x "$fixture"/{apt-get,dpkg}
unset APT_CONFIG
export scenario
for scenario in normal stalled both-unavailable install-failed; do
  : > "$fixture/events"
  rm -f "$fixture/archives"
  status=0
  bash "$script" wine32:i386 > "$fixture/output" 2>&1 || status=$?
  case $scenario in
    normal|install-failed)
      expected=$'runner update\nrunner fetch\nrunner install'
      if [[ $scenario == normal ]]; then expected_status=0; else expected_status=42; fi
      ;;
    stalled)
      expected=$'runner update\nrunner fetch\nubuntu-archive update\nubuntu-archive fetch\nubuntu-archive install'
      expected_status=0
      grep -q 'timed out' "$fixture/output"
      ;;
    both-unavailable)
      expected=$'runner update\nubuntu-archive update'
      expected_status=100
      ;;
  esac
  diff -u <(echo "$expected") "$fixture/events"
  if [[ $status != "$expected_status" ]]; then cat "$fixture/output" >&2; exit 1; fi
  echo "$scenario passed"
done
