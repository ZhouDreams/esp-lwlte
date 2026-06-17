# AGENTS.md

This file serves as an index for AI coding agents working with this repository.

> **Keep in sync:** `AGENTS.md` (English) and `AGENTS_ZH.md` (Chinese) must always be kept synchronized. Whenever you change one, apply the equivalent change to the other in the same commit.

## Project Overview

esp-lwlte is a self-developed ESP-IDF component library that encapsulates AT command communication logic with LTE modules, currently targeting ESP32-C3 as the host MCU and Shanghai Hezhou Air780EP as the LTE module for development and testing, with plans to support multiple LTE module variants while remaining exclusive to the ESP-IDF platform.

## Document Index

| Topic | Document |
|-------|----------|
| Directory Structure | [docs/agents/directory-structure.md](docs/agents/directory-structure.md) |
| Feature Roadmap | [docs/agents/feature-roadmap.md](docs/agents/feature-roadmap.md) |
| Architecture | [docs/agents/architecture.md](docs/agents/architecture.md) |
| Class Design | [docs/agents/classes.md](docs/agents/classes.md) |
| Build & Debug | [docs/agents/build-and-debug.md](docs/agents/build-and-debug.md) |
| Coding Style & Templates | [docs/agents/coding-style.md](docs/agents/coding-style.md) |
| C OOP Design Guidelines | [docs/agents/oop-design.md](docs/agents/oop-design.md) |
| Error Handling | [docs/agents/err.md](docs/agents/err.md) |
| Air780EP AT Commands & URCs | [docs/agents/at_cmd_air780ep.md](docs/agents/at_cmd_air780ep.md) |
| ML307R AT Commands & URCs | [docs/agents/at_cmd_ml307r.md](docs/agents/at_cmd_ml307r.md) |
| Air780EP CME ERROR Codes | [docs/agents/cme_error_air780ep.md](docs/agents/cme_error_air780ep.md) |

## File Usage Guide

- When writing code, you **MUST** follow [Coding Style & Templates](docs/agents/coding-style.md) and [C OOP Design Guidelines](docs/agents/oop-design.md)
- When building, flashing, or monitoring serial output, you **MUST** follow [Build & Debug](docs/agents/build-and-debug.md)
- [reference/esp-lwlte-old/](reference/esp-lwlte-old/) is an archived legacy project for reference only — all main project work **MUST** follow this file and the guidelines above

## Document Modification Guide

- Do NOT modify this file (AGENTS.md) directly unless changing the index structure
- Update the corresponding file under `docs/agents/` for content changes
- If adding a new topic, create a new file in `docs/agents/` and add the link here
