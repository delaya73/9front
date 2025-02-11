#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "ureg.h"
#include "../port/error.h"

enum {
	Qdir = 0,
	Qiob,
	Qiow,
	Qiol,
	Qmsr,
	Qbase,

	Qmax = 32,
};

enum {				/* cpuid standard function codes */
	Highstdfunc = 0,	/* also returns vendor string */
	Procsig,
	Proctlbcache,
	Procserial,
	
	Highextfunc = 0x80000000,
	Procextfeat,
};

typedef long Rdwrfn(Chan*, void*, long, vlong);

static Rdwrfn *readfn[Qmax];
static Rdwrfn *writefn[Qmax];

static Dirtab archdir[Qmax] = {
	".",		{ Qdir, 0, QTDIR },	0,	0555,
	"iob",		{ Qiob, 0 },		0,	0660,
	"iow",		{ Qiow, 0 },		0,	0660,
	"iol",		{ Qiol, 0 },		0,	0660,
	"msr",		{ Qmsr, 0 },		0,	0660,
};
Lock archwlock;	/* the lock is only for changing archdir */
int narchdir = Qbase;
int (*_pcmspecial)(char*, ISAConf*);
void (*_pcmspecialclose)(int);

/*
 * Add a file to the #P listing.  Once added, you can't delete it.
 * You can't add a file with the same name as one already there,
 * and you get a pointer to the Dirtab entry so you can do things
 * like change the Qid version.  Changing the Qid path is disallowed.
 */
Dirtab*
addarchfile(char *name, int perm, Rdwrfn *rdfn, Rdwrfn *wrfn)
{
	int i;
	Dirtab d;
	Dirtab *dp;

	memset(&d, 0, sizeof d);
	strcpy(d.name, name);
	d.perm = perm;

	lock(&archwlock);
	if(narchdir >= Qmax){
		unlock(&archwlock);
		print("addarchfile: out of entries for %s\n", name);
		return nil;
	}

	for(i=0; i<narchdir; i++)
		if(strcmp(archdir[i].name, name) == 0){
			unlock(&archwlock);
			return nil;
		}

	d.qid.path = narchdir;
	archdir[narchdir] = d;
	readfn[narchdir] = rdfn;
	writefn[narchdir] = wrfn;
	dp = &archdir[narchdir++];
	unlock(&archwlock);

	return dp;
}

void
ioinit(void)
{
	char *excluded;

	iomapinit(0xffff);

	/*
	 * This is necessary to make the IBM X20 boot.
	 * Have not tracked down the reason.
	 * i82557 is at 0x1000, the dummy entry is needed for swappable devs.
	 */
	ioalloc(0x0fff, 1, 0, "dummy");

	if ((excluded = getconf("ioexclude")) != nil) {
		char *s;

		s = excluded;
		while (s && *s != '\0' && *s != '\n') {
			char *ends;
			int io_s, io_e;

			io_s = (int)strtol(s, &ends, 0);
			if (ends == nil || ends == s || *ends != '-') {
				print("ioinit: cannot parse option string\n");
				break;
			}
			s = ++ends;

			io_e = (int)strtol(s, &ends, 0);
			if (ends && *ends == ',')
				*ends++ = '\0';
			s = ends;

			ioalloc(io_s, io_e - io_s + 1, 0, "pre-allocated");
		}
	}
}

static void
checkport(ulong start, ulong end)
{
	if(end < start || end > 0x10000)
		error(Ebadarg);

	/* standard vga regs are OK */
	if(start >= 0x2b0 && end <= 0x2df+1)
		return;
	if(start >= 0x3c0 && end <= 0x3da+1)
		return;

	if(iounused(start, end))
		return;
	error(Eperm);
}

static Chan*
archattach(char* spec)
{
	return devattach('P', spec);
}

Walkqid*
archwalk(Chan* c, Chan *nc, char** name, int nname)
{
	return devwalk(c, nc, name, nname, archdir, narchdir, devgen);
}

static int
archstat(Chan* c, uchar* dp, int n)
{
	return devstat(c, dp, n, archdir, narchdir, devgen);
}

static Chan*
archopen(Chan* c, int omode)
{
	return devopen(c, omode, archdir, narchdir, devgen);
}

static void
archclose(Chan*)
{
}

