# Contributing

Run the offline regression suite for changes to the driver, provider or shared encoding. A test using ordinary memory in place of a BAR establishes byte layout, not real MMIO transaction behavior. Keep hardware claims separate from simulated test results.

Keep the source tree free of firmware images, Apple kernel collections, SDK archives, credentials and machine-specific logs. Use explicit runtime arguments and fictional test identities instead of personal paths, private hostnames or real network addresses.

Retain copyright notices and document the origin and license of externally sourced material. Contributions to project code use the repository's Apache License, Version 2.0.

For performance changes, include the exact measurement boundary, initiator, payload size, queue depth, repetitions, raw numeric traces and correctness gate; report regressions and tails as well as medians.
