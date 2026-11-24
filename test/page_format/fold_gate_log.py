"""Correlate fold and foreground records from complete daemon logs."""

import argparse
from collections import defaultdict
import json
from pathlib import Path
import re


EVENT_TAGS = {"page_fold_protected", "page_fold_ready", "page_fold_commit", "page_fold_end"}
REQUEST_TAGS = {"page_fold_request", "page_fold_foreground", "page_fold_admission"}
FOLD_TAGS = {"page_fold_begin", "page_fold"}
EVENT_KEY = ("daemon_id", "system_identifier", "boot_id", "db", "index", "index_nonce", "graph_nonce")
PREFIX = re.compile(r"^\[(?:INFO|WARN)\] \[(\w+)\] (.*)$")
CANCELLED = 11


def read_records(paths):
    records = defaultdict(list)
    for path in paths:
        with path.open() as log:
            for lineno, line in enumerate(log, 1):
                match = PREFIX.match(line.rstrip("\n"))
                if not match or match[1] not in EVENT_TAGS | REQUEST_TAGS | FOLD_TAGS:
                    continue
                if match[1] == "page_fold" and not match[2].startswith("gpu="):
                    continue  # A fatal GPU completion leaves the active interval open.
                fields = dict(word.split("=", 1) for word in match[2].split())
                fields["source"] = f"{path}:{lineno}"
                int(fields["mono_us"])
                records[match[1]].append(fields)
    return records


def group(records, tags, key):
    groups = defaultdict(lambda: defaultdict(list))
    for tag in tags:
        for record in records[tag]:
            groups[tuple(record[field] for field in key)][tag].append(record)
    return groups


def single(records, label):
    if len(records) > 1:
        raise ValueError(f"ambiguous {label}: {[item['source'] for item in records]}")
    return records[0] if records else None


def requests_from(records):
    for (daemon, request), items in group(records, REQUEST_TAGS, ("daemon_id", "request_id")).items():
        if request == "0":  # Direct daemon-unit calls have no wire request.
            continue
        begin = single(items["page_fold_request"], "request start")
        reply = single(items["page_fold_foreground"], "terminal reply")
        if not begin:
            raise ValueError(f"request {daemon}/{request} lacks its start record")
        start = int(begin["mono_us"])
        end = int(reply["mono_us"]) if reply else None
        if reply and (int(reply["started_us"]) != start or end < start):
            raise ValueError(f"request {daemon}/{request} has invalid timestamps")
        status = int(reply["status"]) if reply else None
        known = bool(reply and reply["response"] == "sent" and status >= 0 and status != CANCELLED)
        op = reply["op"] if reply else begin["op"]
        # The backend can reject only from a status 5/13 pre-spool forecast.
        forecast_reject = known and op == "HNSW_PREFLIGHT" and status in (5, 13)
        forecast_only = known and (op == "HNSW_POSTSCAN" or (op == "HNSW_PREFLIGHT" and not forecast_reject))
        yield {"daemon_id": daemon, "request_id": request, "op": op,
               "db": begin["db"], "index": begin["index"], "started_us": start,
               "mono_us": end, "status": status, "known": known,
               "forecast_reject": forecast_reject, "forecast_only": forecast_only,
               "response": reply["response"] if reply else "open",
               "error_hex": reply["error_hex"] if reply else None,
               "err_out_hex": reply["err_out_hex"] if reply else None,
               "error": bytes.fromhex(reply["error_hex"]).decode(errors="replace") if reply else None,
               "err_out": bytes.fromhex(reply["err_out_hex"]).decode(errors="replace") if reply else None,
               "source": reply["source"] if reply else begin["source"], "waits": items["page_fold_admission"]}


def overlaps(start, end, other_start, other_end):
    return (other_end is None or start <= other_end) and (end is None or other_start <= end)


