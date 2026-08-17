/*
 * aok_fork.h -- fork by re-launch, for zsh as a native program in iSH-AOK.
 *
 * Hand-written rather than generated: zsh's makepro.awk is not run by AOK's
 * meson build (see the zsh block in meson.build), so there is no aok_fork.pro
 * and the declarations live here.
 */

#ifndef AOK_FORK_H
#define AOK_FORK_H

/* How much of the parent a particular site's child is meant to inherit.
 * The interesting one is KEEPTRAP: zsh decides per site, unlike bash. See
 * aok_emit_traps. */
#define AOK_SUB_KEEPTRAP 0x01	/* entersubsh's ESUB_KEEPTRAP */
#define AOK_SUB_ASYNC    0x02	/* entersubsh's ESUB_ASYNC */

#define AOK_MAX_CLOSE 32

/* Everything the forked child would have done to itself between fork and
 * running, described in advance because a spawn has no such moment. */
struct aok_spawn {
    int in_fd;			/* dup2 onto 0, or -1 */
    int out_fd;			/* dup2 onto 1, or -1 */
    int close_fds[AOK_MAX_CLOSE];
    int nclose;
    int pgid;			/* -1 inherit, 0 lead a new group, >0 join */
    int flags;			/* AOK_SUB_* */
    int stdin_null;		/* replace fd 0 with /dev/null */
    /* The command text handed to the child is itself a `( ... )`, so the child
     * counts this subshell entry for ZSH_SUBSHELL while re-parsing it and the
     * spawn must not count it a second time. Only the execpline site can know
     * this, because only it is handed the construct's own type. */
    int subsh_counted;
};

void aok_spawn_init(struct aok_spawn *sp);
void aok_spawn_close(struct aok_spawn *sp, int fd);
pid_t aok_spawn_subshell(char *cmdtext, struct aok_spawn *sp);
char *aok_quote_words(LinkList args);

/* Child side, called from init_misc before the -c string is parsed. */
void aok_child_init(void);

/* Set by aok_child_init so source() reads an already-open descriptor rather
 * than a path; consumed by the first source() that sees it. */
extern __thread int aok_source_fd;

/* Traps this shell was handed by its parent, which must not make it fork for
 * the command it was launched to run -- see the comment in aok_fork.c. */
extern __thread int aok_inherited_ntraps;

#endif /* AOK_FORK_H */
