/*
 * powerp-txt.c - Model specific routines for CyberPower text
 *                protocol UPSes
 *
 * Copyright (C)
 *	2007        Doug Reynolds <mav@wastegate.net>
 *	2007-2008   Arjen de Korte <adkorte-guest@alioth.debian.org>
 *	2012-2016   Timothy Pearson <kb9vqf@pearsoncomputing.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/*
 * Throughout this driver, READ and WRITE comments are shown. These are
 * the typical commands to and replies from the UPS that was used for
 * decoding the protocol (with a serial logger).
 */

#include "main.h"
#include "serial.h"
#include "nut_stdint.h"

#include "powerp-txt.h"

#include <ctype.h>

#define POWERPANEL_TEXT_VERSION	"Powerpanel-Text 0.64"

typedef struct {
	float          i_volt;
	float          o_volt;
	int            o_load;
	int            b_chrg;
	unsigned char  has_u_temp;
	int            u_temp;
	float          i_freq;
	unsigned char  flags[2];
	unsigned char  has_b_volt;
	float          b_volt;
	unsigned char  has_o_freq;
	float          o_freq;
	unsigned char  has_runtime;
	int            runtime;
} status_t;

static long	ondelay = 1;	/* minutes */
static long	offdelay = 60;	/* seconds */

static char	powpan_answer[SMALLBUF];

static struct {
	const char	*var;
	const char	*get;
	const char	*set;
} vartab[] = {
	{ "input.transfer.high", "P6\r", "C2:%03d\r" },
	{ "input.transfer.low", "P7\r", "C3:%03d\r" },
	{ "battery.charge.low", "P8\r", "C4:%02d\r" },
	{ NULL, NULL, NULL }
};

static struct {
	const char	*cmd;
	const char	*command;
} cmdtab[] = {
	{ "test.battery.start.quick", "T\r" },
	{ "test.battery.start.deep", "TL\r" },
	{ "test.battery.stop", "CT\r" },
	{ "beeper.enable", "C7:1\r" },
	{ "beeper.disable", "C7:0\r" },
	{ "beeper.on", NULL },
	{ "beeper.off", NULL },
	{ "shutdown.stop", "C\r" },
	{ NULL, NULL }
};

static ssize_t powpan_command(const char *command)
{
	ssize_t	ret;

	ser_flush_io(upsfd);

	ret = ser_send_pace(upsfd, UPSDELAY, "%s", command);

	if (ret < 0) {
		upsdebug_with_errno(3, "send");
		return -1;
	}

	if (ret == 0) {
		upsdebug_with_errno(3, "send: timeout");
		return -1;
	}

	upsdebug_hex(3, "send", command, strlen(command));

	usleep(100000);

	ret = ser_get_line(upsfd, powpan_answer, sizeof(powpan_answer),
		ENDCHAR, IGNCHAR, SER_WAIT_SEC, SER_WAIT_USEC);

	if (ret < 0) {
		upsdebug_with_errno(3, "read");
		upsdebug_hex(4, "  \\_", powpan_answer, strlen(powpan_answer));
		return -1;
	}

	if (ret == 0) {
		upsdebugx(3, "read: timeout");
		upsdebug_hex(4, "  \\_", powpan_answer, strlen(powpan_answer));
		return -1;
	}

	upsdebug_hex(3, "read", powpan_answer, (size_t)ret);
	return ret;
}

