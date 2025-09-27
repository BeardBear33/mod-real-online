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

// ==== blocklist rozsahů účtů (A-B;C-D;...) ====
struct Range { uint32 min=0, max=0; };

static std::vector<Range> ParseRanges(std::string const& txt)
{
    std::vector<Range> out;
    std::stringstream ss(txt);
    std::string seg;
    while (std::getline(ss, seg, ';'))
    {
        seg = Trim2(seg);
        if (seg.empty()) continue;
        auto dash = seg.find('-');
        if (dash == std::string::npos) continue;
        std::string a = Trim2(seg.substr(0, dash));
        std::string b = Trim2(seg.substr(dash + 1));
        if (a.empty() || b.empty()) continue;
        uint32 mn = 0, mx = 0;
        try { mn = static_cast<uint32>(std::stoul(a)); mx = static_cast<uint32>(std::stoul(b)); }
        catch (...) { continue; }
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

static bool DeliverEntitlementOrInventory(Player* plr, uint32 accountId, uint32 itemId, uint32 count, std::string const& deliveryMode)
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

// ==== handler ====
static void HandleLoginStreak(Player* player)
{
    StreakCfg cfg = ReadStreakCfg();
    if (!cfg.enable || !player || !player->GetSession())
        return;
    if (cfg.baseItem == 0 || cfg.baseCount == 0)
        return;

    uint32 acc = player->GetSession()->GetAccountId();
    uint32 today = TodaySerial(cfg.dayBoundaryHour);

// Blokace účtů pro denní odměny podle RealOnline.IgnoreAccountIdRanges
{
    std::vector<Range> blocked = ParseRanges(sConfigMgr->GetOption<std::string>("RealOnline.IgnoreAccountIdRanges", ""));
    if (!blocked.empty() && InRanges(acc, blocked))
        return; // účet je blokovaný – žádný zápis ani vyplácení
}


    uint32 lastSerial = 0, lastRewardSerial = 0, streakDay = 0;

    // načtení stavu
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
            // první záznam pro účet = první den
            streakDay = 1;

            uint32 totalCount = cfg.baseCount;
            bool separateBonus = false;
            uint32 spItem = 0, spCnt = 0;

            if (IsSpecialDay(streakDay, cfg.specialDays))
            {
                ReadSpecialReward(streakDay, spItem, spCnt);
                if (spItem && spCnt)
                {
                    separateBonus = true;
                }
                else
                {
                    totalCount += spCnt; // bonus stejného itemu
                }
            }

            // vyplacení
            if (separateBonus)
            {
                DeliverEntitlementOrInventory(player, acc, cfg.baseItem, cfg.baseCount, cfg.delivery);
                DeliverEntitlementOrInventory(player, acc, spItem, spCnt, cfg.delivery);
            }
            else
            {
                DeliverEntitlementOrInventory(player, acc, cfg.baseItem, totalCount, cfg.delivery);
            }

            // zápis
            std::string ins =
                "INSERT INTO customs.login_streak (account,last_serial,last_reward_serial,streak_day) VALUES (" +
                std::to_string(acc) + "," + std::to_string(today) + "," + std::to_string(today) + "," + std::to_string(streakDay) + ") "
                "ON DUPLICATE KEY UPDATE last_serial=VALUES(last_serial), last_reward_serial=VALUES(last_reward_serial), streak_day=VALUES(streak_day)";
            CharacterDatabase.Execute(ins.c_str());

            // hláška
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
                ChatHandler(player->GetSession()).SendSysMessage(ss.str().c_str());
            }
            return;
        }
    }

    // existující účet
    int64 delta = static_cast<int64>(today) - static_cast<int64>(lastSerial);

    if (delta <= 0)
    {
        // už dnes přihlášen: vyplatit pouze pokud ještě nebylo vyplaceno dnes
        if (lastRewardSerial == today)
            return;
        // streakDay se nemění
    }
    else if (delta == 1)
    {
        // nový den
        streakDay = (streakDay % cfg.cycleLen) + 1;
    }
    else
    {
        // vynecháno víc dnů
        if (cfg.resetOnMiss)
            streakDay = 1;
        else
            streakDay = (streakDay % cfg.cycleLen) + 1;
    }

    // bonusy
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

    // update stavu (ať se vyplácí jen jednou denně na účet)
    {
        std::string up =
            "UPDATE customs.login_streak SET last_serial=" + std::to_string(today) +
            ", last_reward_serial=" + std::to_string(today) +
            ", streak_day=" + std::to_string(streakDay) +
            " WHERE account=" + std::to_string(acc);
        CharacterDatabase.Execute(up.c_str());
    }

    // vyplacení
    if (separateBonus)
    {
        DeliverEntitlementOrInventory(player, acc, cfg.baseItem, cfg.baseCount, cfg.delivery);
        DeliverEntitlementOrInventory(player, acc, spItem, spCnt, cfg.delivery);
    }
    else
    {
        DeliverEntitlementOrInventory(player, acc, cfg.baseItem, totalCount, cfg.delivery);
    }

    // hláška
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
        ChatHandler(player->GetSession()).SendSysMessage(ss.str().c_str());
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
