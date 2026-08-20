// SPDX-License-Identifier: MIT
// Copyright 2026 Advanced Micro Devices, Inc.
/*
 * Generic platform-based test filtering framework
 *
 * This is a vendor-agnostic filtering system. Vendor-specific logic
 * is implemented via platform_filter_ops callbacks.
 */

#include <ctype.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "igt.h"
#include "igt_platform_filter.h"

/* Maximum entries from config file and env variable */
#define MAX_CONFIG_ENTRIES 256
#define MAX_ENV_ENTRIES 128
#define MAX_LINE_LENGTH 512

/* Filter context - holds all runtime state (no globals) */
struct platform_filter_context {
	const struct platform_filter_ops *ops;
	const void *platform_info;

	struct platform_skip_entry config_entries[MAX_CONFIG_ENTRIES];
	int config_entry_count;

	struct platform_skip_entry env_entries[MAX_ENV_ENTRIES];
	int env_entry_count;

	bool initialized;
	char current_platform_name[64];
};

/* Single static instance - initialized on first use */
static struct platform_filter_context *get_filter_context(void)
{
	static struct platform_filter_context ctx = {0};

	return &ctx;
}

/*
 * ================================================================
 * HELPER FUNCTIONS
 * ================================================================
 */

/* Helper: Trim whitespace from string */
static char *trim(char *str)
{
	char *end;

	while (isspace(*str))
		str++;

	if (*str == 0)
		return str;

	end = str + strlen(str) - 1;
	while (end > str && isspace(*end))
		end--;

	*(end + 1) = 0;
	return str;
}

/* Helper: Match wildcard or exact string */
static bool match_string(const char *pattern, const char *str)
{
	if (!pattern || !str)
		return false;

	if (strcmp(pattern, "*") == 0)
		return true;
	return fnmatch(pattern, str, 0) == 0;
}

/* Helper: Check if entry matches platform/test/subtest */
static bool entry_matches(const struct platform_filter_context *ctx,
			  const struct platform_skip_entry *entry,
			  const char *test_name,
			  const char *subtest_name)
{
	if (entry->platform_data && ctx->ops->match_platform) {
		if (!ctx->ops->match_platform(ctx->platform_info, entry->platform_data))
			return false;
	}

	if (entry->test_name && strcmp(entry->test_name, "*") != 0) {
		if (!test_name || !match_string(entry->test_name, test_name))
			return false;
	}

	if (entry->subtest_glob && strcmp(entry->subtest_glob, "*") != 0) {
		if (!subtest_name || !match_string(entry->subtest_glob, subtest_name))
			return false;
	}
	return true;
}

/*
 * ================================================================
 * CONFIG FILE PARSER (/etc/igt/platform_skip.conf)
 * ================================================================
 */

/* Parse one line from config file */
static bool parse_config_line(struct platform_filter_context *ctx,
			      char *line,
			      struct platform_skip_entry *entry)
{
	char *platform, *test, *subtest, *reason;

	/* Skip comments and empty lines */
	line = trim(line);
	if (line[0] == '#' || line[0] == 0)
		return false;

	/* Format: platform:test:subtest:reason */
	platform = strtok(line, ":");
	test = strtok(NULL, ":");
	subtest = strtok(NULL, ":");
	reason = strtok(NULL, "\n");

	if (!platform || !test || !subtest) {
		igt_warn("Invalid config line format (expected platform:test:subtest:reason)\n");
		return false;
	}

	/* Allocate and copy strings */
	entry->test_name = strdup(trim(test));
	entry->subtest_glob = strdup(trim(subtest));
	entry->reason = reason ? strdup(trim(reason)) : strdup("No reason");

	/* Parse platform using vendor callback */
	platform = trim(platform);
	if (ctx->ops->parse_platform_config && strcmp(platform, "*") != 0) {
		/* Vendor-specific platform string */
		if (!ctx->ops->parse_platform_config(platform, &entry->platform_data)) {
			igt_warn("Failed to parse platform: %s\n", platform);
			free((void *)entry->test_name);
			free((void *)entry->subtest_glob);
			free((void *)entry->reason);
			return false;
		}
	} else {
		/* Wildcard or no vendor-specific parsing available */
		entry->platform_data = NULL;
	}
	return true;
}

