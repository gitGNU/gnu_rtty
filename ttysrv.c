/* ttysrv - serve a tty to stdin/stdout, a named pipe, or a network socket
 * vix 28may91 [written]
 */

#ifndef LINT
static char RCSid[] = "$Id: ttysrv.c,v 1.10 1994-04-11 20:36:00 vixie Exp $";
#endif

#include <stdio.h>
#include <termios.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <netdb.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/param.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "rtty.h"
#ifdef NEED_BITYPES_H
# include "bitypes.h"
#endif
#include "ttyprot.h"
#ifdef WANT_TCPIP
# include "locbrok.h"
#endif

struct whoson {
	char *who, *host, *auth;
	time_t lastInput;
	enum {local, remote} type;
	enum {wlogin, wpasswd, auth} state;
	int aux;
};

#define MAX_AUTH_ATTEMPTS 3
#define USAGE_STR "{-o option} [-s LServ|-r RServ] [-l Log]\n\
	-t Tty [-b Baud] [-p Parity] [-w Wordsize] [-i Pidfile]"

#ifdef USE_STDLIB
#include <stdlib.h>
#else
extern	void		*calloc __P((size_t, size_t)),
			*malloc __P((size_t)),
			*realloc __P((void *, size_t)),
			free __P((void *));
#endif

#ifdef USE_UNISTD
#include <unistd.h>
#else
extern	int		gethostname __P((char *, int));
extern	char		*crypt __P((const char *key, const char *setting));
#endif

extern	int		optind, opterr,
			getopt __P((int, char * const *, const char *));
extern	char		*optarg;

#ifdef DEBUG
int			Debug = 0;
#endif

extern	int		rconnect(char *host, char *service,
				 FILE *verbose, FILE *errors, int timeout);
extern	char		Version[];

static	char		*ProgName = "amnesia",
			*LServSpec = NULL,
			*RServSpec = NULL,
			*TtySpec = NULL,
			*LogSpec = NULL,
			*Parity = "none",
			ParityBuf[TP_MAXVAR],
			*PidFile = NULL;
static	int		LServ = -1,
			RServ = -1,
			Tty = -1,
			Ttyios_set = 0,
			LogDirty = FALSE,
			Baud = 9600,
			Wordsize = 8,
			highest_fd = -1,
			LocBrok = -1,
			Sigpiped = 0;
static	unsigned short	Port;
static	struct termios	Ttyios, Ttyios_orig;
static	FILE		*LogF = NULL;
static	fd_set		Clients;
static	ttyprot		T;
static	time_t		Now;
static	char		Hostname[MAXHOSTNAMELEN];

static	struct timeval	TOinput = {0, 3000};	/* 3ms: >1byte @9600baud */
static	struct timeval	TOflush = {1, 0};	/* 1 second */
static	struct whoson	**WhosOn;

static	char		*handle_option __P((char *));
static	void		main_loop __P((void)),
			tty_input __P((int, int)),
			broadcast __P((unsigned char *, int, unsigned int)),
			sigpipe __P((int)),
			sighup __P((int)),
			open_log __P((void)),
			quit __P((int)),
			serv_input __P((int)),
			client_input __P((int)),
			close_client __P((int)),
			set_parity __P((u_int)),
			set_wordsize __P((u_int)),
			auth_needed __P((int)),
			auth_ok __P((int));

static	int		set_baud __P((int)),
			find_parity __P((char *)),
			find_wordsize __P((int));

