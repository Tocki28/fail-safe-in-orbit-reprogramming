# Remote Controller with Fail-Safe Update

An onboard computer that commands, monitors and updates the firmware of a
microcontroller it controls. It is built the way critical avionics software is
built: start from what happens when it fails, write each step down before the
next one starts, and prove every protection by causing the fault it guards
against.

This is a bench demonstration. It is neither flight hardware nor flight-qualified
software.

## The problem

A controller in space cannot be reached to plug in a cable. A failed firmware
update can leave it unable to run any firmware and unable to accept new
firmware. Without physical access, that state is permanent. Everything in this
repository exists to prevent that outcome, and to show the evidence that it is
prevented.

## How it is built

A normal project starts from features. A critical project starts from the
question "what is the worst thing that can happen, and how do I prove it
can't?" The steps below follow the NASA systems engineering lifecycle
(NASA/SP-2016-6105 Rev 2) and NASA's software standards (NPR 7150.2D,
NASA-STD-8739.8B). Each step produces one document before the next step starts.

| Step | Question it answers | Status |
|---|---|---|
| 1. Concept of Operations | How is the system used, normally and when things fail? | Draft: [ConOps](docs/conops/ConOps_Remote-Controller-Fail-Safe-Update_Iss1_Rev0_DRAFT.pdf) |
| 2. Software classification | How bad is each part's worst failure, and so how much rigour does it get? | Not started |
| 3. Hazard analysis | What can go wrong? Top-down from the worst outcome, bottom-up part by part. | Not started |
| 4. Requirements | What exactly must the system do? Numbered, one claim each, each with how it is proven. | Not started |
| 5. Architecture | How is it built to meet the requirements? | Not started |
| 6. Interfaces | What crosses each boundary, byte by byte? Fixed before any code. | Not started |
| 7. Verification plan | How is each requirement proven: test, analysis, inspection or demonstration? | Not started |
| 8. Code | Written under coding rules, with every warning treated as an error and static analysis on every build. | Not started |
| 9. Verification | Unit tests, then integration, then hardware, then deliberate faults. Every result recorded. | Not started |

The Concept of Operations is, in NASA's words, "the overall high-level concept
of how the system will be used to meet stakeholder expectations, usually in a
time sequenced manner" (NASA/SP-2016-6105 Rev 2, p. 51). It comes first because
it describes behaviour before any design exists.

## Rules the work follows

- Every number and every fact carries its source, down to the page.
- No assumption stands until the engineer accepts it. Open items stay visible in
  the documents: TBD for a value not yet known, TBR for a proposal not yet
  accepted. Each document counts them on its cover.
- Every hazard protection records two extra things: what could make the
  protection itself fail, and what else fails at the same time from the same
  cause.
- Revisions are numbered. History is never rewritten; corrections are added.
- "Done" means measured on the hardware.
- A protection never seen to work against its fault is unproven.

## How AI is used

This project is built with an AI assistant (Claude, by Anthropic).

- The engineer decides. The AI proposes, drafts and explains. A proposal the
  engineer has not accepted is marked TBR and does not count as a decision.
- The AI drafts documents and code. The engineer reviews them and must be able
  to explain every line.
- The AI reads standards and datasheets. Every fact it brings back carries a
  page reference so it can be checked against the source.
- The AI is not the evidence. The evidence is what the hardware, the tests and
  the analysis tools show.

## Hardware

| Role | Board |
|---|---|
| Onboard computer (Linux) | STM32MP257F-DK |
| Controller | NUCLEO-G431KB |

## Standards used

Cited by number and page. The documents themselves are not copied here.

- NASA/SP-2016-6105 Rev 2, *NASA Systems Engineering Handbook* (2016):
  lifecycle order; ConOps outline (App. S).
- NPR 7150.2D, *NASA Software Engineering Requirements*: software classes;
  safety-critical software behaviour.
- NASA-STD-8739.8B, *Software Assurance and Software Safety Standard* (2022):
  software causes of hazards (App. A, Table 2).

## Licence

TBD.

## Author

Andrew Toma
