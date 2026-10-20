# Running the usage tests

The standalone package build registers the ordinary tests by default.
The large-data tier retains the history-over-512-MiB, single-log-over-512-MiB,
retained-cache file-count and real 100001-file regressions. The ordinary usage
test runs the remaining accounting and cache contracts.
Large-data tests are opt-in because they create large histories and filesystem
fixtures. Keep the full-size boundaries when exercising this tier.

To include the large-data tier, configure the existing standalone build with:

```sh
cmake -S . -B <build-dir> -DLOGONOMIC_CLI_ADDON_LARGE_DATA_TESTS=ON
```

Build through the platform's normal build workflow, then run the complete suite:

```sh
ctest --test-dir <build-dir> -C Release --output-on-failure
```

With that option enabled, add `-L large-data` to the CTest command to run only
the large-data tier, or add `-LE large-data` to run only the ordinary tier.
The embedded package build does not register these standalone test suites.
