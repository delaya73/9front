#include <u.h>
#include <libc.h>
#include <auth.h>
#include <fcall.h>
#include <ip.h>
#include "dns.h"

enum
{
	Maxrequest=		1024,
	Maxreply=		8192,		/* was 512 */
	Maxrrr=			32,		/* was 16 */
	Maxfdata=		8192,

	Qdir=			0,
	Qdns=			1,
};

typedef struct Mfile	Mfile;
typedef struct Job	Job;
typedef struct Network	Network;

int vers;		/* incremented each clone/attach */

/* holds data to be returned via read of /net/dns, perhaps multiple reads */
struct Mfile
{
	Mfile		*next;		/* next free mfile */

	char		*user;
	Qid		qid;
	int		fid;

	int		type;		/* reply type */
	char		reply[Maxreply];
	ushort		rr[Maxrrr];	/* offset of rr's */
	ushort		nrr;		/* number of rr's */
};

/*
 *  active local requests
 */
struct Job
{
	Job	*next;
	int	flushed;
	Fcall	request;
	Fcall	reply;
};
Lock	joblock;
Job	*joblist;

struct {
	Lock;
	Mfile	*inuse;		/* active mfile's */
} mfalloc;

Cfg	cfg;
int	debug;
int	mfd[2];
int	sendnotifies;

char	*logfile = "dns";	/* or "dns.test" */
char	*dbfile;
char	*dnsuser;
char	mntpt[Maxpath];

int	fillreply(Mfile*, int);
void	freejob(Job*);
void	io(void);
void	mountinit(char*, char*);
Job*	newjob(void);
void	rattach(Job*, Mfile*);
void	rauth(Job*);
void	rclunk(Job*, Mfile*);
void	rcreate(Job*, Mfile*);
void	rflush(Job*);
void	ropen(Job*, Mfile*);
void	rread(Job*, Mfile*);
void	rremove(Job*, Mfile*);
void	rstat(Job*, Mfile*);
void	rversion(Job*);
char*	rwalk(Job*, Mfile*);
void	rwrite(Job*, Mfile*, Request*);
void	rwstat(Job*, Mfile*);
void	sendmsg(Job*, char*);
void	setext(char*, int, char*);

static char *lookupquery(Job*, Mfile*, Request*, char*, char*, int, int);
static char *respond(Job*, Mfile*, RR*, char*, int, int);

void
usage(void)
{
	fprint(2, "usage: %s [-FnrLR] [-a maxage] [-c cert.pem] [-f ndb-file] [-N target] "
		"[-x netmtpt] [-s [addrs...]]\n", argv0);
	exits("usage");
}

