#!/bin/sh

test_description='test fetching files and bundles over HTTP'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-httpd.sh
start_httpd

test_expect_success 'set up source repo' '
	git init src &&
	test_commit -C src one
'

test_expect_success 'fail to get a file over HTTP' '
	cat >input <<-EOF &&
	get $HTTPD_URL/does-not-exist.txt download-failed.txt

	EOF
	git remote-https "$HTTPD_URL" <input 2>err &&
	test_path_is_missing download-failed.txt
'

test_done
