/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2015-2017 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 *
 * This is a shell script written in C. It very intentionally still functions like
 * a shell script, calling out to external executables such as ip(8).
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <regex.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>

#define _printf_(x, y) __attribute__((format(printf, x, y)))
#define _cleanup_(x) __attribute__((cleanup(x)))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

static bool is_exiting = false;

static void *xmalloc(size_t size)
{
	void *ret = malloc(size);
	if (ret)
		return ret;
	perror("Error: malloc");
	exit(errno);
}

static void *xstrdup(const char *str)
{
	char *ret = strdup(str);
	if (ret)
		return ret;
	perror("Error: strdup");
	exit(errno);
}

static void xregcomp(regex_t *preg, const char *regex, int cflags)
{
	char error[4096];
	int ret;

	ret = regcomp(preg, regex, cflags);
	if (!ret)
		return;
	regerror(ret, preg, error, sizeof(error));
	fprintf(stderr, "Error: Regex compilation error: %s\n", error);
	exit(88);
}

static char *concat(char *first, ...)
{
	va_list args;
	size_t len = 0;
	char *ret;

	va_start(args, first);
	for (char *i = first; i; i = va_arg(args, char *))
		len += strlen(i);
	va_end(args);

	ret = xmalloc(len + 1);
	ret[0] = 0;

	va_start(args, first);
	for (char *i = first; i; i = va_arg(args, char *))
		strcat(ret, i);
	va_end(args);

	return ret;
}

struct command_buffer {
	char *line;
	size_t len;
	FILE *stream;
};

static void free_command_buffer(struct command_buffer *c)
{
	if (!c)
		return;
	if (c->stream)
		pclose(c->stream);
	free(c->line);
}

static void freep(void *p)
{
	free(*(void **)p);
}
static void fclosep(FILE **f)
{
	if (*f)
		fclose(*f);
}
#define _cleanup_free_ _cleanup_(freep)
#define _cleanup_fclose_ _cleanup_(fclosep)

#define DEFINE_CMD(name) _cleanup_(free_command_buffer) struct command_buffer name = { 0 };

static char *vcmd_ret(struct command_buffer *c, const char *cmd_fmt, va_list args)
{
	char cmd[8192];

	if (!c->stream && !cmd_fmt)
		return NULL;
	if (c->stream && cmd_fmt)
		pclose(c->stream);

	if (cmd_fmt) {
		if (vsnprintf(cmd, sizeof(cmd), cmd_fmt, args) >= sizeof(cmd)) {
			errno = E2BIG;
			perror("Error: vsnprintf");
			exit(errno);
		}

		c->stream = popen(cmd, "r");
		if (!c->stream) {
			perror("Error: popen");
			exit(errno);
		}
	}
	if (getline(&c->line, &c->len, c->stream) < 0) {
		if (errno) {
			perror("Error: getline");
			exit(errno);
		}
		return NULL;
	}
	return c->line;
}

_printf_(1, 2) static void cmd(const char *cmd_fmt, ...)
{
	char cmd[8192];
	va_list args;
	int ret;

	va_start(args, cmd_fmt);
	if (vsnprintf(cmd, sizeof(cmd), cmd_fmt, args) >= sizeof(cmd)) {
		errno = E2BIG;
		perror("Error: vsnprintf");
		exit(errno);
	}
	va_end(args);

	printf("[#] %s\n", cmd);
	ret = system(cmd);

	if (ret && !is_exiting)
		exit(ret);
}

_printf_(2, 3) static char *cmd_ret(struct command_buffer *c, const char *cmd_fmt, ...)
{
	va_list args;
	char *ret;

	va_start(args, cmd_fmt);
	ret = vcmd_ret(c, cmd_fmt, args);
	va_end(args);
	return ret;
}

_printf_(1, 2) static void cndc(const char *cmd_fmt, ...)
{
	DEFINE_CMD(c);
	char *ret;
	va_list args;
	_cleanup_free_ char *ndc_fmt = concat("ndc ", cmd_fmt, NULL);

	va_start(args, cmd_fmt);
	printf("[#] ");
	vprintf(ndc_fmt, args);
	printf("\n");
	va_end(args);

	va_start(args, cmd_fmt);
	ret = vcmd_ret(&c, ndc_fmt, args);
	va_end(args);

	if (!ret || !strstr(ret, "200 0")) {
		if (ret)
			fprintf(stderr, "Error: %s\n", ret);
		exit(29);
	}
}

static void auto_su(int argc, char *argv[])
{
	char *args[argc + 4];

	if (!getuid())
		return;

	args[0] = "su";
	args[1] = "-p";
	args[2] = "-c";
	memcpy(&args[3], argv, argc * sizeof(*args));
	args[argc + 3] = NULL;

	printf("[$] su -p -c wg-quick\n");
	execvp("su", args);
	exit(errno);
}

