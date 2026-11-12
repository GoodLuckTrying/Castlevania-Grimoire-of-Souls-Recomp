// Debug menu: a small window (F8 by default) with cheats and unlocks for
// testing. Every action runs the game's own save-data code (gdUserSaveData),
// on the main thread, from a hook on the resident gs.Update, then saves.
// The same per-frame hook applies the display settings (display.cpp) and the
// controller tables.
//
// gosr.ini:
//   [debug]
//   menu=1        ; 0 disables the menu and its hotkey
//   hotkey=F8     ; F1-F12, or a virtual-key code
//   warned=1      ; set once the first-use warning was accepted
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "il2cpp-api.h"
#include "il2cpp-object-internals.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#pragma comment(lib, "gdi32.lib")

namespace recomp
{
    void ApplyDisplaySettings();  // display.cpp
    extern std::atomic<bool> g_invincible;        // battle_hooks.cpp
    extern std::atomic<int> g_damageDealtFactor;  // battle_hooks.cpp
}

namespace
{
    // methodPointers slot of gs.Update (0x1FFAE9C), the per-frame update of
    // the game's resident root object, in the v1.4.2 arm64 image.
    constexpr uint64_t kGsUpdateSlot = 0x3df6ed0;

    using UpdateFn = void (*)(void*, const void*);
    UpdateFn g_origUpdate = nullptr;

    std::mutex g_queueLock;
    std::vector<std::function<std::string()>> g_queue;
    std::atomic<bool> g_pending{ false };
    std::atomic<bool> g_menuVisible{ false };
    std::wstring g_iniPath;

    std::mutex g_statusLock;
    std::string g_status = "Ready. Load a save, then pick an action.";
    HWND g_window = nullptr;
    constexpr UINT WM_STATUS = WM_APP + 1;

    void SetStatus(const std::string& s)
    {
        {
            std::lock_guard<std::mutex> lock(g_statusLock);
            g_status = s;
        }
        fprintf(stderr, "debug menu: %s\n", s.c_str());
        if (g_window)
            PostMessageW(g_window, WM_STATUS, 0, 0);
    }

    // ------------------------------------------------------------ il2cpp access

    struct Failure
    {
        std::string what;
    };

