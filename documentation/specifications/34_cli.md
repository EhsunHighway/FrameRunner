# Module 34 - CLI Core

**Files:** `src/cli/cli.c`, `src/cli/cli.h`
**Status:** Ready for implementation; source files are currently empty
**Depends on:** `simulator`, `animation`

## Concepts First

The CLI core is a reusable read-evaluate loop and command registry. It knows
how to tokenize a writable input line, select the longest registered command
name, invoke its handler, and manage the active simulator. It does not know the
meaning of `ping`, `show route`, or other built-in commands; those belong to
Module 35.

A multiword registry requires longest-prefix selection. Given registered names
`show` and `show ip route`, the input `show ip route R1` selects the three-word
name and passes only `R1` to its handler.

## Purpose

The module provides CLI state, registration, command lookup, line execution,
the REPL loop, and atomic replacement of the active simulator/animation pair.

It does not render topology, parse topology files, implement commands, inspect
protocol-private data, or execute scheduler internals directly.

## Data Model

```c
#define CLI_MAX_ARGS     16
#define CLI_LINE_BUF     256
#define CLI_PROMPT       "sim> "
#define CLI_MAX_COMMANDS 64
#define CLI_CMD_NAME_LEN 32

typedef struct CliState CliState;
typedef int (*CliHandler)(CliState *state, int argc, char **argv);

typedef struct CliCommand {
    char        name[CLI_CMD_NAME_LEN];
    CliHandler  handler;
    const char *usage;
} CliCommand;

struct CliState {
    CliCommand cmds[CLI_MAX_COMMANDS];
    int        cmd_count;

    Simulator      *sim;
    AnimationState *animation;
    FILE           *in;
    FILE           *out;
    int             running;
    int             watch_enabled;
};
```

The command name is copied. `usage`, input stream, and output stream are
borrowed. CLI state owns the active simulator and animation.

## Ownership And Lifetime

- `cli_create` takes simulator ownership only after animation creation
  succeeds.
- `cli_free` frees animation before simulator, then frees CLI state.
- Command handlers borrow `CliState` and argument pointers for the duration of
  one call.
- `cli_exec_line` tokenizes its caller-owned writable line buffer in place.
- `cli_replace_simulator` takes ownership of the replacement only on success;
  on failure the caller still owns it and the current simulator remains active.

## Public API

```c
CliState *cli_create(Simulator *sim, FILE *in, FILE *out);
void      cli_free(CliState *state);

int cli_register(CliState   *state,
                 const char *name,
                 CliHandler  handler,
                 const char *usage);

const CliCommand *cli_find_command(const CliState *state,
                                   int             argc,
                                   char          **argv,
                                   int            *matched_words);

int cli_exec_line(CliState *state, char *line);
int cli_loop(CliState *state);

int cli_replace_simulator(CliState *state, Simulator *replacement);
```

Command return values are `0` for success, `1` for usage error, and `-1` for
runtime error.

## Function Behavior

### `cli_create`

1. If `sim == NULL || in == NULL || out == NULL`, return `NULL` without taking
   ownership.
2. Allocate and zero one `CliState`; on failure return `NULL`.
3. Create `AnimationState` borrowing `sim`, `sim->trace`, and `out` with the
   initial terminal dimensions.
4. If animation creation fails, free only CLI state and return `NULL`; the
   caller still owns `sim`.
5. Store simulator, animation, and borrowed streams. Simulator ownership now
   transfers.
6. Set command count to zero, running to one, and watch enabled only when the
   output is an interactive terminal.
7. Return state. Do not register built-in commands here.

### `cli_free`

1. If state is null, return.
2. Free owned animation.
3. Free owned simulator.
4. Free CLI state; do not close either borrowed stream or free usage strings.

### `cli_register`

1. Reject null state/name/handler/usage with `-1`.
2. Reject an empty name, a name that does not fit including NUL, leading or
   trailing whitespace, repeated whitespace, or command-table capacity.
3. Scan existing commands and reject an exact duplicate name.
4. Copy the name into the next slot, store handler and borrowed usage, then
   increment command count.
5. Return `0`; every rejected path leaves the registry unchanged.

### `cli_find_command`

1. Set `*matched_words = 0` when that output pointer is non-null.
2. Reject null state/argv/matched output, nonpositive argc, or invalid command
   count with `NULL`.
3. Scan registered commands in increasing slot order.
4. Split each registered name logically on single spaces and compare its words
   with the leading input arguments. Ignore a command needing more words than
   `argc`.
5. Retain the matching command with the greatest word count. Equal word counts
   cannot represent different exact prefixes because duplicate names are
   rejected.
6. Store the retained word count and return the borrowed command pointer, or
   return `NULL` with zero words when none matches.

### `cli_exec_line`

1. Reject null state or line with `-1`.
2. Remove one trailing newline and optional preceding carriage return.
3. Skip leading whitespace. Return `0` for an empty line or a line whose first
   nonspace byte is `#`.
4. Tokenize in place on spaces and tabs into at most `CLI_MAX_ARGS`. If another
   token exists, print an argument-limit error and return `1`.
5. Call `cli_find_command`. If no command matches, print an unknown-command
   diagnostic and return `1`.
6. Call the handler with `argc - matched_words` and
   `argv + matched_words`; command-name words are not handler arguments.
7. If the handler returns `1`, print its registered usage once.
8. Return the handler result unchanged.

### `cli_loop`

1. Reject a null state or borrowed stream with `-1`.
2. Set running to one.
3. While running is nonzero, print and flush the prompt only for interactive
   input, then read one bounded line.
4. On EOF, leave the loop normally. On input error, return `-1`.
5. If a physical line fills the buffer without a newline and input has not
   reached EOF, consume the rest of that line, print a line-too-long error, and
   continue without executing its prefix.
6. Call `cli_exec_line`. A command result does not terminate the REPL unless a
   handler changes `state->running`.
7. Return `0` after EOF or explicit exit.

### `cli_replace_simulator`

1. Reject null state, replacement, output stream, or replacement trace with
   `-1`; caller retains replacement.
2. Create a new animation for replacement using the current terminal
   dimensions, output stream, playback speed, watch setting, and focus defaults.
3. If creation fails, return `-1`; preserve current state and caller ownership
   of replacement.
4. Save old animation and simulator pointers, then publish replacement and its
   animation together.
5. Free old animation before old simulator.
6. Return `0`; CLI now owns replacement.

## Flow

```text
line -> tokenize -> longest registered prefix -> handler arguments -> result

replacement simulator -> create replacement animation
                      -> publish pair
                      -> free old animation
                      -> free old simulator
```

## ACSL Contract Targets

Contracts must cover command-count bounds, null rejection, unchanged registry
on failed registration, borrowed lookup results within the command array,
in-place line mutation, and atomic simulator replacement ownership.

Use literal bounds `16`, `256`, `64`, and `32` inside ACSL comments.

## KLEVA Verification Plan

Tests cover allocation failure, empty registry, capacity, invalid names,
duplicate names, longest-prefix matching, excessive arguments, comments,
CRLF, missing newline, handler argument slicing, usage printing, EOF, input
failure, and simulator replacement success/failure ownership.

## Common Mistakes

- Implementing built-in command meaning in `cli.c`.
- Passing command-name words to the handler as arguments.
- Selecting the first matching prefix instead of the longest.
- Publishing a replacement simulator before its animation exists.
- Freeing a borrowed stream or usage string.
