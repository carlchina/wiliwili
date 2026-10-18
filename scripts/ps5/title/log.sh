#!/usr/bin/env bash
# wiliwili - PS5 native title: fetch what the console recorded about a run.
#
# A title's output has no reader, so the application redirects it into
# /data/wiliwili/wiliwili-title.log. The loader and the system print their own
# messages to the kernel log, which the console serves on port 3232 while a
# klog payload is running.
#
# Usage:
#   scripts/ps5/title/log.sh [-h HOST] [-p PORT] [-k SECONDS] [-o FILE]
#
# Options:
#   -h HOST       console address (default: $PS5_HOST or "ps5")
#   -p PORT       FTP port of the console (default: 2121), the application log
#   -k SECONDS    also read the kernel log for this long (default: 3, 0 to skip)
#   -o FILE       save the application log to FILE (default: only print it)

set -euo pipefail

host=${PS5_HOST:-ps5}
port=2121
klog_seconds=3
output=""

while getopts "h:p:k:o:" opt; do
    case ${opt} in
        h) host=$OPTARG ;;
        p) port=$OPTARG ;;
        k) klog_seconds=$OPTARG ;;
        o) output=$OPTARG ;;
        *) echo "usage: $0 [-h HOST] [-p PORT] [-k SECONDS] [-o FILE]" >&2; exit 2 ;;
    esac
done

echo "==> [title] /data/wiliwili/wiliwili-title.log on ${host}" >&2
log=$(curl --silent --show-error --fail --max-time 30 \
    "ftp://${host}:${port}/data/wiliwili/wiliwili-title.log") || {
    echo "the log could not be read; the title may never have started" >&2
    exit 1
}
printf '%s\n' "${log}"
if [[ -n ${output} ]]; then
    printf '%s\n' "${log}" > "${output}"
    echo "==> [title] Saved to ${output}" >&2
fi

if [[ ${klog_seconds} != 0 ]]; then
    echo "==> [title] Kernel log of ${host}:3232 (${klog_seconds}s)" >&2
    if command -v nc > /dev/null; then
        nc -w "${klog_seconds}" "${host}" 3232 2>/dev/null | tail -n 60 || true
    else
        echo "nc is not installed, skipping the kernel log" >&2
    fi
fi
