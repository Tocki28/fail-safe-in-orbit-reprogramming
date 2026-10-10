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

1. **ConOps:** The need for the system, and its normal and failure-mode
   operational behavior.
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

- **Traceability:** Every fact and number carries a page-level source citation.
- **Open Items:** Explicitly tracked via TBD (unknown values) and TBR (proposals
  not yet accepted).
- **Fault Coverage:** Every protection documents how it could itself fail, and
  what else fails at the same time from the same cause (common-cause failure).
- **Audit Trail:** Immutable revision history; corrections are appended, never
  rewritten.
- **Hardware Validation:** "Done" means measured on physical hardware.
  Untested protections are unproven.

## How We Use AI

AI can now do the analysis at every phase. What it cannot do is be answerable
for the result: every cell in the last column is a risk a person accepts.

| Phase | The question we ask | How AI helps answer it | The risk the engineer accepts |
|---|---|---|---|
| 1. ConOps | Why do we need this system, and how will it be used, normally and when it fails? | Asks the questions, proposes failure scenarios from NASA's hazard-cause list, writes the document in NASA's outline. | The need, every capability and every scenario. |
| 2. Software Classification | How bad is each part's worst failure, and how much rigor does it need? | Matches each part against the class definitions in NPR 7150.2D, quoting the clause. | Each class, and so the rigor each part gets. |
| 3. Hazard Analysis | What can go wrong, and what stops it? | Drafts the fault tree and failure-mode table. Walks NASA's hazard-cause list item by item. Asks what could defeat each protection and what fails with it. A separate AI session then hunts for gaps. | The hazard list and each severity. |
| 4. Requirements | What exactly must the system do? | Turns each hazard and scenario into numbered, single-claim statements. Checks them against NASA's requirement-writing checklist. Traces each to its source. | Every requirement and every number. |
| 5. Architecture | Which design meets the requirements with the fewest ways to fail? | Lays out the alternatives as a trade study. Reads datasheets and quotes hardware facts with page numbers. | The chosen design over the alternatives. |
| 6. Interfaces | What crosses each boundary, byte by byte? | Drafts the message layouts and the compile-time size checks that enforce them. | The interfaces, before any code. |
| 7. Verification Plan | How will each requirement be proven? | Proposes a method and a pass criterion per requirement. Flags any requirement that cannot be verified. | The plan, including any requirement left unverified. |
| 8. Implementation | Does the code do what the requirements say, and nothing else? | Writes one small subsystem at a time, from its requirements and interface, under the coding rules it is given. States the reason for each decision in comments. Drafts the tests separately from the code. | Splits the system into the smallest subsystems. Chooses the coding rules (e.g. JPL's "Power of Ten"). Evaluates each subsystem against them, then accepts it or sends it back. |
| 9. Verification | Does the system work on hardware, including under injected faults? | Writes the test and fault-injection scripts. Tabulates the results. | The results as evidence, or not. |

At every phase:

- When a claim can be checked by running something, the AI runs it. When it
  cannot, the AI states what would settle it.
- The AI reads only the material that belongs to the task.
- The AI publishes, pushes or sends nothing without the engineer's go.

## License

MIT. See [LICENSE](LICENSE).
