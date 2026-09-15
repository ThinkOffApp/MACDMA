# Driver package

`mcdma driver install` installs the MCDMA driver from a package in this folder:

- `manifest.json` — version, UUID, requirements, tool list, SHA-256 hashes
- `mcdma-driver-<version>.tar.gz` — `MCDMACX5Native.kext/`, `libmcdma-rdmav34.so`,
  `mcdma.driver`, `tools/` (Mac tools) and `tools/linux-arm64/verbs-peer`

Neither file is committed: the kernel extension is a binary built and signed
for one machine's security policy. Build the package from a Mac that already
has the driver installed:

```sh
MAC=<ssh host> BUILD_DIR=<dir of the built tools on that Mac> SPARK=<ssh host> npm run package
```

The driver source lives in the MCDMA repository; `tools/make-driver-package.sh`
only collects the installed files over ssh. Any folder with the same layout
can be selected with `mcdma settings set driverPackage <dir>`.
