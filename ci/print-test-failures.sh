#!/bin/sh
#
# Print output of failing tests
#

set -e

. ${0%/*}/lib-ci-type.sh
. ${0%/*}/lib-tput.sh

if ! ls t/test-results/*.exit >/dev/null 2>/dev/null
then
	echo "Build job failed before the tests could have been run"
	exit
fi

for TEST_EXIT in t/test-results/*.exit
do
	if [ "$(cat "$TEST_EXIT")" != "0" ]
	then
		TEST_NAME="${TEST_EXIT%.exit}"
		TEST_NAME="${TEST_NAME##*/}"
		TEST_OUT="${TEST_NAME}.out"
		TEST_MARKUP="${TEST_NAME}.markup"

		echo "------------------------------------------------------------------------"
		echo "$(tput setaf 1)test-results/${TEST_OUT}...$(tput sgr0)"
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