void
main(int argc, char *argv[])
{
	char ext[Maxpath], servefile[Maxpath];
	char *cert;
	Dir *dir;

	setnetmtpt(mntpt, sizeof mntpt, nil);
	ext[0] = 0;
	cert = nil;
	ARGBEGIN{
	case 'a':
		maxage = atol(EARGF(usage()));
		break;
	case 'd':
		debug = 1;
		break;
	case 'f':
		dbfile = EARGF(usage());
		break;
	case 'F':
		cfg.justforw = cfg.resolver = 1;
		break;
	case 'n':
		sendnotifies = 1;
		break;
	case 'N':
		target = atol(EARGF(usage()));
		if (target < 1000)
			target = 1000;
		break;
	case 'r':
		cfg.resolver = 1;
		break;
	case 'L':
		cfg.localrecursive = 1;
		cfg.nonrecursive = 0;
		break;
	case 'R':
		cfg.nonrecursive = 1;
		cfg.localrecursive = 0;
		break;
	case 's':
		cfg.serve = 1;		/* serve network */
		cfg.cachedb = 1;
		break;
	case 'c':
		cert = EARGF(usage());
		break;
	case 'x':
		setnetmtpt(mntpt, sizeof mntpt, EARGF(usage()));
		setext(ext, sizeof ext, mntpt);
		break;
	default:
		usage();
		break;
	}ARGEND

	if(argc != 0 && !cfg.serve)
		usage();

	/* start syslog before we fork */
	fmtinstall('F', fcallfmt);
	dninit();
	dnslog("starting %s%s%s%sdns %s%son %s",
		(cfg.cachedb? "caching ": ""),
		(cfg.nonrecursive? "non-recursive ": ""),
		(cfg.localrecursive? "local-recursive ": ""),
		(cfg.serve?   "server ": ""),
		(cfg.justforw? "forwarding-only ": ""),
		(cfg.resolver? "resolver ": ""), mntpt);

	opendatabase();
	dnsuser = estrdup(getuser());

	snprint(servefile, sizeof servefile, "/srv/dns%s", ext);
	dir = dirstat(servefile);
	if (dir)
		sysfatal("%s exists; another dns instance is running",
			servefile);
	free(dir);
	mountinit(servefile, mntpt);	/* forks, parent exits */

	srand(truerand());
	db2cache(1);

	if(cfg.serve){
		if(argc == 0) {
			dnudpserver(mntpt, "*");
			dntcpserver(mntpt, "*", nil);
			if(cert != nil)
				dntcpserver(mntpt, "*", cert);
		} else {
			while(argc-- > 0){
				dnudpserver(mntpt, *argv);
				dntcpserver(mntpt, *argv, nil);
				if(cert != nil)
					dntcpserver(mntpt, *argv, cert);
				argv++;
			}
		}
	}
	if(sendnotifies)
		notifyproc(mntpt);

	io();
	_exits(0);
}

/*
 *  if a mount point is specified, set the cs extension to be the mount point
 *  with '_'s replacing '/'s
 */
void
setext(char *ext, int n, char *p)
{
	int i, c;

	n--;
	for(i = 0; i < n; i++){
		c = p[i];
		if(c == 0)
			break;
		if(c == '/')
			c = '_';
		ext[i] = c;
	}
	ext[i] = 0;
}

void
mountinit(char *service, char *mntpt)
{
	int f;
	int p[2];
	char buf[32];

	if(pipe(p) < 0)
		sysfatal("pipe failed: %r");

	/*
	 *  make a /srv/dns
	 */
	if((f = create(service, OWRITE|ORCLOSE, 0666)) < 0)
		sysfatal("create %s failed: %r", service);
	snprint(buf, sizeof buf, "%d", p[1]);
	if(write(f, buf, strlen(buf)) != strlen(buf))
		sysfatal("write %s failed: %r", service);

	/* copy namespace to avoid a deadlock */
	switch(rfork(RFFDG|RFPROC|RFNAMEG|RFREND|RFNOTEG)){
	case 0:			/* child: start main proc */
		close(p[1]);
		procsetname("%s", mntpt);
		break;
	case -1:
		sysfatal("fork failed: %r");
	default:		/* parent: make /srv/dns, mount it, exit */
		close(p[0]);

		/*
		 *  put ourselves into the file system
		 */
		if(mount(p[1], -1, mntpt, MAFTER, "") == -1)
			fprint(2, "dns mount failed: %r\n");
		_exits(0);
	}
	mfd[0] = mfd[1] = p[0];
}

Mfile*
newfid(int fid, int needunused)
{
	Mfile *mf;

	lock(&mfalloc);
	for(mf = mfalloc.inuse; mf != nil; mf = mf->next)
		if(mf->fid == fid){
			unlock(&mfalloc);
			if(needunused)
				return nil;
			return mf;
		}
	mf = emalloc(sizeof(*mf));
	mf->fid = fid;
	mf->qid.vers = vers;
	mf->qid.type = QTDIR;
	mf->qid.path = 0LL;
	mf->user = estrdup("none");
	mf->next = mfalloc.inuse;
	mfalloc.inuse = mf;
	unlock(&mfalloc);
	return mf;
}

void
freefid(Mfile *mf)
{
	Mfile **l;

	lock(&mfalloc);
	for(l = &mfalloc.inuse; *l != nil; l = &(*l)->next)
		if(*l == mf){
			*l = mf->next;
			free(mf->user);
			memset(mf, 0, sizeof *mf);	/* cause trouble */
			free(mf);
			unlock(&mfalloc);
			return;
		}
	unlock(&mfalloc);
	sysfatal("freeing unused fid");
}

