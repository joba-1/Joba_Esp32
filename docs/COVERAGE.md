# Coverage generation

This project uses lcov/genhtml (gcov) to generate C/C++ unit-test coverage reports.

Requirements (local):
- lcov + genhtml
- gcov / gcc toolchain
- platformio (for running unit tests)
- optional: `gcovr` to generate Cobertura XML

Install on Debian/Ubuntu:

```bash
sudo apt update
sudo apt install -y lcov genhtml gcovr build-essential python3-pip
pip3 install platformio
```

Run coverage locally (default PlatformIO env `native`):

```bash
./misc/coverage/run_coverage.sh [env]
# Example:
./misc/coverage/run_coverage.sh native
```

Outputs:
- `coverage/html/index.html` — interactive HTML report
- `coverage/coverage.info.filtered` — lcov info file
- `coverage/cobertura-coverage.xml` — Cobertura XML (if `gcovr` installed)

CI:
- A GitHub Action is provided at `.github/workflows/coverage.yml` which runs the same script and uploads artifacts.
- To publish to Codecov set the `CODECOV_TOKEN` secret in the repository settings.

Notes:
- The script filters out `/usr/*`, `.platformio` and `test/` sources by default. Adjust `misc/coverage/run_coverage.sh` or `.lcovrc` if you need different filters.
- If your PlatformIO test environment has a different name than `native`, pass it as the first argument to the script.
