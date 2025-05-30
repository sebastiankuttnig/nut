/*
 * apcsmart.c - driver for APC smart protocol units (originally "newapc")
 *
 * Copyright (C) 1999  Russell Kroll <rkroll@exploits.org>
 *           (C) 2000  Nigel Metheringham <Nigel.Metheringham@Intechnology.co.uk>
 *           (C) 2011+ Michal Soltys <soltys@ziu.info>
 *           (C) 2024  Jim Klimov <jimklimov+nut@gmail.com>
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

#include "main.h"	/* Must be first, includes "config.h" */

#include <sys/types.h>
#include <sys/file.h>
#include <regex.h>
#include <ctype.h>
#include <strings.h> /* strcasecmp() */

#include "serial.h"
#include "timehead.h"
#include "nut_stdint.h"

#include "apcsmart.h"
#include "apcsmart_tabs.h"

#define DRIVER_NAME	"APC Smart protocol driver"
#define DRIVER_VERSION	"3.36"

#ifdef WIN32
# ifndef ECANCELED
#  define ECANCELED ERROR_CANCELLED
# endif
#endif	/* WIN32 */

/* driver description structure */
upsdrv_info_t upsdrv_info = {
	DRIVER_NAME,
	DRIVER_VERSION,
	"Russell Kroll <rkroll@exploits.org>\n"
	"Nigel Metheringham <Nigel.Metheringham@Intechnology.co.uk>\n"
	"Michal Soltys <soltys@ziu.info>",
	DRV_STABLE,
	{ &apc_tab_info, NULL }
};

static long ups_status = 0;

/* some forwards */

static int sdcmd_S(const void *);
static int sdcmd_AT(const void *);
static int sdcmd_K(const void *);
static int sdcmd_Z(const void *);
static int sdcmd_CS(const void *);

/*
 * following table *must* match order defined in the man page, namely:
 * 0:: soft hibernate (*S*)
 * 1:: hard hibernate (*@*)
 * 2:: delayed poweroff (*K*)
 * 3:: instant poweroff (*Z*)
 * 4:: "force OB hack" (*CS*)
 */

static int (*sdlist[])(const void *) = {
	sdcmd_S,
	sdcmd_AT,
	sdcmd_K,
	sdcmd_Z,
	sdcmd_CS,
};

#define SDIDX_AT	1

/*
 * note: both lookup functions MUST be used after variable detection is
 * completed - that is after deprecate_vars() call; the general reason for this
 * is 1:n and n:1 NUT <-> APC mappings, which are not determined prior to the
 * detection
 */
static apc_vartab_t *vt_lookup_char(char cmdchar)
{
	int	i;

	for (i = 0; apc_vartab[i].name != NULL; i++)
		if ((apc_vartab[i].flags & APC_PRESENT) &&
		    apc_vartab[i].cmd == cmdchar)
			return &apc_vartab[i];

	return NULL;
}

static apc_vartab_t *vt_lookup_name(const char *var)
{
	int	i;

	for (i = 0; apc_vartab[i].name != NULL; i++)
		if ((apc_vartab[i].flags & APC_PRESENT) &&
		    !strcasecmp(apc_vartab[i].name, var))
			return &apc_vartab[i];

	return NULL;
}

static const char *prtchr(char x)
{
	static size_t curr = 24;
	static char info[32];

	curr = (curr + 8) & 0x1F;
	snprintf(info + curr, 8, isprint((size_t)x) ? "%c" : "0x%02x", x);

	return info + curr;
}

static int rexhlp(const char *rex, const char *val)
{
	static const char *empty = "";
	int ret;
	regex_t mbuf;

	if (!rex || !*rex)
		return 1;
	if (!val)
		val = empty;
	regcomp(&mbuf, rex, REG_EXTENDED|REG_NOSUB);
	ret = regexec(&mbuf, val, 0, 0, 0);
	regfree(&mbuf);
	return !ret;
}

/* convert APC formatting to NUT formatting */
/* TODO: handle errors better */
static const char *convert_data(apc_vartab_t *vt, const char *upsval)
{
	static char temp[APC_LBUF];
	long tval;

	memset(temp, '\0', sizeof(temp));

	/* this should never happen */
	if (strlen(upsval) >= sizeof(temp)) {
		upslogx(LOG_CRIT, "%s: the length of [%s] is too big", __func__, vt->name);
		memcpy(temp, upsval, sizeof(temp) - 1);
		temp[sizeof(temp) - 1] = '\0';
		return temp;
	}

	switch (vt->flags & APC_F_MASK) {
		case APC_F_PERCENT:
		case APC_F_VOLT:
		case APC_F_AMP:
		case APC_F_CELSIUS:
		case APC_F_HEX:
		case APC_F_DEC:
		case APC_F_SECONDS:
		case APC_F_LEAVE:
			/* no conversion for any of these */
			strncpy(temp, upsval, sizeof(temp) - 1);
			temp[sizeof(temp) - 1] = '\0';
			return temp;

		case APC_F_HOURS:
			/* convert to seconds */

			tval = 60 * 60 * strtol(upsval, NULL, 10);

			snprintf(temp, sizeof(temp), "%ld", tval);
			return temp;

		case APC_F_MINUTES:
			/* Convert to seconds - NUT standard time measurement */
			tval = 60 * strtol(upsval, NULL, 10);
			/* Ignore errors - there's not much we can do */
			snprintf(temp, sizeof(temp), "%ld", tval);
			return temp;

		case APC_F_REASON:
			switch (upsval[0]) {
				case 'R': return "unacceptable utility voltage rate of change";
				case 'H': return "high utility voltage";
				case 'L': return "low utility voltage";
				case 'T': return "line voltage notch or spike";
				case 'O': return "no transfers yet since turnon";
				case 'S': return "simulated power failure or UPS test";
				default:
					strncpy(temp, upsval, sizeof(temp) - 1);
					temp[sizeof(temp) - 1] = '\0';
					return temp;
			}

		default:
			break;
	}

	/* this should never happen */
	upslogx(LOG_CRIT, "%s: unable to convert [%s]", __func__, vt->name);
	strncpy(temp, upsval, sizeof(temp) - 1);
	temp[sizeof(temp) - 1] = '\0';
	return temp;
}

/* report differences if tcsetattr != tcgetattr, return otherwise */

/*
 * AIX compatible names
 */
#if defined(VWERSE) && !defined(VWERASE)
#define VWERASE VWERSE
#endif /* VWERSE && !VWERASE */

#if defined(VDISCRD) && !defined(VDISCARD)
#define VDISCARD VDISCRD
#endif /* VDISCRD && !VDISCARD */

static void apc_ser_diff(struct termios *tioset, struct termios *tioget)
{
	size_t i;
	const char dir[] = { 's', 'g' };
	struct termios *tio[] = { tioset, tioget };
	struct cchar {
		const char *name;
		unsigned int sub;
	};
	const struct cchar cchars1[] = {
#ifdef VDISCARD
		{ "discard",	VDISCARD	},
#endif
#ifdef VDSUSP
		{ "dsusp",	VDSUSP		},
#endif
		{ "eof",	VEOF		},
		{ "eol",	VEOL		},
		{ "eol2",	VEOL2		},
		{ "erase",	VERASE		},
#ifdef VINTR
		{ "intr",	VINTR		},
#endif
		{ "kill",	VKILL		},
		{ "lnext",	VLNEXT		},
		{ "min",	VMIN		},
		{ "quit",	VQUIT		},
#ifdef VREPRINT
		{ "reprint",	VREPRINT	},
#endif
		{ "start",	VSTART		},
#ifdef VSTATUS
		{ "status",	VSTATUS		},
#endif
		{ "stop",	VSTOP		},
		{ "susp",	VSUSP		},
		{ "time",	VTIME		},
		{ "werase",	VWERASE		},
		{ NULL, 0 },
	}, *cp;

	/* clear status flags so that they don't affect our binary compare */
#if defined(PENDIN) || defined(FLUSHO)
	for (i = 0; i < SIZEOF_ARRAY(tio); i++) {
#ifdef PENDIN
		tio[i]->c_lflag &= ~(unsigned int)PENDIN;
#endif
#ifdef FLUSHO
		tio[i]->c_lflag &= ~(unsigned int)FLUSHO;
#endif
	}
#endif /* defined(PENDIN) || defined(FLUSHO) */

	if (!memcmp(tio[0], tio[1], sizeof(*tio[0])))
		return;

	upslogx(LOG_NOTICE, "%s: device reports different attributes than requested", device_path);

	/*
	 * According to the manual the most common problem is mismatched
	 * combinations of input and output baud rates. If the combination is
	 * not supported then neither are changed. This should not be a
	 * problem here since we set them both to the same extremely common
	 * rate of 2400.
	 */

	for (i = 0; i < SIZEOF_ARRAY(tio); i++) {
		upsdebugx(1,
			"%s: tc%cetattr(): gfmt1:cflag=%x:iflag=%x:lflag=%x:oflag=%x:",
			__func__, dir[i],
			(unsigned int) tio[i]->c_cflag, (unsigned int) tio[i]->c_iflag,
			(unsigned int) tio[i]->c_lflag, (unsigned int) tio[i]->c_oflag);
		for (cp = cchars1; cp->name; ++cp)
			upsdebugx(1, "%s: \t%s=%x:",
				__func__, cp->name, tio[i]->c_cc[cp->sub]);
		upsdebugx(1, "%s: \tispeed=%d:ospeed=%d",
			__func__,
			(int) cfgetispeed(tio[i]),
			(int) cfgetospeed(tio[i]));
	}
}

