/*
 * zle_bindings.c - commands and keymaps
 *
 * This file is part of zsh, the Z shell.
 *
 * Copyright (c) 1992-1997 Paul Falstad
 * All rights reserved.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and to distribute modified versions of this software for any
 * purpose, provided that the above copyright notice and the following
 * two paragraphs appear in all copies of this software.
 *
 * In no event shall Paul Falstad or the Zsh Development Group be liable
 * to any party for direct, indirect, special, incidental, or consequential
 * damages arising out of the use of this software and its documentation,
 * even if Paul Falstad and the Zsh Development Group have been advised of
 * the possibility of such damage.
 *
 * Paul Falstad and the Zsh Development Group specifically disclaim any
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose.  The software
 * provided hereunder is on an "as is" basis, and Paul Falstad and the
 * Zsh Development Group have no obligation to provide maintenance,
 * support, updates, enhancements, or modifications.
 *
 */

#include "zle.mdh"
#include "zle_widget.h"

#include "zle_bindings.pro"

/*
 * widgets is the table of internally implemented widgets.  This
 * table is not used directly, but each widget in it is referenced
 * by address from within the table of thingies (below).  The only
 * complication here is that not all systems support union
 * initialisation.
 *
 * thingies is the table of `known thingies', that exist on startup.
 * Some bits of ZLE rely on some of these thingies always being the
 * ones in this table, rather than doing a name lookup and accepting
 * any semantically identical thingy.  The initial reference count of
 * these thingies is 2: 1 for the widget they name, and 1 extra to
 * make sure they never get deleted.
 *
 * AOK: both tables are per-shell rather than per-process, because both are
 * mutable and two native zsh shells are two threads of one process.  The long
 * version of why, and of why `static __thread' alone will not do it, is on
 * thingies[] in zle.h.  What is left here is the mechanism.
 *
 * Each table's storage is thread-local and its initialiser has moved into an
 * accessor that runs on first touch, which is the shape
 * tools/zsh-tls-fix-tables.py generates for the same wall elsewhere in the
 * tree.  The element counts come from counting the SAME list files with the
 * same macro that fills the tables, so a table and its bound cannot drift
 * apart the way a hand-written count would.
 */

#ifdef HAVE_UNION_INIT
# define BR(X) {X}
# define AOK_WIDGET struct widget
#else /* !HAVE_UNION_INIT */
# define BR(X) X
struct intwidget {
    int flags;
    Thingy first;
    ZleIntFunc fn;
};
# define AOK_WIDGET struct intwidget
#endif /* !HAVE_UNION_INIT */

enum {
    aok_n_widgets = 0
#define W(zle_flags, t_firstname, functionname) + 1
#include "widgets.list"
#undef W
};

typedef AOK_WIDGET aok_tt_widgets[aok_n_widgets];

static __thread aok_tt_widgets aok_tv_widgets;
static __thread char aok_ti_widgets;
static __thread aok_tt_thingies aok_tv_thingies;
static __thread char aok_ti_thingies;

static aok_tt_widgets *aok_tf_widgets(void);

/* Both accessors are in scope from here down, so the list files can name each
 * other's entries exactly as they always did. */
#define widgets (*aok_tf_widgets())

/*
 * The two initialisers are mutually recursive: a widget's `first' is the
 * address of a thingy, a thingy's `widget' is the address of a widget, and a
 * thingy's `samew' is the address of another thingy.  So the done flag is
 * raised BEFORE the initialiser runs rather than after.  Every one of those
 * cross-references is an address-of and none of them reads through the pointer
 * at this point, and a thread-local object has its final address from the
 * moment the thread can name it -- so a re-entrant call hands back that
 * address and returns immediately, the outer call goes on to fill the storage
 * in, and both tables end up complete.  Raising the flag afterwards instead
 * would recurse until the stack ran out.
 */