int
main(argc, argv)
	int argc;
	char *argv[];
{
	int i;
	char ch, *msg;

	gethostname(Hostname, sizeof Hostname);
	ProgName = argv[0];

	while ((ch = getopt(argc, argv, "o:s:r:t:l:b:p:w:x:i:")) != EOF) {
		switch (ch) {
		case 'o':
			msg = handle_option(optarg);
			if (msg) {
				USAGE((stderr, "%s: bad option (%s): %s\n",
				       ProgName, optarg, msg));
			}
		case 's':
			LServSpec = optarg;
			break;
		case 'r':
#ifdef WANT_TCPIP
			RServSpec = optarg;
#else
			USAGE((stderr, "%s: -r not supported on this system\n",
			       ProgName));
#endif
			break;
		case 't':
			TtySpec = optarg;
			break;
		case 'l':
			LogSpec = optarg;
			break;
		case 'b':
			Baud = atoi(optarg);
			break;
		case 'p':
			Parity = optarg;
			break;
		case 'w':
			Wordsize = atoi(optarg);
			break;
#ifdef DEBUG
		case 'x':
			Debug = atoi(optarg);
			break;
#endif
		case 'i':
			PidFile = optarg;
			break;
		default:
			USAGE((stderr, "%s: getopt=%c ?\n", ProgName, ch));
		}
	}

	if (!TtySpec) {
		USAGE((stderr, "%s: must specify -t ttyspec ?\n", ProgName));
	}
	if (0 > (Tty = open(TtySpec, O_NONBLOCK|O_RDWR))) {
		fprintf(stderr, "%s: can't open tty ", ProgName);
		perror(TtySpec);
		exit(2);
	}
	dprintf(stderr, "ttysrv.main: tty open on fd%d\n", Tty);
	tcgetattr(Tty, &Ttyios);
	Ttyios_orig = Ttyios;
	prepare_term(&Ttyios);
	set_baud(Baud);

	if ((i = find_parity(Parity)) == -1) {
		USAGE((stderr, "%s: parity %s ?\n", ProgName, Parity));
	}
	set_parity(i);

	if ((i = find_wordsize(Wordsize)) == -1) {
		USAGE((stderr, "%s: wordsize %d ?\n", ProgName, Wordsize));
	}
	set_wordsize(i);

	signal(SIGINT, quit);
	signal(SIGQUIT, quit);
	install_ttyios(Tty, &Ttyios);
	Ttyios_set++;

	if (!LServSpec && !RServSpec) {
		USAGE((stderr, "%s: must specify either -s or -r\n",
		       ProgName));
	}
	if (LServSpec) {
		struct sockaddr_un n;
		struct stat statbuf;

		if (LServSpec[0] != '/') {
			USAGE((stderr, "%s: -s must specify local pathname\n",
			       ProgName));
		}

		LServ = socket(PF_UNIX, SOCK_STREAM, 0);
		ASSERT(LServ>=0, "socket");

		n.sun_family = AF_UNIX;
		(void) strcpy(n.sun_path, LServSpec);

		if (stat(LServSpec, &statbuf) >= 0) {
			fprintf(stderr, "warning: removing \"%s\"\n",
				LServSpec);
			if ((statbuf.st_mode & S_IFMT) == S_IFSOCK) {
				if (unlink(LServSpec) < 0) perror("unlink");
			}
		}

		ASSERT(0<=bind(LServ, (struct sockaddr *)&n, sizeof n),
		       n.sun_path);
	}
#ifdef WANT_TCPIP
	if (RServSpec) {
		struct sockaddr_in n;
		int nlen = sizeof n;

		RServ = socket(PF_INET, SOCK_STREAM, 0);
		ASSERT(RServ>=0, "socket");

		n.sin_family = AF_INET;
		n.sin_port = 0;
		n.sin_addr.s_addr = INADDR_ANY;
		ASSERT(0<=bind(RServ, (struct sockaddr *)&n, sizeof n),
		       "bind");

		ASSERT(0<=getsockname(RServ, (struct sockaddr *)&n, &nlen),
		       "getsockname");
		Port = ntohs(n.sin_port);
		fprintf(stderr, "serving internet port %d\n", Port);

		/* register with the location broker, or die */
		{
			int len = min(LB_MAXNAMELEN, strlen(RServSpec));
			locbrok lb;

			LocBrok = rconnect("127.1", "locbrok", NULL,stderr,0);
			ASSERT(LocBrok>0, "rconnect locbrok");
			lb.lb_port = htons(Port);
			lb.lb_nlen = htons(len);
			strncpy(lb.lb_name, RServSpec, len);
			ASSERT(0<write(LocBrok, &lb, sizeof lb),
			       "write locbrok")
		}
	}
#endif

	if (LogSpec) {
		open_log();
	}

	if (PidFile) {
		FILE *f = fopen(PidFile, "w");
		if (!f) {
			perror(PidFile);
		} else {
			fprintf(f, "%d\n", getpid());
			fclose(f);
		}
	}

	WhosOn = (struct whoson **) calloc(getdtablesize(),
					   sizeof(struct whoson **));

	main_loop();
}

