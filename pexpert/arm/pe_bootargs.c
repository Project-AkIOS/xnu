/*
 * Copyright (c) 2000-2007 Apple Inc. All rights reserved.
 */
#include <pexpert/pexpert.h>
#include <pexpert/boot.h>
#include <string.h>

/*
 * HakiOS: arguments built into the kernel (CONFIG_DEFAULTS of the device's configuration; tools/hakios-config.py kernel
 * writes them to bsd/dev/hakios.drivers.h as HAKIOS_DEFAULT_BOOTARGS). Each holds unless the command line names it: the
 * command line always wins, x=0 turns a default off. The line every reader sees -- PE_parse_boot_argn, sysctl
 * kern.bootargs (user space), the panic log -- is the command line followed by the defaults it does not name, made
 * once, the first time it is asked for (early in arm_init, one CPU). So what works on a device needs no room on a
 * command line the loader can hand over only ~490 characters of.
 */
#if __has_include("../../bsd/dev/hakios.drivers.h")
#include "../../bsd/dev/hakios.drivers.h"
#endif

#if defined(HAKIOS_DEFAULT_BOOTARGS)
#define HAKIOS_LINE_LENGTH      1024            /* user space's readers take up to 1023 characters */
static char hakios_line[HAKIOS_LINE_LENGTH];
static int hakios_line_made;

static int
hakios_sep(char c)
{
	return c == ' ' || c == '\t' || c == '\0';
}

/* Whether LINE has an argument called NAME (NAME_LEN characters): "name", "name=...", "-name" */
static int
hakios_line_names(const char *line, const char *name, size_t name_len)
{
	const char *p = line;

	for (;;) {
		while (*p != '\0' && hakios_sep(*p)) {
			p++;
		}
		if (*p == '\0') {
			return 0;
		}
		const char *word = p;
		if (*word == '-') {
			word++;
		}
		const char *end = word;
		while (!hakios_sep(*end) && *end != '=') {
			end++;
		}
		if ((size_t)(end - word) == name_len && strncmp(word, name, name_len) == 0) {
			return 1;
		}
		p = end;
		while (!hakios_sep(*p)) {
			p++;
		}
	}
}

static void
hakios_make_line(const char *command_line)
{
	const char *d = HAKIOS_DEFAULT_BOOTARGS;
	size_t len = strlcpy(hakios_line, command_line, sizeof(hakios_line));

	if (len >= sizeof(hakios_line)) {
		return;                         /* cannot be: the command line holds BOOT_LINE_LENGTH */
	}
	while (*d != '\0') {
		while (*d == ' ') {
			d++;
		}
		const char *word = d, *name_end = d;
		while (*d != '\0' && *d != ' ') {
			d++;
		}
		while (name_end < d && *name_end != '=') {
			name_end++;
		}
		size_t word_len = (size_t)(d - word);
		if (word_len == 0 || hakios_line_names(command_line, word, (size_t)(name_end - word))) {
			continue;
		}
		if (len + 1 + word_len >= sizeof(hakios_line)) {
			break;
		}
		if (len > 0) {
			hakios_line[len++] = ' ';
		}
		memcpy(hakios_line + len, word, word_len);
		len += word_len;
		hakios_line[len] = '\0';
	}
}
#endif /* HAKIOS_DEFAULT_BOOTARGS */

char *
PE_boot_args(
	void)
{
	char *line = (char *)((boot_args *)PE_state.bootArgs)->CommandLine;

#if defined(HAKIOS_DEFAULT_BOOTARGS)
	if (HAKIOS_DEFAULT_BOOTARGS[0] != '\0') {
		if (!hakios_line_made) {
			hakios_make_line(line);
			hakios_line_made = 1;
		}
		return hakios_line;
	}
#endif
	return line;
}