Mfile*
copyfid(Mfile *mf, int fid)
{
	Mfile *nmf;

	nmf = newfid(fid, 1);
	if(nmf == nil)
		return nil;
	nmf->fid = fid;
	free(nmf->user);			/* estrdup("none") */
	nmf->user = estrdup(mf->user);
	nmf->qid.type = mf->qid.type;
	nmf->qid.path = mf->qid.path;
	nmf->qid.vers = vers++;
	return nmf;
}

Job*
newjob(void)
{
	Job *job;

	job = emalloc(sizeof *job);
	lock(&joblock);
	job->next = joblist;
	joblist = job;
	job->request.tag = -1;
	unlock(&joblock);
	return job;
}

void
freejob(Job *job)
{
	Job **l;

	lock(&joblock);
	for(l = &joblist; *l; l = &(*l)->next)
		if(*l == job){
			*l = job->next;
			memset(job, 0, sizeof *job);	/* cause trouble */
			free(job);
			break;
		}
	unlock(&joblock);
}

void
flushjob(int tag)
{
	Job *job;

	lock(&joblock);
	for(job = joblist; job; job = job->next)
		if(job->request.tag == tag && job->request.type != Tflush){
			job->flushed = 1;
			break;
		}
	unlock(&joblock);
}

void
io(void)
{
	volatile long n;
	volatile uchar mdata[IOHDRSZ + Maxfdata];
	Job *volatile job;
	Mfile *volatile mf;
	volatile Request req;

	/*
	 *  a slave process is sometimes forked to wait for replies from other
	 *  servers.  The master process returns immediately via a longjmp
	 *  through 'mret'.
	 */
	memset(&req, 0, sizeof req);
	setjmp(req.mret);
	req.isslave = 0;

	while((n = read9pmsg(mfd[0], mdata, sizeof mdata)) != 0){
		if(n < 0){
			dnslog("error reading 9P from %s: %r", mntpt);
			break;
		}

		stats.qrecvd9prpc++;
		job = newjob();
		if(convM2S(mdata, n, &job->request) != n){
			dnslog("format error %ux %ux %ux %ux %ux",
				mdata[0], mdata[1], mdata[2], mdata[3], mdata[4]);
			freejob(job);
			break;
		}
		mf = newfid(job->request.fid, 0);
		if(debug)
			dnslog("%F", &job->request);

		switch(job->request.type){
		default:
			warning("unknown request type %d", job->request.type);
			break;
		case Tversion:
			rversion(job);
			break;
		case Tauth:
			rauth(job);
			break;
		case Tflush:
			rflush(job);
			break;
		case Tattach:
			rattach(job, mf);
			break;
		case Twalk:
			rwalk(job, mf);
			break;
		case Topen:
			ropen(job, mf);
			break;
		case Tcreate:
			rcreate(job, mf);
			break;
		case Tread:
			rread(job, mf);
			break;
		case Twrite:
			getactivity(&req);
			req.aborttime = timems() + Maxreqtm;
			req.from = "9p";
			rwrite(job, mf, &req);
			freejob(job);
			if(req.isslave){
				putactivity(&req);
				_exits(0);
			}
			putactivity(&req);
			continue;
		case Tclunk:
			rclunk(job, mf);
			break;
		case Tremove:
			rremove(job, mf);
			break;
		case Tstat:
			rstat(job, mf);
			break;
		case Twstat:
			rwstat(job, mf);
			break;
		}
		freejob(job);
	}
}

void
rversion(Job *job)
{
	if(job->request.msize > IOHDRSZ + Maxfdata)
		job->reply.msize = IOHDRSZ + Maxfdata;
	else
		job->reply.msize = job->request.msize;
	job->reply.version = "9P2000";
	if(strncmp(job->request.version, "9P", 2) != 0)
		job->reply.version = "unknown";
	sendmsg(job, nil);
}

