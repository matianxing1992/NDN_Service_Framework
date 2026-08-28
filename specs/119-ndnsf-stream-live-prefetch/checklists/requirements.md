# Specification Quality Checklist: NDNSF Stream Live Prefetch

**Purpose**: Validate specification completeness before planning

**Created**: 2026-07-17

**Feature**: [spec.md](../spec.md)

## Content Quality

- [x] Focuses on user-visible behavior and system outcomes
- [x] Keeps implementation choices out of success criteria
- [x] Completes every mandatory section
- [x] Separates reusable cursor/Mapping/prefetch behavior from UAV naming and media policy

## Requirement Completeness

- [x] No clarification markers remain
- [x] Requirements are testable and unambiguous
- [x] Success criteria are measurable
- [x] Acceptance scenarios cover primary flows
- [x] Edge cases include cache, timing, security, restart, and resource bounds
- [x] Dependencies and assumptions are explicit

## Feature Readiness

- [x] Every user story has an independent acceptance path
- [x] Original semantic-name, Mapping integrity, and security invariants are explicit
- [x] Paper-derived mechanisms are distinguishable from project adaptations
- [x] Negative performance results have an explicit retention/default rule
- [x] Fixed cursor-to-block lookup, single-Data wire cap, immutable
  overprediction, aggregate Interest budget, and Provider-side future-hit
  evidence are executable contracts.
- [x] Checkpoint forward/backward continuity and joint retained payload/Mapping-
  chain availability make latest- and beginning-mode bootstrap executable.
- [x] Three matched Core controls isolate live policy and future pending; pair
  inputs/order and non-vacuous future-hit gates are frozen, while Mapping cost
  is measured directly from bytes and Interest share.
- [x] No APP-owned direct-name oracle duplicates Core validation or scheduling;
  the future-off policy remains on the same public Mapping wire and API.
- [x] Public content is opaque, encryption is outside Core, and optional FEC is
  default-off, bounded, Provider/digest-verified, and independently testable.

## Notes

The paper's selector bootstrap is replaced by an authenticated descriptor and
signed Mapping frontier. The Mapping itself is an NDNSF adaptation; the paper
uses directly constructible sequential payload names. Current UAV live-edge
timing uses the measured publication/FEC-group period, not an unproven codec
frame/FPS equivalence. Future-prefetch credit requires the payload Interest to
arrive at the Provider before production; late Mapping is measured, not
explained away.
