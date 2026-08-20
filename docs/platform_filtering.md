# IGT Platform Filtering - Usage Guide

## Overview

The IGT platform filtering framework allows tests to be skipped based on
platform characteristics without modifying test source code.

**Key Feature**: **Automatic Filtering** - No manual calls needed in subtests!

Tests only need to initialize platform filtering once in `igt_fixture`,
and the IGT framework automatically checks each subtest before execution.

## Quick Start

### 1. Initialize in Test (One Time)

```c
#include "lib/amdgpu/amd_platform.h"

int igt_main(void)
{
    struct amdgpu_gpu_info gpu_info;

    igt_fixture {
        fd = drm_open_driver(DRIVER_AMDGPU);
        amdgpu_query_gpu_info(device, &gpu_info);

        /* Initialize platform filtering - enables automatic filtering */
        amd_platform_filter_init(&gpu_info);
    }

    igt_subtest("my-test") {
        /* No manual filtering call needed - automatic! */
        test_code();
    }
}
```

### 2. Configure Filtering Rules

Use one of three methods (checked in priority order):

1. **Built-in rules** (highest priority) - Compiled into test
2. **Config file** (recommended) - `/etc/igt/platform_skip.conf`
3. **Environment variable** (lowest priority) - `IGT_PLATFORM_SKIP_CONFIG`

---

## Method 1: Built-in Rules (Highest Priority)

**Location**: Vendor implementation (e.g., `lib/amdgpu/amd_platform.c`)

### Example

```c
static const struct platform_skip_entry amd_builtin_rules[] = {
    {
        .test_name = "amd_security",
        .subtest_glob = "secure-bounce",
        .reason = "Not supported on APUs",
        .platform_data = &apu_platforms,
    },
};
```

### When to Use Built-in Rules

- Permanent production exclusions that should never run
- Platform limitations known at development time
- Tests that fundamentally cannot work on certain hardware
- Vendor-specific restrictions that apply globally

**Trade-offs:**
- [+] Fast (no file I/O at runtime)
- [+] Guaranteed to be applied
- [+] Version controlled with code
- [-] Requires rebuild to change
- [-] Not flexible for temporary exclusions

---

## Method 2: Config File (RECOMMENDED)

**Location**: `/etc/igt/platform_skip.conf`

**Format**: `platform:test:subtest:reason`

**Note**: The reason field is recommended but can be empty. If omitted, "No reason" is automatically inserted.

### Example Config File

```bash
cat > /etc/igt/platform_skip.conf << 'EOF'
# Platform filtering configuration
# Format: platform:test:subtest:reason
# Use * as wildcard

# Skip all UMQ tests on Navi48 during platform bringup
navi48:amd_basic:*-UMQ:SWDEV-88888 - UMQ stabilization in progress

# Skip specific tests with ticket references
navi31:amd_basic:cs-gfx-with-IP-GFX-UMQ:SWDEV-12345
navi31:amd_basic:cs-compute-with-IP-COMPUTE-UMQ:SWDEV-12345
navi31:amd_basic:cs-sdma-with-IP-DMA-UMQ:SWDEV-12346

# Skip tests on early samples
strix_halo:amd_basic:*:Platform not ready - ES samples

# Skip known flaky test across all platforms
*:amd_basic:eviction-test-with-IP-DMA:SWDEV-99999 - Intermittent failure

# Skip entire test binary on specific platform
navi10:amd_vcn:*:VCN encoding issues on Navi10 A0

# Skip all tests on discontinued platform
vega10:*:*:Platform no longer supported
EOF
```

### Run Tests

```bash
# Config file is loaded automatically - no environment variable needed
./build/tests/amdgpu/amd_basic

# Output shows:
# Subtest cs-gfx-with-IP-GFX-UMQ: SKIP
# Platform filtering (config): SWDEV-12345
```

### When to Use Config File

- Managing multiple test exclusions
- Team-wide or CI/CD configurations
- Persistent filtering rules across sessions
- Easy add/remove/edit without rebuilding
- No need to remember environment variables
- Sharing skip rules across test infrastructure

---

## Method 3: Environment Variable (Lowest Priority)

**Variable**: `IGT_PLATFORM_SKIP_CONFIG`

**Format**: Same as config file, semicolon-separated

### Example - Skip Single Test

```bash
export IGT_PLATFORM_SKIP_CONFIG="navi48:amd_basic:cs-compute-with-IP-COMPUTE-UMQ:Testing"
./build/tests/amdgpu/amd_basic
```

### Example - Skip Multiple Tests

```bash
export IGT_PLATFORM_SKIP_CONFIG="navi48:amd_basic:*-UMQ:Testing;navi31:amd_basic:cs-gfx-*:Known issue"
./build/tests/amdgpu/amd_basic
```

### When to Use Environment Variable

- Quick one-off testing without editing files
- Temporary overrides during debugging sessions
- Testing filter patterns before adding to config file
- Single test runs where config file would be overkill
- Overriding config file rules for specific test runs
- CI pipeline job-specific overrides

