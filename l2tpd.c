/*
 * Layer Two Tunnelling Protocol Daemon
 * Copyright (C) 1998 Adtran, Inc.
 *
 * Mark Spencer
 *
 * This software is distributed under the terms
 * of the GPL, which you should have received
 * along with this source.
 *
 * Main Daemon source.
 *
 */

#include <stdlib.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#if (__GLIBC__ < 2)
# if defined(FREEBSD)
#  include <sys/signal.h>
# elif defined(LINUX)
#  include <bsd/signal.h>
# elif defined(SOLARIS)
#  include <signal.h>
# endif
#else
# include <signal.h>
#endif
#include <netdb.h>
#include <string.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef USE_KERNEL
#include <sys/ioctl.h>
#endif
#include "l2tp.h"

struct tunnel_list tunnels;
int max_tunnels = DEF_MAX_TUNNELS;
struct utsname uts;
int ppd = 1;		/* Packet processing delay */
int control_fd;		/* descriptor of control area */
char *args;

void init_tunnel_list(struct tunnel_list *t) {
	t->head = NULL;
	t->count=0;
	t->calls=0;
}

void show_status(int fd) 
{
	struct schedule_entry *se;
	struct tunnel *t;
	struct call *c;
	struct lns *tlns;
	struct lac *tlac;
	struct host *h;
	int s=0;
	int fd2=dup(fd);
	FILE *f=fdopen(fd2,"a");
	if (!f) {
		log(LOG_WARN, "show_status: fdopen() failed on fd %d\n",fd);
		return;
	}
	fprintf(f,"====== l2tpd statistics ========\n");
	fprintf(f," Scheduler entries:\n");
	se = events;
	while(se) {
		s++;
		t=(struct tunnel *)se->data;
		tlac=(struct lac *)se->data;
		c=(struct call *)se->data;
		if (se->func == &hello) {
			fprintf(f, "%d: HELLO to %d\n",s,t->tid);
		} else if (se->func == &magic_lac_dial) {
			fprintf(f, "%d: Magic dial on %s\n",s,tlac->entname);
		} else if (se->func == &send_zlb) {
			fprintf(f, "%d: Send payload ZLB on call %d:%d\n",s,c->container->tid, c->cid);
		} else if (se->func == &dethrottle) {
			fprintf(f, "%d: Dethrottle call %d:%d\n",s,c->container->tid, c->cid);
		} else 
			fprintf(f, "%d: Unknown event\n",s);
		se=se->next;
	};
	fprintf(f,"Total Events scheduled: %d\n",s);
	fprintf(f,"Number of tunnels open: %d\n",tunnels.count);
	fprintf(f,"Highest file descriptor: %d\n",fd2);
	t=tunnels.head;
	while(t) {
		fprintf(f,"Tunnel %s, ID = %d (local), %d (remote) to %s:%d\n" 
			       "   cSs = %d, cSr = %d, cLr = %d\n",
			(t->lac ? t->lac->entname : (t->lns ? t->lns->entname : "")),
			t->ourtid, t->tid, IPADDY(t->peer.sin_addr), ntohs(t->peer.sin_port),
			t->cSs, t->cSr, t->cLr);
		c=t->call_head;
		while(c) {
			fprintf(f,"    Call %s, ID = %d (local), %d (remote), serno = %d\n"
			          "          pSs = %d, pSr = %d, pLr = %d, tx = %d bytes (%d), rx= %d bytes (%d)\n",
				(c->lac ? c->lac->entname : (c->lns ? c->lns->entname : "")),
				c->ourcid, c->cid, c->serno, c->pSs, c->pSr, c->pLr,
				c->tx_bytes, c->tx_pkts, c->rx_bytes, c->rx_pkts);
				c=c->next;
		}
		t=t->next;
	}
	fprintf(f,"==========Config File===========\n");
	tlns=lnslist;
	while(tlns) {
		fprintf(f, "LNS entry %s\n",tlns->entname[0] ? tlns->entname : "(unnamed)");
		tlns=tlns->next;
	};
	tlac=laclist;
	while(tlac) {
		fprintf(f, "LAC entry %s, LNS is/are:",tlac->entname[0] ? tlac->entname : "(unnamed)");
		h=tlac->lns;
		if (h) {
			while(h) {
				fprintf(f," %s", h->hostname);
				h=h->next;
			}
		} else fprintf(f, " [none]");
		fprintf(f,"\n");
		tlac=tlac->next;
	};
	fprintf(f,"================================\n");
	fclose(f);
	close(fd2);
}

