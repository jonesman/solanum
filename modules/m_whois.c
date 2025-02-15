/*
 *  ircd-ratbox: A slightly useful ircd.
 *  m_whois.c: Shows who a user is.
 *
 *  Copyright (C) 1990 Jarkko Oikarinen and University of Oulu, Co Center
 *  Copyright (C) 1996-2002 Hybrid Development Team
 *  Copyright (C) 2002-2005 ircd-ratbox development team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 *  USA
 */

#include "stdinc.h"
#include "client.h"
#include "hash.h"
#include "channel.h"
#include "hash.h"
#include "ircd.h"
#include "numeric.h"
#include "s_conf.h"
#include "s_user.h"
#include "s_serv.h"
#include "send.h"
#include "match.h"
#include "logger.h"
#include "msg.h"
#include "parse.h"
#include "modules.h"
#include "hook.h"
#include "s_newconf.h"
#include "ratelimit.h"
#include "s_assert.h"

static const char whois_desc[] =
	"Provides the WHOIS command to display information about a user";

static void do_whois(struct Client *client_p, struct Client *source_p, int parc, const char *parv[]);
static void single_whois(struct Client *source_p, struct Client *target_p, int operspy);

static void m_whois(struct MsgBuf *, struct Client *, struct Client *, int, const char **);
static void ms_whois(struct MsgBuf *, struct Client *, struct Client *, int, const char **);

struct Message whois_msgtab = {
	"WHOIS", 0, 0, 0, 0,
	{mg_unreg, {m_whois, 2}, {ms_whois, 2}, mg_ignore, mg_ignore, {m_whois, 2}}
};

int doing_whois_hook;
int doing_whois_global_hook;
int doing_whois_channel_visibility_hook;
int doing_whois_show_idle_hook;

mapi_clist_av1 whois_clist[] = { &whois_msgtab, NULL };
mapi_hlist_av1 whois_hlist[] = {
	{ "doing_whois",			&doing_whois_hook },
	{ "doing_whois_global",			&doing_whois_global_hook },
	{ "doing_whois_channel_visibility",	&doing_whois_channel_visibility_hook },
	{ "doing_whois_show_idle",		&doing_whois_show_idle_hook },
	{ NULL, NULL }
};

DECLARE_MODULE_AV2(whois, NULL, NULL, whois_clist, whois_hlist, NULL, NULL, NULL, whois_desc);

/*
 * m_whois
 *      parv[1] = nickname masklist
 */
static void
m_whois(struct MsgBuf *msgbuf_p, struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	static time_t last_used = 0;

	if(parc > 2)
	{
		if(EmptyString(parv[2]))
		{
			sendto_one(source_p, form_str(ERR_NONICKNAMEGIVEN),
					me.name, source_p->name);
			return;
		}

		if(!IsOperGeneral(source_p))
		{
			/* seeing as this is going across servers, we should limit it */
			if((last_used + ConfigFileEntry.pace_wait_simple) > rb_current_time() || !ratelimit_client(source_p, 2))
			{
				sendto_one(source_p, form_str(RPL_LOAD2HI),
					   me.name, source_p->name, "WHOIS");
				sendto_one_numeric(source_p, RPL_ENDOFWHOIS,
						   form_str(RPL_ENDOFWHOIS), parv[2]);
				return;
			}
			else
				last_used = rb_current_time();
		}

		if(hunt_server(client_p, source_p, ":%s WHOIS %s :%s", 1, parc, parv) !=
		   HUNTED_ISME)
			return;

		parv[1] = parv[2];

	}
	do_whois(client_p, source_p, parc, parv);
}

/*
 * ms_whois
 *      parv[1] = server to reply
 *      parv[2] = nickname to whois
 */
static void
ms_whois(struct MsgBuf *msgbuf_p, struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	struct Client *target_p;

	/* note: early versions of ratbox allowed users to issue a remote
	 * whois with a blank parv[2], so we cannot treat it as a protocol
	 * violation. --anfl
	 */
	if(parc < 3 || EmptyString(parv[2]))
	{
		sendto_one(source_p, form_str(ERR_NONICKNAMEGIVEN),
				me.name, source_p->name);
		return;
	}

	/* check if parv[1] exists */
	if((target_p = find_client(parv[1])) == NULL)
	{
		sendto_one_numeric(source_p, ERR_NOSUCHSERVER,
				   form_str(ERR_NOSUCHSERVER),
				   IsDigit(parv[1][0]) ? "*" : parv[1]);
		return;
	}

	/* if parv[1] isnt my client, or me, someone else is supposed
	 * to be handling the request.. so send it to them
	 */
	if(!MyClient(target_p) && !IsMe(target_p))
	{
		sendto_one(target_p, ":%s WHOIS %s :%s",
			   get_id(source_p, target_p),
			   get_id(target_p, target_p), parv[2]);
		return;
	}

	/* ok, the target is either us, or a client on our server, so perform the whois
	 * but first, parv[1] == server to perform the whois on, parv[2] == person
	 * to whois, so make parv[1] = parv[2] so do_whois is ok -- fl_
	 */
	parv[1] = parv[2];
	do_whois(client_p, source_p, parc, parv);
}

