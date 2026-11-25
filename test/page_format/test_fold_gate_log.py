"""Independent synthetic logs for the fold gate summarizer."""

from pathlib import Path
import tempfile
import unittest

from fold_gate_log import pre_ready_gate, publication_windows, read_records, requests_from


IDENTITY = "db=2 index=3 gpu=0 daemon_id=17 system_identifier=21 boot_id=14 index_nonce=27 graph_nonce=30"
PROTECTED = f"[INFO] [page_fold_protected] {IDENTITY} mono_us=19"
READY = f"[INFO] [page_fold_ready] {IDENTITY} protected_us=19 send_started_us=20 mono_us=25 sent=1"
COMMIT = f"[INFO] [page_fold_commit] {IDENTITY} mono_us=80 accepted_us=84 lsn=AB"
TERMINATE = f"[INFO] [page_fold_end] {IDENTITY} mono_us=80 reason=pg_lifetime"
START = "[INFO] [page_fold_request] op=BUILD db=7 index=8 daemon_id=17 request_id=1 mono_us=28"
REJECT = ("[INFO] [page_fold_foreground] op=BUILD db=7 index=8 daemon_id=17 request_id=1 started_us=28 "
          "mono_us=40 response=sent status=2 error_hex=4f4f4d err_out_hex=415238")
GPU_BEGIN = "[INFO] [page_fold_begin] gpu=0 db=2 index=3 daemon_id=17 mono_us=12"
GPU_END = "[INFO] [page_fold] gpu=0 db=2 index=3 daemon_id=17 started_us=12 mono_us=35"
WAIT = ("[INFO] [page_fold_admission] op=BUILD db=7 index=8 daemon_id=17 request_id=1 "
        "wait_started_us=30 wait_us=7 withdrawn=0 mono_us=37")


def summarize(*lines):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "daemon.log"
        path.write_text("\n".join(lines) + "\n")
        records = read_records([path])
    requests = list(requests_from(records))
    windows = list(publication_windows(records, requests))
    return windows, pre_ready_gate(records, requests, windows, ["BUILD"])


