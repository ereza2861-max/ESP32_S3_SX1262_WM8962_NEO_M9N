# Patch Artifact Goal

## 1. Purpose

This directory contains patch artifacts that represent controlled repository transformations.

The primary purpose of this artifact set is to preserve the **technical lineage, intent, and implementation history** of previously established repository states while allowing subsequent patches to introduce additional fixes, normalization, feature completion, and architectural decisions.

Patch generation MUST therefore be treated as a controlled state transition rather than as an opportunity to reconstruct the repository from scratch.

The patch workflow MUST preserve:

* previously established valid behavior;
* previously resolved architectural decisions;
* security invariants;
* protocol invariants;
* hardware assumptions;
* configuration semantics;
* persistence semantics;
* runtime behavior;
* compatibility requirements that are explicitly supported by repository evidence;
* traceability between historical and current repository states.

---

## 2. Historical Patch Artifacts

Files matching:

```text
old-patch-artifact<index>.patch
```

are historical patch artifacts.

They represent previously generated repository transformations and MUST be treated as **archived implementation history**, not as disposable temporary files.

Historical patch artifacts exist to provide:

1. implementation lineage;
2. historical context;
3. evidence of previously applied transformations;
4. protection against accidental architectural regression;
5. traceability between repository states;
6. preservation of previously resolved decisions;
7. a reference when determining whether a proposed change is genuinely new or merely a reimplementation of an earlier change.

Historical artifacts MUST NOT be deleted, rewritten, or silently replaced merely because a newer patch exists.

---

## 3. Historical Artifact Integrity

Historical patch artifacts are immutable reference material unless an explicit repository-maintenance decision states otherwise.

A new patch MUST NOT modify the contents of historical patch artifacts as part of normal implementation work.

A new patch MUST also avoid:

* reversing a previously established valid change without explicit evidence;
* silently restoring code that a previous patch intentionally removed;
* silently removing behavior introduced by a previous patch;
* reintroducing a previously fixed defect;
* changing a previously resolved architectural decision without an explicit new decision;
* replacing a previous implementation with an incompatible alternative merely because the new implementation is simpler.

If historical implementation appears incorrect, the new audit MUST identify the discrepancy explicitly and classify it as one of:

```text
DETERMINISTIC BUG
ARCHITECTURAL DECISION GAP
REQUIREMENT GAP
STALE ARTIFACT
UNVERIFIED BEHAVIOR
INTENTIONAL CHANGE
```

The historical artifact itself remains preserved.

---

## 4. Patch as a State Transition

Every new patch MUST be understood as a transition:

```text
Repository State N
        │
        │
        ▼
   New Patch
        │
        ▼
Repository State N+1
```

The patch MUST describe the minimum repository transformation necessary to move the current state to the intended state.

The patch MUST NOT assume that the repository can be safely reconstructed from historical patches alone.

The current repository is the primary implementation state.

Historical patches provide lineage and context.

Therefore:

```text
CURRENT REPOSITORY
        >
HISTORICAL PATCH ARTIFACTS
        >
HISTORICAL DOCUMENTATION
```

for determining the actual current implementation.

However, historical artifacts MUST still be inspected whenever they are relevant to determining the intent, chronology, or invariants of an existing implementation.

---

## 5. Baseline Preservation

Before generating a new patch, the implementation process MUST establish the effective baseline.

The baseline consists of:

* current source code;
* current configuration;
* current build configuration;
* current persistence model;
* current API behavior;
* current UI behavior;
* current security controls;
* current protocol behavior;
* current hardware mappings;
* current tests;
* current documentation;
* previously resolved architectural decisions;
* relevant historical patch artifacts.

The new patch MUST be generated against the actual current repository state.

It MUST NOT be generated against an assumed or reconstructed historical state.

---

## 6. Decision Preservation

Previously resolved architectural decisions are part of the repository's effective design constraints.

A new patch MUST NOT silently reopen, reverse, or reinterpret an existing decision.

Examples include, but are not limited to:

* configuration ownership;
* runtime configuration snapshot semantics;
* persistence ownership;
* NVS schema;
* credential storage hierarchy;
* authentication policy;
* rate-limiting policy;
* protocol semantics;
* transport security requirements;
* hardware pin assignments;
* ADC allocation;
* strapping-pin usage;
* build-stack selection;
* partition layout;
* security-feature configuration;
* reboot versus runtime-apply semantics.

If a new audit discovers that a previous decision is technically inconsistent with newly discovered repository evidence, the implementation MUST NOT silently change it.

Instead:

1. identify the inconsistency;
2. document the affected decision;
3. identify the technical consequences;
4. determine whether the change is deterministic or architectural;
5. require an explicit decision when the change is architectural;
6. implement the new decision only after it has been explicitly selected.

---

## 7. No Decision Backward Drift

A new patch MUST NOT introduce **decision backward drift**.

Decision backward drift occurs when a newer patch unintentionally causes the repository to move from a previously resolved state back toward:

* an earlier architecture;
* an abandoned implementation;
* a previously rejected behavior;
* duplicated ownership;
* inconsistent configuration access;
* obsolete persistence semantics;
* insecure credential handling;
* obsolete protocol behavior;
* undocumented compatibility behavior;
* previously removed technical debt.

For example, if a previous patch established a single mutation owner for runtime configuration, a later patch MUST NOT reintroduce direct mutation of the same state from unrelated modules.

Similarly, if a previous patch established a snapshot-based reader contract, a later patch MUST NOT silently restore uncontrolled direct access to mutable global configuration.

---

## 8. Patch Scope

Every new patch MUST have an explicit technical scope.

The patch SHOULD contain only changes required to:

* fix deterministic defects;
* close explicitly selected decision gaps;
* normalize repository artifacts;
* complete required functionality;
* enforce documented security requirements;
* restore consistency between implementation and repository artifacts;
* add or correct required validation;
* establish required recovery behavior;
* preserve architectural invariants.

Unrelated refactoring MUST NOT be included merely because the affected code is encountered during implementation.

Large-scale restructuring MUST require explicit technical justification.

---

## 9. Deterministic Fixes

A deterministic fix is a correction whose intended behavior can be established directly from repository evidence without requiring an architectural choice.

Examples include:

* incorrect conditionals;
* unreachable required code paths;
* missing null/error handling;
* incorrect bounds checking;
* inconsistent validation;
* obvious state-machine errors;
* incorrect serialization/deserialization;
* missing cleanup;
* incorrect resource ownership;
* stale documentation that directly contradicts confirmed implementation;
* missing include/dependency required by existing code;
* build configuration inconsistencies whose intended value is already established elsewhere.

Deterministic fixes SHOULD be applied without waiting for a user decision.

They MUST remain minimal and localized.

---

## 10. Architectural Changes

An architectural change MUST NOT be silently embedded inside a patch.

A change is architectural when it alters, for example:

* ownership;
* lifecycle;
* concurrency model;
* persistence semantics;
* security boundaries;
* authentication policy;
* protocol semantics;
* hardware allocation;
* public API contracts;
* configuration architecture;
* recovery strategy;
* build-stack assumptions;
* compatibility policy.

Such changes MUST first be represented as explicit decision items when the repository does not already establish the intended behavior.

The patch MUST implement only the selected decision.

---

## 11. Repository Consistency

A patch is not considered complete merely because source code compiles.

The implementation and repository artifacts MUST remain consistent across applicable layers:

```text
Firmware
   │
   ├── Configuration
   ├── Persistence / NVS
   ├── Runtime State
   ├── API
   ├── UI
   ├── Security
   ├── Protocol
   ├── Hardware
   ├── Build System
   ├── Tests
   ├── Provisioning
   ├── Scripts
   └── Documentation
```

When behavior changes, all affected repository artifacts MUST be evaluated.

A patch MUST NOT leave contradictory representations such as:

```text
Firmware implements X
Documentation describes Y
UI exposes Z
Persistence stores X
Runtime applies nothing
```

