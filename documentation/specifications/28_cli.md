# Module 26 — CLI (Command Line Interface)

**Files:** `src/cli/cli.c`, `src/cli/cli.h`,
           `src/cli/commands.c`, `src/cli/commands.h`
**Status:** ⬜ Not started
**Depends on:** simulator, topology, topology_view, header_view, ip, arp, route_table

---

## The Problem

After building a simulated network, users need an interactive way to:

- Inspect state (`show interfaces`, `show arp`, `show ip route`).
- Inject events (`ping`, `traceroute`).
- Modify the topology (`set link down`, `add route`).
- Control the simulation (`run`, `step`, `stop`, `set time`).

The CLI reads lines from `stdin` (or a script file), parses them into a
command name + argument list, dispatches to a registered handler
function, and prints results. It is the **only module allowed to call
`printf` / interact with the terminal** — all other display modules take
a `FILE *out` parameter.

## Mental Model

```
   User/Script
       │
       ▼ line input
   cli_readline("sim> ")
       │
       ▼ tokenize
   ["show", "arp", "R1"]
       │
       ▼ dispatch
   cmd_table["show arp"] → cmd_show_arp(sim, argc, argv)
       │
       ▼ output
   arp_cache_print(&R1->arp_cache, stdout)
```

---

## Header File — `cli.h`

### Constants

| Macro                  | Value   | Use                              |
|------------------------|---------|----------------------------------|
| `CLI_MAX_ARGS`         | `16`    | Max tokens per command line      |
| `CLI_LINE_BUF`         | `256`   | Input line buffer                |
| `CLI_PROMPT`           | `"sim> "`| Displayed before each input     |
| `CLI_MAX_COMMANDS`     | `64`    | Command table capacity           |

### `CliCommand` Struct (32 bytes)

```c
typedef struct CliCommand {
    char  name[24];          // e.g. "show arp" — space-separated prefix
    int (*handler)(Simulator *sim, int argc, char **argv);
    char  usage[0];          // flexible array of usage string (in commands.c)
} CliCommand;
```

Simplified (no FAM):

```c
typedef struct CliCommand {
    char  name[24];
    int (*handler)(Simulator *sim, int argc, char **argv);
    const char *usage;       // points to a static string
} CliCommand;                // 40 bytes
```

### `CliState` Struct (2 688 bytes)

```c
typedef struct CliState {
    CliCommand  cmds[64];    // 64 × 40  = 2 560 B
    int         cmd_count;
    Simulator  *sim;
    int         running;     // 1 while in cli_loop
    FILE       *in;          // input source (stdin or script)
    FILE       *out;         // output sink  (stdout or log)
} CliState;                  // ≈ 2 600 B
```

### Public API

| Function                                     | Purpose                                    |
|----------------------------------------------|--------------------------------------------|
| `cli_init(sim, in, out)`                     | Create CliState; register built-in commands.|
| `cli_free`                                   | Release.                                   |
| `cli_register(state, name, handler, usage)`  | Add a command.                             |
| `cli_loop(state)`                            | Read-eval loop until EOF or `exit`.        |
| `cli_exec_line(state, line)`                 | Parse + dispatch one line.                 |

---

## Header File — `commands.h`

All handler functions declared here:

```c
int cmd_show_interfaces(Simulator *sim, int argc, char **argv);
int cmd_show_arp       (Simulator *sim, int argc, char **argv);
int cmd_show_route     (Simulator *sim, int argc, char **argv);
int cmd_show_topology  (Simulator *sim, int argc, char **argv);
int cmd_ping           (Simulator *sim, int argc, char **argv);
int cmd_set_link       (Simulator *sim, int argc, char **argv);
int cmd_add_route      (Simulator *sim, int argc, char **argv);
int cmd_run            (Simulator *sim, int argc, char **argv);
int cmd_step           (Simulator *sim, int argc, char **argv);
int cmd_stop           (Simulator *sim, int argc, char **argv);
int cmd_set_time       (Simulator *sim, int argc, char **argv);
int cmd_help           (Simulator *sim, int argc, char **argv);
int cmd_exit           (Simulator *sim, int argc, char **argv);
```

