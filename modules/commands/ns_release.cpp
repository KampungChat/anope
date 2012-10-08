/* NickServ core functions
 *
 * (C) 2003-2012 Anope Team
 * Contact us at team@anope.org
 *
 * Please read COPYING and README for further details.
 *
 * Based on the original code of Epona by Lara.
 * Based on the original code of Services by Andy Church.
 */

/*************************************************************************/

#include "module.h"

class NSReleaseRequest : public IdentifyRequest
{
	CommandSource source;
	Command *cmd;
	dynamic_reference<NickAlias> na;
 
 public:
	NSReleaseRequest(CommandSource &src, Command *c, NickAlias *n, const Anope::string &pass) : IdentifyRequest(n->nc->display, pass), source(src), cmd(c), na(n) { }

	void OnSuccess() anope_override
	{
		if (!source.GetUser() || !na)
			return;

		bool override = source.GetAccount() != na->nc && source.HasPriv("nickserv/release");
		Log(override ? LOG_OVERRIDE : LOG_COMMAND, source, cmd) << "for nickname " << na->nick;
		na->Release();
		source.Reply(_("Services' hold on \002%s\002 has been released."), na->nick.c_str());
	}

	void OnFail() anope_override
	{
		source.Reply(ACCESS_DENIED);
		if (!GetPassword().empty())
		{
			Log(LOG_COMMAND, source, cmd) << "with invalid password for " << na->nick;
			if (!source.GetUser())
				bad_password(source.GetUser());
		}
	}
};

class CommandNSRelease : public Command
{
 public:
	CommandNSRelease(Module *creator) : Command(creator, "nickserv/release", 1, 2)
	{
		this->SetFlag(CFLAG_ALLOW_UNREGISTERED);
		this->SetDesc(_("Regain custody of your nick after RECOVER"));
		this->SetSyntax(_("\037nickname\037 [\037password\037]"));
	}

	void Execute(CommandSource &source, const std::vector<Anope::string> &params) anope_override
	{
		const Anope::string &nick = params[0];
		Anope::string pass = params.size() > 1 ? params[1] : "";
		NickAlias *na;

		if (!(na = findnick(nick)))
			source.Reply(NICK_X_NOT_REGISTERED, nick.c_str());
		else if (na->nc->HasFlag(NI_SUSPENDED))
			source.Reply(NICK_X_SUSPENDED, na->nick.c_str());
		else if (!na->HasFlag(NS_HELD))
			source.Reply(_("Nick \002%s\002 isn't being held."), nick.c_str());
		else
		{
			bool override = source.GetAccount() != na->nc && source.HasPriv("nickserv/release");

			bool ok = override;
			if (source.GetAccount() == na->nc)
				ok = true;
			else if (source.GetUser() && !na->nc->HasFlag(NI_SECURE) && is_on_access(source.GetUser(), na->nc))
				ok = true;
			else if (source.GetUser() && !source.GetUser()->fingerprint.empty() && na->nc->FindCert(source.GetUser()->fingerprint))
				ok = true;

			if (ok == false && !pass.empty())
			{
				NSReleaseRequest *req = new NSReleaseRequest(source, this, na, pass);
				FOREACH_MOD(I_OnCheckAuthentication, OnCheckAuthentication(source.GetUser(), req));
				req->Dispatch();
			}
			else
			{
				NSReleaseRequest req(source, this, na, pass);

				if (ok)
					req.OnSuccess();
				else
					req.OnFail();
			}
		}
		return;
	}

	bool OnHelp(CommandSource &source, const Anope::string &subcommand) anope_override
	{
		/* Convert Config->NSReleaseTimeout seconds to string format */
		Anope::string relstr = duration(Config->NSReleaseTimeout);

		this->SendSyntax(source);
		source.Reply(" ");
		source.Reply(_("Instructs %s to remove any hold on your nickname\n"
				"caused by automatic kill protection or use of the \002RECOVER\002\n"
				"command. This holds lasts for %s;\n"
				"This command gets rid of them sooner.\n"
				" \n"
				"In order to use the \002RELEASE\002 command for a nick, your\n"
				"current address as shown in /WHOIS must be on that nick's\n"
				"access list, you must be identified and in the group of\n"
				"that nick, or you must supply the correct password for\n"
				"the nickname."), Config->NickServ.c_str(), relstr.c_str());


		return true;
	}
};

class NSRelease : public Module
{
	CommandNSRelease commandnsrelease;

 public:
	NSRelease(const Anope::string &modname, const Anope::string &creator) : Module(modname, creator, CORE),
		commandnsrelease(this)
	{
		this->SetAuthor("Anope");

		if (Config->NoNicknameOwnership)
			throw ModuleException(modname + " can not be used with options:nonicknameownership enabled");
	}
};

MODULE_INIT(NSRelease)
