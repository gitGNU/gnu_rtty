/* locbrok.h - defs for location broker
 * vix 13sep91 [written]
 *
 * $Id: locbrok.h,v 1.1 1992-01-02 02:04:18 vixie Exp $
 */

#define	LB_SERVNAME	"locbrok"
#define LB_SERVPORT	160
#define	LB_MAXNAMELEN	64

typedef struct locbrok {
	unsigned short	lb_port;
	unsigned short	lb_nlen;
	char		lb_name[LB_MAXNAMELEN];
} locbrok;
