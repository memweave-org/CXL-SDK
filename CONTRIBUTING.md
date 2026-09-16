# Contributing to CXL-SDK

Thank you for helping improve CXL-SDK. The project combines original systems
code with adapted research artifacts, so small, well-documented changes are the
easiest to review and reproduce.

## Before you start

- Search existing issues and pull requests before opening a duplicate.
- Use an issue to discuss changes that alter public APIs, persistent layouts,
  shared-memory protocols, or benchmark methodology.
- Do not include confidential traces, credentials, proprietary data, or code
  whose license is incompatible with this repository.

## Development workflow

1. Fork the repository and create a focused branch from `master`.
2. Keep third-party changes separate from CXL-SDK-owned changes when possible.
3. Add or update tests and documentation with the implementation.
4. Run the builds and tests relevant to your change. The commands used by CI
   are recorded in `.github/workflows/build-and-test.yml`.
5. Describe hardware, kernel, NUMA topology, CXL device, compiler, and workload
   assumptions needed to reproduce experimental results.

## Commit and pull-request guidance

- Write imperative, scoped commit messages such as `fix: handle failed shared
  mapping` or `docs: clarify allocator configuration`.
- Keep generated files and build artifacts out of commits.
- Explain what changed, why it changed, user-visible impact, and validation.
- Mark behavior or performance changes explicitly and include before/after data
  when making performance claims.
- Confirm that new dependencies and copied code include their license and
  attribution information alongside the component.

## Testing expectations

At minimum, build the component you changed. For runtime and data-structure
changes, also run the YCSB-C file-backed smoke test used by CI. Hardware-specific
tests may be skipped when the required platform is unavailable, but the pull
request must state what was not run and why.

Additional component-specific commands are documented in the
[developer guide](https://memweave-org.github.io/docs/zh/developer-guide.html).

## Reporting security issues

Do not open public issues for vulnerabilities. Use GitHub's
[private vulnerability reporting](https://github.com/memweave-org/CXL-SDK/security/advisories/new).