static aok_tt_widgets *
aok_tf_widgets(void)
{
    if (!aok_ti_widgets) {
	int i = 0;
	aok_ti_widgets = 1;
#define W(zle_flags, t_firstname, functionname) \
	aok_tv_widgets[i++] = (AOK_WIDGET) \
	    { WIDGET_INT | zle_flags, t_firstname, BR(functionname) };
#include "widgets.list"
#undef W
	DPUTS(i != aok_n_widgets, "BUG: widgets[] filled to the wrong length");
    }
    return &aok_tv_widgets;
}

aok_tt_thingies *
aok_tf_thingies(void)
{
    if (!aok_ti_thingies) {
	int i = 0;
	aok_ti_thingies = 1;
#define T(name, th_flags, w_idget, t_next) \
	aok_tv_thingies[i++] = (struct thingy) \
	    { NULL, name, th_flags, 2, w_idget, t_next };
#include "thingies.list"
#undef T
	aok_tv_thingies[i++] = (struct thingy) { NULL, NULL, 0, 0, NULL, NULL };
	DPUTS(i != aok_n_thingies, "BUG: thingies[] filled to the wrong length");
    }
    return &aok_tv_thingies;
}

/*
 * Default key binding tables:
 *
 * In these tables, each element is bound to a single thingy, the index
 * of which in the above table is stored here.
 */

/**/
__thread int emacsbind[32] = {
    /* ^@ */ z_setmarkcommand,
    /* ^A */ z_beginningofline,
    /* ^B */ z_backwardchar,
    /* ^C */ z_undefinedkey,
    /* ^D */ z_deletecharorlist,
    /* ^E */ z_endofline,
    /* ^F */ z_forwardchar,
    /* ^G */ z_sendbreak,
    /* ^H */ z_backwarddeletechar,
    /* ^I */ z_expandorcomplete,
    /* ^J */ z_acceptline,
    /* ^K */ z_killline,
    /* ^L */ z_clearscreen,
    /* ^M */ z_acceptline,
    /* ^N */ z_downlineorhistory,
    /* ^O */ z_acceptlineanddownhistory,
    /* ^P */ z_uplineorhistory,
    /* ^Q */ z_pushline,
    /* ^R */ z_historyincrementalsearchbackward,
    /* ^S */ z_historyincrementalsearchforward,
    /* ^T */ z_transposechars,
    /* ^U */ z_killwholeline,
    /* ^V */ z_quotedinsert,
    /* ^W */ z_backwardkillword,
    /* ^X */ z_undefinedkey,
    /* ^Y */ z_yank,
    /* ^Z */ z_undefinedkey,
    /* ^[ */ z_undefinedkey,
    /* ^\ */ z_undefinedkey,
    /* ^] */ z_undefinedkey,
    /* ^^ */ z_undefinedkey,
    /* ^_ */ z_undo,
};

