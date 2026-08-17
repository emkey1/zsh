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
 */
#define AOK_VAR_FD      "AOK_ZSH_STATE_FD"
#define AOK_VAR_DOLLAR  "AOK_ZSH_DOLLAR"
#define AOK_VAR_LASTVAL "AOK_ZSH_LASTVAL"
#define AOK_VAR_INHERIT "AOK_ZSH_INHERIT"

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
 * (4) Options before anything that uses a pattern. zsh compiles a pattern at
 *     first USE and caches it forever, so a function containing `@(foo|bar)`
 *     replayed without `kshglob` sources cleanly, returns 0, and matches
 *     nothing. Silently wrong rather than a syntax error, which is the harder
 *     failure to notice.
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
 *     `0` is excluded because it reaches the child as argv[5] of the spawn,
 *     and `typeset -g 0=...` is not even valid syntax.
 *
 * (11) `${(@q-)dirstack}`, with the (@) that the argv line beside it gets from
 *     its `@`. Without it the nested expansion sits in a double-quoted context,
 *     which joins the array into one scalar BEFORE quoting it, so a parent with
 *     /etc and /usr on the stack emitted `dirstack=( '/etc /usr' )` and every
 *     child saw $#dirstack == 1. popd in a subshell then failed with "no such
 *     file or directory: /etc /usr", `cd +2` did not move, and a directory
 *     whose name contains a space was destroyed outright.
 */
static const char aok_state_script[] =
"() {\n"
"  emulate -L zsh\n"
"  setopt no_aliases no_nomatch no_unset\n"
"  local __aok_k __aok_v __aok_a __aok_b\n"
"  local -a __aok_ro __aok_fn __aok_pv __aok_hv __aok_out\n"
"  zmodload -i zsh/parameter 2>/dev/null\n"

/* $functions[k] is not a lookup, it is getpermtext() rendering the whole
 * function body afresh on every reference -- so with compinit loaded and 971
 * functions in the table, saying it three times in this loop would render
 * ~3000 function bodies to serialise 971. It is read once. */
"  for __aok_k in \"${(@k)functions}\"; do\n"
"    __aok_b=$functions[$__aok_k]\n"
/* Anchored, not a substring search. zsh/parameter renders an autoload stub as
 * exactly "builtin autoload -X" plus its flag letters and nothing before it,
 * while a real body always begins with the tab the printer indents it with (or
 * with `{` when a redirection is attached) -- so the anchor is exact, where the
 * `*...*` this used to be matched any function that merely MENTIONED the
 * string. Such a function crossed the fork replaced by an autoload stub, and
 * every call in every subshell answered "function definition file not found".
 * It is not a contrived body either: this file's own test harness tripped it. */
"    if [[ $__aok_b == 'builtin autoload -X'* ]]; then\n"
"      __aok_v=${__aok_b#*autoload -X}\n"
"      __aok_v=${(M)__aok_v##[A-Za-z]#}\n"
"      __aok_out+=( \"autoload ${__aok_v:+-$__aok_v} -- ${(q)__aok_k}\" )\n"
"    elif [[ $__aok_b == '{'* ]]; then\n"
"      __aok_fn+=( \"function ${(qq)__aok_k} $__aok_b\" )\n"
"    else\n"
"      __aok_fn+=( \"function ${(qq)__aok_k} {\" \"$__aok_b\" '}' )\n"
"    fi\n"
"  done\n"
"  for __aok_k in \"${(@k)parameters}\"; do\n"
"    __aok_a=$parameters[$__aok_k]\n"
"    case $__aok_k in (__aok_*|AOK_ZSH_*|argv|status|0) continue ;; esac\n"
"    if [[ $__aok_a == *special* ]]; then\n"
"      case $__aok_k in\n"
"      (IFS|WORDCHARS|KEYBOARD_HACK|HISTCHARS|histchars|SHLVL) ;;\n"
"      (HISTSIZE|SAVEHIST|FUNCNEST|LINES|COLUMNS|ZLE_RPROMPT_INDENT) ;;\n"
"      (OPTIND|OPTARG|NULLCMD|READNULLCMD|POSTEDIT|WATCH|watch) ;;\n"
"      (PS1|PS2|PS3|PS4|RPS1|RPS2|RPROMPT|RPROMPT2|SPROMPT) ;;\n"
"      (prompt|PROMPT|PROMPT2|PROMPT3|PROMPT4) ;;\n"
"      (TRY_BLOCK_ERROR|TRY_BLOCK_INTERRUPT) ;;\n"
"      (PATH|path|CDPATH|cdpath|FPATH|fpath|MANPATH|manpath) ;;\n"
"      (MAILPATH|mailpath|FIGNORE|fignore|PSVAR|psvar) ;;\n"
"      (MODULE_PATH|module_path) ;;\n"
"      (*) continue ;;\n"
"      esac\n"
"    fi\n"
"    [[ $__aok_a == *hideval* ]] && __aok_hv+=( $__aok_k )\n"
"    [[ $__aok_a == *float* ]] && continue\n"
"    if [[ $__aok_a == *readonly* ]]; then __aok_ro+=( $__aok_k )\n"
"    else __aok_pv+=( $__aok_k )\n"
"    fi\n"
"  done\n"
/* A special the parent UNSET does not appear in $parameters at all -- the loop
 * above cannot see it, and the child, which starts as a fresh `zsh -f`, has it
 * back at its default. `unset IFS` is the one that gets written on purpose
 * (POSIX-minded scripts do it), and while zsh splits identically with IFS unset
 * and IFS at its default, `${+IFS}` and `${IFS-x}` do not. So the names a fresh
 * shell has SET are asked about by name; in a shell that unset none of them
 * this emits nothing at all, which is why it is a loop in the parent rather
 * than a line in the child. The list is rule (10)'s, minus the members a fresh
 * `zsh -f` already has unset -- there is nothing to correct for those. */
