#include "cache.h"
#include "bundle-uri.h"
#include "bundle.h"
#include "config.h"
#include "object-store.h"
#include "refs.h"
#include "run-command.h"
#include "hashmap.h"
#include "pkt-line.h"
#include "remote.h"

static int compare_bundles(const void *hashmap_cmp_fn_data,
			   const struct hashmap_entry *he1,
			   const struct hashmap_entry *he2,
			   const void *id)
{
	const struct remote_bundle_info *e1 =
		container_of(he1, const struct remote_bundle_info, ent);
	const struct remote_bundle_info *e2 =
		container_of(he2, const struct remote_bundle_info, ent);

	return strcmp(e1->id, id ? (const char *)id : e2->id);
}

void init_bundle_list(struct bundle_list *list)
{
	memset(list, 0, sizeof(*list));

	/* Implied defaults. */
	list->mode = BUNDLE_MODE_ALL;
	list->version = 1;

	hashmap_init(&list->bundles, compare_bundles, NULL, 0);
}

static int clear_remote_bundle_info(struct remote_bundle_info *bundle,
				    void *data)
{
	FREE_AND_NULL(bundle->id);
	FREE_AND_NULL(bundle->uri);
	strbuf_release(&bundle->file);
	bundle->unbundled = 0;
	return 0;
}

void clear_bundle_list(struct bundle_list *list)
{
	if (!list)
		return;

	for_all_bundles_in_list(list, clear_remote_bundle_info, NULL);
	hashmap_clear_and_free(&list->bundles, struct remote_bundle_info, ent);
	free(list->baseURI);
}

int for_all_bundles_in_list(struct bundle_list *list,
			    bundle_iterator iter,
			    void *data)
{
	struct remote_bundle_info *info;
	struct hashmap_iter i;

	hashmap_for_each_entry(&list->bundles, &i, info, ent) {
		int result = iter(info, data);

		if (result)
			return result;
	}

	return 0;
}

static int summarize_bundle(struct remote_bundle_info *info, void *data)
{
	FILE *fp = data;
	fprintf(fp, "[bundle \"%s\"]\n", info->id);
	fprintf(fp, "\turi = %s\n", info->uri);
	return 0;
}

void print_bundle_list(FILE *fp, struct bundle_list *list)
{
	const char *mode;

	switch (list->mode) {
	case BUNDLE_MODE_ALL:
		mode = "all";
		break;

	case BUNDLE_MODE_ANY:
		mode = "any";
		break;

	case BUNDLE_MODE_NONE:
	default:
		mode = "<unknown>";
	}

	printf("[bundle \"list\"]\n");
	printf("\tversion = %d\n", list->version);
	printf("\tmode = %s\n", mode);

	for_all_bundles_in_list(list, summarize_bundle, fp);
}


/**
 * Given a key-value pair, update the state of the given bundle list.
 * Returns 0 if the key-value pair is understood. Returns 1 if the key
 * is not understood or the value is malformed.
 */
static int bundle_list_update(const char *key, const char *value,
			      struct bundle_list *list)
{
	const char *pkey, *dot;
	struct strbuf id = STRBUF_INIT;
	struct remote_bundle_info lookup = REMOTE_BUNDLE_INFO_INIT;
	struct remote_bundle_info *bundle;

	if (!skip_prefix(key, "bundle.", &pkey))
		return 1;

	if (!strcmp(pkey, "list.version")) {
		int version = atoi(value);
		if (version != 1)
			return 1;

		list->version = version;
		return 0;
	}

	if (!strcmp(pkey, "list.mode")) {
		if (!strcmp(value, "all"))
			list->mode = BUNDLE_MODE_ALL;
		else if (!strcmp(value, "any"))
			list->mode = BUNDLE_MODE_ANY;
		else
			return 1;
		return 0;
	}

	/*
	 * All remaining keys must be of the form "bundle.<id>.*" where
	 * <id> != "list"
	 */

	dot = strchr(pkey, '.');
	if (!dot)
		return 1;
	if (dot - pkey == 4 &&
	    !strncmp(pkey, "list", 4))
		return 1;

	strbuf_add(&id, pkey, dot - pkey);
	dot++;

	/*
	 * Check for an existing bundle with this <id>, or create one
	 * if necessary.
	 */
	lookup.id = id.buf;
	hashmap_entry_init(&lookup.ent, strhash(lookup.id));
	if (!(bundle = hashmap_get_entry(&list->bundles, &lookup, ent, NULL))) {
		CALLOC_ARRAY(bundle, 1);
		bundle->id = strbuf_detach(&id, NULL);
		strbuf_init(&bundle->file, 0);
		hashmap_entry_init(&bundle->ent, strhash(bundle->id));
		hashmap_add(&list->bundles, &bundle->ent);
	}
	strbuf_release(&id);

