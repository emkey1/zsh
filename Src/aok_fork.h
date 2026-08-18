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
/* The text handed over is ONE COMMAND that the parent's execlist has already
 * run its per-sublist machinery for -- the DEBUG trap before it, ZERR and
 * errexit after it. Only execcmd_fork's site is like this; the command- and
 * process-substitution sites hand over a list whose child really does run
 * execlist. See the aok_incmd comment in execlist. */
#define AOK_SUB_INCMD    0x04
/* This site's child is one whose execlist runs with `exiting` set, so it fires
 * its EXIT trap on the way out -- upstream's execode(prog, 0, 1, "cmdsubst")
 * and its three siblings. Only the command- and process-substitution sites are
 * like that; `( )`, a pipeline element and a background job hand their child a
 * DEAD exit trap instead. See aok_emit_exit_trap. */
#define AOK_SUB_EXITTRAP 0x08

/* The bits of AOK_ZSH_INHERIT's fourth field: facts about the CHILD, decided
 * by the site that spawns it, rather than state a fork would have copied. */
#define AOK_CHILD_INCMD      0x01	/* AOK_SUB_INCMD, as the child sees it */
#define AOK_CHILD_EXIT_INERT 0x02	/* define TRAPEXIT, do not arm it */

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

/* True when this thread's C stack is nearly spent, so that a recursion about
 * to go one level deeper refuses instead. Overrunning the stack of a native
 * program kills the app rather than the shell -- see the long comment in
 * aok_fork.c for why zsh's own FUNCNEST cannot cover this. */
int aok_stack_exhausted(void);

/* Records the zmodload autoload registrations this build is BORN with, so that
 * a fork can emit only the ones the user added. Called from zsh_main once the
 * compiled-in modules have registered and before any user code runs -- the
 * child takes the same snapshot at the same point, which is what makes the
 * difference meaningful. */
void aok_snapshot_autoloads(void);

/* Set by aok_child_init so source() reads an already-open descriptor rather
 * than a path; consumed by the first source() that sees it. */
extern __thread int aok_source_fd;

/* Traps this shell was handed by its parent, which must not make it fork for
 * the command it was launched to run -- see the comment in aok_fork.c. */
extern __thread int aok_inherited_ntraps;

/* AOK_SUB_INCMD, as the child sees it: set by aok_child_init and consumed by
 * the first sublist execlist runs afterwards. */
extern __thread int aok_relaunch_incmd;

/* AOK: the REDIRECTIONS of a command, as source text, on the heap.
 *
 * Lives in text.c beside getredirs(), whose buffer machinery it borrows, but
 * deliberately without zsh's `/**\/` marker so that makepro.awk's text.epro
 * stays exactly what upstream would generate. It exists so that the re-launch
 * of an external command can hand its child the EXPANDED words plus the
 * command's own redirections, instead of falling back to source text and
 * running every expansion a second time. See the second execcmd_fork site.
 *
 * Returns NULL if any redirection in the list is one getredirs() cannot render
 * on its own -- a here-document, whose body is emitted later by gettext2, or a
 * process substitution, whose name is not the text it was written as. */
char *aok_redir_text(LinkList redirs);

/* AOK: the assignment prefix of a command (`VAR=x cmd`), as source text, on
 * the heap; NULL when there is none. aok_redir_text's twin -- see the note in
 * text.c -- and used by the same call site for the same reason. */
char *aok_assign_text(Wordcode varspc, Eprog prog);

/* The $RANDOM stream, as a seed and a draw count, so that a re-launched child
 * can rebuild libc's generator state -- which a real fork copies and this one
 * cannot see. Maintained by init.c (the startup seed) and by randomsetfn and
 * randomgetfn in params.c; replayed by aok_child_init. See aok_fork.c. */
extern __thread unsigned int aok_random_seed;
extern __thread zlong aok_random_draws;

/* AOK: the keymap difference, from Src/Zle/zle_keymap.c. Deliberately without
 * zsh's `/**\/` marker so that makepro.awk's zle_keymap.pro stays exactly what
 * upstream would generate -- the same arrangement text.c uses for
 * aok_redir_text. aok_emit_keymaps writes nothing at all when zle was never
 * loaded, which is every shell running a script. */
int aok_have_keymaps(void);
void aok_snapshot_keymaps(void);
void aok_forget_keymaps(void);
void aok_emit_keymaps(FILE *out);

/* AOK: source a global rc file from wherever the guest rootfs actually keeps it
 * -- /etc/zsh/NAME when that exists, the compiled-in GLOBAL_* path otherwise.
 * Used by init.c for zshenv/zprofile/zshrc/zlogin and by builtin.c for zlogout.
 * See the block comment on the definition in aok_fork.c. */
void aok_source_global(const char *flat);

#endif /* AOK_FORK_H */
