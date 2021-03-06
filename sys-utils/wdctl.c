/*
 * wdctl(8) - show hardware watchdog status
 *
 * Copyright (C) 2012 Lennart Poettering
 * Copyright (C) 2012 Karel Zak <kzak@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include <linux/watchdog.h>
#include <sys/ioctl.h>
#include <getopt.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>

#include "nls.h"
#include "c.h"
#include "closestream.h"
#include "optutils.h"
#include "pathnames.h"
#include "strutils.h"
#include "tt.h"

struct wdflag {
	uint32_t	flag;
	const char	*name;
	const char	*description;
};

static const struct wdflag wdflags[] = {
	{ WDIOF_CARDRESET,     "CARDRESET",  N_("Card previously reset the CPU") },
	{ WDIOF_EXTERN1,       "EXTERN1",    N_("External relay 1") },
	{ WDIOF_EXTERN2,       "EXTERN2",    N_("External relay 2") },
	{ WDIOF_FANFAULT,      "FANFAULT",   N_("Fan failed") },
	{ WDIOF_KEEPALIVEPING, "KEEPALIVEPING", N_("Keep alive ping reply") },
	{ WDIOF_MAGICCLOSE,    "MAGICCLOSE", N_("Supports magic close char") },
	{ WDIOF_OVERHEAT,      "OVERHEAT",   N_("Reset due to CPU overheat") },
	{ WDIOF_POWEROVER,     "POWEROVER",  N_("Power over voltage") },
	{ WDIOF_POWERUNDER,    "POWERUNDER", N_("Power bad/power fault") },
	{ WDIOF_PRETIMEOUT,    "PRETIMEOUT", N_("Pretimeout (in seconds)") },
	{ WDIOF_SETTIMEOUT,    "SETTIMEOUT", N_("Set timeout (in seconds)") }
};


/* column names */
struct colinfo {
	const char *name; /* header */
	double	   whint; /* width hint (N < 1 is in percent of termwidth) */
	int	   flags; /* TT_FL_* */
	const char *help;
};

enum { COL_FLAG, COL_DESC, COL_STATUS, COL_BSTATUS, COL_DEVICE };

/* columns descriptions */
static struct colinfo infos[] = {
	[COL_FLAG]    = { "FLAG",        14,  0, N_("flag name") },
	[COL_DESC]    = { "DESCRIPTION", 0.1, TT_FL_TRUNC, N_("flag description") },
	[COL_STATUS]  = { "STATUS",      1,   TT_FL_RIGHT, N_("flag status") },
	[COL_BSTATUS] = { "BOOT-STATUS", 1,   TT_FL_RIGHT, N_("flag boot status") },
	[COL_DEVICE]  = { "DEVICE",      0.1, 0, N_("watchdog device name") }

};

#define NCOLS ARRAY_SIZE(infos)
static int columns[NCOLS], ncolumns;

struct wdinfo {
	char		*device;

	int		timeout;
	int		timeleft;
	int		pretimeout;

	uint32_t	status;
	uint32_t	bstatus;

	struct watchdog_info ident;

	unsigned int	has_timeout : 1,
			has_timeleft : 1,
			has_pretimeout : 1;
};

/* converts flag name to flag bit */
static long name2bit(const char *name, size_t namesz)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(wdflags); i++) {
		const char *cn = wdflags[i].name;
		if (!strncasecmp(name, cn, namesz) && !*(cn + namesz))
			return wdflags[i].flag;
	}
	warnx(_("unknown flag: %s"), name);
	return -1;
}

static int column2id(const char *name, size_t namesz)
{
	size_t i;

	for (i = 0; i < NCOLS; i++) {
		const char *cn = infos[i].name;
		if (!strncasecmp(name, cn, namesz) && !*(cn + namesz))
			return i;
	}
	warnx(_("unknown column: %s"), name);
	return -1;
}

static int get_column_id(int num)
{
	assert(ARRAY_SIZE(columns) == NCOLS);
	assert(num < ncolumns);
	assert(columns[num] < (int) NCOLS);

	return columns[num];
}

static struct colinfo *get_column_info(unsigned num)
{
	return &infos[ get_column_id(num) ];
}

