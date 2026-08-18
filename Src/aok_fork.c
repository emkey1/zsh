/*
 * aok_fork.c -- fork by re-launch, for zsh compiled as a native program in
 * iSH-AOK.
 *
 * WHY THIS EXISTS
 *
 * A native program in AOK is a C function running on a guest task's thread
 * inside the app's ONE address space, not a process. fork() needs two threads
 * of execution to see DIFFERENT memory at the SAME addresses, which is exactly
 * what a process boundary provides and a thread boundary cannot -- so
 * nlibc_fork refuses with ENOSYS and every one of zsh's zfork() sites failed.
 * That is not a corner: `echo $(date)`, `a | b`, `( cd /tmp )`, `cmd &` and
 * `<(cmd)` all go through it, and so does every external command that is not
 * the last thing the shell will ever do.
 *
 * The replacement, proven first on bash (deps/bash/aok_fork.c and
 * docs/bash_native_plan.md), is to make the child a separate guest TASK: spawn
 * a fresh native zsh, hand it this shell's state, and have it run the command.
 * A subshell is one-way by definition -- everything it does to its own state is
 * MEANT to be discarded -- so "state in, status and output out" is the actual
 * contract rather than an approximation of fork.
 *
 * WHAT MADE IT POSSIBLE HERE
 *
 * The child is a second live zsh in the same address space as its parent, so
 * this could not work at all until zsh's globals became __thread. That is
 * tools/bash-tls-fix-statics.py, tools/bash-tls-fix-externs.py and
 * tools/zsh-tls-fix-tables.py, gated by tools/check-bash-tls.py. Before it,
 * kernel/zsh_glue.c allowed exactly one zsh per app session and the second one
 * lost `print` as a builtin.
 *
 * HOW THE STATE TRAVELS, AND WHY NOT IN ARGV
 *
 * bash puts the state in the child's `-c` string. That cannot work for zsh:
 *
 *     % zsh -f -c 'alias q=echo
 *     q hi'
 *     zsh: command not found: q
 *
 * zsh parses a -c STRING in full before executing any of it, and alias
 * expansion happens at PARSE time -- so an alias or function defined by the
 * state is invisible to the command that follows it in the same string. It has
 * to be SOURCED, which parses incrementally, and sourced before the -c string
 * is parsed at all. So the state goes down a pipe the child inherits, the child
 * is told the descriptor in its environment, and init_misc sources it before
 * execstring() ever sees the command.
 *
 * A pipe rather than a temp file: nothing to clean up, no dependency on a
 * writable $TMPPREFIX, and no window in which a path exists. The parent writes
 * AFTER the spawn, so a state larger than the pipe buffer simply blocks until
 * the child -- which reads the whole thing during startup, before running
 * anything -- has drained it.
 *
 * WHAT IS NOT HERE
 *
 * exec.c:1924, execpline's fork to continue a stopped pipeline in a subshell,
 * is deliberately left refusing. Its child does not discard its state, it
 * CARRIES ON with it, in the middle of a half-executed loop with a live C
 * stack. That is the one thing a re-launch cannot express. zsh already
 * degrades gracefully there ("zsh: job can't be suspended") and the cost is one
 * interactive nicety.
 */

#include "zsh.mdh"
#include "aok_fork.h"

#include <errno.h>
#include <math.h>		/* isfinite, for the float emitter */
#include <pthread.h>		/* the stack bounds, see aok_stack_bounds */
#include <signal.h>
#include <unistd.h>

/* The re-launched child. Synthesized by the kernel (kernel/native.c), so it
 * exists in every build that compiles this file. There is deliberately no
 * fallback to the guest's own /usr/bin/zsh: an emulated child would not
 * understand AOK_ZSH_STATE_FD and would silently run the command with none of
 * the parent's state, which is worse than an error. */
#define AOK_ZSH_PATH "/AOK/native/zsh"

/* Handed to the child in its environment, and unset by it at startup so that
 * an ordinary program it runs does not inherit them.
 *
 *   AOK_ZSH_STATE_FD  the descriptor the state script arrives on
 *   AOK_ZSH_DOLLAR    `$$`, as `<pid>/<spawner pid>` -- see aok_child_init for
 *                     why the second half is there
 *   AOK_ZSH_LASTVAL   `$?`, which cannot be set by a shell command; see below
 *   AOK_ZSH_INHERIT   the rest of the readonly state a fork would have copied:
 *                     `$!`, `$PPID`, `$ZSH_SUBSHELL` and `$pipestatus`
 *   AOK_ZSH_HISTFILE  a temp file holding the parent's in-memory history, read
 *                     and then unlinked by the child -- see aok_write_hist_file
 */
#define AOK_VAR_FD       "AOK_ZSH_STATE_FD"
#define AOK_VAR_DOLLAR   "AOK_ZSH_DOLLAR"
#define AOK_VAR_LASTVAL  "AOK_ZSH_LASTVAL"
#define AOK_VAR_INHERIT  "AOK_ZSH_INHERIT"
#define AOK_VAR_HISTFILE "AOK_ZSH_HISTFILE"

/* The last line the state emits. The child checks for it and complains on the
 * REAL stderr if it is missing.
 *
 * This is not belt-and-braces. A single bad line aborts the rest of a sourced
 * file and sets $? to 126, silently -- and the state is sourced with stderr on
 * /dev/null, because that is what keeps a subshell cheap. So a state that is
 * 95% applied looks exactly like one that is 100% applied until some specific
 * thing misbehaves much later. The sentinel turns that into one line naming the
 * cause, and AOK_ZSH_DUMP_STATE=1 prints what the child was handed. */
#define AOK_VAR_OK "AOK_ZSH_STATE_OK"

/* ---------------------------------------------------------------- the stack
 *
 * THE C STACK IS A RESOURCE OF THE APP HERE, AND RUNNING OFF THE END OF IT
 * KILLS THE APP RATHER THAN THE SHELL.
 *
 * A native program is a C function on a guest task's thread inside the app's
 * one address space -- that is the premise this whole file is built on -- so
 * zsh's recursion runs on a thread of iSH-AOK itself. Off-device, a shell that
 * recurses past its stack takes a SIGSEGV and one process dies. Real zsh 5.9
 * does exactly that, and is not being emulated wrongly here:
 *
 *     % zsh -f -c 'FUNCNEST=20000; r() { r; }; r'         killed, exit 139
 *
 * Here the same overrun is a SIGBUS on a thread the app cannot lose, so it
 * takes every other shell, job and terminal down with it (host exit 138). This
 * guard therefore exists because the BLAST RADIUS differs, not because zsh's
 * behaviour does; it is a divergence chosen on purpose, in the direction of
 * the shell reporting an error where real zsh would have died.
 *
 * WHY A DEPTH LIMIT IS NOT ENOUGH, AND WHY THE 4 MB STACK IS NOT EITHER
 *
 * zsh already has a depth limit, FUNCNEST, and kernel/task.c already sizes the
 * task thread's stack so that FUNCNEST's default of 500 trips first. But
 * FUNCNEST counts CALLS while the stack spends BYTES, and the bytes per call
 * are not a constant. Measured on this build against the 4 MB stack:
 *
 *     r() { r; }               ~3.4 KB/level, died between 1200 and 1300
 *     r() { eval r; }          ~6.3 KB/level, died between  600 and  700
 *
 * so a depth picked to be safe for one shape is unsafe for the next. Worse,
 * FUNCNEST is user-settable and zsh's own message when it trips -- "increase
 * FUNCNEST?" -- invites the user to raise it, i.e. the documented remedy for
 * the depth guard is to walk over the cliff. `FUNCNEST=5000` is all it takes.
 * The only quantity that answers the question is the stack that is actually
 * left, so that is what is measured.
 *
 * WHERE THE BOUNDS COME FROM
 *
 * From pthread, not from arithmetic on a frame address: Darwin's
 * pthread_get_stackaddr_np gives the stack's high end and
 * pthread_get_stacksize_np its size, so the low end is exact and is not an
 * assumption about how this thread was created -- a guest task thread, the
 * main thread, and any future host thread that ends up running a native
 * program all answer for themselves. Only "where am I now" comes from
 * __builtin_frame_address(0), which is the one part that has to.
 *
 * A thread's stack never moves, so the bounds are worked out once per thread
 * and what a recursing shell pays afterwards is a load and a compare.
 *
 * THE RESERVE
 *
 * What must still fit below the deepest call this refuses: the rest of the
 * caller's own frame, zerr's formatting, and the unwind back out. 256 KB is
 * some forty levels of the most expensive shape measured above, so it also
 * absorbs one level that costs far more than any of them. Its price is 6% of
 * the usable depth: plain function recursion is refused at ~1150 instead of
 * dying at ~1250, which is still more than twice FUNCNEST's default. That
 * margin is the point -- an ordinary script that recurses too far keeps
 * getting zsh's own message from zsh's own guard, exactly as it does
 * off-device, and this one is reached only by a script that has disabled that.
 */
#define AOK_STACK_RESERVE (256 * 1024)

/* Lowest frame address that is still allowed to recurse. Worked out on first
 * use; aok_stack_state is 0 before that, 1 when the bounds are known and -1
 * when the platform could not answer -- in which case nothing is refused,
 * because a guess here would break working scripts to prevent a crash that
 * might not be coming. */
static __thread uintptr_t aok_stack_floor;
static __thread int aok_stack_state;

static void
aok_stack_bounds(void)
{
#if defined(__APPLE__)
    pthread_t self = pthread_self();
    void *high = pthread_get_stackaddr_np(self);
    size_t size = pthread_get_stacksize_np(self);

    if (high != NULL && size > (size_t) (2 * AOK_STACK_RESERVE)) {
	aok_stack_floor = (uintptr_t) high - size + AOK_STACK_RESERVE;
	aok_stack_state = 1;
	return;
    }
#endif
    aok_stack_state = -1;
}

/* True when there is not enough stack left below this point to survive another
 * level of whatever the caller was about to do. */

/**/
int
aok_stack_exhausted(void)
{
    if (aok_stack_state == 0)
	aok_stack_bounds();
    if (aok_stack_state != 1)
	return 0;
    return (uintptr_t) __builtin_frame_address(0) <= aok_stack_floor;
}

/* Set by aok_child_init, consumed by source() in init.c. */
/**/
__thread int aok_source_fd = -1;

/* How many traps this shell was HANDED, as opposed to set for itself.
 *
 * The late fork site (exec.c) forks when `nsigtrapped` is non-zero, because a
 * shell with traps must survive the command in order to run them. A forked
 * child does that once: it is already forked, so it execs in place. A
 * RE-LAUNCHED child does not know that -- it starts up, the state gives it the
 * parent's traps, it evaluates the same guard and re-launches again. That is an
 * infinite regress, and it is what `trap "print X" INT; /bin/true` did: a tower
 * of shells, none of which ever ran /bin/true.
 *
 * Traps that came in with the state therefore do not count towards the
 * decision, and traps the child sets FOR ITSELF still do -- so
 * `( trap "print bye" EXIT; /bin/true )` still forks and still prints bye. */
/**/
__thread int aok_inherited_ntraps = 0;

/* Whether the command this shell was launched to run is one the SPAWNER's
 * execlist has already fired the DEBUG trap for and will check ZERR and
 * errexit against. Set from the state's AOK_ZSH_INHERIT, consumed by the first
 * sublist execlist runs -- see the aok_incmd comment in exec.c. */
/**/
__thread int aok_relaunch_incmd = 0;

/* Where the $RANDOM sequence has got to.
 *
 * $RANDOM is libc's rand(), so its state lives in libc and a fork copies it
 * for free. A re-launched child gets a fresh libc, which is why a parent that
 * had done `RANDOM=42` for a reproducible run handed every subshell a
 * different stream: `RANDOM=42; print $RANDOM $RANDOM` and `RANDOM=42; print
 * $(print $RANDOM $RANDOM)` disagreed here and agree on a real fork.
 *
 * There is no portable way to READ libc's generator state, so this records
 * what it would take to rebuild it: the last seed and how many numbers have
 * been drawn since. randomsetfn and randomgetfn in params.c keep the pair up
 * to date, init.c records the startup seed, and aok_child_init replays the
 * pair with srand() and that many rand() calls -- which is exact, because
 * srand+rand is deterministic. The replay costs one rand() per draw the parent
 * ever made; rand() is a handful of nanoseconds, so a shell that has read
 * $RANDOM a million times pays about five milliseconds per subshell and every
 * realistic shell pays nothing measurable.
 *
 * Replaying in C rather than emitting `RANDOM=42` plus N reads into the state
 * is not an optimisation: a `$RANDOM` in shell text is a command, and N of
 * them would be N commands for the child to parse and run. */
/**/
__thread unsigned int aok_random_seed = 0;
/**/
__thread zlong aok_random_draws = 0;

