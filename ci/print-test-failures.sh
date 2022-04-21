#!/bin/sh
#
# Print output of failing tests
#

set -e

. ${0%/*}/lib-ci-type.sh
. ${0%/*}/lib-tput.sh

exit_code=
while test $# != 0
do
	case "$1" in
	--exit-code)
		exit_code=t
		;;
	*)
		echo "BUG: invalid $0 argument: $1" >&2
		exit 1
		;;
	esac
	shift
done

if ! ls t/test-results/*.exit >/dev/null 2>/dev/null
then
	echo "Build job failed before the tests could have been run"
	exit
fi

failed=
for TEST_EXIT in t/test-results/*.exit
do
	if [ "$(cat "$TEST_EXIT")" != "0" ]
	then
		failed=t
		TEST_NAME="${TEST_EXIT%.exit}"
		TEST_NAME="${TEST_NAME##*/}"
		TEST_OUT="${TEST_NAME}.out"
		TEST_MARKUP="${TEST_NAME}.markup"

		echo "------------------------------------------------------------------------"
		echo "$(tput setaf 1)${TEST_OUT}...$(tput sgr0)"
		echo "------------------------------------------------------------------------"
		cat "t/test-results/${TEST_OUT}"

		trash_dir="trash directory.$TEST_NAME"
		case "$CI_TYPE" in
		github-actions)
			mkdir -p t/failed-test-artifacts
			echo "FAILED_TEST_ARTIFACTS=t/failed-test-artifacts" >>$GITHUB_ENV
			cp "t/test-results/${TEST_OUT}" t/failed-test-artifacts/
			(
				cd t &&
				tar czf failed-test-artifacts/"$TEST_NAME".trash.tar.gz "$trash_dir"
			)
			;;
		esac
	fi
done

if test -n "$failed"
then
	if test -n "$exit_code"
	then
		exit 1
	fi
fi
