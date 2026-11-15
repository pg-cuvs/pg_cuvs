#!/usr/bin/env python3
"""RLS execution-order regression in an explicitly owned PG16 fixture.

Requires an administrator in a disposable local cluster, installed/preloaded
pg_cuvs and vector, and existing pg_read_all_data with neither superuser nor
bypassrls. Creates only a fresh transactional schema, lowers the session role,
and rolls back; it never creates/grants roles or changes existing policies.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import threading
from test_connectivity_pg_interrupts import Session


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ["output", "pg-bin", "library-dir"]:
        parser.add_argument("--" + name, type=Path, required=name != "library-dir")
    for name in ["schema", "socket", "port", "user", "expected-data-directory"]:
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--database", default="postgres")
    parser.add_argument("--expected-library-sha")
    args = parser.parse_args()
    if not re.fullmatch(r"[a-z][a-z0-9_]{0,35}", args.schema):
        parser.error("schema must be a simple lower-case identifier, max 36 characters")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    schema = args.schema
    libdir = args.library_dir or args.pg_bin.parent / "lib"
    library_sha = hashlib.sha256((libdir / "pg_cuvs.so").read_bytes()).hexdigest()
    if args.expected_library_sha and library_sha != args.expected_library_sha:
        parser.error("installed ELF differs from expected SHA")
    command = [str(args.pg_bin / "psql"), "-XqAt", "-v", "ON_ERROR_STOP=1",
               "-h", args.socket, "-p", args.port, "-U", args.user, "-d", args.database]
    transcript = []
    result = {"cases": [], "candidate_sha256": library_sha,
              "pgvector_sha256": hashlib.sha256((libdir / "vector.so").read_bytes()).hexdigest()}
    actor = control = None
    started = False
    repo = Path(__file__).resolve().parents[2]
    result["source_sha256"] = {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in
        [repo / "src/cuvs_hnsw_rls.c", repo / "src/cuvs_hnsw_scan.c",
         repo / "src/cuvs_hnsw_exact.c", Path(__file__)]}

    def ask(session, sql):
        sql = sql.replace("fixture.", schema + ".").replace("SCHEMA fixture;", "SCHEMA " + schema + ";")
        lines = session.ask(sql)
        transcript.append({"sql": sql, "output": lines})
        return lines

    def value(session, sql):
        return json.loads(ask(session, sql)[-1])

    def stage(tag):
        ask(actor, "SET LOCAL rls_probe.tag='" + tag + "';")
        return len(actor.errors)

    def finish(tag, offset):
        ask(actor, "DO $$BEGIN RAISE NOTICE 'RLS_END " + tag + "';END$$;")
        assert actor.wait_error("RLS_END " + tag, 10), actor.errors
        return actor.errors[offset:]

    def plan_nodes(plan):
        yield plan
        for child in plan.get("Plans", []):
            yield from plan_nodes(child)

    def explain(query, mode="on", analyze=True):
        ask(actor, "SET LOCAL cuvs.hnsw_exact_scan=" + mode + ";")
        options = "ANALYZE," if analyze else ""
        lines = ask(actor, "EXPLAIN(" + options + "FORMAT JSON,COSTS OFF) " + query + ";")
        return json.loads("\n".join(lines))[0]["Plan"]

    def native(plan):
        assert any(n["Node Type"] == "Seq Scan" for n in plan_nodes(plan)), plan
        assert any(n["Node Type"] == "Sort" for n in plan_nodes(plan)), plan
        assert not any(n["Node Type"] in ("Index Scan", "Index Only Scan") for n in plan_nodes(plan)), plan

    def expression_notices(lines):
        return [x for x in lines if "INDEX_EXPR role=pg_read_all_data" in x]

    def no_hidden(lines, allowed=("[10,0]",)):
        for line in expression_notices(lines):
            assert any("value=" + item + " " in line for item in allowed), line

    def query_case(name, query, expected, allowed=("[10,0]",), mode="on"):
        offset = stage(name + "_plan")
        plan = explain(query, mode)
        plan_errors = finish(name + "_plan", offset)
        if mode == "on":
            native(plan)
        no_hidden(plan_errors, allowed)
        offset = stage(name)
        actual = value(actor, "SELECT jsonb_agg(x) FROM(" + query + ")x;")
        errors = finish(name, offset)
        assert actual == expected, (name, actual, expected)
        no_hidden(errors, allowed)
        result["cases"].append({"name": name, "verdict": "PASS", "plan": plan,
                                "rows": actual, "notices": errors})
        return errors

    try:
        actor, control = Session(command), Session(command)
        assert ask(actor, "SHOW data_directory;") == [args.expected_data_directory]
        precondition = value(actor, "SELECT jsonb_build_object('actor',current_user,"
            "'admin',(SELECT rolsuper FROM pg_roles WHERE rolname=current_user),"
            "'superuser',rolsuper,'bypassrls',rolbypassrls,"
            "'existing_schema',(SELECT count(*) FROM pg_namespace WHERE nspname='" + schema + "')) "
            "FROM pg_roles WHERE rolname='pg_read_all_data';")
        assert precondition and precondition["admin"] and not precondition["superuser"]
        assert not precondition["bypassrls"] and precondition["existing_schema"] == 0, precondition
        result["precondition"] = precondition
        ask(actor, "BEGIN;CREATE SCHEMA fixture;SET LOCAL rls_probe.tag='native';")
        started = True
        ask(actor, """