**Trade-offs:**
- [+] No file editing required
- [+] Quick to set and unset
- [+] Can override config file for testing
- [-] Not persistent (lost when session ends)
- [-] Easy to forget it's set
- [-] Inconvenient for large test lists

---

## Wildcard Patterns

All methods support wildcards (`*`) for flexible matching:

```
# Platform wildcards
*:amd_basic:my-test:Reason           # All platforms
navi*:amd_basic:my-test:Reason       # All Navi (navi10, navi31, navi48, etc.)

# Test wildcards
navi48:amd_*:my-test:Reason          # All amd_* tests

# Subtest wildcards
navi48:amd_basic:*-UMQ:Reason        # All subtests ending with -UMQ
navi48:amd_basic:cs-*:Reason         # All subtests starting with cs-

# Combined wildcards
*:*:*-UMQ:Reason                     # All UMQ tests on all platforms
```

### Wildcard Examples

```bash
# Skip all user queue tests during bringup
navi48:*:*-UMQ:Platform bringup - UMQ not ready

# Skip all VCN tests on specific platform
navi10:amd_vcn:*:Known VCN issues

# Skip specific test pattern across all platforms
*:amd_basic:eviction-*:Intermittent failures

# Skip all tests on EOL platform
vega10:*:*:Platform no longer supported
```

### Pattern Matching Rules

- `*` matches zero or more characters
- Patterns are case-sensitive
- Empty field is NOT a wildcard (use `*` explicitly)
- Priority order: Built-in > Config file > Environment variable

### Example Subtest Matching

Given config: `navi48:amd_basic:*-UMQ:Testing`

- cs-gfx-with-IP-GFX-UMQ → **Skipped** (matches)
- cs-compute-with-IP-COMPUTE-UMQ → **Skipped** (matches)
- cs-sdma-with-IP-DMA → Runs normally (doesn't match)

---

## Skip Message Format

Automatic filtering shows the source in skip messages:

```
Subtest cs-gfx-with-IP-GFX-UMQ: SKIP
Platform filtering (config): SWDEV-12345 - UMQ unstable on Navi48
```

Source indicators:
- `(built-in)` - From vendor's compiled rules
- `(config)` - From config file
- `(env)` - From environment variable

---

## Platform Names

Platform names are vendor-specific and defined in vendor backend code.

**For AMD platforms**, see `lib/amdgpu/amd_platform.c` for the complete list.

**Examples:**
- RDNA2/3 discrete GPUs: `navi21`, `navi31`, `navi33`
- RDNA4: `navi48`, `navi44`
- APUs: `strix_halo`, `phoenix`
- Data center: `aldebaran` (MI250)

**To find your platform name:**

```bash
# Method 1: Run any test with debug output
./build/tests/amdgpu/amd_basic --debug
# Look for: "Platform detected: <name>"

# Method 2: Check dmesg for GPU info
dmesg | grep -i amdgpu | grep -i asic

# Method 3: Check /sys
cat /sys/class/drm/card0/device/asic_name  # If available
```

**Note:** Platform names may change with new hardware generations. Check vendor backend code for current platform definitions. The list above is provided as examples only.

---

## Best Practices

### For Development - Quick Testing

Use environment variable:
```bash
# Temporarily skip broken test
export IGT_PLATFORM_SKIP_CONFIG="*:amd_basic:broken-test:WIP"
./build/tests/amdgpu/amd_basic
unset IGT_PLATFORM_SKIP_CONFIG
```

### For Teams - Shared Exclusions

Use config file with ticket references:
```
# /etc/igt/platform_skip.conf
# Updated: 2026-07-09

# Navi48 bringup exclusions
navi48:amd_basic:*-UMQ:SWDEV-88888 - UMQ stabilization
navi48:amd_vcn:vcn-encoder-*:SWDEV-88889 - VCN bringup

# Cross-platform known issues
*:amd_basic:eviction-test-with-IP-DMA:SWDEV-77777 - Flaky
```

### For CI/CD

Deploy config file with test infrastructure:
```bash
#!/bin/bash
# CI pipeline setup
echo "Deploying test exclusions..."
scp ci-skip-rules.conf test-machine:/etc/igt/platform_skip.conf
ssh test-machine "./run-igt-suite.sh"
```

### For Production

Use built-in rules for permanent exclusions:
```c
// In lib/amdgpu/amd_platform.c
static const struct platform_skip_entry amd_builtin_rules[] = {
    {
        .test_name = "amd_basic",
        .subtest_glob = "*-UMQ",
        .reason = "User queues not supported in production",
        .platform_data = &all_platforms,
    },
};
```

---

## Summary

| Method | Use Case | Persistent | Rebuild Required |
|--------|----------|------------|------------------|
| **Built-in** | Production rules | Yes | Yes |
| **Config file** | Team/CI exclusions | Yes | No |
| **Environment** | Quick testing | No | No |

**Recommendation**: Use **config file** for managing test exclusions at scale.

---

## See Also

- `lib/igt_platform_filter.h` - API documentation
- `lib/igt_platform_filter.c` - Framework implementation
- `lib/amdgpu/amd_platform.c` - AMD backend reference