static void add_if(const char *iface)
{
	cmd("ip link add %s type wireguard", iface);
}

static void del_if(const char *iface)
{
	DEFINE_CMD(c);
	regex_t reg;
	char *netid = NULL;
	_cleanup_free_ char *regex = concat("0xc([0-9a-f]+)/0xcffff lookup ", iface, NULL);

	xregcomp(&reg, regex, REG_EXTENDED);

	cmd("ip link del %s", iface);
	for (char *ret = cmd_ret(&c, "ip rule show"); ret; ret = cmd_ret(&c, NULL)) {
		regmatch_t matches[2];

		if (!regexec(&reg, ret, ARRAY_SIZE(matches), matches, 0)) {
			ret[matches[1].rm_eo] = 0;
			netid = &ret[matches[1].rm_so];
			break;
		}
	}

	if (netid)
		cndc("network destroy %lu", strtoul(netid, NULL, 16));
}

static void up_if(unsigned int *netid, const char *iface)
{
	srandom(time(NULL) ^ getpid()); /* Not real randomness. */

	while (*netid < 4096)
		*netid = random() & 0xfffe;

	cmd("wg set %s fwmark 0x20000", iface);
	cndc("interface setcfg %s up", iface);
	cndc("network create %u vpn 1 1", *netid);
	cndc("network interface add %u %s", *netid, iface);
	cndc("network users add %u 0-99999", *netid);
}

static void set_dnses(unsigned int netid, const char *dnses)
{
	size_t len = strlen(dnses);
	_cleanup_free_ char *mutable = xstrdup(dnses);
	_cleanup_free_ char *arglist = xmalloc(len * 4 + 1);
	_cleanup_free_ char *arg = xmalloc(len + 4);

	if (!len)
		return;
	arglist[0] = 0;

	for (char *dns = strtok(mutable, ", \t\n"); dns; dns = strtok(NULL, ", \t\n")) {
		if (strchr(dns, '\'') || strchr(dns, '\\'))
			continue;
		snprintf(arg, len + 3, "'%s' ", dns);
		strncat(arglist, arg, len * 4 - 1);
	}
	if (!strlen(arglist))
		return;
	cndc("resolver setnetdns %u '' %s", netid, arglist);
}

static void add_addr(const char *iface, const char *addr)
{
	if (strchr(addr, ':')) {
		cndc("interface ipv6 %s enable", iface);
		cmd("ip -6 addr add '%s' dev %s", addr, iface);
	} else {
		_cleanup_free_ char *mutable = strdup(addr);
		char *slash = strchr(mutable, '/');
		unsigned char mask = 32;

		if (slash) {
			*slash = 0;
			mask = atoi(slash + 1);
		}
		cndc("interface setcfg %s '%s' %u", iface, mutable, mask);
	}
}

static void set_addr(const char *iface, const char *addrs)
{
	_cleanup_free_ char *mutable = xstrdup(addrs);

	for (char *addr = strtok(mutable, ", \t\n"); addr; addr = strtok(NULL, ", \t\n")) {
		if (strchr(addr, '\'') || strchr(addr, '\\'))
			continue;
		add_addr(iface, addr);
	}
}

static int get_route_mtu(const char *endpoint)
{
	DEFINE_CMD(c_route);
	DEFINE_CMD(c_dev);
	regmatch_t matches[2];
	regex_t regex_mtu, regex_dev;
	char *route, *mtu, *dev;

	xregcomp(&regex_mtu, "mtu ([0-9]+)", REG_EXTENDED);
	xregcomp(&regex_dev, "dev ([^ ]+)", REG_EXTENDED);

	if (strcmp(endpoint, "default"))
		route = cmd_ret(&c_route, "ip -o route get %s", endpoint);
	else
		route = cmd_ret(&c_route, "ip -o route show %s", endpoint);
	if (!route)
		return -1;

	if (!regexec(&regex_mtu, route, ARRAY_SIZE(matches), matches, 0)) {
		route[matches[1].rm_eo] = 0;
		mtu = &route[matches[1].rm_so];
	} else if (!regexec(&regex_dev, route, ARRAY_SIZE(matches), matches, 0)) {
		route[matches[1].rm_eo] = 0;
		dev = &route[matches[1].rm_so];
		route = cmd_ret(&c_dev, "ip -o link show dev %s", dev);
		if (!route)
			return -1;
		if (regexec(&regex_mtu, route, ARRAY_SIZE(matches), matches, 0))
			return -1;
		route[matches[1].rm_eo] = 0;
		mtu = &route[matches[1].rm_so];
	} else
		return -1;
	return atoi(mtu);
}

