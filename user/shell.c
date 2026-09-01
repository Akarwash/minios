// The TownOS interactive shell, as a ring-3 program.
//
// This is the capstone of the syscall boundary. The shell is a fully fenced-in
// user program: it is loaded off the disk like any other (as SHELL.ELF), runs at
// CPL 3 in its own address space, and cannot touch the keyboard, the screen, the
// filesystem, or the loader except through `int 0x50`. Everything it does, reading
// a key, echoing it, listing files, printing a file, launching a program, it does
// with nothing but the syscalls in userlib.h. That it works at all is the proof
// that the boundary is complete.
//
// Built and linked exactly like the test fixtures in user/tests/ (see the
// SHELL.ELF rule in the Makefile
// and user/user.ld), copied onto the FAT32 image as SHELL.ELF, and launched by
// kernel_main. See docs/reference/shell.md.

#include "userlib.h"

// The line buffer is fixed. If it fills, further printable characters are DROPPED
// rather than accepted, so a long line cannot overflow it; one slot is reserved
// for the terminating '\0'.
#define SHELL_LINE_MAX   128

// Scratch for SYS_LIST. Big enough for the handful of 8.3 names on the disk, one
// per line; if the kernel had more names than fit it would drop the overflow.
#define SHELL_LIST_MAX   512

// Scratch for SYS_READFILE. Sized past the largest test file (BIG.TXT is 16384
// bytes) so `read` can print it whole. One byte is reserved for the '\0' the shell
// appends before printing, so SYS_READFILE is asked for at most SHELL_FILE_MAX - 1.
#define SHELL_FILE_MAX   32768

// `clear` scrolls the screen by printing a screenful of newlines. There is no
// clear-screen syscall (that would be a fifth syscall for a cosmetic command), and
// the screen is 25 rows, so 25 newlines pushes everything off. The prompt then
// reprints at the bottom.
#define SHELL_CLEAR_LINES 25

// The most stages a single `a | b | c | ...` line may have. Small on purpose: the
// line buffer is 128 bytes, so a very long pipeline cannot fit anyway, and each
// stage costs two of the shell's own 8 descriptors while it is being wired up. A
// line with more segments than this is rejected with a message rather than
// overflowing the segment array.
#define SHELL_MAX_SEGS 4

// Static (in .bss, inside the ring-3 region), not on the stack: the file buffer is
// large, and keeping these off the 256KB user stack leaves it for call frames.
static char line[SHELL_LINE_MAX];

// Set by the handler, read and cleared by read_line. A handler cannot reach
// read_line's local length counter, and abandoning the half-typed line is most of
// what "Ctrl-C at a prompt" MEANS: without this the fresh prompt is cosmetic, the
// characters already typed are still in the buffer, and the next thing the user
// types is appended to them. Volatile because nothing the compiler can see writes it.
static volatile int line_abandoned;
static char list_buf[SHELL_LIST_MAX];
static char file_buf[SHELL_FILE_MAX];

// The value SYS_LIST / SYS_RUN / SYS_READFILE return on failure. The syscalls hand
// back (unsigned long)-1; name it here so the checks below read as intent.
#define SYS_FAIL  ((unsigned long)-1)

// A minimal string compare: the freestanding user program has no libc, so this is
// hand-rolled. Returns 1 when the two NUL-terminated strings are equal.
static int str_eq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;   // equal only if both reached '\0' at the same spot
}