/* do_whois
 *
 * inputs	- pointer to
 * output	-
 * side effects -
 */
static void
do_whois(struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	struct Client *target_p;
	char *nick;
	char *p = NULL;
	int operspy = 0;

	nick = LOCAL_COPY(parv[1]);
	if((p = strchr(nick, ',')))
		*p = '\0';

	if(IsOperSpy(source_p) && *nick == '!')
	{
		operspy = 1;
		nick++;
	}

	target_p = find_named_person(nick);
	if(target_p != NULL)
	{
		if(operspy)
		{
			char buffer[BUFSIZE];

			snprintf(buffer, sizeof(buffer), "%s!%s@%s %s",
				target_p->name, target_p->username,
				target_p->host, target_p->servptr->name);
			report_operspy(source_p, "WHOIS", buffer);
		}

		single_whois(source_p, target_p, operspy);
	}
	else
		sendto_one_numeric(source_p, ERR_NOSUCHNICK,
				   form_str(ERR_NOSUCHNICK),
				   nick);

	sendto_one_numeric(source_p, RPL_ENDOFWHOIS,
			   form_str(RPL_ENDOFWHOIS), parv[1]);
}

/*
 * single_whois()
 *
 * Inputs	- source_p client to report to
 *		- target_p client to report on
 * Output	- if found return 1
 * Side Effects	- do a single whois on given client
 * 		  writing results to source_p
 */
