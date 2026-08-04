// SPDX-License-Identifier: GPL-2.0-only
/*
 * Mount parameters (fs_context), in genuine AUFS option syntax:
 *
 *   mount -t aufs -o br:PATH=rw[:PATH=MODE...],xino=PATH,udba=MODE,
 *                    dirperm1,nowarn_perm  aufs <mountpoint>
 *   mount -o remount,dirperm1,add=N:PATH=MODE  <mountpoint>
 *   mount -t aufs -o remount,del=PATH          <mountpoint>
 *
 * AUFS spells some options "keyword:value".  generic_parse_monolithic()
 * splits on the first '=' instead, turning "br:/path=rw" into
 * key="br:/path" - so, as AUFS's is_colonopt() does, the first colon
 * after such a keyword is rewritten to '=' first.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include "aufsng.h"

enum aufsng_opt {
	Opt_br,
	Opt_add,
	Opt_del,
	Opt_xino,
	Opt_noxino,
	Opt_udba,
	Opt_dirperm1,
	Opt_nowarn_perm,
	Opt_si,
};

const struct fs_parameter_spec aufsng_parameter_spec[] = {
	fsparam_string("br",		Opt_br),
	fsparam_string("add",		Opt_add),
	fsparam_string("del",		Opt_del),
	fsparam_string("xino",		Opt_xino),
	fsparam_flag("noxino",		Opt_noxino),
	fsparam_string("udba",		Opt_udba),
	fsparam_flag("dirperm1",	Opt_dirperm1),
	fsparam_flag("nowarn_perm",	Opt_nowarn_perm),
	fsparam_string("si",		Opt_si),
	{}
};

/*
 * These keyword names accept "keyword:value" as well as
 * "keyword=value"; only the FIRST colon right after the keyword is
 * ever rewritten to '=' (matching AUFS's is_colonopt()).
 */
static unsigned int aufsng_is_colonopt(const char *str)
{
	static const char * const names[] = {
		"br", "add", "del", NULL,
	};
	int i;

	for (i = 0; names[i]; i++) {
		size_t len = strlen(names[i]);

		if (!strncmp(str, names[i], len) && str[len] == ':')
			return len;
	}
	return 0;
}

static int aufsng_parse_monolithic(struct fs_context *fc, void *data)
{
	char *str = data;
	char *seg = str;
	unsigned int off;

	while (seg) {
		off = aufsng_is_colonopt(seg);
		if (off)
			seg[off] = '=';
		seg = strchr(seg, ',');
		if (!seg)
			break;
		seg++;
	}

	return generic_parse_monolithic(fc, str);
}

static int aufsng_mount_dir(struct fs_context *fc, const char *name,
			 struct path *path)
{
	int err;

	err = kern_path(name, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, path);
	if (err) {
		errorfc(fc, "failed to resolve '%s': %i", name, err);
		return err;
	}
	return 0;
}

/*
 * A branch mode: "rw"/"ro"/"rr" plus optional "+attr" suffixes.  "rr"
 * and "ro" are equivalent here, and the suffixes parse but have no
 * effect.  -EINVAL if @s is not a mode at all.
 */
static int aufsng_parse_perm(const char *s, enum aufsng_br_perm *perm)
{
	if (!strncmp(s, "rw", 2))
		*perm = AUFSNG_BR_RW;
	else if (!strncmp(s, "ro", 2) || !strncmp(s, "rr", 2))
		*perm = AUFSNG_BR_RO;
	else
		return -EINVAL;
	if (s[2] != '\0' && s[2] != '+')
		return -EINVAL;
	return 0;
}

/*
 * Split "PATH=MODE" (MODE optional, default ro); @spec is consumed.
 * The tail after the last '=' counts as a mode only if it parses as
 * one - "br:/data/a=b" is a path, not a mode "b".
 */