CREATE FUNCTION fixture.expr(v vector) RETURNS vector LANGUAGE plpgsql IMMUTABLE STRICT AS $$
DECLARE tag text:=current_setting('rls_probe.tag',true);
BEGIN RAISE NOTICE 'INDEX_EXPR role=% value=% stage=%',current_user,v,tag;
IF tag='allowed_error' THEN RAISE EXCEPTION 'allowed expression failure' USING ERRCODE='22012';END IF;
IF tag IN ('cancel','timeout') THEN PERFORM pg_sleep(0.6);END IF;
RETURN v;END$$;
CREATE FUNCTION fixture.blocked(v vector) RETURNS vector LANGUAGE plpgsql IMMUTABLE STRICT AS $$
BEGIN RAISE NOTICE 'BLOCKED_BODY role=%',current_user;RETURN v;END$$;
CREATE TABLE fixture.blocked_items(id int,v vector(2));
CREATE INDEX blocked_i ON fixture.blocked_items USING pg_cuvs_hnsw(((fixture.blocked(v))::vector(2)) vector_l2_ops);
INSERT INTO fixture.blocked_items VALUES(1,'[10,0]'),(2,'[0,0]');
REVOKE EXECUTE ON FUNCTION fixture.blocked(vector) FROM PUBLIC;
ALTER TABLE fixture.blocked_items ENABLE ROW LEVEL SECURITY;
ALTER TABLE fixture.blocked_items FORCE ROW LEVEL SECURITY;
CREATE POLICY blocked_rows ON fixture.blocked_items TO pg_read_all_data USING(id=1);
CREATE FUNCTION fixture.pred(i int) RETURNS boolean LANGUAGE plpgsql IMMUTABLE STRICT AS $$
BEGIN RAISE NOTICE 'INDEX_PRED role=% id=% stage=%',current_user,i,current_setting('rls_probe.tag',true);RETURN i<4;END$$;
CREATE FUNCTION fixture.observed(i int) RETURNS boolean LANGUAGE plpgsql VOLATILE STRICT AS $$
BEGIN RAISE NOTICE 'USER_QUAL role=% id=% stage=%',current_user,i,current_setting('rls_probe.tag',true);RETURN true;END$$;
CREATE FUNCTION fixture.toggle(i int) RETURNS boolean LANGUAGE plpgsql VOLATILE STRICT AS $$
BEGIN RAISE NOTICE 'TOGGLE_QUAL role=% id=% stage=%',current_user,i,current_setting('rls_probe.tag',true);PERFORM set_config('cuvs.hnsw_exact_scan','on',true);RETURN true;END$$;
CREATE FUNCTION fixture.acl_pred(i int) RETURNS boolean LANGUAGE plpgsql IMMUTABLE STRICT AS $$BEGIN RAISE NOTICE 'ACL_PRED role=%',current_user;RETURN i<3;END$$;
CREATE TABLE fixture.acl_items(id int,v vector(2));
CREATE INDEX acl_i ON fixture.acl_items USING pg_cuvs_hnsw(v vector_l2_ops) WHERE fixture.acl_pred(id);
INSERT INTO fixture.acl_items SELECT i,('['||i||',0]')::vector(2) FROM generate_series(1,5)i;
REVOKE EXECUTE ON FUNCTION fixture.acl_pred(int) FROM PUBLIC;
CREATE FUNCTION fixture.policy(i int) RETURNS boolean LANGUAGE plpgsql VOLATILE STRICT AS $$
BEGIN RAISE NOTICE 'POLICY_ROW role=% id=% stage=%',current_user,i,current_setting('rls_probe.tag',true);RETURN i=1;END$$;
CREATE TABLE fixture.expr_items(id int,v vector(2));
CREATE INDEX expr_i ON fixture.expr_items USING pg_cuvs_hnsw(((fixture.expr(v))::vector(2)) vector_l2_ops);
INSERT INTO fixture.expr_items VALUES(1,'[10,0]'),(2,'[0,0]'),(3,'[1,0]');
CREATE TABLE fixture.pred_items(id int,v vector(2));
CREATE INDEX pred_i ON fixture.pred_items USING pg_cuvs_hnsw(v vector_l2_ops) WHERE fixture.pred(id);
INSERT INTO fixture.pred_items SELECT * FROM fixture.expr_items;
INSERT INTO fixture.pred_items VALUES(4,'[0,0]');
CREATE TABLE fixture.implied(id int,v vector(2));
CREATE INDEX implied_i ON fixture.implied USING pg_cuvs_hnsw(v vector_l2_ops) WHERE id<3;
INSERT INTO fixture.implied SELECT i,('['||i||',0]')::vector(2) FROM generate_series(1,5)i;
ALTER TABLE fixture.implied ENABLE ROW LEVEL SECURITY;
ALTER TABLE fixture.implied FORCE ROW LEVEL SECURITY;
CREATE POLICY implied_rows ON fixture.implied TO pg_read_all_data USING(id<3);
CREATE TABLE fixture.nonleak(id int,v vector(2));
CREATE INDEX nonleak_i ON fixture.nonleak USING pg_cuvs_hnsw(v vector_l2_ops) WHERE fixture.pred(id);
INSERT INTO fixture.nonleak SELECT * FROM fixture.implied;
ALTER TABLE fixture.nonleak ENABLE ROW LEVEL SECURITY;
ALTER TABLE fixture.nonleak FORCE ROW LEVEL SECURITY;
CREATE POLICY nonleak_rows ON fixture.nonleak TO pg_read_all_data USING(id<3);
CREATE TABLE fixture.barrier_pred(id int,v vector(2));
CREATE INDEX barrier_pred_i ON fixture.barrier_pred USING pg_cuvs_hnsw(v vector_l2_ops) WHERE id<3;
INSERT INTO fixture.barrier_pred SELECT * FROM fixture.implied;
CREATE VIEW fixture.barrier_pred_view WITH(security_barrier=true) AS SELECT id,v FROM fixture.barrier_pred WHERE id<3 ORDER BY v <-> '[0,0]'::vector;
CREATE TABLE fixture.plain(id int,v vector(2));
CREATE INDEX plain_i ON fixture.plain USING pg_cuvs_hnsw(v vector_l2_ops);
INSERT INTO fixture.plain SELECT * FROM fixture.expr_items;
ALTER TABLE fixture.expr_items ENABLE ROW LEVEL SECURITY;
ALTER TABLE fixture.expr_items FORCE ROW LEVEL SECURITY;
ALTER TABLE fixture.pred_items ENABLE ROW LEVEL SECURITY;
ALTER TABLE fixture.pred_items FORCE ROW LEVEL SECURITY;
ALTER TABLE fixture.plain ENABLE ROW LEVEL SECURITY;
ALTER TABLE fixture.plain FORCE ROW LEVEL SECURITY;
CREATE POLICY expr_rows ON fixture.expr_items TO pg_read_all_data USING(fixture.policy(id));
CREATE POLICY pred_rows ON fixture.pred_items TO pg_read_all_data USING(id IN (1,4));
CREATE POLICY plain_rows ON fixture.plain TO pg_read_all_data USING(id=1);
CREATE VIEW fixture.invoker_view WITH(security_invoker=true) AS SELECT * FROM fixture.expr_items;
CREATE VIEW fixture.owner_view AS SELECT * FROM fixture.expr_items;
CREATE TABLE fixture.barrier_items(id int,v vector(2));
CREATE INDEX barrier_i ON fixture.barrier_items USING pg_cuvs_hnsw(((fixture.expr(v))::vector(2)) vector_l2_ops);
INSERT INTO fixture.barrier_items SELECT * FROM fixture.expr_items;
CREATE VIEW fixture.barrier_view WITH(security_barrier=true) AS SELECT id FROM fixture.barrier_items WHERE id=1 ORDER BY fixture.expr(v)::vector(2) <-> '[0,0]'::vector;