static void usage(FILE *out)
{
	size_t i;

	fputs(USAGE_HEADER, out);
	fprintf(out,
	      _(" %s [options] [<device> ...]\n"), program_invocation_short_name);

	fputs(USAGE_OPTIONS, out);

	fputs(_(" -f, --flags <list>    print selected flags only\n"
		" -F, --noflags         don't print information about flags\n"
		" -I, --noident         don't print watchdog identity information\n"
		" -n, --noheadings      don't print headings for flags table\n"
		" -O, --oneline         print all information on one line\n"
		" -o, --output <list>   output columns of the flags\n"
		" -r, --raw             use raw output format for flags table\n"
		" -T, --notimeouts      don't print watchdog timeouts\n"
		" -x, --flags-only      print only flags table (same as -I -T)\n"), out);

	fputs(USAGE_SEPARATOR, out);
	fputs(USAGE_HELP, out);
	fputs(USAGE_VERSION, out);
	fputs(USAGE_SEPARATOR, out);

	fprintf(out, _("The default device is %s.\n"), _PATH_WATCHDOG_DEV);
	fputs(USAGE_SEPARATOR, out);

	fputs(_("Available columns:\n"), out);
	for (i = 0; i < ARRAY_SIZE(infos); i++)
		fprintf(out, " %13s  %s\n", infos[i].name, _(infos[i].help));

	fprintf(out, USAGE_MAN_TAIL("wdctl(8)"));

	exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

static void add_flag_line(struct tt *tt, struct wdinfo *wd, const struct wdflag *fl)
{
	int i;
	struct tt_line *line;

	line = tt_add_line(tt, NULL);
	if (!line) {
		warn(_("failed to add line to output"));
		return;
	}

	for (i = 0; i < ncolumns; i++) {
		const char *str = NULL;

		switch (get_column_id(i)) {
		case COL_FLAG:
			str = fl->name;
			break;
		case COL_DESC:
			str = fl->description;
			break;
		case COL_STATUS:
			str = wd->status & fl->flag ? "1" : "0";
			break;
		case COL_BSTATUS:
			str = wd->bstatus & fl->flag ? "1" : "0";
			break;
		case COL_DEVICE:
			str = wd->device;
			break;
		default:
			break;
		}

		if (str)
			tt_line_set_data(line, i, str);
	}
}

static int show_flags(struct wdinfo *wd, int tt_flags, uint32_t wanted)
{
	size_t i;
	int rc = -1;
	struct tt *tt;
	uint32_t flags;

	/* create output table */
	tt = tt_new_table(tt_flags);
	if (!tt) {
		warn(_("failed to initialize output table"));
		return -1;
	}

	/* define columns */
	for (i = 0; i < (size_t) ncolumns; i++) {
		struct colinfo *col = get_column_info(i);

		if (!tt_define_column(tt, col->name, col->whint, col->flags)) {
			warnx(_("failed to initialize output column"));
			goto done;
		}
	}

	/* fill-in table with data
	 * -- one line for each supported flag (option)	 */
	flags = wd->ident.options;

	for (i = 0; i < ARRAY_SIZE(wdflags); i++) {
		if (wanted && !(wanted & wdflags[i].flag))
			; /* ignore */
		else if (flags & wdflags[i].flag)
			add_flag_line(tt, wd, &wdflags[i]);

		flags &= ~wdflags[i].flag;
	}

	if (flags)
		warnx(_("%s: unknown flags 0x%x\n"), wd->device, flags);

	tt_print_table(tt);
	rc = 0;
done:
	tt_free_table(tt);
	return rc;
}

/*
 * Warning: successfully opened watchdog has to be properly closed with magic
 * close character otherwise the machine will be rebooted!
 *
 * Don't use err() or exit() here!
 */
static int read_watchdog(struct wdinfo *wd)
{
	int fd;
	sigset_t sigs, oldsigs;

	assert(wd->device);

	sigemptyset(&oldsigs);
	sigfillset(&sigs);
	sigprocmask(SIG_BLOCK, &sigs, &oldsigs);

	fd = open(wd->device, O_WRONLY|O_CLOEXEC);

	if (fd < 0) {
		if (errno == EBUSY)
			warnx(_("%s: watchdog already in use, terminating."),
					wd->device);
		warn(_("cannot open %s"), wd->device);
		return -1;
	}

	if (ioctl(fd, WDIOC_GETSUPPORT, &wd->ident) < 0)
		warn(_("%s: failed to get information about watchdog"), wd->device);
	else {
		ioctl(fd, WDIOC_GETSTATUS, &wd->status);
		ioctl(fd, WDIOC_GETBOOTSTATUS, &wd->bstatus);

		if (ioctl(fd, WDIOC_GETTIMEOUT, &wd->timeout) >= 0)
			wd->has_timeout = 1;
		if (ioctl(fd, WDIOC_GETPRETIMEOUT, &wd->pretimeout) >= 0)
			wd->has_pretimeout = 1;
		if (ioctl(fd, WDIOC_GETTIMELEFT, &wd->timeleft) >= 0)
			wd->has_timeleft = 1;
	}

	for (;;) {
		/* We just opened this to query the state, not to arm
		 * it hence use the magic close character */
		static const char v = 'V';

		if (write(fd, &v, 1) >= 0)
			break;
		if (errno != EINTR) {
			warn(_("%s: failed to disarm watchdog"), wd->device);
			break;
		}
		/* Let's try hard, since if we don't get this right
		 * the machine might end up rebooting. */
	}

	close(fd);
	sigprocmask(SIG_SETMASK, &oldsigs, NULL);

	return 0;
}

static void print_oneline(struct wdinfo *wd, uint32_t wanted,
		int noident, int notimeouts, int noflags)
{
	printf("%s:", wd->device);

	if (!noident) {
		printf(" VERSION=\"%x\"", wd->ident.firmware_version);

		printf(" IDENTITY=");
		tt_fputs_quoted((char *) wd->ident.identity, stdout);
	}
	if (!notimeouts) {
		if (wd->has_timeout)
			printf(" TIMEOUT=\"%i\"", wd->timeout);
		if (wd->has_pretimeout)
			printf(" PRETIMEOUT=\"%i\"", wd->pretimeout);
		if (wd->has_timeleft)
			printf(" TIMELEFT=\"%i\"", wd->timeleft);
	}

	if (!noflags) {
		size_t i;
		uint32_t flags = wd->ident.options;

		for (i = 0; i < ARRAY_SIZE(wdflags); i++) {
			const struct wdflag *fl;

			if ((wanted && !(wanted & wdflags[i].flag)) ||
			    !(flags & wdflags[i].flag))
				continue;

			fl= &wdflags[i];

			printf(" %s=\"%s\"", fl->name,
					     wd->status & fl->flag ? "1" : "0");
			printf(" %s_BOOT=\"%s\"", fl->name,
					     wd->bstatus & fl->flag ? "1" : "0");

		}
	}

	fputc('\n', stdout);
}

static void show_timeouts(struct wdinfo *wd)
{
	if (wd->has_timeout)
		printf(_("%-15s%2i seconds\n"), _("Timeout:"), wd->timeout);
	if (wd->has_pretimeout)
		printf(_("%-15s%2i seconds\n"), _("Pre-timeout:"), wd->pretimeout);
	if (wd->has_timeleft)
		printf(_("%-15s%2i seconds\n"), _("Timeleft:"), wd->timeleft);
}

int main(int argc, char *argv[])
{
	struct wdinfo wd;
	int c, tt_flags = 0, res = EXIT_SUCCESS, count = 0;
	char noflags = 0, noident = 0, notimeouts = 0, oneline = 0;
	uint32_t wanted = 0;

	enum {
		EXCL_NONE,
		EXCL_FLAGS,
		EXCL_NOFLAGS
	};
	int excl_flag = EXCL_NONE;

	static const struct option long_opts[] = {
		{ "flags",      required_argument, NULL, 'f' },
		{ "flags-only", no_argument,       NULL, 'x' },
		{ "help",	no_argument,       NULL, 'h' },
		{ "noflags",    no_argument,       NULL, 'F' },
		{ "noheadings", no_argument,       NULL, 'n' },
		{ "noident",	no_argument,       NULL, 'I' },
		{ "notimeouts", no_argument,       NULL, 'T' },
		{ "output",     required_argument, NULL, 'o' },
		{ "oneline",    no_argument,       NULL, 'O' },
		{ "raw",        no_argument,       NULL, 'r' },
		{ "version",    no_argument,       NULL, 'V' },
		{ NULL, 0, NULL, 0 }
	};

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	atexit(close_stdout);

	while ((c = getopt_long(argc, argv,
				"d:f:hFnITo:OrVx", long_opts, NULL)) != -1) {
		switch(c) {
		case 'o':
			ncolumns = string_to_idarray(optarg,
						     columns, ARRAY_SIZE(columns),
						     column2id);
			if (ncolumns < 0)
				return EXIT_FAILURE;
			break;
		case 'f':
			exclusive_option(&excl_flag, EXCL_FLAGS, "--{flags,noflags}");
			if (string_to_bitmask(optarg, (unsigned long *) &wanted, name2bit) != 0)
				return EXIT_FAILURE;
			break;
		case 'V':
			printf(UTIL_LINUX_VERSION);
			return EXIT_SUCCESS;
		case 'h':
			usage(stdout);
		case 'F':
			exclusive_option(&excl_flag, EXCL_NOFLAGS, "--{flags,noflags}");
			noflags = 1;
			break;
		case 'I':
			noident = 1;
			break;
		case 'T':
			notimeouts = 1;
			break;
		case 'n':
			tt_flags |= TT_FL_NOHEADINGS;
			break;
		case 'r':
			tt_flags |= TT_FL_RAW;
			break;
		case 'O':
			oneline = 1;
			break;
		case 'x':
			noident = 1;
			notimeouts = 1;
			break;

		case '?':
		default:
			usage(stderr);
		}
	}

	if (!ncolumns) {
		/* default columns */
		columns[ncolumns++] = COL_FLAG;
		columns[ncolumns++] = COL_DESC;
		columns[ncolumns++] = COL_STATUS;
		columns[ncolumns++] = COL_BSTATUS;
	}

	do {
		int rc;

		memset(&wd, 0, sizeof(wd));

		if (optind == argc)
			wd.device = _PATH_WATCHDOG_DEV;
		else
			wd.device = argv[optind++];

		if (count)
			fputc('\n', stdout);
		count++;

		rc = read_watchdog(&wd);
		if (rc) {
			res = EXIT_FAILURE;
			continue;
		}

		if (oneline) {
			print_oneline(&wd, wanted, noident, notimeouts, noflags);
			continue;
		}

		/* pretty output */
		if (!noident) {
			printf("%-15s%s\n", _("Device:"), wd.device);
			printf(_("%-15s%s [version %x]\n"),
					("Identity:"),
					wd.ident.identity,
					wd.ident.firmware_version);
		}
		if (!notimeouts)
			show_timeouts(&wd);
		if (!noflags)
			show_flags(&wd, tt_flags, wanted);
	} while (optind < argc);

	return res;
}