static void
main_loop()
{
	if (LServ != -1)
		listen(LServ, 10);
	if (RServ != -1)
		listen(RServ, 10);
	dprintf(stderr, "ttysrv.main: LServ=%d, RServ=%d\n", LServ, RServ);
	signal(SIGPIPE, sigpipe);
	signal(SIGHUP, sighup);
	FD_ZERO(&Clients);
	highest_fd = max(max(LServ, RServ), Tty);

	for (;;) {
		fd_set readfds;
		register int nfound, fd;

		readfds = Clients;
		if (LServ != -1)
			FD_SET(LServ, &readfds);
		if (RServ != -1)
			FD_SET(RServ, &readfds);
		FD_SET(Tty, &readfds);
#if 0
		dprintf(stderr, "ttysrv.main_loop: select(%d,%08x)\n",
			highest_fd+1, readfds.fds_bits[0]);
#endif
		nfound = select(highest_fd+1, &readfds, NULL, NULL,
				(LogDirty ?&TOflush :NULL));
		if (nfound < 0 && errno == EINTR)
			continue;
		if (nfound == 0 && LogDirty && LogF) {
			fflush(LogF);
			LogDirty = FALSE;
		}
		Now = time(0);
#if 0
		dprintf(stderr, "ttysrv.main_loop: select->%d\n", nfound);
#endif
		for (fd = 0; fd <= highest_fd; fd++) {
			if (!FD_ISSET(fd, &readfds)) {
				continue;
			}
			dprintf(stderr, "ttysrv.main_loop: fd%d readable\n",
				fd);
			if (fd == Tty) {
				tty_input(fd, FALSE);
				tty_input(fd, TRUE);
			} else if ((fd == LServ) || (fd == RServ)) {
				serv_input(fd);
			} else {
				client_input(fd);
			}
		}
	}
}

static void
tty_input(fd, aggregate) {
	int nchars, x;
	unsigned char buf[TP_MAXVAR];

	nchars = 0;
	do {
		x = read(fd, buf+nchars, TP_MAXVAR-nchars);
	} while ((x > 0) &&
		 ((nchars += x) < TP_MAXVAR) &&
		 (aggregate && !select(0, NULL, NULL, NULL, &TOinput))
		 )
	;
	if (nchars == 0) return;
#ifdef DEBUG
	if (Debug) {
		fprintf(stderr, "ttysrv.tty_input: %d bytes read on fd%d",
			nchars, fd);
		if (Debug > 1) {
			fputs(": \"", stderr);
			cat_v(stderr, buf, nchars);
			fputs("\"", stderr);
		}
		fputc('\n', stderr);
	}
#endif
	if (LogF) {
		if (nchars != fwrite(buf, sizeof(char), nchars, LogF)) {
			perror("fwrite(LogF)");
		} else {
			LogDirty = TRUE;
		}
	}
	broadcast(buf, nchars, TP_DATA);
}

static void
broadcast(buf, nchars, typ)
	unsigned char *buf;
	int nchars;
	unsigned int typ;
{
	register int fd, x;

	for (fd = 0;  fd <= highest_fd;  fd++) {
		if (!FD_ISSET(fd, &Clients)) {
			continue;
		}

		Sigpiped = 0;
		x = tp_senddata(fd, buf, nchars, typ);
		dprintf(stderr,
			"ttysrv.tty_input: %d bytes sent to client on fd%d\n",
			x, fd);
		if (Sigpiped) {
			dprintf(stderr, "ttysrv.tty_input: sigpipe on fd%d\n",
				fd);
			close_client(fd);
		}
	}
}

static void
serv_input(fd) {
	struct sockaddr_un un;
	struct sockaddr_in in;
	struct sockaddr *sa;
	int fromlen;

	if (fd == LServ) {
		sa = (struct sockaddr *) &un;
		fromlen = sizeof(un);
	} else if (fd == RServ) {
		sa = (struct sockaddr *) &in;
		fromlen = sizeof(in);
	} else {
		fprintf(stderr, "%s: panic - serv_input(%d)\n", ProgName, fd);
		abort();
	}

	dprintf(stderr, "ttysrv.serv_input: accepting on fd%d\n", fd);

	if ((fd = accept(fd, sa, &fromlen)) == -1) {
		perror("accept");
		return;
	}

	dprintf(stderr, "ttysrv.serv_input: accepted fd%d\n", fd);

	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0)|O_NONBLOCK);
	FD_SET(fd, &Clients);
	if (fd > highest_fd) {
		highest_fd = fd;
	}
	if (!WhosOn[fd]) {
		WhosOn[fd] = (struct whoson *) malloc(sizeof(struct whoson));
	}
	WhosOn[fd]->who = NULL;
	WhosOn[fd]->lastInput = Now;
	WhosOn[fd]->auth = NULL;
	if (sa == (struct sockaddr *) &un) {
		WhosOn[fd]->host = strsave(Hostname);
		WhosOn[fd]->type = local;
		auth_ok(fd);
	} else if (sa == (struct sockaddr *) &in) {
		struct hostent *hp, *gethostbyaddr();

		hp = gethostbyaddr((char *)&in.sin_addr,
				   sizeof(in.sin_addr),
				   in.sin_family);
		WhosOn[fd]->host = strsave(hp
					   ?hp->h_name
					   :inet_ntoa(in.sin_addr));
		WhosOn[fd]->type = remote;
		auth_needed(fd);
	} else {
		fprintf(stderr, "%s: panic - serv_input #2\n", ProgName);
		abort();
	}
}

