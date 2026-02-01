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
#include <cctype>

// =============================
// Playerbots (detekce typu hráče / bota)
//  - Milníky: povolit Human + Alt
//  - Zakázat: RandomBot + AddclassBot
// =============================
#if __has_include("PlayerbotAI.h") && __has_include("RandomPlayerbotMgr.h")
  #include "PlayerbotAI.h"
  #include "RandomPlayerbotMgr.h"
  #define RO_HAS_PLAYERBOTS 1
#else
  #define RO_HAS_PLAYERBOTS 0
#endif

// ---- PlayerbotAI safe accessor (nepoužívat GET_PLAYERBOT_AI) ----
#if RO_HAS_PLAYERBOTS
static inline PlayerbotAI* RO_GetPlayerbotAI(Player* p)
{
    if (!p) return nullptr;

    // Preferované na některých branchech
    #if __has_include("Player.h")
        // mnoho AC/playerbots větví má Player::GetPlayerbotAI()
        // (když ne, kompilátor to odfiltruje až níže přes fallback)
    #endif

    // 1) Pokud existuje GetPlayerbotAI() metoda
    //    (funguje na hodně playerbots branchech)
    #if defined(__clang__) || defined(__GNUG__)
        // Bezpečný trik: zkusíme zavolat, když existuje (SFINAE není snadné v C++14 bez templátů),
        // proto jdeme přímo na fallback přes GetAI() níže, který bývá všude.
    #endif

    // 2) Univerzální fallback: Player::GetAI()
    //    (na playerbots to obvykle vrací PlayerbotAI*)
    if (UnitAI* ai = p->GetAI())
        if (auto* pAI = dynamic_cast<PlayerbotAI*>(ai))
            return pAI;

    return nullptr;
}
#endif

static inline std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static inline bool RO_IsRandomOrAddclass(Player* p)
{
#if RO_HAS_PLAYERBOTS
	return p &&
		(sRandomPlayerbotMgr.IsRandomBot(p) || sRandomPlayerbotMgr.IsAddclassBot(p));
#else
    (void)p;
    return false;
#endif
}

static inline bool RO_IsAltBot(Player* p)
{
#if RO_HAS_PLAYERBOTS
    if (!p) return false;
    if (RO_IsRandomOrAddclass(p)) return false;
    if (PlayerbotAI* ai = RO_GetPlayerbotAI(p))
        return ai->IsAlt();
    return false;
#else
    (void)p;
    return false;
#endif
}

static inline bool RO_IsHuman(Player* p)
{
    if (!p) return false;

#if RO_HAS_PLAYERBOTS
    if (RO_IsRandomOrAddclass(p))
        return false;

    if (PlayerbotAI* ai = RO_GetPlayerbotAI(p))
    {
        if (ai->IsAlt())
            return false;
        return ai->IsRealPlayer(); // master == bot
    }

    return true;
#else
    return true;
#endif
}

// Milestones: povolit human + alt, zakázat random/addclass
static inline bool RO_AllowMilestoneReward(Player* p)
{
    if (!p) return false;
    if (RO_IsRandomOrAddclass(p)) return false;
    return RO_IsHuman(p) || RO_IsAltBot(p);
}

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

// =====================================================================
// Delivery:
//  - inventory  -> pokus do bagů; když nejde, uloží do token banky (customs.rewards stored)
//  - entitlement/stored/bank -> rovnou do token banky (bez pokusu o inventory)
// =====================================================================
static bool DeliverRewardToPlayerOrEntitlement(Player* plr, uint32 accountId, uint32 itemId, uint32 count, std::string const& deliveryMode)
{
    std::string mode = deliveryMode;
    std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);

    auto upsertToBank = [&]()
    {
        // Entitlement nyní znamená "připsat do banky" (stored) a dorovnat claimed,
        // aby nevznikaly nevyzvednuté odměny.
        std::string up =
            "INSERT INTO customs.rewards (`account`,`item`,`entitled`,`claimed`,`stored`) VALUES ("
            + std::to_string(accountId) + "," + std::to_string(itemId) + "," + std::to_string(count) + "," + std::to_string(count) + "," + std::to_string(count) + ") "
            "ON DUPLICATE KEY UPDATE "
            "  `entitled` = `entitled` + VALUES(`entitled`), "
            "  `claimed`  = `claimed`  + VALUES(`claimed`), "
            "  `stored`   = `stored`   + VALUES(`stored`), "
            "  updated_at = NOW()";
        CharacterDatabase.DirectExecute(up.c_str());
        return true;
    };

    // Režimy, které mají jít rovnou do banky (kompatibilita: entitlement zůstává, jen dělá stored)
    if (mode == "entitlement" || mode == "stored" || mode == "bank" || mode == "tokenbank")
        return upsertToBank();

    // Default + explicitní inventory
    if (mode != "inventory")
        mode = "inventory";

    // inventory: zkusit dát hráči do bagů, když nejde -> bank + info
    ItemPosCountVec dest;
    if (plr && plr->GetSession() && plr->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count) == EQUIP_ERR_OK)
    {
        if (Item* it = plr->StoreNewItem(dest, itemId, true))
        {
            plr->SendNewItem(it, count, true, false);
            return true;
        }
    }

    // fallback do banky
    upsertToBank();

    if (plr && plr->GetSession())
    {
        ChatHandler(plr->GetSession()).SendSysMessage(T(
            "Inventář je plný, odměna byla uložena do token banky. Vyber pomocí \".token withdraw <pocet>\" (stav: \".token\").",
            "Inventory is full, reward was stored in token bank. Use \".token withdraw <count>\" (status: \".token\")."
        ));
    }

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

    // FILTR: milníky jen human + alt, nikdy random/addclass
    if (!RO_AllowMilestoneReward(player))
        return;

    uint32 level = player->GetLevel();
    if (level < 10 || level > 80 || (level % 10) != 0)
        return;

    uint32 itemId = 0, count = 0;
    if (!GetMilestoneReward(level, itemId, count))
        return;

    uint32 acc  = player->GetSession()->GetAccountId();
    uint32 guid = player->GetGUID().GetCounter();

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
        player->GetSession()->SendAreaTriggerMessage(ss.str().c_str());
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

        // FILTR: milníky jen human + alt, nikdy random/addclass
        if (!RO_AllowMilestoneReward(player))
            return;

        uint32 newLevel = player->GetLevel();
        if (newLevel <= oldLevel)
            return;

        uint32 acc = player->GetSession()->GetAccountId();
        uint32 guidLow = player->GetGUID().GetCounter();

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
