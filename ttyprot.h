/* ttyproto.h - define protocol used by ttysrv and its clients
 * vix 29may91 [written]
 *
 * $Id: ttyprot.h,v 1.6 1993-12-28 00:49:56 vixie Exp $
 */

#include <termios.h>

#define TP_TYPEMASK	0x00ff
#define	TP_DATA		0x0001	/* inband data (query=ignored) */
#define	TP_BAUD		0x0002
#define	TP_PARITY	0x0003
#define	TP_WORDSIZE	0x0004
#define	TP_BREAK	0x0005	/* send break (query=ignored) */
#define	TP_WHOSON	0x0006	/* who's connected to this tty? (set=="me") */
#define	TP_TAIL		0x0007	/* what's happened recently? (set==ignored) */
#define	TP_NOTICE	0x0008	/* same as DATA but generated by server */
#define	TP_VERSION	0x0009	/* what's your version number? (set==ignore) */
#define	TP_LOGIN	0x000a
#define	TP_PASSWD	0x000b	/* query's "i" field is the salt (netorder) */

#define	TP_OPTIONMASK	0xff00
#define TP_QUERY	0x0100

#define	TP_FIXED	(sizeof(unsigned short) + sizeof(unsigned short))
#define	TP_MAXVAR	468	/* 512 - 40 - TP_FIXED */

typedef struct ttyprot {
	u_int16_t	f;
	u_int16_t	i;
	unsigned char	c[TP_MAXVAR];
} ttyprot;

int tp_senddata __P((int, u_char *, int, int));
int tp_sendctl __P((int, u_int, u_int, u_char *));
int tp_getdata __P((int, ttyprot *));
void cat_v __P((FILE *, u_char *, int));
char *strsave __P((char *));
int install_termios __P((int, struct termios *));
void prepare_term __P((struct termios *));