	if (!strcmp(dot, "uri")) {
		free(bundle->uri);
		bundle->uri = relative_url(list->baseURI, value, NULL);
		return 0;
	}

	/*
	 * At this point, we ignore any information that we don't
	 * understand, assuming it to be hints for a heuristic the client
	 * does not currently understand.
	 */
	return 0;
}

static int config_to_bundle_list(const char *key, const char *value, void *data)
{
	struct bundle_list *list = data;
	return bundle_list_update(key, value, list);
}

int parse_bundle_list_in_config_format(const char *uri,
				       const char *filename,
				       struct bundle_list *list)
{
	int result;
	struct config_options opts = {
		.error_action = CONFIG_ERROR_ERROR,
	};

	list->mode = BUNDLE_MODE_NONE;
	if (!list->baseURI)
		list->baseURI = xstrdup(uri);
	result = git_config_from_file_with_options(config_to_bundle_list,
						   filename, list,
						   &opts);

	if (!result && list->mode == BUNDLE_MODE_NONE) {
		warning(_("bundle list at '%s' has no mode"), uri);
		result = 1;
	}

	return result;
}

static void find_temp_filename(struct strbuf *name)
{
	int fd;
	/*
	 * Find a temporray filename that is available. This is briefly
	 * racy, but unlikely to collide.
	 */
	fd = odb_mkstemp(name, "bundles/tmp_uri_XXXXXX");
	if (fd < 0)
		die(_("failed to create temporary file"));
	close(fd);
	unlink(name->buf);
}

static int download_https_uri_to_file(const char *uri, const char *file)
{
	int result = 0;
	struct child_process cp = CHILD_PROCESS_INIT;
	FILE *child_in = NULL, *child_out = NULL;
	struct strbuf line = STRBUF_INIT;
	int found_get = 0;

	strvec_pushl(&cp.args, "git-remote-https", "origin", uri, NULL);
	cp.in = -1;
	cp.out = -1;

	if (start_command(&cp))
		return 1;

	child_in = fdopen(cp.in, "w");
	if (!child_in) {
		result = 1;
		goto cleanup;
	}

	child_out = fdopen(cp.out, "r");
	if (!child_out) {
		result = 1;
		goto cleanup;
	}

	fprintf(child_in, "capabilities\n");
	fflush(child_in);

	while (!strbuf_getline(&line, child_out)) {
		if (!line.len)
			break;
		if (!strcmp(line.buf, "get"))
			found_get = 1;
	}
	strbuf_release(&line);

	if (!found_get) {
		result = error(_("insufficient capabilities"));
		goto cleanup;
	}

	fprintf(child_in, "get %s %s\n\n", uri, file);

cleanup:
	if (child_in)
		fclose(child_in);
	if (finish_command(&cp))
		return 1;
	if (child_out)
		fclose(child_out);
	return result;
}

static int copy_uri_to_file(const char *uri, const char *file)
{
	const char *out;
	if (skip_prefix(uri, "https:", &out) ||
	    skip_prefix(uri, "http:", &out))
		return download_https_uri_to_file(uri, file);

	if (!skip_prefix(uri, "file://", &out))
		out = uri;

	/* Copy as a file */
	return !!copy_file(out, file, 0);
}

static int unbundle_from_file(struct repository *r, const char *file)
{
	int result = 0;
	int bundle_fd;
	struct bundle_header header = BUNDLE_HEADER_INIT;
	struct strvec extra_index_pack_args = STRVEC_INIT;
	struct string_list_item *refname;
	struct strbuf bundle_ref = STRBUF_INIT;
	size_t bundle_prefix_len;

	if ((bundle_fd = read_bundle_header(file, &header)) < 0)
		return 1;

	result = unbundle(r, &header, bundle_fd, &extra_index_pack_args);

	/*
	 * Convert all refs/heads/ from the bundle into refs/bundles/
	 * in the local repository.
	 */
	strbuf_addstr(&bundle_ref, "refs/bundles/");
	bundle_prefix_len = bundle_ref.len;

	for_each_string_list_item(refname, &header.references) {
		struct object_id *oid = refname->util;
		struct object_id old_oid;
		const char *branch_name;
		int has_old;

		if (!skip_prefix(refname->string, "refs/heads/", &branch_name))
			continue;

		strbuf_setlen(&bundle_ref, bundle_prefix_len);
		strbuf_addstr(&bundle_ref, branch_name);

		has_old = !read_ref(bundle_ref.buf, &old_oid);
		update_ref("fetched bundle", bundle_ref.buf, oid,
			   has_old ? &old_oid : NULL,
			   REF_SKIP_OID_VERIFICATION,
			   UPDATE_REFS_MSG_ON_ERR);
	}

	bundle_header_release(&header);
	return result;
}