static int powpan_instcmd(const char *cmdname, const char *extra)
{
	int	i;
	char	command[SMALLBUF];

	if (!strcasecmp(cmdname, "beeper.off")) {
		/* compatibility mode for old command */
		upslogx(LOG_WARNING,
			"The 'beeper.off' command has been renamed to 'beeper.disable'");
		return powpan_instcmd("beeper.disable", NULL);
	}

	if (!strcasecmp(cmdname, "beeper.on")) {
		/* compatibility mode for old command */
		upslogx(LOG_WARNING,
			"The 'beeper.on' command has been renamed to 'beeper.enable'");
		return powpan_instcmd("beeper.enable", NULL);
	}

	/* May be used in logging below, but not as a command argument */
	NUT_UNUSED_VARIABLE(extra);
	upsdebug_INSTCMD_STARTING(cmdname, extra);

	for (i = 0; cmdtab[i].cmd != NULL; i++) {

		if (strcasecmp(cmdname, cmdtab[i].cmd)) {
			continue;
		}

		upslog_INSTCMD_POWERSTATE_CHECKED(cmdname, extra);
		if ((powpan_command(cmdtab[i].command) == 2) && (!strcasecmp(powpan_answer, "#0"))) {
			return STAT_INSTCMD_HANDLED;
		}

		upslog_INSTCMD_FAILED(cmdname, extra);
		return STAT_INSTCMD_FAILED;
	}

	if (!strcasecmp(cmdname, "shutdown.return")) {
		upslog_INSTCMD_POWERSTATE_CHANGE(cmdname, extra);
		if (offdelay < 60) {
			snprintf(command, sizeof(command), "Z.%ld\r", offdelay / 6);
		} else {
			snprintf(command, sizeof(command), "Z%02ld\r", offdelay / 60);
		}
	} else if (!strcasecmp(cmdname, "shutdown.stayoff")) {
		upslog_INSTCMD_POWERSTATE_CHANGE(cmdname, extra);
		if (offdelay < 60) {
			snprintf(command, sizeof(command), "S.%ld\r", offdelay / 6);
		} else {
			snprintf(command, sizeof(command), "S%02ld\r", offdelay / 60);
		}
	} else if (!strcasecmp(cmdname, "shutdown.reboot")) {
		upslog_INSTCMD_POWERSTATE_CHANGE(cmdname, extra);
		if (offdelay < 60) {
			snprintf(command, sizeof(command), "S.%ldR%04ld\r", offdelay / 6, ondelay);
		} else {
			snprintf(command, sizeof(command), "S%02ldR%04ld\r", offdelay / 60, ondelay);
		}
	} else {
		upslog_INSTCMD_UNKNOWN(cmdname, extra);
		return STAT_INSTCMD_UNKNOWN;
	}

	if ((powpan_command(command) == 2) && (!strcasecmp(powpan_answer, "#0"))) {
		return STAT_INSTCMD_HANDLED;
	}

	upslog_INSTCMD_FAILED(cmdname, extra);
	return STAT_INSTCMD_FAILED;
}

static int powpan_setvar(const char *varname, const char *val)
{
	char	command[SMALLBUF];
	int 	i;

	upsdebug_SET_STARTING(varname, val);

	for (i = 0;  vartab[i].var != NULL; i++) {

		if (strcasecmp(varname, vartab[i].var)) {
			continue;
		}

		if (!strcasecmp(val, dstate_getinfo(varname))) {
			upslogx(LOG_INFO, "%s: [%s] no change for variable [%s]", __func__, val, varname);
			return STAT_SET_HANDLED;
		}

		snprintf_dynamic(command, sizeof(command), vartab[i].set, "%d", atoi(val));

		if ((powpan_command(command) == 2) && (!strcasecmp(powpan_answer, "#0"))) {
			dstate_setinfo(varname, "%s", val);
			return STAT_SET_HANDLED;
		}

		upslog_SET_FAILED(varname, val);
		return STAT_SET_UNKNOWN;	/* FIXME: ..._FAILED? */
	}

	upslog_SET_UNKNOWN(varname, val);
	return STAT_SET_UNKNOWN;
}