"  for __aok_k in IFS WORDCHARS KEYBOARD_HACK HISTCHARS histchars SHLVL \\\n"
"      HISTSIZE SAVEHIST FUNCNEST LINES COLUMNS OPTIND OPTARG NULLCMD \\\n"
"      READNULLCMD PS1 PS2 PS3 PS4 SPROMPT prompt PROMPT PROMPT2 PROMPT3 \\\n"
"      PROMPT4 TRY_BLOCK_ERROR TRY_BLOCK_INTERRUPT PATH path CDPATH cdpath \\\n"
"      FPATH fpath MANPATH manpath MAILPATH mailpath FIGNORE fignore PSVAR \\\n"
"      psvar MODULE_PATH module_path; do\n"
"    [[ -v $__aok_k ]] || __aok_out+=( \"unset -- $__aok_k\" )\n"
"  done\n"
"  __aok_out+=( \"dirstack=( ${(j: :)${(@q-)dirstack}} )\" )\n"
"  __aok_out+=( \"argv=( ${(j: :)${(q-)@}} )\" )\n"
"  print -rl -- \"${(@)__aok_out}\"\n"
"  zmodload -L\n"
"  (( $#__aok_hv )) && typeset -g +H -- \"${(@)__aok_hv}\" 2>/dev/null\n"
"  (( $#__aok_pv )) && typeset -p -- \"${(@)__aok_pv}\"\n"
"  (( $#__aok_fn )) && print -rl -- \"${(@)__aok_fn}\"\n"
"  functions -M\n"
"  alias -L; alias -gL; alias -sL\n"
"  zstyle -L 2>/dev/null\n"
"  hash -dL\n"
"  for __aok_k in \"${(@)__aok_ro}\"; do\n"
"    print -rn -- \"(( \\${+parameters[$__aok_k]} )) || \"\n"
"    typeset -p -- $__aok_k\n"
"  done\n"
"  (( $#__aok_hv )) && print -r -- \"typeset -g -H -- ${(@q)__aok_hv}\"\n"
"  (( $#__aok_hv )) && typeset -g -H -- \"${(@)__aok_hv}\" 2>/dev/null\n"
"  return 0\n"
"} \"$@\"\n";

/* The shell options, emitted from C rather than from the script above.
 *
 * Three reasons, and only the third is about speed.
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
 * Excluded: the options that describe what KIND of shell this is rather than
 * how it behaves. A re-launched child is a non-interactive `zsh -f -c`, and
 * telling it that it is interactive, a login shell, running ZLE or reading its
 * commands from standard input would be false. RCS and GLOBALRCS have already
 * done whatever they were going to do by the time the state is read. ERREXIT,
 * UNSET, VERBOSE, XTRACE, PRINTEXITVALUE and ALIASESOPT are emitted by
 * aok_emit_epilogue instead, after the assignments they would otherwise fire
 * on. */
