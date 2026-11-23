# Deadlines, faults and evidence

Every asynchronous operation in Dannan, and in every system of the stack, has the same shape: its
arguments, then a **deadline**, the caller's **executor**, and **done**.

## The rules

- **No wait without a deadline.** Every call, push and wait says how long it may take.
- **Never inline.** `done` is posted to the caller's executor, even when the answer is already known
  (dannan::contract::answer_now). A caller never finds its handler running inside its own call, with
  its own locks held.
- **Exactly once.** The answer or the deadline, whichever comes first; never both, never neither
  (dannan::contract::Completion).
- **A read that finds nothing succeeds** with an empty value. Failure means the operation could not
  happen.

## Outcome and Fault

An answer is a dannan::contract::Outcome: the value, or a dannan::contract::Fault and a message. The
kinds are one list for the whole stack, so a caller branches the same way whichever system answered:

| Fault | Meaning | What a caller usually does |
|---|---|---|
| not_found | what was named does not exist | treat as absent |
| conflict | not at the version expected | read again, then retry |
| duplicate | the name is taken | choose another |
| invalid | the input, or a reply, is malformed | fix the caller |
| refused | understood and not allowed | stop |
| unavailable | could not be reached, nothing changed | retry, maybe elsewhere |
| timed_out | the deadline passed; a write may yet land | read back before retrying a write |
| failed | failed, and nothing changed | report |
| cancelled | the caller cancelled; a write may yet land | as timed_out |

Why `timed_out` and `cancelled` say "may yet land": the request may have reached the far side and
been applied after the caller stopped waiting. A caller that retries a write must make it safe to
apply twice, or read first.

## Evidence

A failure may carry **evidence** beside its fault: what the operation knows that the caller needs.
A process run that timed out returns the output so far; a write that timed out may say which
replicas took it. Something that happened on more than one node is never a bare boolean.

## Where to read on

- dannan::contract::Outcome, dannan::contract::Fault, dannan::contract::Completion.

## The example

examples/deadlines.cpp, built and run with the tests:

\include deadlines.cpp