static void powpan_initinfo(void)
{
	int	i;
	char	*s;

	dstate_setinfo("ups.delay.start", "%ld", 60 * ondelay);
	dstate_setinfo("ups.delay.shutdown", "%ld", offdelay);

	/*
	 * NOTE: The reply is already in the buffer, since the P4\r command
	 * was used for autodetection of the UPS. No need to do it again.
	 */
	if ((s = strtok(&powpan_answer[1], ",")) != NULL) {
		dstate_setinfo("ups.model", "%s", str_rtrim(s, ' '));
	}
	if ((s = strtok(NULL, ",")) != NULL) {
		dstate_setinfo("ups.firmware", "%s", s);
	}
	if ((s = strtok(NULL, ",")) != NULL) {
		dstate_setinfo("ups.serial", "%s", s);
	}
	if ((s = strtok(NULL, ",")) != NULL) {
		dstate_setinfo("ups.mfr", "%s", str_rtrim(s, ' '));
	}

	/*
	 * WRITE P3\r
	 * READ #12.0,002,008.0,00\r
	 *      #12,2x1,12,0,1,8\r		(CST135XLU)
	 */
	if (powpan_command("P3\r") > 0) {

		if ((s = strtok(&powpan_answer[1], ",")) != NULL) {
			dstate_setinfo("battery.voltage.nominal", "%f", strtod(s, NULL));
		}
		if ((s = strtok(NULL, ",")) != NULL) {
			dstate_setinfo("battery.packs", "%li", strtol(s, NULL, 10));
		}
		if ((s = strtok(NULL, ",")) != NULL) {
			dstate_setinfo("battery.capacity", "%f", strtod(s, NULL));
		}
	}

	/*
	 * WRITE P2\r
	 * READ #1200,0720,120,47,63\r
	 *      #1350,810,120,57,63,11.3\r		(CST135XLU)
	 */
	if (powpan_command("P2\r") > 0) {

		if ((s = strtok(&powpan_answer[1], ",")) != NULL) {
			dstate_setinfo("ups.power.nominal", "%li", strtol(s, NULL, 10));
		}
		if ((s = strtok(NULL, ",")) != NULL) {
			dstate_setinfo("ups.realpower.nominal", "%li", strtol(s, NULL, 10));
		}
		if ((s = strtok(NULL, ",")) != NULL) {
			dstate_setinfo("input.voltage.nominal", "%li", strtol(s, NULL, 10));
		}
		if ((s = strtok(NULL, ",")) != NULL) {
			dstate_setinfo("input.frequency.low", "%li", strtol(s, NULL, 10));
		}
		if ((s = strtok(NULL, ",")) != NULL) {
			dstate_setinfo("input.frequency.high", "%li", strtol(s, NULL, 10));
		}
	}

	/*
	 * WRITE P1\r
	 * READ #120,138,088,20\r
	 *      #120,139,100,0,300\r		(CST135XLU)
	 */
	if (powpan_command("P1\r") > 0) {

		if ((s = strtok(&powpan_answer[1], ",")) != NULL) {
			dstate_setinfo("input.voltage.nominal", "%li", strtol(s, NULL, 10));
		}
		if ((s = strtok(NULL, ",")) != NULL) {
			dstate_setinfo("input.transfer.high", "%li", strtol(s, NULL, 10));
		}
		if ((s = strtok(NULL, ",")) != NULL) {
			dstate_setinfo("input.transfer.low", "%li", strtol(s, NULL, 10));
		}
		if ((s = strtok(NULL, ",")) != NULL) {
			dstate_setinfo("battery.charge.low", "%li", strtol(s, NULL, 10));
		}
	}

	for (i = 0; cmdtab[i].cmd != NULL; i++) {
		dstate_addcmd(cmdtab[i].cmd);
	}

	for (i = 0; vartab[i].var != NULL; i++) {

		if (!dstate_getinfo(vartab[i].var)) {
			continue;
		}

		if (powpan_command(vartab[i].get) < 1) {
			continue;
		}

		if ((s = strtok(&powpan_answer[1], ",")) != NULL) {
			dstate_setflags(vartab[i].var, ST_FLAG_RW);
			dstate_addenum(vartab[i].var, "%li", strtol(s, NULL, 10));
		}

		while ((s = strtok(NULL, ",")) != NULL) {
			dstate_addenum(vartab[i].var, "%li", strtol(s, NULL, 10));
		}
	}

	/*
	 * WRITE P5\r
	 * READ #<unknown>\r
	 */
	if (powpan_command("P5\r") > 0) {
		/*
		 * Looking at the format of the commands "P<n>\r" it seems likely
		 * that this command exists also. Let's see if someone cares to
		 * tell us if it does (should be visible when running with -DDDDD).
		 */
	}

	/*
	 * WRITE P9\r
	 * READ #<unknown>\r
	 */
	if (powpan_command("P9\r") > 0) {
		/*
		 * Looking at the format of the commands "P<n>\r" it seems likely
		 * that this command exists also. Let's see if someone cares to
		 * tell us if it does (should be visible when running with -DDDDD).
		 */
	}

	/*
	 * Cancel pending shutdown.
	 * WRITE C\r
	 * READ #0\r
	*/
	powpan_command("C\r");

	dstate_addcmd("shutdown.return");
	dstate_addcmd("shutdown.stayoff");
	dstate_addcmd("shutdown.reboot");
}

