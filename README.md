# AGENT_CPP

`AGENT_CPP` is a C++ trading-agent / execution-infrastructure repository designed for low-level market connectivity, exchange integration, and operational trading logic.

The goal of this repo is not to be a research notebook or a strategy playground.  
Its goal is to provide a more execution-oriented and systems-oriented layer, suitable for robust crypto trading infrastructure.

## Scope

This repository is intended to support components such as:
- exchange connectivity
- market data ingestion
- order management
- execution logic
- runtime configuration
- operational monitoring hooks
- multi-exchange adapter structure

From the current repository structure, the project appears to support configurable exchange-specific setups, including examples for:
- Binance
- Kraken

## Repository structure

```text
AGENT_CPP/
├── build/
├── src/
├── third_party/
├── config.example.binance.json
├── config.example.kraken.json
├── config.example.json
└── README.md
```

### Directory roles

- `src/`  
  Main C++ source code for the agent, connectors, execution logic, and runtime components.

- `build/`  
  Local build artifacts. This directory should generally remain out of version control unless explicitly needed.

- `third_party/`  
  External libraries, vendored dependencies, or submodules required by the project.

- `config.example.json`  
  Generic example runtime configuration.

- `config.example.binance.json`  
  Example configuration for Binance connectivity.

- `config.example.kraken.json`  
  Example configuration for Kraken connectivity.

## Design philosophy

This repo should be treated as an infrastructure layer, not as a place for loose experimentation.

The intended philosophy is:
- explicit configuration
- deterministic runtime behavior where possible
- clean separation between exchange-specific and core logic
- operational robustness over convenience
- production-minded systems design
- no hidden assumptions in execution behavior

In practice, this means:
- credentials should never be hardcoded
- exchange-specific conventions should be isolated
- logs, errors, and failure states should be explicit
- runtime behavior should be configurable rather than patched ad hoc
- local build outputs should not pollute the repository history

## Configuration

Configuration is expected to be provided through JSON files.

Example files already present:
- `config.example.json`
- `config.example.binance.json`
- `config.example.kraken.json`

A typical workflow is:
1. copy an example config
2. create a local private runtime config
3. fill in exchange credentials, endpoints, symbols, and runtime parameters
4. launch the agent with that local config

For example:

```bash
cp config.example.binance.json config.local.json
```

or:

```bash
cp config.example.kraken.json config.local.json
```

Then edit `config.local.json` with your actual parameters.

## Security rules

This repository must follow strict operational security rules:
- never commit real API keys
- never commit private secrets
- never commit production credentials
- never commit local runtime configs containing sensitive information

Only example configs should be tracked.

Sensitive runtime configs should stay local or be injected through secure deployment workflows.

## Build philosophy

This is a C++ repository, so the build system should remain explicit and reproducible.

General expectations:
- out-of-source builds preferred
- compiler and dependency assumptions should be documented
- third-party dependencies should be controlled carefully
- debug and release behavior should be clearly separated

If the project uses CMake, a typical local workflow would look like:

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

If the actual build system differs, this section should be updated to match the real commands used by the repo.

## Runtime goal

The purpose of the runtime agent is typically to manage one or more of the following:
- connect to exchange APIs
- stream or poll market data
- maintain internal state
- generate execution decisions from upstream logic
- submit, amend, or cancel orders
- log operational events
- fail loudly when invariants are broken

This kind of repository is part of a broader trading stack, not a standalone proof of alpha.

## What this repo is not

This repo is not:
- a research notebook
- a backtest-only environment
- a discretionary trading dashboard
- a retail signal bot
- a place to store large logs or dumps
- a substitute for full execution risk controls

It is an engineering repository for execution-oriented trading infrastructure.

## Git hygiene

The repository should track:
- source code
- build configuration
- example configs
- lightweight documentation
- dependency declarations
- test files if present

The repository should not track:
- local build outputs
- private configs
- credentials
- temporary logs
- crash dumps
- large runtime artifacts

Typical items that should stay out of Git:
- `build/`
- `*.log`
- `config.local.json`
- secrets files
- local debug dumps

## Suggested `.gitignore` principles

Typical exclusions for this kind of repo include:

```gitignore
build/
*.log
*.tmp
*.swp
.DS_Store
config.local.json
secrets.json
```

This should be adjusted to the actual project conventions.

## Operational standard

A trading agent is only useful if it is operationally trustworthy.

That means:
- explicit configuration
- controlled failure modes
- predictable startup behavior
- clear logging
- clean exchange abstraction
- no hidden credentials
- no silent runtime drift

The standard should be closer to execution infrastructure than to research code.

## Status

Current visible status from the repository structure:
- C++ project initialized
- source tree present
- third-party area present
- exchange example configs present
- repository ready for documentation and build standardization