class FoldGate(unittest.TestCase):
    def test_ready_redelivery_counts_one_window_and_preserves_refusal(self):
        retry = READY.replace("send_started_us=20 mono_us=25", "send_started_us=60 mono_us=62")
        windows, _ = summarize(COMMIT, START, retry, REJECT, READY, PROTECTED)
        window, = windows
        self.assertEqual(window["state"], "committed")
        self.assertEqual(window["ready_sent_us"], 25)
        self.assertEqual(window["protected_us"], 19)
        self.assertEqual(window["pre_send_us"], 1)
        self.assertEqual(window["observed_admission_rejections"], 1)
        self.assertEqual(window["requests"][0]["error"], "OOM")
        self.assertEqual(window["requests"][0]["err_out"], "AR8")

    def test_interrupted_or_unproven_windows_never_prove_zero(self):
        for lines, state in (
            ((PROTECTED, READY), "open"),
            ((PROTECTED, READY, TERMINATE), "pg_lifetime"),
            ((PROTECTED, READY.replace("sent=1", "sent=0"), COMMIT), "send_failed"),
            ((PROTECTED, READY, COMMIT.replace("mono_us=80", "mono_us=24")), "delivery_overlap"),
            ((COMMIT,), "missing_ready"),
            ((PROTECTED,), "missing_ready"),  # never returned from READY send
            ((READY, COMMIT), "uncertain_protection"),
            ((PROTECTED, READY, COMMIT), "committed"),  # no requests at all
        ):
            with self.subTest(state=state):
                windows, _ = summarize(*lines)
                self.assertEqual(windows[0]["state"], state)
                self.assertIsNone(windows[0]["observed_failures"])

    def test_same_oids_in_another_boot_do_not_join(self):
        windows, _ = summarize(PROTECTED, READY, COMMIT.replace("boot_id=14", "boot_id=15"))
        self.assertEqual({window["state"] for window in windows}, {"open", "missing_ready"})

    def test_only_authoritative_preflight_refusal_counts(self):
        start = START.replace("op=BUILD", "op=HNSW_PREFLIGHT")
        reply = REJECT.replace("op=BUILD", "op=HNSW_PREFLIGHT").replace("status=2", "status=13")
        reply = reply.replace("err_out_hex=415238", "err_out_hex=")
        windows, _ = summarize(PROTECTED, READY, COMMIT, start, reply)
        self.assertEqual(windows[0]["observed_admission_rejections"], 1)
        windows, _ = summarize(PROTECTED, READY, COMMIT, start, reply.replace("op=HNSW_PREFLIGHT", "op=HNSW_POSTSCAN"))
        self.assertIsNone(windows[0]["observed_admission_rejections"])

    def test_failure_before_ready_is_not_exempted(self):
        protected = PROTECTED.replace("mono_us=19", "mono_us=38")
        later = READY.replace("protected_us=19 send_started_us=20 mono_us=25",
                              "protected_us=38 send_started_us=50 mono_us=51")
        windows, gate = summarize(GPU_BEGIN, GPU_END, START, WAIT, REJECT, protected, later, COMMIT)
        self.assertEqual(gate["status"], "FAIL")
        self.assertEqual(gate["failures"][0]["status"], 2)
        window, = windows
        self.assertEqual(window["pre_send_us"], 12)
        self.assertIsNone(window["observed_failures"])
        self.assertEqual(window["unclassified_requests"][0]["request_id"], "1")

    def test_no_gate_failure_when_already_protected_before_wait(self):
        protected = PROTECTED.replace("mono_us=19", "mono_us=27")
        later = READY.replace("protected_us=19 send_started_us=20 mono_us=25",
                              "protected_us=27 send_started_us=50 mono_us=51")
        windows, gate = summarize(GPU_BEGIN, GPU_END, START, WAIT, REJECT, protected, later, COMMIT)
        self.assertEqual(gate["status"], "INCOMPLETE")
        self.assertFalse(gate["failures"])
        self.assertEqual(len(windows[0]["unclassified_requests"]), 1)

    def test_arrival_before_protection_still_fails_if_wait_registered_later(self):
        protected = PROTECTED.replace("mono_us=19", "mono_us=29")
        later = READY.replace("protected_us=19 send_started_us=20 mono_us=25",
                              "protected_us=29 send_started_us=50 mono_us=51")
        _, gate = summarize(GPU_BEGIN, GPU_END, START, WAIT, REJECT, protected, later, COMMIT)
        self.assertEqual(gate["status"], "FAIL")

    def test_commit_lock_delay_does_not_extend_the_wire_window(self):
        late = REJECT.replace("mono_us=40", "mono_us=82")
        windows, _ = summarize(PROTECTED, READY, COMMIT, START, late)
        self.assertEqual(windows[0]["protection_end_us"], 84)
        self.assertIsNone(windows[0]["observed_failures"])
        self.assertEqual(len(windows[0]["unclassified_requests"]), 1)

    def test_positive_wait_required_and_ready_pressure_excluded(self):
        success = REJECT.replace("status=2 error_hex=4f4f4d err_out_hex=415238", "status=0 error_hex= err_out_hex=")
        for lines, state in (
            ((GPU_BEGIN, GPU_END, START, WAIT, success), "PASS"),
            ((GPU_BEGIN, GPU_END, START, success), "INCOMPLETE"),
            ((GPU_BEGIN, START, WAIT, success), "INCOMPLETE"),
            ((GPU_BEGIN, GPU_END, START, WAIT, success.replace("response=sent", "response=send_failed")), "INCOMPLETE"),
            ((GPU_BEGIN, GPU_END, START, WAIT, REJECT, PROTECTED, READY, COMMIT), "INCOMPLETE"),
        ):
            with self.subTest(state=state, lines=lines):
                _, gate = summarize(*lines)
                self.assertEqual(gate["status"], state)


if __name__ == "__main__":
    unittest.main()