/* ------------------------------------------------------------ the state script
 *
 * Almost all of it is produced by running zsh code in the parent with fd 1
 * pointed at the state pipe, so the quoting is zsh's own rather than a second
 * implementation of it. The alternative -- rebuilding `typeset -p`, `functions`
 * and `alias -L` in C -- is a lot of code whose only job is to agree with zsh,
 * and it would have to go on agreeing.
 *
 * It is one anonymous function so that `emulate -L zsh` applies: a parent
 * sitting in `setopt ksharrays shwordsplit` would otherwise change what every
 * line below means. An anonymous function also leaves no trace in shfunctab,
 * which a named one would -- and which would then be serialised into the state
 * it is producing.
 *
 * THE ORDERING IS LOAD-BEARING. Each rule below was found by breaking it.
 *
 * (1) `setopt no_aliases` is the FIRST thing the child does, and the matching
 *     restore is the LAST. zsh expands aliases in non-interactive shells,
 *     unlike bash, and alias expansion happens while the state is being PARSED.
 *     With `alias echo='print BROKEN'` in the parent, a replayed function body
 *     containing `echo` becomes permanently `print BROKEN`; with a global alias
 *     `alias -g VALUE='ha ha'`, `arr=( VALUE b )` replays as `arr=(ha ha b)`;
 *     and a function whose NAME is an alias is a hard parse error ("defining
 *     function based on alias"). This is zsh's counterpart of bash's extglob
 *     rule and it is broader.
 *
 * (2) Aliases are defined AFTER functions, for the same reason -- a function
 *     body must be parsed before the aliases exist, not after.
 *
 * (3) Modules before parameters. `${+parameters[mapfile]}` is 0 until
 *     `zmodload zsh/mapfile`, and only a fixed handful of modules autoload on
 *     demand; strftime, sysopen and pcre_compile do not.
 *
 * (4) Options LAST, after every line the child has to parse. This rule used to
 *     say the opposite -- options before anything that uses a pattern, because
 *     a function containing `@(foo|bar)` replayed without `kshglob` sources
 *     cleanly, returns 0 and matches nothing. That reasoning was wrong about
 *     WHEN a pattern is compiled, and the mistake was expensive.
 *
 *     zsh compiles a pattern at first USE and caches it in the parse tree, not
 *     at parse time. `f() { case foo in (@(foo|bar)) ... }; setopt kshglob; f`
 *     matches -- in zsh 5.9 and in this shell alike -- so a function body that
 *     arrives before its options still gets its patterns compiled after them,
 *     because the child runs nothing until the whole state is in.
 *
 *     Meanwhile an option set before the text is an option applied to the
 *     LEXER, and the text is not written for it. A parent in `emulate sh` has
 *     SH_GLOB, where `(` is not a pattern character, so a body containing
 *     `[[ ab == (a)b ]]` or a `*(.)` glob qualifier was a parse error: source
 *     abandoned the rest of the file and every function, alias, zstyle, hash
 *     and readonly parameter after it went silently missing, along with the
 *     epilogue that sets errexit and nounset. RC_QUOTES is the same story one
 *     layer down -- with it on, the `'\''` that zsh's own printers write for an
 *     embedded quote stops meaning what they wrote it to mean.
 *
 *     So the ordering is not a compromise between two risks. Every line of the
 *     state is PRODUCED by zsh's printers under `emulate -L zsh`, and zsh's own
 *     defaults -- exactly what a `zsh -f` child has until the options block
 *     runs -- are the reading of it that round-trips. See aok_emit_options.
 *
 *     `setopt no_aliases` is the one exception and it is rule (1)'s, not this
 *     one's: it does not change what a word MEANS, it stops a word being
 *     replaced by another one.
 *
 * (5) Readonly parameters LAST, after every other assignment. A read-only
 *     variable assigned twice is a runtime error, and a runtime error aborts
 *     the REST of the sourced file.
 *
 *     Last is necessary but not sufficient: each one also has to be GUARDED,
 *     because a module can own a readonly parameter and the child has already
 *     loaded that module by rule (3). `zmodload zsh/zftp` creates ZFTP_SESSION
 *     readonly, so the child hit `typeset -g -r ZFTP_SESSION=default` for a
 *     parameter it already had, the source aborted at that line, and every
 *     subshell of a zftp-using shell failed with "subshell state did not
 *     finish". The guard is emitted by concatenation -- `print -rn` for the
 *     test, then zsh's own `typeset -p` for the declaration -- rather than by
 *     rewriting typeset's output, so the declaration stays exactly what zsh
 *     would have written. Note it CANNOT be built with `$(typeset -p ...)`:
 *     command substitution in this shell is a fork, which is the thing this
 *     file exists to provide.
 *
 * (6) errexit and nounset last of all (emitted from C below), after the
 *     variables they would otherwise fire on.
 *
 * (7) `$?` is NOT set by a shell command. `(exit N)` is a subshell -- which is
 *     a fork, which is the thing this file exists to provide -- and under the
 *     `errexit` the state faithfully replays, any shell-level way of producing
 *     a non-zero status kills the child. It comes in through the environment
 *     and is assigned to `lastval` in C.
 *
 * (8) Hidden parameters have to be UNhidden before they can be serialised, and
 *     hidden again afterwards. `typeset -H` means "do not show my value in a
 *     typeset listing", and `typeset -p` honours it to the letter: a parameter
 *     declared `typeset -HA h` with h[k]=v prints as `typeset -A h` -- no
 *     value, and not even the -H. Both halves are lost, silently. This is not
 *     an exotic corner: compinit declares its tables with `typeset -gHA`, so
 *     every completion table a user has loaded empties on the way across.
 *     The `-g` is load-bearing. A plain `typeset +H h` inside this anonymous
 *     function does not expose the global, it declares a LOCAL that shadows it
 *     -- and the global then reads back as `typeset h=''`, i.e. the serialiser
 *     would destroy the very parameter it was trying to copy.
 *
 * (9) Functions are emitted as `function 'name' { BODY }`, built by hand from
 *     $functions, rather than by letting `functions --` print them. Two
 *     separate reasons, both about the NAME, which is the user's to choose:
 *
 *     `functions` writes the definition in the `name () { ... }` form, and that
 *     form cannot express a name the parser reads as a reserved word. `function
 *     'do' { echo D }` round-trips through a real fork perfectly well, but
 *     replayed as `do () {` it is a syntax error, the source abandons the rest
 *     of the file, and every function emitted after it -- hash order, so an
 *     arbitrary set -- is silently gone. `time () {` is worse: this shell reads
 *     it as the `time` reserved word applied to an anonymous function, and
 *     replaying it inside the sourced state SEGFAULTS the child, which being a
 *     thread of the app kills the whole app. The `function` keyword puts the
 *     parser in a state where the next word is a name and nothing else, so
 *     every name survives; the quoting is `(qq)` because it always quotes,
 *     where `(q)` leaves `do` bare.
 *
 *     And $functions reads through shfunctab's getnode2, where `functions --`
 *     reads through getnode -- which matters because aok_run_state_script
 *     blanks getnode for the words this script uses as commands. See the
 *     comment there. A user function named `print` therefore still crosses
 *     intact while being refused as a command inside the serialiser.
 *
 *     The braces are added by hand except for a function with an attached
 *     redirection (`f() { echo } >/dev/null`), which $functions already hands
 *     back as `{ ... } >/dev/null` -- braces included, so that the redirection
 *     has something to attach to. That is what the leading-`{` test is for: a
 *     body from any other function starts with the tab that the printer's
 *     indent puts there, so the two shapes cannot be confused, and adding a
 *     second pair of braces would move the redirection off the function and
 *     onto an inner group.
 *
 *     What this form does not carry, and `functions --` did not carry either:
 *     the `# traced` marker, which is a comment on both sides.
 *
 * (10) SPECIAL parameters are selected by NAME, not by their `special` bit.
 *
 *     The loop used to skip every parameter whose $parameters attributes
 *     contained "special", which reads like a safety rule and is not one. The
 *     question a serialiser has to answer is "would a real forked child have
 *     had this", and the answer for `special` is usually yes: IFS is special,
 *     so every re-launched subshell and every command substitution split words
 *     on the DEFAULT IFS -- silently, and pervasively enough that `IFS=:; (
 *     set -- ${=line} )` counted one field where a fork counts three. cdpath,
 *     fpath, HISTSIZE, SAVEHIST, WORDCHARS, the prompts and psvar went the
 *     same way, and SHLVL came out one HIGHER than the parent's because the
 *     child is a genuinely new shell that increments it at startup.
 *
 *     It is an allow-list rather than a deny-list because the two failure
 *     directions are not comparable. A name missing from an allow-list is a
 *     parameter that does not cross -- the bug we already had, for one name.
 *     A name missing from a deny-list is a line the child EXECUTES: `mapfile`
 *     would write files, `commands` and `functions` are zsh/parameter's views
 *     onto tables this state already carries by other means, and `UID=1000`
 *     from a non-root shell is an error that aborts the rest of the sourced
 *     state and takes every later parameter, function and alias with it. So
 *     the list holds the specials that are plain settable storage describing
 *     how this shell behaves, and leaves out four groups on purpose:
 *
 *       - readonly ones (`$?`, `$!`, `$$`, PPID, ZSH_SUBSHELL, status,
 *         LINENO, HISTCMD, `*`, `@`): no shell command can assign them. The
 *         ones a subshell really does inherit travel in the environment
 *         instead -- see AOK_VAR_INHERIT below.
 *       - identity (UID, EUID, GID, EGID, USERNAME): assigning them attempts a
 *         setuid, and fails.
 *       - what the child measures for itself (RANDOM, SECONDS, ERRNO, TTYIDLE,
 *         TERM and the terminfo pair).
 *       - the module tables, which are `hide hideval` associations owned by
 *         zsh/parameter and friends.
 *
 *     `0` is left out because it reaches the child as argv[5] of the spawn,
 *     and `typeset -g 0=...` is not even valid syntax.
 *
 *     Where several names are one storage, only one of them is listed --
 *     PS1..PS4 rather than the PROMPT, PROMPT2..4 and prompt spellings of the
 *     same four C variables, RPS1/RPS2 rather than RPROMPT/RPROMPT2, histchars
 *     rather than HISTCHARS, and the array half of each tie (path, cdpath,
 *     fpath, manpath, mailpath, psvar, fignore, module_path) rather than both
 *     halves. The name kept is the one that exists in every emulation; the
 *     others would each have added a line to every subshell to assign a value
 *     that had just been assigned. TRY_BLOCK_ERROR and
 *     TRY_BLOCK_INTERRUPT are dropped for a different reason: they describe a
 *     `try` block in progress, and continuing one is precisely what a
 *     re-launched child cannot do.
 *
 * (11) `${(@q-)dirstack}`, with the (@) that the argv line beside it gets from
 *     its `@`. Without it the nested expansion sits in a double-quoted context,
 *     which joins the array into one scalar BEFORE quoting it, so a parent with
 *     /etc and /usr on the stack emitted `dirstack=( '/etc /usr' )` and every
 *     child saw $#dirstack == 1. popd in a subshell then failed with "no such
 *     file or directory: /etc /usr", `cd +2` did not move, and a directory
 *     whose name contains a space was destroyed outright.
 *
 * (12) SERIALISING A SHELL MUST NOT CHANGE IT. This script runs in the PARENT,
 *     which is the whole reason the rules above exist, and it is just as true
 *     of side effects as of quoting. Two lines in here were mutating the shell
 *     they were describing: `zmodload -i zsh/parameter`, which the script needs
 *     to read $functions and $parameters at all, and `zstyle -L`, whose very
 *     name autoloads zsh/zutil -- and zutil pulls in zsh/complete, which pulls
 *     in zsh/zle. So `zmodload -e zsh/zle` answered NO in a fresh shell and YES
 *     after the first command substitution it ever ran, `zmodload -u` could not
 *     keep a module unloaded across any subshell, and every child was told to
 *     load four modules its parent had never asked for. A real fork does all of
 *     that work on the far side of fork() where nobody can see it.
 *
 *     The module list is therefore read BEFORE zsh/parameter is loaded, the
 *     load is undone at the end if this shell did not already have it, and
 *     zstyle is only called when zsh/zutil is loaded already -- which is exact
 *     rather than conservative, because using zstyle is the only way to have a
 *     zstyle and also the thing that loads the module.
 *
 * WHAT IS NOT IN THIS SCRIPT. Four tables have no shell-level printer whose
 * output can be replayed, or are hidden from the script by the parent's own
 * shell context, and they are emitted from C further down: the autoload flags
 * (zsh/parameter renders U and t and cannot render k or z), the command hash
 * table, per-function sticky emulation, and TRAPEXIT -- which starttrapscope()
 * removes from shfunctab for the whole life of the anonymous function above.
 * The history goes its own way again, as a file; see aok_write_hist_file.
 */
static const char aok_state_script[] =
"() {\n"
"  emulate -L zsh\n"
"  setopt no_aliases no_nomatch no_unset\n"
"  local __aok_k __aok_v __aok_a __aok_b\n"
"  local -a __aok_ro __aok_fn __aok_pv __aok_hv __aok_out\n"
"  local -a __aok_sp __aok_pk __aok_dflt\n"
"  local -i __aok_keeppar=0\n"
/* Rule (12): the module list is read BEFORE zsh/parameter is loaded, and
 * zsh/parameter is unloaded again if this shell did not already have it.
 * Serialising a shell must not change it, and this line is where it changed:
 * `zmodload -L` emitted below the load would have told every child to load
 * zsh/parameter too -- a module the parent never asked for, four more lines in
 * every subshell (zsh/zutil pulls in zsh/complete and zsh/zle), and a
 * permanent lie in the parent's own `zmodload -L`. */
"  zmodload -L\n"
"  zmodload -e zsh/parameter && __aok_keeppar=1\n"
"  zmodload -i zsh/parameter 2>/dev/null\n"

/* $functions[k] is not a lookup, it is getpermtext() rendering the whole
 * function body afresh on every reference -- so with compinit loaded and 971
 * functions in the table, saying it three times in this loop would render
 * ~3000 function bodies to serialise 971. It is read once. */
"  for __aok_k in \"${(@k)functions}\"; do\n"
"    __aok_b=$functions[$__aok_k]\n"
/* An autoload stub is skipped here and emitted from C by aok_emit_autoloads.
 * It used to be rebuilt from this text, and the flags never survived: the
 * extraction was `${(M)__aok_v##[A-Za-z]#}`, whose `#` closure operator needs
 * EXTENDED_GLOB, which the `emulate -L zsh` two lines above this loop turns
 * OFF -- so the pattern was read as "a letter followed by a literal #", matched
 * nothing, and every autoload crossed as a bare `autoload -- name`. Losing -U
 * is not cosmetic: -U is exactly what stops alias expansion while the function
 * file is parsed, and compinit registers its 971 functions with `autoload -Uz`.
 *
 * Adding `setopt extended_glob` here would have fixed the extraction and still
 * lost half the answer, because this text is all zsh/parameter can tell us:
 * getfunction() renders a stub as "builtin autoload -X" plus U and t and
 * nothing else, so -k and -z are not in it to be extracted. The shfunc's own
 * flag bits have all four, so the emission moved to C. */
/* Anchored, not a substring search. zsh/parameter renders an autoload stub as
 * exactly "builtin autoload -X" plus its flag letters and nothing before it,
 * while a real body always begins with the tab the printer indents it with (or
 * with `{` when a redirection is attached) -- so the anchor is exact, where the
 * `*...*` this used to be matched any function that merely MENTIONED the
 * string. Such a function crossed the fork replaced by an autoload stub, and
 * every call in every subshell answered "function definition file not found".
 * It is not a contrived body either: this file's own test harness tripped it. */
"    if [[ $__aok_b == 'builtin autoload -X'* ]]; then\n"
"      continue\n"
"    elif [[ $__aok_b == '{'* ]]; then\n"
"      __aok_fn+=( \"function ${(qq)__aok_k} $__aok_b\" )\n"
"    else\n"
"      __aok_fn+=( \"function ${(qq)__aok_k} {\" \"$__aok_b\" '}' )\n"
"    fi\n"
"  done\n"
"  for __aok_k in \"${(@k)parameters}\"; do\n"
"    __aok_a=$parameters[$__aok_k]\n"
"    [[ $__aok_a == *special* ]] && continue\n"
"    case $__aok_k in (__aok_*|AOK_ZSH_*|argv|status) continue ;; esac\n"
"    [[ $__aok_a == *hideval* ]] && __aok_hv+=( $__aok_k )\n"
"    [[ $__aok_a == *float* ]] && continue\n"
"    if [[ $__aok_a == *readonly* ]]; then __aok_ro+=( $__aok_k )\n"
"    else __aok_pv+=( $__aok_k )\n"
"    fi\n"
"  done\n"
/* Rule (10)'s list, and it is worth saying why it is three array expansions
 * rather than a test inside the loop above. Deciding this per parameter, with a
 * `case` of alternations, cost 2.5ms of a 7ms subshell all by itself: the loop
 * runs once per parameter -- 110 of them with zsh/parameter loaded, a thousand
 * after compinit -- and every extra command in its body is paid that many
 * times. Set arithmetic on the names is paid once. `${A:*B}` keeps the names
 * that exist (so nothing has to test, and an emulation without `path` simply
 * contributes nothing rather than an error), and `${A:|B}` is the mirror image
 * used for the unset case below.
 *
 * None of these is readonly, hidden or a float, so they go straight into the
 * plain list rather than through the classification above.
 *
 * Only the array half of each tied pair is named. The two halves are one
 * storage and `typeset -aT CDPATH cdpath=( ... )` sets both, so naming CDPATH
 * as well would have added a line per pair to every subshell to assign what had
 * just been assigned. */
/* OPTIND is deliberately NOT here, and its absence is not an oversight: it is
 * the one special this script CANNOT read. zsh scopes OPTIND per function, so
 * `typeset -p OPTIND` inside the anonymous function wrapping this script
 * reports the function's fresh 1 rather than the shell's value -- which real
 * zsh does too, `zsh -f -c 'OPTIND=7; () { typeset -p OPTIND }'` prints 1 on
 * the host as well. Listing it therefore did worse than nothing: every child
 * was handed `typeset -g -i10 OPTIND=1`, so a parent halfway through a getopts
 * loop did not merely fail to pass OPTIND on, it had the child's own value
 * overwritten with 1. It is emitted from C instead, by aok_emit_specials.
 *
 * TRY_BLOCK_ERROR and TRY_BLOCK_INTERRUPT ARE here. They were dropped on the
 * grounds that continuing a `try` block is what a re-launched child cannot do,
 * which is true of the block and false of the flag: reading $TRY_BLOCK_ERROR
 * from a subshell inside an `always` block is ordinary usage, `{ ((1/0)) }
 * always { print $(print $TRY_BLOCK_ERROR) }` answers 0 on a real fork, and
 * without them the child fell back to the -1 that means "no try block here". */
"  __aok_sp=( IFS WORDCHARS KEYBOARD_HACK histchars SHLVL POSTEDIT HISTSIZE \\\n"
"      SAVEHIST FUNCNEST LINES COLUMNS ZLE_RPROMPT_INDENT OPTARG \\\n"
"      TRY_BLOCK_ERROR TRY_BLOCK_INTERRUPT \\\n"
"      NULLCMD READNULLCMD WATCH watch PS1 PS2 PS3 PS4 RPS1 RPS2 SPROMPT \\\n"
"      path cdpath fpath manpath mailpath psvar fignore module_path )\n"
"  __aok_pk=( \"${(@k)parameters}\" )\n"
"  __aok_pv+=( ${__aok_sp:*__aok_pk} )\n"
/* A special the parent UNSET is not in $parameters at all -- there is nothing
 * to serialise, and the child, being a fresh `zsh -f`, has it back at its
 * default. `unset IFS` is the one that gets written on purpose (POSIX-minded
 * scripts do it): zsh splits identically with IFS unset and IFS at its default,
 * but `${+IFS}` and `${IFS-x}` do not agree, and a fork's child does. The
 * subtraction is against the names a fresh child has SET: for the handful it
 * starts without, an `unset` line would be a no-op paid on every fork. */
"  __aok_dflt=( POSTEDIT ZLE_RPROMPT_INDENT WATCH watch RPS1 RPS2 )\n"
"  __aok_out+=( ${${${__aok_sp:|__aok_pk}:|__aok_dflt}/#/unset -- } )\n"
"  __aok_out+=( \"dirstack=( ${(j: :)${(@q-)dirstack}} )\" )\n"
"  __aok_out+=( \"argv=( ${(j: :)${(q-)@}} )\" )\n"
"  print -rl -- \"${(@)__aok_out}\"\n"
"  (( $#__aok_hv )) && typeset -g +H -- \"${(@)__aok_hv}\" 2>/dev/null\n"
"  (( $#__aok_pv )) && typeset -p -- \"${(@)__aok_pv}\"\n"
"  (( $#__aok_fn )) && print -rl -- \"${(@)__aok_fn}\"\n"
/* The ZLE widget table, which did not cross at all: a re-launched child got a
 * pristine zle, so the parent's `zle -N`, `zle -A` and `zle -C` widgets were
 * absent and `bindkey` answered `undefined-key` for every key bound to one.
 * That is not only an interactive nicety -- compinit registers ~400 widgets,
 * and a completion function that reaches for one inside a subshell found it
 * missing.
 *
 * `zle -lL` is the whole fix because zle's own listing is already in the form
 * that recreates it: `-l` lists the USER-defined widgets and not the ~390
 * builtin ones (so this costs nothing in a shell that has not defined any),
 * and `-L` writes each as the `zle -N`/`zle -C` command that would define it.
 * A `zle -A` alias flattens to a direct `zle -N old new`, which is the same
 * widget by another route rather than a lost one.
 *
 * The position is load-bearing at both ends. It is AFTER the function block
 * because every widget names a shell function that has to exist first, and it
 * is after the `zmodload -L` at the top of this script because `zle` is a
 * builtin of zsh/zle and the child has to have loaded the module before it can
 * run the line. The `zmodload -e` guard is rule (12)'s: asking zle anything
 * would LOAD zsh/zle, and a shell that never used zle must not be handed one
 * -- and must not start telling its children to load it either.
 *
 * What this does not carry is the KEYMAPS. `bindkey -L` prints the whole main
 * keymap rather than the difference from the default, so replaying it would
 * cost a few hundred lines on every subshell of an interactive shell to say
 * almost nothing; a key bound to a widget the parent defined now finds the
 * widget, but a key the parent REbound still reads as its default in the
 * child. That is a narrower gap than the one being closed and it is recorded
 * rather than fixed. */