static void
client_input(fd) {
	register int nchars;
	register int i, new, query;
	unsigned int f;
	unsigned short salt;
	struct passwd *pw, *getpwnam();
	char s[3];

	if (!WhosOn[fd])
		return;
	WhosOn[fd]->lastInput = Now;

	/* read the fixed part of the ttyprot (everything but the array)
	 */
	if (TP_FIXED != (nchars = read(fd, &T, TP_FIXED))) {
		dprintf(stderr, "client_input: read=%d on fd%d: ", nchars, fd);
#ifdef DEBUG
		if (Debug) perror("read");
#endif
		close_client(fd);
		return;
	}
#ifdef DEBUG_not
	cat_v(stderr, &T, nchars);
#endif
	i = ntohs(T.i);
	query = ntohs(T.f) & TP_QUERY;
	f = ntohs(T.f) & TP_TYPEMASK;
#ifdef DEBUG
	if (Debug) {
		fprintf(stderr,
			"ttysrv: #%d fd%d i=0x%x o='%c' f=0x%x",
			nchars, fd, i, query?'Q':' ', f);
		if (Debug > 1) {
			fputs(": \"", stderr);
			cat_v(stderr, (u_char *)&T, nchars);
			fputs("\"", stderr);
		}
		fputc('\n', stderr);
	}
#endif
	switch (f) {
	case TP_DATA:
		if (!(nchars = tp_getdata(fd, &T))) {
			close_client(fd);
			break;
		}
		if (WhosOn[fd]->state != auth)
			break;
# if WANT_CLIENT_LOGGING
		if (LogF) {
			if (nchars != fwrite(T.c, sizeof(char), nchars, LogF)){
				perror("fwrite(LogF)");
			} else {
				LogDirty = TRUE;
			}
		}
# endif /*WANT_CLIENT_LOGGING*/
		nchars = write(Tty, T.c, nchars);
		break;
	case TP_BREAK:
		if (WhosOn[fd]->state != auth)
			break;
		dprintf(stderr, "ttysrv.client_input: sending break\n");
		tcsendbreak(Tty, 0);
		tp_senddata(fd, (u_char *)"BREAK", 5, TP_NOTICE);
		if (LogF) {
			fputs("[BREAK]", LogF);
		}
		dprintf(stderr, "ttysrv.client_input: done sending break\n");
		break;
	case TP_BAUD:
		if (WhosOn[fd]->state != auth)
			break;
		if (query) {
			tp_sendctl(fd, TP_BAUD|TP_QUERY, Baud, NULL);
			break;
		}
		if ((set_baud(i) >= 0) &&
		    (install_ttyios(Tty, &Ttyios) >= 0)) {
			Baud = i;
			if (LogF) {
				fprintf(LogF, "[baud now %d]", i);
			}
			tp_sendctl(fd, TP_BAUD, 1, NULL);
		} else {
			tp_sendctl(fd, TP_BAUD, 0, NULL);
		}
		break;
	case TP_PARITY:
		if (!query) {
			if (!(nchars = tp_getdata(fd, &T))) {
				close_client(fd);
				break;
			}
			T.c[i] = '\0';		/* XXX */
		}
		if (WhosOn[fd]->state != auth)
			break;
		if (query) {
			tp_sendctl(fd, TP_PARITY|TP_QUERY,
				   strlen(Parity), (unsigned char *)Parity);
			break;
		}
		if (-1 == (new = find_parity((char *)T.c))) {
			tp_sendctl(fd, TP_PARITY, 0, NULL);
		} else {
			strcpy(ParityBuf, (char *)T.c);
			Parity = ParityBuf;
			set_parity(new);
			install_ttyios(Tty, &Ttyios);
			if (LogF) {
				fprintf(LogF, "[parity now %s]", (char*) T.c);
			}
			tp_sendctl(fd, TP_PARITY, 1, NULL);
		}
		break;
	case TP_WORDSIZE:
		if (WhosOn[fd]->state != auth)
			break;
		if (query) {
			tp_sendctl(fd, TP_WORDSIZE|TP_QUERY, Wordsize, NULL);
			break;
		}
		if (-1 == (new = find_wordsize(i))) {
			tp_sendctl(fd, TP_WORDSIZE, 0, NULL);
		} else {
			Wordsize = i;
			set_wordsize(new);
			install_ttyios(&Tty, &Ttyios);
			if (LogF) {
				fprintf(LogF, "[wordsize now %d]", i);
			}
			tp_sendctl(fd, TP_WORDSIZE, 1, NULL);
		}
		break;
	case TP_WHOSON:
		if (!query) {
			if (!(nchars = tp_getdata(fd, &T))) {
				close_client(fd);
				break;
			}
			T.c[i] = '\0';		/* XXX */
		}
		if (WhosOn[fd]->state != auth)
			break;
		if (query) {
			int iwho;

			for (iwho = getdtablesize()-1;  iwho >= 0;  iwho--) {
				struct whoson *who = WhosOn[iwho];
				char data[TP_MAXVAR];
				int idle;

				if (!who)
					continue;
				idle = Now - who->lastInput;
				sprintf(data, "%s [%s] (idle %d sec%s)",
					who->who ?who->who :"undeclared",
					who->host ?who->host :"?",
					idle, (idle==1) ?"" :"s");
				tp_senddata(fd, (u_char *)data, strlen(data),
					    TP_NOTICE);
			}
			break;
		}
		if (WhosOn[fd]) {
			if (WhosOn[fd]->who)
				free(WhosOn[fd]->who);
			WhosOn[fd]->who = strsave((char *)T.c);
		}
		{ /*local*/
			char buf[TP_MAXVAR];

			sprintf(buf, "%-*.*s connected\07", i, i, T.c);
			broadcast((u_char *)buf, strlen(buf), TP_NOTICE);
		}
		break;
	case TP_TAIL:
		if (WhosOn[fd]->state != auth)
			break;
		if (!query)
			break;
		if (!LogF)
			break;
		fflush(LogF);
		LogDirty = FALSE;
		if (ftell(LogF) < 1024L) {
			if (0 > fseek(LogF, 0, SEEK_SET))
				break;
		} else {
			if (0 > fseek(LogF, -1024, SEEK_END))
				break;
		}
		{ /*local*/
			char buf[TP_MAXVAR];
			int len, something = FALSE;

			while (0 < (len = fread(buf, sizeof(char), sizeof buf,
						LogF))) {
				if (!something) {
					tp_senddata(fd, (u_char*)"tail+", 5,
						    TP_NOTICE);
					something = TRUE;
				}
				tp_senddata(fd, (u_char*)buf, len, TP_DATA);
			}
			if (something) {
				tp_senddata(fd, (u_char*)"tail-", 5,
					    TP_NOTICE);
			}
		}
		break;
	case TP_VERSION:
		if (!query)
			break;
		tp_senddata(fd, (u_char*)Version, strlen(Version), TP_NOTICE);
		break;
	case TP_LOGIN:
		if (!(nchars = tp_getdata(fd, &T))) {
			close_client(fd);
			break;
		}
		T.c[i] = '\0';		/* XXX */
		if (query)
			break;
		if (WhosOn[fd]->state != wlogin)
			break;
		pw = getpwnam((char*)T.c);
		if (!pw) {
			char data[TP_MAXVAR];

			sprintf(data, "%s - no such user", T.c);
			tp_senddata(fd, (u_char*)data, strlen(data),
				    TP_NOTICE);
		} else if (!pw->pw_passwd[0]) {
			auth_ok(fd);
		} else {
			WhosOn[fd]->state = wpasswd;
			WhosOn[fd]->auth = strsave(pw->pw_passwd);
			salt = WhosOn[fd]->auth[0]<<8 | WhosOn[fd]->auth[1];
			tp_sendctl(fd, TP_PASSWD|TP_QUERY, salt, NULL);
		}
		break;
	case TP_PASSWD:
		if (!(nchars = tp_getdata(fd, &T))) {
			close_client(fd);
			break;
		}
		T.c[i] = '\0';		/* XXX */
		if (query)
			break;
		if (WhosOn[fd]->state != wpasswd)
			break;
		strncpy(s, WhosOn[fd]->auth, 2);
		if (!strcmp((char*)T.c, WhosOn[fd]->auth)) {
			auth_ok(fd);
		} else {
			char data[TP_MAXVAR];

			sprintf(data, "login incorrect");
			if (++WhosOn[fd]->aux > MAX_AUTH_ATTEMPTS) {
				close_client(fd);
			} else {
				tp_senddata(fd, (u_char*)data, strlen(data),
					    TP_NOTICE);
				auth_needed(fd);
			}
		}
		break;
	default:
		fprintf(stderr, "ttysrv: bad T: f=0x%x\n", ntohs(T.f));
		break;
	}
}

