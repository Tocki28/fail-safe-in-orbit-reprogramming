# Fail-Safe In-Orbit Reprogramming of a Spacecraft Controller

A bench-top demonstration of an onboard computer that monitors, commands and
reprograms a spacecraft controller: telemetry, commanding, fault handling and
fail-safe in-orbit reprogramming. It follows the methods used for critical avionics
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

1. **ConOps:** What the system is for and why it is needed, and its normal and
   failure-mode operational behavior.
   **Draft:** [Issue 1, Rev 1](docs/conops/ConOps_Fail-Safe-In-Orbit-Reprogramming_Iss1_Rev1_DRAFT.pdf)
   (supersedes [Rev 0](docs/conops/ConOps_Fail-Safe-In-Orbit-Reprogramming_Iss1_Rev0_DRAFT.pdf))
2. **Software Classification:** Failure severity and required rigor.
   **Draft:** [Issue 1, Rev 1](docs/classification/Software-Classification_Fail-Safe-In-Orbit-Reprogramming_Iss1_Rev1_DRAFT.pdf)
   (supersedes [Rev 0](docs/classification/Software-Classification_Fail-Safe-In-Orbit-Reprogramming_Iss1_Rev0_DRAFT.pdf))
3. **Hazard Analysis:** Top-down and bottom-up fault identification.
   **Draft:** [Issue 1, Rev 1](docs/hazard-analysis/Hazard-Analysis_Fail-Safe-In-Orbit-Reprogramming_Iss1_Rev1_DRAFT.pdf)
   (supersedes [Rev 0](docs/hazard-analysis/Hazard-Analysis_Fail-Safe-In-Orbit-Reprogramming_Iss1_Rev0_DRAFT.pdf))
4. **Requirements:** Traceable, testable, single-claim rules.
   **Draft:** [Issue 1, Rev 0](docs/requirements/Software-Requirements_Fail-Safe-In-Orbit-Reprogramming_Iss1_Rev0_DRAFT.pdf)
5. **Architecture:** System-level design.
   **Draft:** [Issue 1, Rev 0](docs/architecture/Software-Architecture_Fail-Safe-In-Orbit-Reprogramming_Iss1_Rev0_DRAFT.pdf)
6. **Interfaces:** Byte-level communication specs (fixed before coding).
   **Draft:** [Issue 1, Rev 0](docs/interfaces/Interface-Design_Fail-Safe-In-Orbit-Reprogramming_Iss1_Rev0_DRAFT.pdf)
7. **Verification Plan:** Test, analysis, inspection, and demonstration
   methods.
   **Draft:** [Issue 1, Rev 0](docs/verification-plan/Verification-Plan_Fail-Safe-In-Orbit-Reprogramming_Iss1_Rev0_DRAFT.pdf)
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

AI performs the analysis across every phase of development. What it cannot do
is assume accountability: **every entry in the right-hand column represents a
risk explicitly accepted by a human engineer.**

| Phase | The Question We Ask | How AI Helps Answer It | The Risk the Engineer Accepts |
|---|---|---|---|
| **1. ConOps** | What is the system for, why do we need it, and how will it be used normally and during failures? | Asks what problem the system solves and for whom, and proposes a single need statement. Then generates operational questions, proposes failure scenarios using NASA hazard lists, and drafts the document in NASA's standard outline. | Accepts the system's purpose and need, all defining capabilities, and every failure scenario. |
| **2. Software Classification** | How severe is each component's worst-case failure, and how much rigor does it require? | Maps system components against NPR 7150.2D class definitions and cites the relevant clauses. | Formally assigns software classes and commits to the required engineering rigor. |
| **3. Hazard Analysis** | What can go wrong, and what mechanisms prevent it? | Drafts fault trees and failure-mode tables by evaluating NASA's hazard-cause lists. A separate AI session audits for gaps, protection defeats, and common-cause failures. | Accepts the hazard list, severity classifications, and mitigation completeness. |
| **4. Requirements** | What exact, unambiguous rules must the system obey? | Converts hazards and scenarios into numbered, single-claim statements, checks them against NASA checklists, and traces them to sources. | Approves every requirement statement, threshold, and numerical value. |
| **5. Architecture** | Which design satisfies requirements with the fewest single points of failure? | Constructs trade studies evaluating design alternatives using page-cited datasheet specs. | Selects and defends the chosen architecture over all alternatives. |
| **6. Interfaces** | What crosses each boundary, byte by byte? | Drafts message layouts and writes the compile-time size checks that enforce them. | Approves interface specifications prior to writing implementation code. |
| **7. Verification Plan** | How will each requirement be proven? | Maps each requirement to a verification method and pass criterion, flagging any requirement that cannot be verified. | Accepts the verification plan, including the risk of any requirement left unverified. |
| **8. Implementation** | Does the code do what requirements specify, and nothing else? | Implements one small subsystem at a time from its specs and the coding rules it is given (e.g., JPL's *The Power of Ten*), commenting the rationale. Drafts unit tests separately from the code. | Splits the system into the smallest subsystems, establishes coding standards, evaluates each subsystem against them, and approves or rejects generated code. |
| **9. Verification** | Does the system perform on hardware, including under injected faults? | Generates test and fault-injection scripts, then tabulates execution results. | Verifies test execution on physical hardware and accepts the results as valid evidence. |

### Execution Principles

- **Verification First:** If a claim can be tested by running code, the AI
  executes it. Otherwise, it states what is required to verify it.
- **Scoped Context:** The AI reads only the material relevant to the active
  task.
- **Human-in-the-Loop:** The AI publishes, commits, or pushes nothing without
  explicit engineer approval.

## License

MIT. See [LICENSE](LICENSE).
