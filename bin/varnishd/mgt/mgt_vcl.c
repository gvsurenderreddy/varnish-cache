/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2015 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * VCL management stuff
 */

#include "config.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "mgt/mgt.h"

#include "libvcc.h"
#include "vcli_serve.h"
#include "vev.h"
#include "vtim.h"

static const char * const VCL_STATE_COLD = "cold";
static const char * const VCL_STATE_WARM = "warm";
static const char * const VCL_STATE_AUTO = "auto";
static const char * const VCL_STATE_LABEL = "label";

struct vclprog;

struct vcldep {
	unsigned		magic;
#define VCLDEP_MAGIC		0xa9a17dc2
	struct vclprog		*from;
	VTAILQ_ENTRY(vcldep)	lfrom;
	struct vclprog		*to;
	VTAILQ_ENTRY(vcldep)	lto;
};

struct vclprog {
	unsigned		magic;
#define VCLPROG_MAGIC		0x9ac09fea
	VTAILQ_ENTRY(vclprog)	list;
	char			*name;
	char			*fname;
	unsigned		warm;
	const char *		state;
	double			go_cold;
	struct vclprog		*label;
	VTAILQ_HEAD(, vcldep)	dfrom;
	VTAILQ_HEAD(, vcldep)	dto;
};

static VTAILQ_HEAD(, vclprog)	vclhead = VTAILQ_HEAD_INITIALIZER(vclhead);
static struct vclprog		*active_vcl;
static struct vev *e_poker;

/*--------------------------------------------------------------------*/

static void
mgt_vcl_dep_add(struct vclprog *vp_from, struct vclprog *vp_to)
{
	struct vcldep *vd;

	CHECK_OBJ_NOTNULL(vp_from, VCLPROG_MAGIC);
	CHECK_OBJ_NOTNULL(vp_to, VCLPROG_MAGIC);
	ALLOC_OBJ(vd, VCLDEP_MAGIC);
	XXXAN(vd);
	vd->from = vp_from;
	VTAILQ_INSERT_TAIL(&vp_from->dfrom, vd, lfrom);
	vd->to = vp_to;
	VTAILQ_INSERT_TAIL(&vp_to->dto, vd, lto);
}

static void
mgt_vcl_dep_del(struct vcldep *vd)
{

	CHECK_OBJ_NOTNULL(vd, VCLDEP_MAGIC);
	VTAILQ_REMOVE(&vd->from->dfrom, vd, lfrom);
	VTAILQ_REMOVE(&vd->to->dfrom, vd, lto);
	FREE_OBJ(vd);
}

/*--------------------------------------------------------------------*/

static struct vclprog *
mgt_vcl_add(const char *name, const char *state)
{
	struct vclprog *vp;

	assert(state == VCL_STATE_WARM ||
	       state == VCL_STATE_COLD ||
	       state == VCL_STATE_AUTO ||
	       state == VCL_STATE_LABEL);
	ALLOC_OBJ(vp, VCLPROG_MAGIC);
	XXXAN(vp);
	REPLACE(vp->name, name);
	VTAILQ_INIT(&vp->dfrom);
	VTAILQ_INIT(&vp->dto);
	vp->state = state;

	if (vp->state != VCL_STATE_COLD)
		vp->warm = 1;

	if (active_vcl == NULL)
		active_vcl = vp;
	VTAILQ_INSERT_TAIL(&vclhead, vp, list);
	return (vp);
}

static void
mgt_vcl_del(struct vclprog *vp)
{
	char *p;

	CHECK_OBJ_NOTNULL(vp, VCLPROG_MAGIC);
	while (!VTAILQ_EMPTY(&vp->dfrom))
		mgt_vcl_dep_del(VTAILQ_FIRST(&vp->dfrom));

	VTAILQ_REMOVE(&vclhead, vp, list);
	if (vp->fname != NULL) {
		AZ(unlink(vp->fname));
		p = strrchr(vp->fname, '/');
		AN(p);
		*p = '\0';
		VJ_master(JAIL_MASTER_FILE);
		/*
		 * This will fail if any files are dropped next to the library
		 * without us knowing.  This happens for instance with GCOV.
		 * Assume developers know how to clean up after themselves
		 * (or alternatively:  How to run out of disk space).
		 */
		(void)rmdir(vp->fname);
		VJ_master(JAIL_MASTER_LOW);
		free(vp->fname);
	}
	free(vp->name);
	FREE_OBJ(vp);
}