static void
close_client(fd) {
	dprintf(stderr, "close_client: fd%d\n", fd);
	close(fd);
	FD_CLR(fd, &Clients);
	if (WhosOn[fd]) {
		if (WhosOn[fd]->who) {
			char buf[TP_MAXVAR];

			sprintf(buf, "%s disconnected\07", WhosOn[fd]->who);
			broadcast((u_char*)buf, strlen(buf), TP_NOTICE);
			free(WhosOn[fd]->who);
		}
		free(WhosOn[fd]->host);
		if (WhosOn[fd]->auth)
			free(WhosOn[fd]->auth);
		free((char *) WhosOn[fd]);
		WhosOn[fd] = (struct whoson *) NULL;
	}
}

struct partab { char *parity; int sysparity; } partab[] = {
	{ "even", PARENB },
	{ "odd", PARENB|PARODD },
	{ "none", 0 },
	{ NULL, -1 }
};

struct cstab { int wordsize, syswordsize; } cstab[] = {
	{ 5, CS5 },
	{ 6, CS6 },
	{ 7, CS7 },
	{ 8, CS8 },
	{ 0, -1 }
};

static int
set_baud(baud) {
	if (cfsetispeed(&Ttyios, baud) < 0)
		return -1;
	if (cfsetospeed(&Ttyios, baud) < 0)
		return -1;
	return 0;
}