static long
archread(Chan *c, void *a, long n, vlong offset)
{
	ulong port, end;
	uchar *cp;
	ushort *sp;
	ulong *lp;
	vlong *vp;
	Rdwrfn *fn;

	port = offset;
	end = port+n;
	switch((ulong)c->qid.path){
	case Qdir:
		return devdirread(c, a, n, archdir, narchdir, devgen);

	case Qiob:
		checkport(port, end);
		for(cp = a; port < end; port++)
			*cp++ = inb(port);
		return n;

	case Qiow:
		if(n & 1)
			error(Ebadarg);
		checkport(port, end);
		for(sp = a; port < end; port += 2)
			*sp++ = ins(port);
		return n;

	case Qiol:
		if(n & 3)
			error(Ebadarg);
		checkport(port, end);
		for(lp = a; port < end; port += 4)
			*lp++ = inl(port);
		return n;

	case Qmsr:
		if(n & 7)
			error(Ebadarg);
		if((ulong)n/8 > -port)
			error(Ebadarg);
		end = port+(n/8);
		for(vp = a; port != end; port++)
			if(rdmsr(port, vp++) < 0)
				error(Ebadarg);
		return n;

	default:
		if(c->qid.path < narchdir && (fn = readfn[c->qid.path]))
			return fn(c, a, n, offset);
		error(Eperm);
	}
}

static long
archwrite(Chan *c, void *a, long n, vlong offset)
{
	ulong port, end;
	uchar *cp;
	ushort *sp;
	ulong *lp;
	vlong *vp;
	Rdwrfn *fn;

	port = offset;
	end = port+n;
	switch((ulong)c->qid.path){
	case Qiob:
		checkport(port, end);
		for(cp = a; port < end; port++)
			outb(port, *cp++);
		return n;

	case Qiow:
		if(n & 1)
			error(Ebadarg);
		checkport(port, end);
		for(sp = a; port < end; port += 2)
			outs(port, *sp++);
		return n;

	case Qiol:
		if(n & 3)
			error(Ebadarg);
		checkport(port, end);
		for(lp = a; port < end; port += 4)
			outl(port, *lp++);
		return n;

	case Qmsr:
		if(n & 7)
			error(Ebadarg);
		if((ulong)n/8 > -port)
			error(Ebadarg);
		end = port+(n/8);
		for(vp = a; port != end; port++)
			if(wrmsr(port, *vp++) < 0)
				error(Ebadarg);
		return n;

	default:
		if(c->qid.path < narchdir && (fn = writefn[c->qid.path]) != nil)
			return fn(c, a, n, offset);
		error(Eperm);
		break;
	}
}

Dev archdevtab = {
	'P',
	"arch",

	devreset,
	devinit,
	devshutdown,
	archattach,
	archwalk,
	archstat,
	archopen,
	devcreate,
	archclose,
	archread,
	devbread,
	archwrite,
	devbwrite,
	devremove,
	devwstat,
};

/*
 *  the following is a generic version of the
 *  architecture specific stuff
 */

static int
unimplemented(int)
{
	return 0;
}

static void
nop(void)
{
}

/*
 * 386 has no compare-and-swap instruction.
 * Run it with interrupts turned off instead.
 */
static int
cmpswap386(long *addr, long old, long new)
{
	int r, s;

	s = splhi();
	if(r = (*addr == old))
		*addr = new;
	splx(s);
	return r;
}

/*
 * On a uniprocessor, you'd think that coherence could be nop,
 * but it can't.  We still need a barrier when using coherence() in
 * device drivers.
 *
 * On VMware, it's safe (and a huge win) to set this to nop.
 * Aux/vmware does this via the #P/archctl file.
 */
void (*coherence)(void) = nop;

int (*cmpswap)(long*, long, long) = cmpswap386;

PCArch* arch;
extern PCArch* knownarch[];

typedef struct X86type X86type;
struct X86type {
	int	family;
	int	model;
	int	aalcycles;
	char*	name;
};