/**/
__thread int metabind[128] = {
    /* M-^@ */ z_undefinedkey,
    /* M-^A */ z_undefinedkey,
    /* M-^B */ z_undefinedkey,
    /* M-^C */ z_undefinedkey,
    /* M-^D */ z_listchoices,
    /* M-^E */ z_undefinedkey,
    /* M-^F */ z_undefinedkey,
    /* M-^G */ z_sendbreak,
    /* M-^H */ z_backwardkillword,
    /* M-^I */ z_selfinsertunmeta,
    /* M-^J */ z_selfinsertunmeta,
    /* M-^K */ z_undefinedkey,
    /* M-^L */ z_clearscreen,
    /* M-^M */ z_selfinsertunmeta,
    /* M-^N */ z_undefinedkey,
    /* M-^O */ z_undefinedkey,
    /* M-^P */ z_undefinedkey,
    /* M-^Q */ z_undefinedkey,
    /* M-^R */ z_undefinedkey,
    /* M-^S */ z_undefinedkey,
    /* M-^T */ z_undefinedkey,
    /* M-^U */ z_undefinedkey,
    /* M-^V */ z_undefinedkey,
    /* M-^W */ z_undefinedkey,
    /* M-^X */ z_undefinedkey,
    /* M-^Y */ z_undefinedkey,
    /* M-^Z */ z_undefinedkey,
    /* M-^[ */ z_undefinedkey,
    /* M-^\ */ z_undefinedkey,
    /* M-^] */ z_undefinedkey,
    /* M-^^ */ z_undefinedkey,
    /* M-^_ */ z_copyprevword,
    /* M-  */ z_expandhistory,
    /* M-! */ z_expandhistory,
    /* M-" */ z_quoteregion,
    /* M-# */ z_undefinedkey,
    /* M-$ */ z_spellword,
    /* M-% */ z_undefinedkey,
    /* M-& */ z_undefinedkey,
    /* M-' */ z_quoteline,
    /* M-( */ z_undefinedkey,
    /* M-) */ z_undefinedkey,
    /* M-* */ z_undefinedkey,
    /* M-+ */ z_undefinedkey,
    /* M-, */ z_undefinedkey,
    /* M-- */ z_negargument,
    /* M-. */ z_insertlastword,
    /* M-/ */ z_undefinedkey,
    /* M-0 */ z_digitargument,
    /* M-1 */ z_digitargument,
    /* M-2 */ z_digitargument,
    /* M-3 */ z_digitargument,
    /* M-4 */ z_digitargument,
    /* M-5 */ z_digitargument,
    /* M-6 */ z_digitargument,
    /* M-7 */ z_digitargument,
    /* M-8 */ z_digitargument,
    /* M-9 */ z_digitargument,
    /* M-: */ z_undefinedkey,
    /* M-; */ z_undefinedkey,
    /* M-< */ z_beginningofbufferorhistory,
    /* M-= */ z_undefinedkey,
    /* M-> */ z_endofbufferorhistory,
    /* M-? */ z_whichcommand,
    /* M-@ */ z_undefinedkey,
    /* M-A */ z_acceptandhold,
    /* M-B */ z_backwardword,
    /* M-C */ z_capitalizeword,
    /* M-D */ z_killword,
    /* M-E */ z_undefinedkey,
    /* M-F */ z_forwardword,
    /* M-G */ z_getline,
    /* M-H */ z_runhelp,
    /* M-I */ z_undefinedkey,
    /* M-J */ z_undefinedkey,
    /* M-K */ z_undefinedkey,
    /* M-L */ z_downcaseword,
    /* M-M */ z_undefinedkey,
    /* M-N */ z_historysearchforward,
    /* M-O */ z_undefinedkey,
    /* M-P */ z_historysearchbackward,
    /* M-Q */ z_pushline,
    /* M-R */ z_undefinedkey,
    /* M-S */ z_spellword,
    /* M-T */ z_transposewords,
    /* M-U */ z_upcaseword,
    /* M-V */ z_undefinedkey,
    /* M-W */ z_copyregionaskill,
    /* M-X */ z_undefinedkey,
    /* M-Y */ z_undefinedkey,
    /* M-Z */ z_undefinedkey,
    /* M-[ */ z_undefinedkey,
    /* M-\ */ z_undefinedkey,
    /* M-] */ z_undefinedkey,
    /* M-^ */ z_undefinedkey,
    /* M-_ */ z_insertlastword,
    /* M-` */ z_undefinedkey,
    /* M-a */ z_acceptandhold,
    /* M-b */ z_backwardword,
    /* M-c */ z_capitalizeword,
    /* M-d */ z_killword,
    /* M-e */ z_undefinedkey,
    /* M-f */ z_forwardword,
    /* M-g */ z_getline,
    /* M-h */ z_runhelp,
    /* M-i */ z_undefinedkey,
    /* M-j */ z_undefinedkey,
    /* M-k */ z_undefinedkey,
    /* M-l */ z_downcaseword,
    /* M-m */ z_undefinedkey,
    /* M-n */ z_historysearchforward,
    /* M-o */ z_undefinedkey,
    /* M-p */ z_historysearchbackward,
    /* M-q */ z_pushline,
    /* M-r */ z_undefinedkey,
    /* M-s */ z_spellword,
    /* M-t */ z_transposewords,
    /* M-u */ z_upcaseword,
    /* M-v */ z_undefinedkey,
    /* M-w */ z_copyregionaskill,
    /* M-x */ z_executenamedcmd,
    /* M-y */ z_yankpop,
    /* M-z */ z_executelastnamedcmd,
    /* M-{ */ z_undefinedkey,
    /* M-| */ z_vigotocolumn,
    /* M-} */ z_undefinedkey,
    /* M-~ */ z_undefinedkey,
    /* M-^? */ z_backwardkillword,
};