static struct vclprog *
mgt_vcl_byname(const char *name)
{
	struct vclprog *vp;

	VTAILQ_FOREACH(vp, &vclhead, list)
		if (!strcmp(name, vp->name))
			return (vp);
	return (NULL);
}

void
mgt_vcl_depends(struct vclprog *vp1, const char *name)
{
	struct vclprog *vp2;

	CHECK_OBJ_NOTNULL(vp1, VCLPROG_MAGIC);

	vp2 = mgt_vcl_byname(name);
	CHECK_OBJ_NOTNULL(vp2, VCLPROG_MAGIC);
	mgt_vcl_dep_add(vp1, vp2);
}

int
mgt_has_vcl(void)
{

	return (!VTAILQ_EMPTY(&vclhead));
}

static int
mgt_vcl_setstate(struct cli *cli, struct vclprog *vp, const char *vs)
{
	unsigned status, warm;
	double now;
	char *p;
	int i;

	if (vp == active_vcl || vp->label != NULL) {
		AN(vp->warm);
		return (0);
	}
	if (vs == VCL_STATE_AUTO) {
		now = VTIM_mono();
		vs = vp->warm ? VCL_STATE_WARM : VCL_STATE_COLD;
		if (vp->go_cold > 0 && vp->state == VCL_STATE_AUTO &&
		    vp->go_cold + mgt_param.vcl_cooldown < now)
			vs = VCL_STATE_COLD;
	}

	assert(vs != VCL_STATE_AUTO);
	warm = vs == VCL_STATE_WARM ? 1 : 0;

	if (vp->warm == warm)
		return (0);

	vp->warm = warm;

	if (vp->warm == 0)
		vp->go_cold = 0;

	if (child_pid < 0)
		return (0);

	i = mgt_cli_askchild(&status, &p, "vcl.state %s %d%s\n",
	    vp->name, vp->warm, vp->state);
	if (i) {
		AN(cli);
		AN(vp->warm);
		VCLI_SetResult(cli, status);
		VCLI_Out(cli, "%s", p);
	}

	free(p);
	return (i);
}

/*--------------------------------------------------------------------*/

static void
mgt_new_vcl(struct cli *cli, const char *vclname, const char *vclsrc,
    const char *vclsrcfile, const char *state, int C_flag)
{
	unsigned status;
	char *lib, *p;
	struct vclprog *vp;
	char buf[32];

	AN(cli);

	if (C_flag) {
		bprintf(buf, ".CflagTest.%d", (int)getpid());
		vclname = buf;
	}

	if (state == NULL)
		state = VCL_STATE_AUTO;
	else if (!strcmp(state, VCL_STATE_AUTO))
		state = VCL_STATE_AUTO;
	else if (!strcmp(state, VCL_STATE_COLD))
		state = VCL_STATE_COLD;
	else if (!strcmp(state, VCL_STATE_WARM))
		state = VCL_STATE_WARM;
	else {
		VCLI_Out(cli, "State must be one of auto, cold or warm.");
		VCLI_SetResult(cli, CLIS_PARAM);
		return;
	}

	vp = mgt_vcl_add(vclname, state);
	lib = mgt_VccCompile(cli, vp, vclname, vclsrc, vclsrcfile, C_flag);
	if (lib == NULL) {
		mgt_vcl_del(vp);
		return;
	}

	AZ(C_flag);
	vp->fname = lib;

	if (child_pid < 0)
		return;

	if (mgt_cli_askchild(&status, &p, "vcl.load %s %s %d%s\n",
	    vp->name, vp->fname, vp->warm, vp->state)) {
		mgt_vcl_del(vp);
		VCLI_Out(cli, "%s", p);
		VCLI_SetResult(cli, CLIS_PARAM);
	}
	free(p);
}

/*--------------------------------------------------------------------*/

void
mgt_vcc_startup(struct cli *cli, const char *b_arg, const char *f_arg,
    const char *vclsrc, int C_flag)
{
	char buf[BUFSIZ];

	if (b_arg == NULL) {
		AN(vclsrc);
		AN(f_arg);
		mgt_new_vcl(cli, "boot", vclsrc, f_arg, NULL, C_flag);
		return;
	}

	AZ(vclsrc);
	bprintf(buf,
	    "vcl 4.0;\n"
	    "backend default {\n"
	    "    .host = \"%s\";\n"
	    "}\n", b_arg);
	mgt_new_vcl(cli, "boot", buf, "<-b argument>", NULL, C_flag);
}

/*--------------------------------------------------------------------*/

