# Cloud compiling with GitHub Actions

The ESP32-S3 firmware can be compiled in GitHub Actions; a local PlatformIO installation is not required for the cloud build.

## From the GitHub web interface

Open the repository's **Actions** tab and select **ESP32-S3 PlatformIO Build**. A build is started automatically for pushes and pull requests targeting `main`, and it can also be started with **Run workflow**.

After the run finishes, open the run's **Artifacts** section:

- `esp32-s3-firmware-<commit>` contains the firmware binaries and related build files when compilation succeeds.
- `esp32-s3-build-log-<commit>` contains `build.log` and `ci-context.txt`. This artifact is uploaded even when compilation fails, so compiler errors can be downloaded without installing PlatformIO locally.

A failed compile intentionally does not publish the firmware artifact, preventing a partial or stale binary from being mistaken for a valid build.

## Optional command-line download

The Makefile also supports downloading artifacts from an Actions run. This requires GitHub CLI (`gh`) and an authenticated GitHub CLI session.

```text
make download-build-log
make download-artifacts
make download-ci
```

When a commit has multiple runs, pass the exact Actions run ID:

```text
make download-build-log CI_RUN_ID=123456789
make download-artifacts CI_RUN_ID=123456789
```

`make download-ci` downloads the compile log first. If the build failed and therefore has no firmware artifact, the log remains available and the command reports that the firmware artifact is unavailable.

## Security and reproducibility

The workflow does not need a personal access token. Checkout uses the workflow token with read-only repository contents and does not persist credentials into the Git configuration.

The cloud build uses the same PlatformIO/Makefile build entry point as a local build. The workflow pins the Espressif platform version through `platformio.ini`; PlatformIO and its libraries may still resolve according to the dependency constraints in that file.