static X86type x86intel[] =
{
	{ 4,	0,	22,	"486DX", },	/* known chips */
	{ 4,	1,	22,	"486DX50", },
	{ 4,	2,	22,	"486SX", },
	{ 4,	3,	22,	"486DX2", },
	{ 4,	4,	22,	"486SL", },
	{ 4,	5,	22,	"486SX2", },
	{ 4,	7,	22,	"DX2WB", },	/* P24D */
	{ 4,	8,	22,	"DX4", },	/* P24C */
	{ 4,	9,	22,	"DX4WB", },	/* P24CT */
	{ 5,	0,	23,	"P5", },
	{ 5,	1,	23,	"P5", },
	{ 5,	2,	23,	"P54C", },
	{ 5,	3,	23,	"P24T", },
	{ 5,	4,	23,	"P55C MMX", },
	{ 5,	7,	23,	"P54C VRT", },
	{ 6,	1,	16,	"PentiumPro", },/* trial and error */
	{ 6,	3,	16,	"PentiumII", },
	{ 6,	5,	16,	"PentiumII/Xeon", },
	{ 6,	6,	16,	"Celeron", },
	{ 6,	7,	16,	"PentiumIII/Xeon", },
	{ 6,	8,	16,	"PentiumIII/Xeon", },
	{ 6,	0xB,	16,	"PentiumIII/Xeon", },
	{ 6,	0xF,	16,	"Xeon5000-series", },
	{ 6,	0x16,	16,	"Celeron/Core i7 Mobile", },
	{ 6,	0x17,	16,	"Core 2/Xeon", },
	{ 6,	0x1A,	16,	"Core i7/Xeon", },
	{ 6,	0x1C,	16,	"Atom", },
	{ 6,	0x1D,	16,	"Xeon MP", },
	{ 0xF,	1,	16,	"P4", },	/* P4 */
	{ 0xF,	2,	16,	"PentiumIV/Xeon", },
	{ 0xF,	6,	16,	"PentiumIV/Xeon", },

	{ 3,	-1,	32,	"386", },	/* family defaults */
	{ 4,	-1,	22,	"486", },
	{ 5,	-1,	23,	"P5", },
	{ 6,	-1,	16,	"P6", },
	{ 0xF,	-1,	16,	"P4", },	/* P4 */

	{ -1,	-1,	16,	"unknown", },	/* total default */
};

/*
 * The AMD processors all implement the CPUID instruction.
 * The later ones also return the processor name via functions
 * 0x80000002, 0x80000003 and 0x80000004 in registers AX, BX, CX
 * and DX:
 *	K5	"AMD-K5(tm) Processor"
 *	K6	"AMD-K6tm w/ multimedia extensions"
 *	K6 3D	"AMD-K6(tm) 3D processor"
 *	K6 3D+	?
 */
static X86type x86amd[] =
{
	{ 5,	0,	23,	"AMD-K5", },	/* guesswork */
	{ 5,	1,	23,	"AMD-K5", },	/* guesswork */
	{ 5,	2,	23,	"AMD-K5", },	/* guesswork */
	{ 5,	3,	23,	"AMD-K5", },	/* guesswork */
	{ 5,	4,	23,	"AMD Geode GX1", },	/* guesswork */
	{ 5,	5,	23,	"AMD Geode GX2", },	/* guesswork */
	{ 5,	6,	11,	"AMD-K6", },	/* trial and error */
	{ 5,	7,	11,	"AMD-K6", },	/* trial and error */
	{ 5,	8,	11,	"AMD-K6-2", },	/* trial and error */
	{ 5,	9,	11,	"AMD-K6-III", },/* trial and error */
	{ 5,	0xa,	23,	"AMD Geode LX", },	/* guesswork */

	{ 6,	1,	11,	"AMD-Athlon", },/* trial and error */
	{ 6,	2,	11,	"AMD-Athlon", },/* trial and error */

	{ 0x1F,	9,	11,	"AMD-K10 Opteron G34", },/* guesswork */

	{ 4,	-1,	22,	"Am486", },	/* guesswork */
	{ 5,	-1,	23,	"AMD-K5/K6", },	/* guesswork */
	{ 6,	-1,	11,	"AMD-Athlon", },/* guesswork */
	{ 0xF,	-1,	11,	"AMD-K8", },	/* guesswork */
	{ 0x1F,	-1,	11,	"AMD-K10", },	/* guesswork */
	{ 22,	0,	11,	"AMD Jaguar", },
	{ 22,	3,	11,	"AMD Puma", },
	{ 23,	1,	13,	"AMD Ryzen" },

	{ -1,	-1,	11,	"unknown", },	/* total default */
};

