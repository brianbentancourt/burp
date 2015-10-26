#include "../burp.h"
#include "../asfd.h"
#include "../async.h"
#include "../cmd.h"
#include "../conf.h"
#include "../conffile.h"
#include "../handy.h"
#include "../iobuf.h"
#include "../log.h"
#include "autoupgrade.h"
#include "incexc_recv.h"
#include "incexc_send.h"

#ifndef HAVE_WIN32
#include <sys/utsname.h>
#endif

static const char *server_supports(const char *feat, const char *wanted)
{
	return strstr(feat, wanted);
}

static const char *server_supports_autoupgrade(const char *feat)
{
	// 1.3.0 servers did not list the features, but the only feature
	// that was supported was autoupgrade.
	if(!strcmp(feat, "extra_comms_begin ok")) return "ok";
	return server_supports(feat, ":autoupgrade:");
}

int extra_comms(struct async *as, struct conf **confs,
	enum action *action, char **incexc)
{
	int ret=-1;
	char *feat=NULL;
	struct asfd *asfd;
	struct iobuf *rbuf;
	const char *orig_client=get_string(confs[OPT_ORIG_CLIENT]);
	asfd=as->asfd;
	rbuf=asfd->rbuf;

	if(asfd->write_str(asfd, CMD_GEN, "extra_comms_begin"))
	{
		logp("Problem requesting extra_comms_begin\n");
		goto end;
	}
	// Servers greater than 1.3.0 will list the extra_comms
	// features they support.
	if(asfd->read(asfd))
	{
		logp("Problem reading response to extra_comms_begin\n");
		goto end;
	}
	feat=rbuf->buf;
	if(rbuf->cmd!=CMD_GEN
	  || strncmp_w(feat, "extra_comms_begin ok"))
	{
		iobuf_log_unexpected(rbuf, __func__);
		goto end;
	}
	logp("%s\n", feat);
	iobuf_init(rbuf);

	// Can add extra bits here. The first extra bit is the
	// autoupgrade stuff.
	if(server_supports_autoupgrade(feat)
	  && get_string(confs[OPT_AUTOUPGRADE_DIR])
	  && get_string(confs[OPT_AUTOUPGRADE_OS])
	  && autoupgrade_client(as, confs))
		goto end;

	// :srestore: means that the server wants to do a restore.
	if(server_supports(feat, ":srestore:"))
	{
		logp("Server wants to initiate a restore\n");
		if(*action==ACTION_MONITOR)
		{
			logp("Client is in monitor mode, so ignoring\n");
		}
		else if(get_int(confs[OPT_SERVER_CAN_RESTORE]))
		{
			logp("Client accepts.\n");
			if(incexc_recv_client_restore(asfd, incexc, confs))
				goto end;
			if(*incexc)
			{
				if(conf_parse_incexcs_buf(confs, *incexc))
					goto end;
				*action=ACTION_RESTORE;
				log_restore_settings(confs, 1);
			}
		}
		else
		{
			logp("Client configuration says no\n");
			if(asfd->write_str(asfd, CMD_GEN, "srestore not ok"))
				goto end;
		}
	}

	if(orig_client)
	{
		char str[512]="";
		snprintf(str, sizeof(str), "orig_client=%s", orig_client);
		if(!server_supports(feat, ":orig_client:"))
		{
			logp("Server does not support switching client.\n");
			goto end;
		}
		if(asfd->write_str(asfd, CMD_GEN, str)
		  || asfd->read_expect(asfd, CMD_GEN, "orig_client ok"))
		{
			logp("Problem requesting %s\n", str);
			goto end;
		}
		logp("Switched to client %s\n", orig_client);
	}

	// :sincexc: is for the server giving the client the
	// incexc config.
	if(*action==ACTION_BACKUP
	  || *action==ACTION_BACKUP_TIMED
	  || *action==ACTION_TIMER_CHECK)
	{
		if(!*incexc && server_supports(feat, ":sincexc:"))
		{
			logp("Server is setting includes/excludes.\n");
			if(incexc_recv_client(asfd, incexc, confs))
				goto end;
			if(*incexc && conf_parse_incexcs_buf(confs,
				*incexc)) goto end;
		}
	}

	if(server_supports(feat, ":counters:"))
	{
		if(asfd->write_str(asfd, CMD_GEN, "countersok"))
			goto end;
		set_int(confs[OPT_SEND_CLIENT_CNTR], 1);
	}

	// :incexc: is for the client sending the server the
	// incexc conf so that it better knows what to do on
	// resume.
	if(server_supports(feat, ":incexc:")
	  && incexc_send_client(asfd, confs))
		goto end;

	if(server_supports(feat, ":uname:"))
	{
		const char *clientos=NULL;
#ifdef HAVE_WIN32
#ifdef _WIN64
		clientos="Windows 64bit";
#else
		clientos="Windows 32bit";
#endif
#else
		struct utsname utsname;
		if(!uname(&utsname))
			clientos=(const char *)utsname.sysname;
#endif
		if(clientos)
		{
			char msg[128]="";
			snprintf(msg, sizeof(msg),
				"uname=%s", clientos);
			if(asfd->write_str(asfd, CMD_GEN, msg))
				goto end;
		}
	}

	if(server_supports(feat, ":csetproto:"))
	{
		char msg[128]="";
		// Use protocol2 if no choice has been made on client side.
		if(get_protocol(confs)==PROTO_AUTO)
		{
			logp("Server has protocol=0 (auto)\n");
			set_protocol(confs, PROTO_2);
		}
		// Send choice to server.
		snprintf(msg, sizeof(msg), "protocol=%d",
			get_protocol(confs));
		if(asfd->write_str(asfd, CMD_GEN, msg))
			goto end;
		logp("Using protocol=%d\n",
			get_protocol(confs));
	}
	else if(server_supports(feat, ":forceproto=1:"))
	{
		logp("Server is forcing protocol 1\n");
		if(get_protocol(confs)!=PROTO_AUTO
		  && get_protocol(confs)!=PROTO_1)
		{
			logp("But client has set protocol=%d!\n",
				get_protocol(confs));
			goto end;
		}
		set_protocol(confs, PROTO_1);
	}
	else if(server_supports(feat, ":forceproto=2:"))
	{
		logp("Server is forcing protocol 2\n");
		if(get_protocol(confs)!=PROTO_AUTO
		  && get_protocol(confs)!=PROTO_2)
		{
			logp("But client has set protocol=%d!\n",
				get_protocol(confs));
			goto end;
		}
		set_protocol(confs, PROTO_2);
	}

	if(server_supports(feat, ":msg:"))
	{
		set_int(confs[OPT_MESSAGE], 1);
		if(asfd->write_str(asfd, CMD_GEN, "msg"))
			goto end;
	}

#ifndef RS_DEFAULT_STRONG_LEN
	if(server_supports(feat, ":rshash=blake2:"))
	{
		set_e_rshash(confs[OPT_RSHASH], RSHASH_BLAKE2);
		// Send choice to server.
		if(asfd->write_str(asfd, CMD_GEN, "rshash=blake2"))
			goto end;
	}
	else
#endif
		set_e_rshash(confs[OPT_RSHASH], RSHASH_MD4);

	if(asfd->write_str(asfd, CMD_GEN, "extra_comms_end")
	  || asfd->read_expect(asfd, CMD_GEN, "extra_comms_end ok"))
	{
		logp("Problem requesting extra_comms_end\n");
		goto end;
	}

	ret=0;
end:
	return ret;
}
