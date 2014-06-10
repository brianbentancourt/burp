#include "include.h"

#include <sys/un.h>

static int champ_chooser_fork(struct sdirs *sdirs, struct conf *conf)
{
	pid_t childpid=-1;

	if(!conf->forking)
	{
		logp("Not forking a champ chooser process.\n");
		// They need to manually run a separate process.
		return 0;
	}

	switch((childpid=fork()))
	{
		case -1:
			logp("fork failed in %s: %s\n",
				__func__, strerror(errno));
			return -1;
		case 0:
		{
			// Child.
			int cret;
			set_logfp(NULL, conf);
			switch(champ_chooser_server(sdirs, conf))
			{
				case 0: cret=0;
				default: cret=1;
			}
			exit(cret);
		}
		default:
			// Parent.
			logp("forked champ chooser pid %d\n", childpid);
			return 0;
	}
	return -1; // Not reached.
}

int connect_to_champ_chooser(struct sdirs *sdirs, struct conf *conf)
{
	int len;
	int s=-1;
	int tries=0;
	int tries_max=3;
	struct sockaddr_un remote;

	if(!lock_test(sdirs->champlock))
	{
		// Champ chooser is not running.
		// Try to fork a new champ chooser process.
		if(champ_chooser_fork(sdirs, conf)) return -1;
	}

	// Champ chooser should either be running now, or about to run.

	if((s=socket(AF_UNIX, SOCK_STREAM, 0))<0)
	{
		logp("socket error in %s: %s\n", __func__, strerror(errno));
		return -1;
	}

	memset(&remote, 0, sizeof(struct sockaddr_un));
	remote.sun_family=AF_UNIX;
	strcpy(remote.sun_path, sdirs->champsock);
	len=strlen(remote.sun_path)+sizeof(remote.sun_family);

	while(tries++<tries_max)
	{
		int sleeptimeleft=3;
		if(connect(s, (struct sockaddr *)&remote, len)<0)
		{
			logp("connect error in %s: %d %s\n",
				__func__, errno, strerror(errno));
		}
		else
		{
			logp("Connected to champ chooser.\n");
			return s;
		}

		// SIGCHLDs may be interrupting.
		sleeptimeleft=3;
		while(sleeptimeleft>0) sleeptimeleft=sleep(sleeptimeleft);
	}

	logp("Could not connect to champ chooser via %s after %d attempts.\n",
		sdirs->champsock, tries_max);

	return -1;
}