void
mgt_vcl_export_labels(struct vcc *vcc)
{
	struct vclprog *vp;
	VTAILQ_FOREACH(vp, &vclhead, list) {
		if (!strcmp(vp->state, VCL_STATE_LABEL))
			VCC_Predef(vcc, "VCL_VCL", vp->name);
	}
}

/*--------------------------------------------------------------------*/

int
mgt_push_vcls_and_start(struct cli *cli, unsigned *status, char **p)
{
	struct vclprog *vp;

	AN(active_vcl);

	/* The VCL has not been loaded yet, it cannot fail */
	AZ(mgt_vcl_setstate(cli, active_vcl, VCL_STATE_WARM));

	VTAILQ_FOREACH(vp, &vclhead, list) {
		if (!strcmp(vp->state, VCL_STATE_LABEL))
			continue;
		if (mgt_cli_askchild(status, p, "vcl.load \"%s\" %s %d%s\n",
		    vp->name, vp->fname, vp->warm, vp->state))
			return (1);
		free(*p);
		*p = NULL;
	}
	VTAILQ_FOREACH(vp, &vclhead, list) {
		if (strcmp(vp->state, VCL_STATE_LABEL))
			continue;
		if (mgt_cli_askchild(status, p, "vcl.label %s %s\n",
		    vp->name, vp->label->name))
			return (1);
		free(*p);
		*p = NULL;
	}
	if (mgt_cli_askchild(status, p, "vcl.use \"%s\"\n", active_vcl->name))
		return (1);
	free(*p);
	*p = NULL;
	if (mgt_cli_askchild(status, p, "start\n"))
		return (1);
	free(*p);
	*p = NULL;
	return (0);
}

/*--------------------------------------------------------------------*/

static void __match_proto__(cli_func_t)
mcf_vcl_inline(struct cli *cli, const char * const *av, void *priv)
{
	struct vclprog *vp;

	(void)priv;

	vp = mgt_vcl_byname(av[2]);
	if (vp != NULL) {
		VCLI_Out(cli, "Already a VCL program named %s", av[2]);
		VCLI_SetResult(cli, CLIS_PARAM);
		return;
	}

	mgt_new_vcl(cli, av[2], av[3], "<vcl.inline>", av[4], 0);
}

static void __match_proto__(cli_func_t)
mcf_vcl_load(struct cli *cli, const char * const *av, void *priv)
{
	struct vclprog *vp;

	(void)priv;
	vp = mgt_vcl_byname(av[2]);
	if (vp != NULL) {
		VCLI_Out(cli, "Already a VCL program named %s", av[2]);
		VCLI_SetResult(cli, CLIS_PARAM);
		return;
	}

	mgt_new_vcl(cli, av[2], NULL, av[3], av[4], 0);
}

static struct vclprog *
mcf_find_vcl(struct cli *cli, const char *name)
{
	struct vclprog *vp;

	vp = mgt_vcl_byname(name);
	if (vp == NULL) {
		VCLI_SetResult(cli, CLIS_PARAM);
		VCLI_Out(cli, "No configuration named %s known.", name);
	}
	return (vp);
}

static void __match_proto__(cli_func_t)
mcf_vcl_state(struct cli *cli, const char * const *av, void *priv)
{
	struct vclprog *vp;

	(void)priv;
	vp = mcf_find_vcl(cli, av[2]);
	if (vp == NULL)
		return;

	if (!strcmp(vp->state, VCL_STATE_LABEL)) {
		VCLI_Out(cli, "Labels are always warm");
		VCLI_SetResult(cli, CLIS_PARAM);
		return;
	}
	if (vp->label != NULL) {
		AZ(!strcmp(vp->state, "cold"));
		if (!strcmp(av[3], "cold")) {
			VCLI_Out(cli, "A labeled VCL cannot be set cold");
			VCLI_SetResult(cli, CLIS_CANT);
			return;
		}
	}

	if (!strcmp(vp->state, av[3]))
		return;

	if (!strcmp(av[3], VCL_STATE_AUTO)) {
		vp->state = VCL_STATE_AUTO;
		if (vp != active_vcl) {
			vp->go_cold = VTIM_mono();
			(void)mgt_vcl_setstate(cli, vp, VCL_STATE_AUTO);
		}
	} else if (!strcmp(av[3], VCL_STATE_COLD)) {
		if (vp == active_vcl) {
			VCLI_Out(cli, "Cannot set the active VCL cold.");
			VCLI_SetResult(cli, CLIS_PARAM);
			return;
		}
		vp->state = VCL_STATE_AUTO;
		(void)mgt_vcl_setstate(cli, vp, VCL_STATE_COLD);
	} else if (!strcmp(av[3], VCL_STATE_WARM)) {
		if (mgt_vcl_setstate(cli, vp, VCL_STATE_WARM) == 0)
			vp->state = VCL_STATE_WARM;
	} else {
		VCLI_Out(cli, "State must be one of auto, cold or warm.");
		VCLI_SetResult(cli, CLIS_PARAM);
	}
}

