#include "Config.h"
#include "ScriptMgr.h"
#include "World.h"
#include "Chat.h"
#include "Player.h"
#include "AccountMgr.h"
#include "Log.h"
#include "Common.h"
#include "SharedDefines.h"
#include "ObjectAccessor.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include <unordered_set>

#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <cctype>

// =============================
// Locale přepínač (CZ/EN) – čte RealOnline.Locale (cs|en)
// =============================
enum class Lang { CS, EN };
static inline Lang LangOpt()
{
    std::string loc = sConfigMgr->GetOption<std::string>("RealOnline.Locale", "cs");
    std::transform(loc.begin(), loc.end(), loc.begin(), ::tolower);
    return (loc == "en" || loc == "english") ? Lang::EN : Lang::CS;
}
static inline char const* T(char const* cs, char const* en)
{
    return (LangOpt() == Lang::EN) ? en : cs;
}

// =============================
// Pomocné utility
// =============================
static inline std::string Trim(std::string s)
{
    auto notSpace = [](int ch){ return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

static const char* FactionNameFor(Player* p)
{
    switch (p->GetTeamId())
    {
        case TEAM_ALLIANCE: return T("Aliance", "Alliance");
        case TEAM_HORDE:    return T("Horda", "Horde");
        default:            return T("Neznámá", "Unknown");
    }
}

struct Range { uint32 min=0, max=0; };

static std::vector<Range> ParseRanges(std::string const& txt)
{
    std::vector<Range> out;
    std::stringstream ss(txt);
    std::string seg;
    while (std::getline(ss, seg, ';'))
    {
        seg = Trim(seg);
        if (seg.empty()) continue;
        auto dash = seg.find('-');
        if (dash == std::string::npos) continue;
        std::string a = Trim(seg.substr(0, dash));
        std::string b = Trim(seg.substr(dash + 1));
        if (a.empty() || b.empty()) continue;
        uint32 mn = uint32(std::stoul(a));
        uint32 mx = uint32(std::stoul(b));
        if (mn > mx) std::swap(mn, mx);
        out.push_back({mn, mx});
    }
    return out;
}

static bool InRanges(uint32 id, std::vector<Range> const& rs)
{
    for (auto const& r : rs)
        if (id >= r.min && id <= r.max)
            return true;
    return false;
}

// stránkování / rozsah A-B; výstup [begin, end) (EXCLUSIVE)
static bool ParsePageOrRange(char const* args, uint32 total, uint32 pageSize,
                             uint32& outBeginIndex, uint32& outEndIndex, std::string& err)
{
    outBeginIndex = 0; outEndIndex = 0;

    if (!args || !*args)
    {
        outBeginIndex = 0;
        outEndIndex   = std::min(pageSize, total);
        return true;
    }

    std::string s = Trim(args);
    auto dash = s.find('-');
    if (dash != std::string::npos)
    {
        std::string a = Trim(s.substr(0, dash));
        std::string b = Trim(s.substr(dash + 1));
        if (a.empty() || b.empty()) { err = T("Rozsah musí být ve tvaru A-B.", "Range must be in the form A-B."); return false; }
        if (!std::all_of(a.begin(), a.end(), ::isdigit) || !std::all_of(b.begin(), b.end(), ::isdigit))
        { err = T("Rozsah musí obsahovat pouze čísla.", "Range must contain digits only."); return false; }
        uint32 A = uint32(std::stoul(a));
        uint32 B = uint32(std::stoul(b));
        if (A == 0 || B == 0 || A > B) { err = T("Rozsah musí být A-B, A>=1, B>=A.", "Range must be A-B, A>=1, B>=A."); return false; }
        if (A > total) { err = T("Začátek rozsahu je mimo počet online hráčů.", "Range start is beyond online player count."); return false; }
        outBeginIndex = A - 1;
        outEndIndex   = std::min(B, total);
        return true;
    }

    if (!std::all_of(s.begin(), s.end(), ::isdigit))
    { err = T("Očekávám číslo stránky nebo rozsah A-B.", "Expecting page number or A-B range."); return false; }

    uint32 page = uint32(std::stoul(s));
    if (page == 0) { err = T("Číslo stránky začíná od 1.", "Page number starts at 1."); return false; }

    uint32 pages = (total + pageSize - 1) / pageSize;
    if (pages == 0) pages = 1;
    if (page > pages)
    {
        err = (LangOpt()==Lang::EN)
            ? ("Requested page does not exist. Total pages: " + std::to_string(pages) + ".")
            : ("Požadovaná stránka neexistuje. Celkem dostupných stránek: " + std::to_string(pages) + ".");
        return false;
    }

    outBeginIndex = (page - 1) * pageSize;
    outEndIndex   = std::min(outBeginIndex + pageSize, total);
    return true;
}


// =============================
// Detekce nového vs. starého chat API
// =============================
#if __has_include("Chat/ChatCommands/ChatCommand.h")
  #define AC_HAS_NEW_CHAT_API 1
  #include "Chat/ChatCommands/ChatCommand.h"
  using namespace Acore::ChatCommands;
#endif

// =============================
// Režimy výpisu
// =============================
enum class RealOnlineMode { AccountId, Session };

static RealOnlineMode GetMode()
{
    std::string m = sConfigMgr->GetOption<std::string>("RealOnline.Mode", "accountid");
    if (!m.empty())
    {
        std::transform(m.begin(), m.end(), m.begin(), ::tolower);
        if (m == "session")
            return RealOnlineMode::Session;
    }
    return RealOnlineMode::AccountId;
}

static void BuildViaSessions(std::vector<Player*>& out, bool hideGMs, uint32 minLevel)
{
    auto const& sessions = sWorldSessionMgr->GetAllSessions();
    out.reserve(sessions.size());

    for (auto const& [accId, sess] : sessions)
    {
        if (!sess) continue;
        Player* p = sess->GetPlayer();
        if (!p || !p->IsInWorld()) continue;

        if (hideGMs && p->IsGameMaster())
            continue;
        if (minLevel > 0 && p->GetLevel() < minLevel)
            continue;

        out.push_back(p);
    }
}

static void BuildViaAccountId(std::vector<Player*>& out, bool hideGMs, uint32 minLevel,
                              std::vector<Range> const& ignoreAccRanges)
{
    auto const& players = ObjectAccessor::GetPlayers();
    out.reserve(players.size());
    for (auto const& it : players)
    {
        Player* p = it.second;
        if (!p || !p->IsInWorld())
            continue;

        if (hideGMs && p->IsGameMaster())
            continue;
        if (minLevel > 0 && p->GetLevel() < minLevel)
            continue;

        WorldSession* sess = p->GetSession();
        if (!ignoreAccRanges.empty() && sess)
        {
            uint32 accId = sess->GetAccountId();
            if (InRanges(accId, ignoreAccRanges))
                continue;
        }

        out.push_back(p);
    }
}

class RealOnlineCommand : public CommandScript {
public:
    RealOnlineCommand() : CommandScript("RealOnlineCommand") {}

#ifdef AC_HAS_NEW_CHAT_API
    ChatCommandTable GetCommands() const override {
        static ChatCommandTable cmds = {
            { "online", HandleOnline, SEC_PLAYER, Console::No }
        };
        return cmds;
    }
#else
    std::vector<ChatCommand> GetCommands() const override {
        static std::vector<ChatCommand> cmds;
        cmds.push_back({ "online", SEC_PLAYER, false, &HandleOnline, "" });
        return cmds;
    }
#endif

    static bool HandleOnline(ChatHandler* handler, char const* args)
    {
        bool   showLevel = sConfigMgr->GetOption<bool>("RealOnline.ShowLevel", true);
        bool   hideGMs   = sConfigMgr->GetOption<bool>("RealOnline.HideGMs", false);
        uint32 pageSize  = sConfigMgr->GetOption<uint32>("RealOnline.PageSize", 10u);
        uint32 minLevel  = sConfigMgr->GetOption<uint32>("RealOnline.MinLevel", 0u);

        if (pageSize == 0) pageSize = 10;

		std::vector<Player*> list;
		BuildViaSessions(list, hideGMs, minLevel);
		
		std::sort(list.begin(), list.end(),
				[](Player* a, Player* b){ return a->GetName() < b->GetName(); });
		
		uint32 total = uint32(list.size());
		
		uint32 beginIndex = 0, endIndex = 0;
		std::string err;
		if (!ParsePageOrRange(args, total, pageSize, beginIndex, endIndex, err))
		{
			handler->SendSysMessage(err.c_str());
			return true;
		}
		
		uint32 pages = (total + pageSize - 1) / pageSize;
		if (pages == 0) pages = 1;
		
		bool lookedLikeRange = (args && std::string(args).find('-') != std::string::npos);
		std::ostringstream head;
		if (!lookedLikeRange)
		{
			uint32 page = (pageSize == 0) ? 1 : (beginIndex / pageSize + 1);
			head << (LangOpt()==Lang::EN ? "Real players online: " : "Skuteční hráči online: ") << total
				<< (LangOpt()==Lang::EN ? " (page " : " (stránka ") << page << "/" << pages
				<< (LangOpt()==Lang::EN ? ", " : ", ")
				<< pageSize << (LangOpt()==Lang::EN ? " per page)" : " na stránku)");
		}
		else
		{
			head << (LangOpt()==Lang::EN ? "Real players online: " : "Skuteční hráči online: ") << total
				<< (LangOpt()==Lang::EN ? " (range " : " (rozsah ") << (beginIndex + 1) << "-" << endIndex << ")";
		}
		handler->SendSysMessage(head.str().c_str());
		
		std::ostringstream out;
		for (uint32 i = beginIndex; i < endIndex; ++i)
		{
			Player* p = list[i];
			out << p->GetName();
			if (showLevel)
				out << " [lvl " << uint32(p->GetLevel()) << "]";
			out << " - " << FactionNameFor(p) << "\n";
		}
		handler->SendSysMessage(out.str().c_str());
		return true;
	}
};

// =============================
// ==== NAVAZUJÍCÍ REWARD LOGIKA ====
// =============================

struct RewardCfg
{
    bool   enable = false;
    uint32 itemId = 0;
    uint32 intervalMs = 60000;
    uint32 minLevel = 0;
};

static inline uint32 ReadIntervalMs()
{
    std::string unit = sConfigMgr->GetOption<std::string>("RealOnline.Reward.IntervalUnit", "minute");
    uint32 count = sConfigMgr->GetOption<uint32>("RealOnline.Reward.IntervalCount", 1u);
    if (count == 0) count = 1;

    std::transform(unit.begin(), unit.end(), unit.begin(), ::tolower);

    uint64 baseMs = 60000;
    if (unit == "hour" || unit == "hours")
        baseMs = 3600000;

    uint64 total = baseMs * uint64(count);
    if (total > UINT32_MAX) total = UINT32_MAX;
    return uint32(total);
}

static RewardCfg GetRewardCfg()
{
    RewardCfg c;
    c.enable     = sConfigMgr->GetOption<bool>("RealOnline.Reward.Enable", false);
    c.itemId     = sConfigMgr->GetOption<uint32>("RealOnline.Reward.ItemId", 0u);
    c.intervalMs = ReadIntervalMs();
    c.minLevel   = sConfigMgr->GetOption<uint32>("RealOnline.Reward.MinLevel", 0u);
    return c;
}

static void CollectOnlineRealAccountIds(std::vector<uint32>& out, bool hideGMs, uint32 minLevel)
{
    out.clear();

    std::vector<Player*> list;
    BuildViaSessions(list, hideGMs, minLevel);

    std::vector<Range> blockedRanges = ParseRanges(
        sConfigMgr->GetOption<std::string>("RealOnline.IgnoreAccountIdRanges", "")
    );

    std::unordered_set<uint32> uniq;
    uniq.reserve(list.size() * 2 + 8);

    for (Player* p : list)
    {
        if (!p || !p->IsInWorld())
            continue;
        if (hideGMs && p->IsGameMaster())
            continue;
        if (minLevel > 0 && p->GetLevel() < minLevel)
            continue;

        if (WorldSession* s = p->GetSession())
        {
            uint32 acc = s->GetAccountId();

            if (!blockedRanges.empty() && InRanges(acc, blockedRanges))
                continue;

            if (uniq.insert(acc).second)
                out.push_back(acc);
        }
    }
}


class RealOnlineRewardTicker : public WorldScript
{
public:
    RealOnlineRewardTicker() : WorldScript("RealOnlineRewardTicker") {}

    void OnUpdate(uint32 diff) override
    {
        RewardCfg cfg = GetRewardCfg();
        if (!cfg.enable || cfg.itemId == 0)
            return;

        _elapsed += diff;
        if (_elapsed < cfg.intervalMs)
            return;

        _elapsed = 0;

        std::vector<uint32> accounts;
        CollectOnlineRealAccountIds(accounts,
            sConfigMgr->GetOption<bool>("RealOnline.HideGMs", false),
            std::max(cfg.minLevel, sConfigMgr->GetOption<uint32>("RealOnline.MinLevel", 0u))
        );

        if (accounts.empty())
            return;

        for (uint32 acc : accounts)
        {
			std::string q =
				"INSERT INTO customs.rewards (`account`,`item`,`entitled`,`claimed`,`stored`) "
				"VALUES (" + std::to_string(acc) + "," + std::to_string(cfg.itemId) + ",1,0,0) "
				"ON DUPLICATE KEY UPDATE `entitled` = `entitled` + 1, updated_at = NOW()";
            CharacterDatabase.DirectExecute(q.c_str());
        }
    }

private:
    uint32 _elapsed = 0;
};

class RewardCommand : public CommandScript
{
public:
    RewardCommand() : CommandScript("RewardCommand") {}

#ifdef AC_HAS_NEW_CHAT_API
    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable table =
        {
            { "reward", HandleReward, SEC_PLAYER, Console::No }
        };
        return table;
    }
#else
    std::vector<ChatCommand> GetCommands() const override
    {
        static std::vector<ChatCommand> cmds;
        cmds.push_back({ "reward", SEC_PLAYER, false, &HandleReward, "" });
        return cmds;
    }
#endif

    static bool HandleReward(ChatHandler* handler, char const* args)
    {
        Player* plr = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!plr)
            return true;

        RewardCfg cfg = GetRewardCfg();
        if (!cfg.enable || cfg.itemId == 0)
        {
            handler->SendSysMessage(T("Reward system je vypnutý.", "Reward system is disabled."));
            return true;
        }

        std::string sub = args ? Trim(args) : "";
        std::transform(sub.begin(), sub.end(), sub.begin(), ::tolower);

        uint32 acc = handler->GetSession()->GetAccountId();

        uint32 entitled = 0, claimed = 0;
        {
            std::string q =
                "SELECT entitled, claimed FROM customs.rewards "
                "WHERE account=" + std::to_string(acc) +
                " AND item=" + std::to_string(cfg.itemId) + " LIMIT 1";
            if (QueryResult res = CharacterDatabase.Query(q.c_str()))
            {
                Field* f = res->Fetch();
                entitled = f[0].Get<uint32>();
                claimed  = f[1].Get<uint32>();
            }
        }

        uint32 available = (entitled > claimed) ? (entitled - claimed) : 0;

        if (sub.empty())
        {
            std::ostringstream msg;
            if (LangOpt()==Lang::EN)
            {
                msg << "Total earned: " << entitled
                    << " | Total claimed: " << claimed
                    << " | Available: " << available;
                handler->SendSysMessage(msg.str().c_str());
                handler->SendSysMessage("Type \".reward claim\" to collect your reward.");
            }
            else
            {
                msg << "Celkem získáno: " << entitled
                    << " | Celkem vyzvednuto: " << claimed
                    << " | K dispozici: " << available;
                handler->SendSysMessage(msg.str().c_str());
                handler->SendSysMessage("Napiš \".reward claim\" pro výběr odměny.");
            }
            return true;
        }

        if (sub == "claim")
        {
            if (available == 0)
            {
                handler->SendSysMessage(T("Nemáš nic k výběru.", "You have nothing to claim."));
                return true;
            }

            uint32 countToGive = available;

            ItemPosCountVec dest;
            InventoryResult canStore = plr->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, cfg.itemId, countToGive);
            if (canStore != EQUIP_ERR_OK)
            {
                handler->SendSysMessage(T(
                    "Nemáš dost místa v taškách (výběr zrušen). Uvolni místo a zkus znovu.",
                    "Not enough bag space (claim canceled). Free up space and try again."
                ));
                return true;
            }

            if (Item* it = plr->StoreNewItem(dest, cfg.itemId, true, Item::GenerateItemRandomPropertyId(cfg.itemId)))
            {
                plr->SendNewItem(it, countToGive, true, false);

                std::string up =
					"INSERT INTO customs.rewards (`account`,`item`,`entitled`,`claimed`,`stored`) "
					"VALUES (" + std::to_string(acc) + "," + std::to_string(cfg.itemId) + ",0," + std::to_string(countToGive) + ",0) "
					"ON DUPLICATE KEY UPDATE `claimed` = `claimed` + VALUES(`claimed`), updated_at = NOW()";
                CharacterDatabase.DirectExecute(up.c_str());

                std::ostringstream ok;
                ok << (LangOpt()==Lang::EN ? "Claimed: Mystery Token " : "Vybráno: Mystery Token ")
                   << countToGive << (LangOpt()==Lang::EN ? " pcs" : "ks");
                handler->SendSysMessage(ok.str().c_str());
            }
            else
            {
                handler->SendSysMessage(T("Chyba při ukládání itemu do inventáře.", "Error storing item in inventory."));
            }

            return true;
        }

        handler->SendSysMessage(T("Neznámý parametr. Použij \".reward\" nebo \".reward claim\".",
                                  "Unknown parameter. Use \".reward\" or \".reward claim\"."));
        return true;
    }
};