/* Load config file */
static void load_config_file(struct platform_filter_context *ctx, const char *filename)
{
	FILE *f;
	char line[MAX_LINE_LENGTH];

	f = fopen(filename, "r");
	if (!f) {
		igt_debug("Config file not found: %s\n", filename);

		return;
	}

	igt_info("Loading platform skip config from: %s\n", filename);

	while (fgets(line, sizeof(line), f)) {
		if (ctx->config_entry_count >= MAX_CONFIG_ENTRIES) {
			igt_warn("Config file has too many entries (max %d)\n",
				 MAX_CONFIG_ENTRIES);
			break;
		}

		if (parse_config_line(ctx, line, &ctx->config_entries[ctx->config_entry_count]))
			ctx->config_entry_count++;
	}

	fclose(f);
	igt_info("Loaded %d skip rules from config file\n", ctx->config_entry_count);
}

/*
 * ================================================================
 * ENVIRONMENT VARIABLE PARSER (IGT_PLATFORM_SKIP_CONFIG)
 * ================================================================
 */

/* Parse environment variable entries (semicolon-separated) */
static void load_env_variable(struct platform_filter_context *ctx)
{
	char *env, *env_copy, *entry_str, *saveptr;
	const char *env_value;

	env_value = getenv("IGT_PLATFORM_SKIP_CONFIG");
	if (!env_value || env_value[0] == 0) {
		igt_debug("IGT_PLATFORM_SKIP_CONFIG not set\n");

		return;
	}

	igt_info("Loading platform skip config from IGT_PLATFORM_SKIP_CONFIG\n");

	env_copy = strdup(env_value);
	env = env_copy;

	/* Parse semicolon-separated entries */
	while ((entry_str = strtok_r(env, ";", &saveptr)) != NULL) {
		env = NULL; /* For subsequent strtok_r calls */

		if (ctx->env_entry_count >= MAX_ENV_ENTRIES) {
			igt_warn("Too many env variable entries (max %d)\n",
				 MAX_ENV_ENTRIES);
			break;
		}

		if (parse_config_line(ctx, entry_str, &ctx->env_entries[ctx->env_entry_count]))
			ctx->env_entry_count++;
	}

	free(env_copy);
	igt_info("Loaded %d skip rules from environment variable\n", ctx->env_entry_count);
}

/*
 * ================================================================
 * PUBLIC API IMPLEMENTATION
 * ================================================================
 */

/**
 * igt_platform_filter_init:
 * @ops: Platform-specific operation callbacks
 * @platform_info: Vendor-specific platform identification data
 *
 * Initialize the platform filtering system with vendor-specific backend.
 * This sets up the filter context and loads skip rules from three sources
 * in priority order:
 * 1. Built-in rules (highest priority, vendor-provided)
 * 2. Config file /etc/igt/platform_skip.conf
 * 3. Environment variable IGT_PLATFORM_SKIP_CONFIG (lowest priority)
 *
 * The filtering system is vendor-agnostic. Platform matching logic is
 * provided through the @ops callbacks, allowing each vendor to implement
 * their own identification scheme (e.g., AMD uses family_id/chip_rev,
 * Intel could use platform_id/stepping).
 *
 * Must be called once before using igt_platform_require().
 */
void igt_platform_filter_init(const struct platform_filter_ops *ops,
			      const void *platform_info)
{
	struct platform_filter_context *ctx = get_filter_context();

	if (ctx->initialized)

		return;

	if (!ops) {
		igt_warn("Platform filter ops is NULL, filtering disabled\n");

		return;
	}

	ctx->ops = ops;
	ctx->platform_info = platform_info;

	igt_info("Initializing platform filter system (3-tier priority) for vendor: %s\n",
		 ops->name ? ops->name : "unknown");

	/* Get current platform name */

	if (ops->get_platform_name) {
		const char *pname = ops->get_platform_name(platform_info);

		snprintf(ctx->current_platform_name, sizeof(ctx->current_platform_name),
			 "%s", pname ? pname : "unknown");
	}

	/* Priority 1: Built-in array (vendor-specific) */
	igt_debug("  Priority 1: Built-in array (vendor-specific)\n");

	/* Priority 2: Config file */
	igt_debug("  Priority 2: Config file /etc/igt/platform_skip.conf\n");
	load_config_file(ctx, "/etc/igt/platform_skip.conf");

	/* Priority 3: Environment variable */
	igt_debug("  Priority 3: Environment variable IGT_PLATFORM_SKIP_CONFIG\n");
	load_env_variable(ctx);

	ctx->initialized = true;
	igt_info("Platform filter initialization complete\n");
}

/**
 * igt_platform_should_skip:
 * @test_name: Name of the test
 * @subtest_name: Name of the subtest (or NULL for test-level check)
 *
 * Check if a test/subtest should be skipped based on platform filtering rules.
 *
 * Returns: true if the test should be skipped, false otherwise
 */
