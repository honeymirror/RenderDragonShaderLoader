# Native IO regression fixture

Small fixture used by `tools/run_native_io_regression.ps1`.

- `packs/native-io-source-fixture.zip` is the reduced, non-player source fixture
  used to exercise cache-hit attribute mapping.
- `good-src/` contains the minimal ColorPostProcessing sources needed to build
  the canonical candidate and the synthetic buffer/pass probe.

The script creates timestamps, compiled outputs, and invalid mapping variants
only under `build/native-io-regression`; none of those generated files belong
in source control.
