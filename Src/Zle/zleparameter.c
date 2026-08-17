/*
 * zleparameter.c - parameter interface to zle internals
 *
 * This file is part of zsh, the Z shell.
 *
 * Copyright (c) 1999 Sven Wischnowsky
 * All rights reserved.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and to distribute modified versions of this software for any
 * purpose, provided that the above copyright notice and the following
 * two paragraphs appear in all copies of this software.
 *
 * In no event shall Sven Wischnowsky or the Zsh Development Group be liable
 * to any party for direct, indirect, special, incidental, or consequential
 * damages arising out of the use of this software and its documentation,
 * even if Sven Wischnowsky and the Zsh Development Group have been advised of
 * the possibility of such damage.
 *
 * Sven Wischnowsky and the Zsh Development Group specifically disclaim any
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose.  The software
 * provided hereunder is on an "as is" basis, and Sven Wischnowsky and the
 * Zsh Development Group have no obligation to provide maintenance,
 * support, updates, enhancements, or modifications.
 *
 */

#include "zleparameter.mdh"
#include "zleparameter.pro"

/* Functions for the zlewidgets special parameter. */

/**/
static char *
widgetstr(Widget w)
{
    if (!w)
	return dupstring("undefined");
    if (w->flags & WIDGET_INT)
	return dupstring("builtin");
    if (w->flags & WIDGET_NCOMP) {
	char *t = (char *) zhalloc(13 + strlen(w->u.comp.wid) +
				   strlen(w->u.comp.func));

	strcpy(t, "completion:");
	strcat(t, w->u.comp.wid);
	strcat(t, ":");
	strcat(t, w->u.comp.func);

	return t;
    }
    return dyncat("user:", w->u.fnnam);
}

/**/
static HashNode
getpmwidgets(UNUSED(HashTable ht), const char *name)
{
    Param pm = NULL;
    Thingy th;

    pm = (Param) hcalloc(sizeof(struct param));
    pm->node.nam = dupstring(name);
    pm->node.flags = PM_SCALAR | PM_READONLY;
    pm->gsu.s = &nullsetscalar_gsu;

    if ((th = (Thingy) thingytab->getnode(thingytab, name)) &&
	!(th->flags & DISABLED))
	pm->u.str = widgetstr(th->widget);
    else {
	pm->u.str = dupstring("");
	pm->node.flags |= PM_UNSET;
    }
    return &pm->node;
}

/**/
static void
scanpmwidgets(UNUSED(HashTable ht), ScanFunc func, int flags)
{
    struct param pm;
    int i;
    HashNode hn;

    memset((void *)&pm, 0, sizeof(struct param));
    pm.node.flags = PM_SCALAR | PM_READONLY;
    pm.gsu.s = &nullsetscalar_gsu;

    for (i = 0; i < thingytab->hsize; i++)
	for (hn = thingytab->nodes[i]; hn; hn = hn->next) {
	    pm.node.nam = hn->nam;
	    if (func != scancountparams &&
		((flags & (SCANPM_WANTVALS|SCANPM_MATCHVAL)) ||
		 !(flags & SCANPM_WANTKEYS)))
		pm.u.str = widgetstr(((Thingy) hn)->widget);
	    func(&pm.node, flags);
	}
}

/* Functions for the zlekeymaps special parameter. */

static char **
keymapsgetfn(UNUSED(Param pm))
{
    int i;
    HashNode hn;
    char **ret, **p;

    p = ret = (char **) zhalloc((keymapnamtab->ct + 1) * sizeof(char *));

    for (i = 0; i < keymapnamtab->hsize; i++)
	for (hn = keymapnamtab->nodes[i]; hn; hn = hn->next)
	    *p++ = dupstring(hn->nam);
    *p = NULL;

    return ret;
}

/*
 * This is a duplicate of stdhash_gsu.  On some systems
 * (such as Cygwin) we can't put a pointer to an imported variable
 * in a compile-time initialiser, so we use this instead.
 */
static const struct gsu_hash zlestdhash_gsu =
{ hashgetfn, hashsetfn, stdunsetfn };
static __thread const struct gsu_array keymaps_gsu =
{ keymapsgetfn, arrsetfn, stdunsetfn };

/* AOK: partab's initialiser takes the address of a thread-local,
   so it cannot be a static initialiser any more. See
   tools/zsh-tls-fix-tables.py. */
typedef struct paramdef aok_tt_partab[2];
static __thread aok_tt_partab aok_tv_partab;
static __thread char aok_ti_partab;
static aok_tt_partab *aok_tf_partab(void) {
    if (!aok_ti_partab) {
        struct paramdef aok_tmp[] = {
    SPECIALPMDEF("keymaps", PM_ARRAY|PM_READONLY, &keymaps_gsu, NULL, NULL),
    SPECIALPMDEF("widgets", PM_READONLY,
		 &zlestdhash_gsu, getpmwidgets, scanpmwidgets)
};
        _Static_assert(sizeof(aok_tmp)/sizeof(aok_tmp[0]) == 2,
                       "partab: zsh-tls-fix-tables miscounted");
        memcpy(aok_tv_partab, aok_tmp, sizeof(aok_tmp));
        aok_ti_partab = 1;
    }
    return &aok_tv_partab;
}
#define partab (*aok_tf_partab())


/* AOK: see tools/zsh-tls-fix-tables.py. */
static __thread struct features aok_tv_module_features;
static __thread char aok_ti_module_features;
static struct features *aok_tf_module_features(void) {
    if (!aok_ti_module_features) {
        struct features aok_tmp = {
    NULL, 0,
    NULL, 0,
    NULL, 0,
    partab, sizeof(partab)/sizeof(*partab),
    0
};
        aok_tv_module_features = aok_tmp;
        aok_ti_module_features = 1;
    }
    return &aok_tv_module_features;
}
#define module_features (*aok_tf_module_features())


/**/
int
setup_(UNUSED(Module m))
{
    return 0;
}

/**/
int
features_(Module m, char ***features)
{
    *features = featuresarray(m, &module_features);
    return 0;
}

/**/
int
enables_(Module m, int **enables)
{
    return handlefeatures(m, &module_features, enables);
}

/**/
int
boot_(UNUSED(Module m))
{
    return 0;
}

/**/
int
cleanup_(Module m)
{
    return setfeatureenables(m, &module_features, NULL);
}

/**/
int
finish_(UNUSED(Module m))
{
    return 0;
}