static int
find_parity(parity)
	char *parity;
{
	struct partab *parp;
	int sysparity = -1;

	for (parp = partab;  parp->parity;  parp++) {
		if (!strcmp(parp->parity, parity)) {
			sysparity = parp->sysparity;
		}
	}
	return sysparity;
}

static void
set_parity(parity)
	unsigned int parity;
{
	Ttyios.c_cflag &= ~(PARENB|PARODD);
	Ttyios.c_cflag |= parity;
}

static int
find_wordsize(wordsize) {
	struct cstab *csp;
	int syswordsize = -1;

	for (csp = cstab;  csp->wordsize;  csp++) {
		if (csp->wordsize == wordsize)
			syswordsize = csp->syswordsize;
	}
	return syswordsize;
}

static void
set_wordsize(wordsize)
	unsigned int wordsize;
{
	Ttyios.c_cflag &= ~CSIZE;
	Ttyios.c_cflag |= wordsize;
}

static char *
handle_option(option)
	char *option;
{
	if (!strcmp("nolocal", option)) {
		Ttyios.c_cflag &= ~CLOCAL;
	} else {
		return "unrecognized";
	}
	return NULL;
}

static void
auth_needed(fd) {
	char data[TP_MAXVAR];

	sprintf(data, "authorization needed");
	tp_senddata(fd, (u_char*)data, strlen(data), TP_NOTICE);
	WhosOn[fd]->state = wlogin;
	tp_sendctl(fd, TP_LOGIN|TP_QUERY, 0, NULL);
}

static void
auth_ok(fd) {
	char data[TP_MAXVAR];

	sprintf(data, "authorized");
	tp_senddata(fd, (u_char*)data, strlen(data), TP_NOTICE);
	WhosOn[fd]->state = auth;
}

static void
sigpipe(x) {
	Sigpiped++;
}

static void
sighup(x) {
	if (LogF) {
		fclose(LogF);
		LogF = NULL;
		LogDirty = FALSE;
		open_log();
	}
}

static void
open_log() {
	if (!(LogF = fopen(LogSpec, "a+"))) {
		perror(LogSpec);
		fprintf(stderr, "%s: can't open log file\n", ProgName);
	}
}

static void
quit(x) {
	fprintf(stderr, "\r\nttysrv exiting\r\n");
	if (Ttyios_set && (Tty != -1)) {
		tcsetattr(Tty, TCSANOW, &Ttyios_orig);
	}
	exit(0);
}