/*
 * WinChip 240MHz
 */
static X86type x86winchip[] =
{
	{5,	4,	23,	"Winchip",},	/* guesswork */
	{6,	7,	23,	"Via C3 Samuel 2 or Ezra",},
	{6,	8,	23,	"Via C3 Ezra-T",},
	{6,	9,	23,	"Via C3 Eden-N",},
	{6,	0xd,	23,	"Via C7 Eden",},
	{ -1,	-1,	23,	"unknown", },	/* total default */
};

/*
 * SiS 55x
 */
static X86type x86sis[] =
{
	{5,	0,	23,	"SiS 55x",},	/* guesswork */
	{ -1,	-1,	23,	"unknown", },	/* total default */
};

static void	simplecycles(uvlong*);
void	(*cycles)(uvlong*) = simplecycles;
void	_cycles(uvlong*);	/* in l.s */

static void
simplecycles(uvlong*x)
{
	*x = m->ticks;
}

void
cpuidprint(void)
{
	print("cpu%d: %dMHz %s %s (AX %8.8uX CX %8.8uX DX %8.8uX)\n",
		m->machno, m->cpumhz, m->cpuidid, m->cpuidtype,
		m->cpuidax, m->cpuidcx, m->cpuiddx);
}

/*
 *  figure out:
 *	- cpu type
 *	- whether or not we have a TSC (cycle counter)
 *	- whether or not it supports page size extensions
 *		(if so turn it on)
 *	- whether or not it supports machine check exceptions
 *		(if so turn it on)
 *	- whether or not it supports the page global flag
 *		(if so turn it on)
 *	- detect PAT feature and add write-combining entry
 *	- detect MTRR support and synchronize state with cpu0
 *	- detect NX support and enable it for AMD64
 *	- detect watchpoint support
 *	- detect FPU features and enable the FPU
 */