static void apc_ser_set(void)
{
	struct termios tio, tio_chk;
	char *val;

	/*
	 * this must be called before the rest, as ser_set_speed() performs
	 * early initialization of the port, apart from changing speed
	 */
	ser_set_speed(upsfd, device_path, B2400);

	val = getval("cable");
	if (val && !strcasecmp(val, ALT_CABLE_1)) {
		if (ser_set_dtr(upsfd, 1) == -1)
			fatalx(EXIT_FAILURE, "%s: ser_set_dtr(%s) failed", __func__, device_path);
		if (ser_set_rts(upsfd, 0) == -1)
			fatalx(EXIT_FAILURE, "%s: ser_set_rts(%s) failed", __func__, device_path);
	}

	/*
	 * that's all if we want simple non canonical mode; this is meant as a
	 * compatibility measure for windows systems and perhaps some
	 * problematic serial cards/converters
	 */
	if ((val = getval("ttymode")) && !strcmp(val, "raw"))
		return;

	memset(&tio, 0, sizeof(tio));
	errno = 0;

	if (tcgetattr(upsfd, &tio))
		fatal_with_errno(EXIT_FAILURE, "%s: tcgetattr(%s)", __func__, device_path);

	/* set port mode: common stuff, canonical processing */

	tio.c_cflag |= (CS8 | CLOCAL | CREAD);

	tio.c_lflag |= ICANON;
#ifdef NOKERNINFO
	tio.c_lflag |= NOKERNINFO;
#endif
	tio.c_lflag &= ~(unsigned int)(ISIG | IEXTEN);

	tio.c_iflag |= (IGNCR | IGNPAR);
	tio.c_iflag &= ~(unsigned int)(IXON | IXOFF);

	tio.c_cc[VEOL] = '*';	/* specially handled in apc_read() */
#ifdef _POSIX_VDISABLE
	tio.c_cc[VERASE] = _POSIX_VDISABLE;
	tio.c_cc[VKILL]  = _POSIX_VDISABLE;
	tio.c_cc[VEOF] = _POSIX_VDISABLE;
	tio.c_cc[VEOL2] = _POSIX_VDISABLE;
#endif

	if (tcflush(upsfd, TCIOFLUSH))
		fatal_with_errno(EXIT_FAILURE, "%s: tcflush(%s)", __func__, device_path);

	/*
	 * warn:
	 * Note, that tcsetattr() returns success if /any/ of the requested
	 * changes could be successfully carried out. Thus the more complicated
	 * test.
	 */
	if (tcsetattr(upsfd, TCSANOW, &tio))
		fatal_with_errno(EXIT_FAILURE, "%s: tcsetattr(%s)", __func__, device_path);

	memset(&tio_chk, 0, sizeof(tio_chk));
	if (tcgetattr(upsfd, &tio_chk))
		fatal_with_errno(EXIT_FAILURE, "%s: tcgetattr(%s)", __func__, device_path);

	apc_ser_diff(&tio, &tio_chk);
}

static void ups_status_set(void)
{
	status_init();
	if (ups_status & APC_STAT_CAL)
		status_set("CAL");		/* calibration */
	if (ups_status & APC_STAT_TRIM)
		status_set("TRIM");		/* SmartTrim */
	if (ups_status & APC_STAT_BOOST)
		status_set("BOOST");		/* SmartBoost */
	if (ups_status & APC_STAT_OL)
		status_set("OL");		/* on line */
	if (ups_status & APC_STAT_OB)
		status_set("OB");		/* on battery */
	if (ups_status & APC_STAT_OVER)
		status_set("OVER");		/* overload */
	if (ups_status & APC_STAT_LB)
		status_set("LB");		/* low battery */
	if (ups_status & APC_STAT_RB)
		status_set("RB");		/* replace batt */

	if (ups_status == 0)
		status_set("OFF");

	status_commit();
}

static void alert_handler(char ch)
{
	switch (ch) {
		case '!':		/* clear OL, set OB */
			upsdebugx(1, "%s: %s", __func__, "OB");
			ups_status &= ~APC_STAT_OL;
			ups_status |= APC_STAT_OB;
			break;

		case '$':		/* clear OB, set OL */
			upsdebugx(1, "%s: %s", __func__, "OL");
			ups_status &= ~APC_STAT_OB;
			ups_status |= APC_STAT_OL;
			break;

		case '%':		/* set LB */
			upsdebugx(1, "%s: %s", __func__, "LB");
			ups_status |= APC_STAT_LB;
			break;

		case '+':		/* clear LB */
			upsdebugx(1, "%s: %s", __func__, "not LB");
			ups_status &= ~APC_STAT_LB;
			break;

		case '#':		/* set RB */
			upsdebugx(1, "%s: %s", __func__, "RB");
			ups_status |= APC_STAT_RB;
			break;

		case '?':		/* set OVER */
			upsdebugx(1, "%s: %s", __func__, "OVER");
			ups_status |= APC_STAT_OVER;
			break;

		case '=':		/* clear OVER */
			upsdebugx(1, "%s: %s", __func__, "not OVER");
			ups_status &= ~APC_STAT_OVER;
			break;

		default:
			upsdebugx(1, "%s: got 0x%02x (unhandled)", __func__, ch);
			break;
	}

	ups_status_set();
}

/*
 * we need a tiny bit different processing due to "*" and canonical mode; the
 * function is subtly different from generic ser_get_line_alert()
 */
#define apc_read(b, l, f) apc_read_i(b, l, f, __func__, __LINE__)
static ssize_t apc_read_i(char *buf, size_t buflen, int flags, const char *fn, unsigned int ln)
{
	const char *iset = IGN_CHARS, *aset = "";
	size_t	count = 0;
	ssize_t	i, ret;
	int	sec = 3, usec = 0;
	char	temp[APC_LBUF];

	if (buflen > (size_t)SSIZE_MAX) {
		fatalx (EXIT_FAILURE, "Error: apc_read_i called with buflen too large");
	}

	if (INVALID_FD(upsfd))
		return 0;

	if (flags & SER_D0) {
		sec = 0; usec = 0;
	}
	if (flags & SER_DX) {
		sec = 0; usec = 200000;
	}
	if (flags & SER_D1) {
		sec = 1; usec = 500000;
	}
	if (flags & SER_D3) {
		sec = 3; usec = 0;
	}
	if (flags & SER_AA) {
		iset = IGN_AACHARS;
		aset = ALERT_CHARS;
	}
	if (flags & SER_CC) {
		iset = IGN_CCCHARS;
		aset = "";
		sec = 6; usec = 0;
	}
	if (flags & SER_CS) {
		iset = IGN_CSCHARS;
		aset = "";
		sec = 6; usec = 0;
	}

	memset(buf, '\0', buflen);

	while (count < buflen - 1) {
		errno = 0;
		ret = select_read(upsfd, temp, sizeof(temp), sec, usec);

		/* partial timeout (non-canon only paranoid check) */
		if (ret == 0 && count) {
			ser_comm_fail("serial port partial timeout: %u(%s)", ln, fn);
			return -1;
		}
		/* error or no timeout allowed */
		if (ret < 0 || (ret == 0 && !(flags & SER_TO))) {
			if (ret)
				ser_comm_fail("serial port read error: %u(%s): %s", ln, fn, strerror(errno));
			else
				ser_comm_fail("serial port read timeout: %u(%s)", ln, fn);
			return ret;
		}
		/* ok, timeout is acceptable... */
		if (ret == 0 && (flags & SER_TO)) {
			/*
			 * ...but it doesn't imply ser_comm_good
			 *
			 * for example we might be in comm_fail condition,
			 * trying to "nudge" the UPS with some command
			 * obviously expecting timeout if the comm is still
			 * lost. This would result with filling logs with
			 * confusing comm lost/comm re-established pairs due to
			 * successful serial writes
			 */
			return 0;
		}

		/* parse input */
		for (i = 0; i < ret; i++) {
			/* overflow read */
			if (count == buflen - 1) {
				ser_comm_fail("serial port read overflow: %u(%s)", ln, fn);
				tcflush(upsfd, TCIFLUSH);
				return -1;
			}
			/* standard "line received" condition */
			if (temp[i] == ENDCHAR) {
				ser_comm_good();
				return (ssize_t)count;
			}
			/*
			 * "*" is set as a secondary EOL; convert to "OK" only
			 * as a reply to shutdown command in sdok(); otherwise
			 * next select_read() will continue normally
			 */
			if ((flags & SER_HA) && temp[i] == '*') {
				/*
				 * a bit paranoid, but remember that "*" is not
				 * real EOL; there could be some firmware in
				 * existence, which would send both string:
				 * "OK\n" and alert: "*".
				 * Just in case, try to flush the input with a
				 * small 1 sec timeout.
				 */
				memset(buf, '\0', buflen);
				errno = 0;
				ret = select_read(upsfd, temp, sizeof(temp), 1, 0);
				if (ret < 0) {
					ser_comm_fail("serial port read error: %u(%s): %s", ln, fn, strerror(errno));
					return ret;
				}
				buf[0] = 'O';
				buf[1] = 'K';
				ser_comm_good();
				return 2;
			}
			/* ignore set */
			if (strchr(iset, temp[i]) || temp[i] == '*') {
				continue;
			}
			/* alert set */
			if (strchr(aset, temp[i])) {
				alert_handler(temp[i]);
				continue;
			}

			buf[count++] = temp[i];
		}
	}

	ser_comm_good();
	/* buflen range limited above */
	return (ssize_t)count;
}