struct bundle_list_context {
	struct repository *r;
	struct bundle_list *list;
	enum bundle_list_mode mode;
	int count;
	int depth;
};

/*
 * This early definition is necessary because we use indirect recursion:
 *
 * While iterating through a bundle list that was downloaded as part
 * of fetch_bundle_uri_internal(), iterator methods eventually call it
 * again, but with depth + 1.
 */
static int fetch_bundle_uri_internal(struct repository *r,
				     struct remote_bundle_info *bundle,
				     int depth,
				     struct bundle_list *list);

static int download_bundle_to_file(struct remote_bundle_info *bundle, void *data)
{
	struct bundle_list_context *ctx = data;

	if (ctx->mode == BUNDLE_MODE_ANY && ctx->count)
		return 0;

	ctx->count++;
	return fetch_bundle_uri_internal(ctx->r, bundle, ctx->depth + 1, ctx->list);
}

static int download_bundle_list(struct repository *r,
				struct bundle_list *local_list,
				struct bundle_list *global_list,
				int depth)
{
	struct bundle_list_context ctx = {
		.r = r,
		.list = global_list,
		.depth = depth + 1,
		.mode = local_list->mode,
	};

	return for_all_bundles_in_list(local_list, download_bundle_to_file, &ctx);
}

static int fetch_bundle_list_in_config_format(struct repository *r,
					      struct bundle_list *global_list,
					      struct remote_bundle_info *bundle,
					      int depth)
{
	int result;
	struct bundle_list list_from_bundle;

	init_bundle_list(&list_from_bundle);

	if ((result = parse_bundle_list_in_config_format(bundle->uri,
							 bundle->file.buf,
							 &list_from_bundle)))
		goto cleanup;

	if (list_from_bundle.mode == BUNDLE_MODE_NONE) {
		warning(_("unrecognized bundle mode from URI '%s'"),
			bundle->uri);
		result = -1;
		goto cleanup;
	}

	if ((result = download_bundle_list(r, &list_from_bundle,
					   global_list, depth)))
		goto cleanup;

cleanup:
	clear_bundle_list(&list_from_bundle);
	return result;
}

/**
 * This limits the recursion on fetch_bundle_uri_internal() when following
 * bundle lists.
 */
static int max_bundle_uri_depth = 4;

/**
 * Recursively download all bundles advertised at the given URI
 * to files. If the file is a bundle, then add it to the given
 * 'list'. Otherwise, expect a bundle list and recurse on the
 * URIs in that list according to the list mode (ANY or ALL).
 */
static int fetch_bundle_uri_internal(struct repository *r,
				     struct remote_bundle_info *bundle,
				     int depth,
				     struct bundle_list *list)
{
	int result = 0;
	struct remote_bundle_info *bcopy;

	if (depth >= max_bundle_uri_depth) {
		warning(_("exceeded bundle URI recursion limit (%d)"),
			max_bundle_uri_depth);
		return -1;
	}

	if (!bundle->file.len)
		find_temp_filename(&bundle->file);
	if ((result = copy_uri_to_file(bundle->uri, bundle->file.buf)))
		goto cleanup;

	if ((result = !is_bundle(bundle->file.buf, 1))) {
		result = fetch_bundle_list_in_config_format(
				r, list, bundle, depth);
		goto cleanup;
	}

	/* Copy the bundle and insert it into the global list. */
	CALLOC_ARRAY(bcopy, 1);
	bcopy->id = xstrdup(bundle->id);
	strbuf_init(&bcopy->file, 0);
	strbuf_add(&bcopy->file, bundle->file.buf, bundle->file.len);
	strbuf_detach(&bundle->file, NULL);
	hashmap_entry_init(&bcopy->ent, strhash(bcopy->id));
	hashmap_add(&list->bundles, &bcopy->ent);

cleanup:
	if (bundle->file.len)
		unlink(bundle->file.buf);
	return result;
}

struct attempt_unbundle_context {
	struct repository *r;
	int success_count;
	int failure_count;
};

