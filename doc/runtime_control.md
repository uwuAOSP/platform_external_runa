<!--
  SPDX-FileCopyrightText: The uwuAOSP Project
  SPDX-License-Identifier: Apache-2.0
-->

# Runa runtime control

Runa can expose a local Unix stream socket with `--control-socket FILE`. The
socket is mode `0600`, is serviced by the build thread, and is removed when
Runa exits normally. Runa refuses to replace a pre-existing socket path.

Each connection carries one newline-terminated request and receives one
newline-terminated response. The MVP commands are:

```text
set_parallelism N
cancel_action_for_retry
get_status
```

`set_parallelism` changes only the number of new actions Runa may start. It
does not cancel actions when the new value is below the current running count.

`cancel_action_for_retry` chooses the retryable running action with the largest
process-group RSS when that information is available. It returns
`ok state=accepted` after signaling the process group. Runa then waits for
termination, cleans the attempt, and puts the same edge back on the ready
queue. A rejection or a later build failure must be treated by the controller
as a failed recovery attempt.

`get_status` reports the original and effective parallelism, running and
retrying action counts, and cumulative successful action count. A controller
can record the successful count when recovery begins and use the delta as a
recovery signal.

## Retry contract

Ordinary build edges are retryable by default. A rule or build edge can
explicitly opt out with:

```ninja
runa_retryable = false
```

`runa_retry_class` and `runa_retry_cleanup` remain optional metadata. Runa
always removes declared outputs, the depfile, and the rspfile before
requeueing. `runa_retry_cleanup` is an optional whitespace-separated list of
additional files relative to the edge scope. Paths containing whitespace are
not supported by this MVP contract.

Opt-in promises that:

- repeating the command has no externally visible side effects;
- all partial persistent state is covered by the automatic cleanup set or
  `runa_retry_cleanup`;
- child processes remain in the action process group;
- the command does not publish graph structure while it is running.

Runa rejects console, generator, phony-output, and dyndep-related edges even if
they opt in. A canceled attempt is emitted to the existing frontend protocol
with `EdgeFinished.canceled = true`; it is not recorded in `.ninja_log` or
`.ninja_deps`.