#define apc_write(code) apc_write_i(code, __func__, __LINE__)
static ssize_t apc_write_i(unsigned char code, const char *fn, unsigned int ln)
{
	ssize_t ret;
	errno = 0;

	if (INVALID_FD(upsfd))
		return 0;

	ret = ser_send_char(upsfd, code);
	/*
	 * Formally any write() should never return 0, if the count != 0. For
	 * the sake of handling any obscure nonsense, we consider such return
	 * as a failure - thus <= condition; either way, LE is a pretty hard
	 * condition hardly ever happening;
	 */
	if (ret <= 0)
		ser_comm_fail("serial port write error: %u(%s): %s", ln, fn, strerror(errno));

	return ret;
}

/*
 * We have to watch out for NA, here;
 * This is generally safe, as otherwise we will just timeout. The reason we do
 * it, is that under certain conditions an UPS might respond with NA for
 * something it would normally handle (e.g. calling @ while being in powered
 * off or hibernated state. If we keep sending the "arguments" after getting
 * NA, they will be interpreted as commands, which is quite a bug :)
 * Furthermore later flushes might not work properly, if the reply to those
 * commands are generated with some delay.
 *
 * We also set errno to something usable, so outside upslog calls don't output
 * confusing "success".
 */
#define apc_write_long(code) apc_write_long_i(code, __func__, __LINE__)
static ssize_t apc_write_long_i(const char *code, const char *fn, unsigned int ln)
{
	char temp[APC_LBUF];
	ssize_t ret;

	ret = apc_write_i((const unsigned char)(*code), fn, ln);
	if (ret != 1)
		return ret;
	/* peek for the answer - anything at this point is failure */
	ret = apc_read(temp, sizeof(temp), SER_DX|SER_TO);
	if (ret) {
		errno = ECANCELED;
		return -1;
	}

	ret = ser_send_pace(upsfd, 50000, "%s", code + 1);
	if (ret >= 0)
		ret++;
	/* see remark in plain apc_write() */
	if (ret != (int)strlen(code))
		ser_comm_fail("serial port write error: %u(%s): %s", ln, fn, strerror(errno));
	return ret;
}

#define apc_write_rep(code) apc_write_rep_i(code, __func__, __LINE__)
static ssize_t apc_write_rep_i(unsigned char code, const char *fn, unsigned int ln)
{
	char temp[APC_LBUF];
	ssize_t ret;

	ret = apc_write_i(code, fn, ln);
	if (ret != 1)
		return ret;
	/* peek for the answer - anything at this point is failure */
	ret = apc_read(temp, sizeof(temp), SER_DX|SER_TO);
	if (ret) {
		errno = ECANCELED;
		return -1;
	}
	usleep(1300000);

	ret = apc_write_i(code, fn, ln);
	if (ret >= 0)
		ret++;
	return ret;
}

/* all flags other than SER_AA are ignored */
static void apc_flush(int flags)
{
	char temp[APC_LBUF];

	if (flags & SER_AA) {
		tcflush(upsfd, TCOFLUSH);
		/* TODO */
		while(apc_read(temp, sizeof(temp), SER_D0|SER_TO|SER_AA) > 0);
	} else {
		tcflush(upsfd, TCIOFLUSH);
		/* tcflush(upsfd, TCIFLUSH); */
		/* while(apc_read(temp, sizeof(temp), SER_D0|SER_TO)); */
	}
}

/* APC specific wrappers around set/del info - to handle "packed" variables */
static void apc_dstate_delinfo(apc_vartab_t *vt, int skip)
{
	char *name, *nidx;
	int c;

	/* standard not packed var */
	if (!(vt->flags & APC_PACK)) {
		dstate_delinfo(vt->name);
		return;
	}

	if ( !(name = xmalloc(sizeof(char) * vt->nlen0)) ) {
		upslogx(LOG_ERR, "apc_dstate_delinfo() failed to allocate buffer");
		return;
	}

	memset(name, '\0', vt->nlen0);
	strncpy(name, vt->name, vt->nlen0 - 1);
	name[vt->nlen0 - 1] = '\0';
	nidx = strstr(name,".0.") + 1;

	for (c = skip; c < vt->cnt; c++) {
		*nidx = (char)('1' + c);
		dstate_delinfo(name);
	}

	vt->cnt = 0;
	free(name);
}

static void apc_dstate_setinfo(apc_vartab_t *vt, const char *upsval)
{
	char *name, *nidx;
	char *temp, *vidx[APC_PACK_MAX], *com, *curr;
	size_t upsvallen = strlen(upsval);
	int c;

	/* standard not packed var */
	if (!(vt->flags & APC_PACK)) {
		dstate_setinfo(vt->name, "%s", convert_data(vt, upsval));
		return;
	}

	if ( !(name = xmalloc(sizeof(char) * vt->nlen0)) ) {
		upslogx(LOG_ERR, "apc_dstate_setinfo() failed to allocate buffer");
		return;
	}

	if ( !(temp = xmalloc(sizeof(char) * (upsvallen + 2))) ) {
		/* +2 seems like an overkill, but helps hush compiler warnings */
		upslogx(LOG_ERR, "apc_dstate_setinfo() failed to allocate buffer");
		free(name);
		return;
	}

	/* we have to set proper name for dstate_setinfo() calls */
	memset(name, '\0', vt->nlen0);
	strncpy(name, vt->name, vt->nlen0 - 1);
	name[vt->nlen0 - 1] = '\0';
	nidx = strstr(name,".0.") + 1;

	/* split the value string */
	memset(temp, '\0', upsvallen + 2);
	strncpy(temp, upsval, upsvallen + 1);
	temp[upsvallen] = '\0';
	curr = temp;
	c = 0;
	do {
		vidx[c] = curr;
		com = strchr(curr, ',');
		if (com) {
			curr = com + 1;
			*com = '\0';
		}
	} while(++c < APC_PACK_MAX && com);

	/*
	 * unlikely, but keep things tidy - remove leftover values, if
	 * subsequent read returns less
	 */
	if (vt->cnt > c)
		apc_dstate_delinfo(vt, c);

	/* unlikely - warn user if we have more than APC_PACK_MAX fields */
	if (c == APC_PACK_MAX && com)
		upslogx(LOG_WARNING,
				"packed variable %s [%s] longer than %d fields,\n"
				"ignoring remaining fields",
				vt->name, prtchr(vt->cmd), c);

	vt->cnt = c;

	while (c-- > 0) {
		*nidx = (char)('1' + c);
		if (*vidx[c])
			dstate_setinfo(name, "%s", convert_data(vt, vidx[c]));
		else
			dstate_setinfo(name, "N/A");
	}

	free(name);
	free(temp);
}

static const char *preread_data(apc_vartab_t *vt)
{
	ssize_t ret;
	static char temp[APC_LBUF];

	upsdebugx(1, "%s: %s [%s]", __func__, vt->name, prtchr(vt->cmd));

	apc_flush(0);
	ret = apc_write((const unsigned char)vt->cmd);

	if (ret != 1)
		return 0;

	ret = apc_read(temp, sizeof(temp), SER_TO);

	if (ret < 1 || !strcmp(temp, "NA")) {
		if (ret >= 0)
			upslogx(LOG_ERR, "%s: %s [%s] timed out or not supported", __func__, vt->name, prtchr(vt->cmd));
		return 0;
	}

	return temp;
}

static int poll_data(apc_vartab_t *vt)
{
	char temp[APC_LBUF];

	if (!(vt->flags & APC_PRESENT))
		return 1;

	upsdebugx(1, "%s: %s [%s]", __func__, vt->name, prtchr(vt->cmd));

	apc_flush(SER_AA);
	if (apc_write((const unsigned char)vt->cmd) != 1)
		return 0;
	if (apc_read(temp, sizeof(temp), SER_AA) < 1)
		return 0;

	/* automagically no longer supported by the hardware somehow */
	if (!strcmp(temp, "NA")) {
		upslogx(LOG_WARNING, "%s: verified variable %s [%s] returned NA, removing", __func__, vt->name, prtchr(vt->cmd));
		vt->flags &= ~(unsigned int)APC_PRESENT;
		apc_dstate_delinfo(vt, 0);
	} else
		apc_dstate_setinfo(vt, temp);

	return 1;
}

static int update_status(void)
{
	ssize_t	ret;
	char	buf[APC_LBUF];

	upsdebugx(1, "%s: [%s]", __func__, prtchr(APC_STATUS));

	apc_flush(SER_AA);
	if (apc_write(APC_STATUS) != 1)
		return 0;
	ret = apc_read(buf, sizeof(buf), SER_AA);

	if ((ret < 1) || (!strcmp(buf, "NA"))) {
		if (ret >= 0)
			upslogx(LOG_WARNING, "%s: %s", __func__, "failed");
		return 0;
	}

	ups_status = strtol(buf, 0, 16) & 0xff;
	ups_status_set();

	return 1;
}