static void
single_whois(struct Client *source_p, struct Client *target_p, int operspy)
{
	char buf[BUFSIZE];
	hook_data_client hdata;
	struct sockaddr_in ip4;

	if(target_p->user == NULL)
	{
		s_assert(0);
		return;
	}

	sendto_one_numeric(source_p, RPL_WHOISUSER, form_str(RPL_WHOISUSER),
			   target_p->name, target_p->username,
			   target_p->host, target_p->info);

	send_multiline_init(source_p, " ", form_str(RPL_WHOISCHANNELS),
				    get_id(&me, source_p), get_id(source_p, source_p),
				    target_p->name);

	/* Make sure it won't overflow when sending it to the client
	 * in full names; note that serverhiding may require more space
	 * for a different server name (not done here) -- jilles
	 */
	send_multiline_remote_pad(source_p, &me);
	send_multiline_remote_pad(source_p, source_p);

	hdata.client = source_p;
	hdata.target = target_p;

	if (!IsService(target_p))
	{
		hook_data_channel_visibility hdata_vis;
		rb_dlink_node *ps, *pt;
		struct Channel *chptr;
		struct membership *ms, *mt;

		hdata_vis.client = source_p;
		hdata_vis.target = target_p;

		ITER_COMM_CHANNELS(ps, pt, source_p->user->channel.head, target_p->user->channel.head, ms, mt, chptr)
		{
			if (mt == NULL)
				continue;

			hdata_vis.chptr = chptr;
			hdata_vis.clientms = ms;
			hdata_vis.targetms = mt;
			hdata_vis.approved = ms != NULL || PubChannel(chptr);

			call_hook(doing_whois_channel_visibility_hook, &hdata_vis);

			if(hdata_vis.approved || operspy)
			{
				send_multiline_item(source_p, "%s%s%s",
						hdata_vis.approved ? "" : "!",
						find_channel_status(mt, 1),
						chptr->chname);
			}
		}
	}

	send_multiline_fini(source_p, NULL);

	sendto_one_numeric(source_p, RPL_WHOISSERVER, form_str(RPL_WHOISSERVER),
			   target_p->name, target_p->servptr->name,
			   target_p->servptr->info);

	if(target_p->user->away)
		sendto_one_numeric(source_p, RPL_AWAY, form_str(RPL_AWAY),
				   target_p->name, target_p->user->away);

	char *s = NULL;
	if (IsService(target_p))
	{
		s = ConfigFileEntry.servicestring;
	}
	if (!EmptyString(target_p->user->opername) && IsOper(target_p))
	{
		if (target_p == source_p || HasPrivilege(source_p, "oper:privs"))
		{
			const char *privset = "(missing)";
			if (target_p->user->privset != NULL)
				privset = target_p->user->privset->name;
			snprintf(buf, sizeof buf, "is opered as %s, privset %s", target_p->user->opername, privset);
			s = buf;
		}
		else if (IsOper(source_p) && SeesOper(target_p, source_p))
		{
			snprintf(buf, sizeof buf, "is opered as %s", target_p->user->opername);
			s = buf;
		}
		else if (!ConfigFileEntry.hide_opers_in_whois && SeesOper(target_p, source_p))
		{
			s = IsAdmin(target_p) ? GlobalSetOptions.adminstring :
			    GlobalSetOptions.operstring;
		}
	}
	if (s != NULL)
	{
		sendto_one_numeric(source_p, RPL_WHOISOPERATOR, form_str(RPL_WHOISOPERATOR),
				target_p->name, s);
	}

	if(IsSecureClient(target_p))
	{
		char cbuf[256] = "is using a secure connection";

		if (MyClient(target_p) && target_p->localClient->cipher_string != NULL &&
				(!ConfigFileEntry.tls_ciphers_oper_only || source_p == target_p || IsOper(source_p)))
			rb_snprintf_append(cbuf, sizeof(cbuf), " [%s]", target_p->localClient->cipher_string);

		sendto_one_numeric(source_p, RPL_WHOISSECURE, form_str(RPL_WHOISSECURE),
				   target_p->name, cbuf);
		if((source_p == target_p || IsOperGeneral(source_p)) &&
				target_p->certfp != NULL)
			sendto_one_numeric(source_p, RPL_WHOISCERTFP,
					form_str(RPL_WHOISCERTFP),
					target_p->name, target_p->certfp);
	}

	if(MyClient(target_p))
	{
		if (IsDynSpoof(target_p) && (HasPrivilege(source_p, "auspex:hostname") || source_p == target_p))
		{
			/* trick here: show a nonoper their own IP if
			 * dynamic spoofed but not if auth{} spoofed
			 * -- jilles */
			ClearDynSpoof(target_p);
			sendto_one_numeric(source_p, RPL_WHOISHOST,
					   form_str(RPL_WHOISHOST),
					   target_p->name, target_p->orighost,
					   show_ip(source_p, target_p) ? target_p->sockhost : "255.255.255.255");
			SetDynSpoof(target_p);
		}
		else if(ConfigFileEntry.use_whois_actually && show_ip(source_p, target_p))
			sendto_one_numeric(source_p, RPL_WHOISACTUALLY,
					   form_str(RPL_WHOISACTUALLY),
					   target_p->name, target_p->sockhost);

		if (GET_SS_FAMILY(&target_p->localClient->ip) == AF_INET6 &&
				(show_ip(source_p, target_p) ||
				 (source_p == target_p && !IsIPSpoof(target_p))) &&
				rb_ipv4_from_ipv6((struct sockaddr_in6 *)&target_p->localClient->ip, &ip4))
		{
			rb_inet_ntop_sock((struct sockaddr *)&ip4,
					buf, sizeof buf);
			sendto_one_numeric(source_p, RPL_WHOISTEXT,
					"%s :Underlying IPv4 is %s",
					target_p->name, buf);
		}

		/* fire the doing_whois_show_idle hook to allow modules to tell us whether to show the idle time */
		hook_data_client_approval hdata_showidle;

		hdata_showidle.client = source_p;
		hdata_showidle.target = target_p;
		hdata_showidle.approved = WHOIS_IDLE_SHOW;

		call_hook(doing_whois_show_idle_hook, &hdata_showidle);

		sendto_one_numeric(source_p, RPL_WHOISIDLE, form_str(RPL_WHOISIDLE),
			   target_p->name,
			   hdata_showidle.approved ? (long)(rb_current_time() - target_p->localClient->last) : 0,
			   (unsigned long)target_p->localClient->firsttime);

		if (hdata_showidle.approved == WHOIS_IDLE_HIDE && (source_p->umodes & user_modes['I']))
			/* if the source has hidden their idle time, notify the source that they can't view others' idle times either */
			sendto_one_numeric(source_p, RPL_WHOISTEXT, form_str(RPL_WHOISTEXT), target_p->name, "has a hidden idle time because your own idle time is hidden");
		else if (hdata_showidle.approved != WHOIS_IDLE_SHOW)
			/* if the target has hidden their idle time, notify the source */
			sendto_one_numeric(source_p, RPL_WHOISTEXT, form_str(RPL_WHOISTEXT), target_p->name, "is hiding their idle time");
	}
	else
	{
		if (IsDynSpoof(target_p) && (HasPrivilege(source_p, "auspex:hostname") || source_p == target_p))
		{
			ClearDynSpoof(target_p);
			sendto_one_numeric(source_p, RPL_WHOISHOST,
					   form_str(RPL_WHOISHOST),
					   target_p->name, target_p->orighost,
					   show_ip(source_p, target_p) && !EmptyString(target_p->sockhost) && strcmp(target_p->sockhost, "0")? target_p->sockhost : "255.255.255.255");
			SetDynSpoof(target_p);
		}
		else if(ConfigFileEntry.use_whois_actually && show_ip(source_p, target_p) &&
		   !EmptyString(target_p->sockhost) && strcmp(target_p->sockhost, "0"))
		{
			sendto_one_numeric(source_p, RPL_WHOISACTUALLY,
					   form_str(RPL_WHOISACTUALLY),
					   target_p->name, target_p->sockhost);

		}
	}

	/* doing_whois_hook must only be called for local clients,
	 * doing_whois_global_hook must only be called for local targets
	 */
	/* it is important that these are called *before* RPL_ENDOFWHOIS is
	 * sent, services compatibility code depends on it. --anfl
	 */
	if(MyClient(source_p))
		call_hook(doing_whois_hook, &hdata);
	else
		call_hook(doing_whois_global_hook, &hdata);
}
