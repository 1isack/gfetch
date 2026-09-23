/* gfetch.c: the classic Glenda Fetch re-implemented in C for Linux systems. */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <sys/statvfs.h>
#include <dirent.h>

/* Helpers */

static void
chomp(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' '))
		s[--n] = '\0';
}

/* OS/kernel */

static void
get_os(char *buf, size_t sz)
{
	FILE *f;
	char line[256];
	char name[128] = {0};

	f = fopen("/etc/os-release", "r");
	if (!f) f = fopen("/usr/lib/os-release", "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			if (strncmp(line, "PRETTY_NAME=", 12) == 0) {
				char *val = line + 12;
				if (*val == '"') val++;
				chomp(val);
				size_t l = strlen(val);
				if (l && val[l-1] == '"') val[l-1] = '\0';
				snprintf(name, sizeof(name), "%s", val);
				break;
			}
		}
		fclose(f);
	}

	if (!name[0])
		snprintf(name, sizeof(name), "Linux");

	snprintf(buf, sz, "%s", name);
}

static void
get_kernel(char *buf, size_t sz)
{
	struct utsname u;
	if (uname(&u) == 0)
		snprintf(buf, sz, "%s", u.release);
	else
		snprintf(buf, sz, "unknown");
}

/* CPU */

static void
shorten_cpu(char *s)
{
	char *p;
	while ((p = strstr(s, "(R)"))  != NULL) memmove(p, p+3, strlen(p+3)+1);
	while ((p = strstr(s, "(TM)")) != NULL) memmove(p, p+4, strlen(p+4)+1);
	if   ((p = strstr(s, " CPU"))  != NULL) memmove(p, p+4, strlen(p+4)+1);
	if   ((p = strstr(s, " @ "))   != NULL) *p = '\0';

	if   ((p = strstr(s, "-Core")) != NULL) *p = '\0';

	while ((p = strstr(s, "  ")) != NULL) memmove(p, p+1, strlen(p+1)+1);

	size_t n = strlen(s);
	while (n > 0 && s[n-1] == ' ') s[--n] = '\0';
}

static void
get_cpu(char *buf, size_t sz)
{
	FILE *f;
	char line[256];

	f = fopen("/proc/cpuinfo", "r");
	if (!f) {
		snprintf(buf, sz, "unknown");
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "model name", 10) == 0) {
			char *colon = strchr(line, ':');
			if (colon) {
				colon++;
				while (*colon == ' ') colon++;
				chomp(colon);
				snprintf(buf, sz, "%s", colon);
				shorten_cpu(buf);
				fclose(f);
				return;
			}
		}
	}
	fclose(f);
	snprintf(buf, sz, "unknown");
}

/* RAM */

static void
get_ram(char *used_buf, size_t used_sz, char *total_buf, size_t total_sz)
{
	FILE *f;
	char line[256];
	unsigned long long total = 0, memfree = 0, buffers = 0;
	unsigned long long cached = 0, sreclaimable = 0, shmem = 0;

	f = fopen("/proc/meminfo", "r");
	if (!f) {
		snprintf(used_buf,  used_sz,  "?");
		snprintf(total_buf, total_sz, "?");
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		if      (strncmp(line, "MemTotal:",     9)  == 0) sscanf(line + 9,  "%llu", &total);
		else if (strncmp(line, "MemFree:",      8)  == 0) sscanf(line + 8,  "%llu", &memfree);
		else if (strncmp(line, "Buffers:",      8)  == 0) sscanf(line + 8,  "%llu", &buffers);
		else if (strncmp(line, "Cached:",       7)  == 0 &&
		         strncmp(line, "SwapCached:",  11)  != 0) sscanf(line + 7,  "%llu", &cached);
		else if (strncmp(line, "SReclaimable:", 13) == 0) sscanf(line + 13, "%llu", &sreclaimable);
		else if (strncmp(line, "Shmem:",        6)  == 0) sscanf(line + 6,  "%llu", &shmem);
	}
	fclose(f);

	unsigned long long usedDiff = memfree + cached + sreclaimable + buffers;
	unsigned long long used_kb  = (total >= usedDiff) ? total - usedDiff : total - memfree;
	used_kb += shmem;
	double total_gib = total   / (1024.0 * 1024.0);
	double used_gib  = used_kb / (1024.0 * 1024.0);
	snprintf(used_buf,  used_sz,  "%.2f", used_gib);
	snprintf(total_buf, total_sz, "%.2f", total_gib);
}

/* Uptime */