static void set_mtu(const char *iface, unsigned int mtu)
{
	DEFINE_CMD(c_endpoints);
	regex_t regex_endpoint;
	int endpoint_mtu;

	if (mtu) {
		cndc("interface setmtu %s %u", iface, mtu);
		return;
	}

	xregcomp(&regex_endpoint, "^\\[?([a-z0-9:.]+)\\]?:[0-9]+$", REG_EXTENDED);

	endpoint_mtu = get_route_mtu("default");
	if (endpoint_mtu == -1)
		endpoint_mtu = 1500;

	for (char *endpoint = cmd_ret(&c_endpoints, "wg show %s endpoints", iface); endpoint; endpoint = cmd_ret(&c_endpoints, NULL)) {
		regmatch_t matches[2];
		unsigned int next_mtu;

		if (regexec(&regex_endpoint, endpoint, ARRAY_SIZE(matches), matches, 0))
			continue;
		endpoint[matches[1].rm_eo] = 0;
		endpoint = &endpoint[matches[1].rm_so];

		next_mtu = get_route_mtu(endpoint);
		if (next_mtu > 0 && next_mtu < endpoint_mtu)
			endpoint_mtu = next_mtu;
	}

	cndc("interface setmtu %s %d", iface, endpoint_mtu - 80);
}

static void add_route(const char *iface, unsigned int netid, const char *route)
{
	cndc("network route add %u %s %s", netid, iface, route);
}

static void set_routes(const char *iface, unsigned int netid)
{
	DEFINE_CMD(c);

	for (char *allowedips = cmd_ret(&c, "wg show %s allowed-ips", iface); allowedips; allowedips = cmd_ret(&c, NULL)) {
		char *start = strchr(allowedips, '\t');
		if (!start)
			continue;
		++start;
		for (char *allowedip = strtok(start, " \n"); allowedip; allowedip = strtok(NULL, " \n"))
			add_route(iface, netid, allowedip);
	}
}

static void set_config(const char *iface, const char *config)
{
	FILE *config_writer;
	_cleanup_free_ char *cmd = concat("wg setconf ", iface, " /proc/self/fd/0", NULL);

	printf("[#] %s\n", cmd);

	config_writer = popen(cmd, "w");
	if (!config_writer) {
		perror("Error: popen");
		exit(errno);
	}
	if (fputs(config, config_writer) < 0) {
		perror("Error: fputs");
		exit(errno);
	}
	pclose(config_writer);
}

static void cmd_usage(const char *program)
{
	printf( "Usage: %s [ up | down ] [ CONFIG_FILE | INTERFACE ]\n"
		"\n"
		"  CONFIG_FILE is a configuration file, whose filename is the interface name\n"
		"  followed by `.conf'. Otherwise, INTERFACE is an interface name, with\n"
		"  configuration found at /data/misc/wireguard/INTERFACE.conf. It is to be readable\n"
		"  by wg(8)'s `setconf' sub-command, with the exception of the following additions\n"
		"  to the [Interface] section, which are handled by %s:\n"
		"\n"
		"  - Address: may be specified one or more times and contains one or more\n"
		"    IP addresses (with an optional CIDR mask) to be set for the interface.\n"
		"  - MTU: an optional MTU for the interface; if unspecified, auto-calculated.\n"
		"  - DNS: an optional DNS server to use while the device is up.\n"
		"\n"
		"See wg-quick(8) for more info and examples.\n", program, program);
}

static char *cleanup_iface = NULL;

static void cmd_up_cleanup(void)
{
	is_exiting = true;
	if (cleanup_iface)
		del_if(cleanup_iface);
	free(cleanup_iface);
}

static void cmd_up(const char *iface, const char *config, unsigned int mtu, const char *addrs, const char *dnses)
{
	DEFINE_CMD(c);
	unsigned int netid = 0;

	if (cmd_ret(&c, "ip link show dev %s 2>/dev/null", iface)) {
		fprintf(stderr, "Error: %s already exists\n", iface);
		exit(92);
	}

	cleanup_iface = xstrdup(iface);
	atexit(cmd_up_cleanup);

	add_if(iface);
	set_config(iface, config);
	set_mtu(iface, mtu);
	set_addr(iface, addrs);
	up_if(&netid, iface);
	set_dnses(netid, dnses);
	set_routes(iface, netid);

	free(cleanup_iface);
	cleanup_iface = NULL;
	exit(0);
}