bool igt_platform_should_skip(const char *test_name,
			      const char *subtest_name,
			      enum skip_source *source,
			      const char **reason)
{
	struct platform_filter_context *ctx = get_filter_context();
	const struct platform_skip_entry *entry;
	int i, count;

	if (!ctx->initialized) {
		igt_warn("Platform filter not initialized\n");
		if (source)
			*source = SKIP_SOURCE_NONE;
		if (reason)
			*reason = NULL;
		return false;
	}

	/* Priority 1: Check built-in array FIRST */
	if (ctx->ops->get_builtin_rules) {
		const struct platform_skip_entry *builtin = ctx->ops->get_builtin_rules(&count);

		for (i = 0; i < count; i++) {
			entry = &builtin[i];
			if (entry_matches(ctx, entry, test_name, subtest_name)) {
				if (source)
					*source = SKIP_SOURCE_BUILTIN;
				if (reason)
					*reason = entry->reason;
				igt_debug("Skip (built-in): %s:%s - %s\n",
					  entry->test_name ? entry->test_name : "*",
					  entry->subtest_glob ? entry->subtest_glob : "*",
					  entry->reason ? entry->reason : "no reason");
				return true;
			}
		}
	}

	/* Priority 2: Check config file */
	for (i = 0; i < ctx->config_entry_count; i++) {
		entry = &ctx->config_entries[i];
		if (entry_matches(ctx, entry, test_name, subtest_name)) {
			if (source)
				*source = SKIP_SOURCE_CONFIG;
			if (reason)
				*reason = entry->reason;
			igt_debug("Skip (config): %s:%s - %s\n",
				  entry->test_name, entry->subtest_glob, entry->reason);
			return true;
		}
	}

	/* Priority 3: Check environment variable */
	for (i = 0; i < ctx->env_entry_count; i++) {
		entry = &ctx->env_entries[i];
		if (entry_matches(ctx, entry, test_name, subtest_name)) {
			if (source)
				*source = SKIP_SOURCE_ENV;
			if (reason)
				*reason = entry->reason;
			igt_debug("Skip (env): %s:%s - %s\n",
				  entry->test_name, entry->subtest_glob, entry->reason);
			return true;
		}
	}

	if (source)
		*source = SKIP_SOURCE_NONE;
	if (reason)
		*reason = NULL;
	return false;
}

/**
 * igt_platform_require:
 * @subtest_name: Name of the subtest to check
 *
 * Check if current subtest should be skipped and call igt_skip() if matched.
 * This integrates platform filtering with IGT's standard skip mechanism.
 *
 * The function automatically determines the test name from igt_test_name().
 * If a skip rule matches, calls igt_skip() with the configured reason.
 */
void igt_platform_require(const char *subtest_name)
{
	enum skip_source source;
	const char *test_name = igt_test_name();
	const char *reason;

	if (igt_platform_should_skip(test_name, subtest_name, &source, &reason)) {
		const char *source_str;

		switch (source) {
		case SKIP_SOURCE_BUILTIN:
			source_str = "built-in array";
			break;
		case SKIP_SOURCE_CONFIG:
			source_str = "config file";
			break;
		case SKIP_SOURCE_ENV:
			source_str = "environment variable";
			break;
		default:
			source_str = "unknown";
		}

		igt_skip("Skipped on this platform [%s]: %s\n",
			 source_str, reason ? reason : "no reason");
	}
}

/**
 * igt_platform_filter_dump:
 *
 * Dump the current platform filtering configuration to stdout.
 * Shows all loaded skip rules from built-in, config file, and environment
 * variable sources. Useful for debugging which rules are active.
 */
