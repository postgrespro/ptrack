#!/usr/bin/env bash

#
# Copyright (c) 2019-2020, Postgres Professional
#


PG_SRC=$PWD/postgres
status=0

# # Here PG_VERSION is provided by postgres:X-alpine docker image
# curl "https://ftp.postgresql.org/pub/source/v$PG_VERSION/postgresql-$PG_VERSION.tar.bz2" -o postgresql.tar.bz2
# echo "$PG_SHA256 *postgresql.tar.bz2" | sha256sum -c -

# mkdir $PG_SRC

# tar \
# 	--extract \
# 	--file postgresql.tar.bz2 \
# 	--directory $PG_SRC \
# 	--strip-components 1

# Clone Postgres
echo "############### Getting Postgres sources"
git clone https://github.com/postgres/postgres.git -b $PG_BRANCH --depth=1

# Clone pg_probackup
echo "############### Getting pg_probackup sources"
# git clone https://github.com/postgrespro/pg_probackup.git --depth=1
git clone https://github.com/ololobus/pg_probackup.git --depth=1 -b ptrack-tests

# Compile and install Postgres
cd postgres # Go to postgres dir
echo "############### Applying ptrack patch"
git apply -v -3 ../patches/$PG_BRANCH-ptrack-core.diff

echo "############### Compiling Postgres"
./configure --prefix=$PGHOME --enable-debug --enable-cassert --enable-depend --enable-tap-tests
make -s -j$(nproc) install
make -s -j$(nproc) -C contrib/ install

# Override default Postgres instance
export PATH=$PGHOME/bin:$PATH
export LD_LIBRARY_PATH=$PGHOME/lib
export PG_CONFIG=$(which pg_config)

# Get amcheck if missing
if [ ! -d "contrib/amcheck" ]; then
    echo "############### Getting missing amcheck"
    git clone https://github.com/petergeoghegan/amcheck.git --depth=1 contrib/amcheck
    make USE_PGXS=1 -C contrib/amcheck install
fi

# Get back to testdir
cd ..

# Build and install ptrack extension
echo "############### Compiling and installing ptrack extension"
make USE_PGXS=1 PG_CPPFLAGS="-coverage" SHLIB_LINK="-coverage" install

# Show pg_config path (just in case)
echo "############### pg_config path"
which pg_config

# Show pg_config just in case
echo "############### pg_config"
pg_config

# Build and install pg_probackup
echo "############### Compiling and installing pg_probackup"
cd pg_probackup # Go to pg_probackup dir
make USE_PGXS=1 top_srcdir=$PG_SRC install

# Setup python environment
echo "############### Setting up python env"
virtualenv pyenv
source pyenv/bin/activate
pip install testgres==1.8.2

echo "############### Testing"
if [ "$MODE" = "basic" ]; then
    export PG_PROBACKUP_TEST_BASIC=ON
fi

if [ "$TEST_CASE" = "all" ]; then
    python -m unittest -v tests.ptrack || status=$?
else
    for i in `seq $TEST_REPEATS`; do
        python -m unittest -v tests.ptrack.PtrackTest.$TEST_CASE || status=$?
    done
fi

# Get back to testdir
cd ..

# Generate *.gcov files
gcov src/*.c src/*.h

# Send coverage stats to Codecov
bash <(curl -s https://codecov.io/bash)

# Something went wrong, exit with code 1
if [ $status -ne 0 ]; then exit 1; fi