static int aufsng_parse_branch_spec(struct fs_context *fc, char *spec,
				 struct aufsng_ctx_branch *out)
{
	char *eq = strrchr(spec, '=');
	enum aufsng_br_perm perm = AUFSNG_BR_RO;
	int err;

	if (eq && !aufsng_parse_perm(eq + 1, &perm))
		*eq = '\0';

	out->name = kstrdup(spec, GFP_KERNEL);
	if (!out->name)
		return -ENOMEM;

	err = aufsng_mount_dir(fc, spec, &out->path);
	if (err) {
		kfree(out->name);
		out->name = NULL;
		return err;
	}
	out->perm = perm;
	return 0;
}

/* "br:PATH=MODE[:PATH=MODE...]", mount time only */
static int aufsng_parse_br(struct fs_context *fc, char *value)
{
	struct aufsng_fs_context *ctx = fc->fs_private;
	char *tok;
	int err;

	while ((tok = strsep(&value, ":")) != NULL) {
		if (!*tok)
			continue;
		if (ctx->nr >= AUFSNG_MAX_STACK)
			return invalfc(fc, "too many branches, limit is %d",
				       AUFSNG_MAX_STACK);
		err = aufsng_grow_array((void **)&ctx->br, &ctx->cap,
				     ctx->nr + 1, sizeof(*ctx->br),
				     GFP_KERNEL_ACCOUNT);
		if (err)
			return err;
		err = aufsng_parse_branch_spec(fc, tok, &ctx->br[ctx->nr]);
		if (err)
			return err;
		ctx->nr++;
	}
	return 0;
}

/* "add=N:PATH=MODE", remount only */
static int aufsng_parse_add(struct fs_context *fc, char *value)
{
	struct aufsng_fs_context *ctx = fc->fs_private;
	char *colon = strchr(value, ':');
	unsigned int pos;
	int err;

	if (fc->purpose != FS_CONTEXT_FOR_RECONFIGURE)
		return invalfc(fc, "add= is only valid on remount");
	if (!colon)
		return invalfc(fc, "add= requires N:PATH=MODE");
	*colon = '\0';
	if (kstrtouint(value, 10, &pos))
		return invalfc(fc, "add= index must be numeric");
	/*
	 * Only add=1: (new top lower) is implemented: the in-place splice
	 * has no deeper insert position, so another index would apply to
	 * the root stack only and leave cached dirs in a different order.
	 * Reject rather than silently misplace.
	 */
	if (pos != 1)
		return invalfc(fc, "add=%u: is not supported, only add=1: (top of the read-only stack)",
			       pos);

	err = aufsng_grow_array((void **)&ctx->dyn_add, &ctx->cap_dyn_add,
			     ctx->nr_dyn_add + 1, sizeof(*ctx->dyn_add),
			     GFP_KERNEL_ACCOUNT);
	if (err)
		return err;
	err = aufsng_parse_branch_spec(fc, colon + 1,
				    &ctx->dyn_add[ctx->nr_dyn_add]);
	if (err)
		return err;
	ctx->nr_dyn_add++;
	return 0;
}

/* "del=PATH", remount only */
static int aufsng_parse_del(struct fs_context *fc, char *value)
{
	struct aufsng_fs_context *ctx = fc->fs_private;
	struct path path;
	int err;

	if (fc->purpose != FS_CONTEXT_FOR_RECONFIGURE)
		return invalfc(fc, "del= is only valid on remount");

	err = aufsng_mount_dir(fc, value, &path);
	if (err)
		return err;

	err = aufsng_grow_array((void **)&ctx->dyn_del, &ctx->cap_dyn_del,
			     ctx->nr_dyn_del + 1, sizeof(*ctx->dyn_del),
			     GFP_KERNEL_ACCOUNT);
	if (err) {
		path_put(&path);
		return err;
	}
	ctx->dyn_del[ctx->nr_dyn_del++] = path;
	return 0;
}