// Length of a NUL-terminated string. `write` needs it to tell the kernel how many
// bytes of the line to store, and there is no libc strlen to reach for.
static unsigned long str_len(const char *s) {
    unsigned long n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

// Is `name` expressible as an 8.3 name: a base of 1..8 characters, optionally a
// dot and an extension of 0..3? This mirrors the kernel's name_to_83 acceptance
// rule (lengths only; it does not judge individual characters), and it lives here
// so the shell can tell the user their name is the problem BEFORE the syscall,
// distinguishing the one failure that is their fault and fixable by retyping from
// every other reason a write or delete can fail. Never mangle silently.
static int name_is_83(const char *name) {
    int base = 0;
    int i = 0;
    while (name[i] != '\0' && name[i] != '.') {
        if (++base > 8) {
            return 0;   // base too long
        }
        i++;
    }
    if (base == 0) {
        return 0;       // no base name at all
    }
    if (name[i] == '.') {
        i++;
        int ext = 0;
        while (name[i] != '\0') {
            if (++ext > 3) {
                return 0;   // extension too long
            }
            i++;
        }
    }
    return 1;
}

// Print a small non-negative number in decimal. There is no printf and no libc
// here, and the only way out is sys_write, which takes a string: so the digits
// have to be built by hand. Only exit statuses (0..255) go through this, but the
// buffer is sized for the full range of an unsigned long anyway, because a buffer
// sized for exactly the values you expect today is how this kind of helper gets
// overflowed tomorrow.
//
// The digits come out least-significant first, so they are written backwards from
// the end of the buffer and the pointer to the first one is returned. Note the
// do/while: a plain `while (value)` would print nothing at all for zero, which is
// the single most common status there is.
static void print_uint(unsigned long value) {
    char buf[21];              // 20 digits is the most an unsigned 64-bit value needs, plus '\0'
    char *p = &buf[20];
    *p = '\0';

    do {
        *--p = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0);

    sys_print(p);
}

static void print_help(void) {
    // The command names are TownOS's own and deliberately not the Unix ones.
    sys_print("commands:\n");
    sys_print("  list                 list files in the root directory\n");
    sys_print("  read <file>          print a file's contents\n");
    sys_print("  write <file> <text>  write the rest of the line to a file (creates/replaces)\n");
    sys_print("  delete <file>        delete a file\n");
    sys_print("  free                 how many clusters on the volume are free\n");
    sys_print("  run <file>           run a program and wait for it to finish\n");
    sys_print("  help                 show this list\n");
    sys_print("  clear                clear the screen\n");
    sys_print("  return <text>        print the text back\n");
}

static void cmd_list(void) {
    if (sys_list(list_buf, sizeof(list_buf)) == SYS_FAIL) {
        sys_print("list: could not read the directory\n");
        return;
    }
    // The kernel filled list_buf with newline-separated names, NUL-terminated.
    sys_print(list_buf);
}

static void cmd_read(char *name) {
    if (name == (char *)0) {
        sys_print("read: missing filename\n");
        return;
    }

    // STAT FIRST, then decide. Before this, a missing file, a file too big for the
    // buffer, and a disk error all printed the same "cannot read" line, so a user
    // could not tell which had happened. Asking for the size up front splits the
    // two common cases off with their own messages, and only a genuine read error
    // falls through to the old line.
    unsigned long size = 0;
    if (sys_stat(name, &size) == SYS_FAIL) {
        sys_print("read: no such file: ");
        sys_print(name);
        sys_print("\n");
        return;
    }

    // The buffer holds SHELL_FILE_MAX - 1 bytes of file content (one byte is kept
    // for the NUL appended below before printing). A file larger than that is
    // refused, now WITH BOTH NUMBERS so the reason is unambiguous. There is
    // deliberately no partial read: `read` still delivers the whole file or none of
    // it, it just says why when it declines. Showing a prefix would need an offset
    // argument on SYS_READFILE, which is a rung of its own; see
    // docs/decisions/0021-sys-stat.md. This also retires the old unreachable
    // "showing the first N bytes" notice, which only ever fired for a file of
    // exactly the buffer size, which is complete (TODO(read-truncation), now gone).
    if (size > sizeof(file_buf) - 1) {
        sys_print("read: ");
        sys_print(name);
        sys_print(" is ");
        print_uint(size);
        sys_print(" bytes, the buffer holds ");
        print_uint(sizeof(file_buf) - 1);
        sys_print("\n");
        return;
    }

    // Small enough to fit. The size is known, so this is not the too-large case; a
    // failure here is a genuine disk or filesystem error, which the "cannot read"
    // line now names on its own.
    unsigned long n = sys_readfile(name, file_buf, sizeof(file_buf) - 1);
    if (n == SYS_FAIL) {
        sys_print("read: cannot read ");
        sys_print(name);
        sys_print("\n");
        return;
    }
    // File contents are raw and not NUL-terminated, so terminate before printing.
    file_buf[n] = '\0';
    sys_print(file_buf);
    sys_print("\n");   // the file may not end in a newline; keep the prompt tidy
}

// `write <file> <text>`: store the rest of the line, verbatim, as the file's
// contents. `content` is what next_token left pointing at after the filename: the
// untouched remainder of the line, so every space inside it is preserved and NO
// trailing newline is added — exactly what was typed becomes the file. An empty
// remainder writes a zero-length file. Single-cluster files only, in practice,
// since the line buffer caps a typed line well under one cluster; multi-cluster
// writing is exercised by user/tests/F.c, not by typing.
static void cmd_write(char *name, char *content) {
    if (name == (char *)0) {
        sys_print("write: missing filename\n");
        return;
    }
    if (!name_is_83(name)) {
        // The user's fault and fixable by retyping, so it is called out on its own
        // rather than lumped in with disk failures.
        sys_print("write: ");
        sys_print(name);
        sys_print(" is not an 8.3 name (max 8 chars, dot, 3 chars)\n");
        return;
    }
    if (sys_writefile(name, content, str_len(content)) != 0) {
        sys_print("write: could not write ");
        sys_print(name);
        sys_print("\n");
        return;
    }
}

// `free`: print how many clusters on the volume are free. The leak test leans on
// this: create and delete a file repeatedly and this number must return to exactly
// where it started, because a cluster stranded on any cycle would show up as the
// count drifting down. fat32_free_count recounts the whole FAT, so it is honest
// rather than a cached total that a leak could hide behind.
static void cmd_free(void) {
    print_uint(sys_freecount());
    sys_print(" clusters free\n");
}

// `delete <file>`: remove a file from the disk.
static void cmd_delete(char *name) {
    if (name == (char *)0) {
        sys_print("delete: missing filename\n");
        return;
    }
    if (!name_is_83(name)) {
        sys_print("delete: ");
        sys_print(name);
        sys_print(" is not an 8.3 name (max 8 chars, dot, 3 chars)\n");
        return;
    }
    if (sys_delete(name) != 0) {
        sys_print("delete: could not delete ");
        sys_print(name);
        sys_print("\n");
        return;
    }
}

// The shell's own process group. The shell is task 0 and a group is named after its
// leader, so the shell's group is 0 — the value the foreground starts at, and the
// value it must be restored to every time a job ends.
//
// Named rather than written as a bare 0 at the two call sites, because `sys_setfg(0)`
// reads as "clear the foreground" and it is not: it is "the shell is in front again".
#define SHELL_PGID  0UL

static void cmd_run(char *name) {
    if (name == (char *)0) {
        sys_print("run: missing filename\n");
        return;
    }
    // A JOB OF ITS OWN. Even a single program gets a new process group, so Ctrl-C
    // while it runs is addressed to it and not to this shell. The returned task id is
    // that group's id, because a group is named after the task that leads it.
    unsigned long id = sys_run_group(name, -1, -1, SYS_RUN_GROUP_NEW);
    if (id == SYS_FAIL) {   // -1/-1: a plain run, fresh console, no pipe
        sys_print("run: could not start ");
        sys_print(name);
        sys_print("\n");
        return;
    }
    // The program is now a task of its own and its output interleaves with this
    // shell from the next timer tick on. Announce it before waiting, so the letters
    // that follow are visibly attributed to something that was started.
    sys_print("run: started ");
    sys_print(name);
    sys_print("\n");

    // WAIT FOR IT. This is what makes `run` feel like a command rather than a
    // detach: the prompt does not come back until the program is finished, because
    // this call blocks until it is. Costs no CPU while it waits (see sys_wait).
    //
    // THE CHILD MUST EXIT. There is no way to kill a task and there are no signals,
    // so if the program never calls sys_exit, this shell blocks here forever and the
    // only way back is a reboot. That is why every program in user/ has a bounded
    // loop.
    // Hand the keyboard to the job, wait for it, and take it back. sys_setfg is the
    // declaration that this group is what Ctrl-C means from here until the job ends.
    sys_setfg(id);
    long status = sys_wait();
    sys_setfg(SHELL_PGID);   // unconditional: see the note on SHELL_PGID

    // Any real status is 0..255 (the kernel masks it), so a negative return is the
    // error case and cannot be confused with a program that exited 255. It means the
    // kernel says this shell has no children: the program we just started must have
    // finished AND been reaped before we got here, which today cannot happen because
    // only this task reaps its own children. Report it rather than printing a status
    // that was never returned.
    if (status < 0) {
        sys_print("run: no child to wait for\n");
        return;
    }

    sys_print("run: ");
    sys_print(name);
    sys_print(" exited with status ");
    print_uint((unsigned long)status);
    sys_print("\n");
}

static void cmd_clear(void) {
    for (int i = 0; i < SHELL_CLEAR_LINES; i++) {
        sys_print("\n");
    }
}

// `return <text>`: echo the rest of the line. `rest` is what next_token left
// pointing at after the "return" token: the untouched remainder of the line, so
// internal spaces are preserved. Skip any extra separators between "return" and
// the text so `return   hi` prints `hi`, not `  hi`.
static void cmd_return(char *rest) {
    while (*rest == ' ') {
        rest++;
    }
    sys_print(rest);
    sys_print("\n");
}

// Read one line, building it a keystroke at a time. Returns with `line` holding a
// NUL-terminated string (without the newline). Echoes as it goes so the user sees
// the line forming.
static void read_line(void) {
    unsigned int len = 0;

    for (;;) {
        // Blocks until a key is actually available, so this loop turns exactly once
        // per keystroke and the shell costs nothing while the user is thinking. It
        // used to spin here, calling a non-blocking read over and over and burning
        // every slice it was given; the kernel now sleeps the task instead.
        unsigned long key = sys_readkey();

        // A SIGNAL CUTS THIS CALL SHORT, and that is a real answer, not a key. When a
        // signal is raised on a task blocked in a syscall the kernel wakes it,
        // fails the call with SYS_FAIL, and delivers the handler on the way out
        // (decision 9 of the signals rung) — precisely so the call cannot silently
        // re-issue and swallow the signal. From here that looks like sys_readkey
        // returning -1, and treating it as a character appends 0xFF to the line: the
        // symptom is a Ctrl-C at the prompt leaving invisible junk in the buffer, so
        // the next command typed is rejected as unknown for no visible reason.
        //
        // The handler has already run by the time this returns. Nothing is owed here
        // but to wait for a real key.
        if (key == SYS_FAIL) {
            continue;
        }

        // The handler ran, so throw away whatever had been typed. Checked here rather
        // than in the handler because the count lives in this frame; the handler can
        // only raise the flag.
        if (line_abandoned) {
            line_abandoned = 0;
            len = 0;
        }
        char c = (char)key;

        if (c == '\n') {
            sys_print("\n");   // echo the newline that ends the line
            break;
        }

        if (c == '\b') {
            // Backspace: drop the last character if there is one, and erase it on
            // screen (the screen driver's '\b' rubs out the glyph). Guarding on
            // len > 0 means backspace cannot chew back into the prompt.
            if (len > 0) {
                len--;
                sys_print("\b");
            }
            continue;
        }

        // A printable character. Append it only if the fixed buffer has room (one
        // slot reserved for '\0'); otherwise DROP it rather than overflow. Echo it
        // so the user sees what they type.
        if (len < SHELL_LINE_MAX - 1) {
            line[len++] = c;
            char echo[2] = { c, '\0' };
            sys_print(echo);
        }
    }

    line[len] = '\0';
}

// Does the line contain a '|'? That is what routes it to the pipeline path; a line
// with no '|' takes exactly the single-command path it always has.
static int line_has_pipe(const char *s) {
    while (*s != '\0') {
        if (*s == '|') {
            return 1;
        }
        s++;
    }
    return 0;
}

// Start one pipeline stage. A stage must be `run <file>` — the other commands are
// shell builtins that write to the shell's own console rather than to a child, so
// they cannot be a stage. `in_fd`/`out_fd` are the descriptors the child gets as its
// fd 0 and fd 1 (-1 for a fresh console). Returns the child's task id (>= 1), or -1
// on a parse error or a launch failure. `seg` is tokenized in place.
static long start_segment(char *seg, int in_fd, int out_fd, unsigned long pgid) {
    char *pos = seg;
    char *cmd = next_token(&pos, ' ');
    if (cmd == (char *)0 || !str_eq(cmd, "run")) {
        sys_print("pipe: each stage must be 'run <file>'\n");
        return -1;
    }
    char *name = next_token(&pos, ' ');
    if (name == (char *)0) {
        sys_print("pipe: run needs a filename\n");
        return -1;
    }
    unsigned long ret = sys_run_group(name, in_fd, out_fd, pgid);
    if (ret == SYS_FAIL) {
        sys_print("pipe: could not start ");
        sys_print(name);
        sys_print("\n");
        return -1;
    }
    return (long)ret;
}

// Run a pipeline: `run A | run B | ... `. `line` holds the whole line. Split it on
// '|' into stages, create a pipe between each neighbouring pair, start each stage
// with the right ends, and — the load-bearing part — CLOSE THE SHELL'S OWN COPIES of
// each pipe end the moment the children have theirs. Then wait for every stage and
// report the last one's status (decision 8: the pipeline's status is the last
// stage's, like $?). Building the N-stage loop directly, rather than special-casing
// two stages, keeps the descriptor bookkeeping written once.
static void run_pipeline(void) {
    // Split into stages. next_token shreds `line` in place, so each seg points into
    // it. A '|' with an empty side (`| foo`, `foo |`, `a || b`) yields fewer real
    // tokens, so a pipeline needs at least two.
    char *seg[SHELL_MAX_SEGS];
    int n = 0;
    char *pos = line;
    char *s;
    while ((s = next_token(&pos, '|')) != (char *)0) {
        if (n >= SHELL_MAX_SEGS) {
            sys_print("pipe: too many stages (max 4)\n");
            return;   // nothing opened yet, so nothing to clean up
        }
        seg[n++] = s;
    }
    if (n < 2) {
        sys_print("pipe: each side of | needs a command\n");
        return;
    }

    int in_fd = -1;         // the read end the current stage inherits (from the last pipe)
    long last_id = -1;      // task id of the last stage, whose status is the pipeline's
    int started = 0;        // how many stages actually launched (so we wait for exactly that many)
    unsigned long job_pgid = 0;   // the job's group; 0 until the first stage leads one

    for (int i = 0; i < n; i++) {
        int out_fd = -1;
        int next_in = -1;
        if (i < n - 1) {
            int p[2];
            if (sys_pipe(p) != 0) {
                sys_print("pipe: could not create a pipe\n");
                if (in_fd != -1) {
                    sys_close(in_fd);   // B6: drop the read end we were carrying
                }
                goto drain;
            }
            out_fd = p[1];
            next_in = p[0];
        }

        // EVERY STAGE OF A PIPELINE IS IN ONE GROUP. The first stage asks for a new
        // group and leads it; the rest join the id it was given. That is what makes
        // Ctrl-C reach all three stages of `run a | run b | run c` — they are three
        // tasks and one job, and the group is the only thing that says so.
        long id = start_segment(seg[i], in_fd, out_fd,
                                (job_pgid == 0) ? SYS_RUN_GROUP_NEW : job_pgid);

        // THESE TWO CLOSES ARE LOAD-BEARING (B1). The shell created the pipe, so it
        // holds BOTH ends; once the child has its copies the shell must drop its own,
        // or `writers` never reaches zero, the downstream reader blocks forever on an
        // EOF that cannot arrive, and the shell then blocks in wait for a child that
        // never exits. Both hang, with no output and no error. This is the classic
        // Unix pipe bug. Close after spawning, unconditionally — a failed spawn still
        // needs its ends dropped.
        if (in_fd != -1) {
            sys_close(in_fd);           // the child has its own copy now
        }
        if (out_fd != -1) {
            sys_close(out_fd);
        }
        in_fd = next_in;                // carry the new read end to the next stage

        if (id < 0) {
            // This stage did not start. The read end meant for the NEXT stage is now
            // in in_fd and nothing will consume it, so close it (B6), then reap
            // whatever already started and bail.
            if (in_fd != -1) {
                sys_close(in_fd);
                in_fd = -1;
            }
            goto drain;
        }

        if (job_pgid == 0) {
            job_pgid = (unsigned long)id;   // the first stage leads, and names, the group
        }
        if (i == n - 1) {
            last_id = id;               // the last stage's status is the pipeline's
        }
        started++;
    }

drain:
    // Give the keyboard to the job before waiting for it, so Ctrl-C reaches the
    // pipeline rather than this shell. If no stage started there is no group to hand
    // it to, and the shell simply keeps it.
    if (job_pgid != 0) {
        sys_setfg(job_pgid);
    }

    // Reap every stage that started, so no zombie is left behind, and keep the status
    // of the one whose id matches the last stage.
    {
        long last_status = -1;
        for (int i = 0; i < started; i++) {
            unsigned long who = 0;
            long st = sys_wait_id(&who);
            if (st >= 0 && (long)who == last_id) {
                last_status = st;
            }
        }
        if (last_id >= 0 && last_status >= 0) {
            sys_print("pipeline exited with status ");
            print_uint((unsigned long)last_status);
            sys_print("\n");
        }
    }

    // TAKE THE KEYBOARD BACK, UNCONDITIONALLY. This runs whether the pipeline
    // succeeded, failed to start, or was killed by the Ctrl-C that was addressed to
    // it — every path through this function reaches here, including the `goto drain`
    // ones. A shell that only restores the foreground on success loses the keyboard
    // the first time anything goes wrong, and there is nothing that could give it
    // back: the group it handed control to is gone, so no key can reach anything.
    sys_setfg(SHELL_PGID);
}

// The entry point named by user.ld's ENTRY(_start). The loader takes the entry
// address from the ELF header, so this symbol only has to match the linker script.
// The shell's own SIG_INT handler, and the reason it exists is defensive rather
// than featureful.
//
// Ctrl-C is addressed to the foreground group, and the shell hands that over to
// every job it starts and takes it back afterwards, so in the ordinary case Ctrl-C
// never reaches here at all. But "the ordinary case" is doing a lot of work in that
// sentence: at an idle prompt the foreground IS the shell's group, and if a
// sys_setfg ever failed, or a future edit forgot one, Ctrl-C would arrive here too.
//
// Without a handler the default action applies and the shell — task 0 — is killed.
// Nothing waits on task 0, so there is not even a reap line; the machine simply
// stops responding, with no shell and no way to start one. That is the whole failure
// mode, and it is why this is not optional politeness.
//
// So the shell does what every real shell does with an interrupt at a prompt:
// abandon the line and give a fresh one. The prompt is printed here rather than left
// to the main loop because the loop is blocked inside read_line when this runs, and
// will resume there rather than at the top.
static void on_interrupt(int sig) {
    (void)sig;
    line_abandoned = 1;
    sys_print("\n> ");
}

void _start(void) {
    sys_print("TownOS shell. type 'help'.\n");

    // Install it before the first prompt, so there is no window in which a Ctrl-C
    // would find the shell defenceless.
    sys_signal(SIG_INT, on_interrupt);

    for (;;) {
        sys_print("> ");
        read_line();

        // A line with a '|' is a pipeline; anything else takes exactly the
        // single-command path below, unchanged. The single-command case is NOT routed
        // through the pipeline machinery, so a plain `run a.elf` still behaves exactly
        // as it did before pipes existed.
        if (line_has_pipe(line)) {
            run_pipeline();
            continue;
        }

        // Tokenize IN PLACE: next_token shreds `line`, so the command and any
        // argument point straight into it. `pos` walks along the line.
        char *pos = line;
        char *cmd = next_token(&pos, ' ');

        if (cmd == (char *)0) {
            continue;   // empty line (or only spaces): reprint the prompt
        }

        if (str_eq(cmd, "list")) {
            cmd_list();
        } else if (str_eq(cmd, "read")) {
            cmd_read(next_token(&pos, ' '));
        } else if (str_eq(cmd, "write")) {
            // Extract the filename FIRST, then hand cmd_write the raw remainder as
            // the contents. Two statements, not one call: C leaves argument
            // evaluation order unspecified, and `pos` must be read only after
            // next_token has advanced it past the filename.
            char *wname = next_token(&pos, ' ');
            cmd_write(wname, pos);
        } else if (str_eq(cmd, "delete")) {
            cmd_delete(next_token(&pos, ' '));
        } else if (str_eq(cmd, "free")) {
            cmd_free();
        } else if (str_eq(cmd, "run")) {
            cmd_run(next_token(&pos, ' '));
        } else if (str_eq(cmd, "help")) {
            print_help();
        } else if (str_eq(cmd, "clear")) {
            cmd_clear();
        } else if (str_eq(cmd, "return")) {
            cmd_return(pos);   // the raw remainder after the "return" token
        } else {
            sys_print("unknown command: ");
            sys_print(cmd);
            sys_print("\n");
        }
    }
}
