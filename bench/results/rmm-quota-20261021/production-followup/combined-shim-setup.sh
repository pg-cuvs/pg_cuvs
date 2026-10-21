set -eu
apk add --no-cache build-base curl-dev openssl-dev git > /tmp/packages.log 2>&1
git clone --depth 1 --branch v0.8.0 https://github.com/pgvector/pgvector.git /tmp/pgvector > /tmp/pgvector-build.log 2>&1
make -C /tmp/pgvector -j2 with_llvm=no >> /tmp/pgvector-build.log 2>&1
make -C /tmp/pgvector install with_llvm=no >> /tmp/pgvector-build.log 2>&1
cd /work
make test-unit > /tmp/unit.log 2>&1
make -j2 PGCUVS_CPU_SHIM=1 with_llvm=no > /tmp/extension-build.log 2>&1
make -j2 PGCUVS_CPU_SHIM=1 with_llvm=no server server-test > /tmp/daemon-build.log 2>&1
make PGCUVS_CPU_SHIM=1 with_llvm=no install install-server > /tmp/install.log 2>&1
chown -R postgres:postgres /work
mkdir -p /tmp/pgdata /tmp/cuvs_indexes
chown postgres:postgres /tmp/pgdata /tmp/cuvs_indexes
su postgres -c 'initdb -D /tmp/pgdata -A trust' > /tmp/initdb.log 2>&1
printf "shared_preload_libraries='pg_cuvs'\nlisten_addresses=''\nunix_socket_directories='/tmp'\n" >> /tmp/pgdata/postgresql.conf
su postgres -c 'pg_ctl -D /tmp/pgdata -l /tmp/postgres.log -w start'
su postgres -c 'pg_cuvs_server --socket /tmp/.s.pg_cuvs --index-dir /tmp/cuvs_indexes > /tmp/daemon.log 2>&1' &
for i in 1 2 3 4 5 6 7 8 9 10; do [ ! -S /tmp/.s.pg_cuvs ] || break; sleep 1; done
su postgres -c 'createdb -h /tmp postgres'