static ssize_t powpan_status(status_t *status)
{
	ssize_t	ret;
	ssize_t i;
	ssize_t valid = 0;
	int code = -1;
	char value[32];
	ssize_t ofs = 0;
	char seen_i_freq = 0;

	memset(status, 0, sizeof(status_t));

	/*
	 * WRITE D\r
	 * READ #I119.0O119.0L000B100T027F060.0S..\r
	 *      #I118.0O118.0L029B100F060.0R0218S..\r
	 *      #I118.1O118.1L13B100V27.5F60.0HF60.0R65Q1.4S\x80\x84\xc0\x88\x80W\r		(CST135XLU)
	 *      01234567890123456789012345678901234567890123
	 *      0         1         2         3         4
	 */
	ret = powpan_command("D\r");
	if (ret <= 0)
		return -1;

	if (powpan_answer[0] != '#') {
		upsdebugx(2, "Expected start character '#', but got '%c'", powpan_answer[0]);
		return -1;
	}

	for (i = 1; i <= ret; i++) {
		if (i == ret || isalpha((unsigned char)(powpan_answer[i]))) {
			value[ofs++] = '\0';
			valid++;

			switch (code) {
				case 'I':
					status->i_volt = strtof(value, NULL);
					break;
				case 'O':
					status->o_volt = strtof(value, NULL);
					break;
				case 'L':
					status->o_load = strtol(value, NULL, 10);
					break;
				case 'B':
					status->b_chrg = strtol(value, NULL, 10);
					break;
				case 'V':
					status->b_volt = strtof(value, NULL);
					status->has_b_volt = 1;
					break;
				case 'T':
					status->u_temp = strtol(value, NULL, 10);
					status->has_u_temp = 1;
					break;
				case 'F':
					status->i_freq = strtof(value, NULL);
					seen_i_freq = 1;
					break;
				case 'H':
					status->o_freq = strtof(value, NULL);
					status->has_o_freq = 1;
					break;
				case 'R':
					status->runtime = strtol(value, NULL, 10);
					status->has_runtime = 1;
					break;
				case 'S':
					memcpy(&status->flags, value, 2);
					break;
				default:
					/* We didn't really find valid data */
					valid--;
					break;
			}

			code = powpan_answer[i];
			ofs = 0;

			/*
				* Depending on device/firmware, the output frequency is coded as
				* either an H, HF, or a second F (seen once transfered to battery)
				*/
			if (seen_i_freq && code == 'F')
				code = 'H';

			continue;
		}

		value[ofs++] = powpan_answer[i];
	}

	/* if we didn't get at least 3 values consider it a failure */
	if (valid < 3) {
		upsdebugx(4, "Parsing status string failed");
		return -1;
	}

	return 0;
}

