#!/usr/bin/env bash
set -u
task_root=/home/ubuntu/index-accel-validation
test "$(hostname)" = brev-v6nc08sjs || exit 1
task_label=$1
shift
case "$task_label" in *[!a-zA-Z0-9_-]*|'') exit 1;; esac
exec 9>"$task_root/remote-owner.lock"
flock -n 9 || { echo 'another lane job is active' >&2; exit 1; }
test ! -e "$task_root/$task_label.started" || exit 1
date -u +%FT%TZ > "$task_root/$task_label.started"
"$@" > "$task_root/$task_label.log" 2>&1
task_exit=$?
printf '%s\n' "$task_exit" > "$task_root/$task_label.exit"
date -u +%FT%TZ > "$task_root/$task_label.completed"
exit "$task_exit"