void
rauth(Job *job)
{
	sendmsg(job, "dns: authentication not required");
}

/*
 *  don't flush till all the slaves are done
 */
void
rflush(Job *job)
{
	flushjob(job->request.oldtag);
	sendmsg(job, 0);
}

void
rattach(Job *job, Mfile *mf)
{
	if(mf->user != nil)
		free(mf->user);
	mf->user = estrdup(job->request.uname);
	mf->qid.vers = vers++;
	mf->qid.type = QTDIR;
	mf->qid.path = 0LL;
	job->reply.qid = mf->qid;
	sendmsg(job, 0);
}

char*
rwalk(Job *job, Mfile *mf)
{
	int i, nelems;
	char *err;
	char **elems;
	Mfile *nmf;
	Qid qid;

	err = 0;
	nmf = nil;
	elems  = job->request.wname;
	nelems = job->request.nwname;
	job->reply.nwqid = 0;

	if(job->request.newfid != job->request.fid){
		/* clone fid */
		nmf = copyfid(mf, job->request.newfid);
		if(nmf == nil){
			err = "clone bad newfid";
			goto send;
		}
		mf = nmf;
	}
	/* else nmf will be nil */

	qid = mf->qid;
	if(nelems > 0)
		/* walk fid */
		for(i=0; i<nelems && i<MAXWELEM; i++){
			if((qid.type & QTDIR) == 0){
				err = "not a directory";
				break;
			}
			if (strcmp(elems[i], "..") == 0 ||
			    strcmp(elems[i], ".") == 0){
				qid.type = QTDIR;
				qid.path = Qdir;
Found:
				job->reply.wqid[i] = qid;
				job->reply.nwqid++;
				continue;
			}
			if(strcmp(elems[i], "dns") == 0){
				qid.type = QTFILE;
				qid.path = Qdns;
				goto Found;
			}
			err = "file does not exist";
			break;
		}

send:
	if(nmf != nil && (err!=nil || job->reply.nwqid<nelems))
		freefid(nmf);
	if(err == nil)
		mf->qid = qid;
	sendmsg(job, err);
	return err;
}

void
ropen(Job *job, Mfile *mf)
{
	int mode;
	char *err;

	err = 0;
	mode = job->request.mode;
	if(mf->qid.type & QTDIR)
		if(mode)
			err = "permission denied";
	job->reply.qid = mf->qid;
	job->reply.iounit = 0;
	sendmsg(job, err);
}

void
rcreate(Job *job, Mfile *mf)
{
	USED(mf);
	sendmsg(job, "creation permission denied");
}

void
rread(Job *job, Mfile *mf)
{
	int i, n;
	long clock;
	ulong cnt;
	vlong off;
	char *err;
	uchar buf[Maxfdata];
	Dir dir;

	n = 0;
	err = nil;
	off = job->request.offset;
	cnt = job->request.count;
	*buf = '\0';
	job->reply.data = (char*)buf;
	if(mf->qid.type & QTDIR){
		clock = time(nil);
		if(off == 0){
			memset(&dir, 0, sizeof dir);
			dir.name = "dns";
			dir.qid.type = QTFILE;
			dir.qid.vers = vers;
			dir.qid.path = Qdns;
			dir.mode = 0666;
			dir.length = 0;
			dir.uid = dir.gid = dir.muid = mf->user;
			dir.atime = dir.mtime = clock;		/* wrong */
			n = convD2M(&dir, buf, sizeof buf);
		}
	} else if (off < 0)
		err = "negative read offset";
	else {
		/* first offset will always be zero */
		for(i = 1; i <= mf->nrr; i++)
			if(mf->rr[i] > off)
				break;
		if(i <= mf->nrr) {
			if(off + cnt > mf->rr[i])
				n = mf->rr[i] - off;
			else
				n = cnt;
			assert(n >= 0);
			job->reply.data = mf->reply + off;
		}
	}
	job->reply.count = n;
	sendmsg(job, err);
}

