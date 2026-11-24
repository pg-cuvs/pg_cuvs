import subprocess
import sys
import time
from pathlib import Path

db, log_path = sys.argv[1:]
log = Path(log_path)
psql = ["psql", "-XqAt", "-v", "ON_ERROR_STOP=1", "-d", db]

def sql(text):
    return subprocess.check_output(psql + ["-c", text], text=True).strip()


def wait_until(check, what, seconds=20):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if check():
            return
        time.sleep(0.03)
    raise RuntimeError(f"timed out waiting for {what}")


def concurrent_validation(label, incoming, expect_error):
    table = f"cic_{label}"
    idx = f"{table}_idx"
    sql(f"""
        CREATE TABLE {table} (id int, v vector);
        INSERT INTO {table} VALUES (1, '[1,0,0]'), (2, '[2,0,0]');
    """)
    guard = subprocess.Popen(psql, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    builder = None
    try:
        guard.stdin.write("SELECT pg_advisory_lock(7471);\n\\echo locked\n")
        guard.stdin.flush()
        while guard.stdout.readline().strip() != "locked":
            if guard.poll() is not None:
                raise RuntimeError("lock session exited")
        builder = subprocess.Popen(psql + ["-c",
            f"CREATE INDEX CONCURRENTLY {idx} ON {table} USING cagra(page_test_gate(v) vector_l2_ops);"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        wait_until(lambda: sql("SELECT count(*) FROM pg_locks WHERE locktype='advisory' AND objid=7471 AND NOT granted;") == "1",
                   "CIC heap scan at the expression gate")
        if sql(f"SELECT indisready FROM pg_index WHERE indexrelid='{idx}'::regclass;") != "f":
            raise RuntimeError("fixture entered after the first CIC heap scan")
        sql(f"INSERT INTO {table} VALUES (3, '{incoming}');")
        guard.stdin.write("SELECT pg_advisory_unlock(7471);\n")
        guard.stdin.flush()
        stdout, stderr = builder.communicate(timeout=60)
        if expect_error:
            if builder.returncode == 0 or "expected 3 dimensions, not 2" not in stderr:
                raise RuntimeError(f"CIC should reject the mismatched INSERT: {stdout} {stderr}")
        elif builder.returncode:
            raise RuntimeError(f"CIC failed: {stdout} {stderr}")
    finally:
        guard.stdin.close()
        guard.wait(timeout=20)
        if builder is not None and builder.poll() is None:
            builder.terminate()
            builder.communicate(timeout=10)

    if expect_error:
        if sql(f"SELECT indisvalid FROM pg_index WHERE indexrelid='{idx}'::regclass;") != "f":
            raise RuntimeError("a failed CIC became a usable index")
    else:
        state = sql(f"""
            SELECT i.indisvalid, get_byte(get_raw_page('{idx}',0),72),
                   get_byte(get_raw_page('{idx}',0),96),
                   (page_header(get_raw_page('{idx}',65))).lower
            FROM pg_index i WHERE i.indexrelid='{idx}'::regclass;
        """)
        if state != "t|2|1|30":
            raise RuntimeError(f"CIC did not add the post-snapshot row in validation: {state}")
        if sql(f"""
             SET enable_seqscan=off;
             SELECT id FROM {table} ORDER BY page_test_gate(v) <-> '{incoming}' LIMIT 1;
        """) != "3":
            raise RuntimeError("CIC did not find the post-snapshot row")
    print(f"CIC {label} PASS")


def canceled_vacuum():
    sql("""
        CREATE TABLE vac_interrupted (id int, v vector(3));
        INSERT INTO vac_interrupted SELECT i, array_fill(i::real,ARRAY[3])::vector
            FROM generate_series(1,10) AS i;
        CREATE INDEX vac_interrupted_idx ON vac_interrupted USING cagra(v vector_l2_ops);
        DELETE FROM vac_interrupted WHERE id=1;
    """)
    proc = subprocess.Popen(psql, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE,text=True)
    size=log.stat().st_size
    try:
        proc.stdin.write("SET cuvs.test_pause_page_vacuum=on;\nSELECT pg_backend_pid();\n")
        proc.stdin.flush()
        pid=int(proc.stdout.readline().strip())
        proc.stdin.write("VACUUM (INDEX_CLEANUP ON,TRUNCATE OFF) vac_interrupted;\n")
        proc.stdin.flush()
        def paused():
            with log.open() as f:
                f.seek(size)
                return "page-format VACUUM paused before publishing its generation" in f.read()
        wait_until(paused, "VACUUM after data records and before G")
        before=sql("""
            SELECT get_byte(get_raw_page('vac_interrupted_idx',0),96),
                   get_byte(get_raw_page('vac_interrupted_idx',1),8172),
                   (SELECT lp_flags FROM heap_page_items(get_raw_page('vac_interrupted',0)) WHERE lp=1);
        """)
        if before!="0|1|3":
            raise RuntimeError(f"unexpected in-flight VACUUM state: {before}")
        if sql(f"SELECT pg_cancel_backend({pid});")!="t":
            raise RuntimeError("could not cancel VACUUM")
        _, err=proc.communicate(timeout=15)
        if proc.returncode == 0 or "canceling statement due to user request" not in err:
            raise RuntimeError(f"VACUUM did not cancel at the gate: {err}")
    finally:
        if proc.poll() is None:
            proc.terminate()
            proc.communicate(timeout=10)

    sql("VACUUM (INDEX_CLEANUP ON,TRUNCATE OFF) vac_interrupted;")
    after=sql("""
        SELECT get_byte(get_raw_page('vac_interrupted_idx',0),96),
               get_byte(get_raw_page('vac_interrupted_idx',0),104),
               get_byte(get_raw_page('vac_interrupted_idx',1),8172),
               (SELECT lp_flags FROM heap_page_items(get_raw_page('vac_interrupted',0)) WHERE lp=1);
    """)
    if after!="1|1|1|0":
        raise RuntimeError(f"retry VACUUM lost G or its already invalidated slot: {after}")
    print(f"canceled VACUUM PASS ({before} -> {after})")


sql("""
    CREATE FUNCTION page_test_gate(x vector) RETURNS vector
    LANGUAGE plpgsql IMMUTABLE STRICT AS $$
    BEGIN
        PERFORM pg_advisory_xact_lock_shared(7471);
        RETURN x;
    END
    $$;
""")
concurrent_validation("normal", "[0,0,0]", False)
concurrent_validation("dimension", "[0,0]", True)
canceled_vacuum()