static int attempt_unbundle(struct remote_bundle_info *info, void *data)
{
	struct attempt_unbundle_context *ctx = data;

	if (info->unbundled || !unbundle_from_file(ctx->r, info->file.buf)) {
		ctx->success_count++;
		info->unbundled = 1;
	} else {
		ctx->failure_count++;
	}

	return 0;
}

static int unbundle_all_bundles(struct repository *r,
				struct bundle_list *list)
{
	int last_success_count = -1;
	struct attempt_unbundle_context ctx = {
		.r = r,
	};

	/*
	 * Iterate through all bundles looking for ones that can
	 * successfully unbundle. If any succeed, then perhaps another
	 * will succeed in the next attempt.
	 */
	while (last_success_count < ctx.success_count) {
		last_success_count = ctx.success_count;

		ctx.success_count = 0;
		ctx.failure_count = 0;
		for_all_bundles_in_list(list, attempt_unbundle, &ctx);
	}

	if (ctx.success_count)
		git_config_set_multivar_gently("log.excludedecoration",
						"refs/bundle/",
						"refs/bundle/",
						CONFIG_FLAGS_FIXED_VALUE |
						CONFIG_FLAGS_MULTI_REPLACE);

	if (ctx.failure_count) {
		warning(_("failed to unbundle %d bundles"),
			ctx.failure_count);
		return -1;
	}

	return 0;
}

static int unlink_bundle(struct remote_bundle_info *info, void *data)
{
	if (info->file.buf)
		unlink_or_warn(info->file.buf);
	return 0;
}

int fetch_bundle_uri(struct repository *r, const char *uri)
{
	int result;
	struct bundle_list list;
	struct remote_bundle_info bundle = {
		.uri = xstrdup(uri),
		.id = xstrdup("<root>"),
		.file = STRBUF_INIT,
	};

	init_bundle_list(&list);

	/* If a bundle is added to this global list, then it is required. */
	list.mode = BUNDLE_MODE_ALL;

	if ((result = fetch_bundle_uri_internal(r, &bundle, 0, &list)))
		goto cleanup;

	result = unbundle_all_bundles(r, &list);

cleanup:
	for_all_bundles_in_list(&list, unlink_bundle, NULL);
	clear_bundle_list(&list);
	clear_remote_bundle_info(&bundle, NULL);
	return result;
}

/**
 * API for serve.c.
 */

static int advertise_bundle_uri = -1;

int bundle_uri_advertise(struct repository *r, struct strbuf *value)
{
	if (value &&
	    git_env_bool("GIT_TEST_BUNDLE_URI_UNKNOWN_CAPABILITY_VALUE", 0))
		strbuf_addstr(value, "test-unknown-capability-value");

	if (advertise_bundle_uri != -1)
		goto cached;

	advertise_bundle_uri = 0;
	git_config_get_maybe_bool("uploadpack.advertisebundleuris", &advertise_bundle_uri);

cached:
	return advertise_bundle_uri;
}

static int config_to_packet_line(const char *key, const char *value, void *data)
{
	struct packet_reader *writer = data;

	if (!strncmp(key, "bundle.", 7))
		packet_write_fmt(writer->fd, "%s=%s", key, value);

	return 0;
}

int bundle_uri_command(struct repository *r,
		       struct packet_reader *request)
{
	struct packet_writer writer;
	packet_writer_init(&writer, 1);

	while (packet_reader_read(request) == PACKET_READ_NORMAL)
		die(_("bundle-uri: unexpected argument: '%s'"), request->line);
	if (request->status != PACKET_READ_FLUSH)
		die(_("bundle-uri: expected flush after arguments"));

	/*
	 * Read all "bundle.*" config lines to the client as key=value
	 * packet lines.
	 */
	git_config(config_to_packet_line, &writer);

	packet_writer_flush(&writer);

	return 0;
}

/**
 * General API for {transport,connect}.c etc.
 */
int bundle_uri_parse_line(struct bundle_list *list, const char *line)
{
	int result;
	const char *equals;
	struct strbuf key = STRBUF_INIT;

	if (!strlen(line))
		return error(_("bundle-uri: got an empty line"));

	equals = strchr(line, '=');

	if (!equals)
		return error(_("bundle-uri: line is not of the form 'key=value'"));
	if (line == equals || !*(equals + 1))
		return error(_("bundle-uri: line has empty key or value"));

	strbuf_add(&key, line, equals - line);
	result = bundle_list_update(key.buf, equals + 1, list);
	strbuf_release(&key);

	return result;
}
