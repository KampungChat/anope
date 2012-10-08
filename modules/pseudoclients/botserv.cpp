/* BotServ core functions
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

class BotServCore : public Module
{
 public:
	BotServCore(const Anope::string &modname, const Anope::string &creator) : Module(modname, creator, CORE)
	{
		this->SetAuthor("Anope");

		const BotInfo *BotServ = findbot(Config->BotServ);
		if (BotServ == NULL)
			throw ModuleException("No bot named " + Config->BotServ);

		Implementation i[] = { I_OnPrivmsg, I_OnJoinChannel, I_OnLeaveChannel,
					I_OnPreHelp, I_OnPostHelp, I_OnChannelModeSet };
		ModuleManager::Attach(i, this, sizeof(i) / sizeof(Implementation));

	}

	void OnPrivmsg(User *u, Channel *c, Anope::string &msg) anope_override
	{
		if (!u || !c || !c->ci || !c->ci->bi || msg.empty())
			return;

		/* Answer to ping if needed */
		if (msg.substr(0, 6).equals_ci("\1PING ") && msg[msg.length() - 1] == '\1')
		{
			Anope::string ctcp = msg;
			ctcp.erase(ctcp.begin());
			ctcp.erase(ctcp.length() - 1);
			ircdproto->SendCTCP(c->ci->bi, u->nick, "%s", ctcp.c_str());
		}

		Anope::string realbuf = msg;

		bool was_action = false;
		if (realbuf.substr(0, 8).equals_ci("\1ACTION ") && realbuf[realbuf.length() - 1] == '\1')
		{
			realbuf.erase(0, 8);
			realbuf.erase(realbuf.length() - 1);
			was_action = true;
		}
	
		if (realbuf.empty())
			return;

		/* Fantaisist commands */
		if (c->ci->botflags.HasFlag(BS_FANTASY) && (Config->BSFantasyCharacter.empty() || realbuf.find_first_of(Config->BSFantasyCharacter) == 0) && !was_action)
		{
			/* Strip off the fantasy character */
			if (!Config->BSFantasyCharacter.empty())
				realbuf.erase(realbuf.begin());

			std::vector<Anope::string> params = BuildStringVector(realbuf);

			ServerConfig::fantasy_map::const_iterator it = Config->Fantasy.end();
			unsigned count = 0;
			for (unsigned max = params.size(); it == Config->Fantasy.end() && max > 0; --max)
			{
				Anope::string full_command;
				for (unsigned i = 0; i < max; ++i)
					full_command += " " + params[i];
				full_command.erase(full_command.begin());

				++count;
				it = Config->Fantasy.find(full_command);
			}

			if (it == Config->Fantasy.end())
				return;

			const CommandInfo &info = it->second;
			service_reference<Command> cmd("Command", info.name);
			if (!cmd)
			{
				Log(LOG_DEBUG) << "Fantasy command " << it->first << " exists for nonexistant service " << info.name << "!";
				return;
			}

			for (unsigned i = 0, j = params.size() - (count - 1); i < j; ++i)
				params.erase(params.begin());

			while (cmd->MaxParams > 0 && params.size() > cmd->MaxParams)
			{
				params[cmd->MaxParams - 1] += " " + params[cmd->MaxParams];
				params.erase(params.begin() + cmd->MaxParams);
			}

			/* All ChanServ commands take the channel as a first parameter */
			if (cmd->name.find("chanserv/") == 0)
				params.insert(params.begin(), c->ci->name);

			// Command requires registered users only
			if (!cmd->HasFlag(CFLAG_ALLOW_UNREGISTERED) && !u->Account())
				return;

			if (params.size() < cmd->MinParams)
				return;

			CommandSource source(u->nick, u, u->Account(), u);
			source.c = c;
			source.owner = c->ci->bi;
			source.service = c->ci->bi;
			source.command = it->first;
			source.permission = info.permission;

			EventReturn MOD_RESULT;
			if (c->ci->AccessFor(u).HasPriv("FANTASIA"))
			{
				FOREACH_RESULT(I_OnBotFantasy, OnBotFantasy(source, cmd, c->ci, params));
			}
			else
			{
				FOREACH_RESULT(I_OnBotNoFantasyAccess, OnBotNoFantasyAccess(source, cmd, c->ci, params));
			}

			if (MOD_RESULT == EVENT_STOP || !c->ci->AccessFor(u).HasPriv("FANTASIA"))
				return;

			if (MOD_RESULT != EVENT_ALLOW && !info.permission.empty() && !source.HasCommand(info.permission))
				return;

			FOREACH_RESULT(I_OnPreCommand, OnPreCommand(source, cmd, params));
			if (MOD_RESULT == EVENT_STOP)
				return;

			dynamic_reference<User> user_reference(u);
			dynamic_reference<NickCore> nc_reference(u->Account());
			cmd->Execute(source, params);

			if (user_reference && nc_reference)
			{
				FOREACH_MOD(I_OnPostCommand, OnPostCommand(source, cmd, params));
			}
		}
	}

	void OnJoinChannel(User *user, Channel *c) anope_override
	{
		if (c->ci && c->ci->bi)
		{
			/**
			 * We let the bot join even if it was an ignored user, as if we don't,
			 * and the ignored user doesnt just leave, the bot will never
			 * make it into the channel, leaving the channel botless even for
			 * legit users - Rob
			 **/
			if (c->users.size() >= Config->BSMinUsers && !c->FindUser(c->ci->bi))
				c->ci->bi->Join(c, &Config->BotModeList);
			/* Only display the greet if the main uplink we're connected
			 * to has synced, or we'll get greet-floods when the net
			 * recovers from a netsplit. -GD
			 */
			if (c->FindUser(c->ci->bi) && c->ci->botflags.HasFlag(BS_GREET) && user->Account() && !user->Account()->greet.empty() && c->ci->AccessFor(user).HasPriv("GREET") && user->server->IsSynced())
			{
				ircdproto->SendPrivmsg(c->ci->bi, c->name, "[%s] %s", user->Account()->display.c_str(), user->Account()->greet.c_str());
				c->ci->bi->lastmsg = Anope::CurTime;
			}
		}
	}

	void OnLeaveChannel(User *u, Channel *c) anope_override
	{
		/* Channel is persistent, it shouldn't be deleted and the service bot should stay */
		if (c->HasFlag(CH_PERSIST) || (c->ci && c->ci->HasFlag(CI_PERSIST)))
			return;
	
		/* Channel is syncing from a netburst, don't destroy it as more users are probably wanting to join immediatly
		 * We also don't part the bot here either, if necessary we will part it after the sync
		 */
		if (c->HasFlag(CH_SYNCING))
			return;

		/* Additionally, do not delete this channel if ChanServ/a BotServ bot is inhabiting it */
		if (c->HasFlag(CH_INHABIT))
			return;

		if (c->ci && c->ci->bi && u != *c->ci->bi && c->users.size() - 1 <= Config->BSMinUsers && c->FindUser(c->ci->bi))
		{
			bool persist = c->HasFlag(CH_PERSIST);
			c->SetFlag(CH_PERSIST);
			c->ci->bi->Part(c->ci->c);
			if (!persist)
				c->UnsetFlag(CH_PERSIST);
		}
	}

	EventReturn OnPreHelp(CommandSource &source, const std::vector<Anope::string> &params) anope_override
	{
		if (!params.empty() || source.owner->nick != Config->BotServ)
			return EVENT_CONTINUE;
		source.Reply(_("\002%s\002 allows you to have a bot on your own channel.\n"
			"It has been created for users that can't host or\n"
			"configure a bot, or for use on networks that don't\n"
			"allow user bots. Available commands are listed \n"
			"below; to use them, type \002%s%s \037command\037\002. For\n"
			"more information on a specific command, type\n"
			"\002%s%s %s \037command\037\002.\n "),
			Config->BotServ.c_str(), Config->UseStrictPrivMsgString.c_str(), Config->BotServ.c_str(),
			Config->UseStrictPrivMsgString.c_str(), Config->BotServ.c_str(), source.command.c_str());
		return EVENT_CONTINUE;
	}

	void OnPostHelp(CommandSource &source, const std::vector<Anope::string> &params) anope_override
	{
		if (!params.empty() || source.owner->nick != Config->BotServ)
			return;
		source.Reply(_(" \n"
			"Bot will join a channel whenever there is at least\n"
			"\002%d\002 user(s) on it."), Config->BSMinUsers);
		if (!Config->BSFantasyCharacter.empty())
			source.Reply(_("Additionally, all %s commands can be used if fantasy\n"
				"is enabled by prefixing the command name with one of\n"
				"the following characters: %s"), Config->ChanServ.c_str(), Config->BSFantasyCharacter.c_str());
	}

	EventReturn OnChannelModeSet(Channel *c, MessageSource &, ChannelModeName Name, const Anope::string &param) anope_override
	{
		if (Config->BSSmartJoin && Name == CMODE_BAN && c->ci && c->ci->bi && c->FindUser(c->ci->bi))
		{
			BotInfo *bi = c->ci->bi;

			Entry ban(CMODE_BAN, param);
			if (ban.Matches(bi))
				c->RemoveMode(bi, CMODE_BAN, param);
		}

		return EVENT_CONTINUE;
	}
};

MODULE_INIT(BotServCore)