def publication_windows(records, requests):
    for key, items in group(records, EVENT_TAGS, EVENT_KEY).items():
        attempts = items["page_fold_ready"]
        protection = single(items["page_fold_protected"], "READY protection")
        commit = single(items["page_fold_commit"], "effective COMMITTED")
        stopped = single(items["page_fold_end"], "fold termination")
        protected = int(protection["mono_us"]) if protection else None
        started = min((int(r["send_started_us"]) for r in attempts), default=None)
        sent = min((int(r["mono_us"]) for r in attempts if r["sent"] == "1"), default=None)
        end = int((commit or stopped)["mono_us"]) if commit or stopped else None
        protection_end = int(commit["accepted_us"]) if commit else end

        if stopped:
            state = stopped["reason"]
        elif any(r["sent"] != "1" for r in attempts):
            state = "send_failed"
        elif sent is None:
            state = "missing_ready"
        elif protected is None or any(int(r["protected_us"]) != protected or
                                      not protected <= int(r["send_started_us"]) <= int(r["mono_us"])
                                      for r in attempts):
            state = "uncertain_protection"
        elif not commit:
            state = "open"
        elif end < sent or protection_end < end:
            state = "delivery_overlap"
        else:
            state = "committed"
        if protected is not None and protection_end is not None and protection_end < protected:
            state = "uncertain_protection"
            protection_end = None

        # Correlation uses the response boundary, never a causality claim.
        # An unknown or crossing response is retained but cannot prove zero.
        observed = []
        if protected is not None:
            for req in requests:
                if (req["daemon_id"] == key[0] and not req["forecast_only"] and
                        overlaps(req["started_us"], req["mono_us"], protected, protection_end)):
                    observed.append(req)
        inside = [req for req in observed if sent is not None and req["mono_us"] is not None
                  and sent <= req["mono_us"] <= end] if end is not None else []
        complete = (state == "committed" and bool(inside) and
                    all(req["known"] and req in inside for req in observed))
        yield {**dict(zip(EVENT_KEY, key)), "state": state, "attempts": attempts,
               "protected_us": protected, "send_started_us": started,
               "ready_sent_us": sent, "end_us": end, "protection_end_us": protection_end,
               "pre_send_us": started - protected if started is not None and protected is not None else None,
               "lsn": commit["lsn"] if commit else None,
               "observed_failures": sum(req["status"] != 0 for req in inside) if complete else None,
               "observed_admission_rejections": sum(bool(req["err_out"]) or req["forecast_reject"]
                                                    for req in inside) if complete else None,
               "requests": observed, "unclassified_requests": [r for r in observed
                   if state != "committed" or r not in inside or not r["known"]]}


def gpu_intervals(records):
    # The producer's clock is captured while changing its active flag. Log
    # line order can differ because the next request can run before logging.
    ends = {(r["daemon_id"], r["gpu"], r["started_us"]): int(r["mono_us"]) for r in records["page_fold"]}
    return [(r["daemon_id"], int(r["mono_us"]), ends.get((r["daemon_id"], r["gpu"], r["mono_us"])))
            for r in records["page_fold_begin"]]


def protection_overlaps(windows, daemon, start, end):
    return any(w["daemon_id"] == daemon and overlaps(start, end, w["protected_us"] or 0, w["protection_end_us"])
               for w in windows)


def pre_ready_gate(records, requests, windows, required):
    proven, failures, unknown = set(), [], []
    active = gpu_intervals(records)
    for req in requests:
        op = "HNSW" if req["op"] == "HNSW_PREFLIGHT" else req["op"]
        if req["forecast_only"] or op not in required:
            continue
        daemon, start, end = req["daemon_id"], req["started_us"], req["mono_us"]
        work = [(first, last) for owner, first, last in active
                if owner == daemon and overlaps(start, end, first, last)]
        if not work:
            continue
        if not req["known"] or any(last is None for _, last in work):
            unknown.append(req)
            continue

        # The gate follows an observed arrival at the active fold, even if
        # READY is protected later, before the eventual request reply.
        waited = False
        for wait in req["waits"]:
            if wait["withdrawn"] != "0" or int(wait["wait_us"]) <= 0:
                continue
            for first, last in work:
                overlap_start = max(first, int(wait["wait_started_us"]))
                arrived = start if first <= start < last else overlap_start
                if (overlap_start < min(last, int(wait["mono_us"])) and
                        not protection_overlaps(windows, daemon, arrived, arrived)):
                    waited = True
        if req["status"] != 0:
            active_refusal = (any(first <= end < last for first, last in work) and
                              not protection_overlaps(windows, daemon, start, end))
            if waited or active_refusal:
                failures.append(req)
            else:
                unknown.append(req)
        if waited and req["status"] == 0:
            proven.add(req["op"])
        elif req["status"] == 0 and protection_overlaps(windows, daemon, start, end):
            unknown.append(req)
    missing = sorted(set(required) - proven)
    return {"status": "FAIL" if failures else "INCOMPLETE" if missing or unknown else "PASS",
            "failures": failures, "unclassified": unknown, "missing_successful_wait": missing}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--require-pre-ready", nargs="+", metavar="OP",
                        help="Require one-device foreground overlap and successful replies for these operations")
    args = parser.parse_args()
    records = read_records(args.logs)
    requests = list(requests_from(records))
    windows = list(publication_windows(records, requests))
    result = {"windows": windows, "foreground_requests": requests}
    if args.require_pre_ready:
        result["pre_ready_gate"] = pre_ready_gate(records, requests, windows, args.require_pre_ready)
    print(json.dumps(result, indent=2))
    if args.require_pre_ready and result["pre_ready_gate"]["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