int
cpuidentify(void)
{
	int family, model;
	X86type *t, *tab;
	ulong regs[4];
	uintptr cr4;

	cpuid(Highstdfunc, 0, regs);
	memmove(m->cpuidid,   &regs[1], BY2WD);	/* bx */
	memmove(m->cpuidid+4, &regs[3], BY2WD);	/* dx */
	memmove(m->cpuidid+8, &regs[2], BY2WD);	/* cx */
	m->cpuidid[12] = '\0';

	cpuid(Procsig, 0, regs);
	m->cpuidax = regs[0];
	m->cpuidcx = regs[2];
	m->cpuiddx = regs[3];
	
	m->cpuidfamily = m->cpuidax >> 8 & 0xf;
	m->cpuidmodel = m->cpuidax >> 4 & 0xf;
	m->cpuidstepping = m->cpuidax & 0xf;
	switch(m->cpuidfamily){
	case 15:
		m->cpuidfamily += m->cpuidax >> 20 & 0xff;
		m->cpuidmodel += m->cpuidax >> 16 & 0xf;
		break;
	case 6:
		m->cpuidmodel += m->cpuidax >> 16 & 0xf;
		break;
	}

	if(strncmp(m->cpuidid, "AuthenticAMD", 12) == 0 ||
	   strncmp(m->cpuidid, "Geode by NSC", 12) == 0)
		tab = x86amd;
	else if(strncmp(m->cpuidid, "CentaurHauls", 12) == 0)
		tab = x86winchip;
	else if(strncmp(m->cpuidid, "SiS SiS SiS ", 12) == 0)
		tab = x86sis;
	else
		tab = x86intel;

	family = m->cpuidfamily;
	model = m->cpuidmodel;
	for(t=tab; t->name; t++)
		if((t->family == family && t->model == model)
		|| (t->family == family && t->model == -1)
		|| (t->family == -1))
			break;

	/*
	 * This is only meaningfull for old archs on 386 kernel
	 * where we use LOOP+AAM instruction in delayloop()
	 * which has documented cycle times.
	 *
	 * On AMD64, we use a chain of IDIVQ instructions but
	 * hopefully, we have the TSC instruction available
	 * to actually measure the delay.
	 */
	m->delaylcycles = t->aalcycles;
	m->cpuidtype = t->name;

	/*
	 *  if there is one, set tsc to a known value
	 */
	if(m->cpuiddx & Tsc){
		m->havetsc = 1;
		cycles = _cycles;
		if(m->cpuiddx & Cpumsr)
			wrmsr(0x10, 0);
	}

	/*
	 * If machine check exception, page size extensions or page global bit
	 * are supported enable them in CR4 and clear any other set extensions.
	 * If machine check was enabled clear out any lingering status.
	 */
	if(m->cpuiddx & (Pge|Mce|Pse)){
		vlong mca, mct;

		cr4 = getcr4();
		if(m->cpuiddx & Pse)
			cr4 |= 0x10;		/* page size extensions */

		if((m->cpuiddx & Mce) != 0 && getconf("*nomce") == nil){
			if((m->cpuiddx & Mca) != 0){
				vlong cap;
				int bank;

				cap = 0;
				rdmsr(0x179, &cap);

				if(cap & 0x100)
					wrmsr(0x17B, ~0ULL);	/* enable all mca features */

				bank = cap & 0xFF;
				if(bank > 64)
					bank = 64;

				/* init MCi .. MC1 (except MC0) */
				while(--bank > 0){
					wrmsr(0x400 + bank*4, ~0ULL);
					wrmsr(0x401 + bank*4, 0);
				}

				if(family != 6 || model >= 0x1A)
					wrmsr(0x400, ~0ULL);

				wrmsr(0x401, 0);
			}
			else if(family == 5){
				rdmsr(0x00, &mca);
				rdmsr(0x01, &mct);
			}
			cr4 |= 0x40;		/* machine check enable */
		}

		/*
		 * Detect whether the chip supports the global bit
		 * in page directory and page table entries.  When set
		 * in a particular entry, it means ``don't bother removing
		 * this from the TLB when CR3 changes.''
		 *
		 * We flag all kernel pages with this bit.  Doing so lessens the
		 * overhead of switching processes on bare hardware,
		 * even more so on VMware.  See mmu.c:/^memglobal.
		 *
		 * For future reference, should we ever need to do a
		 * full TLB flush, it can be accomplished by clearing
		 * the PGE bit in CR4, writing to CR3, and then
		 * restoring the PGE bit.
		 */
		if(m->cpuiddx & Pge){
			cr4 |= 0x80;		/* page global enable bit */
			m->havepge = 1;
		}
		putcr4(cr4);

		if((m->cpuiddx & (Mca|Mce)) == Mce)
			rdmsr(0x01, &mct);
	}

#ifdef PATWC
	/* IA32_PAT write combining */
	if((m->cpuiddx & Pat) != 0){
		vlong pat;

		if(rdmsr(0x277, &pat) != -1){
			pat &= ~(255LL<<(PATWC*8));
			pat |= 1LL<<(PATWC*8);	/* WC */
			wrmsr(0x277, pat);
		}
	}
#endif

	if((m->cpuiddx & Mtrr) != 0 && getconf("*nomtrr") == nil)
		mtrrsync();

	if(strcmp(m->cpuidid, "GenuineIntel") == 0 && (m->cpuidcx & Rdrnd) != 0)
		hwrandbuf = rdrandbuf;
	else
		hwrandbuf = nil;
	
	if(sizeof(uintptr) == 8) {
		/* 8-byte watchpoints are supported in Long Mode */
		m->havewatchpt8 = 1;

		/* check and enable NX bit */
		cpuid(Highextfunc, 0, regs);
		if(regs[0] >= Procextfeat){
			cpuid(Procextfeat, 0, regs);
			if((regs[3] & (1<<20)) != 0){
				vlong efer;

				/* enable no-execute feature */
				if(rdmsr(Efer, &efer) != -1){
					efer |= 1ull<<11;
					if(wrmsr(Efer, efer) != -1)
						m->havenx = 1;
				}
			}
		}
	} else if(strcmp(m->cpuidid, "GenuineIntel") == 0){
		/* some random CPUs that support 8-byte watchpoints */
		if(family == 15 && (model == 3 || model == 4 || model == 6)
		|| family == 6 && (model == 15 || model == 23 || model == 28))
			m->havewatchpt8 = 1;
		/* Intel SDM claims amd64 support implies 8-byte watchpoint support */
		cpuid(Highextfunc, 0, regs);
		if(regs[0] >= Procextfeat){
			cpuid(Procextfeat, 0, regs);
			if((regs[3] & 1<<29) != 0)
				m->havewatchpt8 = 1;
		}
	}

	fpuinit();

	return t->family;
}