void status_handler(int sig)
{
	show_status(0);
}

void child_handler(int signal)
{
	/*
	 * Oops, somebody we launched was killed.
	 * It's time to reap them and close that call.
	 * But first, we have to find out what PID died.
	 * unfortunately, pppd will 
	 */
	struct tunnel *t;
	struct call *c;
	pid_t pid;
	int status;
	t=tunnels.head;
	pid = waitpid(-1,&status,WNOHANG);
	if (pid<1) {
		/*
		* Oh well, nobody there.  Maybe we reaped it
		* somewhere else already
		*/
		return;
	}
	while (t) {
		c = t->call_head;
		while(c){
			if (c->pppd == pid) {
				log(LOG_DEBUG, "%s : pppd died for call %d\n", __FUNCTION__,c->cid);

				c->needclose = -1;
				return;
			}
			c=c->next;
		}
		t=t->next;
	}
}

void death_handler(int signal) 
{
	/*
	* If we get here, somebody terminated us with a kill or a control-c.
	* we call call_close on each tunnel twice to get a StopCCN out
	* for each one (we can't pause to make sure it's received.
	* Then we close the connections
	*/
	struct tunnel *st, *st2;
	int sec;
	log(LOG_CRIT, "%s: Fatal signal %d received\n",__FUNCTION__,signal);
	st=tunnels.head;
	while (st) {
		st2=st->next;
		strcpy(st->self->errormsg, "Server closing");
		sec = st->self->closing;
		if (st->lac)
			st->lac->redial=0;
		call_close(st->self);
		if (!sec) {
			st->self->closing =-1;
			call_close(st->self);
		}
		st=st2;
	}
	exit(1);
}