"  zmodload -e zsh/zle && zle -lL\n"
"  functions -M\n"
"  alias -L; alias -gL; alias -sL\n"
/* Rule (12) again. `zstyle` is an AUTOLOADED builtin of zsh/zutil, so calling
 * it to find out whether there are any zstyles LOADS zsh/zutil -- and
 * zutil.mdd declares moddeps="zsh/complete", whose complete.mdd declares
 * moddeps="zsh/zle", so one `zstyle -L` in the serialiser permanently added
 * three modules to the shell being serialised. The guard is exact rather than
 * conservative: the only way to have a zstyle is to have run `zstyle`, which
 * is the same thing that loads the module. */
"  zmodload -e zsh/zutil && zstyle -L\n"
"  hash -dL\n"
"  for __aok_k in \"${(@)__aok_ro}\"; do\n"
"    print -rn -- \"(( \\${+parameters[$__aok_k]} )) || \"\n"
"    typeset -p -- $__aok_k\n"
"  done\n"
"  (( $#__aok_hv )) && print -r -- \"typeset -g -H -- ${(@q)__aok_hv}\"\n"
"  (( $#__aok_hv )) && typeset -g -H -- \"${(@)__aok_hv}\" 2>/dev/null\n"
/* Last, because everything above reads through it. */
"  (( __aok_keeppar )) || zmodload -u zsh/parameter 2>/dev/null\n"
"  return 0\n"
"} \"$@\"\n";

/* The shell options and the emulation mode, emitted from C rather than from the
 * script above, and emitted LAST -- after every line of state the child has to
 * parse. Both halves of that sentence were bugs.
 *
 * WHY FROM C. Three reasons, and only the third is about speed.
 *
 * CORRECTNESS: the script runs inside `emulate -L zsh`, which changes the
 * emulation-sensitive options for the duration -- so `${(kv)options}` read in
 * there reports what emulate did, not what the parent actually had. Reading
 * `opts[]` cannot be fooled that way.
 *
 * COMPLETENESS: the child starts as `zsh -f`, so its options are the zsh
 * DEFAULTS. Only the ones that differ have to be said, which is what
 * printoptionnode's `isset(optno) ^ defset(on, emulation)` test is doing when
 * plain `setopt` lists a short list rather than all 180.
 *
 * COST: emitting all of them was about 180 separate commands for the child to
 * parse and run, on every subshell, to say almost nothing.
 *
 * WHY LAST. See rule (4): the state is TEXT, and every option that changes what
 * the lexer does changes what that text means. This block used to be first, and
 * a parent sitting in `emulate sh` therefore had its own function bodies
 * re-lexed under SH_GLOB -- where `(` is not a pattern character, so a body
 * containing `[[ ab == (a)b ]]` or a `*(.)` glob qualifier was a PARSE ERROR.
 * source() gave up at that line and every function, alias, zstyle, hash and
 * readonly parameter after it was silently dropped, along with the epilogue
 * that sets errexit and nounset. RC_QUOTES is the same story one layer down:
 * with it on, the `'\''` that zsh's own printers write for an embedded quote no
 * longer means what they wrote it to mean.
 *
 * Emitting them last is not merely safer, it is what the text asks for: every
 * line of the state was PRODUCED by zsh's printers under `emulate -L zsh`, so
 * zsh's own defaults -- which is exactly what a `zsh -f` child has until this
 * block runs -- are the reading of it that round-trips.
 *
 * The one thing rule (4) was worried about survives the move, and it is worth
 * saying why. A pattern is compiled at first USE and cached in the parse tree,
 * not compiled at parse time: `f() { case foo in (@(foo|bar)) ... }; setopt
 * kshglob; f` matches, in this shell and in zsh 5.9 alike. So a function body
 * that arrives before its options still gets its patterns compiled after them,
 * because the child does not run anything until the whole state is in.
 *
 * Excluded: the options that describe what KIND of shell this is rather than
 * how it behaves. A re-launched child is a non-interactive `zsh -f -c`, and
 * telling it that it is interactive, a login shell, running ZLE or reading its
 * commands from standard input would be false. PRIVILEGED is excluded for a
 * harder reason: turning it OFF is not a bookkeeping change, dosetopt performs
 * a real setuid/setgid, and a refusal there is an error that would abort the
 * rest of the sourced state. RCS and GLOBALRCS are NOT excluded any more --
 * they have indeed already done whatever they were going to do, but the child
 * is spawned with `-f`, which forces NO_RCS, so leaving them out did not leave
 * them alone: it handed every child rcs=off whatever the parent had, and
 * `[[ -o rcs ]]` is a question a script is allowed to ask. ERREXIT, UNSET,
 * VERBOSE, XTRACE, PRINTEXITVALUE and ALIASESOPT are emitted by
 * aok_emit_epilogue instead, after the assignments they would otherwise fire
 * on. */
static int aok_option_excluded(int optno)
{
    switch (optno) {
    case INTERACTIVE: case LOGINSHELL: case MONITOR: case USEZLE:
    case SHINSTDIN: case SINGLECOMMAND: case PRIVILEGED:
    case ERREXIT: case UNSET: case VERBOSE: case XTRACE:
    case PRINTEXITVALUE: case ALIASESOPT:
	return 1;
    default:
	return 0;
    }
}

/* Does the state tell the child to `emulate` something? Only if the parent is
 * not where a fresh shell already is. parseargs runs `emulate(zsh_name, 1,
 * ...)`, so a `zsh -f -c` child boots at EMULATE_ZSH|EMULATE_FULLY exactly. */
static int aok_emulation_emitted(void)
{
    return emulation != (EMULATE_ZSH|EMULATE_FULLY);
}

/* What opts[optno] holds in the child at the moment the option lines below run.
 *
 * This used to be assumed to be `defset(on, EMULATE_ZSH)`, i.e. the option
 * table's own zsh-default bit, and for two options that assumption is false --
 * which makes the parent's value UNREPRESENTABLE in one direction, because the
 * emitter says nothing when the parent agrees with the assumed default and the
 * child then disagrees with both. HASH_DIRS is the one that bit: the table
 * marks it OPT_ALL, but init.c sets `opts[HASHDIRS] = opts[INTERACTIVE]` at
 * startup, so a non-interactive child really starts with it off -- and a parent
 * that had turned it ON emitted nothing at all.
 *
 * Modelling the child rather than the table closes that class instead of that
 * option. The child is a non-interactive `zsh -f -c`, so:
 *
 *   - every option that is not OPT_SPECIAL starts at its zsh default (that is
 *     what parseopts_setemulate's `emulate(zsh_name, 1, ...)` does), and the
 *     OPT_SPECIAL ones are set by hand right after -- all seven of those are in
 *     aok_option_excluded, so they never reach here;
 *   - HASHDIRS and MONITOR are then overwritten with INTERACTIVE, which is 0;
 *   - `-f` on the spawn line forces RCS off;
 *   - and if the state emits an `emulate` line, that has moved the options it
 *     owns to the parent emulation's defaults before this block is read. */
static int aok_child_opt_default(Optname on)
{
    int v = !!(on->node.flags & EMULATE_ZSH);

    switch (on->optno) {
    case HASHDIRS:
	v = 0;			/* init.c: = opts[INTERACTIVE] */
	break;
    case RCS:
	v = 0;			/* the `-f` on the spawn line */
	break;
    default:
	break;
    }

    if (aok_emulation_emitted()) {
	/* setemulate(): with `emulate -R` every option that is not OPT_SPECIAL
	 * is reset, and without it only the ones flagged as relevant to
	 * emulation. OPT_SPECIAL and OPT_EMULATE live in options.c, but they
	 * are just bits above the emulation ones. */
	int fully = (emulation & EMULATE_FULLY);
	int special = (on->node.flags & (EMULATE_UNUSED << 1));
	int emulated = (on->node.flags & EMULATE_UNUSED);

	if (fully ? !special : !!emulated)
	    v = !!(on->node.flags & SHELL_EMULATION());
    }
    return v;
}

/* __thread for the reason aok_disabled gives: two native zsh shells share this
 * address space and can be serialising at the same moment. */
static __thread FILE *aok_opt_out;

static void aok_emit_one_option(HashNode hn, UNUSED(int flags))
{
    Optname on = (Optname) hn;
    int optno = on->optno;

    /* The table holds both `foo` and `nofoo` for every option; the negated
     * alias has a negative optno and would emit each one a second time. */
    if (optno <= 0)
	return;
    /* And it holds bash's and ksh's spellings -- `hashall` for HASH_CMDS,
     * `histappend` for APPEND_HISTORY, `trackall`, `histexpand`, `physical` --
     * as OPT_ALIAS nodes carrying no emulation bits at all. Those compared
     * unequal to every option that is on by default, so a plain `zsh -f`
     * subshell was handed five option lines it did not need, and any option
     * with an alias was said twice. printoptionnode skips them the same way. */
    if (on->node.flags & (EMULATE_UNUSED << 2))
	return;
    if (aok_option_excluded(optno))
	return;
    if (!!opts[optno] == aok_child_opt_default(on))
	return;
    fprintf(aok_opt_out, "builtin %s %s\n", opts[optno] ? "setopt" : "unsetopt",
	    on->node.nam);
}

/* `setopt no_aliases`, on its own, as the FIRST line of the state -- see rule
 * (1). It cannot wait for the options block below, because by then the child
 * has parsed every function body and alias definition in the state.
 *
 * `builtin` in front of it, and in front of every other command this file
 * writes into the state, for the CHILD-side half of the problem the getnode
 * swap in aok_run_state_script solves for the parent. A shell function shadows
 * a builtin of the same name, the state defines the parent's functions, and
 * everything emitted after them therefore ran the user's code instead of the
 * shell's: with `setopt() { print SHADOWED $* }` in the parent, the epilogue's
 * `setopt aliases` printed into the middle of the subshell's output, so
 * `$(print hi)` returned "SHADOWED aliases\nhi" -- and the option was never
 * restored. Moving the options after the function text (rule 4) would have
 * widened that from the epilogue to every option. `builtin` costs one word per
 * line and cannot itself be shadowed by an alias, since aliases are off here;
 * a shell function actually named `builtin` would still get through, which is
 * one name rather than a dozen. */
static void aok_emit_prologue(int fd)
{
    FILE *out = fdopen(dup(fd), "w");

    if (!out)
	return;
    fputs("builtin setopt no_aliases\n", out);
    fclose(out);
}

static void aok_emit_options(int fd)
{
    FILE *out = fdopen(dup(fd), "w");

    if (!out)
	return;
    /* The emulation itself, which is not an option and was not carried at all:
     * a child of `emulate ksh` answered `zsh` to a bare `emulate`, and listed
     * its options against zsh's defaults rather than ksh's, so `emulate sh;
     * setopt` printed 40 words where the fork it stands for prints 8. The
     * option VALUES were right even then -- they are emitted below -- so what
     * was missing is the `emulation` variable that everything else keys off.
     *
     * It goes before the option lines because it MOVES them: `emulate ksh`
     * resets every option it owns, which is why aok_child_opt_default has to
     * model it. And it is followed by `setopt no_aliases` again, because
     * ALIASESOPT is one of the options it resets and the lines after this one
     * are still shell text that the child has to parse. */
    if (aok_emulation_emitted()) {
	const char *mode;

	switch (SHELL_EMULATION()) {
	case EMULATE_CSH: mode = "csh"; break;
	case EMULATE_KSH: mode = "ksh"; break;
	case EMULATE_SH:  mode = "sh";  break;
	default:	  mode = "zsh"; break;
	}
	fprintf(out, "builtin emulate %s%s\nbuiltin setopt no_aliases\n",
		(emulation & EMULATE_FULLY) ? "-R " : "", mode);
    }
    aok_opt_out = out;
    scanhashtable(optiontab, 1, 0, 0, aok_emit_one_option, 0);
    aok_opt_out = NULL;
    fclose(out);
}

/* Everything zsh's own printers write goes to stdout, so the state is
 * collected by pointing fd 1 at the pipe for the duration. Two dup2s for the
 * whole state, rather than a redirection per line.
 *
 * That is not a micro-optimisation; it is the single measured difference
 * between a subshell that costs ~2.8ms and one that costs ~11ms. bash's first
 * version put `2>/dev/null` on every line of its state, about 85 open/dup2/
 * close round trips through the shim per subshell, and it cost 6.5ms of an
 * 8.3ms subshell. Doing the redirection in C rather than in the state avoids
 * even the one `exec` line bash needed. */
/* Everything the PARENT can do to the state script, and why none of it is the
 * script's fault.
 *
 * aok_state_script is zsh TEXT, and the parent parses and runs it in its own
 * shell context -- so every word in it is resolved against the parent's
 * aliases, its shell functions, its builtin table and its reserved words. All
 * four are the user's to change, and all four broke the fork:
 *
 *     alias emulate='builtin :;'     SIGBUS, the whole app, exit 138
 *     alias zmodload='print HIT;'    an infinite hang, killed at 40s
 *     alias for='echo NOPE'          parse error, the ENTIRE state lost
 *     disable print                  SIGBUS, the whole app, exit 138
 *     zmodload() { : }               state lost, and the parent died of SIGPIPE
 *
 * The crashes are all one mechanism. A word the serialiser needs stops being
 * the builtin it meant, degrades to an external-command lookup, that lookup
 * needs a fork, and the fork re-enters this file -- aok_spawn_subshell ->
 * aok_write_state -> aok_run_state_script -> the same broken word -- until the
 * guest task's thread stack is gone. A native program is a function call on a
 * thread of the app, so the fault takes the app down and everything else the
 * user had open goes with it.
 *
 * Rule (1) at the top of the script does say `setopt no_aliases`, but that
 * protects the CHILD sourcing the state. Nothing protected the parent
 * PRODUCING it: the whole `() { ... }` is one parse unit, so the `for ... do`
 * six lines in has already been alias-expanded by the time the `setopt
 * no_aliases` above it runs. The protection cannot live in the script at all;
 * it has to be applied from C, around execstring, where no shell-level name
 * lookup can reach it. Hence the three saves below, and the class each closes:
 *
 *   - noaliases, the lexer's own flag, so no word of the script text is
 *     alias-expanded whatever the parent aliased.
 *   - the DISABLED bit cleared across builtintab and reswdtab, so neither
 *     `disable print` nor `disable -r for` can reach the serialiser.
 *   - shfunctab's getnode swapped for one that refuses exactly the words this
 *     script uses in command position, so a user function named `print` or
 *     `zmodload` cannot shadow the builtin the script meant. getnode2 is left
 *     alone on purpose: $functions reads through getnode2, which is why rule
 *     (9) has function bodies come from $functions rather than from
 *     `functions --`, and why a user function named `print` still crosses
 *     intact while being refused as a command in here.
 *
 * That is the naming closed off IN THE PARENT. aok_serialising is the backstop
 * for whatever is not: while the state is being produced, aok_spawn_subshell
 * refuses, so a serialiser that still somehow reaches a fork fails one command
 * instead of recursing until the app dies.
 *
 * AND THE SAME PROBLEM AT THE OTHER END OF THE PIPE.
 *
 * Everything above is about the text the parent PARSES. The text the parent
 * PRODUCES is the larger exposure, and it was left open: rule (2) requires the
 * user's function definitions to come BEFORE the aliases, so by the time the
 * child reaches the bare words that carry the rest of the state -- `alias`,
 * `zstyle`, `hash -d`, `functions -M`, and every `builtin`-prefixed line this
 * file writes from C -- it has already defined functions with those names, and
 * a shell function shadows a builtin. The state then ran the USER's code
 * instead of the shell's, in the child, with the subshell's stdout live:
 *
 *     alias() { ... }      every alias lost, and the function's output
 *                          injected into the subshell's captured value. This
 *                          fires on EVERY shell, because `zsh -f` is born with
 *                          the run-help and which-command aliases.
 *     zstyle() { ... }     every zstyle silently dropped
 *     hash() { ... }       every named directory dropped
 *     functions() { ... }  every `functions -M` math function dropped
 *     builtin() { ... }    the options, traps, autoloads, command hash, sticky
 *                          emulations, floats and the whole epilogue dropped
 *
 * `typeset` is not on that list only because zsh's parser resolves declaration
 * commands itself and a shell function cannot shadow one -- verified against
 * zsh 5.9 -- which is luck rather than protection, and not luck that holds for
 * the other five.
 *
 * When such a body FORKS it stops being state loss and becomes an app-killer.
 * `alias() { print "A$(echo X)" }` is enough: the child sources the state,
 * reaches `alias run-help=man`, runs the function, the command substitution
 * needs a fork, the fork re-launches a GRANDCHILD, and the grandchild sources
 * the same state and does it again. It produced no output at all, 1.1 GB of
 * RSS in eight seconds and still climbing, and it never terminated. The
 * aok_serialising backstop cannot see it: that flag is __thread and per shell,
 * and here every level of the regress is a different shell on a different
 * guest task thread.
 *
 * So the child gets the parent's answer, applied where the child reads the
 * state: aok_state_armour_on/off below are the same three saves, called by
 * aok_run_state_script around execstring and by aok_child_init around the
 * source of the pipe. Sharing the code is the point -- the two ends cannot
 * drift apart, and a word added for one is protected at both. That closes the
 * CLASS: no word the state uses in command position can be hijacked, whatever
 * the user called their function.
 *
 * aok_sourcing_state is the child's half of the aok_serialising backstop, for
 * the same reason the parent has one. Nothing in the state has any business
 * forking -- it is builtins and definitions from end to end -- so a fork
 * attempted while the state is being sourced is by definition the regress
 * above, and refusing it fails one line of state instead of the app. */

/* Set for the duration of aok_write_state; read by aok_spawn_subshell. */
static __thread int aok_serialising = 0;

/* Set for the duration of the child's source() of the state; read by
 * aok_spawn_subshell. Separate from aok_serialising because they are different
 * shells and a shell can be doing one without the other. */
static __thread int aok_sourcing_state = 0;