    const Il2CppImage* GameImage()
    {
        static const Il2CppImage* image = [] {
            size_t n = 0;
            const Il2CppAssembly** assemblies = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &n);
            for (size_t i = 0; i < n; i++)
            {
                const Il2CppImage* img = il2cpp_assembly_get_image(assemblies[i]);
                if (!strcmp(il2cpp_image_get_name(img), "Assembly-CSharp.dll"))
                    return img;
            }
            return static_cast<const Il2CppImage*>(nullptr);
        }();
        if (!image)
            throw Failure{ "Assembly-CSharp not found" };
        return image;
    }

    Il2CppClass* GameClass(const char* name)
    {
        Il2CppClass* k = il2cpp_class_from_name(GameImage(), "", name);
        if (!k)
            throw Failure{ std::string("class ") + name + " not found" };
        return k;
    }

    FieldInfo* FindField(Il2CppClass* k, const char* name)
    {
        for (; k; k = il2cpp_class_get_parent(k))
            if (FieldInfo* f = il2cpp_class_get_field_from_name(k, name))
                return f;
        throw Failure{ std::string("field ") + name + " not found" };
    }

    Il2CppClass* NestedClass(Il2CppClass* outer, const char* name)
    {
        void* iter = nullptr;
        while (Il2CppClass* k = il2cpp_class_get_nested_types(outer, &iter))
            if (!strcmp(il2cpp_class_get_name(k), name))
                return k;
        return nullptr;
    }

    template <typename T>
    T GetField(Il2CppObject* obj, const char* name)
    {
        T value{};
        il2cpp_field_get_value(obj, FindField(il2cpp_object_get_class(obj), name), &value);
        return value;
    }

    const MethodInfo* FindMethod(Il2CppClass* k, const char* name, int argc)
    {
        for (; k; k = il2cpp_class_get_parent(k))
            if (const MethodInfo* m = il2cpp_class_get_method_from_name(k, name, argc))
                return m;
        throw Failure{ std::string("method ") + name + " not found" };
    }

    Il2CppObject* Invoke(const MethodInfo* m, Il2CppObject* obj, void** args)
    {
        Il2CppException* exc = nullptr;
        Il2CppObject* r = il2cpp_runtime_invoke(m, obj, args, &exc);
        if (exc)
        {
            char msg[512] = {};
            il2cpp_format_exception(exc, msg, sizeof(msg));
            throw Failure{ std::string(il2cpp_method_get_name(m)) + ": " + msg };
        }
        return r;
    }

    template <typename... A>
    Il2CppObject* Call(Il2CppObject* obj, const char* name, A... a)
    {
        void* args[sizeof...(A) + 1] = { static_cast<void*>(a)... };
        return Invoke(FindMethod(il2cpp_object_get_class(obj), name, int(sizeof...(A))), obj, args);
    }

    template <typename... A>
    Il2CppObject* CallStatic(Il2CppClass* k, const char* name, A... a)
    {
        void* args[sizeof...(A) + 1] = { static_cast<void*>(a)... };
        return Invoke(FindMethod(k, name, int(sizeof...(A))), nullptr, args);
    }

    int32_t Int(Il2CppObject* boxed) { return boxed ? *static_cast<int32_t*>(il2cpp_object_unbox(boxed)) : 0; }
    bool Bool(Il2CppObject* boxed) { return boxed && *static_cast<uint8_t*>(il2cpp_object_unbox(boxed)); }

    bool Active(Il2CppObject* component)
    {
        Il2CppObject* go = Call(component, "get_gameObject");
        return go && Bool(Call(go, "get_activeInHierarchy"));
    }

    // bsSingleton<T>.Instance
    Il2CppObject* Singleton(const char* name)
    {
        FieldInfo* f = FindField(GameClass(name), "<Instance>k__BackingField");
        Il2CppObject* obj = nullptr;
        il2cpp_field_static_get_value(f, &obj);
        if (!obj)
            throw Failure{ std::string(name) + " is not created yet" };
        return obj;
    }

    Il2CppObject* SaveData()
    {
        Il2CppObject* save = Call(Singleton("gdUser"), "get_saveData");
        if (!save || !Call(save, "GetContents"))
            throw Failure{ "no save loaded yet (get to the home screen first)" };
        return save;
    }

    // Master table rows (Table.BaseTable<Param>.Dict), without deleted or
    // debug rows, ordered by id.
    std::vector<std::pair<int32_t, Il2CppObject*>> TableRows(const char* table)
    {
        Il2CppObject* t = GetField<Il2CppObject*>(Singleton("gdTable"), table);
        if (!t)
            throw Failure{ std::string("table ") + table + " not loaded" };
        Il2CppObject* dict = GetField<Il2CppObject*>(t, "<Dict>k__BackingField");
        if (!dict)
            throw Failure{ std::string("table ") + table + " is empty" };
        auto* entries = GetField<Il2CppArray*>(dict, "entries");
        int32_t count = GetField<int32_t>(dict, "count");
        std::vector<std::pair<int32_t, Il2CppObject*>> rows;
        if (!entries)
            return rows;
        // Dictionary<int, ref>.Entry: hashCode, next, key, value
        struct Entry { int32_t hashCode, next, key; Il2CppObject* value; };
        if (il2cpp_array_element_size(il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(entries))) != sizeof(Entry))
            throw Failure{ "unexpected dictionary layout" };
        auto* e = reinterpret_cast<Entry*>(reinterpret_cast<uint8_t*>(entries) + kIl2CppSizeOfArray);
        for (int32_t i = 0; i < count && i < int32_t(il2cpp_array_length(entries)); i++)
        {
            if (e[i].hashCode < 0 || !e[i].value)
                continue;
            // ParamBase: id 0x10, delete_flg 0x14, debug_flg 0x15
            const auto* p = reinterpret_cast<const int8_t*>(e[i].value);
            if (p[0x14] || p[0x15])
                continue;
            rows.emplace_back(e[i].key, e[i].value);
        }
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        return rows;
    }

    void Commit(Il2CppObject* save)
    {
        Call(save, "UpdateWorkWithCurrentContents");
        bool force = true, countUp = true;
        Call(save, "Save", &force, &countUp);
    }

    // gdUserSaveData.AddContents gift types (its switch on giftContentsType)
    enum Gift
    {
        kCharacter = 3, kMaterial = 4, kFood = 5, kCommonItem = 6, kWeapon = 7, kSubWeapon = 8, kProtector = 9,
        kJukebox = 15, kAvatar = 19, kFragment = 22, kAccessory = 23
    };

    int AddContents(Il2CppObject* save, int type, int id, int count)
    {
        static Il2CppClass* lostInfo = NestedClass(GameClass("QuestEndResultProtocol"), "LostItemInfo");
        Il2CppObject* lost = nullptr;
        if (lostInfo)
        {
            lost = il2cpp_object_new(lostInfo);
            il2cpp_runtime_object_init(lost);
        }
        Il2CppObject* fragmentInfo = nullptr;
        Il2CppObject* characterResult = nullptr;
        int level = 1;
        return Bool(Call(save, "AddContents", &fragmentInfo, &characterResult, &type, &id, &count, &level, lost));
    }

    // Long actions run as batches of steps spread over frames (a few
    // milliseconds each), so the game keeps drawing and the menu shows progress.
    struct Batch
    {
        std::string label;
        std::vector<std::function<bool()>> steps;  // true: done, false: refused or skipped
        std::function<std::string(int done, int refused)> finish;
        size_t next = 0;
        int done = 0, refused = 0;
    };
    std::deque<Batch> g_batches;  // main thread only

    std::string StartBatch(Batch batch)
    {
        std::string label = batch.label;
        g_batches.push_back(std::move(batch));
        return label + "...";
    }

    void RunBatches()
    {
        if (g_batches.empty())
            return;
        ULONGLONG start = GetTickCount64();
        static ULONGLONG lastStatus = 0;
        while (!g_batches.empty() && GetTickCount64() - start < 12)
        {
            Batch& b = g_batches.front();
            if (b.next < b.steps.size())
            {
                try
                {
                    b.steps[b.next]() ? b.done++ : b.refused++;
                }
                catch (const Failure& f)
                {
                    if (b.refused++ < 5)
                        fprintf(stderr, "debug menu: %s step %zu: %s\n", b.label.c_str(), b.next, f.what.c_str());
                }
                b.next++;
                continue;
            }
            try
            {
                SetStatus(b.finish(b.done, b.refused));
            }
            catch (const Failure& f)
            {
                SetStatus("Failed: " + f.what);
            }
            g_batches.pop_front();
        }
        if (!g_batches.empty() && GetTickCount64() - lastStatus > 250)
        {
            lastStatus = GetTickCount64();
            const Batch& b = g_batches.front();
            char buf[160];
            snprintf(buf, sizeof(buf), "%s: %zu / %zu%s", b.label.c_str(), b.next, b.steps.size(),
                     g_batches.size() > 1 ? (" (+" + std::to_string(g_batches.size() - 1) + " queued)").c_str() : "");
            SetStatus(buf);
        }
    }

    std::string Counted(const char* what, int done, int refused, const char* verb = "added")
    {
        return std::string(what) + ": " + std::to_string(done) + " " + verb + (refused ? ", " + std::to_string(refused) + " skipped" : "");
    }

    // Gives each row of a table, once (when not owned yet, per existsMethod) or count of each.
    std::string GiveAll(const char* table, int type, const char* existsMethod, int count, const char* what)
    {
        Il2CppObject* save = SaveData();
        Batch b;
        b.label = what;
        for (const auto& row : TableRows(table))
        {
            int id = row.first;
            b.steps.push_back([=] {
                int key = id;
                if (existsMethod && Bool(Call(save, existsMethod, &key)))
                    return false;
                return AddContents(save, type, id, count) != 0;
            });
        }
        b.finish = [=](int done, int refused) {
            Commit(save);
            return Counted(what, done, refused);
        };
        return StartBatch(std::move(b));
    }

    // ------------------------------------------------------------- the actions

    std::string MaxGold()
    {
        Il2CppObject* save = SaveData();
        int add = Int(CallStatic(GameClass("gdUserSaveData"), "GetMaxGold")) - Int(Call(save, "GetGold"));
        if (add > 0)
            Call(save, "AddGold", &add);
        Commit(save);
        return "Gold: " + std::to_string(Int(Call(save, "GetGold")));
    }

    std::string MaxGems()
    {
        Il2CppObject* save = SaveData();
        int add = Int(CallStatic(GameClass("gdUserSaveData"), "GetMaxFreeCoin")) - Int(Call(save, "GetFreeCoin"));
        if (add > 0)
            Call(save, "AddFreeCoin", &add);
        Commit(save);
        return "Gems: " + std::to_string(Int(Call(save, "GetFreeCoin")));
    }

    std::atomic<int> g_amount{ 100000 };  // the menu's Amount field

    std::string AddGold()
    {
        Il2CppObject* save = SaveData();
        int room = Int(CallStatic(GameClass("gdUserSaveData"), "GetMaxGold")) - Int(Call(save, "GetGold"));
        int add = std::min(int(g_amount), room);
        if (add > 0)
            Call(save, "AddGold", &add);
        Commit(save);
        return "Gold: " + std::to_string(Int(Call(save, "GetGold"))) + (add < g_amount ? " (maximum reached)" : "");
    }

    std::string AddGems()
    {
        Il2CppObject* save = SaveData();
        int room = Int(CallStatic(GameClass("gdUserSaveData"), "GetMaxFreeCoin")) - Int(Call(save, "GetFreeCoin"));
        int add = std::min(int(g_amount), room);
        if (add > 0)
            Call(save, "AddFreeCoin", &add);
        Commit(save);
        return "Gems: " + std::to_string(Int(Call(save, "GetFreeCoin"))) + (add < g_amount ? " (maximum reached)" : "");
    }

    std::string MaxExchangePoints()
    {
        Il2CppObject* save = SaveData();
        Il2CppClass* k = GameClass("gdUserSaveData");
        int points = Int(CallStatic(k, "GetMaxExchangePoint"));
        int souls = Int(CallStatic(k, "GetMaxExchangePointSoul"));
        Call(save, "SetExchangePoint", &points);
        Call(save, "SetExchangePointSoul", &souls);
        Commit(save);
        return "Exchange points: " + std::to_string(points) + ", soul points: " + std::to_string(souls);
    }

    std::string AllItems()
    {
        GiveAll("MaterialItem", kMaterial, nullptr, 9999, "Materials");
        GiveAll("CommonItem", kCommonItem, nullptr, 9999, "Items");
        return GiveAll("Food", kFood, nullptr, 9999, "Food");
    }

    std::string MaxCharacterLevels()
    {
        Il2CppObject* save = SaveData();
        int n = 0;
        for (const auto& row : TableRows("Character"))
        {
            int id = row.first;
            if (!Bool(Call(save, "IsExistCharacter", &id)))
                continue;
            int exp = 100000000;
            Call(save, "AddExp", &id, &exp);
            n++;
        }
        int rp = 100000000;
        Call(save, "AddRp", &rp);
        Commit(save);
        return "Raised " + std::to_string(n) + " characters and the player rank to the maximum";
    }

    std::string MuseumAndMusic()
    {
        Il2CppObject* save = SaveData();
        int museum = 0;
        for (const auto& row : TableRows("Museum"))
        {
            int id = row.first;
            Call(save, "AddMuseum", &id);
            museum++;
        }
        Commit(save);
        GiveAll("JukeboxMusic", kJukebox, "IsExistJukeboxMusic", 1, "Music");
        return "Museum: " + std::to_string(museum) + " entries; adding music...";
    }

    std::string OpenQuests(bool clear)
    {
        Il2CppObject* save = SaveData();
        Batch b;
        b.label = clear ? "Clearing quests" : "Opening quests";
        for (const auto& row : TableRows("Quest"))
        {
            int id = row.first;
            b.steps.push_back([=] {
                int quest = id, one = 1;
                bool force = true;
                if (clear)
                    Call(save, "AddQuestClearCount", &quest, &one, &one, &one, &force);
                else
                    Call(save, "AddNewQuests", &quest, &force);
                return true;
            });
        }
        b.finish = [=](int done, int refused) {
            Commit(save);
            return Counted("Quests", done, refused, clear ? "cleared" : "opened") + ". Leave and re-enter the quest screen.";
        };
        return StartBatch(std::move(b));
    }

    std::string ResetFreePull()
    {
        Il2CppObject* save = SaveData();
        Call(save, "ResetFreeGachaLimit");
        Commit(save);
        return "Free pull is available again";
    }

    // Widens every gacha's period (in memory, for this session) so that all
    // banners, including past events, can be pulled.
    std::string OpenAllBanners()
    {
        auto widen = [](Il2CppObject* row, const char* field, const char* year) {
            FieldInfo* f = FindField(il2cpp_object_get_class(row), field);
            Il2CppString* s = nullptr;
            il2cpp_field_get_value(row, f, &s);
            if (!s || il2cpp_string_length(s) < 4)
                return;
            std::u16string text(reinterpret_cast<const char16_t*>(il2cpp_string_chars(s)), il2cpp_string_length(s));
            for (int i = 0; i < 4; i++)
                text[i] = char16_t(year[i]);
            Il2CppString* replaced = il2cpp_string_new_utf16(reinterpret_cast<const Il2CppChar*>(text.data()), int32_t(text.size()));
            il2cpp_field_set_value(row, f, replaced);
        };
        int gachas = 0;
        for (const auto& row : TableRows("Gacha"))
        {
            widen(row.second, "start_datetime", "2000");
            widen(row.second, "display_start_datetime", "2000");
            widen(row.second, "end_datetime", "2099");
            widen(row.second, "display_end_datetime", "2099");
            gachas++;
        }
        int banners = 0;
        for (const auto& row : TableRows("GachaBannerList"))
        {
            FieldInfo* f = FindField(il2cpp_object_get_class(row.second), "releaseConditionQuest");
            int zero = 0;
            il2cpp_field_set_value(row.second, f, &zero);
            banners++;
        }
        return "Opened " + std::to_string(gachas) + " gachas and " + std::to_string(banners) +
               " banners until the game closes. Re-enter the summon screen.";
    }

    // Follows the tutorial chain from the current step to its end (the step
    // whose next_tutorial_id is 0); that step marks the tutorial finished.
    // The help popups (parceling tutorials) are closed too.
    std::string SkipTutorial()
    {
        Il2CppObject* save = SaveData();
        if (Int(Call(save, "IsTutorialEnd")))
            return "The tutorial is already finished";
        std::unordered_map<int, int> next;
        int lastStep = 0;
        for (const auto& row : TableRows("Tutorial"))
        {
            int n = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(row.second) + 0x18);  // next_tutorial_id
            next[row.first] = n;
            if (!n)
                lastStep = row.first;
        }
        int step = Int(Call(save, "GetTutorialId"));
        if (next.count(step))
        {
            for (int guard = 0; next[step] && next.count(next[step]) && guard < 1000; guard++)
                step = next[step];
            if (!next[step])
                lastStep = step;
        }
        if (!lastStep)
            return "Failed: no final tutorial step found";
        Call(save, "SetTutorialLevel", &lastStep);
        int popups = 0;
        for (const auto& row : TableRows("ParcelingTutorial"))
        {
            int id = row.first;
            try
            {
                Call(save, "EndPercelingTutorial", &id);
                popups++;
            }
            catch (const Failure&)
            {
            }
        }
        Commit(save);
        return "Tutorial finished (step " + std::to_string(lastStep) + "), " + std::to_string(popups) +
               " help popups closed. Restart the game so it notices.";
    }

    template <typename F>
    void ForEachInList(Il2CppObject* list, F f)
    {
        if (!list)
            return;
        auto* items = GetField<Il2CppArray*>(list, "_items");
        int32_t size = GetField<int32_t>(list, "_size");
        if (!items)
            return;
        auto** data = reinterpret_cast<Il2CppObject**>(reinterpret_cast<uint8_t*>(items) + kIl2CppSizeOfArray);
        for (int32_t i = 0; i < size && i < int32_t(il2cpp_array_length(items)); i++)
            if (data[i])
                f(data[i]);
    }

    // Every owned weapon, sub-weapon and armor piece to its current level cap.
    std::string MaxEquipmentLevels()
    {
        Il2CppObject* save = SaveData();
        Il2CppObject* info = Call(save, "GetUserDataGetInfo");
        if (!info)
            throw Failure{ "no user data" };
        int raised = 0;
        const struct { const char* list; const char* update; } kinds[] = {
            { "Weapon", "UpdateWeaponExpAndLevel" }, { "SubWeapon", "UpdateSubWeaponExpAndLevel" }, { "Protector", "UpdateProtectorExpAndLevel" } };
        for (const auto& k : kinds)
        {
            ForEachInList(GetField<Il2CppObject*>(info, k.list), [&](Il2CppObject* rec) {
                int64_t id = GetField<int64_t>(rec, "<id>k__BackingField");
                int level = GetField<int32_t>(rec, "<level>k__BackingField");
                int maxLevel = GetField<int32_t>(rec, "<max_level>k__BackingField");
                int exp = GetField<int32_t>(rec, "<exp>k__BackingField");
                if (maxLevel > level)
                {
                    Call(save, k.update, &id, &exp, &maxLevel);
                    raised++;
                }
            });
        }
        Commit(save);
        return "Raised " + std::to_string(raised) + " pieces of equipment to their level cap";
    }

    // ---- one item at a time, picked by name
    struct ItemEntry
    {
        int type, id;
        std::wstring label;
    };
    std::mutex g_itemsLock;
    std::vector<ItemEntry> g_items;
    constexpr UINT WM_ITEMS = WM_APP + 2;

    std::wstring Widen(Il2CppString* s)
    {
        return s ? std::wstring(reinterpret_cast<const wchar_t*>(il2cpp_string_chars(s)), size_t(il2cpp_string_length(s))) : std::wstring();
    }

    std::string LoadItemList()
    {
        std::unordered_map<int, std::wstring> names;
        for (const auto& row : TableRows("MLStringName"))
            names[row.first] = Widen(*reinterpret_cast<Il2CppString**>(reinterpret_cast<uint8_t*>(row.second) + 0x18));  // text
        const struct { const char* table; int type; int nameOffset; const wchar_t* kind; } kinds[] = {
            { "Character", kCharacter, 0x18, L"Character" }, { "CharacterAvatar", kAvatar, 0x1C, L"Costume" },
            { "Weapon", kWeapon, 0x18, L"Weapon" }, { "SubWeapon", kSubWeapon, 0x18, L"Sub-weapon" },
            { "Protector", kProtector, 0x18, L"Armor" }, { "Accessory", kAccessory, 0x18, L"Accessory" },
            { "MaterialItem", kMaterial, 0x18, L"Material" }, { "CommonItem", kCommonItem, 0x18, L"Item" },
            { "Food", kFood, 0x18, L"Food" }, { "Fragment", kFragment, 0x18, L"Soul fragment" },
            { "JukeboxMusic", kJukebox, 0x18, L"Music" } };
        auto field = [](Il2CppObject* row, int offset) { return *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(row) + offset); };
        auto nameOf = [&](int nameId) {
            auto it = names.find(nameId);
            return it != names.end() ? it->second : std::wstring();
        };
        std::unordered_map<int, std::wstring> characters;  // costumes say whose they are
        for (const auto& row : TableRows("Character"))
            characters[row.first] = nameOf(field(row.second, 0x18));
        std::vector<ItemEntry> items;
        for (const auto& k : kinds)
        {
            for (const auto& row : TableRows(k.table))
            {
                std::wstring name = nameOf(field(row.second, k.nameOffset));
                if (name.empty() || name == L"NULL")
                    continue;  // placeholders
                if (k.type == kAvatar)
                {
                    auto owner = characters.find(field(row.second, 0x18));  // charaId
                    if (owner != characters.end() && !owner->second.empty() && owner->second != name)
                        name = owner->second + L" - " + name;
                }
                items.push_back({ k.type, row.first, std::wstring(k.kind) + L": " + name });
            }
        }
        size_t n = items.size();
        {
            std::lock_guard<std::mutex> lock(g_itemsLock);
            g_items = std::move(items);
        }
        if (g_window)
            PostMessageW(g_window, WM_ITEMS, 0, 0);
        return "Item list ready (" + std::to_string(n) + " entries)";
    }

    std::atomic<int> g_selectedItem{ -1 };

    std::string GiveSelectedItem()
    {
        ItemEntry e;
        {
            std::lock_guard<std::mutex> lock(g_itemsLock);
            int i = g_selectedItem;
            if (i < 0 || size_t(i) >= g_items.size())
                return "Pick something in the list first";
            e = g_items[size_t(i)];
        }
        Il2CppObject* save = SaveData();
        // owned-once things take a count of 1
        bool unique = e.type == kCharacter || e.type == kAvatar || e.type == kAccessory || e.type == kJukebox;
        int count = unique ? 1 : std::max(1, std::min(int(g_amount), 9999));
        bool ok = AddContents(save, e.type, e.id, count) != 0;
        Commit(save);
        std::string label(e.label.size() * 3, '\0');  // UTF-8 takes at most 3 bytes per UTF-16 unit
        label.resize(size_t(WideCharToMultiByte(CP_UTF8, 0, e.label.c_str(), int(e.label.size()), label.data(), int(label.size()), nullptr, nullptr)));
        return std::string(ok ? "Given: " : "Not given (owned already or at the limit): ") + label + (unique ? "" : " x" + std::to_string(count));
    }

    // Unity pauses the game while its window is unfocused; while the menu is
    // open the game keeps running so that clicks on the menu take effect.
    void ApplyRunInBackground()
    {
        // the managed accessors are stripped from the game; the engine's
        // internal calls are still registered
        using Getter = bool (*)();
        using Setter = void (*)(bool);
        static auto get = reinterpret_cast<Getter>(il2cpp_resolve_icall("UnityEngine.Application::get_runInBackground()"));
        static auto set = reinterpret_cast<Setter>(il2cpp_resolve_icall("UnityEngine.Application::set_runInBackground(System.Boolean)"));
        static int state = -1;  // last value set, -1 untouched
        static bool original = false;
        bool want = g_menuVisible;
        if (!set || (state == -1 && !want) || state == int(want))
        {
            static bool reported = false;
            if (!set && want && !reported)
                reported = true, fprintf(stderr, "debug menu: Application.set_runInBackground is not available\n");
            return;
        }
        if (state == -1)
            original = get && get();
        set(want || original);
        state = int(want);
    }

    std::vector<int32_t> g_macTables[3], g_pcTables[3];  // PS4, Xbox, Xbox Elite
    bool g_pcLayout = false;

    void ApplyControllerLayout()
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        wchar_t layout[16] = {};
        GetPrivateProfileStringW(L"controller", L"layout", L"pc", layout, 16, g_iniPath.c_str());
        if (!_wcsicmp(layout, L"mac"))
            return;
        try
        {
            Il2CppObject* joystick = Singleton("gsJoystick");
            // The Mac tables also feed the options' Default button: they get the
            // PC contents (the originals are kept to translate assignments
            // saved with them, see FixControllerAssignment).
            const struct { const char* mac; const char* pc; } pairs[] = {
                { "TRIGGERS_MAC_PS4", "TRIGGERS_PC_PS4" }, { "TRIGGERS_MAC_XBOX", "TRIGGERS_PC_XBOX" },
                { "TRIGGERS_MAC_XBOX_ELITE", "TRIGGERS_PC_XBOX" } };
            for (size_t t = 0; t < std::size(pairs); t++)
            {
                Il2CppArray* mac = nullptr;
                Il2CppArray* pc = nullptr;
                il2cpp_field_static_get_value(FindField(il2cpp_object_get_class(joystick), pairs[t].mac), &mac);
                il2cpp_field_static_get_value(FindField(il2cpp_object_get_class(joystick), pairs[t].pc), &pc);
                if (!mac || !pc || il2cpp_array_length(mac) != il2cpp_array_length(pc) || il2cpp_array_length(mac) > 32)
                    continue;
                auto* m = reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(mac) + kIl2CppSizeOfArray);
                auto* p = reinterpret_cast<const int32_t*>(reinterpret_cast<uint8_t*>(pc) + kIl2CppSizeOfArray);
                g_macTables[t].assign(m, m + il2cpp_array_length(mac));
                g_pcTables[t].assign(p, p + il2cpp_array_length(pc));
                memcpy(m, p, il2cpp_array_length(pc) * sizeof(int32_t));
            }
            FieldInfo* type = FindField(il2cpp_object_get_class(joystick), "m_defaultType");
            int pc = 0;  // gsJoystick.KeyMapDefaultType.PC
            il2cpp_field_set_value(joystick, type, &pc);
            // re-derive the button assignment for the controller in use
            if (Il2CppObject* module = GetField<Il2CppObject*>(joystick, "m_standaloneInputModule"))
            {
                int joystickType = GetField<int32_t>(module, "m_joystickType");
                Call(joystick, "SetDefaultAssign", &joystickType);
            }
            g_pcLayout = true;
            fprintf(stderr, "GoSRecomp: controller tables switched to the game's PC ones\n");
        }
        catch (const Failure& f)
        {
            fprintf(stderr, "GoSRecomp: controller layout: %s\n", f.what.c_str());
        }
    }

    // The Input System of the macOS build has no XInputControllerWindows, so
    // Unity's XInput devices (Xbox pads on Windows) never become gamepads:
    // the game then misses LT/RT and the d-pad in menus, and its button hints
    // show the generic HID names ("Button11"). The same layout, registered as
    // JSON: XINPUT_GAMEPAD as Unity's Windows backend reports it ("XINP").
    void RegisterXInputLayout()
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        static const char kLayout[] = R"({
"name": "XInputControllerWindows",
"extend": "XInputController",
"format": "XINP",
"device": { "interface": "XInput" },
"controls": [
  { "name": "dpad", "layout": "Dpad", "offset": 0, "bit": 0, "sizeInBits": 4 },
  { "name": "dpad/up", "offset": 0, "bit": 0 },
  { "name": "dpad/down", "offset": 0, "bit": 1 },
  { "name": "dpad/left", "offset": 0, "bit": 2 },
  { "name": "dpad/right", "offset": 0, "bit": 3 },
  { "name": "start", "offset": 0, "bit": 4, "displayName": "Start" },
  { "name": "select", "offset": 0, "bit": 5, "displayName": "Select" },
  { "name": "leftStickPress", "offset": 0, "bit": 6 },
  { "name": "rightStickPress", "offset": 0, "bit": 7 },
  { "name": "leftShoulder", "offset": 0, "bit": 8 },
  { "name": "rightShoulder", "offset": 0, "bit": 9 },
  { "name": "buttonSouth", "offset": 0, "bit": 12, "displayName": "A" },
  { "name": "buttonEast", "offset": 0, "bit": 13, "displayName": "B" },
  { "name": "buttonWest", "offset": 0, "bit": 14, "displayName": "X" },
  { "name": "buttonNorth", "offset": 0, "bit": 15, "displayName": "Y" },
  { "name": "leftTrigger", "offset": 2, "format": "BYTE" },
  { "name": "rightTrigger", "offset": 3, "format": "BYTE" },
  { "name": "leftStick", "layout": "Stick", "offset": 4, "format": "VC2S" },
  { "name": "leftStick/x", "offset": 0, "format": "SHRT", "parameters": "clamp=false,invert=false,normalize=false" },
  { "name": "leftStick/left", "offset": 0, "format": "SHRT", "parameters": "clamp=true,clampMin=-1,clampMax=0,invert" },
  { "name": "leftStick/right", "offset": 0, "format": "SHRT", "parameters": "clamp=true,clampMin=0,clampMax=1" },
  { "name": "leftStick/y", "offset": 2, "format": "SHRT", "parameters": "clamp=false,invert=false,normalize=false" },
  { "name": "leftStick/down", "offset": 2, "format": "SHRT", "parameters": "clamp=true,clampMin=-1,clampMax=0,invert" },
  { "name": "leftStick/up", "offset": 2, "format": "SHRT", "parameters": "clamp=true,clampMin=0,clampMax=1" },
  { "name": "rightStick", "layout": "Stick", "offset": 8, "format": "VC2S" },
  { "name": "rightStick/x", "offset": 0, "format": "SHRT", "parameters": "clamp=false,invert=false,normalize=false" },
  { "name": "rightStick/left", "offset": 0, "format": "SHRT", "parameters": "clamp=true,clampMin=-1,clampMax=0,invert" },
  { "name": "rightStick/right", "offset": 0, "format": "SHRT", "parameters": "clamp=true,clampMin=0,clampMax=1" },
  { "name": "rightStick/y", "offset": 2, "format": "SHRT", "parameters": "clamp=false,invert=false,normalize=false" },
  { "name": "rightStick/down", "offset": 2, "format": "SHRT", "parameters": "clamp=true,clampMin=-1,clampMax=0,invert" },
  { "name": "rightStick/up", "offset": 2, "format": "SHRT", "parameters": "clamp=true,clampMin=0,clampMax=1" }
] })";
        wchar_t on[8] = {};
        GetPrivateProfileStringW(L"controller", L"xinput", L"1", on, 8, g_iniPath.c_str());
        if (on[0] == L'0')
            return;
        try
        {
            Il2CppClass* inputSystem = nullptr;
            size_t n = 0;
            const Il2CppAssembly** assemblies = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &n);
            for (size_t i = 0; i < n && !inputSystem; i++)
                inputSystem = il2cpp_class_from_name(il2cpp_assembly_get_image(assemblies[i]), "UnityEngine.InputSystem", "InputSystem");
            if (!inputSystem)
                throw Failure{ "InputSystem not found" };
            Il2CppString* json = il2cpp_string_new(kLayout);
            alignas(8) uint8_t noMatcher[16] = {};  // InputDeviceMatcher? = null (the layout has its own)
            void* args[3] = { json, nullptr, noMatcher };
            // RegisterLayout(string json, string name, InputDeviceMatcher? matches), not the Type overload
            const MethodInfo* registerJson = nullptr;
            void* iter = nullptr;
            while (const MethodInfo* m = il2cpp_class_get_methods(inputSystem, &iter))
                if (!strcmp(il2cpp_method_get_name(m), "RegisterLayout") && il2cpp_method_get_param_count(m) == 3 &&
                    !strcmp(il2cpp_type_get_name(il2cpp_method_get_param(m, 0)), "System.String"))
                    registerJson = m;
            if (!registerJson)
                throw Failure{ "RegisterLayout(string, ...) not found" };
            Invoke(registerJson, nullptr, args);
            fprintf(stderr, "GoSRecomp: Input System layout for XInput controllers registered\n");
        }
        catch (const Failure& f)
        {
            fprintf(stderr, "GoSRecomp: XInput layout: %s\n", f.what.c_str());
        }
    }

    // m_config can still hold Mac numbers, or another pad's (the game keeps a
    // custom assignment when the pad changes). Sticks and d-pad always get the
    // PC values; buttons sitting on some table's default get the PC default.
    // Skipped with no pad connected, since the game then assumes Xbox.
    void FixControllerAssignment(Il2CppObject* joystick, int joystickType)
    {
        if (!g_pcLayout || joystickType < 0)
            return;
        size_t t = joystickType == 2 || joystickType == 3 ? 0 : joystickType == 1 ? 2 : joystickType == 0 ? 1 : 3;
        if (t > 2 || g_pcTables[t].empty())
            return;
        auto* config = GetField<Il2CppArray*>(joystick, "m_config");
        if (!config)
            return;
        auto* c = reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(config) + kIl2CppSizeOfArray);
        size_t n = std::min<size_t>(il2cpp_array_length(config), g_pcTables[t].size());
        int fixed = 0;
        for (size_t i = 0; i < n; i++)
        {
            bool stickOrPad = i <= 7 || (i >= 20 && i <= 23);
            int32_t want = g_pcTables[t][i];
            if (c[i] == want)
                continue;
            bool otherDefault = false;  // left there by another table, not chosen
            for (size_t u = 0; u < 3; u++)
                if (i < g_pcTables[u].size() && (c[i] == g_pcTables[u][i] || c[i] == g_macTables[u][i]))
                    otherDefault = true;
            if (stickOrPad || otherDefault)
                c[i] = want, fixed++;
        }
        if (fixed)
            fprintf(stderr, "controller: %d entries of the assignment set to the PC numbering (type %d)\n", fixed, joystickType);
    }

    std::vector<std::string> ShownUis();

    // Game state in the log, on every change: the scene, the tutorial step and
    // the tutorial sequence playing, to see where the game waits when it
    // seems stuck.
    // The out-of-battle player characters (ope_og: hub, roster) and their
    // Animator, on every change, for the T-pose some players get there: a
    // character stuck on the base controller's placeholder clip (length 1.00,
    // one clip playing) instead of its overridden animations.
    void LogHubCharacters()
    {
        static std::string last;
        static int logged = 0;
        std::string state;
        try
        {
            static Il2CppClass* og = GameClass("ope_og");
            static Il2CppClass* object = [] {
                size_t n = 0;
                const Il2CppAssembly** assemblies = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &n);
                for (size_t i = 0; i < n; i++)
                    if (Il2CppClass* k = il2cpp_class_from_name(il2cpp_assembly_get_image(assemblies[i]), "UnityEngine", "Object"))
                        return k;
                return static_cast<Il2CppClass*>(nullptr);
            }();
            if (!object)
                return;
            void* args[1] = { il2cpp_type_get_object(il2cpp_class_get_type(og)) };
            auto* found = reinterpret_cast<Il2CppArray*>(Invoke(FindMethod(object, "FindObjectsOfType", 1), nullptr, args));
            if (!found)
                return;
            auto** items = reinterpret_cast<Il2CppObject**>(reinterpret_cast<uint8_t*>(found) + kIl2CppSizeOfArray);
            for (size_t i = 0; i < il2cpp_array_length(found); i++)
            {
                Il2CppObject* c = items[i];
                auto text = [](Il2CppObject* str) {
                    std::string out;
                    for (wchar_t ch : Widen(reinterpret_cast<Il2CppString*>(str)))
                        out += ch < 128 ? char(ch) : '?';
                    return out;
                };
                auto get = [&](auto f) {
                    try
                    {
                        return f();
                    }
                    catch (const Failure&)
                    {
                        return std::string("?");
                    }
                };
                state += "\n  " + get([&] { return text(Call(Call(c, "get_gameObject"), "get_name")); });
                state += get([&] { return std::string(Active(c) ? "" : " (hidden)"); });
                Il2CppObject* animator = nullptr;
                try
                {
                    animator = Call(c, "get_animator");
                }
                catch (const Failure&)
                {
                }
                if (!animator)
                {
                    state += ": no animator";
                    continue;
                }
                char buf[256];
                // engine calls directly: the game's build stripped most of these managed getters
                using BoolGet = bool (*)(Il2CppObject*);
                using FloatGet = float (*)(Il2CppObject*);
                using IntGet = int32_t (*)(Il2CppObject*);
                using ObjGet = Il2CppObject* (*)(Il2CppObject*);
                static auto enabled = reinterpret_cast<BoolGet>(il2cpp_resolve_icall("UnityEngine.Behaviour::get_isActiveAndEnabled()"));
                static auto initialized = reinterpret_cast<BoolGet>(il2cpp_resolve_icall("UnityEngine.Animator::get_isInitialized()"));
                static auto speed = reinterpret_cast<FloatGet>(il2cpp_resolve_icall("UnityEngine.Animator::get_speed()"));
                static auto culling = reinterpret_cast<IntGet>(il2cpp_resolve_icall("UnityEngine.Animator::get_cullingMode()"));
                static auto controller = reinterpret_cast<ObjGet>(il2cpp_resolve_icall("UnityEngine.Animator::get_runtimeAnimatorController()"));
                static auto avatar = reinterpret_cast<ObjGet>(il2cpp_resolve_icall("UnityEngine.Animator::get_avatar()"));
                static auto clipsOf = reinterpret_cast<ObjGet>(il2cpp_resolve_icall("UnityEngine.RuntimeAnimatorController::get_animationClips()"));
                static auto nameOf = reinterpret_cast<ObjGet>(il2cpp_resolve_icall("UnityEngine.Object::GetName(UnityEngine.Object)"));
                snprintf(buf, sizeof buf, ": animator %s%s, speed %g, culling %d", enabled && enabled(animator) ? "on" : "off",
                         initialized && !initialized(animator) ? ", not initialized" : "", speed ? double(speed(animator)) : -1.0,
                         culling ? culling(animator) : -1);
                state += buf;
                Il2CppObject* av = avatar ? avatar(animator) : nullptr;
                state += av ? ", avatar " + text(nameOf ? nameOf(av) : nullptr) : std::string(", no avatar");
                Il2CppObject* ctrl = controller ? controller(animator) : nullptr;
                if (!ctrl)
                    state += ", no controller";
                else
                {
                    state += ", controller " + text(nameOf ? nameOf(ctrl) : nullptr) + " (" + il2cpp_class_get_name(il2cpp_object_get_class(ctrl)) + ")";
                    auto* clips = clipsOf ? reinterpret_cast<Il2CppArray*>(clipsOf(ctrl)) : nullptr;
                    size_t total = clips ? il2cpp_array_length(clips) : 0, missing = 0;
                    for (size_t k = 0; k < total; k++)
                        if (!reinterpret_cast<Il2CppObject**>(reinterpret_cast<uint8_t*>(clips) + kIl2CppSizeOfArray)[k])
                            missing++;
                    snprintf(buf, sizeof buf, ", %zu clips (%zu missing)", total, missing);
                    state += buf;
                }
                state += get([&] {
                    int32_t layer = 0;
                    auto* info = static_cast<int32_t*>(il2cpp_object_unbox(Call(animator, "GetCurrentAnimatorStateInfo", &layer)));
                    auto* f = reinterpret_cast<float*>(info);
                    snprintf(buf, sizeof buf, ", state %08x length %.2f speed %g", uint32_t(info[2]), double(f[4]), double(f[5]));
                    return std::string(buf);
                });
                // clips playing on the base layer (the placeholder counts as one)
                using ClipCount = int32_t (*)(Il2CppObject*, int32_t, bool);
                static auto clipCount = reinterpret_cast<ClipCount>(
                    il2cpp_resolve_icall("UnityEngine.Animator::GetAnimatorClipInfoCount(System.Int32,System.Boolean)"));
                int32_t playing = clipCount ? clipCount(animator, 0, true) : -1;
                snprintf(buf, sizeof buf, ", %d clip(s) playing", playing);
                state += buf;
            }
        }
        catch (const Failure& f)
        {
            state = " (" + f.what + ")";
        }
        if (state != last && logged < 300)
        {
            last = state;
            logged++;
            fprintf(stderr, "player characters [%.1f s]:%s\n", double(GetTickCount64()) / 1000.0, state.empty() ? " none" : state.c_str());
        }
    }

    void LogGameState()
    {
        static std::string last;
        static int logged = 0;
        if (logged > 2000)
            return;
        std::string state;
        try
        {
            Il2CppObject* scenes = Singleton("bsSceneManager");
            state += "scene " + std::to_string(GetField<int32_t>(scenes, "<Current>k__BackingField")) + " next " +
                     std::to_string(GetField<int32_t>(scenes, "<Next>k__BackingField")) +
                     (GetField<bool>(scenes, "_isSceneActive") ? "" : " (inactive)");
        }
        catch (const Failure&)
        {
        }
        try
        {
            Il2CppObject* tutorial = Singleton("outTutorialManager");
            state += ", tutorial " + std::to_string(GetField<int32_t>(tutorial, "_tutorial"));
            if (Il2CppObject* now = GetField<Il2CppObject*>(tutorial, "nowPlay"))
                state += std::string(" playing ") + il2cpp_class_get_name(il2cpp_object_get_class(now)) +
                         (GetField<bool>(now, "IsFinish") ? " (finished)" : "");
            // the tutorial's highlight (mvFocusCursor): where it is and what it lets through
            auto describe = [](Il2CppObject* unityObject) {
                std::string out;
                try
                {
                    for (wchar_t ch : Widen(reinterpret_cast<Il2CppString*>(Call(unityObject, "get_name"))))
                        out += ch < 128 ? char(ch) : '?';
                    Il2CppObject* go = Call(unityObject, "get_gameObject");
                    out += go && Bool(Call(go, "get_activeInHierarchy")) ? "" : " (hidden)";
                }
                catch (const Failure&)
                {
                    out += "?";
                }
                return out;
            };
            if (Il2CppObject* tool = GetField<Il2CppObject*>(tutorial, "_tool"))
                if (Il2CppObject* focus = GetField<Il2CppObject*>(tool, "m_focus"))
                {
                    state += ", focus " + describe(focus);
                    if (Il2CppObject* t = GetField<Il2CppObject*>(focus, "m_transform"))
                    {
                        float* v = static_cast<float*>(il2cpp_object_unbox(Call(t, "get_localPosition")));
                        char buf[96];
                        snprintf(buf, sizeof buf, " at %.0f,%.0f,%.0f", v[0], v[1], v[2]);
                        state += buf;
                    }
                    if (Il2CppObject* blocker = GetField<Il2CppObject*>(focus, "m_touchBlocker"))
                        state += ", blocker " + describe(blocker);
                    ForEachInList(GetField<Il2CppObject*>(focus, "m_controlTargetList"), [&](Il2CppObject* target) {
                        if (Il2CppObject* t = GetField<Il2CppObject*>(target, "m_touchTarget"))
                            state += ", touchable " + describe(t);
                    });
                }
        }
        catch (const Failure&)
        {
        }
        try
        {
            // the hub tutorial steps wait for the header (outTutorialBase.WaitHeader)
            std::string screens;
            for (const std::string& name : ShownUis())
                screens += (screens.empty() ? "" : " ") + name;
            state += ", screens: " + (screens.empty() ? std::string("none") : screens);
            Il2CppObject* header = GetField<Il2CppObject*>(Singleton("outUiManager"), "m_homeheader");
            if (!header)
                state += ", no header";
            else if (!GetField<void*>(header, "m_CachedPtr"))
                state += ", header destroyed";
        }
        catch (const Failure&)
        {
        }
        try
        {
            state += ", saved tutorial step " + std::to_string(Int(Call(SaveData(), "GetTutorialId")));
        }
        catch (const Failure&)
        {
        }
        if (state != last)
        {
            last = state;
            logged++;
            fprintf(stderr, "state [%.1f s]: %s\n", double(GetTickCount64()) / 1000.0, state.c_str());
        }
    }

    // The UI screens kept by outUiManager (its ui_cache).
    template <typename F>
    void ForEachCachedUi(F f)
    {
        Il2CppObject* dict = GetField<Il2CppObject*>(Singleton("outUiManager"), "ui_cache");
        if (!dict)
            return;
        auto* entries = GetField<Il2CppArray*>(dict, "entries");
        int32_t count = GetField<int32_t>(dict, "count");
        // Dictionary<string, ref>.Entry: hashCode, next, key, value
        struct Entry { int32_t hashCode, next; Il2CppObject* key; Il2CppObject* value; };
        if (!entries ||
            il2cpp_array_element_size(il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(entries))) != sizeof(Entry))
            return;
        auto* e = reinterpret_cast<Entry*>(reinterpret_cast<uint8_t*>(entries) + kIl2CppSizeOfArray);
        for (int32_t i = 0; i < count && i < int32_t(il2cpp_array_length(entries)); i++)
            if (e[i].hashCode >= 0 && e[i].value)
                f(e[i].value);
    }

    // The class names of the cached UI screens that are showing.
    std::vector<std::string> ShownUis()
    {
        std::vector<std::string> names;
        ForEachCachedUi([&](Il2CppObject* ui) {
            if (Active(ui))
                names.push_back(il2cpp_class_get_name(il2cpp_object_get_class(ui)));
        });
        std::sort(names.begin(), names.end());
        return names;
    }

    // Softlock after the first summon: the hub screen sets up the step's
    // highlight (SetViewContent_tutorial), then OpenTutorialUI resets the
    // tutorial tools and hides it again. If a hub step is done, the hub screen
    // is up and the highlight has been hidden for 10 s, set the step up again.
    void RecoverHubTutorial()
    {
        static uint64_t hiddenSince = 0;
        static int32_t recovered = 0;
        bool stuck = false;
        int32_t step = 0;
        Il2CppObject* ui = nullptr;
        try
        {
            Il2CppObject* scenes = Singleton("bsSceneManager");
            Il2CppObject* tutorial = Singleton("outTutorialManager");
            step = GetField<int32_t>(tutorial, "_tutorial");
            Il2CppObject* now = GetField<Il2CppObject*>(tutorial, "nowPlay");
            Il2CppObject* tool = GetField<Il2CppObject*>(tutorial, "_tool");
            Il2CppObject* focus = tool ? GetField<Il2CppObject*>(tool, "m_focus") : nullptr;
            if (GetField<int32_t>(scenes, "<Current>k__BackingField") == 3 && GetField<bool>(scenes, "_isSceneActive") &&
                step >= 10003 && step <= 10012 && step != recovered && now && GetField<bool>(now, "IsFinish") && focus &&
                !Active(focus))
            {
                // the hub screen showing, and no other screen over it
                std::vector<std::string> shown = ShownUis();
                bool hubOnly = std::find(shown.begin(), shown.end(), "homeUi_Tutorial") != shown.end() &&
                               std::all_of(shown.begin(), shown.end(), [](const std::string& n) {
                                   return n == "homeUi" || n == "homeUi_Tutorial";
                               });
                if (hubOnly)
                    ForEachCachedUi([&](Il2CppObject* u) {
                        if (!strcmp(il2cpp_class_get_name(il2cpp_object_get_class(u)), "homeUi_Tutorial"))
                            ui = u;
                    });
                stuck = ui != nullptr;
            }
        }
        catch (const Failure&)
        {
        }
        if (!stuck)
        {
            hiddenSince = 0;
            return;
        }
        uint64_t now = GetTickCount64();
        if (!hiddenSince)
            hiddenSince = now;
        if (now - hiddenSince < 10000)
            return;
        recovered = step;
        hiddenSince = 0;
        try
        {
            Il2CppObject* callback = GetField<Il2CppObject*>(ui, "targetMoveEndCallBack");
            Call(ui, "SetViewContent_tutorial", &step, callback);
            fprintf(stderr, "GoSRecomp: hub tutorial step %d had no highlight, set up again\n", step);
        }
        catch (const Failure& f)
        {
            fprintf(stderr, "GoSRecomp: hub tutorial step %d: %s\n", step, f.what.c_str());
        }
    }

    // Controller report in the log (gosr_log.txt): the connected controller,
    // the type the game detected, and the first moves past half range of each
    // joystick axis, so a stick that does nothing can be traced to its axis.
    void LogControllerInput()
    {
        using AxisRaw = float (*)(Il2CppString*);
        static auto getAxis = reinterpret_cast<AxisRaw>(il2cpp_resolve_icall("UnityEngine.Input::GetAxisRaw(System.String)"));
        static std::vector<uint32_t> names;  // GC handles: native statics are not scanned by the GC
        static int lastSign[16] = {};
        static int logged = 0;
        static std::string lastDevice;
        static int lastType = -1;
        if (!getAxis || logged > 400)
            return;
        if (names.empty())
        {
            for (int i = 1; i <= 16; i++)
            {
                char name[16];
                snprintf(name, sizeof name, "axis%02d", i);
                names.push_back(il2cpp_gchandle_new(reinterpret_cast<Il2CppObject*>(il2cpp_string_new(name)), true));
            }
        }
        try
        {
            static int frame = 0;
            if (frame++ % 30 == 0)  // the device and type, when they change
            {
                Il2CppObject* joystick = Singleton("gsJoystick");
                auto* device = GetField<Il2CppString*>(joystick, "<connectedDeviceName>k__BackingField");
                std::string name;
                for (wchar_t ch : Widen(device))
                    name += ch < 128 ? char(ch) : '?';
                int type = -1;
                if (Il2CppObject* module = GetField<Il2CppObject*>(joystick, "m_standaloneInputModule"))
                    type = GetField<int32_t>(module, "m_joystickType");
                if (!name.empty())
                    FixControllerAssignment(joystick, type);
                if (name != lastDevice || type != lastType)
                {
                    lastDevice = name;
                    lastType = type;
                    bool inputSystem = GetField<Il2CppObject*>(joystick, "m_currentDevice") != nullptr;
                    fprintf(stderr, "controller: \"%s\", type %d (0 Xbox, 1 Xbox Elite, 2 PS4, 3 PS5, 4 MFi), %s\n", name.c_str(),
                            type, inputSystem ? "Input System gamepad" : "no Input System gamepad");
                    logged++;
                }
            }
        }
        catch (const Failure&)
        {
        }
        for (int i = 0; i < 16; i++)
        {
            float v = getAxis(reinterpret_cast<Il2CppString*>(il2cpp_gchandle_get_target(names[size_t(i)])));
            int sign = v > 0.5f ? 1 : (v < -0.5f ? -1 : 0);
            if (sign != lastSign[i])
            {
                lastSign[i] = sign;
                if (sign)
                {
                    fprintf(stderr, "controller: axis%02d = %.2f\n", i + 1, v);
                    logged++;
                }
            }
        }
    }

    // --------------------------------------------------------- main-thread hook

    void RunPending()
    {
        recomp::ApplyDisplaySettings();
        RegisterXInputLayout();
        ApplyControllerLayout();
        LogControllerInput();
        static int stateFrame = 0;
        if (stateFrame++ % 30 == 0)
        {
            LogGameState();
            RecoverHubTutorial();
            LogHubCharacters();
        }
        ApplyRunInBackground();
        RunBatches();
        if (!g_pending.exchange(false))
            return;
        std::vector<std::function<std::string()>> work;
        {
            std::lock_guard<std::mutex> lock(g_queueLock);
            work.swap(g_queue);
        }
        for (auto& action : work)
        {
            try
            {
                SetStatus(action());
            }
            catch (const Failure& f)
            {
                SetStatus("Failed: " + f.what);
            }
        }
    }

    void HookedUpdate(void* self, const void* method)
    {
        RunPending();
        g_origUpdate(self, method);
    }

    void Queue(std::function<std::string()> action)
    {
        {
            std::lock_guard<std::mutex> lock(g_queueLock);
            g_queue.push_back(std::move(action));
        }
        g_pending = true;
        SetStatus("Working...");
    }

    // ------------------------------------------------------------------- window

    struct Button
    {
        const wchar_t* group;  // starts a new section when set
        const wchar_t* label;
        std::function<std::string()> action;
        bool immediate = false;  // runs on the menu's thread (settings, not save changes)
        std::function<std::wstring()> liveLabel;  // label after a click
    };

    std::wstring FactorLabel() { return L"Damage dealt x" + std::to_wstring(recomp::g_damageDealtFactor.load()); }
    std::wstring InvincibleLabel() { return recomp::g_invincible ? L"Invincible: ON" : L"Invincible: off"; }

    const std::vector<Button>& Buttons()
    {
        static const std::vector<Button> buttons = {
            { L"Resources", L"Add gold", AddGold },
            { nullptr, L"Add gems", AddGems },
            { nullptr, L"Max gold", MaxGold },
            { nullptr, L"Max gems", MaxGems },
            { nullptr, L"Max exchange points", MaxExchangePoints },
            { nullptr, L"All items x9999", AllItems },
            { nullptr, L"All soul fragments", [] { return GiveAll("Fragment", kFragment, nullptr, 9999, "Fragments"); } },
            { L"Unlocks", L"All characters", [] { return GiveAll("Character", kCharacter, "IsExistCharacter", 1, "Characters"); } },
            { nullptr, L"All costumes", [] { return GiveAll("CharacterAvatar", kAvatar, "IsExistCharacterAvatar", 1, "Costumes"); } },
            { nullptr, L"Max character levels", MaxCharacterLevels },
            { nullptr, L"Max equipment levels", MaxEquipmentLevels },
            { nullptr, L"All weapons", [] { return GiveAll("Weapon", kWeapon, "IsExistWeapon", 1, "Weapons"); } },
            { nullptr, L"All sub-weapons", [] { return GiveAll("SubWeapon", kSubWeapon, "IsExistSubWeapon", 1, "Sub-weapons"); } },
            { nullptr, L"All armor", [] { return GiveAll("Protector", kProtector, "IsExistProtector", 1, "Armor"); } },
            { nullptr, L"All accessories", [] { return GiveAll("Accessory", kAccessory, "IsExistAccessory", 1, "Accessories"); } },
            { nullptr, L"Museum and music", MuseumAndMusic },
            { L"Quests", L"Open all quests", [] { return OpenQuests(false); } },
            { nullptr, L"Clear all quests", [] { return OpenQuests(true); } },
            { L"Summon", L"Reset free pull", ResetFreePull },
            { nullptr, L"Open all banners", OpenAllBanners },
            { L"Progress", L"Skip the tutorial", SkipTutorial },
            { L"Battle", L"Invincible: off", [] {
                  recomp::g_invincible = !recomp::g_invincible;
                  return std::string(recomp::g_invincible ? "Invincible: your characters take no damage" : "Invincible off");
              }, true, InvincibleLabel },
            { nullptr, L"Damage dealt x1", [] {
                  static const int steps[] = { 1, 2, 5, 10, 100 };
                  int cur = recomp::g_damageDealtFactor, next = 1;
                  for (size_t i = 0; i < std::size(steps); i++)
                      if (steps[i] == cur)
                          next = steps[(i + 1) % std::size(steps)];
                  recomp::g_damageDealtFactor = next;
                  return "Enemies take x" + std::to_string(next) + " damage";
              }, true, FactorLabel },
        };
        return buttons;
    }

    constexpr int kColumns = 4, kButtonW = 170, kButtonH = 30, kGap = 8, kHeaderH = 22;
    constexpr int kButtonIdBase = 100;
    HWND g_statusLabel = nullptr;
    HWND g_amountEdit = nullptr;
    HWND g_itemCombo = nullptr;
    constexpr int kGiveItemId = 90, kItemComboId = 91;
    HFONT g_font = nullptr;

    // Shown once (remembered in gosr.ini): the actions change the save for good.
    bool ConfirmCheats(HWND owner)
    {
        if (GetPrivateProfileIntW(L"debug", L"warned", 0, g_iniPath.c_str()))
            return true;
        int answer = MessageBoxW(owner,
            L"The debug menu changes your save immediately and its changes cannot be undone.\n\n"
            L"Giving yourself resources, unlocks or cleared quests skips the game's progression "
            L"and can spoil the intended experience. Back up your save first "
            L"(\"Play with crash log.bat\" puts a copy in crash_report.zip).\n\n"
            L"Use the debug menu anyway?",
            L"GoSRecomp debug menu", MB_YESNO | MB_ICONWARNING | MB_TOPMOST);
        if (answer != IDYES)
            return false;
        WritePrivateProfileStringW(L"debug", L"warned", L"1", g_iniPath.c_str());
        return true;
    }

    void ReadAmount()
    {
        wchar_t text[32] = {};
        GetWindowTextW(g_amountEdit, text, 32);
        long long v = _wtoi64(text);
        g_amount = int(std::max(0ll, std::min(v, 2000000000ll)));
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_COMMAND:
            if (LOWORD(wp) == kItemComboId && HIWORD(wp) == CBN_SELCHANGE)
            {
                g_selectedItem = int(SendMessageW(g_itemCombo, CB_GETCURSEL, 0, 0));
                return 0;
            }
            if (HIWORD(wp) == BN_CLICKED && LOWORD(wp) == kGiveItemId && ConfirmCheats(hwnd))
            {
                ReadAmount();
                bool empty;
                {
                    std::lock_guard<std::mutex> lock(g_itemsLock);
                    empty = g_items.empty();
                }
                Queue(empty ? std::function<std::string()>(LoadItemList) : std::function<std::string()>(GiveSelectedItem));
                return 0;
            }
            if (HIWORD(wp) == BN_CLICKED && LOWORD(wp) >= kButtonIdBase && LOWORD(wp) < kButtonIdBase + Buttons().size() &&
                ConfirmCheats(hwnd))
            {
                const Button& b = Buttons()[LOWORD(wp) - kButtonIdBase];
                ReadAmount();
                if (b.immediate)
                {
                    SetStatus(b.action());
                    if (b.liveLabel)
                        SetWindowTextW(reinterpret_cast<HWND>(lp), b.liveLabel().c_str());
                }
                else
                {
                    Queue(b.action);
                }
            }
            return 0;
        case WM_ITEMS:
        {
            std::lock_guard<std::mutex> lock(g_itemsLock);
            SendMessageW(g_itemCombo, CB_RESETCONTENT, 0, 0);
            for (const auto& e : g_items)
                SendMessageW(g_itemCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(e.label.c_str()));
            g_selectedItem = -1;
            return 0;
        }
        case WM_STATUS:
        {
            std::string s;
            {
                std::lock_guard<std::mutex> lock(g_statusLock);
                s = g_status;
            }
            std::wstring w(s.size() + 1, L'\0');
            w.resize(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), int(w.size())));
            SetWindowTextW(g_statusLabel, w.c_str());
            return 0;
        }
        case WM_MOUSEACTIVATE:
        {
            // buttons work without taking the focus from the game; only the
            // Amount field needs it, for typing
            POINT pt;
            GetCursorPos(&pt);
            HWND under = WindowFromPoint(pt);
            // typing needs the focus: the Amount field and the item list (whose
            // drop-down and edit parts are its children)
            return under == g_amountEdit || under == g_itemCombo || GetParent(under) == g_itemCombo ? MA_ACTIVATE : MA_NOACTIVATE;
        }
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            g_menuVisible = false;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void CreateMenuWindow()
    {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSW wc = {};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"GoSRecompDebugMenu";
        RegisterClassW(&wc);

        NONCLIENTMETRICSW ncm = { sizeof(ncm) };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);

        // lay out first to size the window
        struct Placed { int x, y, w, h; const wchar_t* text; int id; bool header; };
        std::vector<Placed> placed;
        int y = kGap, col = 0;
        int noteY = y;
        y += 2 * kHeaderH;
        int amountY = -1, pickerY = -1;
        const auto& buttons = Buttons();
        for (size_t i = 0; i < buttons.size(); i++)
        {
            if (buttons[i].group)
            {
                if (col)
                    y += kButtonH + kGap;
                col = 0;
                if (!wcscmp(buttons[i].group, L"Unlocks"))  // the item picker ends the resources
                {
                    pickerY = y;
                    y += kButtonH + kGap;
                }
                placed.push_back({ kGap, y, kColumns * (kButtonW + kGap), kHeaderH, buttons[i].group, 0, true });
                y += kHeaderH;
                if (i == 0)  // the Amount field heads the resources
                {
                    amountY = y;
                    y += kButtonH + kGap;
                }
            }
            placed.push_back({ kGap + col * (kButtonW + kGap), y, kButtonW, kButtonH, buttons[i].label, int(kButtonIdBase + i), false });
            if (++col == kColumns)
            {
                col = 0;
                y += kButtonH + kGap;
            }
        }
        if (col)
            y += kButtonH + kGap;
        int clientW = kGap + kColumns * (kButtonW + kGap);
        int statusY = y + kGap;
        int clientH = statusY + 54;

        RECT r = { 0, 0, clientW, clientH };
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        AdjustWindowRectEx(&r, style, FALSE, WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
        g_window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"GoSRecomp debug menu (F8)", style,
                                   80, 80, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
        for (const auto& p : placed)
        {
            HWND h = p.header ? CreateWindowExW(0, L"STATIC", p.text, WS_CHILD | WS_VISIBLE | SS_LEFT, p.x, p.y + 4, p.w, p.h - 4, g_window, nullptr, inst, nullptr)
                              : CreateWindowExW(0, L"BUTTON", p.text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, p.x, p.y, p.w, p.h, g_window,
                                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(p.id)), inst, nullptr);
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        }
        HWND note = CreateWindowExW(0, L"STATIC",
            L"Testing tools: every action changes your save right away and cannot be undone. "
            L"They skip the game's progression and can spoil the intended experience.",
            WS_CHILD | WS_VISIBLE | SS_LEFT, kGap, noteY, clientW - 2 * kGap, 2 * kHeaderH - 4, g_window, nullptr, inst, nullptr);
        SendMessageW(note, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        HWND amountLabel = CreateWindowExW(0, L"STATIC", L"Amount for Add gold / Add gems:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                           kGap, amountY + 7, kButtonW + kGap + 40, kButtonH - 7, g_window, nullptr, inst, nullptr);
        SendMessageW(amountLabel, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        g_amountEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"100000", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
                                       kGap + (kColumns - 1) * (kButtonW + kGap), amountY + 3, kButtonW, kButtonH - 6, g_window, nullptr, inst, nullptr);
        SendMessageW(g_amountEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        SendMessageW(g_amountEdit, EM_SETLIMITTEXT, 10, 0);
        // one item at a time: a searchable list (type to jump) and Give
        g_itemCombo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWNLIST,
                                      kGap, pickerY + 2, (kColumns - 1) * (kButtonW + kGap) - kGap, 400, g_window,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kItemComboId)), inst, nullptr);
        SendMessageW(g_itemCombo, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        SendMessageW(g_itemCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(click \"Give item\" to load the list)"));
        SendMessageW(g_itemCombo, CB_SETCURSEL, 0, 0);
        HWND give = CreateWindowExW(0, L"BUTTON", L"Give item (x Amount)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    kGap + (kColumns - 1) * (kButtonW + kGap), pickerY, kButtonW, kButtonH, g_window,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGiveItemId)), inst, nullptr);
        SendMessageW(give, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        g_statusLabel = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, kGap, statusY, clientW - 2 * kGap, 46,
                                        g_window, nullptr, inst, nullptr);
        SendMessageW(g_statusLabel, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        PostMessageW(g_window, WM_STATUS, 0, 0);
    }

    bool GameIsForeground()
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }

    DWORD WINAPI MenuThread(void* param)
    {
        int hotkey = int(reinterpret_cast<intptr_t>(param));
        CreateMenuWindow();
        SetTimer(g_window, 1, 50, nullptr);
        // testing: GOSR_DEBUG_MENU_SHOW=<x>[,<seconds>] shows the menu at that x after a delay
        wchar_t show[32] = {};
        int showX = -1;
        ULONGLONG showAt = 0;
        if (GetEnvironmentVariableW(L"GOSR_DEBUG_MENU_SHOW", show, 32))
        {
            showX = _wtoi(show);
            const wchar_t* comma = wcschr(show, L',');
            showAt = GetTickCount64() + (comma ? _wtoi(comma + 1) : 0) * 1000ull;
        }
        bool wasDown = false;
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            if (msg.message == WM_TIMER && msg.hwnd == g_window)
            {
                if (showX >= 0 && GetTickCount64() >= showAt)
                {
                    SetWindowPos(g_window, HWND_TOPMOST, showX, 0, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                    g_menuVisible = true;
                    showX = -1;
                }
                bool down = (GetAsyncKeyState(hotkey) & 0x8000) != 0;
                if (down && !wasDown && GameIsForeground())
                {
                    bool show = !IsWindowVisible(g_window);
                    g_menuVisible = show;
                    if (show)
                        SetWindowPos(g_window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                    else
                        ShowWindow(g_window, SW_HIDE);
                }
                wasDown = down;
                continue;
            }
            if (!IsDialogMessageW(g_window, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        return 0;
    }

    std::wstring IniPath()
    {
        wchar_t path[MAX_PATH * 4];
        DWORD len = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
        std::wstring p(path, len);
        return p.substr(0, p.find_last_of(L"\\/") + 1) + L"gosr.ini";
    }
}

namespace recomp
{
    void InstallDebugMenu(uint64_t imageBase)
    {
        std::wstring ini = IniPath();
        g_iniPath = ini;
        // the per-frame hook also applies the display settings (display.cpp)
        auto* slot = reinterpret_cast<UpdateFn*>(imageBase + kGsUpdateSlot);
        g_origUpdate = *slot;
        *slot = HookedUpdate;
        if (!GetPrivateProfileIntW(L"debug", L"menu", 1, ini.c_str()))
            return;
        wchar_t key[32] = {};
        GetPrivateProfileStringW(L"debug", L"hotkey", L"F8", key, DWORD(std::size(key)), ini.c_str());
        int hotkey = (key[0] == L'F' || key[0] == L'f') && key[1] ? VK_F1 + _wtoi(key + 1) - 1 : _wtoi(key);
        if (hotkey <= 0)
            hotkey = VK_F8;
        CreateThread(nullptr, 0, MenuThread, reinterpret_cast<void*>(intptr_t(hotkey)), 0, nullptr);
    }
}