void igt_platform_filter_dump(void)
{
	struct platform_filter_context *ctx = get_filter_context();
	const struct platform_skip_entry *entry;
	int i, total_count, count;

	if (!ctx->initialized) {
		igt_info("Platform filter not initialized\n");

		return;
	}

	igt_info("\n");
	igt_info("═══════════════════════════════════════════════════════════════════\n");
	igt_info(" PLATFORM SKIP FILTER CONFIGURATION - THREE-TIER PRIORITY SYSTEM\n");
	igt_info("═══════════════════════════════════════════════════════════════════\n\n");

	igt_info("Vendor: %s\n", ctx->ops->name ? ctx->ops->name : "unknown");
	if (ctx->current_platform_name[0])
		igt_info("Current Platform: %s\n\n", ctx->current_platform_name);

	/* Priority 1: Built-in array */
	igt_info("───────────────────────────────────────────────────────────────────\n");
	igt_info(" PRIORITY 1: BUILT-IN PRODUCTION ARRAY (VENDOR-SPECIFIC)\n");
	igt_info(" Source: Vendor implementation\n");
	igt_info("───────────────────────────────────────────────────────────────────\n");

	total_count = 0;
	if (ctx->ops->get_builtin_rules) {
		const struct platform_skip_entry *builtin = ctx->ops->get_builtin_rules(&count);

		for (i = 0; i < count; i++) {
			entry = &builtin[i];
			total_count++;
			igt_info("  %2d. %s : %s\n",
				 total_count,
			       entry->test_name ? entry->test_name : "*",
			       entry->subtest_glob ? entry->subtest_glob : "*");
			igt_info("      Reason: %s\n", entry->reason ? entry->reason : "no reason");

			/* Print platform data if vendor provides dump callback */
			if (entry->platform_data && ctx->ops->dump_platform_data) {
				igt_info("      Platform: ");
				ctx->ops->dump_platform_data(entry->platform_data);
				igt_info("\n");
			}
			igt_info("\n");
		}
	}
	if (total_count == 0)
		igt_info("  (No built-in rules)\n\n");

	/* Priority 2: Config file */
	igt_info("───────────────────────────────────────────────────────────────────\n");
	igt_info(" PRIORITY 2: DEVELOPMENT CONFIG FILE\n");
	igt_info(" Source: /etc/igt/platform_skip.conf\n");
	igt_info("───────────────────────────────────────────────────────────────────\n");

	if (ctx->config_entry_count > 0) {
		for (i = 0; i < ctx->config_entry_count; i++) {
			entry = &ctx->config_entries[i];
			igt_info("  %2d. %s : %s\n",
				 i + 1,
			       entry->test_name,
			       entry->subtest_glob);
			igt_info("      Reason: %s\n\n", entry->reason);
		}
	} else {
		igt_info("  (No config file rules loaded)\n\n");
	}

	/* Priority 3: Environment variable */
	igt_info("───────────────────────────────────────────────────────────────────\n");
	igt_info(" PRIORITY 3: RUNTIME ENVIRONMENT VARIABLE\n");
	igt_info(" Source: IGT_PLATFORM_SKIP_CONFIG\n");
	igt_info("───────────────────────────────────────────────────────────────────\n");

	if (ctx->env_entry_count > 0) {
		for (i = 0; i < ctx->env_entry_count; i++) {
			entry = &ctx->env_entries[i];

			igt_info("  %2d. %s : %s\n",
				 i + 1,
			       entry->test_name,
			       entry->subtest_glob);
			igt_info("      Reason: %s\n\n", entry->reason);
		}
	} else {
		igt_info("  (No environment variable rules)\n\n");
	}

	igt_info("───────────────────────────────────────────────────────────────────\n");
	igt_info(" SUMMARY\n");
	igt_info("───────────────────────────────────────────────────────────────────\n");
	igt_info("  Built-in rules:     %d\n", total_count);
	igt_info("  Config file rules:  %d\n", ctx->config_entry_count);
	igt_info("  Environment rules:  %d\n", ctx->env_entry_count);
	igt_info("  Total skip rules:   %d\n",
		 total_count + ctx->config_entry_count + ctx->env_entry_count);
	igt_info("\n");
	igt_info("═══════════════════════════════════════════════════════════════════\n\n");
}

/**
 * igt_platform_filter_dump_to_file:
 * @filename: Path to output file
 *
 * Dump platform filtering configuration to a file.
 *
 * Returns: 0 on success, -1 on error
 */
int igt_platform_filter_dump_to_file(const char *filename)
{
	FILE *old_stdout;
	FILE *f;

	f = fopen(filename, "w");
	if (!f) {
		igt_warn("Failed to open %s for writing\n", filename);
		return -1;
	}

	/* Redirect stdout to file */
	old_stdout = stdout;
	stdout = f;

	igt_platform_filter_dump();

	/* Restore stdout */
	stdout = old_stdout;
	fclose(f);

	igt_info("Platform filter configuration dumped to: %s\n", filename);
	return 0;
}

/**
 * igt_platform_filter_is_initialized:
 *
 * Check if platform filtering has been initialized.
 *
 * Returns: true if initialized, false otherwise
 */
bool igt_platform_filter_is_initialized(void)
{
	struct platform_filter_context *ctx = get_filter_context();

	return ctx && ctx->initialized;
}