/* The words the state uses as commands, at either end. Reserved words are not
 * in the list because a shell function cannot shadow one -- the parser
 * resolves those before it ever looks at shfunctab -- but `disable -r for`
 * can, which is what the DISABLED sweep is for.
 *
 * The first line is what aok_state_script itself runs in the parent. The
 * second is what only the CHILD sees: `builtin`, which prefixes every line
 * this file emits from C and was therefore the single word that could undo all
 * of them at once; `unsetopt` and `unset`, which the option block and the
 * unset-specials line use; and `trap` and `autoload`, which today are reached
 * only through `builtin` and so are already covered by it -- they are listed
 * anyway so that dropping a `builtin ` prefix somewhere cannot quietly reopen
 * this. A word costs one strcmp on a command lookup that only happens while
 * the state is being parsed. */
static const char *const aok_script_words[] = {
    "emulate", "setopt", "local", "typeset", "zmodload", "print",
    "functions", "alias", "zstyle", "hash", "continue", "return", "zle",
    "builtin", "unsetopt", "unset", "trap", "autoload", "disable", "bindkey",
    NULL
};

static __thread GetNodeFunc aok_real_shfunc_getnode;

static HashNode aok_shfunc_getnode(HashTable ht, const char *nam)
{
    const char *const *w;

    for (w = aok_script_words; *w; w++)
	if (!strcmp(nam, *w))
	    return NULL;
    return aok_real_shfunc_getnode(ht, nam);
}

/* The nodes whose DISABLED bit this file cleared, so exactly those can have it
 * back. Recorded rather than re-derived: `enable` on everything would undo a
 * `disable` the user is entitled to keep.
 *
 * __thread because two native zsh shells share this address space -- and they
 * do it at exactly this moment, the parent serialising while the child it just
 * spawned starts up. A shared list would have one shell restoring the other's
 * builtintab, which is __thread itself. */
static __thread HashNode *aok_disabled;
static __thread int aok_ndisabled, aok_disabled_cap;

static void aok_undisable_node(HashNode hn, UNUSED(int flags))
{
    if (!(hn->flags & DISABLED))
	return;
    if (aok_ndisabled == aok_disabled_cap) {
	int ncap = aok_disabled_cap ? aok_disabled_cap * 2 : 16;
	HashNode *nv = (HashNode *) zrealloc(aok_disabled,
					     ncap * sizeof(HashNode));
	if (!nv)
	    return;		/* leave this one disabled rather than lose it */
	aok_disabled = nv;
	aok_disabled_cap = ncap;
    }
    aok_disabled[aok_ndisabled++] = hn;
    hn->flags &= ~DISABLED;
}

/* The three saves described above aok_serialising, as a pair so that both ends
 * of the pipe get exactly the same protection from exactly the same code.
 *
 * The saved noaliases goes in *ONOALIASES rather than in a file-static,
 * because the parent can be armoured while a child of it is armouring itself:
 * they are different threads, but a static would still be one variable per
 * shell and this pair does not nest within a shell.
 *
 * noaliases here is not the same statement as the state's own `setopt
 * no_aliases` first line (rule 1). That line is shell text, so it can only
 * take effect once something has parsed it; this is the lexer's own flag, set
 * before the first byte of the state is read, and it holds even if the line
 * that was supposed to set the option never ran.
 *
 * The getnode save is guarded rather than unconditional. This pair is not
 * supposed to nest -- a shell either sources a state or produces one, never
 * both at once, and a fork out of either is refused -- but if it ever did,
 * saving the already-swapped pointer into aok_real_shfunc_getnode would make
 * aok_shfunc_getnode call itself forever, which is a worse failure than the
 * one being guarded against. The same guard covers the state escaping through
 * a non-local exit and leaving the armour on: the next call then leaves the
 * real getnode where it is instead of losing it. */
static void aok_state_armour_on(int *onoaliases)
{
    *onoaliases = noaliases;
    noaliases = 1;
    aok_ndisabled = 0;
    scanhashtable(builtintab, 0, 0, 0, aok_undisable_node, 0);
    scanhashtable(reswdtab, 0, 0, 0, aok_undisable_node, 0);
    /* And the three tables whose entries have a DEFINITION as well as a bit.
     *
     * A builtin or a reserved word is compiled into the child, so `disable`
     * alone recreates it there; an alias, a suffix alias and a function are
     * not, and every printer that could have emitted them -- `alias -L`,
     * `alias -sL`, and the `${(@k)functions}` loop by way of zsh/parameter --
     * skips a DISABLED node by design (parameter.c tests the bit in every
     * scanner). So the state used to emit `builtin disable -a -- hi` for an
     * alias whose definition it had never emitted, and that line then FAILED
     * in the child: `no such hash table element`. A `disable` the parent could
     * undo with one `enable` was, in every subshell, an object that no longer
     * existed.
     *
     * Un-disabling them here is the same trick already used for the two tables
     * above, and it is safe for the same reason: it lasts only while the state
     * is being written, the words the state itself runs are hidden by
     * aok_shfunc_getnode, and aliases are off entirely for the duration
     * (noaliases below). The bits are put back by aok_emit_disabled, which is
     * the last thing in the state for exactly this reason. */
    scanhashtable(aliastab, 0, 0, 0, aok_undisable_node, 0);
    scanhashtable(sufaliastab, 0, 0, 0, aok_undisable_node, 0);
    scanhashtable(shfunctab, 0, 0, 0, aok_undisable_node, 0);
    if (shfunctab->getnode != aok_shfunc_getnode) {
	aok_real_shfunc_getnode = shfunctab->getnode;
	shfunctab->getnode = aok_shfunc_getnode;
    }
}

static void aok_state_armour_off(int onoaliases)
{
    int i;

    shfunctab->getnode = aok_real_shfunc_getnode;
    for (i = 0; i < aok_ndisabled; i++)
	aok_disabled[i]->flags |= DISABLED;
    aok_ndisabled = 0;
    noaliases = onoaliases;
}

static int aok_run_state_script(int fd)
{
    int saved_out, ret = 0;
    int olastval, oerrflag, otrap_state, onoerrexit, oexit_pending;
    int odebug, ozerr;
    int onoaliases;

    fflush(stdout);
    if ((saved_out = dup(1)) < 0)
	return -1;
    if (dup2(fd, 1) < 0) {
	close(saved_out);
	return -1;
    }

    /* The serialiser must not be observable in the shell that runs it.
     *
     * noerrexit and errflag keep a parent under `setopt errexit` from dying
     * inside its own bookkeeping, and lastval is restored because `$?` is part
     * of the state being captured and every command here would clobber it.
     * trap_state goes INACTIVE so that a `return` inside a trap does not mean
     * "return from the trap" while this runs, which is what source() does for
     * the same reason.
     *
     * The DEBUG and ZERR traps need more than that, and the comment that used
     * to be here credited trap_state with work it does not do: dotrap()
     * consults sigtrapped and errflag, never trap_state, so a DEBUG trap fired
     * once per SUBLIST of this script -- roughly ten times, with fd 1 pointing
     * at the state pipe, so ten "D" lines were written INTO THE STATE the
     * child then sourced. ZERR is the same story for any line of the script
     * whose status is non-zero.
     *
     * The ZSIG_IGNORED bit is dotrapargs' own way of saying "not now": it sets
     * exactly this bit on the trap it is running so the trap cannot re-enter
     * itself. Setting it here says the same thing about the whole serialiser,
     * and it is restored rather than cleared so that a trap the user really
     * has ignored stays ignored. */
    olastval = (int) lastval;
    oerrflag = errflag;
    otrap_state = trap_state;
    onoerrexit = noerrexit;
    oexit_pending = exit_pending;
    odebug = sigtrapped[SIGDEBUG];
    ozerr = sigtrapped[SIGZERR];

    trap_state = TRAP_STATE_INACTIVE;
    noerrexit = NOERREXIT_EXIT | NOERREXIT_RETURN;
    errflag = 0;
    sigtrapped[SIGDEBUG] |= ZSIG_IGNORED;
    sigtrapped[SIGZERR] |= ZSIG_IGNORED;

    /* The parent-proofing described above. */
    aok_state_armour_on(&onoaliases);

    pushheap();
    execstring(dupstring(aok_state_script), 1, 0, "aok-state");
    popheap();

    aok_state_armour_off(onoaliases);

    fflush(stdout);

    lastval = olastval;
    errflag = oerrflag;
    trap_state = otrap_state;
    noerrexit = onoerrexit;
    exit_pending = oexit_pending;
    sigtrapped[SIGDEBUG] = odebug;
    sigtrapped[SIGZERR] = ozerr;

    if (dup2(saved_out, 1) < 0)
	ret = -1;
    close(saved_out);
    return ret;
}

/* The traps that cross, which in zsh is a per-SITE question rather than a
 * global one -- and that is the difference from bash worth knowing about.
 *
 * entersubsh (exec.c) unsets every trap from SIGEXIT up to SIGCOUNT unless the
 * caller passed ESUB_KEEPTRAP, but leaves SIGZERR and SIGDEBUG alone, and keeps
 * anything with ZSIG_FUNC. So:
 *
 *   - `TRAPINT() { ... }` function traps survive a subshell, and cross here for
 *     free, because they are ordinary entries in shfunctab and the state emits
 *     them with the other functions. That includes TRAPEXIT, which really does
 *     fire once per subshell in zsh -- the inverse of bash, where the same
 *     behaviour was a bug.
 *   - `trap 'echo' INT` list traps do NOT survive, except at the pipeline-
 *     element sites which pass ESUB_KEEPTRAP.
 *   - DEBUG and ZERR list traps survive everywhere.
 *
 * A single global rule would be wrong at one site or another, so the flag comes
 * from the call site. */
static void aok_emit_traps(int fd, int flags)
{
    FILE *out;
    int sig;

    out = fdopen(dup(fd), "w");
    if (!out)
	return;
    queue_signals();
    for (sig = 0; sig < TRAPCOUNT; sig++) {
	char *s;
	const char *name;

	if (!sigtrapped[sig] || (sigtrapped[sig] & ZSIG_FUNC))
	    continue;	/* function traps travel as functions */
	/* SIGEXIT never crosses. execcmd_fork is explicit about it --
	 * "EXIT traps shouldn't be called even if we forked to run shell code
	 * as this isn't the main shell" -- and it zeroes sigtrapped[SIGEXIT]
	 * in the child by hand even at the sites that keep the rest. */
	if (sig == SIGEXIT)
	    continue;
	if (!(flags & AOK_SUB_KEEPTRAP) && sig <= SIGCOUNT)
	    continue;
	name = getsigname(sig);
	if (!name)
	    continue;
	if (!siglists[sig]) {
	    fprintf(out, "builtin trap -- '' %s\n", name);
	    continue;
	}
	s = getpermtext(siglists[sig], NULL, 0);
	if (!s)
	    continue;
	fputs("builtin trap -- ", out);
	quotedzputs(s, out);
	fprintf(out, " %s\n", name);
	zsfree(s);
    }
    unqueue_signals();
    fclose(out);
}

/* ------------------------------------------- the tables the script cannot see
 *
 * Everything below is emitted from C for one of two reasons: either the table
 * has no shell-level printer whose output can be replayed (the command hash,
 * the autoload flags, sticky emulation), or the parent's own shell context
 * hides it from the script (TRAPEXIT). */

static __thread FILE *aok_tab_out;

/* Autoloaded functions -- `autoload -Uz f` -- with their FLAGS.
 *
 * The script used to rebuild these from the text zsh/parameter renders for a
 * stub, and got the flags wrong twice over: the extraction needed EXTENDED_GLOB
 * and never ran (see the comment in the loop), and even repaired it could only
 * ever have recovered U and t, because that text has no room for anything else.
 * The four bits are right here on the node.
 *
 * Not carried: the resolved `filename` that `autoload -r`/`-R` stores, because
 * `autoload` has no option that means "and its definition is at this path".
 * A child re-resolves it against the fpath the state gave it, which is the same
 * answer in every case except one where the parent's fpath has since changed. */
static void aok_emit_one_autoload(HashNode hn, UNUSED(int flags))
{
    Shfunc shf = (Shfunc) hn;
    int f = shf->node.flags;

    if (!(f & PM_UNDEFINED) || (f & DISABLED))
	return;
    fputs("builtin autoload", aok_tab_out);
    if (f & PM_UNALIASED)
	fputs(" -U", aok_tab_out);		/* do not alias-expand on load */
    if (f & PM_TAGGED_LOCAL)
	fputs(" -T", aok_tab_out);		/* traced, non-recursively */
    else if (f & PM_TAGGED)
	fputs(" -t", aok_tab_out);		/* traced */
    if (f & PM_KSHSTORED)
	fputs(" -k", aok_tab_out);
    if (f & PM_ZSHSTORED)
	fputs(" -z", aok_tab_out);
    fputs(" -- ", aok_tab_out);
    quotedzputs(shf->node.nam, aok_tab_out);
    fputc('\n', aok_tab_out);
}

/* The command hash table, but only the entries a user PUT there.
 *
 * The state emitted `hash -dL` (named directories) and nothing for cmdnamtab,
 * so `hash mycmd=/bin/echo; mycmd hi` said "command not found" in the child --
 * and the child is where it runs, because zsh's late fork site re-launches for
 * any external command that is not the last thing the shell will do. Worse when
 * the name shadows a real one: `hash ls=/bin/echo; ls SHADOWED` ran the real
 * ls, with no diagnostic at all.
 *
 * HASHED is the bit `hash name=path` sets. The rest of cmdnamtab is the PATH
 * cache, which is derived from a $path the state already carries and which the
 * child rebuilds for itself on demand -- emitting it would be a line per
 * external command the parent had ever run, paid on every subshell, to say what
 * the child can work out. printcmdnamnode writes the same two shapes; this is
 * its PRINT_LIST branch for a HASHED node. */
static void aok_emit_one_hashed(HashNode hn, UNUSED(int flags))
{
    Cmdnam cn = (Cmdnam) hn;

    if (!(cn->node.flags & HASHED))
	return;
    fputs("builtin hash ", aok_tab_out);
    if (cn->node.nam[0] == '-')
	fputs("-- ", aok_tab_out);
    quotedzputs(cn->node.nam, aok_tab_out);
    fputc('=', aok_tab_out);
    quotedzputs(cn->u.cmd, aok_tab_out);
    fputc('\n', aok_tab_out);
}

/* An option's canonical name from its index. options.c keeps the table itself
 * private, so this is a scan; it is only ever used for the handful of options
 * named in an `emulate ... -c` line. */
static __thread int aok_wanted_optno;
static __thread const char *aok_found_optname;

static void aok_match_optno(HashNode hn, UNUSED(int flags))
{
    Optname on = (Optname) hn;

    if (on->optno == aok_wanted_optno && !(on->node.flags & (EMULATE_UNUSED << 2)))
	aok_found_optname = on->node.nam;
}

static const char *aok_optname(int optno)
{
    aok_wanted_optno = optno;
    aok_found_optname = NULL;
    scanhashtable(optiontab, 0, 0, 0, aok_match_optno, 0);
    return aok_found_optname;
}

/* One shell function, written the way rule (9) writes them. */
static void aok_emit_funcdef(FILE *out, Shfunc shf)
{
    char *body = shf->funcdef ? getpermtext(shf->funcdef, NULL, 1) : NULL;
    char *redir = shf->redir ? getpermtext(shf->redir, NULL, 1) : NULL;

    fputs("function ", out);
    quotedzputs(shf->node.nam, out);
    fprintf(out, " {\n\t%s\n}", body ? body : ":");
    if (redir)
	fprintf(out, " %s", redir);
    fputc('\n', out);
    if (body)
	zsfree(body);
    if (redir)
	zsfree(redir);
}

/* TRAPEXIT, which the script cannot see and aok_emit_traps deliberately skips.
 *
 * Both halves of that were right on their own. A function trap IS an ordinary
 * shfunctab entry and does cross with the other functions -- TRAPINT and
 * TRAPZERR do exactly that -- and a `trap '...' EXIT` LIST trap genuinely must
 * not cross, which is what execcmd_fork says when it zeroes sigtrapped[SIGEXIT]
 * in a forked child. TRAPEXIT falls between them: zsh's own subshell keeps it
 * (entersubsh preserves ZSIG_FUNC), so `print A=$(print B)` under a TRAPEXIT
 * prints the trap's output INSIDE the substitution.
 *
 * It went missing because starttrapscope() does `unsettrap(SIGEXIT)` on entry
 * to every shell function, and unsettrap on a ZSIG_FUNC trap REMOVES the
 * function from shfunctab -- so for the whole life of the serialiser's
 * anonymous function, which is exactly when `${(@k)functions}` is read,
 * TRAPEXIT does not exist. `whence -w TRAPEXIT` in the child said "none". By
 * the time this runs, execstring has returned and endtrapscope has put it
 * back. */
static void aok_emit_exit_trap(int fd)
{
    FILE *out;
    Shfunc shf;

    if (!sigtrapped[SIGEXIT] || !(sigtrapped[SIGEXIT] & ZSIG_FUNC))
	return;
    shf = (Shfunc) shfunctab->getnode2(shfunctab, "TRAPEXIT");
    if (!shf || (shf->node.flags & (PM_UNDEFINED|DISABLED)))
	return;
    if (!(out = fdopen(dup(fd), "w")))
	return;
    /* Defining a function whose name is TRAP<SIG> installs the trap; exec.c
     * does the settrap() itself, so there is nothing else to say. The child
     * DISARMS it again when its site is not one of the four that fire it --
     * see aok_child_bits and aok_disarm_exit_trap. */
    aok_emit_funcdef(out, shf);
    fclose(out);
}