---

## Built-in Command Table

| Command name         | Handler                | Usage                                   |
|----------------------|------------------------|-----------------------------------------|
| `show interfaces`    | `cmd_show_interfaces`  | `show interfaces [device]`              |
| `show arp`           | `cmd_show_arp`         | `show arp [device]`                     |
| `show ip route`      | `cmd_show_route`       | `show ip route [device]`                |
| `show topology`      | `cmd_show_topology`    | `show topology`                         |
| `ping`               | `cmd_ping`             | `ping <src_device> <dst_ip> [count]`    |
| `set link`           | `cmd_set_link`         | `set link <dev>:<iface> up\|down`       |
| `add route`          | `cmd_add_route`        | `add route <dev> <prefix/len> <nh>`     |
| `run`                | `cmd_run`              | `run [end_time_ms]`                     |
| `step`               | `cmd_step`             | `step [n]`                              |
| `stop`               | `cmd_stop`             | `stop`                                  |
| `set time`           | `cmd_set_time`         | `set time <end_ms>`                     |
| `help`               | `cmd_help`             | `help [command]`                        |
| `exit`               | `cmd_exit`             | `exit`                                  |

---

## Function Call Sequence — `cli_loop`

```
cli_loop(state):
   │
   │   while state->running && !feof(state->in):
   │       fprintf(state->out, CLI_PROMPT)
   │       fgets(line, CLI_LINE_BUF, state->in)
   │
   │       cli_exec_line(state, line):
   │           │
   │           │   strip trailing \n
   │           │   tokenize: argv[0..argc-1] = strtok(line, " \t")
   │           │
   │           │   find cmd: linear scan of cmds[] for name match
   │           │       (longest match: "show arp" before "show")
   │           │
   │           └─► cmd->handler(state->sim, argc, argv)
   │                   │
   │                   │   e.g. cmd_show_arp:
   │                   │       dev = topology_find_device_by_name(sim->topo, argv[2])
   │                   │       arp_cache_print(&dev->arp_cache, state->out)
   │                   └─► return 0
```

## Function Call Sequence — `ping`

```
cmd_ping(sim, argc, argv):
   │   argv[1] = "H1"   argv[2] = "192.168.1.20"   argv[3] = "5" (count)
   │
   │   src_dev = topology_find_device_by_name(sim->topo, "H1")
   │   dst_ip  = inet_aton("192.168.1.20")
   │   src_ip  = src_dev->interfaces[0]->ip_addr
   │
   │   for i in 0..count:
   │       icmp_send_echo_request(sim, src_ip, dst_ip, id=1, seq=i, NULL, 0)
   │       simulator_run(sim)     ← run until queue empty (or echo reply received)
   │       print RTT or "Request timed out"
```

---

## Design Notes

- **Longest-prefix dispatch**: `show arp R1` must match `"show arp"`
  not `"show"`. Sort `cmds[]` by `strlen(name)` descending at init,
  or compare multi-word commands before single-word ones.
- **Script mode**: `cli_init(sim, script_file, stdout)` replays a text
  file of commands — useful for automated test scenarios.
- **`cli_exec_line` tokenizes in-place** — `strtok` modifies the line
  buffer. The caller must not reuse `line` after this call.
- **Handler return codes**: 0 = success, 1 = usage error (print usage
  string), -1 = runtime error (command performed but failed).
- **`cmd_exit` sets `state->running = 0`** — the loop exits cleanly
  after the current line returns.

## Test Plan (kleva)

- `exec_line_dispatches_correct_handler`
- `exec_line_unknown_command_prints_error`
- `show_arp_prints_cache_entries`
- `show_topology_calls_topology_view`
- `set_link_down_disables_interface`
- `add_route_installs_static_route`
- `exit_sets_running_zero`
- NULL guards: `exec_line_null_state`, `exec_line_null_line`