static void __match_proto__(cli_func_t)
mcf_vcl_use(struct cli *cli, const char * const *av, void *priv)
{
	unsigned status;
	char *p = NULL;
	struct vclprog *vp, *vp2;

	(void)priv;
	vp = mcf_find_vcl(cli, av[2]);
	if (vp == NULL)
		return;
	if (vp == active_vcl)
		return;
	if (mgt_vcl_setstate(cli, vp, VCL_STATE_WARM))
		return;
	if (child_pid >= 0 &&
	    mgt_cli_askchild(&status, &p, "vcl.use %s\n", av[2])) {
		VCLI_SetResult(cli, status);
		VCLI_Out(cli, "%s", p);
		vp->go_cold = VTIM_mono();
		(void)mgt_vcl_setstate(cli, vp, VCL_STATE_AUTO);
	} else {
		VCLI_Out(cli, "VCL '%s' now active", av[2]);
		vp2 = active_vcl;
		active_vcl = vp;
		if (vp2 != NULL) {
			vp2->go_cold = VTIM_mono();
			(void)mgt_vcl_setstate(cli, vp2, VCL_STATE_AUTO);
		}
	}
	free(p);
}

static void __match_proto__(cli_func_t)
mcf_vcl_discard(struct cli *cli, const char * const *av, void *priv)
{
	unsigned status;
	char *p = NULL;
	struct vclprog *vp;
	struct vcldep *vd;
	int n;

	(void)priv;
	vp = mcf_find_vcl(cli, av[2]);
	if (vp == NULL)
		return;
	if (vp == active_vcl) {
		VCLI_SetResult(cli, CLIS_PARAM);
		VCLI_Out(cli, "Cannot discard active VCL program\n");
		return;
	}
	if (!VTAILQ_EMPTY(&vp->dto)) {
		if (vp->label != NULL && strcmp(vp->state, VCL_STATE_LABEL)) {
			AN(vp->warm);
			VCLI_SetResult(cli, CLIS_PARAM);
			VCLI_Out(cli,
			    "Cannot discard labeled (\"%s\") VCL program.\n",
			    vp->label->name);
			return;
		}
		VCLI_SetResult(cli, CLIS_PARAM);
		VCLI_Out(cli,
		    "Cannot discard \"%s\" VCL label, "
		    "other VCLs depend on it.\n", vp->name);
		n = 0;
		VTAILQ_FOREACH(vd, &vp->dto, lto) {
			if (n++ == 5) {
				VCLI_Out(cli, "\t[...]");
				break;
			}
			VCLI_Out(cli, "\t%s\n", vd->from->name);
		}
		return;
	}
	if (!strcmp(vp->state, VCL_STATE_LABEL)) {
		AN(vp->warm);
		vp->label->label = NULL;
		vp->label = NULL;
	} else {
		(void)mgt_vcl_setstate(cli, vp, VCL_STATE_COLD);
	}
	if (child_pid >= 0) {
		/* XXX If this fails the child is crashing, figure that later */
		(void)mgt_cli_askchild(&status, &p, "vcl.discard %s\n", av[2]);
		free(p);
	}
	mgt_vcl_del(vp);
}

static void __match_proto__(cli_func_t)
mcf_vcl_list(struct cli *cli, const char * const *av, void *priv)
{
	unsigned status;
	char *p;
	struct vclprog *vp;

	(void)av;
	(void)priv;
	if (child_pid >= 0) {
		if (!mgt_cli_askchild(&status, &p, "vcl.list\n")) {
			VCLI_SetResult(cli, status);
			VCLI_Out(cli, "%s", p);
		}
		free(p);
	} else {
		VTAILQ_FOREACH(vp, &vclhead, list) {
			VCLI_Out(cli, "%-10s %5s",
			    vp == active_vcl ? "active" : "available",
			    vp->state);
			VCLI_Out(cli, "/%-8s", vp->warm ?
			    VCL_STATE_WARM : VCL_STATE_COLD);
			VCLI_Out(cli, " %6s %s", "", vp->name);
			if (vp->label != NULL)
				VCLI_Out(cli, " %s %s",
				    strcmp(vp->state, VCL_STATE_LABEL) ?
				    "<-" : "->", vp->label->name);
			VCLI_Out(cli, "\n");
		}
	}
}

