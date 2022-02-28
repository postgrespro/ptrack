#!/usr/bin/env bash

#
# Copyright (c) 2019-2021, Postgres Professional
#

PTRACK_SRC=${PWD}/ptrack
PG_SRC=${PWD}/postgres
PBK_SRC=${PWD}/pg_probackup
status=0

#########################################################
# Clone Postgres
echo "############### Getting Postgres sources"
git clone https://github.com/postgres/postgres.git --depth=1 --branch=${PG_BRANCH} ${PG_SRC}

# Clone pg_probackup
echo "############### Getting pg_probackup sources"
git clone https://github.com/postgrespro/pg_probackup.git --depth=1 --branch=master ${PBK_SRC}

#########################################################
# Compile and install Postgres
cd ${PG_SRC} # Go to postgres dir

echo "############### Applying ptrack patch"
git apply --verbose --3way ${PTRACK_SRC}/patches/${PG_BRANCH}-ptrack-core.diff

if [ "${MODE}" = "paranoia" ]; then
    echo "############### Paranoia mode: applying turn-off-hint-bits.diff"
    git apply --verbose --3way ${PTRACK_SRC}/patches/turn-off-hint-bits.diff
fi

echo "############### Compiling Postgres"
if [ "${TEST_CASE}" = "tap" ] && [ "${MODE}" = "legacy" ]; then
    ./configure CFLAGS='-DEXEC_BACKEND' --disable-atomics --prefix=${PGHOME} --enable-debug --enable-cassert --enable-depend --enable-tap-tests --quiet
else
    ./configure --prefix=${PGHOME} --enable-debug --enable-cassert --enable-depend --enable-tap-tests --quiet
fi
make --quiet --jobs=$(nproc) install
make --quiet --jobs=$(nproc) --directory=contrib/ install

# Override default Postgres instance
export PATH=${PGHOME}/bin:${PATH}
export LD_LIBRARY_PATH=${PGHOME}/lib
export PG_CONFIG=$(which pg_config)

# Show pg_config path (just in case)
echo "############### pg_config path"
which pg_config

# Show pg_config just in case
echo "############### pg_config"
pg_config

#########################################################
# Build and install ptrack extension
echo "############### Compiling and installing ptrack extension"
cp --recursive ${PTRACK_SRC} ${PG_SRC}/contrib/ptrack
make USE_PGXS=1 --directory=${PG_SRC}/contrib/ptrack/ clean
make USE_PGXS=1 PG_CPPFLAGS="-coverage" SHLIB_LINK="-coverage" --directory=${PG_SRC}/contrib/ptrack/ install

if [ "${TEST_CASE}" = "tap" ]; then

    # Run tap tests
    echo "############### Running tap tests"
    if [ "${MODE}" = "legacy" ]; then
        # There is a known issue with attaching shared memory segment using the same
        # address each time, when EXEC_BACKEND mechanism is turned on.  It happens due
        # to the ASLR address space randomization, so we are trying to attach a segment
        # to the already occupied location.  That way we simply turning off ASLR here.
        #
        # Postgres comment: https://github.com/postgres/postgres/blob/5cbfce562f7cd2aab0cdc4694ce298ec3567930e/src/backend/postmaster/postmaster.c#L4929
        setarch x86_64 --addr-no-randomize make --directory=${PG_SRC}/contrib/ptrack check || status=$?
    else
        make --directory=${PG_SRC}/contrib/ptrack check || status=$?
    fi

else
    # Set kernel params (used for debugging -- probackup tests)
    echo "############### setting kernel params"
    sudo sh -c 'echo 0 > /proc/sys/kernel/yama/ptrace_scope'

    # Build and install pg_probackup
    echo "############### Compiling and installing pg_probackup"
    cd ${PBK_SRC} # Go to pg_probackup dir
    make USE_PGXS=1 top_srcdir=${PG_SRC} install

    # Setup python environment
    echo "############### Setting up python env"
    virtualenv --python=/usr/bin/python3 pyenv
    source pyenv/bin/activate
    pip install testgres==1.8.2

    echo "############### Testing"
    export PG_PROBACKUP_PTRACK=ON
    if [ "${MODE}" = "basic" ]; then
        export PG_PROBACKUP_TEST_BASIC=ON
    elif [ "${MODE}" = "paranoia" ]; then
        export PG_PROBACKUP_PARANOIA=ON
    fi

    if [ "${TEST_CASE}" = "all" ]; then
        # Run all pg_probackup ptrack tests
        PBK_TEST_CASE=tests.ptrack
    else
        PBK_TEST_CASE=tests.ptrack.PtrackTest.${TEST_CASE}
    fi
    for i in `seq ${TEST_REPEATS}`; do
        python3 -m unittest -v ${PBK_TEST_CASE} || status=$?
    done

    # Exit virtualenv
    deactivate
fi

#########################################################
# codecov
echo "############### Codecov"
cd ${PTRACK_SRC}
# Generate *.gcov files
gcov ${PG_SRC}/contrib/ptrack/*.c ${PG_SRC}/contrib/ptrack/*.h

# Send coverage stats to Codecov
bash <(curl -s https://codecov.io/bash)

# Something went wrong, exit with code 1
if [ ${status} -ne 0 ]; then exit 1; fi