/* WHICH CHILDREN FIRE THE EXIT TRAP, AND WHY THIS IS NOT "ALL OF THEM".
 *
 * A re-launched child is a main shell, so it leaves through zexit(), and
 * zexit() fires the EXIT trap. A FORKED child mostly does not: `( )`, a
 * pipeline element and a background job leave through _realexit(), which is
 * bare _exit() with none of zexit's work. Only the substitution sites run
 * their body with execode(prog, 0, 1, ...) -- `exiting` -- and execlist fires
 * SIGEXIT for that. So a TRAPEXIT inherited by a re-launched child fired at
 * EVERY fork site, one firing per subshell more than zsh, measured against
 * zsh 5.9:
 *
 *     TRAPEXIT() { print -ru2 X }
 *     x=$(print a)          zsh: X   here: X
 *     ( print b )           zsh: -   here: X
 *     { print c } | cat     zsh: -   here: X
 *     { print d } &         zsh: -   here: X
 *     cat <(print e)        zsh: X   here: X
 *
 * The FUNCTION still has to cross, and that is what makes this a bit rather
 * than a suppressed emission. In a real subshell TRAPEXIT is still there --
 * `( functions -- TRAPEXIT )` prints its body -- it is only the trap that is
 * gone, which zsh shows by `( unfunction TRAPEXIT )` answering "no such hash
 * table element": removetrap() returns nothing when sigtrapped[SIGEXIT] is
 * clear, so the node survives its own removal. A child handed no definition at
 * all would answer that question differently and would also lose the trap for
 * its OWN substitution children, which real zsh keeps:
 *
 *     ( print ${+functions[TRAPEXIT]} )     zsh: 1
 *
 * Hence: always emit the definition, and tell the child whether to keep the
 * arming. The condition travels DOWN the whole subtree because a parent whose
 * own TRAPEXIT is already inert emits it as an ordinary function (the state
 * script's `${(@k)functions}` loop can see it once sigtrapped[SIGEXIT] is
 * clear, where an armed one is hidden from that loop) and marks its children
 * inert whatever site they are -- so `( x=$(...) )` fires nothing at either
 * level, as zsh does. */
static int aok_child_bits(int flags)
{
    int bits = (flags & AOK_SUB_INCMD) ? AOK_CHILD_INCMD : 0;
    Shfunc shf = (Shfunc) shfunctab->getnode2(shfunctab, "TRAPEXIT");

    if (shf && !(shf->node.flags & PM_UNDEFINED) &&
	(!(sigtrapped[SIGEXIT] & ZSIG_FUNC) || !(flags & AOK_SUB_EXITTRAP)))
	bits |= AOK_CHILD_EXIT_INERT;
    return bits;
}

/* The child's half: leave TRAPEXIT in shfunctab, take the trap off it.
 *
 * Written out rather than done with unsettrap(), which would take the function
 * with it -- that is the whole distinction being reproduced. What is left is
 * exactly the state zsh's own subshell is in, and it is self-consistent:
 * gettrapnode finds nothing to fire, removetrap finds nothing to remove, and
 * the next `TRAPEXIT() { ... }` in this shell arms it again from scratch.
 * exit_trap_posix is signals.c's business and is already 0 here: the child
 * defines TRAPEXIT while its options are still the `zsh -f` defaults, which is
 * before the state's option block can turn POSIXTRAPS on. */
static void aok_disarm_exit_trap(void)
{
    if (!(sigtrapped[SIGEXIT] & ZSIG_FUNC))
	return;
    if (sigtrapped[SIGEXIT] & ZSIG_TRAPPED)
	nsigtrapped--;
    sigtrapped[SIGEXIT] = 0;
}

/* Per-function STICKY emulation.
 *
 * `emulate ksh -c 'f() { ... }'` gives f a sticky emulation that zsh re-applies
 * on every call, so f keeps answering ksh's word-splitting rules however the
 * caller's options are set. Nothing zsh prints about a function mentions it, so
 * the child defined a plain function that just inherited the ambient options
 * and the answer changed -- in both directions: a sticky-zsh function saw the
 * parent's shwordsplit that it is supposed to be immune to, and a sticky-ksh
 * one lost the shwordsplit it is supposed to impose.
 *
 * The redefinition is the whole point: the script has already emitted this
 * function the ordinary way, and this second definition -- inside `emulate MODE
 * -c` -- replaces it with one that carries the sticky record. Paid only by
 * shells that have such a function, which is why it is not the only path.
 *
 * Note that the body is deliberately re-parsed under MODE here, unlike every
 * other line of the state: that is the emulation the parent parsed it under. */
static void aok_emit_one_sticky(HashNode hn, UNUSED(int flags))
{
    Shfunc shf = (Shfunc) hn;
    Emulation_options st = shf->sticky;
    FILE *out = aok_tab_out;
    const char *mode;
    char *def;
    int i;

    if (!st || (shf->node.flags & (PM_UNDEFINED|DISABLED)))
	return;

    switch (st->emulation & ((1 << 5) - 1)) {
    case EMULATE_CSH: mode = "csh"; break;
    case EMULATE_KSH: mode = "ksh"; break;
    case EMULATE_SH:  mode = "sh";  break;
    default:	      mode = "zsh"; break;
    }
    fprintf(out, "builtin emulate %s%s", (st->emulation & EMULATE_FULLY) ? "-R " : "",
	    mode);
    /* The options named in the original `emulate` line, which are part of the
     * sticky record and not of the mode. */
    for (i = 0; i < st->n_on_opts; i++) {
	const char *nm = aok_optname((int) st->on_opts[i]);
	if (nm)
	    fprintf(out, " -o %s", nm);
    }
    for (i = 0; i < st->n_off_opts; i++) {
	const char *nm = aok_optname((int) st->off_opts[i]);
	if (nm)
	    fprintf(out, " +o %s", nm);
    }

    /* The definition itself, as the single argument of -c. It is built into a
     * string rather than written out, because it has to be quoted as one word;
     * quotedzputs writes the $'...' form for the newlines in it. */
    {
	char *body = shf->funcdef ? getpermtext(shf->funcdef, NULL, 1) : NULL;
	char *redir = shf->redir ? getpermtext(shf->redir, NULL, 1) : NULL;
	char *nam = quotedzputs(shf->node.nam, NULL);
	size_t n = strlen(nam) + (body ? strlen(body) : 1) +
	    (redir ? strlen(redir) : 0) + 32;

	def = (char *) zhalloc(n);
	snprintf(def, n, "function %s {\n\t%s\n}%s%s", nam,
		 body ? body : ":", redir ? " " : "", redir ? redir : "");
	if (body)
	    zsfree(body);
	if (redir)
	    zsfree(redir);
    }
    fputs(" -c ", out);
    quotedzputs(def, out);
    fputc('\n', out);
}

/* zmodload's AUTOLOAD registrations -- `zmodload -ab zsh/datetime strftime` and
 * its -ap, -af and -ac relatives.
 *
 * The state emitted `zmodload -L`, which lists modules that are LOADED. A
 * registration the parent made but never triggered is not a loaded module, so
 * it vanished: the child answered "command not found: strftime", "unknown
 * function: sqrt", and gave back a $mapfile that was not an association.
 * (Trigger one in the parent and it becomes a loaded module, which is why this
 * only shows up for registrations nothing has used yet.)
 *
 * The awkward part is that `zmodload -aL` lists about seventy of them in a
 * shell that has done nothing at all, because a zsh is BORN with that table --
 * and re-stating what the child already knows would be seventy commands per
 * subshell. So the shell records its own table once, at startup, before any
 * user code has run (aok_snapshot_autoloads, called from init.c), and this
 * emits the difference. Parent and child are the same binary, so the two
 * snapshots agree by construction.
 *
 * `-i` on every line for the same reason rule (5) guards readonly parameters:
 * a registration that collides with one the child already has is an ERROR, and
 * an error aborts the rest of the sourced state. */
struct aok_autoreg {
    char kind;			/* b, p, f, c or C, as zmodload spells them */
    char *name;
    char *module;
};

static __thread struct aok_autoreg *aok_def_auto;
static __thread int aok_ndef_auto, aok_def_auto_cap;

/* Filled while scanning: either recording the defaults, or emitting the ones
 * that are not defaults. */
static __thread int aok_auto_recording;

static void aok_note_autoreg(char kind, const char *name, const char *module)
{
    int i;

    if (aok_auto_recording) {
	if (aok_ndef_auto == aok_def_auto_cap) {
	    int ncap = aok_def_auto_cap ? aok_def_auto_cap * 2 : 128;
	    struct aok_autoreg *nv = (struct aok_autoreg *)
		zrealloc(aok_def_auto, ncap * sizeof(*nv));
	    if (!nv)
		return;
	    aok_def_auto = nv;
	    aok_def_auto_cap = ncap;
	}
	aok_def_auto[aok_ndef_auto].kind = kind;
	aok_def_auto[aok_ndef_auto].name = ztrdup(name);
	aok_def_auto[aok_ndef_auto].module = ztrdup(module);
	aok_ndef_auto++;
	return;
    }

    for (i = 0; i < aok_ndef_auto; i++)
	if (aok_def_auto[i].kind == kind &&
	    !strcmp(aok_def_auto[i].name, name) &&
	    !strcmp(aok_def_auto[i].module, module))
	    return;		/* the child is born with this one */

    fprintf(aok_tab_out, "builtin zmodload -i -a%c ", kind);
    quotedzputs(module, aok_tab_out);
    fputc(' ', aok_tab_out);
    quotedzputs(name, aok_tab_out);
    fputc('\n', aok_tab_out);
}

static void aok_scan_one_autobin(HashNode hn, UNUSED(int flags))
{
    Builtin bn = (Builtin) hn;

    /* BINF_ADDED means the module is loaded and this is the real builtin;
     * autoloadscan uses the same test. */
    if ((bn->node.flags & BINF_ADDED) || !bn->optstr)
	return;
    aok_note_autoreg('b', bn->node.nam, bn->optstr);
}

static void aok_scan_one_autoparam(HashNode hn, UNUSED(int flags))
{
    Param pm = (Param) hn;

    if (!(pm->node.flags & PM_AUTOLOAD) || !pm->u.str)
	return;
    aok_note_autoreg('p', pm->node.nam, pm->u.str);
}

static void aok_scan_autoloads(void)
{
    MathFunc mf;
    Conddef cd;

    scanhashtable(builtintab, 0, 0, 0, aok_scan_one_autobin, 0);
    scanhashtable(realparamtab, 0, 0, 0, aok_scan_one_autoparam, 0);
    for (mf = mathfuncs; mf; mf = mf->next)
	if (!(mf->flags & MFF_USERFUNC) && mf->module)
	    aok_note_autoreg('f', mf->name, mf->module);
    for (cd = condtab; cd; cd = cd->next)
	if (cd->module)
	    aok_note_autoreg((cd->flags & CONDF_INFIX) ? 'C' : 'c',
			     cd->name, cd->module);
}

/* Called from init.c once the compiled-in modules have registered themselves
 * and before any line of user code has run. */
/**/
void aok_snapshot_autoloads(void)
{
    if (aok_ndef_auto)
	return;
    aok_auto_recording = 1;
    aok_scan_autoloads();
    aok_auto_recording = 0;
}

/* The in-memory history list, which travels as a history FILE rather than in
 * the state script.
 *
 * `fc` in a subshell said "no such event: 0", because the child started with an
 * empty list -- and a pipeline element is itself a re-launch, so even the
 * ordinary `fc -l -3 | grep` idiom failed in a shell whose own history was
 * perfectly intact.
 *
 * The obvious way to put a line into another shell's history is `print -s`, and
 * it is the wrong one: it is one command per entry, and a shell with 3000
 * entries paid 24ms on EVERY subshell to replay them -- eight times the whole
 * cost of the subshell. Batching them into an array and looping was 17ms, so
 * the cost is not the parsing. It is `print -s`. zsh's own reader does the same
 * 3000 entries in 1.6ms, so this writes what that reader reads.
 *
 * It has to be a file and not the state pipe, because readhistfile stats its
 * argument and returns immediately when st_size is 0 -- which is what a pipe
 * always reports, so `fc -R /dev/fd/N` silently read nothing. The file is
 * mkstemp'd with mode 0600, and the CHILD unlinks it as soon as it has read it
 * (aok_child_init), which is the only moment anything can know it is finished
 * with. A child that dies before that leaves one 0600 temp file behind, which
 * is the same exposure zsh's own `=( ... )` has.
 *
 * A shell reading a script has no history at all -- zsh records lines it read
 * interactively, plus whatever `print -s` put there -- so a script pays nothing
 * for this, not even the temp file.
 *
 * The format is zsh's own EXTENDED_HISTORY line, `: <start>:<duration>;<text>`,
 * whatever the parent's setting of that option: readhistfile detects the shape
 * rather than consulting the option, and writing it unconditionally is what
 * carries the timestamps that `fc -d` and `fc -f` print. The escaping is
 * savehistfile's, verbatim -- an embedded newline becomes backslash-newline and
 * a line ending in backslashes gets a trailing space, so that the reader's
 * continuation rule puts back exactly what was there. */
static char *aok_write_hist_file(void)
{
    char *fname = NULL, *dupname;
    FILE *out;
    Histent he, first;
    int fd;

    if (!hist_ring)
	return NULL;
    if ((fd = gettempfile(NULL, 1, &fname)) < 0)
	return NULL;
    if (!(out = fdopen(fd, "w"))) {
	close(fd);
	unlink(fname);
	return NULL;
    }

    /* hist_ring is the newest entry and hist_ring->down the oldest, so walking
     * `down` from there writes them oldest first, which is the order that gives
     * the child the same event numbers the parent had. */
    first = hist_ring->down;
    he = first;
    do {
	char *t;
	int end_backslashes = 0;

	if (!he->node.nam || !*he->node.nam ||
	    (he->node.flags & (HIST_TMPSTORE|HIST_DUP))) {
	    he = he->down;
	    continue;
	}
	fprintf(out, ": %ld:%ld;", (long) he->stim,
		he->ftim ? (long) (he->ftim - he->stim) : 0L);
	t = he->node.nam;
	/* The overwhelmingly common entry is one line with no backslash in it,
	 * and writing it a character at a time cost more than the rest of this
	 * function put together at 3000 entries. */
	if (!strchr(t, '\n') && !strchr(t, '\\')) {
	    fputs(t, out);
	} else {
	    for (; *t; t++) {
		if (*t == '\n')
		    fputc('\\', out);
		end_backslashes = (*t == '\\' || (end_backslashes && *t == ' '));
		fputc(*t, out);
	    }
	    if (end_backslashes)
		fputc(' ', out);
	}
	fputc('\n', out);
	he = he->down;
    } while (he != first);

    fclose(out);
    /* gettempfile's heap name does not outlive the next popheap. */
    dupname = ztrdup(fname);
    return dupname;
}

/* The four scans above, in one place, so that aok_write_state reads as a list
 * of what crosses rather than a list of file descriptors. */
static void aok_emit_tables(int fd)
{
    FILE *out = fdopen(dup(fd), "w");

    if (!out)
	return;
    aok_tab_out = out;
    aok_scan_autoloads();
    scanhashtable(shfunctab, 0, 0, 0, aok_emit_one_autoload, 0);
    scanhashtable(cmdnamtab, 0, 0, 0, aok_emit_one_hashed, 0);
    scanhashtable(shfunctab, 0, 0, 0, aok_emit_one_sticky, 0);
    aok_tab_out = NULL;
    fclose(out);
}

/* Floating-point parameters, emitted from C because `typeset -p` cannot
 * describe one.
 *
 * Two losses in one line, both silent. A float parameter prints through
 * convfloat() at its DISPLAY precision, so `typeset -F 2 f` holding 1.0/3 comes
 * back out of typeset -p as `typeset -F f=0.33`: the double itself is gone, and
 * the child's arithmetic is then WRONG rather than merely formatted
 * differently -- $(( f * 1000000 )) answered 330000 where a fork answers
 * 333333.33333333331. And the precision is not printed at all. `typeset -p`
 * keeps the number for -i, -Z and -L but not for -F and -E, so even a value
 * that survives intact reappears in the child with zsh's default 10 digits.
 *
 * From C both halves are there: the getter for the exact double, and pm->base,
 * which is where typeset stores the digit count. "%.17g" is the shortest form
 * that reads back as the identical double, and the child's assignment to a
 * numeric parameter is an arithmetic evaluation, so a plain numeric literal is
 * all it needs -- with the one wrinkle about integral values that the emitter
 * below explains.
 *
 * Special floats are not emitted here, for the same reason rule (10) does not
 * carry them: the only way to have one is `typeset -F SECONDS`, and SECONDS is
 * a clock the child reads rather than a value it inherits.
 */
static __thread FILE *aok_float_out;