void
rwrite(Job *job, Mfile *mf, Request *req)
{
	int rooted, wantsav, send;
	ulong cnt;
	char *err, *p, *atype;
	char errbuf[ERRMAX];

	err = nil;
	cnt = job->request.count;
	send = 1;
	if(mf->qid.type & QTDIR)
		err = "can't write directory";
	else if (job->request.offset != 0)
		err = "writing at non-zero offset";
	else if(cnt >= Maxrequest)
		err = "request too long";
	else
		send = 0;
	if (send)
		goto send;

	job->request.data[cnt] = 0;
	if(cnt > 0 && job->request.data[cnt-1] == '\n')
		job->request.data[cnt-1] = 0;

	if(strcmp(mf->user, "none") == 0 || strcmp(mf->user, dnsuser) != 0)
		goto query;	/* skip special commands if not owner */

	/*
	 *  special commands
	 */
	if(debug)
		dnslog("%d: rwrite got: %s", req->id, job->request.data);
	send = 1;
	if(strcmp(job->request.data, "debug")==0)
		debug ^= 1;
	else if(strcmp(job->request.data, "refresh")==0)
		needrefresh = 1;
	else if(strncmp(job->request.data, "target ", 7)==0){
		target = atol(job->request.data + 7);
		dnslog("%d: target set to %ld", req->id, target);
	} else
		send = 0;
	if (send)
		goto send;

query:
	/*
	 *  kill previous reply
	 */
	mf->nrr = 0;
	mf->rr[0] = 0;

	/*
	 *  break up request (into a name and a type)
	 */
	atype = strchr(job->request.data, ' ');
	if(atype == 0){
		snprint(errbuf, sizeof errbuf, "illegal request %s",
			job->request.data);
		err = errbuf;
		goto send;
	} else
		*atype++ = 0;

	/* normal request: domain [type] */
	stats.qrecvd9p++;
	mf->type = rrtype(atype);
	if(mf->type < 0){
		snprint(errbuf, sizeof errbuf, "unknown type %s", atype);
		err = errbuf;
		goto send;
	}

	p = atype - 2;
	if(p >= job->request.data && *p == '.'){
		rooted = 1;
		*p = 0;
	} else
		rooted = 0;

	p = job->request.data;
	if(*p == '!'){
		wantsav = 1;
		p++;
	} else
		wantsav = 0;

	err = lookupquery(job, mf, req, errbuf, p, wantsav, rooted);
send:
	job->reply.count = cnt;
	sendmsg(job, err);
}

/*
 * dnsdebug calls
 *	rr = dnresolve(buf, Cin, type, &req, nil, 0, Recurse, rooted, nil);
 * which generates a UDP query, which eventually calls
 *	dnserver(&reqmsg, &repmsg, &req, buf, rcode);
 * which calls
 *	rp = dnresolve(name, Cin, type, req, &mp->an, 0, recurse, 1, nil);
 *
 * but here we just call dnresolve directly.
 */
static char *
lookupquery(Job *job, Mfile *mf, Request *req, char *errbuf, char *p,
	int wantsav, int rooted)
{
	int rcode;
	RR *rp, *neg;

	rcode = Rok;
	rp = dnresolve(p, Cin, mf->type, req, nil, 0, Recurse, rooted, &rcode);

	neg = rrremneg(&rp);
	if(neg){
		rcode = neg->negrcode;
		rrfreelist(neg);
	}

	return respond(job, mf, rp, errbuf, rcode, wantsav);
}

static char *
respond(Job *job, Mfile *mf, RR *rp, char *errbuf, int rcode, int wantsav)
{
	long n;
	RR *tp;

	if(rp == nil)
		switch(rcode){
		case Rname:
			return "name does not exist";
		case Rserver:
			return "dns failure";
		case Rok:
		default:
			snprint(errbuf, ERRMAX,
				"resource does not exist; negrcode %d (%s)",
					rcode, rcname(rcode));
			return errbuf;
		}

	lock(&joblock);
	if(!job->flushed){
		/* format data to be read later */
		n = 0;
		mf->nrr = 0;
		for(tp = rp; mf->nrr < Maxrrr-1 && n < Maxreply && tp &&
		    tsame(mf->type, tp->type); tp = tp->next){
			mf->rr[mf->nrr++] = n;
			if(wantsav)
				n += snprint(mf->reply+n, Maxreply-n, "%Q", tp);
			else
				n += snprint(mf->reply+n, Maxreply-n, "%R", tp);
		}
		mf->rr[mf->nrr] = n;
	}
	unlock(&joblock);

	rrfreelist(rp);

	return nil;
}