unless the difference is explicitly intentional and documented.

---

## 12. Patch Completeness

The final patch MUST contain every implementation change required for the selected scope.

It MUST NOT contain:

* placeholders;
* pseudo-code;
* omitted changes;
* truncated files;
* invented paths;
* invented symbols;
* invented APIs;
* fabricated line numbers;
* changes that were not actually implemented;
* claims unsupported by repository evidence.

If a required change cannot be implemented safely, it MUST remain explicitly identified as an unresolved gap rather than being represented as completed.

---

## 13. Unified Diff Requirement

The final implementation artifact MUST be a valid Git Unified Diff.

The patch MUST use standard paths:

```text
a/<path>
b/<path>
```

For example:

```text
diff --git a/src/example.cpp b/src/example.cpp
```

The patch MUST be compatible with:

```bash
git apply <patch-file>
```

The patch SHOULD be generated from the actual repository diff whenever possible.

The patch MUST NOT be manually fabricated from assumptions about line numbers or file contents.

---

## 14. Patch Applicability

Before declaring a patch complete, the implementation process SHOULD verify:

```bash
git apply --check <patch-file>
```

When practical, the patch SHOULD also be applied to a clean matching baseline and verified again.

The patch must correspond to the repository state for which it was generated.

A patch that is logically correct but cannot be applied to its intended baseline is not a complete patch artifact.

---

## 15. Non-Regression Requirements

Every new patch MUST be evaluated for regression against the established baseline.

At minimum, review for regression in:

* configuration ownership;
* runtime state;
* persistence;
* initialization;
* shutdown;
* error handling;
* concurrency;
* memory ownership;
* resource lifetime;
* security boundaries;
* authentication;
* authorization;
* credential handling;
* network behavior;
* protocol behavior;
* hardware pin assignments;
* timing-sensitive functionality;
* build configuration;
* test assumptions.

A new implementation MUST NOT be considered successful merely because the newly targeted feature works.

It must also avoid breaking previously valid behavior.

---

## 16. Security Preservation

Security-related behavior MUST be treated as an invariant unless an explicit security decision changes it.

New patches MUST NOT unintentionally weaken:

* credential confidentiality;
* credential lifecycle handling;
* authentication;
* authorization;
* transport security;
* TLS requirements;
* rate limiting;
* replay protection;
* input validation;
* memory safety;
* secure storage;
* provisioning security;
* device identity;
* secure boot assumptions;
* flash-encryption assumptions.

If a patch changes a security boundary, the change MUST be explicitly identified.

---

## 17. Hardware Preservation

Hardware-related changes require additional caution.

A patch MUST NOT silently change:

* GPIO assignment;
* ADC allocation;
* strapping-pin behavior;
* boot-critical pins;
* peripheral routing;
* interrupt assumptions;
* UART/SPI/I2C ownership;
* PWM channels;
* RTC-capable requirements;
* GPIO Matrix assumptions;
* external hardware polarity;
* power-control behavior.

If the PCB or hardware has not yet been fabricated, repository evidence MAY establish the intended design, but it MUST NOT be represented as physically validated hardware behavior.

Hardware validation and source-level correctness are separate evidence categories.

---

## 18. Verification Classification

Verification results MUST distinguish between:

```text
IMPLEMENTED
VERIFIED
NOT VERIFIED
NOT APPLICABLE
BLOCKED
```

In particular:

```text
NOT VERIFIED != NOT IMPLEMENTED
```

Examples:

A source-level implementation may be:

```text
IMPLEMENTED
NOT VERIFIED
```

when physical hardware validation has not yet occurred.

A manufacturing security state may remain:

```text
NOT VERIFIED
```

even when the firmware contains the required configuration.

The patch artifact MUST NOT convert absence of physical evidence into a PASS claim.

---

## 19. Historical Patch Relationship

When creating a new patch, inspect relevant historical patch artifacts to determine whether the proposed change:

