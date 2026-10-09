#!/usr/bin/env bash

TARGETS=(
    "cdn http://example.com"
    "utah http://mirrors.xmission.com/debian"
    "berkeley http://mirrors.ocf.berkeley.edu/debian"
    "rochester http://mirrors.rit.edu/debian"
    "germany http://ftp.fau.de/debian"
    "japan http://ftp.jaist.ac.jp/debian"
)

usage() {
    echo "Usage: $0 {laptop|onyx|codespace}" >&2
}

if [[ $# -ne 1 ]]; then
    usage
    exit 1
fi

vantage=$1
case "$vantage" in
    laptop|onyx|codespace) ;;
    *)
        usage
        exit 1
        ;;
esac

exec > >(tee "measurements-${vantage}.txt") 2>&1

SECONDS=0
failed_targets=0
public_file=
first_file=
second_file=
download_file=
first_log=
second_log=

cleanup() {
    rm -f -- "$public_file" "$first_file" "$second_file" "$download_file" "$first_log" "$second_log"
}
trap cleanup EXIT

make_temp() {
    mktemp "./.measure-${1}.XXXXXX"
}

make
build_status=$?
if (( build_status != 0 )); then
    echo "make failed with exit status $build_status"
    exit "$build_status"
fi

public_file=$(make_temp public) || exit 1
first_file=$(make_temp first) || exit 1
second_file=$(make_temp second) || exit 1
download_file=$(make_temp packages) || exit 1
first_log=$(make_temp first-log) || exit 1
second_log=$(make_temp second-log) || exit 1

echo "=== x1 measure.sh  vantage=$vantage ==="
echo "date      $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "hostname  $(hostname)"
echo "uname     $(uname -a)"
echo "commit    $(git rev-parse --short HEAD)"
if command -v ip >/dev/null 2>&1; then
    ip -4 addr
elif command -v ifconfig >/dev/null 2>&1; then
    ifconfig
else
    echo "Neither ip nor ifconfig is available; IPv4 interface addresses were not captured." >&2
fi

section() {
    printf '\n=== %s  %s ===\n' "$1" "$(date +%H:%M:%S)"
}

base_for() {
    local wanted=$1 target name base
    for target in "${TARGETS[@]}"; do
        read -r name base <<< "$target"
        if [[ $name == "$wanted" ]]; then
            printf '%s\n' "$base"
            return 0
        fi
    done
    return 1
}

report_failure() {
    local description=$1 status=$2
    if (( status != 0 )); then
        echo "FAILED: $description (exit status $status)"
        failed_targets=$((failed_targets + 1))
    fi
}

section "public address"
./pageload -4 -o "$public_file" http://checkip.amazonaws.com/
status=$?
report_failure "public address" "$status"
if (( status == 0 )); then
    cat "$public_file"
fi

for target in "${TARGETS[@]}"; do
    read -r name base <<< "$target"
    section "latency $name"
    if [[ $name == cdn ]]; then
        ./pageload -4 -n 5 "$base"
    else
        ./pageload -4 -n 5 "$base/README"
    fi
    status=$?
    report_failure "latency $name" "$status"
done

for name in utah japan; do
    base=$(base_for "$name") || exit 1
    section "download $name"
    ./pageload -4 -n 3 -o "$download_file" "$base/dists/stable/main/binary-amd64/Packages.gz"
    status=$?
    report_failure "download $name" "$status"
done

section "two at once"
utah_base=$(base_for utah) || exit 1
./pageload -4 -o "$first_file" "$utah_base/README" >"$first_log" 2>&1 &
first_pid=$!
./pageload -4 -o "$second_file" "$utah_base/README" >"$second_log" 2>&1 &
second_pid=$!
wait "$first_pid"
first_status=$?
wait "$second_pid"
second_status=$?
echo "First pageload:"
cat "$first_log"
report_failure "two at once, first pageload" "$first_status"
echo "Second pageload:"
cat "$second_log"
report_failure "two at once, second pageload" "$second_status"
echo "First file:"
cat "$first_file"
echo "Second file:"
cat "$second_file"

section "dns trace"
if command -v dig >/dev/null 2>&1; then
    echo "Note: DNS trace may fail on Onyx because it blocks DNS to servers other than the campus resolver."
    dig +trace ftp.jaist.ac.jp
    status=$?
    if (( status != 0 )); then
        echo "DNS trace failed with exit status $status; continuing."
    fi
else
    echo "Note: dig is not installed, so the DNS trace cannot run."
fi

echo
echo "=== done in $SECONDS s, $failed_targets failed ==="