static int powpan_updateinfo(void)
{
	status_t	status;

	if (powpan_status(&status)) {
		return -1;
	}

	dstate_setinfo("input.voltage", "%.1f", status.i_volt);
	dstate_setinfo("output.voltage", "%.1f", status.o_volt);
	dstate_setinfo("ups.load", "%d", status.o_load);
	dstate_setinfo("input.frequency", "%.1f", status.i_freq);
	if (status.has_u_temp) {
		dstate_setinfo("ups.temperature", "%d", status.u_temp);
	}
	dstate_setinfo("battery.charge", "%d", status.b_chrg);
	if (status.has_b_volt) {
		dstate_setinfo("battery.voltage", "%.1f", status.b_volt);
		if (status.b_volt > 20.0 && status.b_volt < 28.0) {
			dstate_setinfo("battery.voltage.nominal", "%f", 24.0);
		}
	}
	if (status.has_o_freq) {
		dstate_setinfo("output.frequency", "%.1f", status.o_freq);
	}
	if (status.has_runtime) {
		dstate_setinfo("battery.runtime", "%d", status.runtime*60);
	}

	status_init();

	if (status.flags[0] & 0x40) {
		status_set("OB");
	} else {
		status_set("OL");
	}

	if (status.flags[0] & 0x20) {
		status_set("LB");
	}

	/* !OB && !TEST */
	if (!(status.flags[0] & 0x48)) {

		if (status.o_volt < 0.5 * status.i_volt) {
			upsdebugx(2, "%s: output voltage too low", __func__);
		} else if (status.o_volt < 0.95 * status.i_volt) {
			status_set("TRIM");
		} else if (status.o_volt < 1.05 * status.i_volt) {
			/* ignore */
		} else if (status.o_volt < 1.5 * status.i_volt) {
			status_set("BOOST");
		} else {
			upsdebugx(2, "%s: output voltage too high", __func__);
		}
	}

	if (status.flags[0] & 0x08) {
		status_set("TEST");
	}

	if (status.flags[0] == 0) {
		status_set("OFF");
	}

	status_commit();

	return (status.flags[0] & 0x40) ? 1 : 0;
}

static ssize_t powpan_initups(void)
{
	ssize_t	ret;
	int	i;

	upsdebugx(1, "Trying text protocol...");

	ser_set_speed(upsfd, device_path, B2400);

	/* This fails for many devices, so don't bother to complain */
	powpan_command("\r\r");

	for (i = 0; i < MAXTRIES; i++) {

		const char	*val;

		/*
		 * WRITE P4\r
		 * READ #BC1200     ,1.600,000000000000,CYBER POWER
		 *      #CST135XLU,BF01403AAH2,CR7EV2002320,CyberPower Systems Inc.,,,
		 *      01234567890123456789012345678901234567890123456
		 *      0         1         2         3         4
		 */
		ret = powpan_command("P4\r");

		if (ret < 1) {
			continue;
		}

		if (ret < 46) {
			upsdebugx(2, "Expected 46 bytes, but only got %" PRIiSIZE, ret);
			continue;
		}

		if (powpan_answer[0] != '#') {
			upsdebugx(2, "Expected start character '#', but got '%c'", powpan_answer[0]);
			continue;
		}

		val = getval("ondelay");
		if (val) {
			ondelay = strtol(val, NULL, 10);
		}

		if ((ondelay < 0) || (ondelay > 9999)) {
			fatalx(EXIT_FAILURE, "Start delay '%ld' out of range [0..9999]", ondelay);
		}

		val = getval("offdelay");
		if (val) {
			offdelay = strtol(val, NULL, 10);
		}

		if ((offdelay < 6) || (offdelay > 600)) {
			fatalx(EXIT_FAILURE, "Shutdown delay '%ld' out of range [6..600]", offdelay);
		}

		/* Truncate to nearest setable value */
		if (offdelay < 60) {
			offdelay -= (offdelay % 6);
		} else {
			offdelay -= (offdelay % 60);
		}

		return ret;
	}

	return -1;
}

subdriver_t powpan_text = {
	"text",
	POWERPANEL_TEXT_VERSION,
	powpan_instcmd,
	powpan_setvar,
	powpan_initups,
	powpan_initinfo,
	powpan_updateinfo
};
