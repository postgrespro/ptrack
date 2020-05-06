#!/usr/bin/env sh

if [ -z ${PG_VERSION+x} ]; then
	echo PG_VERSION is not set!
	exit 1
fi

if [ -z ${PG_BRANCH+x} ]; then
	echo PG_BRANCH is not set!
	exit 1
fi

if [ -z ${MODE+x} ]; then
	MODE=basic
else
	echo MODE=${MODE}
fi

if [ -z ${TEST_CASE+x} ]; then
	TEST_CASE=all
else
	echo TEST_CASE=${TEST_CASE}
fi

if [ -z ${TEST_REPEATS+x} ]; then
	TEST_REPEATS=1
else
	echo TEST_REPEATS=${TEST_REPEATS}
fi

echo PG_VERSION=${PG_VERSION}
echo PG_BRANCH=${PG_BRANCH}

sed \
	-e 's/${PG_VERSION}/'${PG_VERSION}/g \
	-e 's/${PG_BRANCH}/'${PG_BRANCH}/g \
	-e 's/${MODE}/'${MODE}/g \
	-e 's/${TEST_CASE}/'${TEST_CASE}/g \
	-e 's/${TEST_REPEATS}/'${TEST_REPEATS}/g \
Dockerfile.in > Dockerfile