CREATE TABLE fixture.parts(id int,v vector(2)) PARTITION BY RANGE(id);
CREATE TABLE fixture.parts_a PARTITION OF fixture.parts FOR VALUES FROM(1) TO(4);
CREATE TABLE fixture.parts_b PARTITION OF fixture.parts FOR VALUES FROM(4) TO(7);
CREATE INDEX parts_a_i ON fixture.parts_a USING pg_cuvs_hnsw(((fixture.expr(v))::vector(2)) vector_l2_ops);
CREATE INDEX parts_b_i ON fixture.parts_b USING pg_cuvs_hnsw(((fixture.expr(v))::vector(2)) vector_l2_ops);
INSERT INTO fixture.parts SELECT * FROM fixture.expr_items;
INSERT INTO fixture.parts VALUES(4,'[0,0]'),(5,'[1,0]'),(6,'[2,0]');
ALTER TABLE fixture.parts ENABLE ROW LEVEL SECURITY;
ALTER TABLE fixture.parts FORCE ROW LEVEL SECURITY;
CREATE POLICY part_rows ON fixture.parts TO pg_read_all_data USING(id=1);
DO $$BEGIN IF EXISTS(SELECT 1 FROM pg_roles WHERE rolname='pg_read_all_data' AND(rolsuper OR rolbypassrls)) OR
EXISTS(SELECT 1 FROM pg_class c JOIN pg_namespace n ON c.relnamespace=n.oid WHERE n.nspname='""" + schema + """' AND c.relowner<>(SELECT oid FROM pg_roles WHERE rolname=current_user)) THEN RAISE EXCEPTION 'role/ownership differs';END IF;END$$;
SET LOCAL ROLE pg_read_all_data;
SET LOCAL enable_seqscan=off;
SET LOCAL cuvs.hnsw_exact_scan=on;
""")
        expression = "SELECT id FROM fixture.expr_items ORDER BY fixture.expr(v)::vector(2) <-> '[0,0]'::vector"
        errors = query_case("expression_and_policy_once", expression, [{"id": 1}])
        policy_ids = [int(re.search(r" id=(\d+)", line).group(1)) for line in errors if "POLICY_ROW role=pg_read_all_data" in line]
        assert Counter(policy_ids) == Counter({1: 1, 2: 1, 3: 1}), policy_ids
        pred_query = "SELECT id FROM fixture.pred_items WHERE fixture.pred(id) ORDER BY v <-> '[0,0]'::vector"
        errors = query_case("partial_predicate_restored", pred_query, [{"id": 1}])
        pred_ids = [int(re.search(r" id=(\d+)", line).group(1)) for line in errors if "INDEX_PRED role=pg_read_all_data" in line]
        assert Counter(pred_ids) == Counter({1: 1, 4: 1}), pred_ids
        implied_query = "SELECT id FROM fixture.implied WHERE fixture.observed(id) ORDER BY v <-> '[0,0]'::vector"
        errors = query_case("policy_implied_by_partial", implied_query, [{"id": 1}, {"id": 2}])
        user_ids = [int(re.search(r" id=(\d+)", line).group(1)) for line in errors if "USER_QUAL role=pg_read_all_data" in line]
        assert Counter(user_ids) == Counter({1: 1, 2: 1}), user_ids
        nonleak_query = "SELECT id FROM fixture.nonleak WHERE fixture.pred(id) AND fixture.observed(id) ORDER BY v <-> '[0,0]'::vector"
        errors = query_case("nonleakproof_partial_predicate", nonleak_query, [{"id": 1}, {"id": 2}])
        pred_ids = [int(re.search(r" id=(\d+)", line).group(1)) for line in errors if "INDEX_PRED role=pg_read_all_data" in line]
        user_ids = [int(re.search(r" id=(\d+)", line).group(1)) for line in errors if "USER_QUAL role=pg_read_all_data" in line]
        assert Counter(pred_ids) == Counter({1: 1, 2: 1}) and Counter(user_ids) == Counter({1: 1, 2: 1}), (pred_ids, user_ids)
        barrier_query = "SELECT id FROM fixture.barrier_pred_view WHERE fixture.observed(id)"
        errors = query_case("barrier_partial_user_qual", barrier_query, [{"id": 1}, {"id": 2}])
        user_ids = [int(re.search(r" id=(\d+)", line).group(1)) for line in errors if "USER_QUAL role=pg_read_all_data" in line]
        assert Counter(user_ids) == Counter({1: 1, 2: 1}), user_ids
        toggle_query = "SELECT id FROM fixture.implied WHERE fixture.toggle(id) ORDER BY v <-> '[0,0]'::vector"
        offset = stage("midquery_toggle")
        toggle_plan = explain(toggle_query, "off", False)
        assert any(node["Node Type"] == "Index Scan" for node in plan_nodes(toggle_plan)), toggle_plan
        toggle_rows = value(actor, "SELECT jsonb_agg(x) FROM(" + toggle_query + ")x;")
        toggle_notices = finish("midquery_toggle", offset)
        toggle_ids = [int(re.search(r" id=(\d+)", line).group(1)) for line in toggle_notices if "TOGGLE_QUAL role=pg_read_all_data" in line]
        assert toggle_rows == [{"id": 1}, {"id": 2}] and Counter(toggle_ids) == Counter({1: 1, 2: 1}), (toggle_rows, toggle_ids)
        assert ask(actor, "SHOW cuvs.hnsw_exact_scan;") == ["on"]
        next_plan = explain(implied_query, "on", False)
        native(next_plan)
        result["cases"].append({"name": "midquery_policy_toggle", "verdict": "PASS", "old_plan": toggle_plan, "next_plan": next_plan, "rows": toggle_rows, "notices": toggle_notices})
        query_case("plain_rls", "SELECT id FROM fixture.plain ORDER BY v <-> '[0,0]'::vector", [{"id": 1}])

        ask(actor, "SET LOCAL plan_cache_mode=force_generic_plan;PREPARE rls_q(vector) AS SELECT jsonb_agg(x) FROM(SELECT id FROM fixture.expr_items ORDER BY fixture.expr(v)::vector(2) <-> $1)x;")
        plans, rows = [], []
        for step, mode in enumerate(["off", "on", "off"]):
            offset = stage("generic_" + str(step))
            plan = explain("EXECUTE rls_q('[0,0]')", mode, False)
            actual = value(actor, "EXECUTE rls_q('[0,0]');")
            no_hidden(finish("generic_" + str(step), offset))
            assert actual == [{"id": 1}], actual
            if mode == "on": native(plan)
            else: assert any(n["Node Type"] == "Index Scan" for n in plan_nodes(plan)), plan
            plans.append(plan); rows.append(actual)
        counts = value(actor, "SELECT jsonb_build_object('generic',generic_plans,'custom',custom_plans) FROM pg_prepared_statements WHERE name='rls_q';")
        assert counts["generic"] >= 3 and counts["custom"] == 0, counts
        result["cases"].append({"name": "generic_plan_revalidation", "verdict": "PASS", "plans": plans, "rows": rows, "counts": counts})
        # Policy DDL is confined to the newly created fixture and rolls back.
        # The same prepared statement must pick up the changed policy on replan.
        ask(actor, "RESET ROLE;ALTER POLICY expr_rows ON fixture.expr_items USING(fixture.policy(id) OR id=2);SET LOCAL ROLE pg_read_all_data;SET LOCAL cuvs.hnsw_exact_scan=on;")
        offset = stage("changed_policy")
        changed = value(actor, "EXECUTE rls_q('[0,0]');")
        notices = finish("changed_policy", offset)
        assert changed == [{"id": 2}, {"id": 1}], changed
        no_hidden(notices, ("[10,0]", "[0,0]"))
        result["cases"].append({"name": "policy_invalidation", "verdict": "PASS", "rows": changed})
        ask(actor, "RESET ROLE;ALTER POLICY expr_rows ON fixture.expr_items USING(fixture.policy(id));SET LOCAL ROLE pg_read_all_data;")
        ask(actor, "SET LOCAL cuvs.hnsw_exact_scan=on;SET LOCAL plan_cache_mode=force_custom_plan;PREPARE rls_custom(vector) AS SELECT id FROM fixture.expr_items ORDER BY fixture.expr(v)::vector(2) <-> $1;")
        offset = stage("custom")
        custom_plan = explain("EXECUTE rls_custom('[0,0]')")
        no_hidden(finish("custom", offset)); native(custom_plan)
        counts = value(actor, "SELECT jsonb_build_object('generic',generic_plans,'custom',custom_plans) FROM pg_prepared_statements WHERE name='rls_custom';")
        assert counts["custom"] >= 1 and counts["generic"] == 0
        result["cases"].append({"name": "custom_plan", "verdict": "PASS", "plan": custom_plan, "counts": counts})
        lateral = "SELECT q.i,t.id,t.d FROM(VALUES(1,'[0,0]'::vector),(2,'[10,0]'::vector))q(i,v) CROSS JOIN LATERAL(SELECT id,v <-> q.v AS d FROM fixture.pred_items WHERE fixture.pred(id) ORDER BY v <-> q.v LIMIT 1)t ORDER BY q.i"
        query_case("parameterized_partial_rescan", lateral, [{"i": 1, "id": 1, "d": 10}, {"i": 2, "id": 1, "d": 0}])
        query_case("security_invoker_view", "SELECT id FROM fixture.invoker_view ORDER BY fixture.expr(v)::vector(2) <-> '[0,0]'::vector", [{"id": 1}])
        query_case("view_owner_context", "SELECT id FROM fixture.owner_view ORDER BY fixture.expr(v)::vector(2) <-> '[0,0]'::vector", [{"id": 2}, {"id": 3}, {"id": 1}], ("[10,0]", "[0,0]", "[1,0]"))
        query_case("security_barrier_without_rls", "SELECT id FROM fixture.barrier_view", [{"id": 1}])
        query_case("partition_parent_policy", "SELECT id FROM fixture.parts ORDER BY fixture.expr(v)::vector(2) <-> '[0,0]'::vector", [{"id": 1}])
        offset = stage("cursor")
        ask(actor, "DECLARE rls_cursor SCROLL CURSOR FOR " + expression + ";")
        assert ask(actor, "FETCH 1 FROM rls_cursor;") == ["1"]
        ask(actor, "SET LOCAL cuvs.hnsw_exact_scan=off;MOVE BACKWARD ALL FROM rls_cursor;")
        assert ask(actor, "FETCH ALL FROM rls_cursor;") == ["1"]
        ask(actor, "CLOSE rls_cursor;SET LOCAL cuvs.hnsw_exact_scan=on;")
        no_hidden(finish("cursor", offset))
        result["cases"].append({"name": "cursor_lifetime", "verdict": "PASS"})
        offset = stage("allowed_error")
        ask(actor, "DO $$BEGIN BEGIN PERFORM id FROM fixture.expr_items ORDER BY fixture.expr(v)::vector(2) <-> '[0,0]'::vector;RAISE EXCEPTION 'missing expected error';EXCEPTION WHEN division_by_zero THEN RAISE NOTICE 'RLS_CAUGHT_22012';END;END$$;")
        assert actor.wait_error("RLS_CAUGHT_22012", 10)
        no_hidden(finish("allowed_error", offset))
        recovery_offset = stage("error_recovery")
        assert value(actor, "SELECT jsonb_agg(x) FROM(" + expression + ")x;") == [{"id": 1}]
        no_hidden(finish("error_recovery", recovery_offset))
        result["cases"].append({"name": "subtransaction_error_recovery", "verdict": "PASS"})
        offset = stage("permission_denial")
        ask(actor, "DO $$BEGIN BEGIN PERFORM id FROM fixture.blocked_items ORDER BY fixture.blocked(v)::vector(2) <-> '[0,0]'::vector;RAISE EXCEPTION 'missing EXECUTE denial';EXCEPTION WHEN insufficient_privilege THEN RAISE NOTICE 'RLS_DENIED_42501';END;END$$;")
        assert actor.wait_error("RLS_DENIED_42501", 10)
        notices = finish("permission_denial", offset)
        assert not any("BLOCKED_BODY role=pg_read_all_data" in line for line in notices), notices
        stage("permission_recovery")
        assert value(actor, "SELECT jsonb_agg(x) FROM(" + expression + ")x;") == [{"id": 1}]
        result["cases"].append({"name": "execution_acl_start_error_recovery", "verdict": "PASS", "notices": notices})
        pid = int(ask(actor, "SELECT pg_backend_pid();")[-1])
        for event in ["cancel", "timeout"]:
            offset = stage(event)
            if event == "timeout": ask(actor, "SET LOCAL statement_timeout='200ms';")
            outcome = {}
            sql = "DO $$BEGIN BEGIN PERFORM id FROM fixture.expr_items ORDER BY fixture.expr(v)::vector(2) <-> '[0,0]'::vector;RAISE EXCEPTION 'missing expected cancel';EXCEPTION WHEN query_canceled THEN RAISE NOTICE 'RLS_CAUGHT_57014';END;END$$;"
            def work():
                try: outcome["lines"] = ask(actor, sql)
                except Exception as error: outcome["error"] = repr(error)
            worker = threading.Thread(target=work)
            worker.start()
            assert actor.wait_error("stage=" + event, 10), "no expression entry marker"
            if event == "cancel": assert ask(control, "SELECT pg_cancel_backend(" + str(pid) + ");") == ["t"]
            worker.join(5)
            assert not worker.is_alive() and "error" not in outcome, outcome
            assert actor.wait_error("RLS_CAUGHT_57014", 10), actor.errors
            ask(actor, "SET LOCAL statement_timeout=0;")
            notices = finish(event, offset)
            no_hidden(notices)
            assert any("RLS_CAUGHT_57014" in line for line in notices), notices
            stage(event + "_recovery")
            assert value(actor, "SELECT jsonb_agg(x) FROM(" + expression + ")x;") == [{"id": 1}]
            assert int(ask(actor, "SELECT pg_backend_pid();")[-1]) == pid
            result["cases"].append({"name": event + "_same_backend_recovery", "verdict": "PASS", "pid": pid, "notices": notices})
        offset = stage("non_rls_acl")
        statement = "PERFORM id FROM fixture.acl_items WHERE fixture.acl_pred(id) ORDER BY v <-> '[0,0]'::vector;"
        ask(actor, "SET LOCAL cuvs.hnsw_exact_scan=off;SET LOCAL enable_seqscan=off;SET LOCAL enable_indexscan=on;DO $$BEGIN " + statement + " RAISE NOTICE 'ACL_OFF_OK';EXCEPTION WHEN insufficient_privilege THEN RAISE NOTICE 'ACL_OFF_DENIED';END$$;")
        ask(actor, "SET LOCAL cuvs.hnsw_exact_scan=on;DO $$BEGIN " + statement + " RAISE NOTICE 'ACL_ON_OK';EXCEPTION WHEN insufficient_privilege THEN RAISE NOTICE 'ACL_ON_DENIED';END$$;")
        ask(actor, "SET LOCAL cuvs.hnsw_exact_scan=off;SET LOCAL enable_indexscan=off;SET LOCAL enable_seqscan=on;DO $$BEGIN " + statement + " RAISE NOTICE 'ACL_SEQ_OK';EXCEPTION WHEN insufficient_privilege THEN RAISE NOTICE 'ACL_SEQ_DENIED';END$$;")
        notices = finish("non_rls_acl", offset)
        assert all(any(tag in line for line in notices) for tag in ('ACL_OFF_OK','ACL_ON_DENIED','ACL_SEQ_DENIED')), notices
        assert not any('ACL_PRED role=pg_read_all_data' in line for line in notices), notices
        ask(actor, "SET LOCAL enable_indexscan=on;SET LOCAL enable_seqscan=off;SET LOCAL cuvs.hnsw_exact_scan=on;")
        result['cases'].append({'name': 'non_rls_predicate_execution_acl', 'verdict': 'PASS', 'notices': notices})
        ask(actor, "SET LOCAL debug_parallel_query=on;SET LOCAL max_parallel_workers_per_gather=2;SET LOCAL parallel_setup_cost=0;SET LOCAL parallel_tuple_cost=0;")
        query_case("postgres_parallel_executor", "SELECT id FROM fixture.plain ORDER BY v <-> '[0,0]'::vector", [{"id": 1}])
        gather_nodes = [node for node in plan_nodes(result["cases"][-1]["plan"]) if node["Node Type"] == "Gather"]
        assert gather_nodes and any(node.get("Workers Planned", 0) > 0 for node in gather_nodes), result["cases"][-1]["plan"]
        assert all(node.get("Workers Launched", 0) <= node.get("Workers Planned", 0) for node in gather_nodes), gather_nodes
        assert len(result["cases"]) == 22, len(result["cases"])
        result["verdict"] = "PASS"
    except Exception as error:
        result.update(verdict="FAIL", error=repr(error))
        raise
    finally:
        cleanup_error = None
        if actor:
            try:
                if started:
                    ask(actor, "ROLLBACK;")
                    result["same_session_after_rollback"] = value(actor, "SELECT jsonb_build_object('role',current_user,'policies',(SELECT count(*) FROM pg_policy p JOIN pg_class c ON p.polrelid=c.oid JOIN pg_namespace n ON c.relnamespace=n.oid WHERE n.nspname='" + schema + "'),'schemas',(SELECT count(*) FROM pg_namespace WHERE nspname='" + schema + "'));")
                    assert result["same_session_after_rollback"] == {"role": result["precondition"]["actor"], "policies": 0, "schemas": 0}
            except Exception as error:
                cleanup_error = error
                result.update(verdict="FAIL", cleanup_error=repr(error))
            finally:
                actor.close()
                result["stderr"] = list(actor.errors)
        if control:
            control.close()
        (output / "transcript.json").write_text(json.dumps(transcript, indent=2) + "\n")
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        (output / "stderr.log").write_text("\n".join(result.get("stderr", [])) + "\n")
        print(json.dumps({"verdict": result.get("verdict"), "cases": len(result["cases"]), "error": result.get("error"), "cleanup_error": result.get("cleanup_error")}))
        if cleanup_error:
            raise cleanup_error


if __name__ == "__main__":
    main()
