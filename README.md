# Failsafe Remote Controller

A bench-top demonstration of an onboard computer that monitors, commands and
updates a remote embedded controller: telemetry, commanding, fault handling and
fail-safe firmware updates. It follows the methods used for critical avionics
software, with one central goal: a failed update must never brick the
controller.

> **Note:** This is a lab prototype, neither flight hardware nor
> flight-qualified software.

## Hardware Stack

- **Onboard Computer (Linux):** STM32MP257F-DK
- **Target Controller:** NUCLEO-G431KB

## Engineering Lifecycle

Developed following NASA systems engineering and software standards
(NASA/SP-2016-6105 Rev 2, NPR 7150.2D, NASA-STD-8739.8B). Each phase produces
its document before the next phase starts. A phase is revisited when later work
changes it: NASA applies these processes "iteratively and recursively"
(NASA/SP-2016-6105 Rev 2, p. 7).

1. **ConOps:** Normal and failure-mode operational behavior.
   **Draft:** [Issue 1, Rev 0](docs/conops/ConOps_Remote-Controller-Fail-Safe-Update_Iss1_Rev0_DRAFT.pdf)
2. **Software Classification:** Failure severity and required rigor.
   *Not started.*
3. **Hazard Analysis:** Top-down and bottom-up fault identification.
   *Not started.*
4. **Requirements:** Traceable, testable, single-claim rules.
   *Not started.*
5. **Architecture:** System-level design.
   *Not started.*
6. **Interfaces:** Byte-level communication specs (fixed before coding).
   *Not started.*
7. **Verification Plan:** Test, analysis, inspection, and demonstration
   methods. *Not started.*
8. **Implementation:** Strict compilation rules, zero warnings, mandatory
   static analysis. *Not started.*
9. **Verification:** Unit tests, integration, hardware-in-the-loop, and
   deliberate fault injection. *Not started.*

## Core Rules

- **Traceability:** Every fact and number includes a source citation (with
  page numbers).
- **Open Items:** Tracked explicitly via TBD (unknown values) and TBR (pending
  proposals).
- **Fault Coverage:** Every safety mechanism documents what could make it fail,
  and what else fails at the same time from the same cause (common-cause
  failure).
- **Audit Trail:** Immutable revision histories; corrections are appended,
  never rewritten.
- **Hardware Validation:** "Done" means verified on physical hardware.
  Untested protections are unproven.

## How We Use AI

These rules apply to all of our engineering work with AI, not only this
repository. The assistant is Claude, by Anthropic.

- **Decisions:** The engineer decides. The AI asks, proposes options and
  explains them. It does not choose.
- **Assumptions:** Nothing the AI assumes stands until the engineer accepts it.
  Open proposals stay marked TBR; acceptances are recorded.
- **Sources:** Every fact the AI brings carries its source and exact location
  (page, table, equation). A fact without one is treated as unverified.
- **Measure, Don't Assert:** When a claim can be checked by running something,
  the AI runs it instead of reasoning about it. When it cannot, the AI states
  what would settle it.
- **Scope:** The AI reads only the material that belongs to the task. Similar
  material from another project is never used as a source.
- **Authorship:** The AI may draft documents and code. Nothing is published
  that the engineer cannot explain line by line.
- **Evidence:** Proof comes from test, analysis, inspection or demonstration.
  The AI's own statement is never evidence.
- **Actions:** The AI publishes, pushes or sends nothing without the
  engineer's explicit go.

## License

MIT. See [LICENSE](LICENSE).
