/*
 * Windows paths onto the host. The exe believes it runs from C:\GTA\WINO and
 * reads ..\GTADATA\... and ..\MUSIC\...; each of those folders can live
 * anywhere (ini keys exe, gtadata, music, root). Names match ignoring case.
 */
#include "shell.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define VIRTUAL_EXE "C:\\GTA\\WINO\\Grand Theft Auto.exe"

const char *files_exe_winpath(void)
{
	return VIRTUAL_EXE;
}

/* the folder part of the configured exe path */
static void exe_dir(char *out, size_t len)
{
	const char *exe = config_str("exe", "WINO/Grand Theft Auto.exe");
	const char *slash = strrchr(exe, '/');
	if (!slash)
		snprintf(out, len, ".");
	else
		snprintf(out, len, "%.*s", (int)(slash - exe), exe);
}

/* appends one path component, finding its real spelling if it exists */
static void add_component(char *host, size_t len, const char *comp)
{
	char probe[1024];
	struct stat st;
	DIR *d;
	struct dirent *e;
	size_t n = strlen(host);

	snprintf(probe, sizeof probe, "%s/%s", host, comp);
	if (stat(probe, &st) == 0 || !(d = opendir(n ? host : "."))) {
		snprintf(host + n, len - n, "/%s", comp);
		return;
	}
	while ((e = readdir(d)) != NULL)
		if (!strcasecmp(e->d_name, comp)) {
			snprintf(host + n, len - n, "/%s", e->d_name);
			closedir(d);
			return;
		}
	closedir(d);
	snprintf(host + n, len - n, "/%s", comp);
}

char *files_host_path(const char *winpath, char *out, size_t outlen)
{
	char path[1024], *comps[64], *tok, *save;
	int n = 0, start;
	const char *base;
	char dir[512];

	/* absolute virtual path: C:\GTA\WINO\ + relative part */
	if (winpath[0] && winpath[1] == ':')
		snprintf(path, sizeof path, "%s", winpath + 2);
	else if (winpath[0] == '\\' || winpath[0] == '/')
		snprintf(path, sizeof path, "%s", winpath);
	else
		snprintf(path, sizeof path, "\\GTA\\WINO\\%s", winpath);
	for (char *p = path; *p; p++)
		if (*p == '\\')
			*p = '/';
	for (tok = strtok_r(path, "/", &save); tok && n < 64; tok = strtok_r(NULL, "/", &save)) {
		if (!strcmp(tok, ".") || !*tok)
			continue;
		if (!strcmp(tok, "..")) {
			if (n > 0)
				n--;
			continue;
		}
		comps[n++] = tok;
	}

	/* which configured folder the path starts in */
	start = 0;
	base = config_str("root", ".");
	if (n >= 2 && !strcasecmp(comps[0], "GTA")) {
		if (!strcasecmp(comps[1], "WINO")) {
			exe_dir(dir, sizeof dir);
			base = dir;
			start = 2;
		} else if (!strcasecmp(comps[1], "GTADATA")) {
			base = config_str("gtadata", "GTADATA");
			start = 2;
		} else if (!strcasecmp(comps[1], "MUSIC")) {
			base = config_str("music", "Music");
			start = 2;
		} else {
			start = 1;
		}
	}
	snprintf(out, outlen, "%s", base);
	if (strlen(out) > 1 && out[strlen(out) - 1] == '/')
		out[strlen(out) - 1] = 0;
	for (int i = start; i < n; i++)
		add_component(out, outlen, comps[i]);
	return out;
}
