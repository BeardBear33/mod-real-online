#include "Config.h"
#include "ScriptMgr.h"
#include "Player.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "WorldSession.h"
#include "GameTime.h"
#include <algorithm>
#include <string>
#include <vector>
#include <sstream>
#include <unordered_map>
#include <cctype>
#include "ObjectAccessor.h"
#include "EventProcessor.h"

// =============================
// Playerbots (detekce typu hráče / bota)
//  - Login streak: jen Human (skutečný hráč)
//  - Zakázat: Alt + RandomBot + AddclassBot
// =============================
#if __has_include("PlayerbotAI.h") && __has_include("RandomPlayerbotMgr.h") && __has_include("Playerbots.h")
  #include "PlayerbotAI.h"
  #include "RandomPlayerbotMgr.h"
  #include "Playerbots.h" // GET_PLAYERBOT_AI + sPlayerbotsMgr
  #define RO_HAS_PLAYERBOTS 1
#else
  #define RO_HAS_PLAYERBOTS 0
#endif

#if RO_HAS_PLAYERBOTS
static inline PlayerbotAI* RO_GetPlayerbotAI(Player* p)
{
    if (!p)
        return nullptr;

    // Nejstabilnější na tvém branche: přes PlayerbotsMgr
    // Playerbots.h definuje GET_PLAYERBOT_AI(object) -> sPlayerbotsMgr->GetPlayerbotAI(object)
    return GET_PLAYERBOT_AI(p);
}
#endif

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