void
rclunk(Job *job, Mfile *mf)
{
	freefid(mf);
	sendmsg(job, 0);
}

void
rremove(Job *job, Mfile *mf)
{
	USED(mf);
	sendmsg(job, "remove permission denied");
}

void
rstat(Job *job, Mfile *mf)
{
	Dir dir;
	uchar buf[IOHDRSZ+Maxfdata];

	memset(&dir, 0, sizeof dir);
	if(mf->qid.type & QTDIR){
		dir.name = ".";
		dir.mode = DMDIR|0555;
	} else {
		dir.name = "dns";
		dir.mode = 0666;
	}
	dir.qid = mf->qid;
	dir.length = 0;
	dir.uid = dir.gid = dir.muid = mf->user;
	dir.atime = dir.mtime = time(nil);
	job->reply.nstat = convD2M(&dir, buf, sizeof buf);
	job->reply.stat = buf;
	sendmsg(job, 0);
}

void
rwstat(Job *job, Mfile *mf)
{
	USED(mf);
	sendmsg(job, "wstat permission denied");
}

void
sendmsg(Job *job, char *err)
{
	int n;
	uchar mdata[IOHDRSZ + Maxfdata];
	char ename[ERRMAX];

	if(err){
		job->reply.type = Rerror;
		snprint(ename, sizeof ename, "dns: %s", err);
		job->reply.ename = ename;
	}else
		job->reply.type = job->request.type+1;
	job->reply.tag = job->request.tag;
	n = convS2M(&job->reply, mdata, sizeof mdata);
	if(n == 0){
		warning("sendmsg convS2M of %F returns 0", &job->reply);
		abort();
	}
	lock(&joblock);
	if(job->flushed == 0)
		if(write(mfd[1], mdata, n)!=n)
			sysfatal("mount write");
	unlock(&joblock);
	if(debug)
		dnslog("%F %d", &job->reply, n);
}

/*
 *  the following varies between dnsdebug and dns
 */
void
logreply(int id, char *rcvd, uchar *addr, DNSmsg *mp)
{
	RR *rp;

	if(!debug)
		return;
	dnslog("%d: %s %I %s (%s%s%s%s%s)", id, rcvd, addr, rcname(getercode(mp)),
		mp->flags & Fauth? " auth": "",
		mp->flags & Ftrunc? " trunc": "",
		mp->flags & Frecurse? " rd": "",
		mp->flags & Fcanrec? " ra": "",
		(mp->flags & (Fauth|Rmask)) == (Fauth|Rname)? " nx": "");
	for(rp = mp->qd; rp != nil; rp = rp->next)
		dnslog("%d: %s %I qd %s", id, rcvd, addr, rp->owner->name);
	for(rp = mp->an; rp != nil; rp = rp->next)
		dnslog("%d: %s %I an %R", id, rcvd, addr, rp);
	for(rp = mp->ns; rp != nil; rp = rp->next)
		dnslog("%d: %s %I ns %R", id, rcvd, addr, rp);
	for(rp = mp->ar; rp != nil; rp = rp->next)
		dnslog("%d: %s %I ar %R", id, rcvd, addr, rp);
}

void
logrequest(int id, int depth, char *send, uchar *addr, char *sname, char *rname, int type)
{
	char tname[32];

	if(!debug)
		return;

	dnslog("%d.%d: %s %I/%s %s %s",
		id, depth, send, addr, sname, rname,
		rrname(type, tname, sizeof tname));
}

RR*
getdnsservers(int class)
{
	return dnsservers(class);
}