1. extends a previous implementation;
2. completes an incomplete previous implementation;
3. fixes a regression;
4. normalizes a previously introduced change;
5. replaces an obsolete implementation;
6. intentionally changes an existing architectural decision.

The result MUST be classified explicitly.

A new patch MUST NOT duplicate a historical transformation unnecessarily.

For example, if an older patch already introduced:

```text
ConfigurationManager
```

a newer patch should extend or correct that implementation rather than creating a second independent configuration-management mechanism.

---

## 20. Patch Lineage

Where useful, implementation analysis SHOULD establish lineage such as:

```text
Historical Patch
      │
      ▼
Current Repository State
      │
      ▼
New Audit Finding
      │
      ▼
New Technical Decision
      │
      ▼
New Patch
      │
      ▼
Verified Repository State
```

This lineage allows future audits to determine:

* why a change exists;
* which decision authorized it;
* which historical implementation it superseded;
* which invariants it was intended to preserve;
* whether a later change accidentally regressed it.

---

## 21. Artifact Preservation Rule

Historical patch artifacts MUST remain available unless there is a separate explicit repository-maintenance decision to remove them.

The existence of a newer patch does not invalidate the historical patch.

The archive therefore acts as a technical record rather than merely a backup.

The intended repository structure is conceptually:

```text
patch artifact/
├── artifact-goal.md
├── old-patch-artifact1.patch
├── old-patch-artifact2.patch
├── old-patch-artifact3.patch
└── ...
```

The exact filenames and numbering MUST follow the actual repository state.

No filename MUST be invented merely to satisfy this documentation.

---

## 22. Failure Handling

If patch generation encounters an ambiguity, missing dependency, contradictory artifact, or insufficient evidence, the process MUST NOT silently guess.

The issue MUST be classified as one of:

```text
INSUFFICIENT EVIDENCE
CONTRADICTORY REQUIREMENT
ARCHITECTURAL DECISION REQUIRED
IMPLEMENTATION DEFECT
ENVIRONMENT LIMITATION
HARDWARE VALIDATION REQUIRED
```

The patch MUST contain only changes that can be justified by available evidence and explicit decisions.

---

## 23. Definition of Done

A new patch artifact is considered technically complete only when all applicable conditions below are satisfied:

* the target repository state has been audited;
* relevant historical patch artifacts have been considered;
* all meaningful gaps within scope have been identified;
* deterministic fixes have been implemented;
* required architectural decisions have been explicitly resolved;
* selected decisions have been implemented;
* affected repository artifacts have been normalized;
* security invariants have been reviewed;
* hardware assumptions have been reviewed;
* non-regression has been evaluated;
* applicable tests/build checks have been executed;
* unverified physical behavior remains explicitly classified as unverified;
* the final patch contains all implementation changes;
* no placeholder or fabricated change remains;
* the patch uses standard `a/` and `b/` paths;
* the patch is applicable to the intended repository baseline;
* historical patch artifacts remain preserved;
* no previously resolved decision has been silently reversed.

---

## 24. Core Principle

The objective of this directory is not simply to store old patch files.

Its purpose is to maintain **controlled technical evolution of the repository**.

The governing principle is:

```text
PRESERVE VALID HISTORY
        ↓
AUDIT CURRENT STATE
        ↓
IDENTIFY ACTUAL GAPS
        ↓
SEPARATE DETERMINISTIC FIXES
FROM ARCHITECTURAL DECISIONS
        ↓
PRESERVE EXISTING DECISIONS
        ↓
APPLY ONLY JUSTIFIED CHANGES
        ↓
VERIFY
        ↓
RE-AUDIT
        ↓
PRODUCE TRACEABLE PATCH
```

A new patch MUST move the repository forward from its actual current state.

It MUST NOT accidentally move the repository backward toward an obsolete state.

Historical patch artifacts therefore serve as **implementation lineage and regression-prevention evidence**, while the current repository remains the authoritative implementation baseline.

The final repository state must remain internally coherent, technically traceable, reproducible, and defensible from the available implementation evidence and explicitly recorded engineering decisions.