static void aok_emit_one_float(HashNode hn, UNUSED(int flags))
{
    Param pm = (Param) hn;
    int pmf = pm->node.flags;
    double d;
    char val[80];
    int n;

    if (!(pmf & (PM_FFLOAT|PM_EFLOAT)))
	return;
    if (pmf & (PM_SPECIAL|PM_AUTOLOAD))
	return;
    if (!strncmp(hn->nam, "__aok_", 6) || !strncmp(hn->nam, "AOK_ZSH_", 8))
	return;

    /* Rule (5)'s guard, for the reason rule (5) gives: a module can own a
     * readonly parameter, the child has already loaded that module by rule
     * (3), and assigning a readonly that already exists aborts the rest of the
     * sourced state. */
    if (pmf & PM_READONLY)
	fprintf(aok_float_out, "(( ${+parameters[%s]} )) || ", hn->nam);
    fputs("builtin typeset -g", aok_float_out);
    if (pmf & PM_EXPORTED)
	fputs(" -x", aok_float_out);
    if (pmf & PM_HIDEVAL)
	fputs(" -H", aok_float_out);
    if (pmf & PM_READONLY)
	fputs(" -r", aok_float_out);
    fputs((pmf & PM_FFLOAT) ? " -F" : " -E", aok_float_out);
    if (pm->base > 0)
	fprintf(aok_float_out, " %d", pm->base);

    d = pm->gsu.f->getfn(pm);
    /* `typeset -F f` with no value leaves the parameter declared and unset,
     * and the getter answers 0 for it -- so writing `f=0` here would hand the
     * child a value its parent never had. The declaration alone is what
     * crosses, which is also what typeset -p writes for such a parameter. */
    if (pmf & PM_UNSET) {
	fprintf(aok_float_out, " -- %s\n", hn->nam);
	return;
    }
    /* Inf and NaN used to be emitted the same way -- declared and unset, on
     * the grounds that this transport could not carry them -- and the effect
     * was that a parent holding Inf handed the child a 0. Silently, and in the
     * direction that turns an overflow into an ordinary small number, so a
     * subshell's arithmetic disagreed with its parent's without anything
     * saying so.
     *
     * It turns out the transport can carry them after all. zsh's arithmetic
     * reads the words `Inf` and `NaN` as those values (math.c's number lexer
     * hands the text to strtod, which recognises both), so `typeset -g -F --
     * f=Inf` round-trips exactly what convfloat prints -- `$(( 1/f ))` is 0 in
     * the child as it is in the parent, and `$(( n != n ))` is 1 for the NaN.
     * The `--` already on the line is what makes the negative case safe:
     * `f=-Inf` is one word beginning with `f`, not an option.
     *
     * Nothing is said about the NaN's payload or sign bit. A real fork copies
     * those and this does not, but no zsh-level operation can observe them, so
     * the difference is not reachable from a script. */
    if (!isfinite(d)) {
	fprintf(aok_float_out, " -- %s=%s\n", hn->nam,
		isnan(d) ? "NaN" : (signbit(d) ? "-Inf" : "Inf"));
	return;
    }
    n = snprintf(val, sizeof(val), "%.17g", d);
    /* "%.17g" of a value with no fractional part has no decimal point, and for
     * NEGATIVE ZERO that silently throws the value away. `-0` is not a float
     * literal to zsh's arithmetic; it is unary minus applied to the INTEGER 0,
     * and -0 == 0, so the sign bit the parent held did not survive: a parent
     * where $(( 1/n )) is -Inf handed the child an n for which it is +Inf,
     * which a real fork never does. The `typeset -p` path this emitter
     * replaced happened to get this one case right, because convfloat always
     * prints a point -- so it was a regression rather than an old hole.
     *
     * Forcing a point on every integral value, rather than special-casing the
     * sign bit, is deliberate: it makes each emitted number a float literal
     * instead of an integer expression, which is what the parameter it is
     * being assigned to actually holds. */
    if (n > 0 && (size_t) n + 3 <= sizeof(val) && !strpbrk(val, ".eE"))
	memcpy(val + n, ".0", 3);
    fprintf(aok_float_out, " -- %s=%s\n", hn->nam, val);
}

static void aok_emit_floats(int fd)
{
    FILE *out = fdopen(dup(fd), "w");

    if (!out)
	return;
    aok_float_out = out;
    scanhashtable(paramtab, 0, 0, 0, aok_emit_one_float, 0);
    aok_float_out = NULL;
    fclose(out);
}

/* A word the child must re-read as exactly these bytes, single-quoted the way
 * aok_quote_words does it and for the same reason: single quotes are the one
 * form no further expansion can touch, and a builtin, reserved word, alias or
 * function name is the user's to choose. */
static void aok_emit_quoted(FILE *out, const char *w)
{
    fputc('\'', out);
    for (; *w; w++) {
	if (*w == '\'')
	    fputs("'\\''", out);
	else
	    fputc(*w, out);
    }
    fputc('\'', out);
}

/* The two specials the state SCRIPT cannot describe, emitted here instead.
 *
 * OPTIND, because zsh gives every function its own copy: the script runs
 * inside an anonymous function (see the block comment on aok_state_script for
 * why it has to), so `typeset -p OPTIND` in there reports the function's fresh
 * 1 and not the shell's value. Real zsh does the same -- `zsh -f -c 'OPTIND=7;
 * () { typeset -p OPTIND }'` prints 1 on the host too -- so this is not a
 * quirk of the port and no rearrangement inside the script can fix it. Reading
 * `zoptind` from C is reading the same storage without the function scope in
 * the way. It matters for the shape getopts is actually used in: a parent
 * halfway through `while getopts ab: opt` had every child told OPTIND=1, so a
 * subshell that continued the loop restarted it.
 *
 * SECONDS, because it is a clock rather than a value. The old note called it
 * "what the child measures for itself", but that is not what a fork does:
 * SECONDS is an offset from `shtimer`, a fork copies shtimer, and so a forked
 * child reports the PARENT's elapsed time and remembers an explicit
 * `SECONDS=1000`. shtimer itself crosses in the environment and is installed
 * by C in the child (see aok_inherit_string), which is exact -- the alternative
 * of emitting `SECONDS=<value>` as shell text would have re-based the clock at
 * the moment the child ran the line, losing the spawn latency. What has to be
 * said HERE is only the parameter's TYPE, since `typeset -F SECONDS` is a real
 * and load-bearing thing to have done: it switches the parameter to the
 * floating-point getter, and a child that did not know would answer whole
 * seconds to code written for fractions. */
static void aok_emit_specials(int fd)
{
    FILE *out = fdopen(dup(fd), "w");
    Param pm;

    if (!out)
	return;
    fprintf(out, "builtin typeset -g -i10 OPTIND=%lld\n", (long long) zoptind);

    pm = (Param) paramtab->getnode(paramtab, "SECONDS");
    if (pm) {
	int pmf = pm->node.flags;
	const char *type = (pmf & PM_FFLOAT) ? "-F" :
			   (pmf & PM_EFLOAT) ? "-E" : NULL;

	if (type) {
	    fprintf(out, "builtin typeset -g %s", type);
	    if (pm->base > 0)
		fprintf(out, " %d", pm->base);
	    fputs(" SECONDS\n", out);
	}
    }
    fclose(out);
}

/* The DISABLED bits, which did not cross at all: `disable print` in the parent
 * left `print` working in every subshell, where a real fork inherits the bit
 * and answers "command not found". Silent, and in the direction that runs code
 * the user had switched off.
 *
 * THE POSITION IS THE WHOLE DESIGN. This has to be the last thing the child
 * does, because the state is itself made of builtins: `builtin`, `typeset`,
 * `print`, `zmodload`, `alias`, `hash`, `zstyle`, `setopt` and `disable`
 * itself are all words the state runs, and a `disable` applied early would
 * break the rest of the state that emitted it. That is the same reason
 * aok_state_armour_on CLEARS every DISABLED bit while a shell serialises --
 * and it is why re-emitting them early would not merely fail here, it would
 * re-break the child's own serialiser on the child's next nested subshell.
 *
 * One line per table rather than one per name, for a reason that is not
 * brevity: `disable builtin` and `disable disable` are legal, and emitted
 * name-by-name the first of them would have stopped the lines after it from
 * running. A single command applies the whole set at once, so a set that
 * includes the words this line is made of still lands intact. builtintab goes
 * LAST for the same reason relative to the other tables.
 *
 * `disable -p`, which switches off pattern characters rather than table
 * entries, keeps its own list inside builtin.c and is not carried. */
static void aok_emit_disabled_table(FILE *out, HashTable ht, const char *opt)
{
    int i, first = 1;

    for (i = 0; i < ht->hsize; i++) {
	HashNode hn;

	for (hn = ht->nodes[i]; hn; hn = hn->next) {
	    if (!(hn->flags & DISABLED))
		continue;
	    if (first) {
		fprintf(out, "builtin disable %s--", opt);
		first = 0;
	    }
	    fputc(' ', out);
	    aok_emit_quoted(out, hn->nam);
	}
    }
    if (!first)
	fputc('\n', out);
}

/* `disable -p`, which switches off a PATTERN CHARACTER rather than a table
 * entry, and so has no node anywhere for aok_emit_disabled_table to find. Its
 * state is pattern.c's zpc_disables[], one byte per character, and the strings
 * that name those characters are zpc_strings[] beside it -- which is exactly
 * the argument form `disable -p` takes, so the line writes itself.
 *
 * It belongs here rather than with the options because it changes what a
 * PATTERN means, not what an option means: with `disable -p '*'` the parent
 * answers `print /etc/hos*` with the literal word and every subshell answered
 * it with the expansion, silently. And it belongs at the END of the state for
 * the same reason the rest of this block does -- the state's own lines are
 * full of patterns, and turning `*` off halfway through would change what they
 * matched. A function body that arrives earlier still compiles its patterns
 * after this, because a pattern is compiled at first USE and the child runs
 * nothing until the whole state is in. */
static void aok_emit_disabled_patchars(FILE *out)
{
    int i, first = 1;

    for (i = 0; i < ZPC_COUNT; i++) {
	if (!zpc_strings[i] || !zpc_disables[i])
	    continue;
	if (first) {
	    fputs("builtin disable -p --", out);
	    first = 0;
	}
	fputc(' ', out);
	aok_emit_quoted(out, zpc_strings[i]);
    }
    if (!first)
	fputc('\n', out);
}

static void aok_emit_disabled(int fd)
{
    FILE *out = fdopen(dup(fd), "w");

    if (!out)
	return;
    aok_emit_disabled_patchars(out);
    aok_emit_disabled_table(out, shfunctab, "-f ");
    aok_emit_disabled_table(out, reswdtab, "-r ");
    aok_emit_disabled_table(out, aliastab, "-a ");
    aok_emit_disabled_table(out, sufaliastab, "-s ");
    aok_emit_disabled_table(out, builtintab, "");
    fclose(out);
}

/* The tail of the state: the options that had to wait, the alias switch turned
 * back on, and the sentinel. */
/* The keymap difference, from zle_keymap.c. AFTER the state script, because
 * every binding names a widget and the script's `zle -lL` is what defines the
 * parent's widgets; and only worth a descriptor when zle exists at all. */
static void aok_emit_keymap_state(int fd)
{
    FILE *out;

    if (!aok_have_keymaps())
	return;
    if (!(out = fdopen(dup(fd), "w")))
	return;
    aok_emit_keymaps(out);
    fclose(out);
}

static void aok_emit_epilogue(int fd)
{
    FILE *out;

    out = fdopen(dup(fd), "w");
    if (!out)
	return;
    /* xtrace and verbose here rather than with the other options, so that the
     * state itself is not traced into the user's stderr. */
    fprintf(out, "builtin %s xtrace\n", isset(XTRACE) ? "setopt" : "unsetopt");
    fprintf(out, "builtin %s verbose\n", isset(VERBOSE) ? "setopt" : "unsetopt");
    fprintf(out, "builtin %s printexitvalue\n",
	    isset(PRINTEXITVALUE) ? "setopt" : "unsetopt");
    /* Rule (6): after every assignment the state made. */
    fprintf(out, "builtin %s errexit\n", isset(ERREXIT) ? "setopt" : "unsetopt");
    fprintf(out, "builtin %s nounset\n", isset(UNSET) ? "unsetopt" : "setopt");
    /* Rule (1)'s other half. `unsetopt` when the parent genuinely had
     * NO_ALIASES, which is not the same as leaving it alone. */
    fprintf(out, "builtin %s aliases\n", isset(ALIASESOPT) ? "setopt" : "unsetopt");
    /* A bare assignment, not `typeset -g`, because this line is on the wrong
     * side of the `setopt aliases` above it and the state has by now defined
     * every alias and function the parent had. With `alias typeset='echo
     * NOPE'` in the parent the sentinel used to come out as `echo NOPE -g
     * AOK_ZSH_STATE_OK=1`: the marker was never set, so every subshell warned
     * that its state did not finish when it had, and the echo's output was
     * captured as part of the substitution -- `$(print -r -- $v)` returned
     * "NOPE -g AOK_ZSH_STATE_OK=1" instead of the value. An assignment has no
     * command word to alias and no builtin to shadow, and the state is sourced
     * at the top level, so it is global without having to say so. */
    /* The DISABLED bits, here and nowhere earlier -- see aok_emit_disabled for
     * why "last" is the whole point. Flushed first because that function writes
     * through a descriptor of its own, and only before the sentinel so that a
     * `disable` line which somehow fails still shows up as a state that did not
     * finish rather than as silence. */
    fflush(out);
    aok_emit_disabled(fileno(out));
    fprintf(out, "%s=1\n", AOK_VAR_OK);
    fclose(out);
}

/* The already-expanded words of a simple command, quoted so that a fresh shell
 * re-parses them to exactly the same list.
 *
 * This is what keeps the late fork site from evaluating a `$(...)` twice: the
 * words handed over here have already been through prefork in this shell, so
 * the child expands nothing. Single quotes because zsh's own quotestring(...,
 * QT_SINGLE) is what `functions` and `typeset -p` round-trip through, and
 * because it is the one form no further expansion can touch. */
char *aok_quote_words(LinkList args)
{
    LinkNode node;
    char *out = NULL;
    size_t len = 0, cap = 0;

    for (node = firstnode(args); node; incnode(node)) {
	char *w = dupstring((char *) getdata(node)), *p;
	size_t need;

	untokenize(w);
	/* Worst case is every character being a quote, which becomes four. */
	need = len + 3 + strlen(w) * 4 + 2;
	if (need > cap) {
	    cap = need * 2;
	    out = out ? (char *) zrealloc(out, cap) : (char *) zalloc(cap);
	    if (!out)
		return NULL;
	}
	if (len)
	    out[len++] = ' ';
	/* Single quotes, written out here rather than through zsh's
	 * quotestring(): QT_SINGLE escapes what needs escaping INSIDE quotes
	 * but does not add the quotes, and QT_SINGLE_OPTIONAL adds them only
	 * when it judges them necessary. Both are right for their own callers
	 * and wrong here -- `/bin/echo '  spaced'` came out as
	 * `/bin/echo   spaced`, and the argument silently lost its leading
	 * whitespace. Always quoting is one line and cannot misjudge. */
	out[len++] = '\'';
	for (p = w; *p; p++) {
	    if (*p == '\'') {
		memcpy(out + len, "'\\''", 4);
		len += 4;
	    } else {
		out[len++] = *p;
	    }
	}
	out[len++] = '\'';
	out[len] = '\0';
    }
    return out ? out : ztrdup("''");
}

/* ------------------------------------------------------------------ the spawn */

void aok_spawn_init(struct aok_spawn *sp)
{
    memset(sp, 0, sizeof(*sp));
    sp->in_fd = -1;
    sp->out_fd = -1;
    sp->pgid = -1;
    sp->nclose = 0;
}

void aok_spawn_close(struct aok_spawn *sp, int fd)
{
    if (fd >= 0 && sp->nclose < AOK_MAX_CLOSE)
	sp->close_fds[sp->nclose++] = fd;
}

/* The readonly parameters a fork copies and no shell command can assign.
 *
 * `$?` already travelled this way for the reason rule (7) gives, and these are
 * the rest of the same class. Each was measurably wrong before:
 *
 *   $!         read 0 in every child, so `kill $!` inside a subshell became
 *              `kill 0` -- "signal my whole process group" -- and killed the
 *              parent shell along with the job it meant.
 *   $PPID      the child reported its real getppid(), which is the spawning
 *              SHELL rather than the shell's own parent.
 *   ZSH_SUBSHELL  started at 0 instead of the parent's depth plus one.
 *   $pipestatus  emptied, so `false | true; ( print $pipestatus )` lost the
 *              pipeline it was asked about. It cannot go in the state script
 *              either, even though it is assignable: every command run while
 *              the state is sourced overwrites it, so like `$?` it has to be
 *              restored from C after the last of them.
 *   shtimer    the origin $SECONDS is measured from. A fork copies it, so a
 *              forked child reports the PARENT's elapsed time and remembers an
 *              explicit `SECONDS=1000`; here every child restarted at 0. The
 *              two halves of the timespec travel as integers and are installed
 *              verbatim, which is exact -- emitting `SECONDS=<value>` as shell
 *              text would have re-based the clock when the child ran the line
 *              and quietly lost the spawn latency. `typeset -F SECONDS` is
 *              carried separately by aok_emit_specials, since the TYPE is
 *              shell-visible state and this is not.
 *   $RANDOM    as a seed and a draw count -- see aok_random_seed. The stream
 *              lives in libc, which a fork copies and a spawn does not.
 *   `$_`       the last argument of the previous command, which crosses a fork
 *              like any other memory. It cannot go in the state script for the
 *              same reason `$?` cannot: every line of the state sets it, and
 *              zsh will not let a script assign it at all (`_` is
 *              nullstrsetfn). It is LAST in the string because it is the only
 *              field whose value is arbitrary user text -- a path, a filename,
 *              anything with a `/` in it -- so it gets the whole remainder
 *              rather than a delimiter it could contain.
 *
 * One variable rather than eight: the child validates the whole thing once,
 * against the same spawner check `$$` gets. */