/**/
__thread int viinsbind[32] = {
    /* ^@ */ z_undefinedkey,
    /* ^A */ z_selfinsert,
    /* ^B */ z_selfinsert,
    /* ^C */ z_selfinsert,
    /* ^D */ z_listchoices,
    /* ^E */ z_selfinsert,
    /* ^F */ z_selfinsert,
    /* ^G */ z_listexpand,
    /* ^H */ z_vibackwarddeletechar,
    /* ^I */ z_expandorcomplete,
    /* ^J */ z_acceptline,
    /* ^K */ z_selfinsert,
    /* ^L */ z_clearscreen,
    /* ^M */ z_acceptline,
    /* ^N */ z_selfinsert,
    /* ^O */ z_selfinsert,
    /* ^P */ z_selfinsert,
    /* ^Q */ z_viquotedinsert,
    /* ^R */ z_redisplay,
    /* ^S */ z_selfinsert,
    /* ^T */ z_selfinsert,
    /* ^U */ z_vikillline,
    /* ^V */ z_viquotedinsert,
    /* ^W */ z_vibackwardkillword,
    /* ^X */ z_undefinedkey,
    /* ^Y */ z_selfinsert,
    /* ^Z */ z_selfinsert,
    /* ^[ */ z_vicmdmode,
    /* ^\ */ z_selfinsert,
    /* ^] */ z_selfinsert,
    /* ^^ */ z_selfinsert,
    /* ^_ */ z_selfinsert,
};

