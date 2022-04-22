#!/bin/sh

# TODO: Ideally we'd compile t/helper/test-online-cpus.c early, but
# that currently presents a chicken & egg problem. We need this before
# we build (much of) anything.
online_cpus() {
	NPROC=

	if command -v nproc >/dev/null
	then
		# GNU coreutils
		NPROC=$(nproc)
	elif command -v sysctl >/dev/null
	then
		# BSD & Mac OS X
		NPROC=$(sysctl -n hw.ncpu)
	elif test -n "$NUMBER_OF_PROCESSORS"
	then
		# Windows
		NPROC="$NUMBER_OF_PROCESSORS"
	else
		NPROC=1
	fi

	echo $NPROC
}