static char *aok_inherit_string(zlong subshell, int flags)
{
    /* UNmetafied, because this is going into the environment and the child
     * metafies everything it imports from there. Handing over zunderscore as
     * it is stored made the child metafy an already-metafied string, so a `$_`
     * holding the byte 0x83 -- which is zsh's own Meta character, and the
     * second byte of plenty of ordinary UTF-8 -- came back two bytes long. */
    const char *under = zunderscore ? unmetafy(dupstring(zunderscore), NULL)
				    : "";
    size_t cap = 192 + (size_t) numpipestats * 12 + strlen(under), len;
    char *s = (char *) zalloc(cap);
    int i;

    /* Always a freeable string, never NULL: aok_relaunch_env_free finds the
     * entries it owns by counting back from the end of the vector, so a
     * missing one would have it freeing a borrowed environ string. */
    if (!s)
	return ztrdup(AOK_VAR_INHERIT "=");
    /* The fourth field is a small bag of BITS about the child rather than
     * state a fork copies. They travel here because they ride the validation
     * this variable already has, and inventing another AOK_ZSH_* variable per
     * bit would mean another thing for the child to check and another thing to
     * strip out of the environment of every program it runs.
     *
     *   AOK_CHILD_INCMD       AOK_SUB_INCMD, see execlist's aok_incmd
     *   AOK_CHILD_EXIT_INERT  the child's TRAPEXIT must be DEFINED but not
     *                         ARMED -- see aok_emit_exit_trap
     *
     * The comma half of the same field is `optcind`, which is state a fork DOES
     * copy: it is how far getopts has read into a CLUSTERED option word, so
     * `set -- -ab; getopts ab o; print $(getopts ab o2; print $o2)` answers b
     * on zsh 5.9 and answered a here -- the child restarted the cluster and
     * handed back an option the parent had already consumed. It travels here
     * rather than with OPTIND because, unlike OPTIND, there is no parameter to
     * write it into: it is a bare int in builtin.c. Read now rather than with
     * the rest of the specials because doshfunc zeroes it on entry to a
     * function and the state script is one. */
    len = (size_t) snprintf(s, cap, "%s=%lld/%lld/%lld/%d,%d/%lld,%lld/%u,%lld/",
			    AOK_VAR_INHERIT,
			    (long long) lastpid, (long long) ppid,
			    (long long) subshell,
			    aok_child_bits(flags), optcind,
			    (long long) shtimer.tv_sec,
			    (long long) shtimer.tv_nsec,
			    aok_random_seed, (long long) aok_random_draws);
    for (i = 0; i < numpipestats && len + 13 < cap; i++)
	len += (size_t) snprintf(s + len, cap - len, i ? ",%d" : "%d",
				 pipestats[i]);
    snprintf(s + len, cap - len, "/%s", under);
    return s;
}

/* The child's environment: this shell's, plus the four AOK_ZSH_* entries, and
 * with any stale copy of them removed.
 *
 * The strings are BORROWED from environ except the four appended ones, so the
 * vector is freed with free() and only those four with it. Nothing has to
 * outlive the spawn: the shim packs argv and envp into flat buffers before the
 * child starts. */
static char **aok_relaunch_env(int state_fd, zlong subshell, int flags,
			       const char *histfile)
{
    char **src, **vec;
    size_t n, i, j;
    char buf[128];

    src = environ;
    for (n = 0; src && src[n]; n++)
	;
    vec = (char **) zalloc((n + 6) * sizeof(char *));
    if (!vec)
	return NULL;

    for (i = 0, j = 0; i < n; i++) {
	if (!strncmp(src[i], AOK_VAR_FD "=", sizeof(AOK_VAR_FD)) ||
	    !strncmp(src[i], AOK_VAR_DOLLAR "=", sizeof(AOK_VAR_DOLLAR)) ||
	    !strncmp(src[i], AOK_VAR_LASTVAL "=", sizeof(AOK_VAR_LASTVAL)) ||
	    !strncmp(src[i], AOK_VAR_HISTFILE "=", sizeof(AOK_VAR_HISTFILE)) ||
	    !strncmp(src[i], AOK_VAR_INHERIT "=", sizeof(AOK_VAR_INHERIT)))
	    continue;
	vec[j++] = src[i];
    }
    /* Always present, empty when this shell has no history, so that
     * aok_relaunch_env_free can go on finding its own entries by counting back
     * from the end of the vector. */
    {
	char *hf = (char *) zalloc(sizeof(AOK_VAR_HISTFILE) + 1 +
				   (histfile ? strlen(histfile) : 0));
	if (hf) {
	    sprintf(hf, "%s=%s", AOK_VAR_HISTFILE, histfile ? histfile : "");
	    vec[j++] = hf;
	} else {
	    vec[j++] = ztrdup(AOK_VAR_HISTFILE "=");
	}
    }
    sprintf(buf, "%s=%d", AOK_VAR_FD, state_fd);
    vec[j++] = ztrdup(buf);
    /* `$$` and the pid of the task doing the spawning, which the child checks
     * against its own getppid(). See aok_child_init. */
    sprintf(buf, "%s=%lld/%lld", AOK_VAR_DOLLAR, (long long) mypid,
	    (long long) getpid());
    vec[j++] = ztrdup(buf);
    sprintf(buf, "%s=%lld", AOK_VAR_LASTVAL, (long long) lastval);
    vec[j++] = ztrdup(buf);
    vec[j++] = aok_inherit_string(subshell, flags);
    vec[j] = NULL;
    return vec;
}

static void aok_relaunch_env_free(char **vec)
{
    size_t n;

    if (!vec)
	return;
    for (n = 0; vec[n]; n++)
	;
    if (n >= 5) {
	zsfree(vec[n - 1]);
	zsfree(vec[n - 2]);
	zsfree(vec[n - 3]);
	zsfree(vec[n - 4]);
	zsfree(vec[n - 5]);
    }
    zfree(vec, 0);
}

/* Write the state down the pipe, with SIGPIPE ignored for the duration.
 *
 * The child normally drains this while it starts up, so the write finishes
 * long before the pipe fills. If the child died first, the write gets EPIPE --
 * and without this, the SHELL would get SIGPIPE and die with it. Ignoring is
 * right rather than blocking: an ignored signal is discarded, where a blocked
 * one is delivered the moment it is unblocked. */
static void aok_write_state(int fd, int flags)
{
    struct sigaction sa, old;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, &old);

    /* No SIGNAL trap may run while a state is being produced, for the same
     * reason no DEBUG or ZERR trap may -- and this is the asynchronous half of
     * a class the file had only closed for the synchronous one.
     *
     * aok_run_state_script points fd 1 at the state pipe. A trap that fires
     * while it is doing so therefore writes ITS output into the state, and the
     * state is shell text: the child sourced the trap's words as commands.
     * `TRAPUSR1() { print T }; kill -USR1 $$; /bin/true; print AFTER` was
     * enough -- the T never reached the terminal, the child died partway
     * through reading a state it could not parse, the parent then wrote the
     * rest of it into a broken pipe, and the re-launch retried, so a shell that
     * should have printed "T" and "AFTER" printed a page of "write error:
     * broken pipe" and hung. Whether it hung or merely lost the trap depended
     * on where in the script the signal landed, i.e. on timing.
     *
     * DEFER rather than suppress, because a fork does not lose the signal:
     * zsh's own trap queue holds it and unqueue_traps() below replays it. The
     * replay is deliberately AFTER the descriptor has been put back and after
     * aok_serialising is clear, so the trap runs in a shell that looks normal
     * to it -- output on the real stdout, and free to fork if its body wants
     * to. Ordering against the oracle survives: real zsh runs the trap before
     * the external command's output appears, and so does this, because the
     * state is written before the child gets going.
     *
     * That also closes the fork half of the same window. A trap body is
     * arbitrary user code, so it can contain a command substitution; run from
     * inside the serialiser, that fork would have been refused by
     * aok_serialising and the trap would have failed. Deferred, it runs
     * afterwards and forks normally.
     *
     * It has to be the TRAP queue and not the SIGNAL queue, which is the part
     * that is not obvious and cost a build to learn. queue_signals() looks
     * like the right tool and is not: execcmd calls dont_queue_signals()
     * before every builtin (exec.c, just above its execbuiltin call), which
     * zeroes queueing_enabled and REPLAYS the queue -- and the state script is
     * builtins from end to end, so the first `print` in it released the signal
     * and ran the trap exactly where it must not. queue_traps() sets a
     * different flag, which handletrap() checks and nothing in exec.c touches.
     *
     * TRAPSASYNC is masked for the one call rather than honoured. The option
     * means "run traps while WAITING for a job rather than deferring them",
     * which is a statement about waiting; here the deferral is not a policy
     * choice but the only way the trap's own output can reach the terminal
     * instead of the state. opts[] is put back immediately, before
     * aok_emit_options reads it, so the child still gets the parent's real
     * setting. */
    {
	int otrapsasync = opts[TRAPSASYNC];

	opts[TRAPSASYNC] = 0;
	queue_traps(0);
	opts[TRAPSASYNC] = otrapsasync;
    }

    /* AOK_ZSH_NO_STATE is a measurement knob, not a feature: it spawns the
     * child with an EMPTY state so the fixed cost of the spawn can be told
     * apart from the cost of serialising. A shell run with it set is wrong on
     * purpose. */
    /* No fork may start while one is being served -- see the comment above
     * aok_run_state_script. Set around the whole write rather than around the
     * script alone, because the flag is what makes the refusal a refusal
     * wherever it comes from, and nothing here has any business forking. */
    aok_serialising++;

    /* The order is rule (4)'s: everything the child has to PARSE first, under
     * the zsh defaults its own printers wrote it for, and the options that
     * change what parsing means last of all. */
    if (!getenv("AOK_ZSH_NO_STATE")) {
	aok_emit_prologue(fd);
	aok_run_state_script(fd);
	aok_emit_tables(fd);
	aok_emit_keymap_state(fd);
	aok_emit_exit_trap(fd);
	aok_emit_floats(fd);
	aok_emit_specials(fd);
	aok_emit_traps(fd, flags);
	aok_emit_options(fd);
    }
    aok_emit_epilogue(fd);

    /* The single most useful thing when a re-launched child misbehaves: what
     * it was actually handed. Emitted a second time rather than tee'd, because
     * the state is written by zsh's own printers straight down the descriptor
     * and there is nothing in the middle to copy it from. Under a knob, so the
     * doubled cost is only paid by someone debugging. */
    if (getenv("AOK_ZSH_DUMP_STATE")) {
	fflush(stderr);
	fprintf(stderr, "----- AOK ZSH STATE -----\n");
	fflush(stderr);
	aok_emit_prologue(2);
	aok_run_state_script(2);
	aok_emit_tables(2);
	aok_emit_keymap_state(2);
	aok_emit_exit_trap(2);
	aok_emit_floats(2);
	aok_emit_specials(2);
	aok_emit_traps(2, flags);
	aok_emit_options(2);
	aok_emit_epilogue(2);
	fprintf(stderr, "----- END -----\n");
	fflush(stderr);
    }

    aok_serialising--;

    sigaction(SIGPIPE, &old, NULL);
    /* Last, so that anything the queue replays sees a shell with its own
     * stdout back and nothing refusing to fork. */
    unqueue_traps();
}

/* Spawn a re-launched zsh running CMDTEXT with this shell's state.
 *
 * Returns the child's pid, or -1. Never returns 0 -- which is what lets every
 * caller keep zsh's own parent-side bookkeeping (addproc, cmdoutpid,
 * procsubstpid, the job table) exactly as the fork version had it. */
pid_t aok_spawn_subshell(char *cmdtext, struct aok_spawn *sp)
{
    char *argv[7], *cmd, **envp, *histfile;
    void *fa = NULL, *attr = NULL;
    int statepipe[2], err, i;
    pid_t pid;

    if (!cmdtext) {
	errno = ENOSYS;
	return (pid_t) -1;
    }

    /* A fork asked for while this shell is serialising a previous one is the
     * infinite regress described above aok_run_state_script, and it ends as
     * SIGBUS with the app's whole address space. One failed command is the
     * better answer, and the caller already knows how to report a fork that
     * did not happen. */
    /* And the mirror image of it: a fork asked for while this shell is
     * SOURCING a state is the same regress seen from the other end, and it is
     * the one that actually killed the app, because each level of it is a new
     * shell on a new thread and no per-shell flag can see the tower. The state
     * is builtins and definitions from end to end and never legitimately
     * forks, so this cannot refuse anything the state meant to do. */
    if (aok_serialising || aok_sourcing_state) {
	errno = ENOSYS;
	return (pid_t) -1;
    }

    /* argv is not metafied. zsh keeps strings with the high bit set as
     * Meta+(c^32) internally, and handing that straight to the kernel would
     * put mojibake in the child's command line for any non-ASCII word. */
    cmd = ztrdup(cmdtext);
    unmetafy(cmd, NULL);

    if (pipe(statepipe) < 0) {
	zsfree(cmd);
	return (pid_t) -1;
    }
    /* Out of the way of the shell's own low descriptors, and recorded in
     * zsh's fdtable so nothing else claims them. */
    statepipe[0] = movefd(statepipe[0]);
    statepipe[1] = movefd(statepipe[1]);
    if (statepipe[0] < 0 || statepipe[1] < 0) {
	zclose(statepipe[0]);
	zclose(statepipe[1]);
	zsfree(cmd);
	return (pid_t) -1;
    }

    if (posix_spawn_file_actions_init(&fa) != 0) {
	zclose(statepipe[0]);
	zclose(statepipe[1]);
	zsfree(cmd);
	return (pid_t) -1;
    }
    /* The write end must not survive into the child: it would keep the state
     * pipe open against the child's own read, which never sees EOF. The
     * mirror-image of the `yes | head -1` lesson from bash's port. */
    posix_spawn_file_actions_addclose(&fa, statepipe[1]);

    /* A backgrounded job with no job control reads from /dev/null rather than
     * the terminal -- entersubsh does exactly this for ESUB_ASYNC. Without it,
     * `cmd &` competes with the interactive shell for keystrokes. */
    if (sp->stdin_null)
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDWR, 0);
    if (sp->in_fd >= 0) {
	posix_spawn_file_actions_adddup2(&fa, sp->in_fd, 0);
	if (sp->in_fd != 0)
	    posix_spawn_file_actions_addclose(&fa, sp->in_fd);
    }
    if (sp->out_fd >= 0) {
	posix_spawn_file_actions_adddup2(&fa, sp->out_fd, 1);
	if (sp->out_fd != 1)
	    posix_spawn_file_actions_addclose(&fa, sp->out_fd);
    }
    for (i = 0; i < sp->nclose; i++)
	if (sp->close_fds[i] >= 0 && sp->close_fds[i] != statepipe[0])
	    posix_spawn_file_actions_addclose(&fa, sp->close_fds[i]);

    /* The process group, and the job-control dispositions.
     *
     * A forked child does both for itself, first thing, inside entersubsh. A
     * spawned one has no such moment and the parent cannot do it afterwards --
     * sys_setpgid refuses with EACCES once the child has exec'd -- so it is
     * described to the spawn, which applies it while impersonating the child.
     *
     * SIGTSTP/TTIN/TTOU back to SIG_DFL matters for the same reason it did in
     * bash: exec resets handlers but PRESERVES SIG_IGN, an interactive shell
     * ignores all three for itself, and without this every job it started
     * ignored them too and ^Z did nothing at all. */
    if (posix_spawnattr_init(&attr) == 0) {
	short flags = 0;
	sigset_t dfl;

	if (sp->pgid >= 0) {
	    flags |= NLIBC_SPAWN_SETPGROUP;
	    posix_spawnattr_setpgroup(&attr, sp->pgid);
	}
	flags |= NLIBC_SPAWN_SETSIGDEF;
	sigemptyset(&dfl);
	sigaddset(&dfl, SIGTSTP);
	sigaddset(&dfl, SIGTTIN);
	sigaddset(&dfl, SIGTTOU);
	/* SIGQUIT too, and only for a child that is not asynchronous.
	 *
	 * zsh ignores SIGQUIT for itself in init_signals, and exec PRESERVES
	 * SIG_IGN -- so a re-launched child started up, found QUIT already
	 * ignored, and recorded it as an inherited trap. `$(trap)` then listed
	 * a `trap -- '' QUIT` that a real forked subshell does not have, which
	 * is the one line this whole conversion differed from the guest's own
	 * zsh by. entersubsh ignores INT and QUIT only for ESUB_ASYNC, so this
	 * matches it: a background job keeps the ignore. */
	if (!(sp->flags & AOK_SUB_ASYNC))
	    sigaddset(&dfl, SIGQUIT);
	/* And the same rule generalised, which is what SIGQUIT above is one
	 * instance of: `trap '' SIG` installs a REAL SIG_IGN, exec preserves
	 * SIG_IGN, and entersubsh's first act in a forked child is to unsettrap
	 * everything up to SIGCOUNT -- which puts each of those back to the
	 * default. Nothing did that here, so with `trap '' INT` in the parent,
	 * `$(/bin/sh -c 'kill -INT $$; echo survived')` printed "survived"
	 * where a fork prints nothing: the ignore had reached a
	 * great-grandchild that never asked for it. Confirmed the same way for
	 * TERM, PIPE and USR1.
	 *
	 * The two exceptions are entersubsh's own, not conveniences:
	 * POSIXTRAPS is the option that makes an ignored trap survive the
	 * unsettrap loop, and a background job re-ignores INT and QUIT for
	 * itself (settrap(SIGINT, NULL, 0) under ESUB_ASYNC) so resetting them
	 * would be undone a moment later anyway. ZSIG_FUNC cannot appear here
	 * -- a function trap is not an ignore -- but the test is written the
	 * way entersubsh writes it so the two stay comparable. */
	if (!(sp->flags & AOK_SUB_KEEPTRAP) && !isset(POSIXTRAPS)) {
	    int sig;

	    for (sig = 1; sig <= SIGCOUNT; sig++) {
		if (!(sigtrapped[sig] & ZSIG_IGNORED) ||
		    (sigtrapped[sig] & ZSIG_FUNC))
		    continue;
		if ((sp->flags & AOK_SUB_ASYNC) &&
		    (sig == SIGINT || sig == SIGQUIT))
		    continue;
		sigaddset(&dfl, sig);
	    }
	}
	posix_spawnattr_setsigdefault(&attr, &dfl);
	posix_spawnattr_setflags(&attr, flags);
    }

    argv[0] = "zsh";
    /* -f: no startup files. The state IS the startup, and reading
     * /etc/zshenv on top of it would be both slower and wrong. */
    argv[1] = "-f";
    argv[2] = "-c";
    /* `--`, because zsh goes on scanning options after -c and the command text
     * is the user's. `echo $(-lead)` handed the child `-lead`, which is a
     * perfectly good option string (-l -e -a -d), so the child ate it as
     * options and took the NEXT argument -- $0, the parent's script path -- as
     * the command to run: "permission denied: /tmp/zt/a.zsh", an empty
     * substitution, and status 0. `$(-lead extra)` contains a space and was
     * rejected outright ("bad option string"), whereupon the child exited
     * without draining the state pipe and the parent died of SIGPIPE, exit
     * 141. A leading `-` is not exotic: it is any command name starting with a
     * dash, including the mistyped ones. */
    argv[3] = "--";
    argv[4] = cmd;
    /* $0. A subshell keeps the parent's, so without this every re-launch would
     * name itself "zsh" in its own error messages. The positional parameters
     * come from the state, not from here, so that $@ survives quoting. */
    argv[5] = argzero ? argzero : "zsh";
    argv[6] = NULL;

    /* ZSH_SUBSHELL counts subshell ENTRIES, and entersubsh does the counting
     * in the forked child -- which is why this site has to supply the one the
     * fork stood for. Except when the text handed over IS the subshell: at the
     * execpline site a `( ... )` element arrives as its own source, and the
     * child increments while re-parsing it, exactly as an already-forked child
     * of real zsh does not (`( print $ZSH_SUBSHELL ) | cat` is 1, not 2). So
     * that one site says so and this adds nothing on top of it. */
    histfile = aok_write_hist_file();
    envp = aok_relaunch_env(statepipe[0],
			    zsh_subshell + (sp->subsh_counted ? 0 : 1),
			    sp->flags, histfile);
    err = posix_spawn(&pid, AOK_ZSH_PATH, &fa, attr ? &attr : NULL, argv, envp);
    aok_relaunch_env_free(envp);
    if (attr)
	posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    zsfree(cmd);

    if (err != 0) {
	/* Nothing is going to read it, so nothing is going to remove it. */
	if (histfile) {
	    unlink(histfile);
	    zsfree(histfile);
	}
	zclose(statepipe[0]);
	zclose(statepipe[1]);
	errno = err;
	return (pid_t) -1;
    }
    if (histfile)
	zsfree(histfile);

    zclose(statepipe[0]);
    aok_write_state(statepipe[1], sp->flags);
    zclose(statepipe[1]);
    return pid;
}