/**/
__thread int vicmdbind[128] = {
    /* ^@ */ z_undefinedkey,
    /* ^A */ z_undefinedkey,
    /* ^B */ z_undefinedkey,
    /* ^C */ z_undefinedkey,
    /* ^D */ z_listchoices,
    /* ^E */ z_undefinedkey,
    /* ^F */ z_undefinedkey,
    /* ^G */ z_listexpand,
    /* ^H */ z_vibackwardchar,
    /* ^I */ z_undefinedkey,
    /* ^J */ z_acceptline,
    /* ^K */ z_undefinedkey,
    /* ^L */ z_clearscreen,
    /* ^M */ z_acceptline,
    /* ^N */ z_downhistory,
    /* ^O */ z_undefinedkey,
    /* ^P */ z_uphistory,
    /* ^Q */ z_undefinedkey,
    /* ^R */ z_redo,
    /* ^S */ z_undefinedkey,
    /* ^T */ z_undefinedkey,
    /* ^U */ z_undefinedkey,
    /* ^V */ z_undefinedkey,
    /* ^W */ z_undefinedkey,
    /* ^X */ z_undefinedkey,
    /* ^Y */ z_undefinedkey,
    /* ^Z */ z_undefinedkey,
    /* ^[ */ z_beep,
    /* ^\ */ z_undefinedkey,
    /* ^] */ z_undefinedkey,
    /* ^^ */ z_undefinedkey,
    /* ^_ */ z_undefinedkey,
    /*   */ z_viforwardchar,
    /* ! */ z_undefinedkey,
    /* " */ z_visetbuffer,
    /* # */ z_poundinsert,
    /* $ */ z_viendofline,
    /* % */ z_vimatchbracket,
    /* & */ z_undefinedkey,
    /* ' */ z_vigotomarkline,
    /* ( */ z_undefinedkey,
    /* ) */ z_undefinedkey,
    /* * */ z_undefinedkey,
    /* + */ z_vidownlineorhistory,
    /* , */ z_virevrepeatfind,
    /* - */ z_viuplineorhistory,
    /* . */ z_virepeatchange,
    /* / */ z_vihistorysearchbackward,
    /* 0 */ z_vidigitorbeginningofline,
    /* 1 */ z_digitargument,
    /* 2 */ z_digitargument,
    /* 3 */ z_digitargument,
    /* 4 */ z_digitargument,
    /* 5 */ z_digitargument,
    /* 6 */ z_digitargument,
    /* 7 */ z_digitargument,
    /* 8 */ z_digitargument,
    /* 9 */ z_digitargument,
    /* : */ z_executenamedcmd,
    /* ; */ z_virepeatfind,
    /* < */ z_viunindent,
    /* = */ z_listchoices,
    /* > */ z_viindent,
    /* ? */ z_vihistorysearchforward,
    /* @ */ z_undefinedkey,
    /* A */ z_viaddeol,
    /* B */ z_vibackwardblankword,
    /* C */ z_vichangeeol,
    /* D */ z_vikilleol,
    /* E */ z_viforwardblankwordend,
    /* F */ z_vifindprevchar,
    /* G */ z_vifetchhistory,
    /* H */ z_undefinedkey,
    /* I */ z_viinsertbol,
    /* J */ z_vijoin,
    /* K */ z_undefinedkey,
    /* L */ z_undefinedkey,
    /* M */ z_undefinedkey,
    /* N */ z_virevrepeatsearch,
    /* O */ z_viopenlineabove,
    /* P */ z_viputbefore,
    /* Q */ z_undefinedkey,
    /* R */ z_vireplace,
    /* S */ z_vichangewholeline,
    /* T */ z_vifindprevcharskip,
    /* U */ z_undefinedkey,
    /* V */ z_visuallinemode,
    /* W */ z_viforwardblankword,
    /* X */ z_vibackwarddeletechar,
    /* Y */ z_viyankwholeline,
    /* Z */ z_undefinedkey,
    /* [ */ z_undefinedkey,
    /* \ */ z_undefinedkey,
    /* ] */ z_undefinedkey,
    /* ^ */ z_vifirstnonblank,
    /* _ */ z_vifirstnonblank,
    /* ` */ z_vigotomark,
    /* a */ z_viaddnext,
    /* b */ z_vibackwardword,
    /* c */ z_vichange,
    /* d */ z_videlete,
    /* e */ z_viforwardwordend,
    /* f */ z_vifindnextchar,
    /* g */ z_undefinedkey,
    /* h */ z_vibackwardchar,
    /* i */ z_viinsert,
    /* j */ z_downlineorhistory,
    /* k */ z_uplineorhistory,
    /* l */ z_viforwardchar,
    /* m */ z_visetmark,
    /* n */ z_virepeatsearch,
    /* o */ z_viopenlinebelow,
    /* p */ z_viputafter,
    /* q */ z_undefinedkey,
    /* r */ z_vireplacechars,
    /* s */ z_visubstitute,
    /* t */ z_vifindnextcharskip,
    /* u */ z_undo,
    /* v */ z_visualmode,
    /* w */ z_viforwardword,
    /* x */ z_videletechar,
    /* y */ z_viyank,
    /* z */ z_undefinedkey,
    /* { */ z_undefinedkey,
    /* | */ z_vigotocolumn,
    /* } */ z_undefinedkey,
    /* ~ */ z_viswapcase,
    /* ^? */ z_vibackwardchar,
};
