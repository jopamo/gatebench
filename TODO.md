# gatebench TODO

This file tracks gaps in gatebench coverage (control-plane + datapath).

## Netlink/dump correctness
- Add RTM_GETACTION (dump) checks after create/replace/delete.
- Verify serialized schedule fields: base_time, cycle_time, cycle_time_ext,
  clockid, flags, priority, entry count, entry contents.
- Add negative dump tests (e.g., oversize entry list / ENOSPC handling).

## Concurrency / RCU stress
- Background dump loop while running high-rate replace loop.
- Concurrent delete while replace loop is active (ensure no crashes, no UAF).
- Timer callback race: replace while timer is firing (stress with short intervals).

## Datapath validation
- Minimal qdisc/filter setup to exercise gate open/close behavior.
- Packet generator to validate gate timing and max-octets enforcement.
- Validate drop/accept stats against expected schedule.

## Offload path
- Exercise tcf_gate_offload_act_setup() via flow_action path.
- Validate entry list export and cleanup destructor.

## Parameter coverage
- Explicit cycle_time_ext and flags coverage.
- Clockid variants (REALTIME, MONOTONIC, BOOTTIME, TAI).
- Base time handling: past/future, replace with/without basetime.
- Derived cycle time correctness (sum of intervals) and limits.

## Error-path hardening
- "EINVAL but created" checks for bad_cycletime (mirroring bad_basetime).
- Ensure deletes after failed create/replace do not leave lingering actions.

## Scale / bounds
- Large entry lists (size thresholds for dump sizing).
- Tiny intervals / large intervals (timer precision and overflow behavior).
- Multiple action indices in parallel (index collisions, replace semantics).