int start_pppd(struct call *c, struct ppp_opts *opts)
{
	char a,b;
	char tty[80];
	char *stropt[80];
	struct ppp_opts *p;
#ifdef USE_KERNEL
	struct l2tp_call_opts co;
#endif
	int pos=1;
	int fd2;
#ifdef DEBUG_PPPD
	int x;
#endif
	char *str;
	p=opts;
	stropt[0]=strdup(PPPD);
	while(p) {
		stropt[pos]=(char *)malloc(strlen(p->option)+1);
		strncpy(stropt[pos],p->option,strlen(p->option)+1);
		pos++;
		p=p->next;
	}
	stropt[pos]=NULL;
	if (c->pppd>0) {
		log(LOG_WARN, "%s: PPP already started on call!\n",__FUNCTION__);
		return -EINVAL;
	}
	if (c->fd>-1) {
		log(LOG_WARN, "%s: file descriptor already assigned!\n",__FUNCTION__);
		return -EINVAL;
	}
#ifdef USE_KERNEL
        if (kernel_support) {
                co.ourtid = c->container->ourtid;
                co.ourcid = c->ourcid;
                ioctl(server_socket, L2TPIOCGETCALLOPTS, &co);
		stropt[pos++]=strdup("channel");
		stropt[pos]=(char *)malloc(10);
		snprintf(stropt[pos],10,"%d",co.id);
		pos++;
		stropt[pos]=NULL;
        } else {
#endif
 		if ((c->fd = getPtyMaster(&a, &b))<0) {
			log(LOG_WARN, "%s: unable to allocate pty, abandoning!\n",__FUNCTION__);
			return -EINVAL;
		}
		snprintf(tty,sizeof(tty),"/dev/tty%c%c",a,b);
		fd2=open(tty,O_RDWR);
#ifdef USE_KERNEL
	}
#endif
	str=stropt[0];
#ifdef DEBUG_PPPD
	log(LOG_DEBUG, "%s: I'm running:  ",__FUNCTION__);
	for(x=0;stropt[x];x++) {
		log(LOG_DEBUG, "\"%s\" ",stropt[x]);
	};
	log(LOG_DEBUG, "\n");
#endif
	c->pppd = fork();
	if (c->pppd<0) {
		log(LOG_WARN, "%s: unable to fork(), abandoning!\n",__FUNCTION__);
		return -EINVAL;
	} else if (!c->pppd) {
		close(0);
		close(1);
		close(2);
#ifdef USE_KERNEL
		if (!kernel_support && (fd2<0)) {
#else
		if (fd2 < 0) {
#endif
			log(LOG_WARN,"%s: Unable to open %s to launch pppd!\n",__FUNCTION__,tty);
			exit(1);
		}
		dup2(fd2,0);
		dup2(fd2,1);
		execv(PPPD,stropt);
		log(LOG_WARN, "%s: Exec of %s failed!\n",PPPD);
		exit(1);
	};
	close(fd2);
	pos=0;
	while(stropt[pos]) { free(stropt[pos]) ; pos++; };
	return 0;
}

void destroy_tunnel(struct tunnel *t)
{
	/*
	 * Immediately destroy a tunnel (and all its calls)
	 * and free its resources.  This may be called
	 * by the tunnel itself,so it needs to be
	 * "suicide safe"
	 */
	 
	struct call *c, *me;
	struct tunnel *p;
	struct timeval tv;
	if (!t) return;
	
	/*
	 * Save ourselves until the very
	 * end, since we might be calling this ourselves.
	 * We must divorce ourself from the tunnel
	 * structure, however, to avoid recursion
	 * because of the logic of the destroy_call
	 */
	me = t->self; 
	
	/*
	 * Destroy all the member calls
	 */
	c=t->call_head;
	while (c) {
		destroy_call(c);
		c=c->next;
	};
	/*
	 * Remove ourselves from the list of tunnels
	 */
	
	if (tunnels.head == t) {
		tunnels.head = t->next;
		tunnels.count--;
	} else {
		p=tunnels.head;
		if (p) {
			while (p->next && (p->next !=t)) p=p->next;
			if (p->next) {
				p->next = t->next;
				tunnels.count--;
			} else {
				log(LOG_WARN,
				"%s: unable to locate tunnel in tunnel list\n",__FUNCTION__);
			}
		} else {
			log(LOG_WARN, "%s: tunnel list is empty!\n",__FUNCTION__);
		}
	}
	if (t->lac) {
		t->lac->t=NULL;
		if (t->lac->redial && (t->lac->rtimeout > 0) && !t->lac->rsched &&
			t->lac->active) {
			log(LOG_LOG, "%s: Will redial in %d seconds\n",__FUNCTION__, t->lac->rtimeout);
			tv.tv_sec = t->lac->rtimeout;
			tv.tv_usec = 0;
			t->lac->rsched=schedule(tv, magic_lac_dial, t->lac);
		}
	}
	if (t->lns)
		t->lns->t=NULL;
	free(t);
	free(me);
}

struct tunnel *l2tp_call(char *host, int port, struct lac *lac, struct lns *lns) 
{
	/*
	 * Establish a tunnel from us to host
	 * on port port
	 */
	struct call *tmp=NULL;
	struct hostent *hp;
	unsigned int addr;
	port = htons(port);
	hp = gethostbyname(host);
	if (!hp) {
		log(LOG_WARN, "%s: gethostbyname() failed for %s.\n",__FUNCTION__,host);
		return NULL;
	}
	bcopy(hp->h_addr, &addr, hp->h_length);
	/* Force creation of a new tunnel
	   and set it's tid to 0 to cause
	   negotiation to occur */
	tmp = get_call(0,0,addr, port);
	tmp->container->tid = 0;
	tmp->container->lac = lac;
	tmp->container->lns = lns;
	tmp->lac=lac;
	tmp->lns=lns;
	if (lac)
		lac->t = tmp->container;
	if (lns)
		lns->t = tmp->container;
	if (!tmp) {
		log(LOG_WARN, "%s: Unable to create tunnel to %s.\n",__FUNCTION__,host);
		return NULL;
	}
	/*
	 * Since our state is 0, we will establish a tunnel now
	 */
	log(LOG_LOG, "%s:Connecting to host %s, port %d\n",__FUNCTION__,host,ntohs(port));
	control_finish(tmp->container, tmp);
	return tmp->container;
}

void magic_lac_tunnel(void *data)
{
	struct lac *lac;
	lac = (struct lac *)data;
	if (!lac) {
		log(LOG_WARN, "%s: magic_lac_tunnel: called on NULL lac!\n",__FUNCTION__);
		return;
	}
	if (lac->lns) {
		/* FIXME: I should try different LNS's if I get failures */
		l2tp_call(lac->lns->hostname, lac->lns->port, lac, NULL);
		return;
	} else if (deflac && deflac->lns) {
		l2tp_call(deflac->lns->hostname, deflac->lns->port, lac, NULL);
		return;
	} else {
		log(LOG_WARN, "%s: Unable to find hostname to dial for '%s'\n",__FUNCTION__,
			lac->entname);
		return;
	}
}

struct call *lac_call(int tid, struct lac *lac, struct lns *lns)
{
	struct tunnel *t = tunnels.head;
	struct call *tmp;
	while(t) {
		if (t->ourtid == tid) {
			tmp = new_call(t);
			if (!tmp) {
				log(LOG_WARN, "%s: unable to create new call\n",__FUNCTION__);
				return NULL;
			}
			tmp->next = t->call_head;
			t->call_head = tmp;
			t->count++;
			tmp->cid = 0;
			tmp->lac=lac;
			tmp->lns=lns;
			if (lac)
				lac->c = tmp;
			log(LOG_LOG, "%s: Calling on tunnel %d\n",__FUNCTION__,tid);
			control_finish(t,tmp);
			return tmp;
		}
		t=t->next;
	};
	log(LOG_DEBUG, "%s: No such tunnel %d to generate call.\n",__FUNCTION__,tid);
	return NULL;
}

void magic_lac_dial(void *data)
{
	struct lac *lac;
	lac = (struct lac *)data;
	if (!lac->active) {
		log(LOG_DEBUG, "%s: LAC %s not active",__FUNCTION__,lac->entname);
		return;
	}
	lac->rsched=NULL;
	lac->rtries++;
	if (lac->rmax && (lac->rtries > lac->rmax)) {
		log(LOG_LOG, "%s: maximum retries exceeded.\n",__FUNCTION__);
		return;
	}
	if (!lac) {
		log(LOG_WARN, "%s : called on NULL lac!\n",__FUNCTION__);
		return;
	}
	if (!lac->t) {
#ifdef DEGUG_MAGIC
		log(LOG_DEBUG, "%s : tunnel not up!  Connecting!\n",__FUNCTION__);
#endif
		magic_lac_tunnel(lac);
		return;
	}
	lac_call(lac->t->ourtid, lac,NULL);
}

void lac_hangup(int cid)
{
	struct tunnel *t = tunnels.head;
	struct call *tmp;
	while(t) {
		tmp = t->call_head;
		while (tmp) {
			if (tmp->ourcid == cid) {
				log(LOG_LOG, "%s :Hanging up call %d, Local: %d, Remote: %d\n",__FUNCTION__,tmp->serno, tmp->ourcid, tmp->cid);
				strcpy(tmp->errormsg,"Goodbye!");
				tmp->needclose = -1;
				return;
			}
			tmp=tmp->next;
		}
		t=t->next;
	};
	log(LOG_DEBUG, "%s : No such call %d to hang up.\n",__FUNCTION__,cid);
	return;
}

void lac_disconnect(int tid)
{
	struct tunnel *t = tunnels.head;
	while(t) {
		if (t->ourtid == tid) {
			log(LOG_LOG, "%s: Disconnecting from %s, Local: %d, Remote: %d\n",__FUNCTION__,IPADDY(t->peer.sin_addr),t->ourtid, t->tid);
			t->self->needclose= -1;
			strcpy(t->self->errormsg, "Goodbye!");
			call_close(t->self);
			return;
		}
		t=t->next;
	};
	log(LOG_DEBUG, "%s: No such tunnel %d to hang up.\n",__FUNCTION__,tid);
	return;
}

struct tunnel *new_tunnel() {
	struct tunnel *tmp = malloc(sizeof(struct tunnel));
	if (!tmp) return NULL;
	tmp->cSs=0;
	tmp->cSr=0;
	tmp->cLr=0;
	tmp->call_head = NULL;
	tmp->next = NULL;
	tmp->debug = -1;
	tmp->tid = -1;
	tmp->hello = NULL;
#ifndef TESTING
/*	while(get_call((tmp->ourtid = rand() & 0xFFFF),0,0,0)); */
#ifdef USE_KERNEL
	if (kernel_support) 
		tmp->ourtid = ioctl(server_socket, L2TPIOCADDTUNNEL, 0);
	else
#endif
		tmp->ourtid = rand() & 0xFFFF;
#else
	tmp->ourtid = 0x6227;
#endif
	tmp->nego = 0;
	tmp->count = 0;
	tmp->state = 0;		/* Nothing */
	tmp->peer.sin_family = AF_INET;
	tmp->peer.sin_port = 0;
	bzero(&(tmp->peer.sin_addr), sizeof(tmp->peer.sin_addr));
	tmp->sanity = -1;
	tmp->qtid = -1;
	tmp->ourfc = ASYNC_FRAMING | SYNC_FRAMING;
	tmp->ourbc = 0;
	tmp->ourtb = (((_u64)rand()) <<32) | ((_u64)rand());
	tmp->fc = -1;	/* These really need to be specified by the peer */
	tmp->bc = -1;	/* And we want to know if they forgot */
	tmp->hostname[0]=0;
	tmp->vendor[0]=0;
	tmp->secret[0]=0;
	if (!(tmp->self = new_call(tmp))) {
		free(tmp);
		return NULL;
	};
	tmp->self->ourrws = DEFAULT_RWS_SIZE;
	tmp->self->ourfbit = FBIT;
	tmp->lac=NULL;
	tmp->lns=NULL;
	tmp->chal_us.state=0;
	tmp->chal_us.secret[0]=0;
	memset(tmp->chal_us.reply,0,MD_SIG_SIZE);
	tmp->chal_them.state=0;
	tmp->chal_them.secret[0]=0;
	memset(tmp->chal_them.reply,0,MD_SIG_SIZE);
	tmp->chal_them.vector=(unsigned char *)malloc(VECTOR_SIZE);
	tmp->chal_us.vector=NULL;
	tmp->hbit = 0;
	return tmp;
}

void do_control() {
	char buf[1024];
	char *host;
	char *tunstr;
	char *callstr;
	struct lac *lac;
	int call;
	int tunl;
	int cnt=-1;
	while (cnt) {
		cnt=read(control_fd, buf, sizeof(buf));
		if (cnt>0) {
			if (buf[cnt-1]=='\n') buf[--cnt]=0;
#ifdef DEBUG_CONTROL
			log(LOG_DEBUG, "%s: Got message %s (%d bytes long)\n",__FUNCTION__,buf,cnt);
#endif
			switch(buf[0]) {
			case 't':
				host=strchr(buf,' ') + 1;
#ifdef DEBUG_CONTROL
				log(LOG_DEBUG, "%s: Attempting to tunnel to %s\n",__FUNCTION__,host);
#endif
				l2tp_call(host,UDP_LISTEN_PORT,NULL,NULL);
				break;
			case 'c':
				tunstr=strchr(buf,' ') +1;
				lac=laclist;
				while(lac) {
					if (!strcasecmp(lac->entname, tunstr)) {
						lac->active=-1;
						lac->rtries=0;
						if (!lac->c)
							magic_lac_dial(lac);
						else
							log(LOG_DEBUG, "%s: Session '%s' already active!\n",__FUNCTION__,lac->entname);
						break;
					}
					lac=lac->next;
				}
				if (lac) break;
				tunl = atoi(tunstr);
				if (!tunl) {
					log(LOG_DEBUG, "%s: No such tunnel '%s'\n",__FUNCTION__,tunstr);
					break;
				}
#ifdef DEBUG_CONTROL
				log(LOG_DEBUG, "%s: Attempting to call on tunnel %d\n",__FUNCTION__,tunl);
#endif
				lac_call(tunl,NULL,NULL);
				break;
			case 'h':
				callstr=strchr(buf,' ') +1;
				call=atoi(callstr);
#ifdef DEBUG_CONTROL
				log(LOG_DEBUG, "%s: Attempting to call %d\n",__FUNCTION__,call);
#endif
				lac_hangup(call);
				break;
			case 'd':
				tunstr=strchr(buf,' ') +1;
				lac=laclist;
				while(lac) {
					if (!strcasecmp(lac->entname, tunstr)) {
						lac->active=0;
						lac->rtries=0;
						if (lac->t)
							lac_disconnect(lac->t->ourtid);
						else
							log(LOG_DEBUG, "%s: Session '%s' not up\n",__FUNCTION__ ,lac->entname);
						break;
					}
					lac=lac->next;
				}
				if (lac) break;
				tunl = atoi(tunstr);
				if (!tunl) {
					log(LOG_DEBUG, "%s: No such tunnel '%s'\n", __FUNCTION__, tunstr);
					break;
				}
#ifdef DEBUG_CONTROL
				log(LOG_DEBUG, "%s: Attempting to disconnect tunnel %d\n", __FUNCTION__,tunl);
#endif
				lac_disconnect(tunl);
				break;
			default:
				log(LOG_DEBUG, "%s: Unknown command %c\n", __FUNCTION__,buf[0]);
			}
		}
	}
	/* Otherwise select goes nuts */
	close(control_fd);
	control_fd = open(CONTROL_PIPE, O_RDONLY | O_NONBLOCK, 0600);
}

void init() {
	struct lac *lac;
	init_addr();
	if (init_config()) {
		log(LOG_CRIT, "%s: Unable to load config file\n", __FUNCTION__);
		exit(1);
	}
	if (uname(&uts)) {
		log(LOG_CRIT, "%s : Unable to determine host system\n", __FUNCTION__);
		exit(1);
	}
	init_tunnel_list(&tunnels);
	if (init_network()) exit(1);
	signal(SIGTERM, &death_handler);
	signal(SIGINT, &death_handler);
	signal(SIGCHLD, &child_handler);
	signal(SIGUSR1, &status_handler);
	init_scheduler();
	mkfifo(CONTROL_PIPE, 0600);
	control_fd = open(CONTROL_PIPE, O_RDONLY | O_NONBLOCK, 0600);
	if (control_fd<0) {
		log(LOG_CRIT, "%s: Unable to open " CONTROL_PIPE " for reading.", __FUNCTION__);
		exit(1);
	}
	log(LOG_LOG, "l2tpd version " SERVER_VERSION " started on %s\n",hostname);
	log(LOG_LOG, "Written by Mark Spencer, Copyright (C) 1998, Adtran, Inc.\n");
	log(LOG_LOG, "%s version %s on a %s, port %d\n",uts.sysname,uts.release,uts.machine,gconfig.port);
	lac = laclist;
	while(lac) {
		if (lac->autodial) {
#ifdef DEBUG_MAGIC		
			log(LOG_DEBUG, "%s: Autodialing '%s'\n",__FUNCTION__, lac->entname[0] ? lac->entname : "(unnamed)");
#endif
			lac->active=-1;
			magic_lac_dial(lac);
		}
		lac=lac->next;
	}
}

int main(int argc, char *argv[]) {
	args=argv[0];
	init();
	network_thread();
	return 0;
}