static inline bool RO_IsHuman(Player* p)
{
    if (!p)
        return false;

#if RO_HAS_PLAYERBOTS
    // Random + Addclass vždy pryč
    if (RO_IsRandomOrAddclass(p))
        return false;

    // Pokud existuje PlayerbotAI, rozhodneme přes něj:
    // - altbot pryč
    // - human = IsRealPlayer() (master == bot)
    if (PlayerbotAI* ai = RO_GetPlayerbotAI(p))
    {
        if (ai->IsAlt())
            return false;

        return ai->IsRealPlayer();
    }

    // Když nemáme AI (typicky normální hráč bez bot AI), bereme jako human
    return true;
#else
    return true;
#endif
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
static std::string Trim2(std::string s)
{
    auto notSpace = [](int ch){ return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

static std::vector<uint32> ParseCSVu32b(std::string const& s)
{
    std::vector<uint32> out;
    std::stringstream ss(s);
    std::string seg;
    while (std::getline(ss, seg, ','))
    {
        seg = Trim2(seg);
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
static bool DeliverEntitlementOrInventory(Player* plr, uint32 accountId, uint32 itemId, uint32 count, std::string const& deliveryMode)
{
    std::string mode = deliveryMode;
    std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);

    auto upsertToBank = [&]()
    {
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

    if (mode == "entitlement" || mode == "stored" || mode == "bank" || mode == "tokenbank")
        return upsertToBank();

    if (mode != "inventory")
        mode = "inventory";

    ItemPosCountVec dest;
    if (plr && plr->GetSession() && plr->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count) == EQUIP_ERR_OK)
    {
        if (Item* it = plr->StoreNewItem(dest, itemId, true))
        {
            plr->SendNewItem(it, count, true, false);
            return true;
        }
    }

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
struct StreakCfg
{
    bool enable = false;
    uint32 baseItem = 0;
    uint32 baseCount = 0;
    uint32 cycleLen = 28;
    std::vector<uint32> specialDays;
    uint32 dayBoundaryHour = 4;
    bool resetOnMiss = true;
    std::string delivery = "inventory";
    bool announce = true;
};

static StreakCfg ReadStreakCfg()
{
    StreakCfg c;
    c.enable          = sConfigMgr->GetOption<bool>("Token.Streak.Enable", false);
    c.baseItem        = sConfigMgr->GetOption<uint32>("Token.Streak.Base.ItemId", 0u);
    c.baseCount       = sConfigMgr->GetOption<uint32>("Token.Streak.Base.Count", 0u);
    c.cycleLen        = std::max(1u, sConfigMgr->GetOption<uint32>("Token.Streak.CycleLength", 28u));
    c.specialDays     = ParseCSVu32b(sConfigMgr->GetOption<std::string>("Token.Streak.SpecialDays", "7,14,21,28"));
    c.dayBoundaryHour = sConfigMgr->GetOption<uint32>("Token.Streak.DayBoundaryHour", 4u);
    c.resetOnMiss     = sConfigMgr->GetOption<bool>("Token.Streak.ResetOnMiss", true);
    c.delivery        = sConfigMgr->GetOption<std::string>("Token.Streak.Delivery", "inventory");
    c.announce        = sConfigMgr->GetOption<bool>("Token.Streak.Announce", true);
    return c;
}

static inline uint32 TodaySerial(uint32 boundaryHour)
{
    time_t now = static_cast<time_t>(GameTime::GetGameTime().count());
    int64 shifted = static_cast<int64>(now) - static_cast<int64>(boundaryHour) * 3600;
    if (shifted < 0) shifted = 0;
    return static_cast<uint32>(shifted / 86400);
}

static bool IsSpecialDay(uint32 day, std::vector<uint32> const& specials)
{
    return std::binary_search(specials.begin(), specials.end(), day);
}

static void ReadSpecialReward(uint32 day, uint32& itemId, uint32& count)
{
    std::string base = "Token.Streak.Special." + std::to_string(day) + ".";
    itemId = sConfigMgr->GetOption<uint32>((base + "ItemId").c_str(), 0u);
    count  = sConfigMgr->GetOption<uint32>((base + "Count").c_str(), 0u);
}

class StreakAnnounceEvent : public BasicEvent
{
public:
    StreakAnnounceEvent(ObjectGuid guid, std::string msg) : _guid(guid), _msg(std::move(msg)) { }

    bool Execute(uint64 /*execTime*/, uint32 /*diff*/) override
    {
        if (Player* p = ObjectAccessor::FindPlayer(_guid))
        {
            if (p->GetSession())
            {
                ChatHandler(p->GetSession()).SendSysMessage(_msg.c_str());
                p->GetSession()->SendAreaTriggerMessage(_msg.c_str());
            }
        }
        return true;
    }

private:
    ObjectGuid _guid;
    std::string _msg;
};

static void SendStreakAnnounceDelayed(Player* player, std::string const& msg, uint32 delayMs = 1500)
{
    if (!player || !player->GetSession())
        return;

    player->m_Events.AddEvent(new StreakAnnounceEvent(player->GetGUID(), msg), player->m_Events.CalculateTime(delayMs));
}

// =====================================================================
// Anti-dup guard: jeden účet může dostat streak reward max 1× za "todaySerial"
// (řeší vlnu loginů altů / více postav na stejném účtu)
// =====================================================================
static std::unordered_map<uint32, uint32> s_rewardedTodayByAccount; // acc -> todaySerial
static uint32 s_guardSerial = 0;

// ==== handler ====
static void HandleLoginStreak(Player* player)
{
    StreakCfg cfg = ReadStreakCfg();
    if (!cfg.enable || !player || !player->GetSession())
        return;

    // FILTR: jen skutečný hráč
    if (!RO_IsHuman(player))
        return;

    if (cfg.baseItem == 0 || cfg.baseCount == 0)
        return;

    uint32 acc = player->GetSession()->GetAccountId();
    uint32 today = TodaySerial(cfg.dayBoundaryHour);

    // reset mapy při změně dne
    if (s_guardSerial != today)
    {
        s_rewardedTodayByAccount.clear();
        s_guardSerial = today;
    }

    // guard proti více spuštěním v jedné login vlně
    auto itg = s_rewardedTodayByAccount.find(acc);
    if (itg != s_rewardedTodayByAccount.end() && itg->second == today)
        return;

    uint32 lastSerial = 0, lastRewardSerial = 0, streakDay = 0;

    {
        std::string q =
            "SELECT last_serial, last_reward_serial, streak_day FROM customs.login_streak WHERE account=" +
            std::to_string(acc) + " LIMIT 1";
        if (QueryResult r = CharacterDatabase.Query(q.c_str()))
        {
            Field* f = r->Fetch();
            lastSerial       = f[0].Get<uint32>();
            lastRewardSerial = f[1].Get<uint32>();
            streakDay        = f[2].Get<uint32>();
        }
        else
        {
            // první záznam pro účet => Day 1 a odměna
            streakDay = 1;

            uint32 totalCount = cfg.baseCount;
            bool separateBonus = false;
            uint32 spItem = 0, spCnt = 0;

            if (IsSpecialDay(streakDay, cfg.specialDays))
            {
                ReadSpecialReward(streakDay, spItem, spCnt);
                if (spItem && spCnt)
                    separateBonus = true;
                else
                    totalCount += spCnt;
            }

            // nastav guard ještě před rewardem
            s_rewardedTodayByAccount[acc] = today;

            if (separateBonus)
            {
                DeliverEntitlementOrInventory(player, acc, cfg.baseItem, cfg.baseCount, cfg.delivery);
                DeliverEntitlementOrInventory(player, acc, spItem, spCnt, cfg.delivery);
            }
            else
            {
                DeliverEntitlementOrInventory(player, acc, cfg.baseItem, totalCount, cfg.delivery);
            }

            std::string ins =
                "INSERT INTO customs.login_streak (account,last_serial,last_reward_serial,streak_day) VALUES (" +
                std::to_string(acc) + "," + std::to_string(today) + "," + std::to_string(today) + "," + std::to_string(streakDay) + ") "
                "ON DUPLICATE KEY UPDATE last_serial=VALUES(last_serial), last_reward_serial=VALUES(last_reward_serial), streak_day=VALUES(streak_day)";
            CharacterDatabase.Execute(ins.c_str());

            if (cfg.announce)
            {
                std::ostringstream ss;
                if (LangOpt()==Lang::EN)
                {
                    ss << "Congrats! Day " << streakDay << " in a row out of " << cfg.cycleLen << ". ";
                    if (separateBonus)
                        ss << "You receive " << cfg.baseCount << "× Mystery Token and additionally " << spCnt << "× Mystery Token.";
                    else if (totalCount != cfg.baseCount)
                        ss << "You receive " << totalCount << "× Mystery Token (including bonus " << (totalCount - cfg.baseCount) << "×).";
                    else
                        ss << "You receive " << cfg.baseCount << "× Mystery Token.";
                }
                else
                {
                    ss << "Gratulace! " << streakDay << ". den v řadě z " << cfg.cycleLen << ". ";
                    if (separateBonus)
                        ss << "Získáváš " << cfg.baseCount << "× Mystery Token a navíc " << spCnt << "× Mystery Token.";
                    else if (totalCount != cfg.baseCount)
                        ss << "Získáváš " << totalCount << "× Mystery Token (včetně bonusu " << (totalCount - cfg.baseCount) << "×).";
                    else
                        ss << "Získáváš " << cfg.baseCount << "× Mystery Token.";
                }
                SendStreakAnnounceDelayed(player, ss.str());
            }
            return;
        }
    }

    // už dnes vyplaceno => nic
    if (lastRewardSerial == today)
        return;

    int64 delta = static_cast<int64>(today) - static_cast<int64>(lastSerial);

    if (delta == 1)
        streakDay = (streakDay % cfg.cycleLen) + 1;
    else if (delta > 1)
    {
        if (cfg.resetOnMiss)
            streakDay = 1;
        else
            streakDay = (streakDay % cfg.cycleLen) + 1;
    }
    // delta <= 0: necháme streakDay beze změny, jen vyplatíme pokud nebyl reward dnes

    uint32 totalCount = cfg.baseCount;
    bool separateBonus = false;
    uint32 spItem = 0, spCnt = 0;

    if (IsSpecialDay(streakDay, cfg.specialDays))
    {
        ReadSpecialReward(streakDay, spItem, spCnt);
        if (spItem && spCnt)
            separateBonus = true;
        else
            totalCount += spCnt;
    }

    // nastav guard ještě před rewardem
    s_rewardedTodayByAccount[acc] = today;

    {
        std::string up =
            "UPDATE customs.login_streak SET last_serial=" + std::to_string(today) +
            ", last_reward_serial=" + std::to_string(today) +
            ", streak_day=" + std::to_string(streakDay) +
            " WHERE account=" + std::to_string(acc);
        CharacterDatabase.Execute(up.c_str());
    }

    if (separateBonus)
    {
        DeliverEntitlementOrInventory(player, acc, cfg.baseItem, cfg.baseCount, cfg.delivery);
        DeliverEntitlementOrInventory(player, acc, spItem, spCnt, cfg.delivery);
    }
    else
    {
        DeliverEntitlementOrInventory(player, acc, cfg.baseItem, totalCount, cfg.delivery);
    }

    if (cfg.announce)
    {
        std::ostringstream ss;
        if (LangOpt()==Lang::EN)
        {
            ss << "Congrats! Day " << streakDay << " in a row out of " << cfg.cycleLen << ". ";
            if (separateBonus)
                ss << "You receive " << cfg.baseCount << "× Mystery Token and additionally " << spCnt << "× Mystery Token.";
            else if (totalCount != cfg.baseCount)
                ss << "You receive " << totalCount << "× Mystery Token (including bonus " << (totalCount - cfg.baseCount) << "×).";
            else
                ss << "You receive " << cfg.baseCount << "× Mystery Token.";
        }
        else
        {
            ss << "Gratulace! " << streakDay << ". den v řadě z " << cfg.cycleLen << ". ";
            if (separateBonus)
                ss << "Získáváš " << cfg.baseCount << "× Mystery Token a navíc " << spCnt << "× Mystery Token.";
            else if (totalCount != cfg.baseCount)
                ss << "Získáváš " << totalCount << "× Mystery Token (včetně bonusu " << (totalCount - cfg.baseCount) << "×).";
            else
                ss << "Získáváš " << cfg.baseCount << "× Mystery Token.";
        }
        SendStreakAnnounceDelayed(player, ss.str());
    }
}

// ==== script ====
class TokenLoginStreak : public PlayerScript
{
public:
    TokenLoginStreak() : PlayerScript("TokenLoginStreak") { }
    void OnPlayerLogin(Player* player) override { HandleLoginStreak(player); }
};

void Addmod_token_login_streakScripts()
{
    new TokenLoginStreak();
}