static void cmd_down(const char *iface)
{
	DEFINE_CMD(c);
	bool found = false;

	char *ifaces = cmd_ret(&c, "wg show interfaces");
	if (ifaces) {
		for (char *eiface = strtok(ifaces, " \n"); eiface; eiface = strtok(NULL, " \n")) {
			if (!strcmp(iface, eiface)) {
				found = true;
				break;
			}
		}
	}
	if (!found) {
		fprintf(stderr, "Error: %s is not a WireGuard interface\n", iface);
		exit(43);
	}

	del_if(iface);
	exit(0);
}

static char *concat_and_free(char *orig, const char *delim, const char *new_line)
{
	char *ret;

	if (!orig)
		ret = xstrdup(new_line);
	else
		ret = concat(orig, delim, new_line, NULL);
	free(orig);
	return ret;
}

static void parse_options(char **iface, char **config, unsigned int *mtu, char **addrs, char **dnses, const char *arg)
{
	_cleanup_fclose_ FILE *file = NULL;
	char filename[PATH_MAX + 1], *line = NULL;
	regex_t regex_iface, regex_conf;
	regmatch_t matches[2];
	struct stat sbuf;
	size_t n = 0;
	bool in_interface_section = false;

	*iface = *config = *addrs = *dnses = NULL;
	*mtu = 0;

	xregcomp(&regex_iface, "^[a-zA-Z0-9_=+.-]{1,16}$", REG_EXTENDED | REG_NOSUB);
	xregcomp(&regex_conf, "/?([a-zA-Z0-9_=+.-]{1,16})\\.conf$", REG_EXTENDED);

	filename[sizeof(filename) - 1] = 0;
	if (!regexec(&regex_iface, arg, 0, NULL, 0))
		snprintf(filename, sizeof(filename) - 1, "/data/misc/wireguard/%s.conf", arg);
	else
		strncpy(filename, arg, sizeof(filename) - 1);

	file = fopen(filename, "r");
	if (!file) {
		fprintf(stderr, "Error: Unable to open configuration file `%s': %s\n", filename, strerror(errno));
		exit(errno);
	}

	if (regexec(&regex_conf, filename, ARRAY_SIZE(matches), matches, 0)) {
		fprintf(stderr, "Error: The config file must be a valid interface name, followed by .conf\n");
		exit(77);
	}

	if (fstat(fileno(file), &sbuf) < 0) {
		perror("Error: fstat");
		exit(errno);
	}
	if (sbuf.st_mode & 0007)
		fprintf(stderr, "Warning: `%s' is world accessible\n", filename);

	filename[matches[1].rm_eo] = 0;
	*iface = xstrdup(&filename[matches[1].rm_so]);

	while (getline(&line, &n, file) >= 0) {
		size_t len = strlen(line), j = 0;
		_cleanup_free_ char *clean = xmalloc(len + 1);

		for (size_t i = 0; i < len; ++i) {
			if (!isspace(line[i]))
				clean[j++] = line[i];
		}
		clean[j] = 0;
		len = strlen(clean);

		if (clean[0] == '[')
			in_interface_section = false;
		if (!strcasecmp(clean, "[Interface]"))
			in_interface_section = true;
		if (in_interface_section) {
			if (!strncasecmp(clean, "Address=", 8) && len > 8) {
				*addrs = concat_and_free(*addrs, ",", clean + 8);
				continue;
			} else if (!strncasecmp(clean, "DNS=", 4) && len > 4) {
				*dnses = concat_and_free(*dnses, ",", clean + 4);
				continue;
			} else if (!strncasecmp(clean, "MTU=", 4) && len > 4) {
				*mtu = atoi(clean + 4);
				continue;
			}
		}
		*config = concat_and_free(*config, "", line);
	}

	if (!*iface)
		*iface = xstrdup("");
	if (!*config)
		*config = xstrdup("");
	if (!*addrs)
		*addrs = xstrdup("");
	if (!*dnses)
		*dnses = xstrdup("");
}

int main(int argc, char *argv[])
{
	_cleanup_free_ char *iface = NULL;
	_cleanup_free_ char *config = NULL;
	_cleanup_free_ char *addrs = NULL;
	_cleanup_free_ char *dnses = NULL;
	unsigned int mtu;

	if (argc == 2 && (!strcmp(argv[1], "help") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")))
		cmd_usage(argv[0]);
	else if (argc == 3 && !strcmp(argv[1], "up")) {
		auto_su(argc, argv);
		parse_options(&iface, &config, &mtu, &addrs, &dnses, argv[2]);
		cmd_up(iface, config, mtu, addrs, dnses);
	} else if (argc == 3 && !strcmp(argv[1], "down")) {
		auto_su(argc, argv);
		parse_options(&iface, &config, &mtu, &addrs, &dnses, argv[2]);
		cmd_down(iface);
	} else {
		cmd_usage(argv[0]);
		return 1;
	}
	return 0;
}