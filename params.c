// SPDX-License-Identifier: GPL-2.0-only
/*
 * aufs-ng mount parameter handling (fs_context), speaking genuine
 * AUFS option syntax so that scripts issuing real AUFS mount/remount
 * commands need no changes:
 *
 *   mount -t aufs -o br:PATH=rw[:PATH=MODE...],xino=PATH,udba=MODE,
 *                    dirperm1,nowarn_perm  aufs <mountpoint>
 *   mount -o remount,dirperm1,add=N:PATH=MODE  <mountpoint>
 *   mount -t aufs -o remount,del=PATH          <mountpoint>
 *
 * AUFS accepts "keyword:value" as an alternate spelling of
 * "keyword=value" for a handful of options whose value itself
 * contains further ':'/'=' structure (br, add, del, ...).  The
 * generic mount-option splitter (generic_parse_monolithic) only
 * splits on commas and then the first '=' within each segment, so
 * "br:/path=rw" would otherwise be parsed as key="br:/path",
 * value="rw" - wrong.  We replicate AUFS's own fix: before handing
 * the raw option string to the generic splitter, rewrite the FIRST
 * colon following one of these keywords into '=', exactly as AUFS's
 * is_colonopt()/au_fsctx_parse_monolithic() do (verified against
 * fs/aufs/fsctx.c in the upstream aufs-standalone source).
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
 * Parse an AUFS branch mode: a base permission "rw"/"ro"/"rr",
 * optionally followed by "+attr" suffixes ("ro+wh", "rw+nolwh", ...).
 * "rr" (real-readonly, e.g. squashfs) and "ro" are equivalent here -
 * neither is ever written to - and the attributes tune whiteout
 * handling that aufs-ng applies uniformly, so they parse but have no
 * further effect.  Returns -EINVAL if @s is not a mode at all.
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
 * Split "PATH=MODE" (MODE optional, defaults to ro) into a resolved
 * path and a permission.  @spec is consumed (NUL bytes inserted).
 * The suffix after the last '=' is treated as a mode only when it
 * actually parses as one; otherwise the '=' belongs to the branch
 * path itself ("br:/data/a=b" is a path, not a mode "b").
 */
static int aufsng_parse_branch_spec(struct fs_context *fc, char *spec,
				 struct aufsng_ctx_branch *out)
{
	char *eq = strrchr(spec, '=');
	enum aufsng_br_perm perm = AUFSNG_BR_RO;
	const char *permstr = "ro";
	int err;

	if (eq && !aufsng_parse_perm(eq + 1, &perm)) {
		*eq = '\0';
		permstr = eq + 1;
	}
	strscpy(out->permstr, permstr, sizeof(out->permstr));

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

static int aufsng_ctx_realloc(void **arr, size_t *cap, size_t need,
			  size_t elemsize)
{
	void *p;
	size_t nr;

	if (need <= *cap)
		return 0;
	nr = max_t(size_t, 16, *cap * 2);
	if (nr < need)
		nr = need;
	p = krealloc_array(*arr, nr, elemsize, GFP_KERNEL_ACCOUNT);
	if (!p)
		return -ENOMEM;
	*arr = p;
	*cap = nr;
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
		err = aufsng_ctx_realloc((void **)&ctx->br, &ctx->cap,
				      ctx->nr + 1, sizeof(*ctx->br));
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
	 * Only "add=1:" (insert immediately below the writable branch,
	 * i.e. as the new TOP lower) is implemented: the in-place splice
	 * into cached directories has no notion of a deeper insert
	 * position, so accepting another index would honor it in the
	 * root stack only and leave cached directories merged in a
	 * different priority order (and "0" would mean outranking the
	 * writable branch, which a single-rw design cannot do).  Reject
	 * rather than silently misplace.
	 */
	if (pos != 1)
		return invalfc(fc, "add=%u: is not supported, only add=1: (top of the read-only stack)",
			       pos);

	err = aufsng_ctx_realloc((void **)&ctx->dyn_add, &ctx->cap_dyn_add,
			      ctx->nr_dyn_add + 1, sizeof(*ctx->dyn_add));
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

	err = aufsng_ctx_realloc((void **)&ctx->dyn_del, &ctx->cap_dyn_del,
			      ctx->nr_dyn_del + 1, sizeof(*ctx->dyn_del));
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
		 * Tools replay a mount's current options on "-o
		 * remount"; tolerate anything unknown there instead of
		 * failing the whole remount (matches how overlayfs and
		 * real aufs both behave on legacy remounts).
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
		 * The value set real AUFS accepts (au_udba_val); anything
		 * else is rejected, as AUFS rejects it - a typo silently
		 * mapping to "none" would disable revalidation while the
		 * user believes it is on (worse on remount, where the
		 * explicit value overrides the mount-time choice).
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

static int aufsng_reconfigure(struct fs_context *fc)
{
	return aufsng_dyn_reconfigure(fc);
}

static const struct fs_context_operations aufsng_context_ops = {
	.parse_param		= aufsng_parse_param,
	.parse_monolithic	= aufsng_parse_monolithic,
	.get_tree		= aufsng_get_tree,
	.reconfigure		= aufsng_reconfigure,
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