static void
get_uptime(char *buf, size_t sz)
{
	struct sysinfo si;
	if (sysinfo(&si) != 0) {
		snprintf(buf, sz, "unknown");
		return;
	}
	long up = si.uptime;
	int days  = up / 86400;
	int hours = (up % 86400) / 3600;
	int mins  = (up % 3600)  / 60;

	if (days > 0)
		snprintf(buf, sz, "%dd %dh %dm", days, hours, mins);
	else if (hours > 0)
		snprintf(buf, sz, "%dh %dm", hours, mins);
	else
		snprintf(buf, sz, "%dm", mins);
}

/* Shell */

static void
get_shell(char *buf, size_t sz)
{
	const char *s = getenv("SHELL");
	if (s) {
		const char *b = strrchr(s, '/');
		snprintf(buf, sz, "%s", b ? b + 1 : s);
	} else {
		snprintf(buf, sz, "unknown");
	}
}

/* Display server (Wayland vs Xorg) */

static void
get_display_server(char *buf, size_t sz)
{
	const char *session_type = getenv("XDG_SESSION_TYPE");
	const char *wayland_display = getenv("WAYLAND_DISPLAY");
	const char *x_display = getenv("DISPLAY");

	if (session_type) {
		if (strcasecmp(session_type, "wayland") == 0) {
			snprintf(buf, sz, "Wayland%s%s",
			         wayland_display ? " (" : "",
			         wayland_display ? wayland_display : "");
			if (wayland_display) strncat(buf, ")", sz - strlen(buf) - 1);
			return;
		}
		if (strcasecmp(session_type, "x11") == 0) {
			snprintf(buf, sz, "Xorg");
			return;
		}
		if (strcasecmp(session_type, "tty") == 0) {
			snprintf(buf, sz, "tty (no display server)");
			return;
		}
	}

	/* Fallback: env vars alone, session_type unset or unrecognized */
	if (wayland_display && x_display) {
		snprintf(buf, sz, "Wayland+XWayland");
	} else if (wayland_display) {
		snprintf(buf, sz, "Wayland");
	} else if (x_display) {
		snprintf(buf, sz, "Xorg");
	} else {
		snprintf(buf, sz, "none (headless/tty)");
	}
}

/* Disk */

static void
get_disk(char *buf, size_t sz)
{
	struct statvfs st;
	if (statvfs("/", &st) != 0) {
		snprintf(buf, sz, "unknown");
		return;
	}
	unsigned long long total = (unsigned long long)st.f_blocks * st.f_frsize;
	unsigned long long free  = (unsigned long long)st.f_bfree  * st.f_frsize;
	unsigned long long used  = total - free;

	double used_gib  = used  / (1024.0 * 1024.0 * 1024.0);
	double total_gib = total / (1024.0 * 1024.0 * 1024.0);
	snprintf(buf, sz, "%.1f / %.1f GiB", used_gib, total_gib);
}

/*
 * GPU
 */

static void
get_gpu(char *buf, size_t sz)
{
	FILE *f;
	char line[256];
	char driver[64] = {0};
	char pci_id[16] = {0};

	char uevent_path[64];
	for (int card = 0; card <= 1; card++) {
		snprintf(uevent_path, sizeof(uevent_path),
		         "/sys/class/drm/card%d/device/uevent", card);
		f = fopen(uevent_path, "r");
		if (f) break;
	}
	if (!f) {
		snprintf(buf, sz, "unknown");
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		chomp(line);
		if (strncmp(line, "DRIVER=", 7) == 0) {
			snprintf(driver, sizeof(driver), "%.63s", line + 7);
		} else if (strncmp(line, "PCI_ID=", 7) == 0) {
			snprintf(pci_id, sizeof(pci_id), "%.9s", line + 7);
		}
	}
	fclose(f);

	if (!pci_id[0] && !driver[0]) {
		snprintf(buf, sz, "unknown");
		return;
	}

	const char *vendor = "";
	if      (strncasecmp(pci_id, "8086", 4) == 0) vendor = "Intel";
	else if (strncasecmp(pci_id, "1002", 4) == 0) vendor = "AMD";
	else if (strncasecmp(pci_id, "10de", 4) == 0) vendor = "NVIDIA";

	char model[128] = {0};
	char label_path[64];
	snprintf(label_path, sizeof(label_path),
	         "/sys/class/drm/card%d/device/label",
	         strstr(uevent_path, "card1") ? 1 : 0);
	f = fopen(label_path, "r");
	if (f) {
		if (fgets(model, sizeof(model), f))
			chomp(model);
		fclose(f);
	}

	if (model[0])
		snprintf(buf, sz, "%s", model);
	else if (vendor[0] && driver[0])
		snprintf(buf, sz, "%s (%s)", vendor, driver);
	else if (vendor[0])
		snprintf(buf, sz, "%s", vendor);
	else
		snprintf(buf, sz, "%s", driver);
}