static int aok_option_excluded(int optno)
{
    switch (optno) {
    case INTERACTIVE: case LOGINSHELL: case MONITOR: case USEZLE:
    case SHINSTDIN: case SINGLECOMMAND: case PRIVILEGED:
    case RCS: case GLOBALRCS:
    case ERREXIT: case UNSET: case VERBOSE: case XTRACE:
    case PRINTEXITVALUE: case ALIASESOPT:
	return 1;
    default:
	return 0;
    }
}

static FILE *aok_opt_out;

static void aok_emit_one_option(HashNode hn, UNUSED(int flags))
{
    Optname on = (Optname) hn;
    int optno = on->optno;

    /* The table holds both `foo` and `nofoo` for every option; the negated
     * alias has a negative optno and would emit each one a second time. */
    if (optno <= 0)
	return;
    if (aok_option_excluded(optno))
	return;
    /* defset() is a macro private to options.c: an option is on by default in
     * an emulation when its flags carry that emulation's bit. The child's
     * emulation is EMULATE_ZSH, because it is started as `zsh -f`. */
    if (!!opts[optno] == !!(on->node.flags & EMULATE_ZSH))
	return;
    fprintf(aok_opt_out, "%s %s\n", opts[optno] ? "setopt" : "unsetopt",
	    on->node.nam);
}