/*
 * two informative functions, to not redo the same thing in few places
 */

static inline void confirm_cv(unsigned char cmd, const char *tag, const char *name)
{
	upsdebugx(1, "%s [%s] - %s supported", name, prtchr((char)cmd), tag);
}

static inline void warn_cv(unsigned char cmd, const char *tag, const char *name)
{
	if (tag && name)
		upslogx(LOG_WARNING, "%s [%s] - %s invalid", name, prtchr((char)cmd), tag);
	else
		upslogx(LOG_WARNING, "[%s] unrecognized", prtchr((char)cmd));
}

static void var_string_setup(apc_vartab_t *vt)
{
	/*
	 * handle special data for our two strings; note - STRING variables
	 * cannot be PACK at the same time
	 */
	if (vt->flags & APC_STRING) {
		dstate_setflags(vt->name, ST_FLAG_RW | ST_FLAG_STRING);
		dstate_setaux(vt->name, APC_STRLEN);
		vt->flags |= APC_RW;
	}
}

static int var_verify(apc_vartab_t *vt)
{
	const char *temp;

	if (vt->flags & APC_MULTI) {
		/* APC_MULTI are handled by deprecate_vars() */
		vt->flags |= APC_PRESENT;
		return -1;
	}

	temp = preread_data(vt);
	/* no conversion here, validator should operate on raw values */
	if (!temp || !rexhlp(vt->regex, temp)) {
		warn_cv((const unsigned char)vt->cmd, "variable", vt->name);
		return 0;
	}

	vt->flags |= APC_PRESENT;
	apc_dstate_setinfo(vt, temp);
	var_string_setup(vt);

	confirm_cv((const unsigned char)vt->cmd, "variable", vt->name);

	return 1;
}

/*
 * This function iterates over vartab, deprecating NUT<->APC 1:n and n:1
 * variables. We prefer earliest present variable. All the other ones must be
 * marked as not present (which implies deprecation).
 * This pass is required after completion of all protocol_verify() and/or
 * legacy_verify() calls.
 */
static void deprecate_vars(void)
{
	int i, j;
	const char *temp;
	apc_vartab_t *vt, *vtn;

	for (i = 0; apc_vartab[i].name != NULL; i++) {
		vt = &apc_vartab[i];
		if (!(vt->flags & APC_MULTI) || !(vt->flags & APC_PRESENT)) {
			/*
			 * a) not interesting, or
			 * b) not marked as present earlier, or already handled
			 */
			continue;
		}
		/* pre-read data, we have to verify it */
		temp = preread_data(vt);
		/* no conversion here, validator should operate on raw values */
		if (!temp || !rexhlp(vt->regex, temp)) {
			vt->flags &= ~(unsigned int)APC_PRESENT;

			warn_cv((const unsigned char)vt->cmd, "variable combination", vt->name);
			continue;
		}

		/* multi & present, deprecate all the remaining ones */
		for (j = i + 1; apc_vartab[j].name != NULL; j++) {
			vtn = &apc_vartab[j];
			if (strcmp(vtn->name, vt->name) && vtn->cmd != vt->cmd)
				continue;
			vtn->flags &= ~(unsigned int)APC_PRESENT;
		}

		apc_dstate_setinfo(vt, temp);
		var_string_setup(vt);

		confirm_cv((const unsigned char)vt->cmd, "variable combination", vt->name);
	}
}

static void apc_getcaps(int qco)
{
	const	char	*ptr, *entptr;
	char	upsloc, temp[APC_LBUF], cmd, loc, etmp[APC_SBUF], *endtemp;
	int	matrix, valid;
	size_t	nument, entlen, i;
	ssize_t	ret;
	apc_vartab_t *vt;

	/*
	 * If we can do caps, then we need the Firmware revision which has the
	 * locale descriptor as the last character (ugh); this is valid for
	 * both 'V' and 'b' commands.
	 */
	ptr = dstate_getinfo("ups.firmware");
	if (ptr)
		upsloc = ptr[strlen(ptr) - 1];
	else
		upsloc = 0;

	/* get capability string */
	apc_flush(0);
	if (apc_write(APC_CAPS) != 1)
		return;

	/*
	 * note - apc_read() needs larger timeout grace (not a problem w.r.t.
	 * to NUT's timing, as it's done only during setup) and different
	 * ignore set due to certain characters like '#' being received
	 */
	ret = apc_read(temp, sizeof(temp), SER_CC|SER_TO);

	if ((ret < 1) || (!strcmp(temp, "NA"))) {

		/*
		 * Early Smart-UPS not as smart as the later ones ...
		 * this should never happen on properly functioning hardware -
		 * as capability support was reported earlier
		 */
		if (ret >= 0)
			upslogx(LOG_WARNING, "%s", "APC cannot do capabilities but said it could !");
		return;
	}

	/* recv always puts a \0 at the end, so this is safe */
	/* however it assumes a zero byte cannot be embedded */
	endtemp = &temp[0] + strlen(temp);

	if (temp[0] != '#') {
		upslogx(LOG_WARNING, "unknown capability start char [%c] !", temp[0]);
		upsdebugx(1, "%s: please report this caps string: %s",
			__func__, temp);
		return;
	}

	if (temp[1] == '#') {		/* Matrix-UPS */
		ptr = &temp[0];
		matrix = 1;
	} else {
		ptr = &temp[1];
		matrix = 0;
	}

	/* command char, location, # of entries, entry length */

	while (ptr[0] != '\0') {
		if (matrix)
			ptr += 2;	/* jump over repeating ## */

		/* check for idiocy */
		if (ptr >= endtemp) {
			/* if we expected this, just ignore it */
			if (qco)
				return;
			fatalx(EXIT_FAILURE,
				"capability string has overflowed, "
				"please report this error with device details!");
		}

		entptr = &ptr[4];
		cmd = ptr[0];
		loc = ptr[1];

		if (ptr[2] < 48 || ptr[3] < 48) {
			upsdebugx(3,
				"%s: SKIP: nument (%d) or entlen (%d) "
				"out of range for cmd %d at loc %d",
				__func__, (ptr[2] - 48), (ptr[3] - 48),
				cmd, loc);

			/* just ignore it as we did for ages see e.g. v2.7.4
			 * (note the next loop cycle was and still would be
			 * no-op anyway, if "nument <= 0").
			 */
			nument = 0;
			entlen = 0;

			/* NOT a full skip: Gotta handle "vt" to act like before */
			/*ptr = entptr;*/
			/*continue;*/
		} else {
			nument = (size_t)ptr[2] - 48;
			entlen = (size_t)ptr[3] - 48;
		}

		vt = vt_lookup_char(cmd);
		valid = vt && ((loc == upsloc) || (loc == '4')) && !(vt->flags & APC_PACK);

		/* mark this as writable */
		if (valid) {
			upsdebugx(1, "%s: %s [%s(%c)] - capability supported",
				__func__, vt->name, prtchr(cmd), loc);

			dstate_setflags(vt->name, ST_FLAG_RW);

			/* make sure setvar knows what this is */
			vt->flags |= APC_RW | APC_ENUM;
		} else if (vt && (vt->flags & APC_PACK))
			/*
			 * Currently we assume - basing on the following
			 * feedback:
			 * http://www.mail-archive.com/nut-upsdev@lists.alioth.debian.org/msg03398.html
			 * - that "packed" variables are not enumerable; if at
			 * some point in the future it turns out to be false,
			 * the handling will have to be a bit more complex
			 */
			upslogx(LOG_WARNING,
					"WARN: packed APC variable %s [%s] reported as enumerable,\n"
					"please report it on the mailing list", vt->name, prtchr(cmd));

		for (i = 0; i < nument; i++) {
			if (valid) {
				snprintf(etmp, entlen + 1, "%s", entptr);
				dstate_addenum(vt->name, "%s", convert_data(vt, etmp));
			}

			entptr += entlen;
		}

		ptr = entptr;
	}
}

static void legacy_verify(const char *var)
{
	int i;
	/*
	 * note: some NUT variables map onto multiple APC ones, e.g. firmware:
	 * V,b -> ups.firmware; that's why we keep the loop, as it's over NUT
	 * names
	 */
	for (i = 0; apc_vartab[i].name != NULL; i++) {
		if (strcmp(apc_vartab[i].name, var))
			continue;
		var_verify(&apc_vartab[i]);
	}
}

static void protocol_verify(unsigned char cmd)
{
	int i, found;
	apc_vartab_t *vt;
	apc_cmdtab_t *ct;

	/* don't bother with cmd/var we don't care about */
	if (strchr(APC_UNR_CMDS, cmd))
		return;

	/*
	 * loop necessary for APC:NUT 1:n cases (e.g. T -> device.uptime,
	 * ambient.0.temperature)
	 */
	found = 0;
	for (i = 0; apc_vartab[i].name != NULL; i++) {
		vt = &apc_vartab[i];
		if (vt->cmd != cmd)
			continue;
		var_verify(vt);
		found = 1;
	}
	if (found)
		return;

	/*
	 * see if it's a command
	 * loop necessary for APC:NUT 1:n cases (e.g. D -> calibrate.start,
	 * calibrate.stop)
	 */
	found = 0;
	for (i = 0; apc_cmdtab[i].name != NULL; i++) {
		ct = &apc_cmdtab[i];
		if (ct->cmd != cmd)
			continue;
		ct->flags |= APC_PRESENT;
		dstate_addcmd(ct->name);
		confirm_cv(cmd, "command", ct->name);
		found = 1;
	}

	if (found)
		return;

	/*
	 * epilogue - unrecognized command / variable not included
	 * in APC_UNR_CMDS
	 */
	warn_cv(cmd, NULL, NULL);
}