/* Packages: detect whichever package managers are present and count
 * installed packages for each, e.g. "pacman 941, flatpak 12, nix 503" */

struct pm_info {
	const char *label;   /* shown in output */
	const char *bin;     /* binary to look for on PATH */
	const char *cmd;     /* shell command whose stdout line count == pkg count */
};

static const struct pm_info pkg_managers[] = {
	{ "pacman",  "pacman",      "pacman -Qq 2>/dev/null" },
	{ "xbps",    "xbps-query",  "xbps-query -l 2>/dev/null" },
	{ "nix",     "nix-store",   "nix-store -q --requisites /run/current-system 2>/dev/null" },
	{ "cave",    "cave",        "cave print-ids -m installed --format '%c/%p\\n' 2>/dev/null" },
	{ "flatpak", "flatpak",     "flatpak list --columns=application 2>/dev/null" },
	{ "kiss",    "kiss",        "kiss list 2>/dev/null" },
	{ "apk",     "apk",         "apk info 2>/dev/null" },
	{ "dpkg",    "dpkg-query",  "dpkg-query -f '.\\n' -W 2>/dev/null" },
	{ "rpm",     "rpm",         "rpm -qa 2>/dev/null" },
	{ "portage", "qlist",       "qlist -I 2>/dev/null" },
	{ NULL, NULL, NULL }
};

static int
bin_on_path(const char *bin)
{
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", bin);
	return system(cmd) == 0;
}

static long
count_lines(const char *cmd)
{
	FILE *f = popen(cmd, "r");
	if (!f) return -1;
	long n = 0;
	char line[512];
	while (fgets(line, sizeof(line), f)) n++;
	int status = pclose(f);
	if (status != 0 && n == 0) return -1;
	return n;
}

static void
get_packages(char *buf, size_t sz)
{
	buf[0] = '\0';
	int first = 1;

	for (int i = 0; pkg_managers[i].bin != NULL; i++) {
		if (!bin_on_path(pkg_managers[i].bin))
			continue;

		long n = count_lines(pkg_managers[i].cmd);
		if (n < 0)
			continue;

		char entry[64];
		snprintf(entry, sizeof(entry), "%s%s %ld",
		         first ? "" : ", ", pkg_managers[i].label, n);

		if (strlen(buf) + strlen(entry) + 1 >= sz)
			break;

		strncat(buf, entry, sz - strlen(buf) - 1);
		first = 0;
	}

	if (buf[0] == '\0')
		snprintf(buf, sz, "unknown");
}

/* Glenda the rabbit */

static const char *rabbit[] = {
	"             %s@%s",
	"    (\\(\\     -----------",
	"   j\". ..    os: %s",
	"   (  . .)   kernel: %s",
	"   |   \xc2\xb0 \xc2\xa1   shell: %s",
	"   \xc2\xbf     ;   display: %s",
	"   c?\".UJ    uptime: %s",
	"             cpu: %s",
	"             ram: %s / %s GiB",
	"             disk: %s",
	"             gpu: %s",
	"             pkgs: %s",
	NULL
};

/* main */

int
main(void)
{
	char os[128], kernel[128], cpu[256];
	char shell[64], uptime[64], display[128];
	char ram_used[32], ram_total[32];
	char disk[64], gpu[128], packages[512];
	char hostname[64] = {0};
	const char *user;

	get_os(os, sizeof(os));
	get_kernel(kernel, sizeof(kernel));
	get_cpu(cpu, sizeof(cpu));
	get_shell(shell, sizeof(shell));
	get_display_server(display, sizeof(display));
	get_uptime(uptime, sizeof(uptime));
	get_ram(ram_used, sizeof(ram_used), ram_total, sizeof(ram_total));
	get_disk(disk, sizeof(disk));
	get_gpu(gpu, sizeof(gpu));
	get_packages(packages, sizeof(packages));

	gethostname(hostname, sizeof(hostname) - 1);
	user = getenv("USER");
	if (!user) user = getenv("LOGNAME");
	if (!user) user = "unknown";

	printf(rabbit[0], user, hostname);      putchar('\n');
	printf("%s\n", rabbit[1]);
	printf(rabbit[2], os); putchar('\n');
	printf(rabbit[3], kernel); putchar('\n');
	printf(rabbit[4], shell); putchar('\n');
	printf(rabbit[5], display); putchar('\n');
	printf(rabbit[6], uptime); putchar('\n');
	printf(rabbit[7], cpu); putchar('\n');
	printf(rabbit[8], ram_used, ram_total); putchar('\n');
	printf(rabbit[9], disk); putchar('\n');
	printf(rabbit[10], gpu); putchar('\n');
	printf(rabbit[11], packages); putchar('\n');

	return 0;
}
