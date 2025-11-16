#include "Config.h"
#include "ScriptMgr.h"
#include "Player.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "WorldSession.h"
#include "Log.h"
#include <algorithm>
#include <string>
#include <vector>
#include <sstream>

// ==== Locale přepínač (CZ/EN) – čte RealOnline.Locale (cs|en) ====
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

// ==== utils ====
static std::string Trim(std::string s)
{
    auto notSpace = [](int ch){ return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

static std::vector<uint32> ParseCSVu32(std::string const& s)
{
    std::vector<uint32> out;
    std::stringstream ss(s);
    std::string seg;
    while (std::getline(ss, seg, ','))
    {
        seg = Trim(seg);
        if (seg.empty()) continue;
        try { out.push_back(static_cast<uint32>(std::stoul(seg))); } catch (...) {}
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// ==== range parser pro blokaci účtů (A-B;C-D;...) ====
struct Range { uint32 min = 0, max = 0; };

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
        uint32 mn = 0, mx = 0;
        try { mn = static_cast<uint32>(std::stoul(a)); mx = static_cast<uint32>(std::stoul(b)); } catch (...) { continue; }
        if (mn > mx) std::swap(mn, mx);
        out.push_back({ mn, mx });
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

static bool DeliverRewardToPlayerOrEntitlement(Player* plr, uint32 accountId, uint32 itemId, uint32 count, std::string const& deliveryMode)
{
    std::string mode = deliveryMode;
    std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);

    if (mode == "inventory")
    {
        ItemPosCountVec dest;
        if (plr->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count) == EQUIP_ERR_OK)
        {
            if (Item* it = plr->StoreNewItem(dest, itemId, true))
            {
                plr->SendNewItem(it, count, true, false);
                return true;
            }
        }
        ChatHandler(plr->GetSession()).SendSysMessage(T(
            "Inventář je plný, odměna byla připsána na účet. Vyzvedni pomocí \".reward claim\".",
            "Inventory is full, reward was credited to your account. Use \".reward claim\" to collect."
        ));
    }

    std::string up =
        "INSERT INTO customs.rewards (account,item,entitled,claimed) VALUES (" +
        std::to_string(accountId) + "," + std::to_string(itemId) + "," + std::to_string(count) + ",0) "
        "ON DUPLICATE KEY UPDATE entitled = entitled + VALUES(entitled), updated_at = NOW()";
    CharacterDatabase.DirectExecute(up.c_str());
    return true;
}

// ==== config ====
struct LvlCfg
{
    bool enable = false;
    std::vector<uint32> milestones;
    std::string delivery = "inventory";
    bool announce = true;
};

static LvlCfg ReadLvlCfg()
{
    LvlCfg c;
    c.enable     = sConfigMgr->GetOption<bool>("Token.Level.Enable", false);
    c.milestones = ParseCSVu32(sConfigMgr->GetOption<std::string>("Token.Level.Milestones", "10,20,30,40,50,60,70,80"));
    c.delivery   = sConfigMgr->GetOption<std::string>("Token.Level.Delivery", "inventory");
    c.announce   = sConfigMgr->GetOption<bool>("Token.Level.Announce", true);
    return c;
}

static bool GetMilestoneReward(uint32 milestone, uint32& outItemId, uint32& outCount)
{
    std::string base = "Token.Level." + std::to_string(milestone) + ".";
    outItemId = sConfigMgr->GetOption<uint32>((base + "ItemId").c_str(), 0u);
    outCount  = sConfigMgr->GetOption<uint32>((base + "Count").c_str(), 0u);
    return outItemId != 0 && outCount != 0;
}

// ==== handler ====
static void HandleLevelMilestone(Player* player)
{
    LvlCfg cfg = ReadLvlCfg();
    if (!cfg.enable || !player || !player->GetSession())
        return;

    uint32 level = player->GetLevel();
    if (level < 10 || level > 80 || (level % 10) != 0)
        return;

    uint32 itemId = 0, count = 0;
    if (!GetMilestoneReward(level, itemId, count))
        return;

    uint32 acc  = player->GetSession()->GetAccountId();
    uint32 guid = player->GetGUID().GetCounter();

    {
        std::vector<Range> blocked = ParseRanges(sConfigMgr->GetOption<std::string>("RealOnline.IgnoreAccountIdRanges", ""));
        if (!blocked.empty() && InRanges(acc, blocked))
            return;
    }

    std::string q1 =
        "SELECT 1 FROM customs.level_milestones "
        "WHERE account=" + std::to_string(acc) +
        " AND guid=" + std::to_string(guid) +
        " AND milestone=" + std::to_string(level) +
        " LIMIT 1";
    if (QueryResult r = CharacterDatabase.Query(q1.c_str()))
        return;

    std::string q2 =
        "SELECT COUNT(*) FROM customs.level_milestones "
        "WHERE account=" + std::to_string(acc) +
        " AND milestone=" + std::to_string(level);
    uint32 totalForAcc = 0;
    if (QueryResult r2 = CharacterDatabase.Query(q2.c_str()))
        totalForAcc = r2->Fetch()[0].Get<uint32>();
    if (totalForAcc >= 10)
        return;

    std::string ins =
        "INSERT INTO customs.level_milestones (account,guid,milestone) VALUES (" +
        std::to_string(acc) + "," + std::to_string(guid) + "," + std::to_string(level) + ")";
    CharacterDatabase.DirectExecute(ins.c_str());

    DeliverRewardToPlayerOrEntitlement(player, acc, itemId, count, cfg.delivery);

    if (cfg.announce)
    {
        std::ostringstream ss;
        if (LangOpt()==Lang::EN)
            ss << "Grats! You reached level " << level << " and receive " << count << "x Mystery Token.";
        else
            ss << "Gratuluji! Dosáhl jsi " << level << ". levelu a získáváš " << count << "x Mystery Token.";
        ChatHandler(player->GetSession()).SendSysMessage(ss.str().c_str());
    }
}



// ==== script ====
class TokenLevelMilestones : public PlayerScript
{
public:
    TokenLevelMilestones() : PlayerScript("TokenLevelMilestones") { }

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        LvlCfg cfg = ReadLvlCfg();
        if (!cfg.enable || !player || !player->GetSession())
            return;

        uint32 newLevel = player->GetLevel();
        if (newLevel <= oldLevel)
            return;

        uint32 acc = player->GetSession()->GetAccountId();
        uint32 guidLow = player->GetGUID().GetCounter();

        std::vector<Range> blocked = ParseRanges(sConfigMgr->GetOption<std::string>("RealOnline.IgnoreAccountIdRanges", ""));
        bool isBlocked = (!blocked.empty() && InRanges(acc, blocked));

        uint32 start = oldLevel + 1;
        uint32 end   = newLevel;
        uint32 firstMilestone = ((start + 9) / 10) * 10;

        auto ms = cfg.milestones;

        for (uint32 m = firstMilestone; m <= end && m <= 80; m += 10)
        {
            if (!std::binary_search(ms.begin(), ms.end(), m))
                continue;

            uint32 itemId = 0, count = 0;
            if (!GetMilestoneReward(m, itemId, count))
                continue;

            if (isBlocked)
                continue;

            uint32 totalForAcc = 0;
            {
                std::string q = "SELECT COUNT(*) FROM customs.level_milestones WHERE account="
                              + std::to_string(acc) + " AND milestone=" + std::to_string(m);
                if (QueryResult r = CharacterDatabase.Query(q.c_str()))
                    totalForAcc = r->Fetch()[0].Get<uint32>();
            }
            if (totalForAcc >= 10)
                continue;

            std::string ins = "INSERT IGNORE INTO customs.level_milestones (account,guid,milestone) VALUES ("
                            + std::to_string(acc) + "," + std::to_string(guidLow) + "," + std::to_string(m) + ")";
            CharacterDatabase.DirectExecute(ins.c_str());

            uint32 nowCount = 0;
            {
                std::string q2 = "SELECT COUNT(*) FROM customs.level_milestones WHERE account="
                               + std::to_string(acc) + " AND guid=" + std::to_string(guidLow)
                               + " AND milestone=" + std::to_string(m);
                if (QueryResult r2 = CharacterDatabase.Query(q2.c_str()))
                    nowCount = r2->Fetch()[0].Get<uint32>();
            }
            if (nowCount == 0)
                continue;

            DeliverRewardToPlayerOrEntitlement(player, acc, itemId, count, cfg.delivery);

            if (cfg.announce)
            {
                std::ostringstream ss;
                if (LangOpt()==Lang::EN)
                    ss << "Grats! You reached level " << m << " and receive " << count << "x Mystery Token.";
                else
                    ss << "Gratuluji! Dosáhl jsi " << m << ". levelu a získáváš " << count << "x Mystery Token.";
                ChatHandler(player->GetSession()).SendSysMessage(ss.str().c_str());
                player->GetSession()->SendAreaTriggerMessage(ss.str().c_str());
            }
        }
    }
};

void Addmod_token_level_milestonesScripts()
{
    new TokenLevelMilestones();
}