static void oldapcsetup(void)
{
	/*
	 * note: battery.date and ups.id make little sense here, as
	 * that would imply writability and this is an *old* APC UPS
	 */
	legacy_verify("ups.temperature");
	legacy_verify("ups.load");
	legacy_verify("input.voltage");
	legacy_verify("output.voltage");
	legacy_verify("battery.charge");
	legacy_verify("battery.voltage");

	/* these will usually timeout */
	legacy_verify("ups.model");
	legacy_verify("ups.serial");
	legacy_verify("ups.firmware");
	legacy_verify("output.current");

	deprecate_vars();

	/* see if this might be an old Matrix-UPS instead */
	if (vt_lookup_name("output.current"))
		dstate_setinfo("ups.model", "Matrix-UPS");
	else {
		/* really old models don't support ups.model (APC: 0x01) */
		if (!vt_lookup_name("ups.model"))
			/* force the model name */
			dstate_setinfo("ups.model", "Smart-UPS");
	}

	/*
	 * If we have come down this path then we don't do capabilities and
	 * other shiny features.
	 */
}

/* some hardware is a special case - hot-wire the list of cmdchars */
static int firmware_table_lookup(void)
{
	ssize_t ret;
	unsigned int i, j;
	char buf[APC_LBUF];

	upsdebugx(1, "%s: attempting firmware lookup using [%s]",
		__func__, prtchr(APC_FW_OLD));

	apc_flush(0);
	if (apc_write(APC_FW_OLD) != 1)
		return 0;
	if ((ret = apc_read(buf, sizeof(buf), SER_TO)) < 0)
		return 0;

	/*
	 * Some UPSes support both 'V' and 'b'. As 'b' doesn't always return
	 * firmware version, we attempt that only if 'V' doesn't work.
	 */
	if (!ret || !strcmp(buf, "NA")) {
		upsdebugx(1, "%s: attempting firmware lookup using [%s]",
			__func__, prtchr(APC_FW_NEW));

		if (apc_write(APC_FW_NEW) != 1)
			return 0;
		if (apc_read(buf, sizeof(buf), SER_TO) < 1)
			return 0;
	}

	upsdebugx(1, "%s: detected firmware version: %s", __func__, buf);

	/* this will be reworked if we get a lot of these things */
	if (!strcmp(buf, "451.2.I")) {
		/* quirk_capability_overflow */
		upsdebugx(1, "%s: WARN: quirky firmware !", __func__);
		return 2;
	}

	if (rexhlp("^[a-fA-F0-9]{2}$", buf)) {
		/*
		 * certain old set of UPSes that return voltage above 255V
		 * through 'b'; see:
		 * http://article.gmane.org/gmane.comp.monitoring.nut.user/7762
		 */
		memset(buf, '\0', sizeof(buf));
		strncpy(buf, "set\1", sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = '\0';
	}

	for (i = 0; apc_compattab[i].firmware != NULL; i++) {
		if (!strcmp(apc_compattab[i].firmware, buf)) {

			upsdebugx(1, "%s: matched firmware: %s",
				__func__, apc_compattab[i].firmware);

			/* magic ? */
			if (strspn(apc_compattab[i].firmware, "05")) {
				dstate_setinfo("ups.model", "Matrix-UPS");
			} else {
				dstate_setinfo("ups.model", "Smart-UPS");
			}

			/* matched - run the cmdchars from the table */
			upsdebugx(1,
				"%s: parsing out supported cmds and vars",
				__func__);
			for (j = 0; j < strlen(apc_compattab[i].cmdchars); j++)
				protocol_verify((const unsigned char)(apc_compattab[i].cmdchars[j]));
			deprecate_vars();

			return 1;	/* matched */
		}
	}

	return 0;
}

static int getbaseinfo(void)
{
	unsigned int	i;
	ssize_t	ret;
	int	qco;
	char 	*cmds, *tail, temp[APC_LBUF];

	/*
	 *  try firmware lookup first; we could start with 'a', but older models
	 *  sometimes return other things than a command set
	 */
	qco = firmware_table_lookup();
	if (qco == 1)
		/* found compat */
		return 1;

	upsdebugx(1, "%s: attempting var/cmdset lookup using [%s]",
		__func__, prtchr(APC_CMDSET));
	/*
	 * Initially we ask the UPS what commands it takes. If this fails we are
	 * going to need an alternate strategy - we can deal with that if it
	 * happens
	 */

	apc_flush(0);
	if (apc_write(APC_CMDSET) != 1)
		return 0;
	if ((ret = apc_read(temp, sizeof(temp), SER_CS|SER_TO)) < 0)
		return 0;

	if (!ret || !strcmp(temp, "NA") || !rexhlp(APC_CMDSET_FMT, temp)) {
		/* We have an old dumb UPS - go to specific code for old stuff */
		upslogx(LOG_NOTICE, "very old or unknown APC model, support will be limited");
		oldapcsetup();
		return 1;
	}

	upsdebugx(1, "%s: parsing out supported cmds/vars", __func__);
	/*
	 * returned set is verified for validity above, so just extract
	 * what's interesting for us
	 *
	 * the known format is:
	 * ver.alerts.commands[.stuff]
	 */
	cmds = strchr(temp, '.');
	cmds = strchr(cmds + 1, '.');
	tail = strchr(++cmds, '.');
	if (tail)
		*tail = 0;
	for (i = 0; i < strlen(cmds); i++)
		protocol_verify((const unsigned char)cmds[i]);
	deprecate_vars();

	/* if capabilities are supported, add them here */
	if (strchr(cmds, APC_CAPS)) {
		upsdebugx(1, "%s: parsing out caps", __func__);
		apc_getcaps(qco);
	}
	return 1;
}

/* check for calibration status and either start or stop */
static int do_cal(int start)
{
	char	temp[APC_LBUF];
	long	tval;
	ssize_t	ret;

	apc_flush(SER_AA);
	ret = apc_write(APC_STATUS);

	if (ret != 1) {
		return STAT_INSTCMD_HANDLED;		/* FUTURE: failure */
	}

	ret = apc_read(temp, sizeof(temp), SER_AA);

	/* if we can't check the current calibration status, bail out */
	if ((ret < 1) || (!strcmp(temp, "NA"))) {
		upslogx(LOG_WARNING, "%s", "runtime calibration state can not be determined");
		return STAT_INSTCMD_HANDLED;		/* FUTURE: failure */
	}

	tval = strtol(temp, 0, 16);

	if (tval & APC_STAT_CAL) {	/* calibration currently happening */
		if (start == 1) {
			/* requested start while calibration still running */
			upslogx(LOG_NOTICE, "%s", "runtime calibration already in progress");
			return STAT_INSTCMD_HANDLED;	/* FUTURE: failure */
		}

		/* stop requested */

		upslogx(LOG_NOTICE, "%s", "stopping runtime calibration");

		ret = apc_write(APC_CMD_CALTOGGLE);

		if (ret != 1) {
			return STAT_INSTCMD_HANDLED;	/* FUTURE: failure */
		}

		ret = apc_read(temp, sizeof(temp), SER_AA);

		if ((ret < 1) || (!strcmp(temp, "NA")) || (!strcmp(temp, "NO"))) {
			upslogx(LOG_WARNING, "stop calibration failed, cmd returned: %s", temp);
			return STAT_INSTCMD_HANDLED;	/* FUTURE: failure */
		}

		return STAT_INSTCMD_HANDLED;	/* FUTURE: success */
	}

	/* calibration not happening */

	if (start == 0) {		/* stop requested */
		upslogx(LOG_NOTICE, "%s", "runtime calibration not occurring");
		return STAT_INSTCMD_HANDLED;		/* FUTURE: failure */
	}

	upslogx(LOG_NOTICE, "%s", "starting runtime calibration");

	ret = apc_write(APC_CMD_CALTOGGLE);

	if (ret != 1) {
		return STAT_INSTCMD_HANDLED;	/* FUTURE: failure */
	}

	ret = apc_read(temp, sizeof(temp), SER_AA);

	if ((ret < 1) || (!strcmp(temp, "NA")) || (!strcmp(temp, "NO"))) {
		upslogx(LOG_WARNING, "start calibration failed, cmd returned: %s", temp);
		return STAT_INSTCMD_HANDLED;	/* FUTURE: failure */
	}

	return STAT_INSTCMD_HANDLED;			/* FUTURE: success */
}

#if 0
/* get the UPS talking to us in smart mode */
static int smartmode(void)
{
	int	ret;
	char	temp[APC_LBUF];

	apc_flush(0);
	ret = apc_write(APC_GOSMART);
	if (ret != 1) {
		return 0;
	}
	ret = apc_read(temp, sizeof(temp), 0);

	if ((ret < 1) || (!strcmp(temp, "NA")) || (!strcmp(temp, "NO"))) {
		upslogx(LOG_CRIT, "%s", "enabling smartmode failed !");
		return 0;
	}

	return 1;
}
#endif

/*
 * get the UPS talking to us in smart mode
 * note: this is weird overkill, but possibly excused due to some obscure
 * hardware/firmware combinations; simpler version commented out above, for
 * now let's keep minimally adjusted old one
 */
static int smartmode(int cnt)
{
	ssize_t ret;
	int tries;
	char temp[APC_LBUF];

	for (tries = 0; tries < cnt; tries++) {

		apc_flush(0);
		if (apc_write(APC_GOSMART) != 1)
			return 0;

		/* timeout here is intended */
		ret = apc_read(temp, sizeof(temp), SER_TO|SER_D1);
		if (ret > 0 && !strcmp(temp, "SM"))
			return 1;	/* success */
		if (ret < 0)
			/* error, so we didn't timeout - wait a bit before retry */
			sleep(1);

		if (apc_write(27) != 1) /* ESC */
			return 0;

		/* eat the response (might be NA, might be something else) */
		apc_read(temp, sizeof(temp), SER_TO|SER_D1);
	}

	return 0;	/* failure */
}

/*
 * all shutdown commands should respond with "OK" or "*"
 * apc_read() handles conversion to "OK" so we care only about that one
 * ign allows for timeout without assuming an error
 */
static int sdok(int ign)
{
	ssize_t ret;
	char temp[APC_SBUF];

	/*
	 * older UPSes on failed commands might just timeout, we cut down
	 * timeout grace though
	 * furthermore, command 'Z' will not reply with anything
	 */
	ret = apc_read(temp, sizeof(temp), SER_HA|SER_D1|SER_TO);
	if (ret < 0)
		return STAT_INSTCMD_FAILED;

	upsdebugx(1, "%s: got \"%s\"", __func__, temp);

	if ((!ret && ign) || !strcmp(temp, "OK")) {
		upsdebugx(1, "%s: %s", __func__, "last shutdown cmd succeeded");
		return STAT_INSTCMD_HANDLED;
	}

	upsdebugx(1, "%s: %s", __func__, "last shutdown cmd failed");
	return STAT_INSTCMD_FAILED;
}

/* soft hibernate: S - working only when OB, otherwise ignored */
static int sdcmd_S(const void *foo)
{
	apc_flush(0);
	if (!foo)
		upsdebugx(1, "%s: issuing [%s]", __func__, prtchr(APC_CMD_SOFTDOWN));
	if (apc_write(APC_CMD_SOFTDOWN) != 1)
		return STAT_INSTCMD_FAILED;
	return sdok(0);
}

/* soft hibernate, hack version for CS 350 & co. */
static int sdcmd_CS(const void *foo)
{
	ssize_t ret;
	useconds_t cshd = 3500000;
	char temp[APC_SBUF];
	const char *val;
	NUT_UNUSED_VARIABLE(foo);

	/* TODO: Catch overflows?
	 * Let compilers complain about (non-)casting on systems
	 * where useconds_t is not a good target for strtod() output
	 */
	if ((val = getval("cshdelay")))
		cshd = (strtod(val, NULL) * 1000000);

	upsdebugx(1, "%s: issuing CS 'hack' [%s+%s] with %2.1f sec delay",
		__func__, prtchr(APC_CMD_SIMPWF), prtchr(APC_CMD_SOFTDOWN),
		(double)cshd / 1000000);
	if (ups_status & APC_STAT_OL) {
		apc_flush(0);
		upsdebugx(1, "%s: issuing [%s]", __func__, prtchr(APC_CMD_SIMPWF));
		ret = apc_write(APC_CMD_SIMPWF);
		if (ret != 1) {
			return STAT_INSTCMD_FAILED;
		}
		/* eat response, allow timeout */
		ret = apc_read(temp, sizeof(temp), SER_D1|SER_TO);
		if (ret < 0) {
			return STAT_INSTCMD_FAILED;
		}
		usleep(cshd);
	}
	/* continue with regular soft hibernate */
	return sdcmd_S((void *)1);
}

/*
 * hard hibernate: @nnn / @nn
 * note: works differently for older and new models, see manual page for
 * thorough explanation
 */
static int sdcmd_AT(const void *str)
{
	ssize_t ret;
	size_t cnt, padto, i;
	const char *awd = str;
	char temp[APC_SBUF], *ptr;

	memset(temp, '\0', sizeof(temp));

	if (!awd)
		awd = "000";

	cnt = strlen(awd);
	padto = cnt == 2 ? 2 : 3;

	temp[0] = APC_CMD_GRACEDOWN;
	ptr = temp + 1;
	for (i = cnt; i < padto ; i++) {
		*ptr++ = '0';
	}
	strncpy(ptr, awd, sizeof(temp) - (ptr - temp) - 1);
	temp[sizeof(temp) - 1] = '\0';

	upsdebugx(1, "%s: issuing [%s] with %ld minutes of additional wake-up delay",
		__func__, prtchr(APC_CMD_GRACEDOWN), strtol(awd, NULL, 10)*6);

	apc_flush(0);
	ret = apc_write_long(temp);
	/* Range-check: padto is 2 or 3 per above */
	if (ret != (ssize_t)padto + 1) {
		/* FIXME: ...INSTCMD_CONVERSION_FAILED ?*/
		upslogx(LOG_INSTCMD_FAILED,
			"issuing [%s] with %" PRIuSIZE " digits failed",
			prtchr(APC_CMD_GRACEDOWN), padto);
		return STAT_INSTCMD_FAILED;
	}

	ret = sdok(0);
	if (ret == STAT_INSTCMD_HANDLED || padto == 3)
		return (int)ret;

	/* FIXME: ...INSTCMD_CONVERSION_FAILED ?*/
	upslogx(LOG_INSTCMD_FAILED,
		"command [%s] with 2 digits doesn't work - try 3 digits",
		prtchr(APC_CMD_GRACEDOWN));
	/*
	 * "tricky" part - we tried @nn variation and it (unsurprisingly)
	 * failed; we have to abort the sequence with something bogus to have
	 * the clean state; newer UPSes will respond with "NO", older will be
	 * silent (YMMV);
	 */
	apc_write(APC_GOSMART);
	/* eat response, allow it to timeout */
	apc_read(temp, sizeof(temp), SER_D1|SER_TO);

	return STAT_INSTCMD_FAILED;
}

/* shutdown: K - delayed poweroff */
static int sdcmd_K(const void *foo)
{
	ssize_t ret;
	NUT_UNUSED_VARIABLE(foo);

	upsdebugx(1, "%s: issuing [%s]", __func__, prtchr(APC_CMD_SHUTDOWN));

	apc_flush(0);
	ret = apc_write_rep(APC_CMD_SHUTDOWN);
	if (ret != 2)
		return STAT_INSTCMD_FAILED;

	return sdok(0);
}

/* shutdown: Z - immediate poweroff */
static int sdcmd_Z(const void *foo)
{
	ssize_t ret;
	NUT_UNUSED_VARIABLE(foo);

	upsdebugx(1, "%s: issuing [%s]", __func__, prtchr(APC_CMD_OFF));

	apc_flush(0);
	ret = apc_write_rep(APC_CMD_OFF);
	if (ret != 2) {
		return STAT_INSTCMD_FAILED;
	}

	/* note: UPS will not reply anything after this command */
	return sdok(1);
}

static void upsdrv_shutdown_simple(void)
{
	long sdtype = 0;
	const char *val;

	if ((val = getval("sdtype")))
		sdtype = strtol(val, NULL, 10);

	upsdebugx(1, "%s: currently: %s, sdtype: %ld", __func__,
		(ups_status & APC_STAT_OL) ? "on-line" : "on battery", sdtype);

	switch (sdtype) {

	case 5:		/* hard hibernate */
		sdcmd_AT(getval("awd"));
		break;
	case 4:		/* special hack for CS 350 and similar models */
		sdcmd_CS(0);
		break;

	case 3:		/* delayed poweroff */
		sdcmd_K(0);
		break;

	case 2:		/* instant poweroff */
		sdcmd_Z(0);
		break;
	case 1:
		/*
		 * Send a combined set of shutdown commands which can work
		 * better if the UPS gets power during shutdown process
		 * Specifically it sends both the soft shutdown 'S' and the
		 * hard hibernate '@nnn' commands
		 */

		/* S works only when OB */
		if (!(ups_status & APC_STAT_OB) || sdcmd_S(0) != STAT_INSTCMD_HANDLED)
			sdcmd_AT(getval("awd"));
		break;

	default:
		/*
		 * Send @nnn or S, depending on OB / OL status
		 */
		if (ups_status & APC_STAT_OL)		/* on line */
			sdcmd_AT(getval("awd"));
		else
			sdcmd_S(0);
	}
}

static void upsdrv_shutdown_advanced(void)
{
	const void *arg;
	const char *val;
	size_t i, len;

	val = getval("advorder");
	len = strlen(val);

	upsdebugx(1, "%s: currently: %s, advorder: %s", __func__,
		(ups_status & APC_STAT_OL) ? "on-line" : "on battery", val);

	/*
	 * try each method in the list with a little bit of handling in certain
	 * cases
	 */
	for (i = 0; i < len; i++) {
		switch (val[i] - '0') {
			case SDIDX_AT:
				arg = getval("awd");
				break;
			default:
				arg = NULL;
		}

		if (sdlist[val[i] - '0'](arg) == STAT_INSTCMD_HANDLED)
			break;	/* finish if command succeeded */
	}
}

/* power down the attached load immediately */
void upsdrv_shutdown(void)
{
	/* Only implement "shutdown.default"; do not invoke
	 * general handling of other `sdcommands` here */

	char	temp[APC_LBUF];

	if (!smartmode(1))
		upslogx(LOG_WARNING, "%s: %s", __func__, "setting SmartMode failed !");

	/* check the line status */

	if (apc_write(APC_STATUS) == 1) {
		if (apc_read(temp, sizeof(temp), SER_D1) == 1) {
			ups_status = strtol(temp, 0, 16);
		} else {
			upslogx(LOG_WARNING, "%s: %s", __func__, "status read failed, assuming LB+OB");
			ups_status = APC_STAT_LB | APC_STAT_OB;
		}
	} else {
		upslogx(LOG_WARNING, "%s: %s", __func__, "status write failed, assuming LB+OB");
		ups_status = APC_STAT_LB | APC_STAT_OB;
	}

	if (testvar("advorder") && toupper((size_t)*getval("advorder")) != 'N')
		upsdrv_shutdown_advanced();
	else
		upsdrv_shutdown_simple();
}

static int update_info(int all)
{
	int i;

	upsdebugx(1, "%s: starting scan%s", __func__, all ? " (all vars)" : "");

	for (i = 0; apc_vartab[i].name != NULL; i++) {
		if (!all && (apc_vartab[i].flags & APC_POLL) == 0)
			continue;

		if (!poll_data(&apc_vartab[i])) {
			upsdebugx(1, "%s: %s", __func__, "aborting scan");
			return 0;
		}
	}

	upsdebugx(1, "%s: %s", __func__, "scan completed");
	return 1;
}

static int setvar_enum(apc_vartab_t *vt, const char *val)
{
	int	i;
	ssize_t ret;
	char	orig[APC_LBUF], temp[APC_LBUF];
	const char	*ptr;

	apc_flush(SER_AA);
	if (apc_write((const unsigned char)vt->cmd) != 1)
		return STAT_SET_FAILED;

	ret = apc_read(orig, sizeof(orig), SER_AA);

	if ((ret < 1) || (!strcmp(orig, "NA")))
		return STAT_SET_FAILED;

	ptr = convert_data(vt, orig);

	/* suppress redundant changes - easier on the EEPROM */
	if (!strcmp(ptr, val)) {
		upslogx(LOG_INFO, "%s: ignoring SET %s='%s' (unchanged value)",
			__func__, vt->name, val);

		return STAT_SET_HANDLED;	/* FUTURE: no change */
	}

	for (i = 0; i < 32; i++) {
		if (apc_write(APC_NEXTVAL) != 1)
			return STAT_SET_FAILED;

		/* this should return either OK (if rotated) or NO (if not) */
		ret = apc_read(temp, sizeof(temp), SER_AA);

		if ((ret < 1) || (!strcmp(temp, "NA")))
			return STAT_SET_FAILED;

		/* sanity checks */
		if (!strcmp(temp, "NO"))
			return STAT_SET_FAILED;
		if (strcmp(temp, "OK"))
			return STAT_SET_FAILED;

		/* see what it rotated onto */
		if (apc_write((const unsigned char)vt->cmd) != 1)
			return STAT_SET_FAILED;

		ret = apc_read(temp, sizeof(temp), SER_AA);

		if ((ret < 1) || (!strcmp(temp, "NA")))
			return STAT_SET_FAILED;

		ptr = convert_data(vt, temp);

		upsdebugx(1, "%s: rotate - got [%s], want [%s]", __func__, ptr, val);

		if (!strcmp(ptr, val)) {	/* got it */
			upslogx(LOG_INFO, "%s: SET %s='%s'", __func__, vt->name, val);

			/* refresh data from the hardware */
			poll_data(vt);

			return STAT_SET_HANDLED;	/* FUTURE: success */
		}

		/* check for wraparound */
		if (!strcmp(ptr, orig)) {
			upslogx(LOG_SET_FAILED, "%s: variable %s wrapped", __func__, vt->name);

			return STAT_SET_FAILED;
		}
	}

	upslogx(LOG_SET_FAILED, "%s: gave up after 6 tries for %s", __func__, vt->name);

	/* refresh data from the hardware */
	poll_data(vt);

	return STAT_SET_FAILED;
}

static int setvar_string(apc_vartab_t *vt, const char *val)
{
	size_t	i;
	ssize_t	ret;
	char	temp[APC_LBUF], *ptr;

	/* sanitize length */
	if (strlen(val) > APC_STRLEN) {
		upslogx(LOG_SET_FAILED, "%s: value (%s) too long", __func__, val);
		return STAT_SET_FAILED;
	}

	memset(temp, '\0', sizeof(temp));

	apc_flush(SER_AA);
	if (apc_write((const unsigned char)vt->cmd) != 1)
		return STAT_SET_FAILED;

	ret = apc_read(temp, sizeof(temp), SER_AA);

	if ((ret < 1) || (!strcmp(temp, "NA")))
		return STAT_SET_FAILED;

	/* suppress redundant changes - easier on the EEPROM */
	if (!strcmp(temp, val)) {
		upslogx(LOG_INFO, "%s: ignoring SET %s='%s' (unchanged value)",
			__func__, vt->name, val);

		return STAT_SET_HANDLED;	/* FUTURE: no change */
	}

	/* length sanitized above */
	temp[0] = APC_NEXTVAL;
	strncpy(temp + 1, val, sizeof(temp) - 1);
	temp[sizeof(temp) - 1] = '\0';
	ptr = temp + strlen(temp);
	for (i = strlen(val); i < APC_STRLEN; i++)
		*ptr++ = '\015'; /* pad with CRs */
	*ptr = 0;

	if (apc_write_long(temp) != APC_STRLEN + 1)
		return STAT_SET_FAILED;

	ret = apc_read(temp, sizeof(temp), SER_AA);

	if (ret < 1) {
		upslogx(LOG_SET_FAILED, "%s: %s", __func__, "short final read");
		return STAT_SET_FAILED;
	}

	if (!strcmp(temp, "NO")) {
		upslogx(LOG_SET_FAILED, "%s: %s", __func__, "got NO at final read");
		return STAT_SET_FAILED;
	}

	/* refresh data from the hardware */
	poll_data(vt);

	upslogx(LOG_INFO, "%s: SET %s='%s'", __func__, vt->name, val);

	return STAT_SET_HANDLED;	/* FUTURE: success */
}

static int setvar(const char *varname, const char *val)
{
	apc_vartab_t	*vt;

	upsdebug_SET_STARTING(varname, val);

	vt = vt_lookup_name(varname);

	if (!vt)
		return STAT_SET_UNKNOWN;

	if ((vt->flags & APC_RW) == 0) {
		upslogx(LOG_SET_UNKNOWN, "%s: [%s] is not writable", __func__, varname);
		return STAT_SET_UNKNOWN;
	}

	if (vt->flags & APC_ENUM)
		return setvar_enum(vt, val);

	if (vt->flags & APC_STRING)
		return setvar_string(vt, val);

	upslogx(LOG_SET_UNKNOWN, "%s: unknown type for [%s]", __func__, varname);
	return STAT_SET_UNKNOWN;
}

/* load on */
static int do_loadon(void)
{
	apc_flush(0);
	upsdebugx(1, "%s: issuing [%s]", __func__, prtchr(APC_CMD_ON));

	if (apc_write_rep(APC_CMD_ON) != 2)
		return STAT_INSTCMD_FAILED;

	/*
	 * UPS will not reply anything after this command, but might
	 * generate brief OVER condition (which will be corrected on
	 * the next status update)
	 */

	upsdebugx(1, "%s: [%s] completed", __func__, prtchr(APC_CMD_ON));
	return STAT_INSTCMD_HANDLED;
}

/* actually send the instcmd's char to the UPS */
static int do_cmd(const apc_cmdtab_t *ct)
{
	ssize_t ret;
	int c;
	char temp[APC_LBUF];

	apc_flush(SER_AA);

	if (ct->flags & APC_REPEAT) {
		ret = apc_write_rep((const unsigned char)ct->cmd);
		c = 2;
	} else {
		ret = apc_write((const unsigned char)ct->cmd);
		c = 1;
	}

	if (ret != c) {
		return STAT_INSTCMD_FAILED;
	}

	ret = apc_read(temp, sizeof(temp), SER_AA);

	if (ret < 1)
		return STAT_INSTCMD_FAILED;

	if (strcmp(temp, "OK")) {
		upslogx(LOG_INSTCMD_FAILED, "%s: got [%s] after command [%s]",
			__func__, temp, ct->name);

		return STAT_INSTCMD_FAILED;
	}

	upslogx(LOG_INFO, "%s: %s completed", __func__, ct->name);
	return STAT_INSTCMD_HANDLED;
}

/* some commands must be repeated in a window to execute */
static int instcmd_chktime(apc_cmdtab_t *ct, const char *ext)
{
	double	elapsed;
	time_t	now;
	static	time_t	last = 0;

	time(&now);

	elapsed = difftime(now, last);
	last = now;

	/* you have to hit this in a small window or it fails */
	if ((elapsed < MINCMDTIME) || (elapsed > MAXCMDTIME)) {
		upsdebugx(1, "%s: outside window for [%s %s] (%2.0f)",
				__func__, ct->name, ext ? ext : "\b", elapsed);
		return 0;
	}

	return 1;
}

static int instcmd(const char *cmd, const char *ext)
{
	int i;
	apc_cmdtab_t *ct = NULL;

	upsdebug_INSTCMD_STARTING(cmd, ext);

	for (i = 0; apc_cmdtab[i].name != NULL; i++) {
		/* cmd must match */
		if (strcasecmp(apc_cmdtab[i].name, cmd))
			continue;
		/* if cmd specifies regex, ext must match */
		if (apc_cmdtab[i].ext) {
			if (!rexhlp(apc_cmdtab[i].ext, ext))
				continue;
		/* if cmd doesn't specify regex, ext must be NULL */
		} else {
			if (ext)
				continue;
		}
		ct = &apc_cmdtab[i];
		break;
	}

	if (!ct) {
		upslog_INSTCMD_UNKNOWN(cmd, ext);
		return STAT_INSTCMD_UNKNOWN;
	}

	if (!(ct->flags & APC_PRESENT)) {
		upslogx(LOG_INSTCMD_INVALID, "%s: command [%s %s] recognized, but"
		       " not supported by your UPS model", __func__, cmd,
				ext ? ext : "\b");
		return STAT_INSTCMD_INVALID;
	}

	/* first verify if the command is "nasty" */
	if ((ct->flags & APC_NASTY) && !instcmd_chktime(ct, ext))
		return STAT_INSTCMD_HANDLED;	/* future: again */

	/* we're good to go, handle special stuff first, then generic cmd */

	if (!strcasecmp(cmd, "calibrate.start")) {
		upslog_INSTCMD_POWERSTATE_MAYBE(cmd, ext);
		return do_cal(1);
	}

	if (!strcasecmp(cmd, "calibrate.stop")) {
		upslog_INSTCMD_POWERSTATE_MAYBE(cmd, ext);
		return do_cal(0);
	}

	if (!strcasecmp(cmd, "load.on")) {
		upslog_INSTCMD_POWERSTATE_MAYBE(cmd, ext);
		return do_loadon();
	}

	if (!strcasecmp(cmd, "load.off")) {
		upslog_INSTCMD_POWERSTATE_CHANGE(cmd, ext);
		return sdcmd_Z(0);
	}

	if (!strcasecmp(cmd, "shutdown.stayoff")) {
		upslog_INSTCMD_POWERSTATE_CHANGE(cmd, ext);
		return sdcmd_K(0);
	}

	if (!strcasecmp(cmd, "shutdown.return")) {
		upslog_INSTCMD_POWERSTATE_CHANGE(cmd, ext);

		if (!ext || !*ext)
			return sdcmd_S(0);

		if (toupper((size_t)*ext) == 'A')
			return sdcmd_AT(ext + 3);

		if (toupper((size_t)*ext) == 'C')
			return sdcmd_CS(0);
	}

	/* nothing special here */
	upslog_INSTCMD_POWERSTATE_CHECKED(cmd, ext);
	return do_cmd(ct);
}

/* install pointers to functions for msg handlers called from msgparse */
static void setuphandlers(void)
{
	upsh.setvar = setvar;
	upsh.instcmd = instcmd;
}

/* ---- functions that interface with main.c ------------------------------- */

void upsdrv_makevartable(void)
{
	addvar(VAR_VALUE, "ttymode", "tty discipline selection");
	addvar(VAR_VALUE, "cable", "alternate cable (940-0095B) selection");
	addvar(VAR_VALUE, "awd", "hard hibernate's additional wake-up delay");
	addvar(VAR_VALUE, "sdtype", "simple shutdown method");
	addvar(VAR_VALUE, "advorder", "advanced shutdown control");
	addvar(VAR_VALUE, "cshdelay", "CS hack delay");
}

void upsdrv_help(void)
{
	printf(
		"\nFor detailed information, please refer to:\n"
		  " - apcsmart(8)\n"
		  " - https://www.networkupstools.org/docs/man/apcsmart.html\n"
	      );
}

void upsdrv_initups(void)
{
	char *val;
	apc_vartab_t *ptr;

	/* sanitize awd (additional wake-up delay of '@' command) */
	if ((val = getval("awd")) && !rexhlp(APC_AWDFMT, val)) {
			fatalx(EXIT_FAILURE, "invalid value (%s) for option 'awd'", val);
	}

	/* sanitize sdtype */
	if ((val = getval("sdtype")) && !rexhlp(APC_SDFMT, val)) {
			fatalx(EXIT_FAILURE, "invalid value (%s) for option 'sdtype'", val);
	}

	/* sanitize advorder */
	if ((val = getval("advorder")) && !rexhlp(APC_ADVFMT, val)) {
			fatalx(EXIT_FAILURE, "invalid value (%s) for option 'advorder'", val);
	}

	/* sanitize cshdelay */
	if ((val = getval("cshdelay")) && !rexhlp(APC_CSHDFMT, val)) {
			fatalx(EXIT_FAILURE, "invalid value (%s) for option 'cshdelay'", val);
	}

	upsfd = extrafd = ser_open(device_path);
	apc_ser_set();

	/* fill length values */
	for (ptr = apc_vartab; ptr->name; ptr++)
		ptr->nlen0 = strlen(ptr->name) + 1;
}

void upsdrv_cleanup(void)
{
	char temp[APC_LBUF];

	if (INVALID_FD(upsfd))
		return;

	apc_flush(0);
	/* try to bring the UPS out of smart mode */
	apc_write(APC_GODUMB);
	apc_read(temp, sizeof(temp), SER_TO);
	ser_close(upsfd, device_path);
}

void upsdrv_initinfo(void)
{
	const char *pmod, *pser;

	if (!smartmode(5)) {
		fatalx(EXIT_FAILURE,
			"unable to detect an APC Smart protocol UPS on port %s\n"
			"check the cabling, port name or model name and try again", device_path
			);
	}

	if (!getbaseinfo()) {
		fatalx(EXIT_FAILURE,
			"Problems with communicating APC UPS on port %s\n", device_path
			);
	}

	/* manufacturer ID - hard-coded in this particular module */
	dstate_setinfo("ups.mfr", "APC");

	if (!(pmod = dstate_getinfo("ups.model")))
		pmod = "\"unknown model\"";
	if (!(pser = dstate_getinfo("ups.serial")))
		pser = "unknown serial";

	upsdebugx(1, "%s: detected %s [%s] on %s",
		__func__, pmod, pser, device_path);

	setuphandlers();
	/*
	 * seems to be ok so far, it must be set so initial call of
	 * upsdrv_updateinfo() doesn't begin with stale condition
	 */
	dstate_dataok();
}

void upsdrv_updateinfo(void)
{
	static int last_worked = 0;
	static time_t last_full = 0;
	int all;
	time_t now;

	/* try to wake up a dead UPS once in awhile */
	if (dstate_is_stale()) {
		char temp[LARGEBUF];
		size_t count = 0;

		if (!last_worked)
			upsdebugx(1, "%s: %s", __func__, "comm lost");

		/* reset this so a full update runs when the UPS returns */
		last_full = 0;

		/* Flush the buffer in case it helps,
		 * or sleep 1 sec if buffer is empty */
		while (select_read(upsfd, temp, sizeof(temp), 1, 0) > 0) {
			count++;
		}
		errno = 0;

		if (!count) {
			/* Do not flood the device (and our logs, below)
			 * with retry attempts */
			usleep(1000000);
		}

		if (++last_worked < 10)
			return;

		/* become aggressive after a few tries */
		if (!(last_worked % 60)) {
			upslogx(LOG_WARNING, "Trying to reconnect to the UPS");
			dstate_setinfo("driver.state", "reconnect.trying");

			upsdebugx(1, "%s: call upsdrv_cleanup", __func__);
			/* dstate_setinfo("driver.state", "cleanup.upsdrv"); */
			upsdrv_cleanup();

			upsdebugx(1, "%s: sleep 5 sec", __func__);
			usleep(5000000);

			upsdebugx(1, "%s: call upsdrv_initups", __func__);
			/* dstate_setinfo("driver.state", "init.device"); */
			upsdrv_initups();

			upsdebugx(1, "%s: call upsdrv_initinfo", __func__);
			/* dstate_setinfo("driver.state", "init.info"); */
			upsdrv_initinfo();

			upsdebugx(1, "%s: call upsdrv_updateinfo", __func__);
			dstate_setinfo("driver.state", "reconnect.updateinfo");
			/* dstate_setinfo("driver.state", "init.updateinfo"); */
			upsdrv_updateinfo();

			dstate_setinfo("driver.state", "init.quiet");
		}

		upsdebugx(1, "%s: nudging UPS with 'Y', iteration #%d ...",
			__func__, last_worked);
		if (!smartmode(1)) {
			return;
		}

		if (last_worked > 10)
			upslogx(LOG_WARNING, "%s: recovered the connection", __func__);
		upsdebugx(1, "%s: recovered the connection", __func__);
		last_worked = 0;
	}

	if (!update_status()) {
		dstate_datastale();
		return;
	}

	time(&now);

	/* refresh all variables hourly */
	/* does not catch measure-ups II insertion/removal */
	if (difftime(now, last_full) > 3600) {
		last_full = now;
		all = 1;
	} else
		all = 0;

	if (update_info(all)) {
		dstate_dataok();
	} else {
		dstate_datastale();
	}
}