static int aufsng_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct aufsng_fs_context *ctx = fc->fs_private;
	struct fs_parse_result result;
	int opt;

	opt = fs_parse(fc, aufsng_parameter_spec, param, &result);
	if (opt < 0) {
		/*
		 * Tools replay the current options on "-o remount";
		 * tolerate unknowns there, as AUFS and overlayfs do.
		 */
		if (fc->purpose == FS_CONTEXT_FOR_RECONFIGURE &&
		    opt == -ENOPARAM)
			return 0;
		return opt;
	}

	switch (opt) {
	case Opt_br:
		if (fc->purpose == FS_CONTEXT_FOR_RECONFIGURE)
			return 0;	/* replayed option, ignore */
		return aufsng_parse_br(fc, param->string);
	case Opt_add:
		return aufsng_parse_add(fc, param->string);
	case Opt_del:
		return aufsng_parse_del(fc, param->string);
	case Opt_xino:
		if (fc->purpose == FS_CONTEXT_FOR_RECONFIGURE)
			return 0;
		kfree(ctx->config.xino_path);
		ctx->config.xino_path = kstrdup(param->string, GFP_KERNEL);
		if (!ctx->config.xino_path)
			return -ENOMEM;
		return 0;
	case Opt_noxino:
		return 0;
	case Opt_udba:
		/*
		 * AUFS's own value set; anything else is rejected, as AUFS
		 * rejects it.  A typo mapping to "none" would disable
		 * revalidation behind the user's back.
		 */
		if (!strcmp(param->string, "notify") ||
		    !strcmp(param->string, "fsnotify"))
			ctx->config.udba = AUFSNG_UDBA_NOTIFY;
		else if (!strcmp(param->string, "reval"))
			ctx->config.udba = AUFSNG_UDBA_REVAL;
		else if (!strcmp(param->string, "none"))
			ctx->config.udba = AUFSNG_UDBA_NONE;
		else
			return invalfc(fc, "unknown udba value '%s'",
				       param->string);
		ctx->udba_set = true;
		return 0;
	case Opt_dirperm1:
	case Opt_nowarn_perm:
		return 0;	/* accepted, no functional effect needed */
	case Opt_si:
		return 0;	/* our own output, replayed back; ignored as in AUFS */
	}

	return -EINVAL;
}

static int aufsng_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, aufsng_fill_super);
}

static void aufsng_reset_branches(struct aufsng_ctx_branch *br, size_t nr)
{
	size_t i;

	for (i = 0; i < nr; i++) {
		path_put(&br[i].path);
		kfree(br[i].name);
	}
}

static void aufsng_free_fs_context(struct fs_context *fc)
{
	struct aufsng_fs_context *ctx = fc->fs_private;
	size_t i;

	if (!ctx)
		return;

	aufsng_reset_branches(ctx->br, ctx->nr);
	kfree(ctx->br);
	aufsng_reset_branches(ctx->dyn_add, ctx->nr_dyn_add);
	kfree(ctx->dyn_add);
	for (i = 0; i < ctx->nr_dyn_del; i++)
		path_put(&ctx->dyn_del[i]);
	kfree(ctx->dyn_del);
	kfree(ctx->config.xino_path);
	kfree(ctx);
	fc->fs_private = NULL;
}

static const struct fs_context_operations aufsng_context_ops = {
	.parse_param		= aufsng_parse_param,
	.parse_monolithic	= aufsng_parse_monolithic,
	.get_tree		= aufsng_get_tree,
	.reconfigure		= aufsng_dyn_reconfigure,
	.free			= aufsng_free_fs_context,
};

int aufsng_init_fs_context(struct fs_context *fc)
{
	struct aufsng_fs_context *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	/* AUFS's own default: revalidate branches on access (see udba=) */
	ctx->config.udba = AUFSNG_UDBA_REVAL;

	fc->fs_private = ctx;
	fc->ops = &aufsng_context_ops;
	return 0;
}