static long
cputyperead(Chan*, void *a, long n, vlong offset)
{
	char str[32];

	snprint(str, sizeof(str), "%s %d\n", m->cpuidtype, m->cpumhz);
	return readstr(offset, a, n, str);
}

static long
archctlread(Chan*, void *a, long nn, vlong offset)
{
	int n;
	char *buf, *p, *ep;

	p = buf = smalloc(READSTR);
	ep = p + READSTR;
	p = seprint(p, ep, "cpu %s %d%s\n",
		m->cpuidtype, m->cpumhz, m->havepge ? " pge" : "");
	p = seprint(p, ep, "pge %s\n", getcr4()&0x80 ? "on" : "off");
	p = seprint(p, ep, "coherence ");
	if(coherence == mb386)
		p = seprint(p, ep, "mb386\n");
	else if(coherence == mb586)
		p = seprint(p, ep, "mb586\n");
	else if(coherence == mfence)
		p = seprint(p, ep, "mfence\n");
	else if(coherence == nop)
		p = seprint(p, ep, "nop\n");
	else
		p = seprint(p, ep, "0x%p\n", coherence);
	p = seprint(p, ep, "cmpswap ");
	if(cmpswap == cmpswap386)
		p = seprint(p, ep, "cmpswap386\n");
	else if(cmpswap == cmpswap486)
		p = seprint(p, ep, "cmpswap486\n");
	else
		p = seprint(p, ep, "0x%p\n", cmpswap);
	p = seprint(p, ep, "arch %s\n", arch->id);
	n = p - buf;
	n += mtrrprint(p, ep - p);
	buf[n] = '\0';

	n = readstr(offset, a, nn, buf);
	free(buf);
	return n;
}

enum
{
	CMpge,
	CMcoherence,
	CMcache,
};

static Cmdtab archctlmsg[] =
{
	CMpge,		"pge",		2,
	CMcoherence,	"coherence",	2,
	CMcache,	"cache",	4,
};

static long
archctlwrite(Chan*, void *a, long n, vlong)
{
	uvlong base, size;
	Cmdbuf *cb;
	Cmdtab *ct;
	char *ep;

	cb = parsecmd(a, n);
	if(waserror()){
		free(cb);
		nexterror();
	}
	ct = lookupcmd(cb, archctlmsg, nelem(archctlmsg));
	switch(ct->index){
	case CMpge:
		if(!m->havepge)
			error("processor does not support pge");
		if(strcmp(cb->f[1], "on") == 0)
			putcr4(getcr4() | 0x80);
		else if(strcmp(cb->f[1], "off") == 0)
			putcr4(getcr4() & ~0x80);
		else
			cmderror(cb, "invalid pge ctl");
		break;
	case CMcoherence:
		if(strcmp(cb->f[1], "mb386") == 0)
			coherence = mb386;
		else if(strcmp(cb->f[1], "mb586") == 0){
			if(m->cpuidfamily < 5)
				error("invalid coherence ctl on this cpu family");
			coherence = mb586;
		}else if(strcmp(cb->f[1], "mfence") == 0){
			if((m->cpuiddx & Sse2) == 0)
				error("invalid coherence ctl on this cpu family");
			coherence = mfence;
		}else if(strcmp(cb->f[1], "nop") == 0){
			/* only safe on vmware */
			if(conf.nmach > 1)
				error("cannot disable coherence on a multiprocessor");
			coherence = nop;
		}else
			cmderror(cb, "invalid coherence ctl");
		break;
	case CMcache:
		base = strtoull(cb->f[1], &ep, 0);
		if(*ep)
			error("cache: parse error: base not a number?");
		size = strtoull(cb->f[2], &ep, 0);
		if(*ep)
			error("cache: parse error: size not a number?");
		ep = mtrr(base, size, cb->f[3]);
		if(ep != nil)
			error(ep);
		break;
	}
	free(cb);
	poperror();
	return n;
}

