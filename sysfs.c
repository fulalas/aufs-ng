// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/string_helpers.h>
#include "aufsng.h"

struct aufsng_br_attr {
	struct kobj_attribute ka;
	unsigned int idx;
	char name[sizeof("br" __stringify(AUFSNG_MAXBRANCH))];
};

struct aufsng_sysfs {
	struct kobject kobj;
	struct super_block *sb;
	unsigned int nr;
	size_t cap;
	struct aufsng_br_attr **attrs;
};

static struct kobject *aufsng_sysfs_root;

static const char aufsng_esc_chars[] =
	"\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10"
	"\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f\x20"
	"\x7f\\";

static const char * const aufsng_perm_str[] = {
	[AUFSNG_BR_RW] = "rw",
	[AUFSNG_BR_RO] = "ro",
	[AUFSNG_BR_RR] = "rr",
};

static ssize_t aufsng_br_show(struct kobject *kobj, struct kobj_attribute *ka,
			      char *buf)
{
	struct aufsng_sysfs *s = container_of(kobj, struct aufsng_sysfs, kobj);
	struct aufsng_br_attr *a = container_of(ka, struct aufsng_br_attr, ka);
	struct aufsng_fs *pfs = AUFSNG_FS(s->sb);
	const struct aufsng_layer *layer = NULL;
	int room = PAGE_SIZE - sizeof("=rw\n");
	char *tmp = __getname(), *p = ERR_PTR(-ENOENT);
	enum aufsng_br_perm perm = AUFSNG_BR_RO;
	int len;

	if (!tmp)
		return -ENOMEM;
	percpu_down_read(&pfs->dyn_lock);
	if (!a->idx) {
		layer = pfs->upper;
	} else {
		struct aufsng_entry *oe = AUFSNG_E(s->sb->s_root);

		if (a->idx <= oe->numlower)
			layer = oe->lowerstack[a->idx - 1].layer;
	}
	if (layer) {
		p = d_path(&layer->user_path, tmp, PATH_MAX);
		perm = layer->perm;
	}
	percpu_up_read(&pfs->dyn_lock);

	if (IS_ERR(p)) {
		len = PTR_ERR(p);
	} else {
		len = string_escape_str(p, buf, room, ESCAPE_OCTAL,
					aufsng_esc_chars);
		if (len >= room)
			len = -ENAMETOOLONG;
		else
			len += sysfs_emit_at(buf, len, "=%s\n",
					     aufsng_perm_str[perm]);
	}
	__putname(tmp);
	return len;
}

static void aufsng_sysfs_release(struct kobject *kobj)
{
	struct aufsng_sysfs *s = container_of(kobj, struct aufsng_sysfs, kobj);
	size_t i;

	for (i = 0; i < s->nr; i++)
		kfree(s->attrs[i]);
	kfree(s->attrs);
	kfree(s);
}

static const struct kobj_type aufsng_sysfs_ktype = {
	.release	= aufsng_sysfs_release,
	.sysfs_ops	= &kobj_sysfs_ops,
};

static struct aufsng_br_attr *aufsng_br_attr_new(unsigned int idx)
{
	struct aufsng_br_attr *a = kzalloc(sizeof(*a), GFP_KERNEL);

	if (!a)
		return NULL;
	a->idx = idx;
	snprintf(a->name, sizeof(a->name), "br%u", idx);
	sysfs_attr_init(&a->ka.attr);
	a->ka.attr.name = a->name;
	a->ka.attr.mode = 0444;
	a->ka.show = aufsng_br_show;
	return a;
}

void aufsng_sysfs_sync(struct super_block *sb)
{
	struct aufsng_sysfs *s = AUFSNG_FS(sb)->sysfs;
	unsigned int want = 1 + AUFSNG_E(sb->s_root)->numlower;

	while (s->nr > want) {
		s->nr--;
		sysfs_remove_file(&s->kobj, &s->attrs[s->nr]->ka.attr);
		kfree(s->attrs[s->nr]);
	}
	while (s->nr < want) {
		struct aufsng_br_attr *a = NULL;

		if (!aufsng_grow_array((void **)&s->attrs, &s->cap, s->nr + 1,
				       sizeof(*s->attrs), GFP_KERNEL))
			a = aufsng_br_attr_new(s->nr);
		if (!a || sysfs_create_file(&s->kobj, &a->ka.attr)) {
			kfree(a);
			pr_warn("aufs (aufs-ng): si_%lx: cannot list branch %u in sysfs\n",
				AUFSNG_FS(sb)->si_id, s->nr);
			break;
		}
		s->attrs[s->nr++] = a;
	}
}

int aufsng_sysfs_mount(struct super_block *sb)
{
	struct aufsng_fs *pfs = AUFSNG_FS(sb);
	struct aufsng_sysfs *s;
	int err;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->sb = sb;
	err = kobject_init_and_add(&s->kobj, &aufsng_sysfs_ktype,
				   aufsng_sysfs_root, "si_%lx", pfs->si_id);
	if (err) {
		kobject_put(&s->kobj);
		return err;
	}
	pfs->sysfs = s;
	aufsng_sysfs_sync(sb);
	return 0;
}

void aufsng_sysfs_umount(struct super_block *sb)
{
	struct aufsng_fs *pfs = AUFSNG_FS(sb);

	if (!pfs || !pfs->sysfs)
		return;
	kobject_del(&pfs->sysfs->kobj);
	kobject_put(&pfs->sysfs->kobj);
	pfs->sysfs = NULL;
}

int __init aufsng_sysfs_init(void)
{
	aufsng_sysfs_root = kobject_create_and_add(AUFSNG_NAME, fs_kobj);
	return aufsng_sysfs_root ? 0 : -ENOMEM;
}

void aufsng_sysfs_exit(void)
{
	kobject_put(aufsng_sysfs_root);
}