/* ------------------------------------------------------------------ child side */

/* `$$` in a re-launched child is the PARENT's, and it has to be, because the
 * `file-$$` idiom is everywhere: without it `mkfifo /tmp/f-$$; cmd >/tmp/f-$$ &`
 * deadlocks on a FIFO nobody will ever open. bash's port found this through its
 * own test suite.
 *
 * The validation is strict on purpose. A pid adopted here becomes `$$` for the
 * whole shell, so a stale or garbage value is a silently wrong answer rather
 * than a visible failure -- and every reason to reject falls back to getpid(),
 * which is what a shell would have done anyway. The spawner's pid is checked
 * against getppid() so that a value which leaked into some unrelated program's
 * environment cannot be adopted by a fresh top-level shell that program runs.
 *
 * The variable is removed from this shell's parameter table either way, so it
 * cannot reach anything this shell runs and cannot be emitted into the state
 * this shell hands to ITS children -- aok_relaunch_env puts a fresh one in, so
 * it propagates through nesting while a genuinely new shell started as an
 * external command still gets its own. */
static int aok_adopt_dollar(void)
{
    char *raw, *value, *end;
    long long dollar, spawner;
    int adopted = 0;

    raw = getsparam(AOK_VAR_DOLLAR);
    if (!raw)
	return 0;
    value = ztrdup(raw);
    unsetparam(AOK_VAR_DOLLAR);

    errno = 0;
    dollar = strtoll(value, &end, 10);
    if (end == value || *end != '/' || errno != 0)
	goto done;
    raw = end + 1;
    errno = 0;
    spawner = strtoll(raw, &end, 10);
    if (end == raw || *end != '\0' || errno != 0)
	goto done;
    if (dollar <= 0 || spawner <= 0)
	goto done;
    if ((pid_t) spawner != getppid())
	goto done;
    mypid = (zlong) dollar;
    adopted = 1;
done:
    zsfree(value);
    return adopted;
}

/* `$!`, `$PPID`, `$ZSH_SUBSHELL` and `$pipestatus` -- see aok_inherit_string
 * for what each of them cost while it was missing.
 *
 * Gated on the same check `$$` passed, and for the same reason: these are
 * readonly parameters, so a value adopted from a stale environment would be a
 * silently wrong answer that no later command can correct. Rejecting leaves
 * each of them at what a fresh shell computes, which is what happened before
 * any of this existed.
 *
 * The variable is removed from the parameter table whether it is used or not,
 * so it neither reaches a program this shell runs nor gets serialised into the
 * state this shell hands its own children.
 *
 * pipestatus is not applied here. Every command sourced from the state script
 * overwrites it, so the value is handed back to the caller and restored at the
 * end, next to `$?` and for the identical reason. */
/* `$_` as it arrived, held between aok_adopt_inherit and the bottom of
 * aok_child_init -- see the comment where it is parsed. */
static __thread char *aok_inherit_underscore;

/* AOK_CHILD_EXIT_INERT, held between aok_adopt_inherit (which runs before the
 * state) and the bottom of aok_child_init (which is after it, because the
 * state is what defines TRAPEXIT in the first place). */
static __thread int aok_exit_inert;

/* getopts' position within a clustered option word, held for the same reason
 * and installed in the same place: the state script is an anonymous function,
 * and doshfunc zeroes optcind on the way in and restores its own saved copy on
 * the way out. */
static __thread int aok_inherit_optcind;

static char *aok_adopt_inherit(int validated)
{
    char *raw, *value, *end, *pipes = NULL;
    long long bang, parent, subshell, cbits, coptcind, shsec, shnsec, rdraws;
    unsigned long long rseed;

    raw = getsparam(AOK_VAR_INHERIT);
    if (!raw)
	return NULL;
    value = ztrdup(raw);
    unsetparam(AOK_VAR_INHERIT);
    if (!validated)
	goto done;

    errno = 0;
    bang = strtoll(value, &end, 10);
    if (end == value || *end != '/' || errno != 0)
	goto done;
    raw = end + 1;
    errno = 0;
    parent = strtoll(raw, &end, 10);
    if (end == raw || *end != '/' || errno != 0)
	goto done;
    raw = end + 1;
    errno = 0;
    subshell = strtoll(raw, &end, 10);
    if (end == raw || *end != '/' || errno != 0)
	goto done;
    raw = end + 1;
    errno = 0;
    cbits = strtoll(raw, &end, 10);
    if (end == raw || *end != ',' || errno != 0)
	goto done;
    raw = end + 1;
    errno = 0;
    coptcind = strtoll(raw, &end, 10);
    if (end == raw || *end != '/' || errno != 0)
	goto done;
    if (bang < 0 || parent < 0 || subshell < 0 || cbits < 0 || coptcind < 0)
	goto done;

    /* shtimer, as the two halves of the timespec. Installed rather than
     * converted: SECONDS is the difference between now and this origin, so
     * copying the origin is what makes the child's clock the parent's. */
    raw = end + 1;
    errno = 0;
    shsec = strtoll(raw, &end, 10);
    if (end == raw || *end != ',' || errno != 0)
	goto done;
    raw = end + 1;
    errno = 0;
    shnsec = strtoll(raw, &end, 10);
    if (end == raw || *end != '/' || errno != 0 || shnsec < 0)
	goto done;

    /* The $RANDOM stream: a seed and how many numbers were drawn from it. */
    raw = end + 1;
    errno = 0;
    rseed = strtoull(raw, &end, 10);
    if (end == raw || *end != ',' || errno != 0)
	goto done;
    raw = end + 1;
    errno = 0;
    rdraws = strtoll(raw, &end, 10);
    if (end == raw || *end != '/' || errno != 0 || rdraws < 0)
	goto done;

    lastpid = (zlong) bang;
    ppid = (zlong) parent;
    zsh_subshell = (zlong) subshell;
    aok_relaunch_incmd = (cbits & AOK_CHILD_INCMD) != 0;
    aok_exit_inert = (cbits & AOK_CHILD_EXIT_INERT) != 0;
    aok_inherit_optcind = (int) coptcind;
    shtimer.tv_sec = (time_t) shsec;
    shtimer.tv_nsec = (long) shnsec;
    /* Rebuild libc's generator by replaying the parent's draws. Exact, because
     * srand+rand is deterministic; see aok_random_seed for the cost. */
    aok_random_seed = (unsigned int) rseed;
    aok_random_draws = (zlong) rdraws;
    srand(aok_random_seed);
    while (rdraws-- > 0)
	(void) rand();

    /* The pipestatus list, then `$_` -- which is the rest of the string,
     * whatever it contains. `$_` is held rather than installed here: this runs
     * BEFORE the state is sourced and every line of the state sets `_` to its
     * own last word, so it goes on at the bottom of aok_child_init with `$?`
     * and $pipestatus, for the same reason those do. */
    raw = end + 1;
    end = strchr(raw, '/');
    if (end) {
	*end = '\0';
	zsfree(aok_inherit_underscore);
	aok_inherit_underscore = ztrdup(end + 1);
    }
    pipes = ztrdup(raw);
done:
    zsfree(value);
    return pipes;
}

/* The other half of aok_write_hist_file: read the parent's history, then take
 * the file away.
 *
 * The unlink is here rather than in the parent because this is the first moment
 * anything knows the file has been read -- the parent has no way to wait for
 * it without waiting for the whole child.
 *
 * readhistfile takes and drops a <file>.LOCK of its own while it reads, which
 * is why the unlink comes after it rather than before. The lock is pointless
 * here -- the file is a private mkstemp handed to exactly one child, so it is
 * serialising against nobody -- but skipping it would mean exporting hist.c's
 * static lockhistct and regenerating hist.pro, and it measures 0.2ms of a
 * subshell that only pays it at all when the parent has history. */
static void aok_read_hist_file(char *histfile)
{
    if (!histfile)
	return;
    readhistfile(histfile, 0, 0);
    unlink(unmeta(histfile));
    zsfree(histfile);
}

static void aok_restore_pipestatus(char *pipes)
{
    char *p;
    int n = 0;

    if (!pipes)
	return;
    for (p = pipes; *p && n < MAX_PIPESTATS; ) {
	char *end;
	long v = strtol(p, &end, 10);

	if (end == p)
	    break;
	pipestats[n++] = (int) v;
	if (*end != ',')
	    break;
	p = end + 1;
    }
    if (n)
	numpipestats = n;
    zsfree(pipes);
}

/* Read and apply the state this shell was re-launched with.
 *
 * Called from init_misc, BEFORE the -c string is parsed -- see the block
 * comment at the top of this file for why that ordering is not negotiable.
 */
/* `$_`, applied last of all for the reason `$?` and $pipestatus are: it is set
 * by every command, and the state is nothing but commands. Without it a
 * subshell saw an empty `$_` where a fork sees the parent's last word --
 * `print hello; print "[$(print -rn -- $_)]"` gave `[]` here and `[hello]` on
 * a real fork. */
static void aok_restore_underscore(void)
{
    if (!aok_inherit_underscore)
	return;
    setunderscore(aok_inherit_underscore);
    zsfree(aok_inherit_underscore);
    aok_inherit_underscore = NULL;
}

static void aok_restore_lastval(char *lastvalstr)
{
    if (!lastvalstr)
	return;
    {
	char *lend;
	long long lv = strtoll(lastvalstr, &lend, 10);
	if (*lend == '\0')
	    lastval = (zlong) lv;
    }
    zsfree(lastvalstr);
}

/**/
void aok_child_init(void)
{
    char *fdstr, *lastvalstr, *pipes, *end, *histfile = NULL;
    int fd, devnull, saved_err = -1, incmd, onoaliases;
    long v;
    enum source_return ret = SOURCE_OK;

    pipes = aok_adopt_inherit(aok_adopt_dollar());

    /* AOK_SUB_INCMD is about the command this shell was LAUNCHED to run, and
     * the state script gets executed first -- whose very first sublist would
     * otherwise consume the flag and leave the real command with nothing. Held
     * aside here and installed at the bottom of this function, after the state
     * and after the history, so the next sublist to see it is the right one. */
    incmd = aok_relaunch_incmd;
    aok_relaunch_incmd = 0;

    /* The parent's history, which is read AFTER the state -- HISTSIZE arrives
     * with the state and is what bounds the list. Copied before the unbind for
     * the reason the two below it give, and unbound whether it is used or not
     * so that it reaches neither a program this shell runs nor the state this
     * shell hands its own children. */
    if ((end = getsparam(AOK_VAR_HISTFILE)) != NULL) {
	if (*end)
	    histfile = ztrdup(end);
	unsetparam(AOK_VAR_HISTFILE);
    }

    /* Copied before the unbind: getsparam returns a pointer into the parameter
     * table, which unsetparam frees. */
    lastvalstr = NULL;
    if ((end = getsparam(AOK_VAR_LASTVAL)) != NULL) {
	lastvalstr = ztrdup(end);
	unsetparam(AOK_VAR_LASTVAL);
    }

    /* Copied before the unbind, like the one above and for the same reason:
     * getsparam hands back a pointer INTO the parameter table, unsetparam
     * frees it, and `end` points into that same buffer. Reading it afterwards
     * is a use-after-free -- which read as garbage, failed the "trailing
     * characters" test, and made every re-launched child silently skip its own
     * state. */
    fdstr = NULL;
    if ((end = getsparam(AOK_VAR_FD)) != NULL) {
	fdstr = ztrdup(end);
	unsetparam(AOK_VAR_FD);
    }
    if (!fdstr) {
	aok_relaunch_incmd = incmd;
	aok_read_hist_file(histfile);
	aok_restore_lastval(lastvalstr);
	aok_restore_pipestatus(pipes);
	aok_restore_underscore();
	return;
    }
    v = strtol(fdstr, &end, 10);
    if (*end != '\0' || v < 0) {
	zsfree(fdstr);
	aok_relaunch_incmd = incmd;
	aok_read_hist_file(histfile);
	aok_restore_lastval(lastvalstr);
	aok_restore_pipestatus(pipes);
	aok_restore_underscore();
	return;
    }
    zsfree(fdstr);
    fd = (int) v;

    /* One redirection for the whole state, done here rather than as a line
     * inside it. What it hides is real -- a readonly variable assigned twice,
     * an option this zsh does not know -- and what it saves is the 85-odd
     * open/dup2/close round trips per subshell that bash measured at 6.5ms.
     * The sentinel below is the compensation: the state says whether it
     * finished, on the real stderr. */
    fflush(stderr);
    saved_err = dup(2);
    devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
	dup2(devnull, 2);
	close(devnull);
    }

    aok_source_fd = fd;
    /* And the child's half of the same suppression the parent does around
     * aok_run_state_script -- but through `intrap` rather than the ZSIG_IGNORED
     * bit, because here the traps do not exist yet: the state is what CREATES
     * them, and the `builtin trap -- 'print D' DEBUG` line partway through it
     * would otherwise fire for every line after itself. `trap "print D" DEBUG;
     * ( print in )` printed ten D's where zsh prints two, and half of them
     * arrived before any of the user's own output.
     *
     * intrap is exactly zsh's flag for "a synchronous trap must not run in
     * here": dotrapargs returns immediately for EXIT, DEBUG and ZERR while it
     * is set, and execlist skips its two DEBUG sites outright. It is the same
     * answer at both ends of the pipe; only the way of saying it differs. */
    intrap++;
    /* And the child's half of the parent-proofing, for the reasons set out
     * above aok_serialising: the state defines the user's functions before it
     * runs the words that carry the rest of itself, so without this a function
     * named `alias`, `hash`, `zstyle`, `functions` or `builtin` ate everything
     * emitted after it -- and one that forked took the app down. */
    aok_state_armour_on(&onoaliases);
    aok_sourcing_state++;
    ret = source("(aok subshell state)");
    aok_sourcing_state--;
    aok_state_armour_off(onoaliases);
    intrap--;

    if (saved_err >= 0) {
	fflush(stderr);
	dup2(saved_err, 2);
	close(saved_err);
    }

    /* Written straight to the descriptor rather than through zwarn(), which
     * returns without printing when errflag is set -- and a state that failed
     * partway through has just set it. The one message that must survive is
     * the one that says the state failed. */
    if (getsparam(AOK_VAR_OK)) {
	unsetparam(AOK_VAR_OK);
    } else {
	char msg[128];
	int n = snprintf(msg, sizeof(msg),
		"zsh: subshell state did not finish (source returned %d); "
		"AOK_ZSH_DUMP_STATE=1 in the parent shows what was sent\n",
		(int) ret);
	if (n > 0)
	    write(2, msg, (size_t) n);
    }
    /* Before aok_inherited_ntraps is taken, because a trap this shell will
     * never fire must not be counted as one of the traps it was handed -- that
     * count is what decides whether the command it was launched to run has to
     * fork at all. */
    if (aok_exit_inert)
	aok_disarm_exit_trap();
    optcind = aok_inherit_optcind;
    aok_inherited_ntraps = nsigtrapped;
    aok_relaunch_incmd = incmd;

    /* After the state, because HISTSIZE came with it and HISTSIZE is what
     * bounds the list this reads into. */
    aok_read_hist_file(histfile);

    /* Errors inside the state are the state's problem, not the command's. */
    errflag = 0;
    lastval = 0;
    /* $? last of all, because every line of the state has just set it. This is
     * rule (7) at the top of the file: a subshell really does inherit `$?`
     * (`false; print "$( print $? )"` prints 1) and there is no way to say so
     * in a script that survives `errexit`. $pipestatus is the same story: it is
     * assignable, but every sourced line rewrites it. */
    aok_restore_lastval(lastvalstr);
    aok_restore_pipestatus(pipes);
    aok_restore_underscore();
}