static long
rmemrw(int isr, void *a, long n, vlong off)
{
	uintptr addr = off;

	if(off < 0 || n < 0)
		error("bad offset/count");
	if(isr){
		if(addr >= MB)
			return 0;
		if(addr+n > MB)
			n = MB - addr;
		memmove(a, KADDR(addr), n);
	}else{
		/* allow vga framebuf's write access */
		if(addr >= MB || addr+n > MB ||
		    (addr < 0xA0000 || addr+n > 0xB0000+0x10000))
			error("bad offset/count in write");
		memmove(KADDR(addr), a, n);
	}
	return n;
}

static long
rmemread(Chan*, void *a, long n, vlong off)
{
	return rmemrw(1, a, n, off);
}

static long
rmemwrite(Chan*, void *a, long n, vlong off)
{
	return rmemrw(0, a, n, off);
}

void
archinit(void)
{
	PCArch **p;

	arch = knownarch[0];
	for(p = knownarch; *p != nil; p++){
		if((*p)->ident != nil && (*p)->ident() == 0){
			arch = *p;
			break;
		}
	}
	if(arch != knownarch[0]){
		if(arch->id == nil)
			arch->id = knownarch[0]->id;
		if(arch->reset == nil)
			arch->reset = knownarch[0]->reset;
		if(arch->intrinit == nil)
			arch->intrinit = knownarch[0]->intrinit;
		if(arch->intrassign == nil)
			arch->intrassign = knownarch[0]->intrassign;
		if(arch->clockinit == nil)
			arch->clockinit = knownarch[0]->clockinit;
	}

	/*
	 *  Decide whether to use copy-on-reference (386 and mp).
	 *  We get another chance to set it in mpinit() for a
	 *  multiprocessor.
	 */
	if(m->cpuidfamily == 3)
		conf.copymode = 1;

	if(m->cpuidfamily >= 4)
		cmpswap = cmpswap486;

	if(m->cpuidfamily >= 5)
		coherence = mb586;

	if(m->cpuiddx & Sse2)
		coherence = mfence;

	addarchfile("cputype", 0444, cputyperead, nil);
	addarchfile("archctl", 0664, archctlread, archctlwrite);
	addarchfile("realmodemem", 0660, rmemread, rmemwrite);
}

/*
 *  call either the pcmcia or pccard device setup
 */
int
pcmspecial(char *idstr, ISAConf *isa)
{
	return (_pcmspecial != nil)? _pcmspecial(idstr, isa): -1;
}

/*
 *  call either the pcmcia or pccard device teardown
 */
void
pcmspecialclose(int a)
{
	if (_pcmspecialclose != nil)
		_pcmspecialclose(a);
}

/*
 *  return value and speed of timer set in arch->clockenable
 */
uvlong
fastticks(uvlong *hz)
{
	return (*arch->fastclock)(hz);
}

ulong
µs(void)
{
	return fastticks2us((*arch->fastclock)(nil));
}

/*
 *  set next timer interrupt
 */
void
timerset(Tval x)
{
	(*arch->timerset)(x);
}

/*
 *  put the processor in the halt state if we've no processes to run.
 *  an interrupt will get us going again.
 *
 *  halting in an smp system can result in a startup latency for
 *  processes that become ready.
 *  if idle_spin is zero, we care more about saving energy
 *  than reducing this latency.
 *
 *  the performance loss with idle_spin == 0 seems to be slight
 *  and it reduces lock contention (thus system time and real time)
 *  on many-core systems with large values of NPROC.
 */
void
idlehands(void)
{
	extern int nrdy, idle_spin;

	if(conf.nmach == 1)
		halt();
	else if(m->cpuidcx & Monitor)
		mwait(&nrdy);
	else if(idle_spin == 0)
		halt();
}