// =============================
// ==== TOKEN BANK – .token příkazy ====
// =============================
class TokenBankCommand : public CommandScript
{
public:
    TokenBankCommand() : CommandScript("TokenBankCommand") {}

#ifdef AC_HAS_NEW_CHAT_API
    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable table =
        {
            { "token", HandleToken, SEC_PLAYER, Console::No }
        };
        return table;
    }
#else
    std::vector<ChatCommand> GetCommands() const override
    {
        static std::vector<ChatCommand> cmds;
        cmds.push_back({ "token", SEC_PLAYER, false, &HandleToken, "" });
        return cmds;
    }
#endif

    static uint32 ReadStored(uint32 acc, uint32 itemId)
	{
		std::string q = "SELECT `stored` FROM customs.rewards WHERE account="
					+ std::to_string(acc) + " AND item=" + std::to_string(itemId) + " LIMIT 1";
		if (QueryResult r = CharacterDatabase.Query(q.c_str()))
			return r->Fetch()[0].Get<uint32>();
		return 0;
	}

    static void UpsertAddStored(uint32 acc, uint32 itemId, uint32 add)
	{
		std::string up =
			"INSERT INTO customs.rewards (`account`,`item`,`entitled`,`claimed`,`stored`) VALUES ("
			+ std::to_string(acc) + "," + std::to_string(itemId) + ",0,0," + std::to_string(add) + ") "
			"ON DUPLICATE KEY UPDATE `stored` = `stored` + VALUES(`stored`), updated_at = NOW()";
		CharacterDatabase.DirectExecute(up.c_str());
	}


    static bool HandleToken(ChatHandler* handler, char const* args)
    {
        Player* plr = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!plr)
            return true;

        RewardCfg cfg = GetRewardCfg();
        if (!cfg.enable || cfg.itemId == 0)
        {
            handler->SendSysMessage(T("Reward system je vypnutý.", "Reward system is disabled."));
            return true;
        }

        uint32 acc = handler->GetSession()->GetAccountId();
        std::string sub = args ? Trim(args) : "";
        std::transform(sub.begin(), sub.end(), sub.begin(), ::tolower);

        auto showHelp = [&](){
			uint32 stored = ReadStored(acc, cfg.itemId);
		
			if (LangOpt()==Lang::EN)
			{
				std::ostringstream ss;
				ss << "Stored tokens: " << stored;
				handler->SendSysMessage(ss.str().c_str());
			}
			else
			{	
				std::ostringstream ss;
				ss << "Uskladněné tokeny: " << stored;
				handler->SendSysMessage(ss.str().c_str());
			}
		};


        if (sub.empty())
        {
            showHelp();
            return true;
        }

        std::string cmd, num;
        {
            std::stringstream ss(sub);
            ss >> cmd;
            ss >> num;
        }

        auto parseCount = [&](std::string const& s, uint32& out)->bool{
            if (s.empty() || !std::all_of(s.begin(), s.end(), ::isdigit))
                return false;
            uint64 v = 0;
            try { v = std::stoull(s); } catch (...) { return false; }
            if (v == 0 || v > UINT32_MAX) return false;
            out = static_cast<uint32>(v);
            return true;
        };

        if (cmd == "deposit")
        {
            uint32 amount = 0;
            if (!parseCount(num, amount))
            {
                handler->SendSysMessage(T("Zadej kladný počet: .token deposit <pocet>", "Enter a positive number: .token deposit <count>"));
                return true;
            }

            uint32 have = plr->GetItemCount(cfg.itemId, true);
            if (have < amount)
            {
                std::ostringstream ss;
                if (LangOpt()==Lang::EN)
                    ss << "Not enough tokens in your bags. You have " << have << ".";
                else
                    ss << "Nemáš dost tokenů v taškách. Máš " << have << ".";
                handler->SendSysMessage(ss.str().c_str());
                return true;
            }

            plr->DestroyItemCount(cfg.itemId, amount, true, false);

            UpsertAddStored(acc, cfg.itemId, amount);

            std::ostringstream ok;
            if (LangOpt()==Lang::EN)
                ok << "Deposited " << amount << " token(s) to storage.";
            else
                ok << "Uloženo " << amount << " tokenů do úschovy.";
            handler->SendSysMessage(ok.str().c_str());
            return true;
        }
        else if (cmd == "withdraw")
        {
            uint32 amount = 0;
            if (!parseCount(num, amount))
            {
                handler->SendSysMessage(T("Zadej kladný počet: .token withdraw <pocet>", "Enter a positive number: .token withdraw <count>"));
                return true;
            }

            uint32 stored = ReadStored(acc, cfg.itemId);
            if (stored < amount)
            {
                std::ostringstream ss;
                if (LangOpt()==Lang::EN)
                    ss << "Not enough stored tokens. You have " << stored << ".";
                else
                    ss << "Nemáš dost uskladněných tokenů. Máš " << stored << ".";
                handler->SendSysMessage(ss.str().c_str());
                return true;
            }

            ItemPosCountVec dest;
            InventoryResult canStore = plr->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, cfg.itemId, amount);
            if (canStore != EQUIP_ERR_OK)
            {
                handler->SendSysMessage(T(
                    "Nemáš dost místa v taškách. Uvolni místo a zkus znovu.",
                    "Not enough bag space. Free up space and try again."
                ));
                return true;
            }

            if (Item* it = plr->StoreNewItem(dest, cfg.itemId, true, Item::GenerateItemRandomPropertyId(cfg.itemId)))
            {
                plr->SendNewItem(it, amount, true, false);

                std::string up = "UPDATE customs.rewards SET `stored` = `stored` - " + std::to_string(amount)
							   + ", updated_at = NOW() WHERE account=" + std::to_string(acc)
							   + " AND item=" + std::to_string(cfg.itemId) + " AND `stored` >= " + std::to_string(amount);
					CharacterDatabase.DirectExecute(up.c_str());

                std::ostringstream ok;
                if (LangOpt()==Lang::EN)
                    ok << "Withdrew " << amount << " token(s) from storage.";
                else
                    ok << "Vybráno " << amount << " tokenů z úschovy.";
                handler->SendSysMessage(ok.str().c_str());
            }
            else
            {
                handler->SendSysMessage(T("Chyba při ukládání itemu do inventáře.", "Error storing item in inventory."));
            }

            return true;
        }

        if (LangOpt()==Lang::EN)
            handler->SendSysMessage("Unknown parameter. Use \".token\", \".token deposit <count>\", or \".token withdraw <count>\".");
        else
            handler->SendSysMessage("Neznámý parametr. Použij \".token\", \".token deposit <pocet>\", nebo \".token withdraw <pocet>\".");
        return true;
    }
};

void Addmod_token_level_milestonesScripts();
void Addmod_token_login_streakScripts();

void Addmod_real_onlineScripts()
{
    new RealOnlineCommand();
    new RealOnlineRewardTicker();
    new RewardCommand();
    new TokenBankCommand();

    Addmod_token_level_milestonesScripts();
    Addmod_token_login_streakScripts();
}