static void __match_proto__(cli_func_t)
mcf_vcl_label(struct cli *cli, const char * const *av, void *priv)
{
	struct vclprog *vpl;
	struct vclprog *vpt;
	unsigned status;
	char *p;
	int i;

	(void)priv;
	vpt = mcf_find_vcl(cli, av[3]);
	if (vpt == NULL)
		return;
	if (!strcmp(vpt->state, VCL_STATE_LABEL)) {
		VCLI_SetResult(cli, CLIS_PARAM);
		VCLI_Out(cli, "VCL labels cannot point to labels");
		return;
	}
	if (vpt->label != NULL) {
		VCLI_SetResult(cli, CLIS_PARAM);
		VCLI_Out(cli, "VCL already labeled (\"%s\")",
		    vpt->label->name);
		return;
	}
	vpl = mgt_vcl_byname(av[2]);
	if (vpl != NULL) {
		if (strcmp(vpl->state, VCL_STATE_LABEL)) {
			VCLI_SetResult(cli, CLIS_PARAM);
			VCLI_Out(cli, "%s is not a label", vpl->name);
			return;
		}
		AN(vpl->label);
		assert(vpl->label->label == vpl);
		/* XXX SET vp->label AUTO */
		vpl->label->label = NULL;
		vpl->label = NULL;
		mgt_vcl_dep_del(VTAILQ_FIRST(&vpl->dfrom));
		AN(VTAILQ_EMPTY(&vpl->dfrom));
	} else {
		/* XXX should check for C-syntax */
		if (strchr(av[2], '.')) {
			VCLI_SetResult(cli, CLIS_PARAM);
			VCLI_Out(cli, "VCL labels cannot contain '.'");
			return;
		}
		vpl = mgt_vcl_add(av[2], VCL_STATE_LABEL);
	}
	AN(vpl);
	mgt_vcl_dep_add(vpl, vpt);
	vpl->warm = 1;
	vpl->label = vpt;
	vpt->label = vpl;
	if (vpt->state == VCL_STATE_COLD)
		vpt->state = VCL_STATE_AUTO;
	(void)mgt_vcl_setstate(cli, vpt, VCL_STATE_WARM);
	if (child_pid < 0)
		return;

	i = mgt_cli_askchild(&status, &p, "vcl.label %s %s\n", av[2], av[3]);
	if (i) {
		VCLI_SetResult(cli, status);
		VCLI_Out(cli, "%s", p);
	}
	free(p);
}

/*--------------------------------------------------------------------*/

static int __match_proto__(vev_cb_f)
mgt_vcl_poker(const struct vev *e, int what)
{
	struct vclprog *vp;

	(void)e;
	(void)what;
	e_poker->timeout = mgt_param.vcl_cooldown * .45;
	VTAILQ_FOREACH(vp, &vclhead, list)
		(void)mgt_vcl_setstate(NULL, vp, VCL_STATE_AUTO);
	return (0);
}

/*--------------------------------------------------------------------*/

static struct cli_proto cli_vcl[] = {
	{ CLICMD_VCL_LOAD,		"", mcf_vcl_load },
	{ CLICMD_VCL_INLINE,		"", mcf_vcl_inline },
	{ CLICMD_VCL_USE,		"", mcf_vcl_use },
	{ CLICMD_VCL_STATE,		"", mcf_vcl_state },
	{ CLICMD_VCL_DISCARD,		"", mcf_vcl_discard },
	{ CLICMD_VCL_LIST,		"", mcf_vcl_list },
	{ CLICMD_VCL_LABEL,		"", mcf_vcl_label },
	{ NULL }
};

/*--------------------------------------------------------------------*/

static void
mgt_vcl_atexit(void)
{
	struct vclprog *vp;

	if (getpid() != mgt_pid)
		return;
	do {
		vp = VTAILQ_FIRST(&vclhead);
		if (vp != NULL)
			mgt_vcl_del(vp);
	} while (vp != NULL);
}

void
mgt_vcl_init(void)
{

	e_poker = vev_new();
	AN(e_poker);
	e_poker->timeout = 3;		// random, prime

	e_poker->callback = mgt_vcl_poker;
	e_poker->name = "vcl poker";
	AZ(vev_add(mgt_evb, e_poker));

	AZ(atexit(mgt_vcl_atexit));

	VCLS_AddFunc(mgt_cls, MCF_AUTH, cli_vcl);
}