int
isaconfig(char *class, int ctlrno, ISAConf *isa)
{
	char cc[32], *p, *x;
	int i;

	snprint(cc, sizeof cc, "%s%d", class, ctlrno);
	p = getconf(cc);
	if(p == nil)
		return 0;

	x = nil;
	kstrdup(&x, p);
	p = x;

	isa->type = "";
	isa->nopt = tokenize(p, isa->opt, NISAOPT);
	for(i = 0; i < isa->nopt; i++){
		p = isa->opt[i];
		if(cistrncmp(p, "type=", 5) == 0)
			isa->type = p + 5;
		else if(cistrncmp(p, "port=", 5) == 0)
			isa->port = strtoull(p+5, &p, 0);
		else if(cistrncmp(p, "irq=", 4) == 0)
			isa->irq = strtoul(p+4, &p, 0);
		else if(cistrncmp(p, "dma=", 4) == 0)
			isa->dma = strtoul(p+4, &p, 0);
		else if(cistrncmp(p, "mem=", 4) == 0)
			isa->mem = strtoul(p+4, &p, 0);
		else if(cistrncmp(p, "size=", 5) == 0)
			isa->size = strtoul(p+5, &p, 0);
		else if(cistrncmp(p, "freq=", 5) == 0)
			isa->freq = strtoul(p+5, &p, 0);
	}
	return 1;
}

void
dumpmcregs(void)
{
	vlong v, w;
	int bank;

	if((m->cpuiddx & (Mce|Cpumsr)) != (Mce|Cpumsr))
		return;
	if((m->cpuiddx & Mca) == 0){
		rdmsr(0x00, &v);
		rdmsr(0x01, &w);
		iprint("MCA %8.8llux MCT %8.8llux\n", v, w);
		return;
	}
	rdmsr(0x179, &v);
	rdmsr(0x17A, &w);
	iprint("MCG CAP %.16llux STATUS %.16llux\n", v, w);

	bank = v & 0xFF;
	if(bank > 64)
		bank = 64;
	while(--bank >= 0){
		rdmsr(0x401 + bank*4, &v);
		if((v & (1ull << 63)) == 0)
			continue;
		iprint("MC%d STATUS %.16llux", bank, v);
		if(v & (1ull << 58)){
			rdmsr(0x402 + bank*4, &w);
			iprint(" ADDR %.16llux", w);
		}
		if(v & (1ull << 59)){
			rdmsr(0x403 + bank*4, &w);
			iprint(" MISC %.16llux", w);
		}
		iprint("\n");
	}
}

static void
nmihandler(Ureg *ureg, void*)
{
	iprint("cpu%d: nmi PC %#p, status %ux\n",
		m->machno, ureg->pc, inb(0x61));
	while(m->machno != 0)
		;
}

void
nmienable(void)
{
	int x;

	trapenable(VectorNMI, nmihandler, nil, "nmi");

	/*
	 * Hack: should be locked with NVRAM access.
	 */
	outb(0x70, 0x80);		/* NMI latch clear */
	outb(0x70, 0);

	x = inb(0x61) & 0x07;		/* Enable NMI */
	outb(0x61, 0x0C|x);
	outb(0x61, x);
}

void
setupwatchpts(Proc *pr, Watchpt *wp, int nwp)
{
	int i;
	u8int cfg;
	Watchpt *p;

	if(nwp > 4)
		error("there are four watchpoints.");
	if(nwp == 0){
		memset(pr->dr, 0, sizeof(pr->dr));
		return;
	}
	for(p = wp; p < wp + nwp; p++){
		switch(p->type){
		case WATCHRD|WATCHWR: case WATCHWR:
			break;
		case WATCHEX:
			if(p->len != 1)
				error("length must be 1 on breakpoints");
			break;
		default:
			error("type must be rw-, -w- or --x");
		}
		switch(p->len){
		case 1: case 2: case 4:
			break;
		case 8:
			if(m->havewatchpt8) break;
		default:
			error(m->havewatchpt8 ? "length must be 1,2,4,8" : "length must be 1,2,4");
		}
		if((p->addr & p->len - 1) != 0)
			error("address must be aligned according to length");
	}
	
	memset(pr->dr, 0, sizeof(pr->dr));
	pr->dr[6] = 0xffff8ff0;
	for(i = 0; i < nwp; i++){
		pr->dr[i] = wp[i].addr;
		switch(wp[i].type){
			case WATCHRD|WATCHWR: cfg = 3; break;
			case WATCHWR: cfg = 1; break;
			case WATCHEX: cfg = 0; break;
			default: continue;
		}
		switch(wp[i].len){
			case 1: break;
			case 2: cfg |= 4; break;
			case 4: cfg |= 12; break;
			case 8: cfg |= 8; break;
			default: continue;
		}
		pr->dr[7] |= cfg << 16 + 4 * i;
		pr->dr[7] |= 1 << 2 * i + 1;
	}
}