static void aok_emit_options(int fd)
{
    FILE *out = fdopen(dup(fd), "w");

    if (!out)
	return;
    aok_opt_out = out;
    /* no_aliases FIRST, before anything the child parses -- see rule (1). */
    fputs("setopt no_aliases\n", out);
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
 * That is the naming closed off. aok_serialising is the backstop for whatever
 * is not: while the state is being produced, aok_spawn_subshell refuses, so a
 * serialiser that still somehow reaches a fork fails one command instead of
 * recursing until the app dies. */

/* Set for the duration of aok_write_state; read by aok_spawn_subshell. */
static __thread int aok_serialising = 0;

/* The words aok_state_script runs as commands. Reserved words are not in the
 * list because a shell function cannot shadow one -- the parser resolves those
 * before it ever looks at shfunctab -- but `disable -r for` can, which is what
 * the DISABLED sweep is for. */
static const char *const aok_script_words[] = {
    "emulate", "setopt", "local", "typeset", "zmodload", "print",
    "functions", "alias", "zstyle", "hash", "continue", "return", NULL
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

static int aok_run_state_script(int fd)
{
    int saved_out, ret = 0;
    int olastval, oerrflag, otrap_state, onoerrexit, oexit_pending;
    int onoaliases, i;

    fflush(stdout);
    if ((saved_out = dup(1)) < 0)
	return -1;
    if (dup2(fd, 1) < 0) {
	close(saved_out);
	return -1;
    }

    /* The serialiser must not be observable in the shell that runs it.
     *
     * trap_state goes INACTIVE so a DEBUG or ZERR trap does not fire once per
     * line of the state -- bash's equivalent bug was an EXIT trap firing once
     * per subshell, and this is the same class one step earlier. noerrexit and
     * errflag keep a parent under `setopt errexit` from dying inside its own
     * bookkeeping, and lastval is restored because `$?` is part of the state
     * being captured and every command here would clobber it. */
    olastval = (int) lastval;
    oerrflag = errflag;
    otrap_state = trap_state;
    onoerrexit = noerrexit;
    oexit_pending = exit_pending;

    trap_state = TRAP_STATE_INACTIVE;
    noerrexit = NOERREXIT_EXIT | NOERREXIT_RETURN;
    errflag = 0;

    /* The parent-proofing described above. */
    onoaliases = noaliases;
    noaliases = 1;
    aok_ndisabled = 0;
    scanhashtable(builtintab, 0, 0, 0, aok_undisable_node, 0);
    scanhashtable(reswdtab, 0, 0, 0, aok_undisable_node, 0);
    aok_real_shfunc_getnode = shfunctab->getnode;
    shfunctab->getnode = aok_shfunc_getnode;

    pushheap();
    execstring(dupstring(aok_state_script), 1, 0, "aok-state");
    popheap();

    shfunctab->getnode = aok_real_shfunc_getnode;
    for (i = 0; i < aok_ndisabled; i++)
	aok_disabled[i]->flags |= DISABLED;
    aok_ndisabled = 0;
    noaliases = onoaliases;

    fflush(stdout);

    lastval = olastval;
    errflag = oerrflag;
    trap_state = otrap_state;
    noerrexit = onoerrexit;
    exit_pending = oexit_pending;

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
	    fprintf(out, "trap -- '' %s\n", name);
	    continue;
	}
	s = getpermtext(siglists[sig], NULL, 0);
	if (!s)
	    continue;
	fputs("trap -- ", out);
	quotedzputs(s, out);
	fprintf(out, " %s\n", name);
	zsfree(s);
    }
    unqueue_signals();
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
 * all it needs.
 *
 * Special floats are not emitted here, for the same reason rule (10) does not
 * carry them: the only way to have one is `typeset -F SECONDS`, and SECONDS is
 * a clock the child reads rather than a value it inherits.
 */
static FILE *aok_float_out;

static void aok_emit_one_float(HashNode hn, UNUSED(int flags))
{
    Param pm = (Param) hn;
    int pmf = pm->node.flags;
    double d;
    char val[80];

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
    fputs("typeset -g", aok_float_out);
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
     * crosses, which is also what typeset -p writes for such a parameter.
     *
     * Inf and NaN are left the same way: convfloat spells them "Inf" and
     * "NaN", which the child's arithmetic reads as unset parameters, i.e. 0.
     * Declared-but-not-assigned is the honest answer for a value this
     * transport cannot carry. */
    if ((pmf & PM_UNSET) || !isfinite(d)) {
	fprintf(aok_float_out, " -- %s\n", hn->nam);
	return;
    }
    snprintf(val, sizeof(val), "%.17g", d);
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

/* The tail of the state: the options that had to wait, the alias switch turned
 * back on, and the sentinel. */
static void aok_emit_epilogue(int fd)
{
    FILE *out;

    out = fdopen(dup(fd), "w");
    if (!out)
	return;
    /* xtrace and verbose here rather than with the other options, so that the
     * state itself is not traced into the user's stderr. */
    fprintf(out, "%s xtrace\n", isset(XTRACE) ? "setopt" : "unsetopt");
    fprintf(out, "%s verbose\n", isset(VERBOSE) ? "setopt" : "unsetopt");
    fprintf(out, "%s printexitvalue\n",
	    isset(PRINTEXITVALUE) ? "setopt" : "unsetopt");
    /* Rule (6): after every assignment the state made. */
    fprintf(out, "%s errexit\n", isset(ERREXIT) ? "setopt" : "unsetopt");
    fprintf(out, "%s nounset\n", isset(UNSET) ? "unsetopt" : "setopt");
    /* Rule (1)'s other half. `unsetopt` when the parent genuinely had
     * NO_ALIASES, which is not the same as leaving it alone. */
    fprintf(out, "%s aliases\n", isset(ALIASESOPT) ? "setopt" : "unsetopt");
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
 *
 * One variable rather than four: the child validates the whole thing once,
 * against the same spawner check `$$` gets. */
static char *aok_inherit_string(zlong subshell)
{
    size_t cap = 128 + (size_t) numpipestats * 12, len;
    char *s = (char *) zalloc(cap);
    int i;

    /* Always a freeable string, never NULL: aok_relaunch_env_free finds the
     * entries it owns by counting back from the end of the vector, so a
     * missing one would have it freeing a borrowed environ string. */
    if (!s)
	return ztrdup(AOK_VAR_INHERIT "=");
    len = (size_t) snprintf(s, cap, "%s=%lld/%lld/%lld/", AOK_VAR_INHERIT,
			    (long long) lastpid, (long long) ppid,
			    (long long) subshell);
    for (i = 0; i < numpipestats && len + 13 < cap; i++)
	len += (size_t) snprintf(s + len, cap - len, i ? ",%d" : "%d",
				 pipestats[i]);
    return s;
}

/* The child's environment: this shell's, plus the four AOK_ZSH_* entries, and
 * with any stale copy of them removed.
 *
 * The strings are BORROWED from environ except the four appended ones, so the
 * vector is freed with free() and only those four with it. Nothing has to
 * outlive the spawn: the shim packs argv and envp into flat buffers before the
 * child starts. */
static char **aok_relaunch_env(int state_fd, zlong subshell)
{
    char **src, **vec;
    size_t n, i, j;
    char buf[128];

    src = environ;
    for (n = 0; src && src[n]; n++)
	;
    vec = (char **) zalloc((n + 5) * sizeof(char *));
    if (!vec)
	return NULL;

    for (i = 0, j = 0; i < n; i++) {
	if (!strncmp(src[i], AOK_VAR_FD "=", sizeof(AOK_VAR_FD)) ||
	    !strncmp(src[i], AOK_VAR_DOLLAR "=", sizeof(AOK_VAR_DOLLAR)) ||
	    !strncmp(src[i], AOK_VAR_LASTVAL "=", sizeof(AOK_VAR_LASTVAL)) ||
	    !strncmp(src[i], AOK_VAR_INHERIT "=", sizeof(AOK_VAR_INHERIT)))
	    continue;
	vec[j++] = src[i];
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
    vec[j++] = aok_inherit_string(subshell);
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
    if (n >= 4) {
	zsfree(vec[n - 1]);
	zsfree(vec[n - 2]);
	zsfree(vec[n - 3]);
	zsfree(vec[n - 4]);
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

    /* AOK_ZSH_NO_STATE is a measurement knob, not a feature: it spawns the
     * child with an EMPTY state so the fixed cost of the spawn can be told
     * apart from the cost of serialising. A shell run with it set is wrong on
     * purpose. */
    /* No fork may start while one is being served -- see the comment above
     * aok_run_state_script. Set around the whole write rather than around the
     * script alone, because the flag is what makes the refusal a refusal
     * wherever it comes from, and nothing here has any business forking. */
    aok_serialising++;

    if (!getenv("AOK_ZSH_NO_STATE")) {
	aok_emit_options(fd);
	aok_run_state_script(fd);
	aok_emit_floats(fd);
	aok_emit_traps(fd, flags);
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
	aok_emit_options(2);
	aok_run_state_script(2);
	aok_emit_floats(2);
	aok_emit_traps(2, flags);
	aok_emit_epilogue(2);
	fprintf(stderr, "----- END -----\n");
	fflush(stderr);
    }

    aok_serialising--;

    sigaction(SIGPIPE, &old, NULL);
}

/* Spawn a re-launched zsh running CMDTEXT with this shell's state.
 *
 * Returns the child's pid, or -1. Never returns 0 -- which is what lets every
 * caller keep zsh's own parent-side bookkeeping (addproc, cmdoutpid,
 * procsubstpid, the job table) exactly as the fork version had it. */
pid_t aok_spawn_subshell(char *cmdtext, struct aok_spawn *sp)
{
    char *argv[7], *cmd, **envp;
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
    if (aok_serialising) {
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
    envp = aok_relaunch_env(statepipe[0],
			    zsh_subshell + (sp->subsh_counted ? 0 : 1));
    err = posix_spawn(&pid, AOK_ZSH_PATH, &fa, attr ? &attr : NULL, argv, envp);
    aok_relaunch_env_free(envp);
    if (attr)
	posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    zsfree(cmd);

    if (err != 0) {
	zclose(statepipe[0]);
	zclose(statepipe[1]);
	errno = err;
	return (pid_t) -1;
    }

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
static char *aok_adopt_inherit(int validated)
{
    char *raw, *value, *end, *pipes = NULL;
    long long bang, parent, subshell;

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
    if (bang < 0 || parent < 0 || subshell < 0)
	goto done;

    lastpid = (zlong) bang;
    ppid = (zlong) parent;
    zsh_subshell = (zlong) subshell;
    pipes = ztrdup(end + 1);
done:
    zsfree(value);
    return pipes;
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
    char *fdstr, *lastvalstr, *pipes, *end;
    int fd, devnull, saved_err = -1;
    long v;
    enum source_return ret = SOURCE_OK;

    pipes = aok_adopt_inherit(aok_adopt_dollar());

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
	aok_restore_lastval(lastvalstr);
	aok_restore_pipestatus(pipes);
	return;
    }
    v = strtol(fdstr, &end, 10);
    if (*end != '\0' || v < 0) {
	zsfree(fdstr);
	aok_restore_lastval(lastvalstr);
	aok_restore_pipestatus(pipes);
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
    ret = source("(aok subshell state)");

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
    aok_inherited_ntraps = nsigtrapped;

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
}
