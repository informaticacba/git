#!/bin/sh
#
# Print output of failing tests
#

set -e

. ${0%/*}/lib-ci-type.sh
. ${0%/*}/lib-tput.sh

cd t/

if ! ls test-results/*.exit >/dev/null 2>/dev/null
then
	echo "Build job failed before the tests could have been run"
	exit
fi

for TEST_EXIT in test-results/*.exit
do
	if [ "$(cat "$TEST_EXIT")" != "0" ]
	then
		TEST_OUT="${TEST_EXIT%exit}out"
		echo "------------------------------------------------------------------------"
		echo "$(tput setaf 1)${TEST_OUT}...$(tput sgr0)"
		echo "------------------------------------------------------------------------"
		cat "${TEST_OUT}"

		test_name="${TEST_EXIT%.exit}"
		test_name="${test_name##*/}"
		trash_dir="trash directory.$test_name"
		case "$CI_TYPE" in
		github-actions)
			mkdir -p failed-test-artifacts
			echo "FAILED_TEST_ARTIFACTS=t/failed-test-artifacts" >>$GITHUB_ENV
			cp "${TEST_EXIT%.exit}.out" failed-test-artifacts/
			tar czf failed-test-artifacts/"$test_name".trash.tar.gz "$trash_dir"
			;;
		esac
	fi
done
