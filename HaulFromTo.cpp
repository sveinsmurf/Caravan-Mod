// HaulFromTo.cpp
// HaulFromTo - RE_Kenshi plugin fork based on the working SellingJob structure.
//
// This version is a DEVELOPMENT / STABILITY fork with VS2010/SDK compile fixes:
//   - It adds an "E" button only beside vanilla Haul To job rows (task key 124 from debug log).
//   - The vanilla Haul To job row is used as the UI/config anchor.
//   - The plugin disables the vanilla Haul To score only after that row is configured for HaulFromTo,
//     so Kenshi's native hauling AI stops fighting our source -> target logic.
//   - It uses a simple state machine: go source -> fill inventory -> go target -> deposit all -> repeat.
//   - It keeps minimal logs, locks source/target to the exact selected containers, and writes HaulFromTo_crash_report.txt/.dmp if the process crashes.
//   - v14 uses the backpack item inventory as carrier cargo and re-enables automatic carrier routing.
//
// Build locally with the same KenshiLib setup used by SellingJob.

#define BOOST_ALL_NO_LIB

#include <Debug.h>

#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/Character.h>
#include <kenshi/Inventory.h>
#include <kenshi/Item.h>
#include <kenshi/GameData.h>
#include <kenshi/RootObject.h>
#include <kenshi/Tasker.h>
#include <kenshi/GameSaveState.h>
#include <kenshi/Building/Building.h>
#include <kenshi/gui/ForgottenGUI.h>
#include <kenshi/gui/OrdersPanel.h>
#include <kenshi/gui/ScreenLabel.h>
#include <kenshi/util/hand.h>
#include <kenshi/util/lektor.h>

#include <mygui/MyGUI.h>
#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_EditBox.h>
#include <mygui/MyGUI_RenderManager.h>
#include <mygui/MyGUI_Delegate.h>
#include <mygui/MyGUI_TextBox.h>
#include <mygui/MyGUI_ScrollView.h>

#include <core/Functions.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>

#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <fstream>
#include <ctime>
#include <cmath>
#include <cctype>
#include <cstring>

// ---------------------------------------------------------------------------
// Debugging helpers
// ---------------------------------------------------------------------------

#define HFT_RANGE 260.0f
#define HFT_HAUL_JOB_TASK_KEY 124
#define HFT_STAY_CLOSE_ADD_TASK_KEY 31
#define HFT_STAY_CLOSE_REQUEST_TASK_KEY 31
#define HFT_STAY_CLOSE_ROW_TASK_KEY 289
#define HFT_BODYGUARD_TASK_KEY 45
// v30 confirmed stable task map:
// carrier: create 31 -> visible follow row 289 -> remove 289
// guard: create 45 -> creates rows 45 + 31 -> remove stored rows
#define HFT_MOVE_ORDER_COOLDOWN_TICKS 90
#define HFT_TRANSFER_MAX_UNITS 9999
#define HFT_PROBE_MAX_UNITS 9999
#define HFT_VERBOSE_LOG 1
#define HFT_RUNTIME_DEBUG_TOGGLE 1
#define HFT_DEBUG_TOGGLE_FILE "HaulFromTo_debug_on.txt"
#define HFT_CLEAN_ALPHA 1
#define HFT_NODE_RANGE 420.0f
#define HFT_CARRIER_TRANSFER_RANGE 420.0f
#define HFT_CARAVAN_LEASH_RANGE 1400.0f
#define HFT_CARAVAN_SOFT_LEASH_RANGE 850.0f
#define HFT_CARAVAN_NODE_ARRIVE_RANGE 650.0f
#define HFT_FOLLOW_LEADER_RANGE 900.0f
#define HFT_GROUP_LAUNCH_RANGE 520.0f
#define HFT_SELECTED_MOVE_REISSUE_TICKS 2400
#define HFT_POST_TRANSFER_SETTLE_TICKS 300
#define HFT_SETTLE_WAIT_LOG_TICKS 180
#define HFT_GROUP_WAIT_LOG_TICKS 240
#define HFT_SCORE_HEARTBEAT_GRACE_TICKS 240

class AI;

static std::string IntStr(int value)
{
    std::ostringstream ss;
    ss << value;
    return ss.str();
}

static std::string PtrStr(void* p)
{
    std::ostringstream ss;
    ss << p;
    return ss.str();
}

static bool HFTPathExists(const char* path)
{
    if (path == NULL || path[0] == 0)
        return false;

    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static std::string HFTModuleDirectory()
{
    HMODULE module = NULL;
    char path[MAX_PATH];

    path[0] = 0;

    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)&HFTModuleDirectory,
            &module))
        return "";

    DWORD len = GetModuleFileNameA(module, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return "";

    std::string dir(path);
    size_t slash = dir.find_last_of("\\/");
    if (slash == std::string::npos)
        return "";

    return dir.substr(0, slash);
}

static bool HFTDebugEnabledRaw()
{
    // Preferred: put this file next to HaulFromTo.dll:
    //   [Kenshi]\mods\HaulFromTo\HaulFromTo_debug_on.txt
    std::string dllDir = HFTModuleDirectory();
    if (!dllDir.empty())
    {
        std::string p = dllDir + "\\" + HFT_DEBUG_TOGGLE_FILE;
        if (HFTPathExists(p.c_str()))
            return true;
    }

    // Fallbacks for current working directory / game root installs.
    if (HFTPathExists(HFT_DEBUG_TOGGLE_FILE))
        return true;
    if (HFTPathExists("mods\\HaulFromTo\\HaulFromTo_debug_on.txt"))
        return true;
    if (HFTPathExists("mods/HaulFromTo/HaulFromTo_debug_on.txt"))
        return true;

    return false;
}

static bool HFTDebugEnabled()
{
#if HFT_RUNTIME_DEBUG_TOGGLE
    // Cache briefly so HFTLog can be called often without constantly hitting disk.
    // Creating/deleting the marker file will take effect within about one second.
    static bool cached = false;
    static DWORD lastCheck = 0;

    DWORD now = GetTickCount();
    if (lastCheck != 0 && now - lastCheck < 1000)
        return cached;

    lastCheck = now;
    cached = HFTDebugEnabledRaw();
    return cached;
#else
    return true;
#endif
}

static bool HFTIsStartupLog(const std::string& msg)
{
    return msg.find("startPlugin called.") != std::string::npos ||
           msg.find("hooked ") != std::string::npos ||
           msg.find("Ready v43c.") != std::string::npos;
}

static void HFTFileLog(const std::string& msg)
{
    std::ofstream f("HaulFromTo_debug.log", std::ios::app);
    if (!f.good())
        return;

    std::time_t now = std::time(NULL);
    struct tm* t = std::localtime(&now);
    if (t != NULL)
    {
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", t);
        f << "[" << buf << "] ";
    }
    f << msg << std::endl;
}

static void HFTLog(const std::string& msg)
{
#if HFT_RUNTIME_DEBUG_TOGGLE
    // Default: debug off. Keep only startup/hook/ready lines.
    // To enable full debug logging while experimenting, create:
    //   HaulFromTo_debug_on.txt
    // next to HaulFromTo.dll.
    if (!HFTDebugEnabled() && !HFTIsStartupLog(msg))
        return;
#endif

    HFTFileLog(msg);
    DebugLog(std::string("HaulFromTo: ") + msg);
}

static void HFTError(const std::string& msg)
{
    // Errors should always be written even when debug is off.
    HFTFileLog(std::string("ERROR: ") + msg);
    ErrorLog(std::string("HaulFromTo ERROR: ") + msg);
}

static void HFTTrace(const std::string& msg)
{
#if HFT_VERBOSE_LOG
    if (HFTDebugEnabled())
        HFTLog(msg);
#endif
}


// Lightweight crash reporter. It may also capture crashes caused by Kenshi or other mods,
// but it gives us a crash dump/report to inspect if HaulFromTo is involved.
static LONG WINAPI HFTUnhandledExceptionFilter(EXCEPTION_POINTERS* info)
{
    SYSTEMTIME st;
    GetLocalTime(&st);

    HANDLE report = CreateFileA("HaulFromTo_crash_report.txt", GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (report != INVALID_HANDLE_VALUE)
    {
        char buf[2048];
        DWORD written = 0;
        DWORD code = 0;
        void* addr = NULL;
        if (info != NULL && info->ExceptionRecord != NULL)
        {
            code = info->ExceptionRecord->ExceptionCode;
            addr = info->ExceptionRecord->ExceptionAddress;
        }

        int len = wsprintfA(buf,
            "HaulFromTo crash reporter\r\n"
            "Time: %04d-%02d-%02d %02d:%02d:%02d\r\n"
            "ExceptionCode: 0x%08X\r\n"
            "ExceptionAddress: %p\r\n"
            "Note: this process-level handler may also catch crashes from Kenshi/RE_Kenshi/other mods.\r\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            code, addr);
        WriteFile(report, buf, (DWORD)len, &written, NULL);
        CloseHandle(report);
    }

    HANDLE dump = CreateFileA("HaulFromTo_crash.dmp", GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (dump != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
                          MiniDumpWithDataSegs, info != NULL ? &mei : NULL, NULL, NULL);
        CloseHandle(dump);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

static bool IsValidPtr(void* p)
{
    if (p == NULL)
        return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)))
        return false;
    return (mbi.State == MEM_COMMIT)
        && (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
            | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static std::string SafeGameDataName(GameData* gd)
{
    if (gd == NULL || !IsValidPtr(gd))
        return "(none)";
    std::string name = gd->name;
    if (name.empty())
        name = gd->stringID;
    if (name.empty())
        name = "(unnamed)";
    return name;
}

static std::string BuildingName(Building* b)
{
    if (!IsValidPtr(b))
        return "(none)";
    std::string name = b->getName();
    if (name.empty())
    {
        GameData* gd = b->getGameData();
        if (gd != NULL && IsValidPtr(gd))
            name = gd->name;
    }
    if (name.empty())
        name = "(unnamed building)";
    return name;
}

static std::string CharacterName(Character* c)
{
    if (!IsValidPtr(c))
        return "(none)";
    std::string name = c->getName();
    if (name.empty())
    {
        GameData* gd = c->getGameData();
        if (gd != NULL && IsValidPtr(gd))
            name = gd->name;
    }
    if (name.empty())
        name = "(unnamed character)";
    return name;
}

// ---------------------------------------------------------------------------
// Per-character HaulFromTo configuration
// ---------------------------------------------------------------------------

static std::map<Character*, GameData*> g_haulItemForChar;
static std::map<Character*, Building*> g_sourceBuildingForChar;
static std::map<Character*, Building*> g_targetBuildingForChar;
static std::map<Character*, Character*> g_carrierForChar;
static std::map<Character*, Character*> g_guardForChar;
static std::map<Character*, hand> g_sourceHandForChar;
static std::map<Character*, hand> g_targetHandForChar;
static std::map<Character*, hand> g_carrierHandForChar;
static std::map<Character*, hand> g_guardHandForChar;
static std::map<Character*, bool> g_caravanModeForChar;
static std::map<Character*, Ogre::Vector3> g_sourceNodeForChar;
static std::map<Character*, Ogre::Vector3> g_targetNodeForChar;

// 0 = needs to go to source/fetch, 1 = carrying item and must go to target/deposit.
static std::map<Character*, int> g_stageForChar;
static std::map<Character*, int> g_lastMoveOrderTickForChar;
static std::map<Character*, int> g_lastLeashLogTickForChar;
static std::map<Character*, bool> g_carrierRealFollowJobForChar;
static std::map<Character*, bool> g_guardRealBodyguardJobForChar;
// v29b: exact permanent rows created on a guard when Set guard is clicked.
static std::map<Character*, std::vector<Tasker*> > g_guardCreatedRowsForChar;
static std::map<Character*, int> g_routeConfiguredTickForChar;
static std::map<Character*, int> g_lastHaulJobScoreTickForChar;

// Maps the visible Haul To job-row Tasker back to the character. This lets Tasker::score
// suppress only the vanilla haul job that the user configured through the C button.
static std::map<Tasker*, Character*> g_characterForTasker;

static int g_globalHFTTick = 0;
static std::map<Character*, int> g_lastCaravanProbeTickForChar;
static int g_squadProbeCounter = 0;
static std::map<Character*, void*> g_originalSquadForChar;
static std::map<Character*, Ogre::Vector3> g_lastSelectedSquadMoveDestForChar;
static std::map<Character*, int> g_lastSelectedSquadMoveTickForChar;
static std::map<Character*, int> g_lastGroupLaunchWaitLogTickForChar;
static std::map<Character*, int> g_nextCaravanLaunchTickForChar;
static std::map<Character*, int> g_lastSettleWaitLogTickForChar;
static PlayerInterface* g_lastPlayerInterface = NULL;

void (*newPlayerTaskSelectedCharacters_orig)(PlayerInterface*, TaskType, const hand&, Building*,
                                             const Ogre::Vector3&, bool) = NULL;

// v12: learned from a real player minimap/ground click.
// Default stays MOVE_CUS_ORDERED, but if Kenshi uses another TaskType for far map clicks,
// the PlayerInterface hook will learn it and IssuePointMoveOrder will reuse it.
static int g_learnedPointMoveTaskType = -1;
static Ogre::Vector3 g_lastLearnedPointMovePos = Ogre::Vector3::ZERO;

static Character* g_ordersCharacter = NULL;
static Character* g_pickCharacter = NULL;
static Tasker* g_pickTasker = NULL;

static GameData* GetHaulItemFor(Character* c)
{
    if (c == NULL)
        return NULL;
    std::map<Character*, GameData*>::iterator it = g_haulItemForChar.find(c);
    return it != g_haulItemForChar.end() ? it->second : NULL;
}

static Building* GetSourceBuildingFor(Character* c)
{
    if (c == NULL)
        return NULL;

    // v7: prefer the exact live pointer selected in this game session.
    // The persisted hand is only a fallback after loading a save or if the pointer becomes invalid.
    std::map<Character*, Building*>::iterator it = g_sourceBuildingForChar.find(c);
    if (it != g_sourceBuildingForChar.end())
    {
        if (it->second != NULL && IsValidPtr(it->second))
            return it->second;
        g_sourceBuildingForChar.erase(it);
    }

    std::map<Character*, hand>::iterator hIt = g_sourceHandForChar.find(c);
    if (hIt != g_sourceHandForChar.end())
    {
        Building* b = hIt->second.getBuilding();
        if (b != NULL && IsValidPtr(b))
        {
            g_sourceBuildingForChar[c] = b;
            return b;
        }
    }

    return NULL;
}

static Building* GetTargetBuildingFor(Character* c)
{
    if (c == NULL)
        return NULL;

    // v7: prefer the exact live pointer selected from the haul job row.
    // The persisted hand is only a fallback after loading a save or if the pointer becomes invalid.
    std::map<Character*, Building*>::iterator it = g_targetBuildingForChar.find(c);
    if (it != g_targetBuildingForChar.end())
    {
        if (it->second != NULL && IsValidPtr(it->second))
            return it->second;
        g_targetBuildingForChar.erase(it);
    }

    std::map<Character*, hand>::iterator hIt = g_targetHandForChar.find(c);
    if (hIt != g_targetHandForChar.end())
    {
        Building* b = hIt->second.getBuilding();
        if (b != NULL && IsValidPtr(b))
        {
            g_targetBuildingForChar[c] = b;
            return b;
        }
    }

    return NULL;
}

static Character* GetCarrierFor(Character* c)
{
    if (c == NULL)
        return NULL;

    std::map<Character*, Character*>::iterator it = g_carrierForChar.find(c);
    if (it != g_carrierForChar.end())
    {
        if (it->second != NULL && IsValidPtr(it->second))
            return it->second;
        g_carrierForChar.erase(it);
    }

    std::map<Character*, hand>::iterator hIt = g_carrierHandForChar.find(c);
    if (hIt != g_carrierHandForChar.end())
    {
        RootObject* ro = hIt->second.getRootObject();
        if (ro != NULL && IsValidPtr(ro))
        {
            Character* carrier = static_cast<Character*>(ro);
            if (carrier != NULL && IsValidPtr(carrier))
            {
                g_carrierForChar[c] = carrier;
                return carrier;
            }
        }
    }

    return NULL;
}

static Character* GetGuardFor(Character* c)
{
    if (c == NULL)
        return NULL;

    std::map<Character*, Character*>::iterator it = g_guardForChar.find(c);
    if (it != g_guardForChar.end())
    {
        if (it->second != NULL && IsValidPtr(it->second))
            return it->second;
        g_guardForChar.erase(it);
    }

    std::map<Character*, hand>::iterator hIt = g_guardHandForChar.find(c);
    if (hIt != g_guardHandForChar.end())
    {
        RootObject* ro = hIt->second.getRootObject();
        if (ro != NULL && IsValidPtr(ro))
        {
            Character* guard = static_cast<Character*>(ro);
            if (guard != NULL && IsValidPtr(guard))
            {
                g_guardForChar[c] = guard;
                return guard;
            }
        }
    }

    return NULL;
}

static bool IsCaravanMode(Character* c)
{
    if (c == NULL)
        return false;
    std::map<Character*, bool>::iterator it = g_caravanModeForChar.find(c);
    if (it != g_caravanModeForChar.end() && it->second)
        return true;
    return GetCarrierFor(c) != NULL;
}

static Inventory* GetBackpackCargoInventory(Character* c)
{
    if (c == NULL || !IsValidPtr(c))
        return NULL;

    // Kenshi stores opened bag/bull-pack contents on the backpack Item, not in the character's main grid.
    Item* backpack = c->hasABackpackOn();
    if (backpack != NULL && IsValidPtr(backpack))
    {
        Inventory* inv = backpack->getInventory();
        if (inv != NULL)
            return inv;
    }

    // Fallback path based on the equipped backpack slot.
    Inventory* mainInv = c->getInventory();
    if (mainInv != NULL)
    {
        InventorySection* section = mainInv->getSectionOfType(ATTACH_BACKPACK);
        if (section != NULL && !section->isEmpty())
        {
            const Ogre::vector<InventorySection::SectionItem>::type& items = section->getItems();
            if (!items.empty() && items[0].item != NULL)
            {
                Inventory* inv = items[0].item->getInventory();
                if (inv != NULL)
                    return inv;
            }
        }
    }

    return NULL;
}

static Inventory* GetCarrierCargoInventory(Character* carrier)
{
    if (carrier == NULL || !IsValidPtr(carrier))
        return NULL;

    Inventory* backpackInv = GetBackpackCargoInventory(carrier);
    if (backpackInv != NULL)
        return backpackInv;

    // Fallback for human/animal without an equipped container.
    return carrier->getInventory();
}

static bool ItemMatchesChoice(GameData* gd, GameData* want)
{
    if (gd == NULL || want == NULL)
        return false;
    return (gd == want || gd->stringID == want->stringID);
}

static bool InventoryHasWantedItem(Inventory* inv, GameData* want)
{
    if (inv == NULL || want == NULL)
        return false;

    lektor<InventorySection*>& sections = inv->sectionsInSearchOrder;
    for (unsigned int i = 0; i < sections.count; ++i)
    {
        InventorySection* sec = sections.stuff[i];
        if (sec == NULL)
            continue;
        const Ogre::vector<InventorySection::SectionItem>::type& itms = sec->getItems();
        for (size_t j = 0; j < itms.size(); ++j)
        {
            Item* it = itms[j].item;
            if (it == NULL || it->isStolen(false))
                continue;
            if (ItemMatchesChoice(it->getGameData(), want))
                return true;
        }
    }

    return false;
}

static int CountWantedItemsInInventory(Inventory* inv, GameData* want)
{
    if (inv == NULL || want == NULL)
        return 0;

    int count = 0;
    lektor<InventorySection*>& sections = inv->sectionsInSearchOrder;
    for (unsigned int i = 0; i < sections.count; ++i)
    {
        InventorySection* sec = sections.stuff[i];
        if (sec == NULL)
            continue;
        const Ogre::vector<InventorySection::SectionItem>::type& itms = sec->getItems();
        for (size_t j = 0; j < itms.size(); ++j)
        {
            Item* it = itms[j].item;
            if (it == NULL || it->isStolen(false))
                continue;
            if (ItemMatchesChoice(it->getGameData(), want))
                count += (it->quantity > 0 ? it->quantity : 1);
        }
    }

    return count;
}

static void LogInventoryCountsFor(Character* worker, const std::string& prefix)
{
    if (worker == NULL)
        return;

    GameData* want = GetHaulItemFor(worker);
    Building* src = GetSourceBuildingFor(worker);
    Building* tgt = GetTargetBuildingFor(worker);
    Character* carrier = GetCarrierFor(worker);

    Inventory* carrierMain = (carrier != NULL) ? carrier->getInventory() : NULL;
    Inventory* carrierBag = (carrier != NULL) ? GetBackpackCargoInventory(carrier) : NULL;
    Inventory* carrierCargo = (carrier != NULL) ? GetCarrierCargoInventory(carrier) : NULL;

    int srcCount = (src != NULL && want != NULL) ? CountWantedItemsInInventory(src->getInventory(), want) : -1;
    int workerCount = (want != NULL) ? CountWantedItemsInInventory(worker->getInventory(), want) : -1;
    int carrierMainCount = (carrierMain != NULL && want != NULL) ? CountWantedItemsInInventory(carrierMain, want) : -1;
    int carrierBagCount = (carrierBag != NULL && want != NULL) ? CountWantedItemsInInventory(carrierBag, want) : -1;
    int carrierCargoCount = (carrierCargo != NULL && want != NULL) ? CountWantedItemsInInventory(carrierCargo, want) : -1;
    int targetCount = (tgt != NULL && want != NULL) ? CountWantedItemsInInventory(tgt->getInventory(), want) : -1;

    HFTLog(prefix
        + " | item=" + SafeGameDataName(want)
        + " | source=" + BuildingName(src) + " count=" + IntStr(srcCount)
        + " | worker=" + CharacterName(worker) + " count=" + IntStr(workerCount)
        + " | carrier=" + (carrier != NULL ? CharacterName(carrier) : "none")
        + " main=" + IntStr(carrierMainCount)
        + " bag=" + IntStr(carrierBagCount)
        + " cargo=" + IntStr(carrierCargoCount)
        + " | target=" + BuildingName(tgt) + " count=" + IntStr(targetCount));
}

static bool CharacterHasWantedItem(Character* c)
{
    if (c == NULL)
        return false;
    return InventoryHasWantedItem(c->getInventory(), GetHaulItemFor(c));
}

static bool CarrierHasWantedItem(Character* c)
{
    Character* carrier = GetCarrierFor(c);
    if (carrier == NULL)
        return false;
    return InventoryHasWantedItem(GetCarrierCargoInventory(carrier), GetHaulItemFor(c));
}

static bool BuildingHasWantedItem(Building* b, Character* c)
{
    if (b == NULL || !IsValidPtr(b))
        return false;
    return InventoryHasWantedItem(b->getInventory(), GetHaulItemFor(c));
}

static bool IsHFTConfigured(Character* c)
{
    if (c == NULL)
        return false;
    GameData* want = GetHaulItemFor(c);
    Building* src = GetSourceBuildingFor(c);
    Building* tgt = GetTargetBuildingFor(c);
    if (want == NULL || src == NULL || tgt == NULL)
        return false;
    if (src == tgt)
        return false;
    if (IsCaravanMode(c) && GetCarrierFor(c) == NULL)
        return false;
    return true;
}

static std::string VecStr(const Ogre::Vector3& v);
static Ogre::Vector3 GetSourceNodeFor(Character* c);
static Ogre::Vector3 GetTargetNodeFor(Character* c);


static double Dist2D(const Ogre::Vector3& a, const Ogre::Vector3& b)
{
    double dx = (double)a.x - (double)b.x;
    double dz = (double)a.z - (double)b.z;
    return sqrt(dx * dx + dz * dz);
}

static void LogCaravanMovementProbe(Character* leader, const char* reason)
{
    if (leader == NULL || !IsValidPtr(leader))
        return;

    Character* carrier = GetCarrierFor(leader);
    Character* guard = GetGuardFor(leader);

    if (carrier == NULL && guard == NULL)
        return;

    int last = 0;
    std::map<Character*, int>::iterator it = g_lastCaravanProbeTickForChar.find(leader);
    if (it != g_lastCaravanProbeTickForChar.end())
        last = it->second;

    // About every 10 seconds at normal tick pacing; enough to inspect movement without log spam.
    if (g_globalHFTTick - last < 600)
        return;

    g_lastCaravanProbeTickForChar[leader] = g_globalHFTTick;

    Ogre::Vector3 lp = leader->getPosition();
    std::string msg = std::string("CaravanMovementProbe: leader=") + CharacterName(leader)
        + " | reason=" + reason
        + " | leaderPos=" + VecStr(lp);

    if (carrier != NULL && IsValidPtr(carrier))
    {
        Ogre::Vector3 cp = carrier->getPosition();
        msg += " | carrier=" + CharacterName(carrier)
            + " dist=" + IntStr((int)Dist2D(lp, cp))
            + " pos=" + VecStr(cp);
    }
    else
        msg += " | carrier=none";

    if (guard != NULL && IsValidPtr(guard))
    {
        Ogre::Vector3 gp = guard->getPosition();
        msg += " | guard=" + CharacterName(guard)
            + " dist=" + IntStr((int)Dist2D(lp, gp))
            + " pos=" + VecStr(gp);
    }
    else
        msg += " | guard=none";

    HFTTrace(msg);
}


static void LogRouteStatus(Character* c, const std::string& prefix)
{
    if (c == NULL)
        return;

    GameData* want = GetHaulItemFor(c);
    Building* src = GetSourceBuildingFor(c);
    Building* tgt = GetTargetBuildingFor(c);
    Character* carrier = GetCarrierFor(c);
    Character* guard = GetGuardFor(c);

    std::string missing;
    if (want == NULL) missing += " item";
    if (src == NULL) missing += " source";
    if (tgt == NULL) missing += " target";
    if (IsCaravanMode(c) && carrier == NULL) missing += " carrier";
    if (src != NULL && tgt != NULL && src == tgt) missing += " source_equals_target";

    HFTLog(prefix + " | worker=" + CharacterName(c)
        + " | item=" + SafeGameDataName(want)
        + " | source=" + BuildingName(src)
        + " | target=" + BuildingName(tgt)
        + " | carrier=" + (carrier != NULL ? CharacterName(carrier) : "none")
        + " | guard=" + (guard != NULL ? CharacterName(guard) : "none")
        + " | srcPoint=" + VecStr(GetSourceNodeFor(c))
        + " | tgtPoint=" + VecStr(GetTargetNodeFor(c))
        + " | learnedMoveTask=" + (g_learnedPointMoveTaskType >= 0 ? IntStr(g_learnedPointMoveTaskType) : "none")
        + " | ready=" + std::string(IsHFTConfigured(c) ? "yes" : "no")
        + (missing.empty() ? "" : (" | missing:" + missing)));
}

static std::vector<Tasker*> DirectReadPermanentJobRows(Character* owner);
static int DirectRemovePermanentJobRowsByPointers(Character* owner, const std::vector<Tasker*>& rows, const char* reason);
static void CaptureGuardCreatedRows(Character* leader, Character* guard, const std::vector<Tasker*>& beforeRows, const char* reason);
static void MoveSingleCharacterToLeaderSquad(Character* leader, Character* member, const char* label);
static void RestoreSingleCharacterOriginalSquad(Character* c, const char* label);
static void RestoreRouteMemberSquadMovesForLeader(Character* leader, Character* carrier, Character* guard, const char* label);
static int DirectRemovePermanentJobRows(Character* owner, int taskKey, RootObject* subject, const char* reason);
static int DirectRemovePermanentHaulJobRows(Character* owner, Building* target, const char* reason);

static bool TryRemoveRealStayCloseJob(Character* carrier, Character* leader, const char* label);
static bool TryCreateRealBodyguardJob(Character* guard, Character* leader, const char* label);
static bool TryRemoveRealBodyguardJob(Character* guard, Character* leader, const char* label);

static void ClearSelectedSquadMoveLatch(Character* worker);

static void ClearHFTConfigFor(Character* c, const std::string& reason)
{
    if (c == NULL)
        return;
    Character* oldCarrier = GetCarrierFor(c);
    Character* oldGuard = GetGuardFor(c);

    bool had = (g_haulItemForChar.find(c) != g_haulItemForChar.end())
            || (g_sourceBuildingForChar.find(c) != g_sourceBuildingForChar.end())
            || (g_targetBuildingForChar.find(c) != g_targetBuildingForChar.end())
            || (g_carrierForChar.find(c) != g_carrierForChar.end())
            || (g_guardForChar.find(c) != g_guardForChar.end())
            || (g_sourceHandForChar.find(c) != g_sourceHandForChar.end())
            || (g_targetHandForChar.find(c) != g_targetHandForChar.end())
            || (g_carrierHandForChar.find(c) != g_carrierHandForChar.end())
            || (g_guardHandForChar.find(c) != g_guardHandForChar.end())
            || (g_caravanModeForChar.find(c) != g_caravanModeForChar.end())
            || (g_sourceNodeForChar.find(c) != g_sourceNodeForChar.end())
            || (g_targetNodeForChar.find(c) != g_targetNodeForChar.end());

    if (oldCarrier != NULL && IsValidPtr(oldCarrier))
        TryRemoveRealStayCloseJob(oldCarrier, c, reason.c_str());
    if (oldGuard != NULL && IsValidPtr(oldGuard))
        TryRemoveRealBodyguardJob(oldGuard, c, reason.c_str());

    RestoreRouteMemberSquadMovesForLeader(c, oldCarrier, oldGuard, reason.c_str());

    g_haulItemForChar.erase(c);
    g_sourceBuildingForChar.erase(c);
    g_targetBuildingForChar.erase(c);
    g_carrierForChar.erase(c);
    g_guardForChar.erase(c);
    g_sourceHandForChar.erase(c);
    g_targetHandForChar.erase(c);
    g_carrierHandForChar.erase(c);
    g_guardHandForChar.erase(c);
    g_caravanModeForChar.erase(c);
    g_sourceNodeForChar.erase(c);
    g_targetNodeForChar.erase(c);
    g_stageForChar.erase(c);
    g_lastMoveOrderTickForChar.erase(c);
    g_routeConfiguredTickForChar.erase(c);
    g_lastHaulJobScoreTickForChar.erase(c);
    g_carrierRealFollowJobForChar.erase(c);
    g_guardRealBodyguardJobForChar.erase(c);
    g_guardCreatedRowsForChar.erase(c);
    ClearSelectedSquadMoveLatch(c);

    if (had)
        HFTLog("Route cleared for " + CharacterName(c) + ": " + reason);
}

static void HardClearHFTRouteFor(Character* c, const std::string& reason)
{
    if (c == NULL)
        return;

    Building* oldTarget = GetTargetBuildingFor(c);
    Character* oldCarrier = GetCarrierFor(c);
    Character* oldGuard = GetGuardFor(c);

    if (oldCarrier != NULL && IsValidPtr(oldCarrier))
        TryRemoveRealStayCloseJob(oldCarrier, c, reason.c_str());
    if (oldGuard != NULL && IsValidPtr(oldGuard))
        TryRemoveRealBodyguardJob(oldGuard, c, reason.c_str());

    RestoreRouteMemberSquadMovesForLeader(c, oldCarrier, oldGuard, reason.c_str());

    // Remove the vanilla Haul To job row used as our C-button anchor.
    // This is the missing piece from v24: Clear route cleared HFT state, but
    // the vanilla task row stayed, so the route could be reopened/re-seeded.
    int removedAnchor = DirectRemovePermanentHaulJobRows(c, oldTarget, reason.c_str());

    // Avoid duplicate removal attempts inside ClearHFTConfigFor after hard clear already removed visible rows.
    g_carrierForChar.erase(c);
    g_carrierHandForChar.erase(c);
    g_carrierRealFollowJobForChar.erase(c);
    g_guardForChar.erase(c);
    g_guardHandForChar.erase(c);
    g_guardRealBodyguardJobForChar.erase(c);
    g_guardCreatedRowsForChar.erase(c);

    ClearHFTConfigFor(c, reason);

    // Fallback: if oldTarget was unknown, remove any Haul To anchor rows from this worker.
    if (removedAnchor == 0 && oldTarget == NULL)
        DirectRemovePermanentHaulJobRows(c, NULL, reason.c_str());

    if (gui != NULL)
        gui->updateToOrdersPanel();
}

static bool TaskerMatchesConfiguredRoute(Tasker* tk, Character* c)
{
    if (tk == NULL || c == NULL || !IsValidPtr(tk))
        return false;
    if ((int)tk->key() != HFT_HAUL_JOB_TASK_KEY)
        return false;

    Building* tgt = GetTargetBuildingFor(c);
    if (tgt == NULL || !IsValidPtr(tgt))
        return false;

    Building* subject = tk->subject.getBuilding();
    if (subject == NULL || !IsValidPtr(subject))
        return false;

    return subject == tgt;
}

static bool CharacterHasMatchingHaulPermaJob(Character* c)
{
    if (c == NULL || !IsValidPtr(c))
        return false;

    OrdersReceiver* or_ = c->getOrdersReciever();
    if (or_ == NULL || !IsValidPtr(or_))
        return false;

    char* base = (char*)or_;
    int pc = *(int*)(base + 0x90);
    Tasker** pa = *(Tasker***)(base + 0x98);

    if (pc < 0 || pc > 64 || pa == NULL || !IsValidPtr(pa))
    {
        HFTTrace("Perma-job list unavailable for " + CharacterName(c)
            + " | pc=" + IntStr(pc) + " | assuming matching job row still exists");
        return true;
    }

    for (int i = 0; i < pc; ++i)
    {
        Tasker* tk = pa[i];
        if (TaskerMatchesConfiguredRoute(tk, c))
        {
            g_characterForTasker[tk] = c;
            return true;
        }
    }
    return false;
}

static bool CharacterHasRecentHaulJobHeartbeat(Character* c)
{
    if (c == NULL)
        return false;

    std::map<Character*, int>::iterator hit = g_lastHaulJobScoreTickForChar.find(c);
    if (hit != g_lastHaulJobScoreTickForChar.end())
        return (g_globalHFTTick - hit->second) <= HFT_SCORE_HEARTBEAT_GRACE_TICKS;

    // Newly configured routes get a short grace period before the score hook has had
    // time to heartbeat. After that, no heartbeat means the job is probably off/idle.
    std::map<Character*, int>::iterator cit = g_routeConfiguredTickForChar.find(c);
    if (cit != g_routeConfiguredTickForChar.end())
        return (g_globalHFTTick - cit->second) <= HFT_SCORE_HEARTBEAT_GRACE_TICKS;

    return true;
}

static float Distance2D(RootObject* a, RootObject* b)
{
    if (a == NULL || b == NULL)
        return 999999.0f;
    Ogre::Vector3 ap = a->getPosition();
    Ogre::Vector3 bp = b->getPosition();
    float dx = ap.x - bp.x;
    float dz = ap.z - bp.z;
    return sqrtf(dx * dx + dz * dz);
}

static float Distance2DToPos(RootObject* a, const Ogre::Vector3& p)
{
    if (a == NULL)
        return 999999.0f;
    Ogre::Vector3 ap = a->getPosition();
    float dx = ap.x - p.x;
    float dz = ap.z - p.z;
    return sqrtf(dx * dx + dz * dz);
}

static float Distance2DPosToPos(const Ogre::Vector3& a, const Ogre::Vector3& b)
{
    float dx = a.x - b.x;
    float dz = a.z - b.z;
    return sqrtf(dx * dx + dz * dz);
}

static std::string VecStr(const Ogre::Vector3& v)
{
    return std::string("(") + IntStr((int)v.x) + "," + IntStr((int)v.y) + "," + IntStr((int)v.z) + ")";
}

static bool IsProbablyRealPoint(const Ogre::Vector3& p)
{
    // Kenshi map coordinates are large; zero is usually a missing/default clickpos.
    return (p.x > 1.0f || p.x < -1.0f || p.z > 1.0f || p.z < -1.0f);
}

static Ogre::Vector3 GetSourceNodeFor(Character* c)
{
    std::map<Character*, Ogre::Vector3>::iterator it = g_sourceNodeForChar.find(c);
    if (it != g_sourceNodeForChar.end())
        return it->second;

    Building* b = GetSourceBuildingFor(c);
    if (b != NULL && IsValidPtr(b))
        return b->getPosition();

    return Ogre::Vector3::ZERO;
}

static Ogre::Vector3 GetTargetNodeFor(Character* c)
{
    std::map<Character*, Ogre::Vector3>::iterator it = g_targetNodeForChar.find(c);
    if (it != g_targetNodeForChar.end())
        return it->second;

    Building* b = GetTargetBuildingFor(c);
    if (b != NULL && IsValidPtr(b))
        return b->getPosition();

    return Ogre::Vector3::ZERO;
}

static Character* GetFirstSelectedCharacterOr(Character* fallback)
{
    if (ou == NULL || ou->player == NULL)
        return fallback;
    lektor<RootObject*> sel;
    ou->player->getAllSelectedObjects(sel, CHARACTER);
    for (uint32_t i = 0; i < sel.count; i++)
    {
        Character* c = (sel.stuff[i] != NULL) ? static_cast<Character*>(sel.stuff[i]) : NULL;
        if (c != NULL && IsValidPtr(c))
            return c;
    }
    return fallback;
}

// ---------------------------------------------------------------------------
// Inventory transfer: source storage -> character -> target storage
// ---------------------------------------------------------------------------

static int MoveWantedItems(Inventory* from, Inventory* to, GameData* want, int maxUnits)
{
    if (from == NULL || to == NULL || want == NULL)
        return 0;

    std::vector<Item*> targets;

    lektor<InventorySection*>& sections = from->sectionsInSearchOrder;
    for (unsigned int i = 0; i < sections.count; ++i)
    {
        InventorySection* sec = sections.stuff[i];
        if (sec == NULL)
            continue;
        const Ogre::vector<InventorySection::SectionItem>::type& itms = sec->getItems();
        for (size_t j = 0; j < itms.size(); ++j)
        {
            Item* it = itms[j].item;
            if (it == NULL || it->isStolen(false))
                continue;
            if (ItemMatchesChoice(it->getGameData(), want))
                targets.push_back(it);
        }
    }

    int moved = 0;
    for (size_t t = 0; t < targets.size(); ++t)
    {
        if (maxUnits > 0 && moved >= maxUnits)
            break;
        Item* it = targets[t];
        if (it == NULL)
            continue;
        int qty = it->quantity > 0 ? it->quantity : 1;
        for (int k = 0; k < qty; ++k)
        {
            if (maxUnits > 0 && moved >= maxUnits)
                break;

            Item* unit = from->removeItemDontDestroy_returnsItem(it, 1, true);
            if (unit == NULL)
                break;

            if (!to->addItem(unit, 1, true, true))
            {
                from->addItem(unit, 1, true, true);
                return moved;
            }
            moved++;
        }
    }
    return moved;
}

static int DoFetch(Character* c)
{
    if (c == NULL)
        return 0;
    Building* src = GetSourceBuildingFor(c);
    GameData* want = GetHaulItemFor(c);
    if (src == NULL || want == NULL)
        return 0;

    float dist = Distance2D(c, src);
    if (dist > HFT_RANGE)
    {
        HFTTrace("Fetch blocked: too far from source. char=" + CharacterName(c) + " source=" + BuildingName(src) + " dist=" + IntStr((int)dist));
        return 0;
    }

    // Fill as much of the character's exposed inventory as Kenshi's addItem() allows.
    // If backpack space is exposed through Character::getInventory(), it will be used too.
    int moved = MoveWantedItems(src->getInventory(), c->getInventory(), want, HFT_TRANSFER_MAX_UNITS);
    if (moved > 0)
        HFTLog("Fetched " + IntStr(moved) + "x " + SafeGameDataName(want) + " from " + BuildingName(src) + " into " + CharacterName(c));
    return moved;
}

static int DoDeposit(Character* c)
{
    if (c == NULL)
        return 0;
    Building* tgt = GetTargetBuildingFor(c);
    GameData* want = GetHaulItemFor(c);
    if (tgt == NULL || want == NULL)
        return 0;

    float dist = Distance2D(c, tgt);
    if (dist > HFT_RANGE)
    {
        HFTTrace("Deposit blocked: too far from target. char=" + CharacterName(c) + " target=" + BuildingName(tgt) + " dist=" + IntStr((int)dist));
        return 0;
    }

    int moved = MoveWantedItems(c->getInventory(), tgt->getInventory(), want, HFT_TRANSFER_MAX_UNITS);
    if (moved > 0)
        HFTLog("Deposited " + IntStr(moved) + "x " + SafeGameDataName(want) + " from " + CharacterName(c) + " into " + BuildingName(tgt));
    return moved;
}

static int DoLoadCarrier(Character* c)
{
    if (c == NULL)
        return 0;

    Character* carrier = GetCarrierFor(c);
    Building* src = GetSourceBuildingFor(c);
    GameData* want = GetHaulItemFor(c);
    if (carrier == NULL || src == NULL || want == NULL)
        return 0;

    // The worker opens the source. The carrier backpack/cargo only needs to be close to the worker,
    // so pack animals do not have to path into buildings or right on top of the chest.
    if (Distance2D(c, src) > HFT_RANGE || Distance2D(c, carrier) > HFT_CARRIER_TRANSFER_RANGE)
    {
        HFTTrace("Load carrier blocked: worker/chest/carrier not close enough. worker=" + CharacterName(c)
            + " carrier=" + CharacterName(carrier)
            + " source=" + BuildingName(src)
            + " workerToSource=" + IntStr((int)Distance2D(c, src))
            + " workerToCarrier=" + IntStr((int)Distance2D(c, carrier)));
        return 0;
    }

    int moved = MoveWantedItems(src->getInventory(), GetCarrierCargoInventory(carrier), want, HFT_TRANSFER_MAX_UNITS);
    if (moved > 0)
        HFTLog("Loaded " + IntStr(moved) + "x " + SafeGameDataName(want) + " from " + BuildingName(src)
            + " into carrier " + CharacterName(carrier) + " via " + CharacterName(c));
    return moved;
}

static int DoUnloadCarrier(Character* c)
{
    if (c == NULL)
        return 0;

    Character* carrier = GetCarrierFor(c);
    Building* tgt = GetTargetBuildingFor(c);
    GameData* want = GetHaulItemFor(c);
    if (carrier == NULL || tgt == NULL || want == NULL)
        return 0;

    // The worker opens the target. The carrier backpack/cargo only needs to be close to the worker.
    if (Distance2D(c, tgt) > HFT_RANGE || Distance2D(c, carrier) > HFT_CARRIER_TRANSFER_RANGE)
    {
        HFTTrace("Unload carrier blocked: worker/chest/carrier not close enough. worker=" + CharacterName(c)
            + " carrier=" + CharacterName(carrier)
            + " target=" + BuildingName(tgt)
            + " workerToTarget=" + IntStr((int)Distance2D(c, tgt))
            + " workerToCarrier=" + IntStr((int)Distance2D(c, carrier)));
        return 0;
    }

    int moved = MoveWantedItems(GetCarrierCargoInventory(carrier), tgt->getInventory(), want, HFT_TRANSFER_MAX_UNITS);
    if (moved > 0)
        HFTLog("Unloaded " + IntStr(moved) + "x " + SafeGameDataName(want) + " from carrier "
            + CharacterName(carrier) + " into " + BuildingName(tgt) + " via " + CharacterName(c));
    return moved;
}

static int DoMoveWorkerCargoToCarrier(Character* c)
{
    if (c == NULL)
        return 0;

    Character* carrier = GetCarrierFor(c);
    GameData* want = GetHaulItemFor(c);
    if (carrier == NULL || want == NULL)
        return 0;

    if (Distance2D(c, carrier) > HFT_CARRIER_TRANSFER_RANGE)
        return 0;

    int moved = MoveWantedItems(c->getInventory(), GetCarrierCargoInventory(carrier), want, HFT_TRANSFER_MAX_UNITS);
    if (moved > 0)
        HFTLog("Moved " + IntStr(moved) + "x " + SafeGameDataName(want) + " from worker "
            + CharacterName(c) + " into carrier " + CharacterName(carrier));
    return moved;
}

// ---------------------------------------------------------------------------
// GameWorld update: debug-guarded worker loop
// ---------------------------------------------------------------------------

void (*GameWorld_update_orig)(GameWorld*, float) = NULL;

static bool MoveCooldownReady(Character* c)
{
    if (c == NULL)
        return false;

    int last = -999999;
    std::map<Character*, int>::iterator it = g_lastMoveOrderTickForChar.find(c);
    if (it != g_lastMoveOrderTickForChar.end())
        last = it->second;

    if (g_globalHFTTick - last < HFT_MOVE_ORDER_COOLDOWN_TICKS)
        return false;

    g_lastMoveOrderTickForChar[c] = g_globalHFTTick;
    return true;
}

static void HFTLeashLog(Character* c, const std::string& msg)
{
    if (c == NULL)
        return;

    int last = -999999;
    std::map<Character*, int>::iterator it = g_lastLeashLogTickForChar.find(c);
    if (it != g_lastLeashLogTickForChar.end())
        last = it->second;

    if (g_globalHFTTick - last < 180)
        return;

    g_lastLeashLogTickForChar[c] = g_globalHFTTick;
    HFTTrace(msg);
}

static void IssueMoveOrder(Character* c, Building* b, const char* label)
{
    if (c == NULL || b == NULL)
        return;
    if (!MoveCooldownReady(c))
        return;

    HFTTrace(std::string("Move order -> ") + label + ": " + CharacterName(c) + " to " + BuildingName(b));
    c->addOrder(NULL, GET_NEAR_TO, b, false, true, Ogre::Vector3::ZERO);
}


static bool HFTSelectionContainsCharacter(Character* c)
{
    if (c == NULL || !IsValidPtr(c))
        return false;

    PlayerInterface* pi = g_lastPlayerInterface;
    if (pi == NULL && ou != NULL)
        pi = ou->player;

    if (pi == NULL)
        return false;

    lektor<RootObject*> sel;
    pi->getAllSelectedObjects(sel, CHARACTER);
    for (uint32_t i = 0; i < sel.count; i++)
    {
        Character* sc = (sel.stuff[i] != NULL) ? static_cast<Character*>(sel.stuff[i]) : NULL;
        if (sc == c)
            return true;
    }

    return false;
}

static int HFTSelectedCharacterCount()
{
    PlayerInterface* pi = g_lastPlayerInterface;
    if (pi == NULL && ou != NULL)
        pi = ou->player;

    if (pi == NULL)
        return 0;

    lektor<RootObject*> sel;
    pi->getAllSelectedObjects(sel, CHARACTER);
    int count = 0;
    for (uint32_t i = 0; i < sel.count; i++)
    {
        Character* sc = (sel.stuff[i] != NULL) ? static_cast<Character*>(sel.stuff[i]) : NULL;
        if (sc != NULL && IsValidPtr(sc))
            count++;
    }

    return count;
}

static bool IsRouteMoveGroupSelected(Character* worker, Character* carrier, std::string& missing)
{
    missing = "";

    if (worker == NULL || !IsValidPtr(worker))
    {
        missing = "worker";
        return false;
    }

    if (!HFTSelectionContainsCharacter(worker))
        missing += " leader";

    if (carrier != NULL && carrier != worker && IsValidPtr(carrier))
    {
        if (!HFTSelectionContainsCharacter(carrier))
            missing += " carrier";
    }

    Character* guard = GetGuardFor(worker);
    if (guard != NULL && guard != worker && guard != carrier && IsValidPtr(guard))
    {
        if (!HFTSelectionContainsCharacter(guard))
            missing += " guard";
    }

    return missing.empty();
}


static bool IsRouteMemberCloseToLeader(Character* leader, Character* member, const char* memberLabel, std::string& waitReason)
{
    if (member == NULL || member == leader || !IsValidPtr(member))
        return true;

    float d = Distance2D(leader, member);
    if (d <= HFT_GROUP_LAUNCH_RANGE)
        return true;

    if (!waitReason.empty())
        waitReason += ", ";
    waitReason += std::string(memberLabel) + "=" + CharacterName(member) + " dist=" + IntStr((int)d);
    return false;
}

static bool IsSelectedRouteGroupCloseEnoughToLaunch(Character* worker, Character* carrier, std::string& waitReason)
{
    waitReason = "";

    if (worker == NULL || !IsValidPtr(worker))
    {
        waitReason = "leader invalid";
        return false;
    }

    bool ok = true;

    if (!IsRouteMemberCloseToLeader(worker, carrier, "carrier", waitReason))
        ok = false;

    Character* guard = GetGuardFor(worker);
    if (guard != NULL && guard != worker && guard != carrier && IsValidPtr(guard))
    {
        if (!IsRouteMemberCloseToLeader(worker, guard, "guard", waitReason))
            ok = false;
    }

    return ok;
}

static void LogSelectedGroupLaunchWait(Character* worker, const std::string& waitReason, const char* label)
{
    if (worker == NULL)
        return;

    int last = -999999;
    std::map<Character*, int>::iterator it = g_lastGroupLaunchWaitLogTickForChar.find(worker);
    if (it != g_lastGroupLaunchWaitLogTickForChar.end())
        last = it->second;

    if (g_globalHFTTick - last < HFT_GROUP_WAIT_LOG_TICKS)
        return;

    g_lastGroupLaunchWaitLogTickForChar[worker] = g_globalHFTTick;

    HFTLog(std::string("Selected squad launch wait -> ") + label
        + " | leader=" + CharacterName(worker)
        + " | waitReason=" + waitReason
        + " | launchRange=" + IntStr((int)HFT_GROUP_LAUNCH_RANGE)
        + " | note=suppressing route move until carrier/guard are gathered");
}

static bool IsDuplicateSelectedSquadMove(Character* worker, const Ogre::Vector3& p)
{
    if (worker == NULL)
        return false;

    std::map<Character*, Ogre::Vector3>::iterator itDest = g_lastSelectedSquadMoveDestForChar.find(worker);
    std::map<Character*, int>::iterator itTick = g_lastSelectedSquadMoveTickForChar.find(worker);

    if (itDest == g_lastSelectedSquadMoveDestForChar.end() || itTick == g_lastSelectedSquadMoveTickForChar.end())
        return false;

    float dist = Distance2DPosToPos(itDest->second, p);
    if (dist > 120.0f)
        return false;

    if (g_globalHFTTick - itTick->second >= HFT_SELECTED_MOVE_REISSUE_TICKS)
        return false;

    return true;
}

static void MarkSelectedSquadMove(Character* worker, const Ogre::Vector3& p)
{
    if (worker == NULL)
        return;

    g_lastSelectedSquadMoveDestForChar[worker] = p;
    g_lastSelectedSquadMoveTickForChar[worker] = g_globalHFTTick;
}

static void ClearSelectedSquadMoveLatch(Character* worker)
{
    if (worker == NULL)
        return;

    g_lastSelectedSquadMoveDestForChar.erase(worker);
    g_lastSelectedSquadMoveTickForChar.erase(worker);
    g_lastGroupLaunchWaitLogTickForChar.erase(worker);
    g_nextCaravanLaunchTickForChar.erase(worker);
    g_lastSettleWaitLogTickForChar.erase(worker);
}


static void SetCaravanLaunchDelay(Character* worker, const char* reason)
{
    if (worker == NULL)
        return;

    int allowTick = g_globalHFTTick + HFT_POST_TRANSFER_SETTLE_TICKS;
    g_nextCaravanLaunchTickForChar[worker] = allowTick;
    g_lastSettleWaitLogTickForChar.erase(worker);
    g_lastSelectedSquadMoveDestForChar.erase(worker);
    g_lastSelectedSquadMoveTickForChar.erase(worker);

    HFTLog(std::string("Caravan launch delayed -> leader=")
        + CharacterName(worker)
        + " | reason=" + reason
        + " | settleTicks=" + IntStr(HFT_POST_TRANSFER_SETTLE_TICKS)
        + " | note=wait before issuing next selected-squad move");
}

static bool IsCaravanLaunchDelayActive(Character* worker, const char* label)
{
    if (worker == NULL)
        return false;

    std::map<Character*, int>::iterator it = g_nextCaravanLaunchTickForChar.find(worker);
    if (it == g_nextCaravanLaunchTickForChar.end())
        return false;

    int allowTick = it->second;
    int remaining = allowTick - g_globalHFTTick;

    if (remaining <= 0)
    {
        g_nextCaravanLaunchTickForChar.erase(it);
        g_lastSettleWaitLogTickForChar.erase(worker);
        HFTTrace(std::string("Caravan launch delay complete -> leader=")
            + CharacterName(worker)
            + " | label=" + label);
        return false;
    }

    int last = -999999;
    std::map<Character*, int>::iterator lt = g_lastSettleWaitLogTickForChar.find(worker);
    if (lt != g_lastSettleWaitLogTickForChar.end())
        last = lt->second;

    if (g_globalHFTTick - last >= HFT_SETTLE_WAIT_LOG_TICKS)
    {
        g_lastSettleWaitLogTickForChar[worker] = g_globalHFTTick;
        HFTLog(std::string("Caravan launch settle wait -> ") + label
            + " | leader=" + CharacterName(worker)
            + " | remainingTicks=" + IntStr(remaining)
            + " | note=post-transfer delay before group move");
    }

    return true;
}

static bool TryIssueSelectedSquadPointMoveOrder(Character* worker, Character* carrier, const Ogre::Vector3& p, const char* label)
{
    if (worker == NULL || !IsValidPtr(worker))
        return false;

    if (newPlayerTaskSelectedCharacters_orig == NULL)
        return false;

    PlayerInterface* pi = g_lastPlayerInterface;
    if (pi == NULL && ou != NULL)
        pi = ou->player;

    if (pi == NULL)
        return false;

    std::string missing;
    if (!IsRouteMoveGroupSelected(worker, carrier, missing))
    {
        HFTTrace(std::string("Selected squad move not used -> route group not selected | missing=") + missing
            + " | label=" + label);
        return false;
    }

    // v38: do not let the leader start the next leg before the carrier/guard
    // have gathered around him. This is what caused Griffin to begin earlier.
    std::string waitReason;
    if (!IsSelectedRouteGroupCloseEnoughToLaunch(worker, carrier, waitReason))
    {
        LogSelectedGroupLaunchWait(worker, waitReason, label);
        return true; // suppress individual fallback while the group catches up
    }

    // v38: after one selected-squad move to a destination, do not keep reissuing
    // the same move every cooldown cycle. Reissuing can reset formation pacing and
    // make the leader appear to launch ahead. A long retry window remains in case
    // a path gets interrupted.
    if (IsDuplicateSelectedSquadMove(worker, p))
    {
        HFTTrace(std::string("Selected squad move latched -> ") + label
            + " | leader=" + CharacterName(worker)
            + " | to=" + VecStr(p)
            + " | note=same destination already issued recently");
        return true;
    }

    // If the selected group is ready but the move cooldown has not elapsed, suppress
    // the old individual fallback for this tick. Otherwise the mod would still split
    // the group with individual point orders between selected group moves.
    if (!MoveCooldownReady(worker))
        return true;

    TaskType moveTask = (g_learnedPointMoveTaskType >= 0) ? (TaskType)g_learnedPointMoveTaskType : MOVE_CUS_ORDERED;

    hand targetH;
    targetH.setNull();

    HFTLog(std::string("Selected squad point move -> ") + label
        + " | leader=" + CharacterName(worker)
        + " | carrier=" + (carrier != NULL ? CharacterName(carrier) : "none")
        + " | guard=" + (GetGuardFor(worker) != NULL ? CharacterName(GetGuardFor(worker)) : "none")
        + " | selectedCount=" + IntStr(HFTSelectedCharacterCount())
        + " | to=" + VecStr(p)
        + " | task=" + IntStr((int)moveTask)
        + (g_learnedPointMoveTaskType >= 0 ? " [learned]" : " [fallback MOVE_CUS_ORDERED]")
        + " | note=using selected-character group movement path with v40 settle delay");

    newPlayerTaskSelectedCharacters_orig(pi, moveTask, targetH, NULL, p, false);
    MarkSelectedSquadMove(worker, p);
    return true;
}

static void IssuePointMoveOrder(Character* c, const Ogre::Vector3& p, const char* label)
{
    if (c == NULL)
        return;
    if (!MoveCooldownReady(c))
        return;

    TaskType moveTask = (g_learnedPointMoveTaskType >= 0) ? (TaskType)g_learnedPointMoveTaskType : MOVE_CUS_ORDERED;

    HFTTrace(std::string("Point move -> ") + label
        + ": " + CharacterName(c)
        + " to " + VecStr(p)
        + " task=" + IntStr((int)moveTask)
        + (g_learnedPointMoveTaskType >= 0 ? " [learned]" : " [fallback MOVE_CUS_ORDERED]"));

    // v12: reuse the TaskType learned from a real minimap/world click.
    c->addOrder(NULL, moveTask, NULL, false, true, p);
}

static bool HasRealCarrierFollowJob(Character* worker)
{
    if (worker == NULL)
        return false;
    std::map<Character*, bool>::iterator it = g_carrierRealFollowJobForChar.find(worker);
    return it != g_carrierRealFollowJobForChar.end() && it->second;
}

static bool IsOnlySelectedCharacter(Character* c)
{
    if (c == NULL || g_lastPlayerInterface == NULL)
        return false;

    lektor<RootObject*> sel;
    g_lastPlayerInterface->getAllSelectedObjects(sel, CHARACTER);
    int count = 0;
    Character* only = NULL;
    for (uint32_t i = 0; i < sel.count; i++)
    {
        Character* sc = (sel.stuff[i] != NULL) ? static_cast<Character*>(sel.stuff[i]) : NULL;
        if (sc != NULL && IsValidPtr(sc))
        {
            count++;
            only = sc;
        }
    }
    return count == 1 && only == c;
}

static bool TryCreateRealStayCloseJob(Character* carrier, Character* leader, const char* label)
{
    if (carrier == NULL || leader == NULL || carrier == leader)
        return false;

    // Best path: the user just selected the pack beast to press Set Carrier.
    // In that case we can call the same PlayerInterface job path Kenshi used
    // when the log showed task=31 for "Staying close: Griffin".
    if (g_lastPlayerInterface != NULL && IsOnlySelectedCharacter(carrier))
    {
        g_lastPlayerInterface->addJobSelectedCharacters((TaskType)HFT_STAY_CLOSE_ADD_TASK_KEY,
                                                        leader, true, false, Ogre::Vector3::ZERO);
        if (gui != NULL)
            gui->updateToOrdersPanel();

        HFTLog(std::string("Real Staying Close job requested -> ")
            + CharacterName(carrier) + " stays close to " + CharacterName(leader)
            + " | requestTask=" + IntStr(HFT_STAY_CLOSE_ADD_TASK_KEY) + " | expectedVisibleRowTask=" + IntStr(HFT_STAY_CLOSE_ROW_TASK_KEY)
            + " | reason=" + label);
        return true;
    }

    // Fallback: this may be a current order rather than a clean visible permanent job,
    // but it lets us test whether task 31 can be applied directly to the carrier.
    carrier->addOrder(NULL, (TaskType)HFT_STAY_CLOSE_ADD_TASK_KEY, leader, false, true, Ogre::Vector3::ZERO);
    HFTLog(std::string("Fallback Staying Close order requested -> ")
        + CharacterName(carrier) + " stays close to " + CharacterName(leader)
        + " | requestTask=" + IntStr(HFT_STAY_CLOSE_ADD_TASK_KEY) + " | expectedVisibleRowTask=" + IntStr(HFT_STAY_CLOSE_ROW_TASK_KEY)
        + " | selectedOnlyCarrier=no | reason=" + label);
    return false;
}

static bool TryRemoveRealStayCloseJob(Character* carrier, Character* leader, const char* label)
{
    if (carrier == NULL || leader == NULL || carrier == leader)
        return false;

    // v23: direct row removal. v21 proved the real visible Staying Close row is
    // in the permanent job list as key 289, even though it is created through
    // request task 31. PlayerInterface removal did not visibly delete it, so we
    // remove the matching row from the carrier's OrdersReceiver permanent list.
    int directRemoved = DirectRemovePermanentJobRows(carrier, HFT_STAY_CLOSE_ROW_TASK_KEY, leader, label);
    if (directRemoved > 0)
        return true;

    // Harmless fallback if the row layout is different in this moment.
    if (g_lastPlayerInterface != NULL && IsOnlySelectedCharacter(carrier))
    {
        g_lastPlayerInterface->removeJobSelectedCharacters((TaskType)HFT_STAY_CLOSE_ROW_TASK_KEY);
        g_lastPlayerInterface->removeJobSelectedCharacters((TaskType)HFT_STAY_CLOSE_ADD_TASK_KEY);

        if (gui != NULL)
            gui->updateToOrdersPanel();

        HFTLog(std::string("Fallback selected Staying Close removal requested -> ")
            + CharacterName(carrier)
            + " | visibleRowTask=" + IntStr(HFT_STAY_CLOSE_ROW_TASK_KEY)
            + " | requestTask=" + IntStr(HFT_STAY_CLOSE_ADD_TASK_KEY)
            + " | selectedOnlyCarrier=yes | reason=" + label);
        return true;
    }

    HFTLog(std::string("Could not remove Staying Close job automatically -> ")
        + CharacterName(carrier)
        + " | direct row not found and carrier is not the only selected character"
        + " | visibleRowTask=" + IntStr(HFT_STAY_CLOSE_ROW_TASK_KEY)
        + " | requestTask=" + IntStr(HFT_STAY_CLOSE_ADD_TASK_KEY)
        + " | reason=" + label);
    return false;
}



static bool TryCreateRealBodyguardJob(Character* guard, Character* leader, const char* label)
{
    if (guard == NULL || leader == NULL || guard == leader)
        return false;

    if (g_lastPlayerInterface != NULL && IsOnlySelectedCharacter(guard))
    {
        g_lastPlayerInterface->addJobSelectedCharacters((TaskType)HFT_BODYGUARD_TASK_KEY,
                                                        leader, true, false, Ogre::Vector3::ZERO);
        if (gui != NULL)
            gui->updateToOrdersPanel();

        HFTLog(std::string("Real Bodyguard job requested -> ")
            + CharacterName(guard) + " bodyguards " + CharacterName(leader)
            + " | task=" + IntStr(HFT_BODYGUARD_TASK_KEY)
            + " | reason=" + label);
        return true;
    }

    guard->addOrder(NULL, (TaskType)HFT_BODYGUARD_TASK_KEY, leader, false, true, Ogre::Vector3::ZERO);
    HFTLog(std::string("Fallback Bodyguard order requested -> ")
        + CharacterName(guard) + " bodyguards " + CharacterName(leader)
        + " | task=" + IntStr(HFT_BODYGUARD_TASK_KEY)
        + " | selectedOnlyGuard=no | reason=" + label);
    return false;
}

static bool TryRemoveRealBodyguardJob(Character* guard, Character* leader, const char* label)
{
    if (guard == NULL || leader == NULL || guard == leader)
        return false;

    int removedStored = 0;

    std::map<Character*, std::vector<Tasker*> >::iterator it = g_guardCreatedRowsForChar.find(leader);
    if (it != g_guardCreatedRowsForChar.end())
        removedStored = DirectRemovePermanentJobRowsByPointers(guard, it->second, label);

    int removedBodyguard = 0;
    int removedFollow31 = 0;
    int removedFollow289 = 0;

    // v36b fix:
    // When v36 auto-moves the guard into the leader's squad, the Bodyguard order
    // can be created through the fallback addOrder path instead of the selected-
    // character UI path. In that case the stored-row capture can be empty, but
    // the visible leftover helper row is task 31, not 289.
    //
    // Therefore, when stored rows were not removed, clear:
    //   task 45  = Bodyguard/Protect row
    //   task 31  = guard helper Staying Close row
    //   task 289 = older carrier-style visible Staying Close fallback
    if (removedStored == 0)
    {
        removedBodyguard = DirectRemovePermanentJobRows(guard, HFT_BODYGUARD_TASK_KEY, leader, label);
        removedFollow31 = DirectRemovePermanentJobRows(guard, HFT_STAY_CLOSE_REQUEST_TASK_KEY, leader, label);
        removedFollow289 = DirectRemovePermanentJobRows(guard, HFT_STAY_CLOSE_ROW_TASK_KEY, NULL, label);
    }

    if (removedStored > 0 || removedBodyguard > 0 || removedFollow31 > 0 || removedFollow289 > 0)
    {
        HFTLog(std::string("Bodyguard job cleared -> guard=")
            + CharacterName(guard) + " | leader=" + CharacterName(leader)
            + " | removedStoredRows=" + IntStr(removedStored)
            + " | removedBodyguard=" + IntStr(removedBodyguard)
            + " | removedFollow31=" + IntStr(removedFollow31)
            + " | removedAnyFollowRow289=" + IntStr(removedFollow289)
            + " | reason=" + label);
        g_guardCreatedRowsForChar.erase(leader);
        return true;
    }

    if (g_lastPlayerInterface != NULL && IsOnlySelectedCharacter(guard))
    {
        g_lastPlayerInterface->removeJobSelectedCharacters((TaskType)HFT_BODYGUARD_TASK_KEY);
        g_lastPlayerInterface->removeJobSelectedCharacters((TaskType)HFT_STAY_CLOSE_REQUEST_TASK_KEY);
        g_lastPlayerInterface->removeJobSelectedCharacters((TaskType)HFT_STAY_CLOSE_ROW_TASK_KEY);

        if (gui != NULL)
            gui->updateToOrdersPanel();

        HFTLog(std::string("Fallback selected Bodyguard removal requested -> ")
            + CharacterName(guard)
            + " | bodyguardTask=" + IntStr(HFT_BODYGUARD_TASK_KEY)
            + " | followTask31=" + IntStr(HFT_STAY_CLOSE_REQUEST_TASK_KEY)
            + " | anyFollowRowTask289=" + IntStr(HFT_STAY_CLOSE_ROW_TASK_KEY)
            + " | reason=" + label);
        g_guardCreatedRowsForChar.erase(leader);
        return true;
    }

    HFTLog(std::string("Could not remove Bodyguard job automatically -> ")
        + CharacterName(guard)
        + " | stored rows not found, direct rows not found, and guard is not the only selected character"
        + " | reason=" + label);
    return false;
}

static bool HFTVectorContainsTasker(const std::vector<Tasker*>& rows, Tasker* tk)
{
    for (size_t i = 0; i < rows.size(); ++i)
    {
        if (rows[i] == tk)
            return true;
    }
    return false;
}

static std::string TaskerDebugString(Tasker* tk)
{
    if (tk == NULL || !IsValidPtr(tk))
        return "null";

    std::string s = "task=" + IntStr((int)tk->key());

    Building* b = tk->subject.getBuilding();
    RootObject* ro = tk->subject.getRootObject();

    if (b != NULL && IsValidPtr(b))
        s += " building=" + BuildingName(b);
    else
        s += " subject=" + PtrStr(ro);

    s += " ptr=" + PtrStr(tk);
    return s;
}

static std::vector<Tasker*> DirectReadPermanentJobRows(Character* owner)
{
    std::vector<Tasker*> rows;

    if (owner == NULL || !IsValidPtr(owner))
        return rows;

    OrdersReceiver* or_ = owner->getOrdersReciever();
    if (or_ == NULL || !IsValidPtr(or_))
        return rows;

    char* base = (char*)or_;
    int count = *(int*)(base + 0x90);
    Tasker** arr = *(Tasker***)(base + 0x98);

    if (count < 0 || count > 64 || arr == NULL || !IsValidPtr(arr))
        return rows;

    for (int i = 0; i < count; ++i)
    {
        Tasker* tk = arr[i];
        if (tk != NULL && IsValidPtr(tk))
            rows.push_back(tk);
    }

    return rows;
}

static void CaptureGuardCreatedRows(Character* leader, Character* guard, const std::vector<Tasker*>& beforeRows, const char* reason)
{
    if (leader == NULL || guard == NULL || !IsValidPtr(leader) || !IsValidPtr(guard))
        return;

    std::vector<Tasker*> afterRows = DirectReadPermanentJobRows(guard);
    std::vector<Tasker*> created;

    for (size_t i = 0; i < afterRows.size(); ++i)
    {
        Tasker* tk = afterRows[i];
        if (tk != NULL && IsValidPtr(tk) && !HFTVectorContainsTasker(beforeRows, tk))
            created.push_back(tk);
    }

    g_guardCreatedRowsForChar[leader] = created;

    std::string msg = "Guard created-row capture -> leader=" + CharacterName(leader)
        + " | guard=" + CharacterName(guard)
        + " | created=" + IntStr((int)created.size())
        + " | before=" + IntStr((int)beforeRows.size())
        + " | after=" + IntStr((int)afterRows.size())
        + " | reason=" + reason;

    for (size_t i = 0; i < created.size(); ++i)
        msg += " | row" + IntStr((int)i) + "{" + TaskerDebugString(created[i]) + "}";

    HFTTrace(msg);
}

static int DirectRemovePermanentJobRowsByPointers(Character* owner, const std::vector<Tasker*>& rows, const char* reason)
{
    if (owner == NULL || !IsValidPtr(owner) || rows.empty())
        return 0;

    OrdersReceiver* or_ = owner->getOrdersReciever();
    if (or_ == NULL || !IsValidPtr(or_))
    {
        HFTLog("Stored-row removal failed: no OrdersReceiver for " + CharacterName(owner));
        return 0;
    }

    char* base = (char*)or_;
    int* countPtr = (int*)(base + 0x90);
    Tasker*** arrayPtr = (Tasker***)(base + 0x98);

    if (countPtr == NULL || arrayPtr == NULL)
        return 0;

    int count = *countPtr;
    Tasker** arr = *arrayPtr;

    if (count < 0 || count > 64 || arr == NULL || !IsValidPtr(arr))
    {
        HFTLog("Stored-row removal failed: unreadable perma list for " + CharacterName(owner)
            + " | count=" + IntStr(count));
        return 0;
    }

    int removed = 0;
    for (int i = count - 1; i >= 0; --i)
    {
        Tasker* tk = arr[i];
        if (tk == NULL || !IsValidPtr(tk))
            continue;

        if (HFTVectorContainsTasker(rows, tk))
        {
            HFTLog("Direct removing stored guard row -> owner=" + CharacterName(owner)
                + " | " + TaskerDebugString(tk)
                + " | reason=" + reason);

            for (int j = i; j < count - 1; ++j)
                arr[j] = arr[j + 1];

            arr[count - 1] = NULL;
            count--;
            removed++;
        }
    }

    if (removed > 0)
    {
        *countPtr = count;

        if (gui != NULL)
            gui->updateToOrdersPanel();

        HFTLog("Stored guard row removal complete -> owner=" + CharacterName(owner)
            + " | removed=" + IntStr(removed)
            + " | remainingPermaCount=" + IntStr(count));
    }
    else
    {
        HFTLog("Stored guard row removal found no matching live rows -> owner="
            + CharacterName(owner)
            + " | storedRows=" + IntStr((int)rows.size())
            + " | reason=" + reason);
    }

    return removed;
}

static int DirectRemovePermanentJobRows(Character* owner, int taskKey, RootObject* subject, const char* reason)
{
    if (owner == NULL || !IsValidPtr(owner))
        return 0;

    OrdersReceiver* or_ = owner->getOrdersReciever();
    if (or_ == NULL || !IsValidPtr(or_))
    {
        HFTLog("Direct job-row removal failed: no OrdersReceiver for " + CharacterName(owner));
        return 0;
    }

    // These offsets were validated by v21 JobListProbe:
    // permanent job count at +0x90 and permanent job pointer array at +0x98.
    char* base = (char*)or_;
    int* countPtr = (int*)(base + 0x90);
    Tasker*** arrayPtr = (Tasker***)(base + 0x98);

    if (countPtr == NULL || arrayPtr == NULL)
        return 0;

    int count = *countPtr;
    Tasker** arr = *arrayPtr;

    if (count < 0 || count > 64 || arr == NULL || !IsValidPtr(arr))
    {
        HFTLog("Direct job-row removal failed: unreadable perma list for " + CharacterName(owner)
            + " | count=" + IntStr(count));
        return 0;
    }

    int removed = 0;
    for (int i = count - 1; i >= 0; --i)
    {
        Tasker* tk = arr[i];
        if (tk == NULL || !IsValidPtr(tk))
            continue;

        int key = (int)tk->key();
        RootObject* tkSubject = tk->subject.getRootObject();

        if (key == taskKey && (subject == NULL || tkSubject == subject))
        {
            HFTLog("Direct removing permanent job row -> owner=" + CharacterName(owner)
                + " | task=" + IntStr(key)
                + " | subject=" + PtrStr(tkSubject)
                + " | reason=" + reason);

            for (int j = i; j < count - 1; ++j)
                arr[j] = arr[j + 1];

            arr[count - 1] = NULL;
            count--;
            removed++;
        }
    }

    if (removed > 0)
    {
        *countPtr = count;

        if (gui != NULL)
            gui->updateToOrdersPanel();

        HFTLog("Direct job-row removal complete -> owner=" + CharacterName(owner)
            + " | removed=" + IntStr(removed)
            + " | task=" + IntStr(taskKey)
            + " | remainingPermaCount=" + IntStr(count));
    }
    else
    {
        HFTLog("Direct job-row removal found no matching row -> owner=" + CharacterName(owner)
            + " | task=" + IntStr(taskKey)
            + " | reason=" + reason);
    }

    return removed;
}

static int DirectRemovePermanentHaulJobRows(Character* owner, Building* target, const char* reason)
{
    if (owner == NULL || !IsValidPtr(owner))
        return 0;

    OrdersReceiver* or_ = owner->getOrdersReciever();
    if (or_ == NULL || !IsValidPtr(or_))
    {
        HFTLog("Direct Haul To anchor removal failed: no OrdersReceiver for " + CharacterName(owner));
        return 0;
    }

    char* base = (char*)or_;
    int* countPtr = (int*)(base + 0x90);
    Tasker*** arrayPtr = (Tasker***)(base + 0x98);

    int count = *countPtr;
    Tasker** arr = *arrayPtr;

    if (count < 0 || count > 64 || arr == NULL || !IsValidPtr(arr))
    {
        HFTLog("Direct Haul To anchor removal failed: unreadable perma list for " + CharacterName(owner)
            + " | count=" + IntStr(count));
        return 0;
    }

    int removed = 0;
    for (int i = count - 1; i >= 0; --i)
    {
        Tasker* tk = arr[i];
        if (tk == NULL || !IsValidPtr(tk))
            continue;

        int key = (int)tk->key();
        Building* tkBuilding = tk->subject.getBuilding();

        if (key == HFT_HAUL_JOB_TASK_KEY && (target == NULL || tkBuilding == target))
        {
            HFTLog("Direct removing Haul To anchor row -> owner=" + CharacterName(owner)
                + " | task=" + IntStr(key)
                + " | building=" + BuildingName(tkBuilding)
                + " | reason=" + reason);

            for (int j = i; j < count - 1; ++j)
                arr[j] = arr[j + 1];

            arr[count - 1] = NULL;
            count--;
            removed++;
        }
    }

    if (removed > 0)
    {
        *countPtr = count;

        if (gui != NULL)
            gui->updateToOrdersPanel();

        HFTLog("Direct Haul To anchor removal complete -> owner=" + CharacterName(owner)
            + " | removed=" + IntStr(removed)
            + " | remainingPermaCount=" + IntStr(count));
    }
    else
    {
        HFTLog("Direct Haul To anchor removal found no matching row -> owner=" + CharacterName(owner)
            + " | target=" + BuildingName(target)
            + " | reason=" + reason);
    }

    return removed;
}





static void IssueCarrierPointMoveOrder(Character* carrier, const Ogre::Vector3& p, const char* label)
{
    if (carrier == NULL)
        return;
    if (!MoveCooldownReady(carrier))
        return;

    TaskType moveTask = (g_learnedPointMoveTaskType >= 0) ? (TaskType)g_learnedPointMoveTaskType : MOVE_CUS_ORDERED;

    HFTTrace(std::string("Carrier point move -> ") + label
        + ": " + CharacterName(carrier)
        + " to " + VecStr(p)
        + " task=" + IntStr((int)moveTask)
        + (g_learnedPointMoveTaskType >= 0 ? " [learned]" : " [fallback MOVE_CUS_ORDERED]"));

    carrier->addOrder(NULL, moveTask, NULL, false, true, p);
}

// v17: create/use Kenshi's real visible "Staying close" job where possible.
static void IssueFollowLeaderOrder(Character* follower, Character* leader, const char* label)
{
    if (follower == NULL || leader == NULL || follower == leader)
        return;

    bool ok = TryCreateRealStayCloseJob(follower, leader, label);
    // Mark true only for the safe PlayerInterface job path. If the fallback path ran,
    // caravan movement still uses point sync as backup.
    if (ok)
        g_carrierRealFollowJobForChar[leader] = true;
}

static void IssueRouteMoveOrder(Character* c, Building* b, const Ogre::Vector3& p, const char* label)
{
    if (c == NULL || b == NULL)
        return;

    if (Distance2DToPos(c, p) > HFT_NODE_RANGE)
        IssuePointMoveOrder(c, p, label);
    else
        IssueMoveOrder(c, b, label);
}

static void IssueCaravanRouteMoveOrder(Character* worker, Character* carrier, Building* b, const Ogre::Vector3& p, const char* label)
{
    if (worker == NULL || b == NULL)
        return;

    bool realFollow = (carrier != NULL && carrier != worker && HasRealCarrierFollowJob(worker));

    // v37 experiment:
    // If the player has the full route group selected (leader + carrier + guard),
    // issue the point move through PlayerInterface::newPlayerTaskSelectedCharacters.
    // This should make Kenshi apply normal selected-squad movement/formation rules.
    // If the group is not selected, fall back to the proven v36c individual/leader path.
    if (Distance2DToPos(worker, p) > HFT_NODE_RANGE)
    {
        if (IsCaravanLaunchDelayActive(worker, label))
            return;

        if (TryIssueSelectedSquadPointMoveOrder(worker, carrier, p, label))
            return;
    }
    else
    {
        ClearSelectedSquadMoveLatch(worker);
    }

    // v17 preferred mode:
    // If we successfully created the visible "Staying close" job on the carrier,
    // do NOT keep overwriting it with direct point move orders. Move the leader only;
    // Kenshi's follow job keeps the pack beast with the leader and respects squad pacing better.
    if (realFollow && carrier != NULL && IsValidPtr(carrier))
    {
        float pairDist = Distance2D(worker, carrier);

        if (pairDist > HFT_CARAVAN_LEASH_RANGE)
        {
            HFTLeashLog(worker, "Caravan hard leash with real follow: " + CharacterName(worker)
                + " waiting for " + CharacterName(carrier)
                + " | pairDist=" + IntStr((int)pairDist)
                + " | route=" + std::string(label));
            IssuePointMoveOrder(worker, carrier->getPosition(), "caravan wait for real-follow carrier");
            return;
        }

        if (pairDist > HFT_CARAVAN_SOFT_LEASH_RANGE)
        {
            HFTLeashLog(worker, "Caravan soft leash with real follow: " + CharacterName(worker)
                + " slowing for " + CharacterName(carrier)
                + " | pairDist=" + IntStr((int)pairDist)
                + " | route=" + std::string(label));
        }

        if (Distance2DToPos(worker, p) > HFT_NODE_RANGE)
            IssuePointMoveOrder(worker, p, label);
        else
            IssueMoveOrder(worker, b, label);
        return;
    }

    // Fallback mode: no real follow job confirmed, so use v16 point-sync/leash behavior.
    if (carrier != NULL && carrier != worker && IsValidPtr(carrier))
    {
        float pairDist = Distance2D(worker, carrier);
        float workerToNode = Distance2DToPos(worker, p);
        float carrierToNode = Distance2DToPos(carrier, p);

        if (pairDist > HFT_CARAVAN_LEASH_RANGE)
        {
            HFTLeashLog(worker, "Caravan hard leash: " + CharacterName(worker)
                + " waiting for " + CharacterName(carrier)
                + " | pairDist=" + IntStr((int)pairDist)
                + " | node=" + VecStr(p)
                + " | route=" + std::string(label));

            IssuePointMoveOrder(worker, carrier->getPosition(), "caravan wait for carrier");
            IssueCarrierPointMoveOrder(carrier, p, label);
            return;
        }

        if (pairDist > HFT_CARAVAN_SOFT_LEASH_RANGE)
        {
            HFTLeashLog(worker, "Caravan soft leash: syncing " + CharacterName(worker)
                + " and " + CharacterName(carrier)
                + " | pairDist=" + IntStr((int)pairDist)
                + " | route=" + std::string(label));

            if (workerToNode < carrierToNode)
            {
                IssuePointMoveOrder(worker, carrier->getPosition(), "caravan slow for carrier");
                IssueCarrierPointMoveOrder(carrier, p, label);
                return;
            }
        }

        if (carrierToNode > HFT_CARAVAN_NODE_ARRIVE_RANGE)
            IssueCarrierPointMoveOrder(carrier, p, label);
    }

    if (Distance2DToPos(worker, p) > HFT_NODE_RANGE)
        IssuePointMoveOrder(worker, p, label);
    else
        IssueMoveOrder(worker, b, label);
}

static void HFTWorkerTick(GameWorld* thisptr, float time)
{
    if (ou == NULL)
        return;

    static int s_tick = 0;
    ++s_tick;
    g_globalHFTTick = s_tick;

    if ((s_tick % 30) != 0)
        return;

    const ogre_unordered_set<Character*>::type& chars = ou->getCharacterUpdateList();
    for (ogre_unordered_set<Character*>::type::const_iterator it = chars.begin(); it != chars.end(); ++it)
    {
        Character* c = *it;
        if (c == NULL || !IsValidPtr(c))
            continue;

        GameData* want = GetHaulItemFor(c);
        Building* src = GetSourceBuildingFor(c);
        Building* tgt = GetTargetBuildingFor(c);
        if (want == NULL || src == NULL || tgt == NULL)
            continue;

        if (src == tgt)
        {
            HFTTrace("Blocked: source and target are the same object for " + CharacterName(c));
            continue;
        }

        // v8: the custom worker loop only runs while the matching Haul To permanent-job row still exists.
        // If the player removes that job, clear the saved HFT route so it cannot keep running invisibly.
        if (!CharacterHasMatchingHaulPermaJob(c))
        {
            ClearHFTConfigFor(c, "matching Haul To job row was removed or changed");
            continue;
        }

        // v8: if the AI stops scoring the matching haul job for a while, pause movement.
        // This is intended to make the normal Jobs on/off toggle pause HaulFromTo too.
        if (!CharacterHasRecentHaulJobHeartbeat(c))
        {
            HFTTrace("Paused route for " + CharacterName(c) + ": no recent Haul To job heartbeat");
            continue;
        }

        // v10 caravan mode: cargo priority is Carrier inventory first.
        // The worker opens source/target containers, while the carrier acts as the moving cargo hold.
        if (IsCaravanMode(c))
        {
            Character* carrier = GetCarrierFor(c);
            if (carrier == NULL || !IsValidPtr(carrier))
                continue;

            LogCaravanMovementProbe(c, "active caravan route");

            Ogre::Vector3 srcNode = GetSourceNodeFor(c);
            Ogre::Vector3 tgtNode = GetTargetNodeFor(c);

            int stage = 0;
            std::map<Character*, int>::iterator st = g_stageForChar.find(c);
            if (st != g_stageForChar.end())
                stage = st->second;
            else
                stage = (CarrierHasWantedItem(c) || CharacterHasWantedItem(c)) ? 1 : 0;

            if (stage == 1)
            {
                if (!CarrierHasWantedItem(c) && !CharacterHasWantedItem(c))
                {
                    g_stageForChar[c] = 0;
                    continue;
                }

                if (Distance2D(c, tgt) <= HFT_RANGE && Distance2D(c, carrier) <= HFT_CARRIER_TRANSFER_RANGE)
                {
                    int moved = 0;
                    moved += DoUnloadCarrier(c);

                    // Safety: if native Kenshi managed to put the cargo in the worker,
                    // unload it too so the route does not jam.
                    moved += DoDeposit(c);

                    if (moved > 0)
                    {
                        g_stageForChar[c] = 0;
                        SetCaravanLaunchDelay(c, "after unload");
                        HFTLog(std::string("Caravan unloaded; source launch pending -> leader=")
                            + CharacterName(c)
                            + " | carrier=" + CharacterName(carrier)
                            + " | note=v40 waits briefly before launching back to source");
                    }
                }
                else
                {
                    IssueCaravanRouteMoveOrder(c, carrier, tgt, tgtNode, "caravan target");
                }
            }
            else
            {
                if (CarrierHasWantedItem(c) || CharacterHasWantedItem(c))
                {
                    g_stageForChar[c] = 1;
                    IssueCaravanRouteMoveOrder(c, carrier, tgt, tgtNode, "caravan target with cargo");
                    continue;
                }

                if (!BuildingHasWantedItem(src, c))
                    continue;

                if (Distance2D(c, src) <= HFT_RANGE && Distance2D(c, carrier) <= HFT_CARRIER_TRANSFER_RANGE)
                {
                    // In carrier mode, worker cargo is treated as accidental/native pickup.
                    // Move it into the carrier before leaving the source.
                    DoMoveWorkerCargoToCarrier(c);

                    int moved = DoLoadCarrier(c);
                    moved += DoMoveWorkerCargoToCarrier(c);

                    if (moved > 0)
                    {
                        g_stageForChar[c] = 1;
                        SetCaravanLaunchDelay(c, "after load");
                        HFTLog(std::string("Caravan loaded; target launch pending -> leader=")
                            + CharacterName(c)
                            + " | carrier=" + CharacterName(carrier)
                            + " | loaded=" + IntStr(moved)
                            + " | note=v40 waits briefly before launching to target");
                    }
                }
                else
                {
                    IssueCaravanRouteMoveOrder(c, carrier, src, srcNode, "caravan source");
                }
            }

            continue;
        }

        int stage = 0;
        std::map<Character*, int>::iterator st = g_stageForChar.find(c);
        if (st != g_stageForChar.end())
            stage = st->second;
        else
            stage = CharacterHasWantedItem(c) ? 1 : 0;

        if (stage == 1)
        {
            if (!CharacterHasWantedItem(c))
            {
                g_stageForChar[c] = 0;
                continue;
            }

            float d = Distance2D(c, tgt);
            if (d <= HFT_RANGE)
            {
                int moved = DoDeposit(c);
                if (moved > 0)
                    g_stageForChar[c] = 0;
            }
            else
            {
                IssueRouteMoveOrder(c, tgt, GetTargetNodeFor(c), "target");
            }
        }
        else
        {
            bool charHas = CharacterHasWantedItem(c);
            float d = Distance2D(c, src);

            if (d <= HFT_RANGE)
            {
                // If the worker is at the source, top up the inventory before going to target.
                if (BuildingHasWantedItem(src, c))
                {
                    int moved = DoFetch(c);
                    if (moved > 0)
                    {
                        g_stageForChar[c] = 1;
                        IssueRouteMoveOrder(c, tgt, GetTargetNodeFor(c), "target after bulk fetch");
                        continue;
                    }
                }

                if (charHas || CharacterHasWantedItem(c))
                {
                    g_stageForChar[c] = 1;
                    IssueRouteMoveOrder(c, tgt, GetTargetNodeFor(c), "target after carried item");
                    continue;
                }
            }
            else
            {
                if (charHas)
                {
                    g_stageForChar[c] = 1;
                    IssueRouteMoveOrder(c, tgt, GetTargetNodeFor(c), "target with carried item");
                    continue;
                }

                if (BuildingHasWantedItem(src, c))
                    IssueRouteMoveOrder(c, src, GetSourceNodeFor(c), "source");
            }
        }
    }
}

void GameWorld_update_hook(GameWorld* thisptr, float time)
{
    if (GameWorld_update_orig != NULL)
        GameWorld_update_orig(thisptr, time);

    HFTWorkerTick(thisptr, time);
}

// ---------------------------------------------------------------------------
// Player task probe: logs task numbers when you create vanilla jobs
// ---------------------------------------------------------------------------

void newPlayerTaskSelectedCharacters_hook(PlayerInterface* self, TaskType t, const hand& targetH,
                                          Building* dest, const Ogre::Vector3& clickpos,
                                          bool addDontClear)
{
    g_lastPlayerInterface = self;
    RootObject* subject = targetH.getRootObject();
    Building* b = targetH.getBuilding();
    std::string name = b != NULL ? BuildingName(b) : "(not building)";

    HFTTrace("PlayerTaskProbe: task=" + IntStr((int)t)
        + " addDontClear=" + std::string(addDontClear ? "true" : "false")
        + " subject=" + PtrStr(subject)
        + " building=" + name
        + " dest=" + BuildingName(dest)
        + " clickpos=" + VecStr(clickpos)
        + " note=use this line to identify vanilla follow/bodyguard tasks if tested manually");

    // Learn the exact TaskType Kenshi uses for normal minimap/ground movement.
    // We only learn when there is no target object/building and the click position is real.
    if (subject == NULL && dest == NULL && b == NULL && IsProbablyRealPoint(clickpos))
    {
        if (g_learnedPointMoveTaskType != (int)t)
        {
            g_learnedPointMoveTaskType = (int)t;
            g_lastLearnedPointMovePos = clickpos;
            HFTLog("Learned manual map/ground point-move task=" + IntStr(g_learnedPointMoveTaskType)
                + " at " + VecStr(clickpos)
                + ". Future caravan point moves will use this task.");
        }
    }

    if (newPlayerTaskSelectedCharacters_orig != NULL)
        newPlayerTaskSelectedCharacters_orig(self, t, targetH, dest, clickpos, addDontClear);
}


// ---------------------------------------------------------------------------
// v31 Squad / Formation Probe
// ---------------------------------------------------------------------------

static bool HFTReadableMemory(const void* p, size_t bytes)
{
    if (p == NULL || bytes == 0)
        return false;

    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)))
        return false;

    if (mbi.State != MEM_COMMIT)
        return false;

    if ((mbi.Protect & PAGE_GUARD) != 0)
        return false;

    DWORD protect = mbi.Protect & 0xff;
    bool readable =
        protect == PAGE_READONLY ||
        protect == PAGE_READWRITE ||
        protect == PAGE_WRITECOPY ||
        protect == PAGE_EXECUTE_READ ||
        protect == PAGE_EXECUTE_READWRITE ||
        protect == PAGE_EXECUTE_WRITECOPY;

    if (!readable)
        return false;

    const char* start = (const char*)p;
    const char* end = start + bytes;
    const char* regionStart = (const char*)mbi.BaseAddress;
    const char* regionEnd = regionStart + mbi.RegionSize;

    return start >= regionStart && end <= regionEnd;
}

static std::string HexSize(size_t value)
{
    std::ostringstream ss;
    ss << "0x" << std::hex << value;
    return ss.str();
}

static bool ReadPointerField(void* base, size_t offset, void** out)
{
    if (out == NULL)
        return false;
    *out = NULL;

    if (base == NULL || !IsValidPtr(base))
        return false;

    const char* addr = ((const char*)base) + offset;
    if (!HFTReadableMemory(addr, sizeof(void*)))
        return false;

    void* value = NULL;
    std::memcpy(&value, addr, sizeof(void*));
    *out = value;
    return true;
}

static void AddUniqueCharacter(std::vector<Character*>& chars, Character* c)
{
    if (c == NULL || !IsValidPtr(c))
        return;

    for (size_t i = 0; i < chars.size(); ++i)
    {
        if (chars[i] == c)
            return;
    }

    chars.push_back(c);
}

static std::vector<Character*> GetSelectedCharactersForProbe()
{
    std::vector<Character*> chars;

    PlayerInterface* pi = g_lastPlayerInterface;
    if (pi == NULL && ou != NULL)
        pi = ou->player;

    if (pi != NULL)
    {
        lektor<RootObject*> sel;
        pi->getAllSelectedObjects(sel, CHARACTER);
        for (uint32_t i = 0; i < sel.count; i++)
        {
            Character* c = (sel.stuff[i] != NULL) ? static_cast<Character*>(sel.stuff[i]) : NULL;
            AddUniqueCharacter(chars, c);
        }
    }

    return chars;
}

static std::string CharacterListNames(const std::vector<Character*>& chars)
{
    std::string out;
    for (size_t i = 0; i < chars.size(); ++i)
    {
        if (i > 0)
            out += ", ";
        out += CharacterName(chars[i]);
    }
    if (out.empty())
        out = "(none)";
    return out;
}

static void LogCharacterPermanentRowsForProbe(Character* c, const char* label)
{
    if (c == NULL || !IsValidPtr(c))
        return;

    std::vector<Tasker*> rows = DirectReadPermanentJobRows(c);
    std::string msg = std::string("SquadProbeRows: label=") + label
        + " | char=" + CharacterName(c)
        + " | ptr=" + PtrStr(c)
        + " | permaCount=" + IntStr((int)rows.size());

    for (size_t i = 0; i < rows.size(); ++i)
        msg += " | row" + IntStr((int)i) + "{" + TaskerDebugString(rows[i]) + "}";

    HFTLog(msg);
}

static void LogSharedPointerCandidates(const std::vector<Character*>& chars, const char* label)
{
    if (chars.size() < 2)
    {
        HFTLog(std::string("SquadProbeSharedPtr: label=") + label + " | need at least 2 selected characters");
        return;
    }

    int logged = 0;

    // We do not know the squad field yet. Scan pointer-sized fields in Character
    // and log offsets where at least 2 selected characters share the same readable pointer.
    // Compare logs before/after moving characters into Test; a squad pointer should change
    // with squad membership and be shared by the members in that squad.
    for (size_t off = 0; off <= 0x900; off += sizeof(void*))
    {
        void* values[16];
        int valueCount = 0;

        for (size_t i = 0; i < chars.size() && i < 16; ++i)
        {
            void* v = NULL;
            if (ReadPointerField(chars[i], off, &v) && v != NULL && HFTReadableMemory(v, sizeof(void*)))
            {
                values[valueCount++] = v;
            }
        }

        for (int vi = 0; vi < valueCount; ++vi)
        {
            void* candidate = values[vi];
            int shared = 0;
            std::string names;

            for (size_t ci = 0; ci < chars.size() && ci < 16; ++ci)
            {
                void* v = NULL;
                if (ReadPointerField(chars[ci], off, &v) && v == candidate)
                {
                    if (!names.empty())
                        names += ", ";
                    names += CharacterName(chars[ci]);
                    shared++;
                }
            }

            if (shared >= 2)
            {
                bool alreadyLogged = false;
                for (int prev = 0; prev < vi; ++prev)
                {
                    if (values[prev] == candidate)
                        alreadyLogged = true;
                }

                if (!alreadyLogged)
                {
                    HFTLog(std::string("SquadProbeSharedPtr: label=") + label
                        + " | offset=" + HexSize(off)
                        + " | ptr=" + PtrStr(candidate)
                        + " | sharedBy=" + IntStr(shared)
                        + " [" + names + "]");

                    logged++;
                    if (logged >= 80)
                    {
                        HFTLog(std::string("SquadProbeSharedPtr: label=") + label
                            + " | stopped after 80 candidates to avoid log spam");
                        return;
                    }
                }
            }
        }
    }

    if (logged == 0)
        HFTLog(std::string("SquadProbeSharedPtr: label=") + label + " | no shared pointer candidates found");
}


static unsigned long long ReadU64Safe(void* base, size_t offset, bool* ok)
{
    if (ok != NULL)
        *ok = false;

    if (base == NULL || !IsValidPtr(base))
        return 0;

    const char* addr = ((const char*)base) + offset;
    if (!HFTReadableMemory(addr, sizeof(unsigned long long)))
        return 0;

    unsigned long long value = 0;
    std::memcpy(&value, addr, sizeof(unsigned long long));

    if (ok != NULL)
        *ok = true;

    return value;
}

static bool MemoryContainsAsciiNeedle(void* p, size_t bytes, const char* needle)
{
    if (p == NULL || needle == NULL || bytes == 0)
        return false;

    size_t needleLen = strlen(needle);
    if (needleLen == 0 || needleLen > bytes)
        return false;

    if (!HFTReadableMemory(p, bytes))
        return false;

    const char* data = (const char*)p;
    for (size_t i = 0; i + needleLen <= bytes; ++i)
    {
        bool match = true;
        for (size_t j = 0; j < needleLen; ++j)
        {
            if (data[i + j] != needle[j])
            {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }

    return false;
}

static bool MemoryContainsWideNeedle(void* p, size_t bytes, const char* asciiNeedle)
{
    if (p == NULL || asciiNeedle == NULL || bytes == 0)
        return false;

    size_t n = strlen(asciiNeedle);
    if (n == 0 || (n * 2) > bytes)
        return false;

    if (!HFTReadableMemory(p, bytes))
        return false;

    const unsigned char* data = (const unsigned char*)p;
    for (size_t i = 0; i + (n * 2) <= bytes; ++i)
    {
        bool match = true;
        for (size_t j = 0; j < n; ++j)
        {
            if (data[i + j * 2] != (unsigned char)asciiNeedle[j] || data[i + j * 2 + 1] != 0)
            {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }

    return false;
}

static void LogCandidateObjectStringHits(void* candidate, const char* label, const char* offsetLabel)
{
    if (candidate == NULL || !HFTReadableMemory(candidate, sizeof(void*)))
        return;

    bool directTest = MemoryContainsAsciiNeedle(candidate, 0x400, "Test")
                   || MemoryContainsWideNeedle(candidate, 0x400, "Test");
    bool directCaravan = MemoryContainsAsciiNeedle(candidate, 0x400, "Caravan")
                      || MemoryContainsWideNeedle(candidate, 0x400, "Caravan");

    if (directTest || directCaravan)
    {
        HFTLog(std::string("SquadCandidateStringHit: label=") + label
            + " | charOffset=" + offsetLabel
            + " | candidatePtr=" + PtrStr(candidate)
            + " | directMemoryContains="
            + std::string(directTest ? "Test" : "")
            + std::string((directTest && directCaravan) ? "," : "")
            + std::string(directCaravan ? "Caravan" : ""));
    }

    // Search pointer fields inside the candidate object for strings.
    for (size_t off = 0; off <= 0x300; off += sizeof(void*))
    {
        void* p = NULL;
        if (!ReadPointerField(candidate, off, &p))
            continue;
        if (p == NULL || !HFTReadableMemory(p, sizeof(void*)))
            continue;

        bool hasTest = MemoryContainsAsciiNeedle(p, 0x400, "Test")
                    || MemoryContainsWideNeedle(p, 0x400, "Test");
        bool hasCaravan = MemoryContainsAsciiNeedle(p, 0x400, "Caravan")
                       || MemoryContainsWideNeedle(p, 0x400, "Caravan");

        if (hasTest || hasCaravan)
        {
            HFTLog(std::string("SquadCandidateStringHit: label=") + label
                + " | charOffset=" + offsetLabel
                + " | candidatePtr=" + PtrStr(candidate)
                + " | field=" + HexSize(off)
                + " | fieldPtr=" + PtrStr(p)
                + " | contains="
                + std::string(hasTest ? "Test" : "")
                + std::string((hasTest && hasCaravan) ? "," : "")
                + std::string(hasCaravan ? "Caravan" : ""));
        }
    }
}

static void LogFocusedOffsetGroup(const std::vector<Character*>& chars, size_t offset, const char* label)
{
    std::vector<void*> seen;

    for (size_t i = 0; i < chars.size(); ++i)
    {
        Character* c = chars[i];
        if (c == NULL || !IsValidPtr(c))
            continue;

        void* v = NULL;
        bool ok = ReadPointerField(c, offset, &v);

        HFTLog(std::string("SquadWatchPtr: label=") + label
            + " | char=" + CharacterName(c)
            + " | offset=" + HexSize(offset)
            + " | ok=" + std::string(ok ? "yes" : "no")
            + " | ptr=" + PtrStr(v));

        if (!ok || v == NULL)
            continue;

        bool already = false;
        for (size_t s = 0; s < seen.size(); ++s)
        {
            if (seen[s] == v)
                already = true;
        }
        if (!already)
            seen.push_back(v);
    }

    for (size_t s = 0; s < seen.size(); ++s)
    {
        void* candidate = seen[s];
        std::string names;
        int shared = 0;

        for (size_t i = 0; i < chars.size(); ++i)
        {
            void* v = NULL;
            if (ReadPointerField(chars[i], offset, &v) && v == candidate)
            {
                if (!names.empty())
                    names += ", ";
                names += CharacterName(chars[i]);
                shared++;
            }
        }

        if (shared >= 2)
        {
            HFTLog(std::string("SquadWatchGroup: label=") + label
                + " | offset=" + HexSize(offset)
                + " | ptr=" + PtrStr(candidate)
                + " | sharedBy=" + IntStr(shared)
                + " [" + names + "]");

            if (offset == 0x658)
                LogCandidateObjectStringHits(candidate, label, "0x658");
        }
    }
}

static void LogFocusedSquadCandidates(const std::vector<Character*>& important, const char* label)
{
    // Watchlist chosen from v31 logs. 0x658 is the strongest candidate because
    // it was shared by leader/carrier/guard and changed between later probes.
    size_t offsets[] = { 0x658, 0x800, 0xa8, 0x610, 0x830, 0x870, 0x898 };
    for (int i = 0; i < 7; ++i)
        LogFocusedOffsetGroup(important, offsets[i], label);
}


static int ReadI32Safe(void* base, size_t offset, bool* ok)
{
    if (ok != NULL)
        *ok = false;

    if (base == NULL || !IsValidPtr(base))
        return 0;

    const char* addr = ((const char*)base) + offset;
    if (!HFTReadableMemory(addr, sizeof(int)))
        return 0;

    int value = 0;
    std::memcpy(&value, addr, sizeof(int));

    if (ok != NULL)
        *ok = true;

    return value;
}

static unsigned char ReadU8Safe(void* base, size_t offset, bool* ok)
{
    if (ok != NULL)
        *ok = false;

    if (base == NULL || !IsValidPtr(base))
        return 0;

    const char* addr = ((const char*)base) + offset;
    if (!HFTReadableMemory(addr, sizeof(unsigned char)))
        return 0;

    unsigned char value = 0;
    std::memcpy(&value, addr, sizeof(unsigned char));

    if (ok != NULL)
        *ok = true;

    return value;
}

static unsigned int HashMemoryChunkSafe(void* base, size_t offset, size_t bytes, bool* ok)
{
    if (ok != NULL)
        *ok = false;

    if (base == NULL || !IsValidPtr(base) || bytes == 0)
        return 0;

    const unsigned char* data = ((const unsigned char*)base) + offset;
    if (!HFTReadableMemory(data, bytes))
        return 0;

    unsigned int h = 2166136261u;
    for (size_t i = 0; i < bytes; ++i)
    {
        h ^= (unsigned int)data[i];
        h *= 16777619u;
    }

    if (ok != NULL)
        *ok = true;

    return h;
}

static std::string HexU32(unsigned int value)
{
    std::ostringstream ss;
    ss << "0x" << std::hex << value;
    return ss.str();
}

static void GetCharsSharingOffsetPointer(const std::vector<Character*>& chars, size_t offset, void* ptr, std::string& names, int& count)
{
    names = "";
    count = 0;

    for (size_t i = 0; i < chars.size(); ++i)
    {
        Character* c = chars[i];
        void* v = NULL;
        if (ReadPointerField(c, offset, &v) && v == ptr)
        {
            if (!names.empty())
                names += ", ";
            names += CharacterName(c);
            count++;
        }
    }
}

static void AddUniquePtr(std::vector<void*>& ptrs, void* p)
{
    if (p == NULL || !HFTReadableMemory(p, sizeof(void*)))
        return;

    for (size_t i = 0; i < ptrs.size(); ++i)
    {
        if (ptrs[i] == p)
            return;
    }

    ptrs.push_back(p);
}

static void LogSquadObjectHashes(void* squadPtr, const char* label, const std::string& sharedNames)
{
    if (squadPtr == NULL || !HFTReadableMemory(squadPtr, sizeof(void*)))
        return;

    std::string msg = std::string("SquadObjectHash: label=") + label
        + " | squadPtr=" + PtrStr(squadPtr)
        + " | sharedBy=[" + sharedNames + "]";

    for (size_t off = 0; off <= 0x3c0; off += 0x40)
    {
        bool ok = false;
        unsigned int h = HashMemoryChunkSafe(squadPtr, off, 0x40, &ok);
        if (ok)
            msg += " | +" + HexSize(off) + "=" + HexU32(h);
    }

    HFTLog(msg);
}

static void LogSquadObjectSmallFields(void* squadPtr, const char* label, const std::string& sharedNames)
{
    if (squadPtr == NULL || !HFTReadableMemory(squadPtr, sizeof(void*)))
        return;

    std::string i32msg = std::string("SquadObjectI32: label=") + label
        + " | squadPtr=" + PtrStr(squadPtr)
        + " | sharedBy=[" + sharedNames + "]";

    int i32Count = 0;
    for (size_t off = 0; off <= 0x240; off += 4)
    {
        bool ok = false;
        int v = ReadI32Safe(squadPtr, off, &ok);
        if (!ok)
            continue;

        // Keep only values that look like counters/enums/settings.
        if (v >= -5 && v <= 10000)
        {
            i32msg += " | +" + HexSize(off) + "=" + IntStr(v);
            i32Count++;
            if (i32Count >= 70)
                break;
        }
    }

    HFTLog(i32msg);

    std::string u8msg = std::string("SquadObjectU8NonZero: label=") + label
        + " | squadPtr=" + PtrStr(squadPtr)
        + " | sharedBy=[" + sharedNames + "]";

    int u8Count = 0;
    for (size_t off = 0; off <= 0x240; ++off)
    {
        bool ok = false;
        unsigned char v = ReadU8Safe(squadPtr, off, &ok);
        if (!ok)
            continue;

        // Avoid huge zero dumps. Small bytes are likely enum/bool candidates.
        if (v > 0 && v <= 20)
        {
            u8msg += " | +" + HexSize(off) + "=" + IntStr((int)v);
            u8Count++;
            if (u8Count >= 90)
                break;
        }
    }

    HFTLog(u8msg);
}

static bool PointerEqualsAnyCharacter(void* p, const std::vector<Character*>& chars, std::string& name)
{
    for (size_t i = 0; i < chars.size(); ++i)
    {
        if ((void*)chars[i] == p)
        {
            name = CharacterName(chars[i]);
            return true;
        }
    }

    return false;
}

static void LogSquadMemberStorageCandidates(void* squadPtr, const std::vector<Character*>& chars, const char* label, const std::string& sharedNames)
{
    if (squadPtr == NULL || !HFTReadableMemory(squadPtr, sizeof(void*)))
        return;

    int logged = 0;

    for (size_t off = 0; off <= 0x300; off += sizeof(void*))
    {
        void* field = NULL;
        if (!ReadPointerField(squadPtr, off, &field))
            continue;

        std::string directName;
        if (PointerEqualsAnyCharacter(field, chars, directName))
        {
            HFTLog(std::string("SquadObjectDirectCharRef: label=") + label
                + " | squadPtr=" + PtrStr(squadPtr)
                + " | sharedBy=[" + sharedNames + "]"
                + " | field=" + HexSize(off)
                + " | char=" + directName
                + " | charPtr=" + PtrStr(field));
            logged++;
        }

        if (field == NULL || !HFTReadableMemory(field, sizeof(void*)))
            continue;

        std::string hitNames;
        int hits = 0;

        for (size_t inner = 0; inner <= 0x500; inner += sizeof(void*))
        {
            void* maybeChar = NULL;
            if (!ReadPointerField(field, inner, &maybeChar))
                continue;

            std::string nm;
            if (PointerEqualsAnyCharacter(maybeChar, chars, nm))
            {
                if (!hitNames.empty())
                    hitNames += ", ";
                hitNames += nm + "@+" + HexSize(inner);
                hits++;
            }
        }

        if (hits > 0)
        {
            HFTLog(std::string("SquadMemberArrayCandidate: label=") + label
                + " | squadPtr=" + PtrStr(squadPtr)
                + " | sharedBy=[" + sharedNames + "]"
                + " | squadField=" + HexSize(off)
                + " | fieldPtr=" + PtrStr(field)
                + " | charHits=" + IntStr(hits)
                + " [" + hitNames + "]");
            logged++;

            if (logged >= 40)
            {
                HFTLog(std::string("SquadMemberArrayCandidate: label=") + label
                    + " | stopped after 40 hits");
                return;
            }
        }
    }

    if (logged == 0)
    {
        HFTLog(std::string("SquadMemberArrayCandidate: label=") + label
            + " | squadPtr=" + PtrStr(squadPtr)
            + " | sharedBy=[" + sharedNames + "]"
            + " | noneFoundInFirst0x300Fields");
    }
}

static void LogSquadObjectDeepCandidates(const std::vector<Character*>& chars, const char* label)
{
    std::vector<void*> squadPtrs;

    for (size_t i = 0; i < chars.size(); ++i)
    {
        void* p = NULL;
        if (ReadPointerField(chars[i], 0x658, &p))
            AddUniquePtr(squadPtrs, p);
    }

    HFTLog(std::string("SquadObjectDeepStart: label=") + label
        + " | uniqueSquadPtrsAt0x658=" + IntStr((int)squadPtrs.size()));

    for (size_t i = 0; i < squadPtrs.size(); ++i)
    {
        void* squadPtr = squadPtrs[i];

        std::string names;
        int sharedCount = 0;
        GetCharsSharingOffsetPointer(chars, 0x658, squadPtr, names, sharedCount);

        void* nameField = NULL;
        ReadPointerField(squadPtr, 0x78, &nameField);

        HFTLog(std::string("SquadObjectProbe: label=") + label
            + " | squadPtr=" + PtrStr(squadPtr)
            + " | sharedByCount=" + IntStr(sharedCount)
            + " | sharedBy=[" + names + "]"
            + " | nameField0x78=" + PtrStr(nameField));

        LogCandidateObjectStringHits(squadPtr, label, "0x658");
        LogSquadObjectHashes(squadPtr, label, names);
        LogSquadObjectSmallFields(squadPtr, label, names);
        LogSquadMemberStorageCandidates(squadPtr, chars, label, names);
    }

    HFTLog(std::string("SquadObjectDeepEnd: label=") + label
        + " | note=compare SquadObjectHash/I32/U8 before and after formation/speed changes");
}


static std::string FindKnownSquadNameHint(void* squadPtr)
{
    if (squadPtr == NULL || !HFTReadableMemory(squadPtr, sizeof(void*)))
        return "unknown";

    bool directTest = MemoryContainsAsciiNeedle(squadPtr, 0x400, "Test")
                   || MemoryContainsWideNeedle(squadPtr, 0x400, "Test");
    bool directCaravan = MemoryContainsAsciiNeedle(squadPtr, 0x400, "Caravan")
                      || MemoryContainsWideNeedle(squadPtr, 0x400, "Caravan");

    if (directTest && directCaravan)
        return "Test,Caravan";
    if (directTest)
        return "Test";
    if (directCaravan)
        return "Caravan";

    for (size_t off = 0; off <= 0x120; off += sizeof(void*))
    {
        void* p = NULL;
        if (!ReadPointerField(squadPtr, off, &p))
            continue;
        if (p == NULL || !HFTReadableMemory(p, sizeof(void*)))
            continue;

        bool hasTest = MemoryContainsAsciiNeedle(p, 0x400, "Test")
                    || MemoryContainsWideNeedle(p, 0x400, "Test");
        bool hasCaravan = MemoryContainsAsciiNeedle(p, 0x400, "Caravan")
                       || MemoryContainsWideNeedle(p, 0x400, "Caravan");

        if (hasTest && hasCaravan)
            return "Test,Caravan@field+" + HexSize(off);
        if (hasTest)
            return "Test@field+" + HexSize(off);
        if (hasCaravan)
            return "Caravan@field+" + HexSize(off);
    }

    return "unknown";
}

// ---------------------------------------------------------------------------
// v35 Squad Join Experiment
// ---------------------------------------------------------------------------
// Confirmed read-only field map from v34:
// Character + 0x658 = squad pointer
// Squad + 0x58 = member count
// Squad + 0x5c = capacity
// Squad + 0x60 = member array (Character**)
// Squad + 0x78 = name string area
// Squad + 0xa0 = representative / first member pointer
//
// This block writes only when the user clicks Join leader sq / Undo sq.

static bool HFTWritableMemory(void* p, size_t bytes)
{
    if (p == NULL || bytes == 0)
        return false;

    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)))
        return false;

    if (mbi.State != MEM_COMMIT)
        return false;

    if ((mbi.Protect & PAGE_GUARD) != 0)
        return false;

    DWORD protect = mbi.Protect & 0xff;
    bool writable =
        protect == PAGE_READWRITE ||
        protect == PAGE_WRITECOPY ||
        protect == PAGE_EXECUTE_READWRITE ||
        protect == PAGE_EXECUTE_WRITECOPY;

    if (!writable)
        return false;

    const char* start = (const char*)p;
    const char* end = start + bytes;
    const char* regionStart = (const char*)mbi.BaseAddress;
    const char* regionEnd = regionStart + mbi.RegionSize;

    return start >= regionStart && end <= regionEnd;
}

static bool WritePointerField(void* base, size_t offset, void* value)
{
    if (base == NULL || !IsValidPtr(base))
        return false;

    char* addr = ((char*)base) + offset;
    if (!HFTWritableMemory(addr, sizeof(void*)))
        return false;

    std::memcpy(addr, &value, sizeof(void*));
    return true;
}

static bool WriteI32Field(void* base, size_t offset, int value)
{
    if (base == NULL || !IsValidPtr(base))
        return false;

    char* addr = ((char*)base) + offset;
    if (!HFTWritableMemory(addr, sizeof(int)))
        return false;

    std::memcpy(addr, &value, sizeof(int));
    return true;
}

static void* GetCharacterSquadPtr(Character* c)
{
    void* squad = NULL;
    if (c != NULL && IsValidPtr(c))
        ReadPointerField(c, 0x658, &squad);
    return squad;
}

static int GetSquadMemberCount(void* squadPtr)
{
    bool ok = false;
    int count = ReadI32Safe(squadPtr, 0x58, &ok);
    if (!ok || count < 0 || count > 64)
        return -1;
    return count;
}

static int GetSquadCapacity(void* squadPtr)
{
    bool ok = false;
    int capacity = ReadI32Safe(squadPtr, 0x5c, &ok);
    if (!ok || capacity < 0 || capacity > 64)
        return -1;
    return capacity;
}

static void* GetSquadMemberArray(void* squadPtr)
{
    void* arr = NULL;
    if (squadPtr != NULL && IsValidPtr(squadPtr))
        ReadPointerField(squadPtr, 0x60, &arr);
    return arr;
}

static Character* GetSquadMemberAt(void* squadPtr, int index)
{
    if (index < 0)
        return NULL;

    void* arr = GetSquadMemberArray(squadPtr);
    if (arr == NULL || !HFTReadableMemory(arr, sizeof(void*)))
        return NULL;

    void* value = NULL;
    if (!ReadPointerField(arr, (size_t)index * sizeof(void*), &value))
        return NULL;

    Character* c = (Character*)value;
    if (c == NULL || !IsValidPtr(c))
        return NULL;

    return c;
}

static bool SetSquadMemberAt(void* squadPtr, int index, Character* c)
{
    if (index < 0)
        return false;

    void* arr = GetSquadMemberArray(squadPtr);
    if (arr == NULL || !HFTWritableMemory(arr, sizeof(void*)))
        return false;

    return WritePointerField(arr, (size_t)index * sizeof(void*), (void*)c);
}

static int FindCharacterInSquadFirstCount(void* squadPtr, Character* c)
{
    if (squadPtr == NULL || c == NULL || !IsValidPtr(c))
        return -1;

    int count = GetSquadMemberCount(squadPtr);
    if (count < 0)
        return -1;

    for (int i = 0; i < count; ++i)
    {
        Character* member = GetSquadMemberAt(squadPtr, i);
        if (member == c)
            return i;
    }

    return -1;
}

static bool SquadContainsCharacterFirstCount(void* squadPtr, Character* c)
{
    return FindCharacterInSquadFirstCount(squadPtr, c) >= 0;
}

static bool SetCharacterSquadPtr(Character* c, void* squadPtr, const char* label)
{
    if (c == NULL || !IsValidPtr(c) || squadPtr == NULL || !IsValidPtr(squadPtr))
        return false;

    bool ok = WritePointerField(c, 0x658, squadPtr);
    HFTLog(std::string("SquadJoinSetCharPtr: label=") + label
        + " | char=" + CharacterName(c)
        + " | newSquadPtr=" + PtrStr(squadPtr)
        + " | nameHint=" + FindKnownSquadNameHint(squadPtr)
        + " | ok=" + std::string(ok ? "yes" : "no"));

    return ok;
}

static void UpdateSquadRepresentativeAfterRemove(void* squadPtr, Character* removed)
{
    if (squadPtr == NULL || !IsValidPtr(squadPtr))
        return;

    void* rep = NULL;
    ReadPointerField(squadPtr, 0xa0, &rep);

    if (rep != (void*)removed)
        return;

    int count = GetSquadMemberCount(squadPtr);
    Character* newRep = NULL;
    if (count > 0)
        newRep = GetSquadMemberAt(squadPtr, 0);

    WritePointerField(squadPtr, 0xa0, (void*)newRep);
}

static bool RemoveCharacterFromSquadList(void* squadPtr, Character* c, const char* label)
{
    if (squadPtr == NULL || c == NULL || !IsValidPtr(squadPtr) || !IsValidPtr(c))
        return false;

    int count = GetSquadMemberCount(squadPtr);
    int capacity = GetSquadCapacity(squadPtr);
    if (count < 0 || capacity < 0)
    {
        HFTLog(std::string("SquadJoinRemoveFailed: label=") + label
            + " | char=" + CharacterName(c)
            + " | squadPtr=" + PtrStr(squadPtr)
            + " | reason=bad count/capacity");
        return false;
    }

    int index = FindCharacterInSquadFirstCount(squadPtr, c);
    if (index < 0)
    {
        HFTLog(std::string("SquadJoinRemoveSkip: label=") + label
            + " | char=" + CharacterName(c)
            + " | squadPtr=" + PtrStr(squadPtr)
            + " | nameHint=" + FindKnownSquadNameHint(squadPtr)
            + " | reason=not in first memberCount entries");
        return false;
    }

    for (int i = index; i < count - 1; ++i)
    {
        Character* next = GetSquadMemberAt(squadPtr, i + 1);
        SetSquadMemberAt(squadPtr, i, next);
    }

    SetSquadMemberAt(squadPtr, count - 1, NULL);
    WriteI32Field(squadPtr, 0x58, count - 1);
    UpdateSquadRepresentativeAfterRemove(squadPtr, c);

    HFTLog(std::string("SquadJoinRemove: label=") + label
        + " | char=" + CharacterName(c)
        + " | fromSquad=" + PtrStr(squadPtr)
        + " | nameHint=" + FindKnownSquadNameHint(squadPtr)
        + " | oldCount=" + IntStr(count)
        + " | newCount=" + IntStr(count - 1));

    return true;
}

static bool AddCharacterToSquadList(void* squadPtr, Character* c, const char* label)
{
    if (squadPtr == NULL || c == NULL || !IsValidPtr(squadPtr) || !IsValidPtr(c))
        return false;

    int count = GetSquadMemberCount(squadPtr);
    int capacity = GetSquadCapacity(squadPtr);

    if (count < 0 || capacity <= 0 || count > capacity)
    {
        HFTLog(std::string("SquadJoinAddFailed: label=") + label
            + " | char=" + CharacterName(c)
            + " | squadPtr=" + PtrStr(squadPtr)
            + " | nameHint=" + FindKnownSquadNameHint(squadPtr)
            + " | reason=bad count/capacity"
            + " | count=" + IntStr(count)
            + " | capacity=" + IntStr(capacity));
        return false;
    }

    if (SquadContainsCharacterFirstCount(squadPtr, c))
    {
        HFTLog(std::string("SquadJoinAddSkip: label=") + label
            + " | char=" + CharacterName(c)
            + " | squadPtr=" + PtrStr(squadPtr)
            + " | nameHint=" + FindKnownSquadNameHint(squadPtr)
            + " | reason=already in first memberCount entries");
        return true;
    }

    if (count >= capacity)
    {
        HFTLog(std::string("SquadJoinAddFailed: label=") + label
            + " | char=" + CharacterName(c)
            + " | squadPtr=" + PtrStr(squadPtr)
            + " | nameHint=" + FindKnownSquadNameHint(squadPtr)
            + " | reason=capacity full"
            + " | count=" + IntStr(count)
            + " | capacity=" + IntStr(capacity));
        return false;
    }

    if (!SetSquadMemberAt(squadPtr, count, c))
    {
        HFTLog(std::string("SquadJoinAddFailed: label=") + label
            + " | char=" + CharacterName(c)
            + " | squadPtr=" + PtrStr(squadPtr)
            + " | nameHint=" + FindKnownSquadNameHint(squadPtr)
            + " | reason=could not write member slot");
        return false;
    }

    if (!WriteI32Field(squadPtr, 0x58, count + 1))
    {
        HFTLog(std::string("SquadJoinAddFailed: label=") + label
            + " | char=" + CharacterName(c)
            + " | squadPtr=" + PtrStr(squadPtr)
            + " | nameHint=" + FindKnownSquadNameHint(squadPtr)
            + " | reason=could not write count");
        return false;
    }

    void* rep = NULL;
    ReadPointerField(squadPtr, 0xa0, &rep);
    if (rep == NULL)
        WritePointerField(squadPtr, 0xa0, (void*)c);

    HFTLog(std::string("SquadJoinAdd: label=") + label
        + " | char=" + CharacterName(c)
        + " | toSquad=" + PtrStr(squadPtr)
        + " | nameHint=" + FindKnownSquadNameHint(squadPtr)
        + " | oldCount=" + IntStr(count)
        + " | newCount=" + IntStr(count + 1));

    return true;
}

static bool MoveCharacterToSquadObject(Character* c, void* targetSquad, const char* label, bool rememberOriginal)
{
    if (c == NULL || !IsValidPtr(c) || targetSquad == NULL || !IsValidPtr(targetSquad))
        return false;

    void* oldSquad = GetCharacterSquadPtr(c);

    HFTLog(std::string("SquadJoinMoveStart: label=") + label
        + " | char=" + CharacterName(c)
        + " | oldSquad=" + PtrStr(oldSquad)
        + " | oldNameHint=" + FindKnownSquadNameHint(oldSquad)
        + " | targetSquad=" + PtrStr(targetSquad)
        + " | targetNameHint=" + FindKnownSquadNameHint(targetSquad));

    if (oldSquad == targetSquad)
    {
        HFTLog(std::string("SquadJoinMoveSkip: label=") + label
            + " | char=" + CharacterName(c)
            + " | reason=already in target squad pointer");
        return true;
    }

    if (rememberOriginal && g_originalSquadForChar.find(c) == g_originalSquadForChar.end())
        g_originalSquadForChar[c] = oldSquad;

    int targetCount = GetSquadMemberCount(targetSquad);
    int targetCapacity = GetSquadCapacity(targetSquad);

    if (targetCount < 0 || targetCapacity <= 0 || targetCount >= targetCapacity)
    {
        HFTLog(std::string("SquadJoinMoveFailed: label=") + label
            + " | char=" + CharacterName(c)
            + " | reason=target full or unreadable"
            + " | targetCount=" + IntStr(targetCount)
            + " | targetCapacity=" + IntStr(targetCapacity));
        return false;
    }

    if (oldSquad != NULL && IsValidPtr(oldSquad))
        RemoveCharacterFromSquadList(oldSquad, c, label);

    bool added = AddCharacterToSquadList(targetSquad, c, label);
    if (!added)
    {
        HFTLog(std::string("SquadJoinMoveFailed: label=") + label
            + " | char=" + CharacterName(c)
            + " | reason=add to target failed; attempting old squad restore");

        if (oldSquad != NULL && IsValidPtr(oldSquad))
            AddCharacterToSquadList(oldSquad, c, label);

        return false;
    }

    bool ptrOk = SetCharacterSquadPtr(c, targetSquad, label);

    HFTLog(std::string("SquadJoinMoveEnd: label=") + label
        + " | char=" + CharacterName(c)
        + " | ok=" + std::string(ptrOk ? "yes" : "no"));

    return ptrOk;
}

static void MoveSingleCharacterToLeaderSquad(Character* leader, Character* member, const char* label)
{
    if (leader == NULL || member == NULL || !IsValidPtr(leader) || !IsValidPtr(member) || leader == member)
        return;

    void* targetSquad = GetCharacterSquadPtr(leader);
    if (targetSquad == NULL || !IsValidPtr(targetSquad))
    {
        HFTLog(std::string("AutoSquadJoinSkipped: label=") + label
            + " | leader=" + CharacterName(leader)
            + " | member=" + CharacterName(member)
            + " | reason=leader squad pointer unreadable");
        return;
    }

    MoveCharacterToSquadObject(member, targetSquad, label, true);
}

static void RestoreSingleCharacterOriginalSquad(Character* c, const char* label)
{
    if (c == NULL || !IsValidPtr(c))
        return;

    std::map<Character*, void*>::iterator it = g_originalSquadForChar.find(c);
    if (it == g_originalSquadForChar.end())
    {
        HFTLog(std::string("SquadRestoreSkip: label=") + label
            + " | char=" + CharacterName(c)
            + " | reason=no stored original squad");
        return;
    }

    void* originalSquad = it->second;
    if (originalSquad == NULL || !IsValidPtr(originalSquad))
    {
        HFTLog(std::string("SquadRestoreSkip: label=") + label
            + " | char=" + CharacterName(c)
            + " | reason=stored original squad invalid");
        g_originalSquadForChar.erase(it);
        return;
    }

    MoveCharacterToSquadObject(c, originalSquad, label, false);
    g_originalSquadForChar.erase(c);
}

static void RestoreRouteMemberSquadMovesForLeader(Character* leader, Character* carrier, Character* guard, const char* label)
{
    if (leader == NULL || !IsValidPtr(leader))
        return;

    HFTLog(std::string("SquadRestoreRouteStart: label=") + label
        + " | leader=" + CharacterName(leader)
        + " | carrier=" + (carrier != NULL ? CharacterName(carrier) : "none")
        + " | guard=" + (guard != NULL ? CharacterName(guard) : "none"));

    if (carrier != NULL && carrier != leader)
        RestoreSingleCharacterOriginalSquad(carrier, label);

    if (guard != NULL && guard != leader && guard != carrier)
        RestoreSingleCharacterOriginalSquad(guard, label);

    HFTLog(std::string("SquadRestoreRouteEnd: label=") + label
        + " | leader=" + CharacterName(leader));
}


static void MoveRouteMembersToLeaderSquad(const char* label)
{
    Character* leader = g_pickCharacter;
    if (leader == NULL || !IsValidPtr(leader))
    {
        HFTLog(std::string("SquadJoinFailed: label=") + label + " | reason=no leader");
        return;
    }

    void* targetSquad = GetCharacterSquadPtr(leader);
    if (targetSquad == NULL || !IsValidPtr(targetSquad))
    {
        HFTLog(std::string("SquadJoinFailed: label=") + label
            + " | leader=" + CharacterName(leader)
            + " | reason=leader has no readable squad pointer");
        return;
    }

    Character* carrier = GetCarrierFor(leader);
    Character* guard = GetGuardFor(leader);

    HFTLog(std::string("SquadJoinStart: label=") + label
        + " | leader=" + CharacterName(leader)
        + " | leaderSquad=" + PtrStr(targetSquad)
        + " | leaderSquadNameHint=" + FindKnownSquadNameHint(targetSquad)
        + " | carrier=" + (carrier != NULL ? CharacterName(carrier) : "none")
        + " | guard=" + (guard != NULL ? CharacterName(guard) : "none"));

    int moved = 0;

    if (carrier != NULL && carrier != leader)
    {
        if (MoveCharacterToSquadObject(carrier, targetSquad, label, true))
            moved++;
    }

    if (guard != NULL && guard != leader && guard != carrier)
    {
        if (MoveCharacterToSquadObject(guard, targetSquad, label, true))
            moved++;
    }

    HFTLog(std::string("SquadJoinEnd: label=") + label
        + " | leader=" + CharacterName(leader)
        + " | movedOrAlreadyThere=" + IntStr(moved));
}

static void UndoRouteMemberSquadMoves(const char* label)
{
    Character* leader = g_pickCharacter;
    if (leader == NULL || !IsValidPtr(leader))
    {
        HFTLog(std::string("SquadUndoFailed: label=") + label + " | reason=no leader");
        return;
    }

    Character* carrier = GetCarrierFor(leader);
    Character* guard = GetGuardFor(leader);

    HFTLog(std::string("SquadUndoStart: label=") + label
        + " | leader=" + CharacterName(leader)
        + " | carrier=" + (carrier != NULL ? CharacterName(carrier) : "none")
        + " | guard=" + (guard != NULL ? CharacterName(guard) : "none"));

    int restored = 0;

    Character* chars[2];
    chars[0] = carrier;
    chars[1] = guard;

    for (int i = 0; i < 2; ++i)
    {
        Character* c = chars[i];
        if (c == NULL || !IsValidPtr(c) || c == leader)
            continue;

        std::map<Character*, void*>::iterator it = g_originalSquadForChar.find(c);
        if (it == g_originalSquadForChar.end())
        {
            HFTLog(std::string("SquadUndoSkip: label=") + label
                + " | char=" + CharacterName(c)
                + " | reason=no stored original squad");
            continue;
        }

        void* originalSquad = it->second;
        if (originalSquad == NULL || !IsValidPtr(originalSquad))
        {
            HFTLog(std::string("SquadUndoSkip: label=") + label
                + " | char=" + CharacterName(c)
                + " | reason=stored original squad invalid");
            g_originalSquadForChar.erase(it);
            continue;
        }

        if (MoveCharacterToSquadObject(c, originalSquad, label, false))
            restored++;

        g_originalSquadForChar.erase(c);
    }

    HFTLog(std::string("SquadUndoEnd: label=") + label
        + " | leader=" + CharacterName(leader)
        + " | restoredOrAlreadyThere=" + IntStr(restored));
}



static std::string NameForKnownCharacterPtr(void* p, const std::vector<Character*>& chars)
{
    if (p == NULL)
        return "null";

    for (size_t i = 0; i < chars.size(); ++i)
    {
        if ((void*)chars[i] == p)
            return CharacterName(chars[i]);
    }

    return "unselected";
}

static void LogSquadSummaryForPtr(void* squadPtr, const std::vector<Character*>& chars, const char* label)
{
    if (squadPtr == NULL || !HFTReadableMemory(squadPtr, sizeof(void*)))
        return;

    bool countOk = false;
    bool capacityOk = false;
    int count = ReadI32Safe(squadPtr, 0x58, &countOk);
    int capacity = ReadI32Safe(squadPtr, 0x5c, &capacityOk);

    void* memberArray = NULL;
    bool arrayOk = ReadPointerField(squadPtr, 0x60, &memberArray);

    void* nameField10 = NULL;
    void* nameField78 = NULL;
    ReadPointerField(squadPtr, 0x10, &nameField10);
    ReadPointerField(squadPtr, 0x78, &nameField78);

    void* representative = NULL;
    ReadPointerField(squadPtr, 0xa0, &representative);

    std::string names;
    int sharedCount = 0;
    GetCharsSharingOffsetPointer(chars, 0x658, squadPtr, names, sharedCount);

    HFTLog(std::string("SquadSummary: label=") + label
        + " | squadPtr=" + PtrStr(squadPtr)
        + " | nameHint=" + FindKnownSquadNameHint(squadPtr)
        + " | sharedByCount=" + IntStr(sharedCount)
        + " | sharedBy=[" + names + "]"
        + " | memberCount0x58=" + std::string(countOk ? IntStr(count) : "unreadable")
        + " | capacity0x5c=" + std::string(capacityOk ? IntStr(capacity) : "unreadable")
        + " | memberArray0x60=" + PtrStr(memberArray)
        + " | nameField0x10=" + PtrStr(nameField10)
        + " | nameField0x78=" + PtrStr(nameField78)
        + " | representative0xa0=" + PtrStr(representative)
        + " (" + NameForKnownCharacterPtr(representative, chars) + ")");

    if (!countOk || !arrayOk || memberArray == NULL || !HFTReadableMemory(memberArray, sizeof(void*)))
    {
        HFTLog(std::string("SquadSummaryMembers: label=") + label
            + " | squadPtr=" + PtrStr(squadPtr)
            + " | unableToReadMembers");
        return;
    }

    int safeCount = count;
    if (safeCount < 0)
        safeCount = 0;
    if (safeCount > 32)
        safeCount = 32;

    std::string memberMsg = std::string("SquadSummaryMembers: label=") + label
        + " | squadPtr=" + PtrStr(squadPtr)
        + " | countUsed=" + IntStr(safeCount);

    for (int i = 0; i < safeCount; ++i)
    {
        void* memberPtr = NULL;
        bool ok = ReadPointerField(memberArray, (size_t)i * sizeof(void*), &memberPtr);

        memberMsg += " | member[" + IntStr(i) + "]=";
        if (!ok)
            memberMsg += "unreadable";
        else
            memberMsg += PtrStr(memberPtr) + "(" + NameForKnownCharacterPtr(memberPtr, chars) + ")";
    }

    HFTLog(memberMsg);

    // Log one extra slot after count. This helps identify whether the array keeps stale entries.
    if (safeCount >= 0 && safeCount < 32)
    {
        void* extraPtr = NULL;
        bool extraOk = ReadPointerField(memberArray, (size_t)safeCount * sizeof(void*), &extraPtr);
        HFTLog(std::string("SquadSummaryAfterCount: label=") + label
            + " | squadPtr=" + PtrStr(squadPtr)
            + " | slot=" + IntStr(safeCount)
            + " | ok=" + std::string(extraOk ? "yes" : "no")
            + " | ptr=" + PtrStr(extraPtr)
            + " (" + NameForKnownCharacterPtr(extraPtr, chars) + ")");
    }
}

static void LogCleanSquadSummaries(const std::vector<Character*>& chars, const char* label)
{
    std::vector<void*> squadPtrs;

    for (size_t i = 0; i < chars.size(); ++i)
    {
        void* squadPtr = NULL;
        if (ReadPointerField(chars[i], 0x658, &squadPtr))
            AddUniquePtr(squadPtrs, squadPtr);
    }

    HFTLog(std::string("SquadSummaryStart: label=") + label
        + " | uniqueSquadPtrsAtCharacter0x658=" + IntStr((int)squadPtrs.size()));

    for (size_t i = 0; i < squadPtrs.size(); ++i)
        LogSquadSummaryForPtr(squadPtrs[i], chars, label);

    HFTLog(std::string("SquadSummaryEnd: label=") + label
        + " | confirmedFields=Character+0x658 squad, Squad+0x58 count, Squad+0x5c capacity, Squad+0x60 member array, Squad+0x78 name field, Squad+0xa0 representative");
}

static void RunSquadFormationProbe(const char* label)
{
    std::vector<Character*> selected = GetSelectedCharactersForProbe();

    HFTLog(std::string("SquadProbeStart: label=") + label
        + " | selectedCount=" + IntStr((int)selected.size())
        + " | selected=[" + CharacterListNames(selected) + "]");

    std::vector<Character*> important = selected;
    AddUniqueCharacter(important, g_pickCharacter);
    AddUniqueCharacter(important, GetCarrierFor(g_pickCharacter));
    AddUniqueCharacter(important, GetGuardFor(g_pickCharacter));

    for (size_t i = 0; i < important.size(); ++i)
    {
        Character* c = important[i];
        if (c == NULL || !IsValidPtr(c))
            continue;

        Ogre::Vector3 p = c->getPosition();
        HFTLog(std::string("SquadProbeChar: label=") + label
            + " | char=" + CharacterName(c)
            + " | ptr=" + PtrStr(c)
            + " | pos=" + VecStr(p)
            + " | isLeader=" + std::string(c == g_pickCharacter ? "yes" : "no")
            + " | isCarrier=" + std::string(c == GetCarrierFor(g_pickCharacter) ? "yes" : "no")
            + " | isGuard=" + std::string(c == GetGuardFor(g_pickCharacter) ? "yes" : "no"));

        LogCharacterPermanentRowsForProbe(c, label);
    }

    LogFocusedSquadCandidates(important, label);
    LogCleanSquadSummaries(important, label);
    HFTLog(std::string("SquadProbeEnd: label=") + label
        + " | note=clean squad summary; compare memberCount/memberArray after squad changes");
}


// ---------------------------------------------------------------------------
// Picker UI: item + source + target
// ---------------------------------------------------------------------------

#define PICK_W 560
#define PICK_H 620
#define PICK_ROW_H 22

static MyGUI::Widget*     g_pickWin = NULL;
static MyGUI::EditBox*    g_pickSearch = NULL;
static MyGUI::TextBox*    g_pickTitle = NULL;
static MyGUI::TextBox*    g_pickSourceLabel = NULL;
static MyGUI::TextBox*    g_pickTargetLabel = NULL;
static MyGUI::TextBox*    g_pickCarrierLabel = NULL;
static MyGUI::TextBox*    g_pickGuardLabel = NULL;
static MyGUI::TextBox*    g_pickSourceNodeLabel = NULL;
static MyGUI::TextBox*    g_pickTargetNodeLabel = NULL;
static MyGUI::ScrollView* g_pickScroll = NULL;
static MyGUI::Widget*     g_pickCanvas = NULL;
static std::vector<GameData*> g_pickItems;
static std::vector<MyGUI::Widget*> g_pickRows;

static void PickRebuildList();
static void PickClose();
static void PickOnSearchChanged(MyGUI::EditBox* _sender);
static void PickOnItemClicked(MyGUI::WidgetPtr _sender);
static void PickOnCloseClicked(MyGUI::WidgetPtr _sender);
static void PickUpdateLabels();
static void PickOnSetSourceClicked(MyGUI::WidgetPtr _sender);
static void PickOnSetTargetClicked(MyGUI::WidgetPtr _sender);
static void PickOnSetCarrierClicked(MyGUI::WidgetPtr _sender);
static void PickOnClearCarrierClicked(MyGUI::WidgetPtr _sender);
static void PickOnSetGuardClicked(MyGUI::WidgetPtr _sender);
static void PickOnClearGuardClicked(MyGUI::WidgetPtr _sender);
static void PickOnClearRouteClicked(MyGUI::WidgetPtr _sender);
static void PickOnSetSourceNodeClicked(MyGUI::WidgetPtr _sender);
static void PickOnSetTargetNodeClicked(MyGUI::WidgetPtr _sender);
static void PickOnDebugClicked(MyGUI::WidgetPtr _sender);
static void PickOnSquadProbeClicked(MyGUI::WidgetPtr _sender);
static void PickOnSquadJoinClicked(MyGUI::WidgetPtr _sender);
static void PickOnSquadUndoClicked(MyGUI::WidgetPtr _sender);

static void PickUpdateLabels()
{
    if (g_pickSourceLabel != NULL)
        g_pickSourceLabel->setCaption("Source: " + BuildingName(GetSourceBuildingFor(g_pickCharacter)));
    if (g_pickTargetLabel != NULL)
        g_pickTargetLabel->setCaption("Target: " + BuildingName(GetTargetBuildingFor(g_pickCharacter)));
    if (g_pickCarrierLabel != NULL)
    {
        Character* carrier = GetCarrierFor(g_pickCharacter);
        g_pickCarrierLabel->setCaption(std::string("Carrier: ") + (carrier != NULL ? CharacterName(carrier) : "none"));
    }
    if (g_pickGuardLabel != NULL)
    {
        Character* guard = GetGuardFor(g_pickCharacter);
        g_pickGuardLabel->setCaption(std::string("Guard: ") + (guard != NULL ? CharacterName(guard) : "none"));
    }
    if (g_pickSourceNodeLabel != NULL)
    {
        bool has = g_sourceNodeForChar.find(g_pickCharacter) != g_sourceNodeForChar.end();
        g_pickSourceNodeLabel->setCaption(std::string("Src point: ") + (has ? VecStr(GetSourceNodeFor(g_pickCharacter)) : "auto"));
    }
    if (g_pickTargetNodeLabel != NULL)
    {
        bool has = g_targetNodeForChar.find(g_pickCharacter) != g_targetNodeForChar.end();
        g_pickTargetNodeLabel->setCaption(std::string("Tgt point: ") + (has ? VecStr(GetTargetNodeFor(g_pickCharacter)) : "auto"));
    }
    if (g_pickTitle != NULL)
    {
        GameData* cur = GetHaulItemFor(g_pickCharacter);
        g_pickTitle->setCaption(std::string("Caravan Route  |  Cargo: ") + (cur != NULL ? SafeGameDataName(cur) : "none"));
    }
}

static void PickOnSquadJoinClicked(MyGUI::WidgetPtr _sender)
{
    MoveRouteMembersToLeaderSquad("Join leader sq clicked");
}

static void PickOnSquadUndoClicked(MyGUI::WidgetPtr _sender)
{
    UndoRouteMemberSquadMoves("Undo sq clicked");
}


static void PickOnSquadProbeClicked(MyGUI::WidgetPtr _sender)
{
    g_squadProbeCounter++;
    std::string label = "manual UI probe #" + IntStr(g_squadProbeCounter);
    RunSquadFormationProbe(label.c_str());
}


static Building* GetFirstSelectedBuilding()
{
    if (ou == NULL || ou->player == NULL)
        return NULL;
    lektor<RootObject*> sel;
    ou->player->getAllSelectedObjects(sel, BUILDING);
    for (uint32_t i = 0; i < sel.count; i++)
    {
        Building* b = (sel.stuff[i] != NULL) ? static_cast<Building*>(sel.stuff[i]) : NULL;
        if (b != NULL && IsValidPtr(b))
            return b;
    }
    return NULL;
}

static Character* GetFirstSelectedCarrier(Character* owner)
{
    if (ou == NULL || ou->player == NULL)
        return NULL;
    lektor<RootObject*> sel;
    ou->player->getAllSelectedObjects(sel, CHARACTER);
    for (uint32_t i = 0; i < sel.count; i++)
    {
        Character* c = (sel.stuff[i] != NULL) ? static_cast<Character*>(sel.stuff[i]) : NULL;
        if (c != NULL && IsValidPtr(c) && c != owner)
            return c;
    }
    return NULL;
}

static Character* GetFirstSelectedGuard(Character* owner)
{
    if (ou == NULL || ou->player == NULL)
        return NULL;

    Character* currentCarrier = GetCarrierFor(owner);

    lektor<RootObject*> sel;
    ou->player->getAllSelectedObjects(sel, CHARACTER);
    for (uint32_t i = 0; i < sel.count; i++)
    {
        Character* c = (sel.stuff[i] != NULL) ? static_cast<Character*>(sel.stuff[i]) : NULL;
        if (c != NULL && IsValidPtr(c) && c != owner && c != currentCarrier)
            return c;
    }
    return NULL;
}

static void PickOnSetCarrierClicked(MyGUI::WidgetPtr _sender)
{
    if (g_pickCharacter == NULL)
        return;

    Character* carrier = GetFirstSelectedCarrier(g_pickCharacter);
    if (carrier == NULL)
    {
        HFTLog("Carrier not set: select a pack animal/second character, not the worker itself.");
    }
    else
    {
        g_carrierForChar[g_pickCharacter] = carrier;
        g_carrierHandForChar[g_pickCharacter] = hand(carrier);
        g_caravanModeForChar[g_pickCharacter] = true;
        g_stageForChar[g_pickCharacter] = CarrierHasWantedItem(g_pickCharacter) ? 1 : 0;
        g_routeConfiguredTickForChar[g_pickCharacter] = g_globalHFTTick;
        HFTLog("Carrier set for " + CharacterName(g_pickCharacter) + " -> " + CharacterName(carrier)
            + " | source=" + BuildingName(GetSourceBuildingFor(g_pickCharacter))
            + " | target=" + BuildingName(GetTargetBuildingFor(g_pickCharacter)));
        MoveSingleCharacterToLeaderSquad(g_pickCharacter, carrier, "carrier set auto squad join");
        LogRouteStatus(g_pickCharacter, "Route status after carrier");
        IssueFollowLeaderOrder(carrier, g_pickCharacter, "carrier set");

        if (GetHaulItemFor(g_pickCharacter) != NULL && CharacterHasWantedItem(g_pickCharacter))
            DoMoveWorkerCargoToCarrier(g_pickCharacter);
    }

    PickUpdateLabels();
}

static void PickOnClearCarrierClicked(MyGUI::WidgetPtr _sender)
{
    if (g_pickCharacter == NULL)
        return;

    Character* oldCarrier = GetCarrierFor(g_pickCharacter);

    if (oldCarrier != NULL && IsValidPtr(oldCarrier))
    {
        TryRemoveRealStayCloseJob(oldCarrier, g_pickCharacter, "No carrier clicked");
        RestoreSingleCharacterOriginalSquad(oldCarrier, "No carrier clicked");
    }

    g_carrierForChar.erase(g_pickCharacter);
    g_carrierHandForChar.erase(g_pickCharacter);
    g_caravanModeForChar.erase(g_pickCharacter);
    g_carrierRealFollowJobForChar.erase(g_pickCharacter);
    g_stageForChar[g_pickCharacter] = CharacterHasWantedItem(g_pickCharacter) ? 1 : 0;
    g_routeConfiguredTickForChar[g_pickCharacter] = g_globalHFTTick;
    HFTLog("Carrier cleared for " + CharacterName(g_pickCharacter) + ". Route remains active in normal worker-carry mode.");
    PickUpdateLabels();
}


static void PickOnSetGuardClicked(MyGUI::WidgetPtr _sender)
{
    if (g_pickCharacter == NULL)
        return;

    Character* guard = GetFirstSelectedGuard(g_pickCharacter);
    if (guard == NULL)
    {
        HFTLog("Guard not set: select a guard character that is not the worker or carrier.");
    }
    else
    {
        MoveSingleCharacterToLeaderSquad(g_pickCharacter, guard, "guard set auto squad join");
        std::vector<Tasker*> beforeRows = DirectReadPermanentJobRows(guard);

        g_guardForChar[g_pickCharacter] = guard;
        g_guardHandForChar[g_pickCharacter] = hand(guard);
        g_guardRealBodyguardJobForChar[g_pickCharacter] = true;
        g_routeConfiguredTickForChar[g_pickCharacter] = g_globalHFTTick;

        HFTLog("Guard set for " + CharacterName(g_pickCharacter) + " -> " + CharacterName(guard));
        TryCreateRealBodyguardJob(guard, g_pickCharacter, "guard set");
        CaptureGuardCreatedRows(g_pickCharacter, guard, beforeRows, "guard set");
        LogRouteStatus(g_pickCharacter, "Route status after guard");
    }

    PickUpdateLabels();
}

static void PickOnClearGuardClicked(MyGUI::WidgetPtr _sender)
{
    if (g_pickCharacter == NULL)
        return;

    Character* oldGuard = GetGuardFor(g_pickCharacter);
    if (oldGuard != NULL && IsValidPtr(oldGuard))
    {
        TryRemoveRealBodyguardJob(oldGuard, g_pickCharacter, "No guard clicked");
        RestoreSingleCharacterOriginalSquad(oldGuard, "No guard clicked");
    }

    g_guardForChar.erase(g_pickCharacter);
    g_guardHandForChar.erase(g_pickCharacter);
    g_guardRealBodyguardJobForChar.erase(g_pickCharacter);
    g_guardCreatedRowsForChar.erase(g_pickCharacter);
    g_routeConfiguredTickForChar[g_pickCharacter] = g_globalHFTTick;

    HFTLog("Guard cleared for " + CharacterName(g_pickCharacter) + ". Route remains active.");
    PickUpdateLabels();
}

static void PickOnClearRouteClicked(MyGUI::WidgetPtr _sender)
{
    if (g_pickCharacter == NULL)
        return;

    HardClearHFTRouteFor(g_pickCharacter, "Clear route clicked");
    HFTLog("Route hard-cleared from UI for " + CharacterName(g_pickCharacter));
    PickUpdateLabels();

    if (g_pickWin != NULL)
        g_pickWin->setVisible(false);
}

static void PickOnSetSourceNodeClicked(MyGUI::WidgetPtr _sender)
{
    if (g_pickCharacter == NULL)
        return;

    Character* marker = GetFirstSelectedCharacterOr(GetCarrierFor(g_pickCharacter));
    if (marker == NULL)
        marker = g_pickCharacter;

    g_sourceNodeForChar[g_pickCharacter] = marker->getPosition();
    g_routeConfiguredTickForChar[g_pickCharacter] = g_globalHFTTick;
    HFTLog("Source travel point set for " + CharacterName(g_pickCharacter) + " -> " + VecStr(g_sourceNodeForChar[g_pickCharacter])
        + " using " + CharacterName(marker));
    LogRouteStatus(g_pickCharacter, "Route status after source point");
    PickUpdateLabels();
}

static void PickOnSetTargetNodeClicked(MyGUI::WidgetPtr _sender)
{
    if (g_pickCharacter == NULL)
        return;

    Character* marker = GetFirstSelectedCharacterOr(GetCarrierFor(g_pickCharacter));
    if (marker == NULL)
        marker = g_pickCharacter;

    g_targetNodeForChar[g_pickCharacter] = marker->getPosition();
    g_routeConfiguredTickForChar[g_pickCharacter] = g_globalHFTTick;
    HFTLog("Target travel point set for " + CharacterName(g_pickCharacter) + " -> " + VecStr(g_targetNodeForChar[g_pickCharacter])
        + " using " + CharacterName(marker));
    LogRouteStatus(g_pickCharacter, "Route status after target point");
    PickUpdateLabels();
}

static void PickOnSetSourceClicked(MyGUI::WidgetPtr _sender)
{
    if (g_pickCharacter == NULL)
        return;
    Building* b = GetFirstSelectedBuilding();
    if (b == NULL)
    {
        g_sourceBuildingForChar.erase(g_pickCharacter);
        g_sourceHandForChar.erase(g_pickCharacter);
        HFTLog("Source cleared; no building selected");
    }
    else
    {
        Building* curTarget = GetTargetBuildingFor(g_pickCharacter);
        if (curTarget != NULL && curTarget == b)
        {
            HFTLog("Source ignored: selected source is the same as current target -> " + BuildingName(b));
        }
        else
        {
            Building* oldSource = GetSourceBuildingFor(g_pickCharacter);
            bool sameSource = (oldSource != NULL && oldSource == b);
            bool hadManualNode = (g_sourceNodeForChar.find(g_pickCharacter) != g_sourceNodeForChar.end());

            g_sourceBuildingForChar[g_pickCharacter] = b;
            g_sourceHandForChar[g_pickCharacter] = hand(b);

            if (!sameSource || !hadManualNode)
                g_sourceNodeForChar[g_pickCharacter] = b->getPosition();

            g_stageForChar[g_pickCharacter] = 0;
            g_routeConfiguredTickForChar[g_pickCharacter] = g_globalHFTTick;
            HFTLog("Source set for " + CharacterName(g_pickCharacter) + " -> " + BuildingName(b)
                + " | target=" + BuildingName(GetTargetBuildingFor(g_pickCharacter))
                + " | srcPoint=" + VecStr(GetSourceNodeFor(g_pickCharacter))
                + (sameSource && hadManualNode ? " [manual point preserved]" : " [auto point]"));
            LogRouteStatus(g_pickCharacter, "Route status after source");
        }
    }
    PickUpdateLabels();
}

static void PickOnSetTargetClicked(MyGUI::WidgetPtr _sender)
{
    if (g_pickCharacter == NULL)
        return;

    Building* b = NULL;
    if (g_pickTasker != NULL && IsValidPtr(g_pickTasker))
        b = g_pickTasker->subject.getBuilding();

    if (b == NULL || !IsValidPtr(b))
        b = GetFirstSelectedBuilding();

    if (b == NULL)
    {
        g_targetBuildingForChar.erase(g_pickCharacter);
        g_targetHandForChar.erase(g_pickCharacter);
        HFTLog("Target cleared; no target could be read from job or selection");
    }
    else
    {
        Building* curSource = GetSourceBuildingFor(g_pickCharacter);
        if (curSource != NULL && curSource == b)
        {
            HFTLog("Target ignored: selected target is the same as current source -> " + BuildingName(b));
        }
        else
        {
            Building* oldTarget = GetTargetBuildingFor(g_pickCharacter);
            bool sameTarget = (oldTarget != NULL && oldTarget == b);
            bool hadManualNode = (g_targetNodeForChar.find(g_pickCharacter) != g_targetNodeForChar.end());

            g_targetBuildingForChar[g_pickCharacter] = b;
            g_targetHandForChar[g_pickCharacter] = hand(b);

            if (!sameTarget || !hadManualNode)
                g_targetNodeForChar[g_pickCharacter] = b->getPosition();

            g_stageForChar[g_pickCharacter] = CharacterHasWantedItem(g_pickCharacter) ? 1 : 0;
            g_routeConfiguredTickForChar[g_pickCharacter] = g_globalHFTTick;
            HFTLog("Target set for " + CharacterName(g_pickCharacter) + " -> " + BuildingName(b)
                + " | source=" + BuildingName(GetSourceBuildingFor(g_pickCharacter))
                + " | tgtPoint=" + VecStr(GetTargetNodeFor(g_pickCharacter))
                + (sameTarget && hadManualNode ? " [manual point preserved]" : " [auto point]"));
            LogRouteStatus(g_pickCharacter, "Route status after target");
        }
    }
    PickUpdateLabels();
}

static void PickOnDebugClicked(MyGUI::WidgetPtr _sender)
{
    if (g_pickCharacter == NULL)
    {
        HFTLog("DBG: no character selected in picker");
        return;
    }
    GameData* want = GetHaulItemFor(g_pickCharacter);
    Building* src = GetSourceBuildingFor(g_pickCharacter);
    Building* tgt = GetTargetBuildingFor(g_pickCharacter);
    HFTLog("DBG: character=" + CharacterName(g_pickCharacter)
        + " item=" + SafeGameDataName(want)
        + " source=" + BuildingName(src) + " ptr=" + PtrStr(src)
        + " target=" + BuildingName(tgt) + " ptr=" + PtrStr(tgt)
        + " carrier=" + CharacterName(GetCarrierFor(g_pickCharacter)) + " ptr=" + PtrStr(GetCarrierFor(g_pickCharacter))
        + " srcPoint=" + VecStr(GetSourceNodeFor(g_pickCharacter))
        + " tgtPoint=" + VecStr(GetTargetNodeFor(g_pickCharacter))
        + " charHasItem=" + std::string(CharacterHasWantedItem(g_pickCharacter) ? "yes" : "no")
        + " carrierHasItem=" + std::string(CarrierHasWantedItem(g_pickCharacter) ? "yes" : "no")
        + " sourceHasItem=" + std::string(BuildingHasWantedItem(src, g_pickCharacter) ? "yes" : "no"));
}

static void PickRebuildList()
{
    if (g_pickCanvas == NULL)
        return;

    MyGUI::Gui* mg = MyGUI::Gui::getInstancePtr();
    for (size_t i = 0; i < g_pickRows.size(); i++)
    {
        if (g_pickRows[i] != NULL && mg != NULL)
            mg->destroyWidget(g_pickRows[i]);
    }
    g_pickRows.clear();

    std::string keyword = g_pickSearch != NULL ? g_pickSearch->getCaption() : "";
    std::string kw;
    for (size_t i = 0; i < keyword.size(); i++)
        kw += (char)toupper(keyword[i]);

    int y = 2;
    for (size_t i = 0; i < g_pickItems.size(); i++)
    {
        GameData* gd = g_pickItems[i];
        if (gd == NULL)
            continue;
        std::string nm = gd->name;
        if (!kw.empty())
        {
            std::string up;
            for (size_t k = 0; k < nm.size(); k++)
                up += (char)toupper(nm[k]);
            if (up.find(kw) == std::string::npos)
                continue;
        }

        MyGUI::Button* b = g_pickCanvas->createWidget<MyGUI::Button>("Kenshi_Button1",
            2, y, PICK_W - 28, PICK_ROW_H, MyGUI::Align::Default, "HFTItemRow");
        b->setCaption(nm);
        b->setTextAlign(MyGUI::Align::Left);
        b->setUserData((size_t)i);
        b->eventMouseButtonClick = MyGUI::newDelegate(PickOnItemClicked);
        g_pickRows.push_back(b);
        y += PICK_ROW_H;
    }

    if (y < PICK_H - 120)
        y = PICK_H - 120;
    g_pickCanvas->setSize(PICK_W - 20, y);
    if (g_pickScroll != NULL)
    {
        g_pickScroll->setCanvasSize(PICK_W - 20, y);
        g_pickScroll->setVisibleVScroll(true);
    }
    PickUpdateLabels();
}

static void PickLoadItems()
{
    g_pickItems.clear();
    if (ou == NULL)
        return;

    lektor<GameData*> datas;
    ou->gamedata.getDataOfType(datas, ITEM);
    ou->gamedata.getDataOfType(datas, MAP_ITEM);
    ou->gamedata.getDataOfType(datas, NEST_ITEM);
    ou->gamedata.getDataOfType(datas, WEAPON);
    ou->gamedata.getDataOfType(datas, ARMOUR);
    ou->gamedata.getDataOfType(datas, CONTAINER);
    ou->gamedata.getDataOfType(datas, CROSSBOW);
    ou->gamedata.getDataOfType(datas, LIMB_REPLACEMENT);

    for (uint32_t i = 0; i < datas.count; i++)
    {
        GameData* gd = datas.stuff[i];
        if (gd != NULL && !gd->name.empty())
            g_pickItems.push_back(gd);
    }
    HFTTrace("Loaded " + IntStr((int)g_pickItems.size()) + " item templates for picker");
}

static void PickCreateWidgets()
{
    if (g_pickWin != NULL)
        return;

    MyGUI::Gui* mg = MyGUI::Gui::getInstancePtr();
    if (mg == NULL)
        return;

    g_pickWin = mg->createWidget<MyGUI::Widget>("Kenshi_FloatingPanelSkin",
        0, 0, PICK_W, PICK_H, MyGUI::Align::Default, "Popup", "HaulFromToPickerWin");
    if (g_pickWin == NULL)
        return;

    g_pickTitle = g_pickWin->createWidget<MyGUI::TextBox>("Kenshi_GenericTextBoxFlat",
        4, 4, PICK_W - 8, 32, MyGUI::Align::Default, "HFTPickTitle");
    g_pickTitle->setCaption("Caravan Route");
    g_pickTitle->setTextAlign(MyGUI::Align::Center);

    const int labelX = 8;
    const int labelW = PICK_W - 178;
    const int btnX = PICK_W - 162;
    const int btnW = 154;

    g_pickSourceLabel = g_pickWin->createWidget<MyGUI::TextBox>("Kenshi_GenericTextBoxFlat",
        labelX, 42, labelW, 26, MyGUI::Align::Default, "HFTSourceLabel");
    g_pickSourceLabel->setTextAlign(MyGUI::Align::Left | MyGUI::Align::VCenter);

    MyGUI::Button* setSource = g_pickWin->createWidget<MyGUI::Button>("Kenshi_Button1",
        btnX, 42, btnW, 24, MyGUI::Align::Default, "HFTSetSourceBtn");
    setSource->setCaption("Set source");
    setSource->setTextAlign(MyGUI::Align::Center);
    setSource->eventMouseButtonClick = MyGUI::newDelegate(PickOnSetSourceClicked);

    g_pickTargetLabel = g_pickWin->createWidget<MyGUI::TextBox>("Kenshi_GenericTextBoxFlat",
        labelX, 72, labelW, 26, MyGUI::Align::Default, "HFTTargetLabel");
    g_pickTargetLabel->setTextAlign(MyGUI::Align::Left | MyGUI::Align::VCenter);

    MyGUI::Button* setTarget = g_pickWin->createWidget<MyGUI::Button>("Kenshi_Button1",
        btnX, 72, btnW, 24, MyGUI::Align::Default, "HFTSetTargetBtn");
    setTarget->setCaption("Set target");
    setTarget->setTextAlign(MyGUI::Align::Center);
    setTarget->eventMouseButtonClick = MyGUI::newDelegate(PickOnSetTargetClicked);

    g_pickCarrierLabel = g_pickWin->createWidget<MyGUI::TextBox>("Kenshi_GenericTextBoxFlat",
        labelX, 102, PICK_W - 304, 26, MyGUI::Align::Default, "HFTCarrierLabel");
    g_pickCarrierLabel->setTextAlign(MyGUI::Align::Left | MyGUI::Align::VCenter);

    MyGUI::Button* setCarrier = g_pickWin->createWidget<MyGUI::Button>("Kenshi_Button1",
        PICK_W - 292, 102, 140, 24, MyGUI::Align::Default, "HFTSetCarrierBtn");
    setCarrier->setCaption("Carrier");
    setCarrier->setTextAlign(MyGUI::Align::Center);
    setCarrier->eventMouseButtonClick = MyGUI::newDelegate(PickOnSetCarrierClicked);

    MyGUI::Button* clearCarrier = g_pickWin->createWidget<MyGUI::Button>("Kenshi_Button1",
        PICK_W - 146, 102, 138, 24, MyGUI::Align::Default, "HFTClearCarrierBtn");
    clearCarrier->setCaption("No carrier");
    clearCarrier->setTextAlign(MyGUI::Align::Center);
    clearCarrier->eventMouseButtonClick = MyGUI::newDelegate(PickOnClearCarrierClicked);

    g_pickGuardLabel = g_pickWin->createWidget<MyGUI::TextBox>("Kenshi_GenericTextBoxFlat",
        labelX, 132, PICK_W - 304, 26, MyGUI::Align::Default, "HFTGuardLabel");
    g_pickGuardLabel->setTextAlign(MyGUI::Align::Left | MyGUI::Align::VCenter);

    MyGUI::Button* setGuard = g_pickWin->createWidget<MyGUI::Button>("Kenshi_Button1",
        PICK_W - 292, 132, 140, 24, MyGUI::Align::Default, "HFTSetGuardBtn");
    setGuard->setCaption("Guard");
    setGuard->setTextAlign(MyGUI::Align::Center);
    setGuard->eventMouseButtonClick = MyGUI::newDelegate(PickOnSetGuardClicked);

    MyGUI::Button* clearGuard = g_pickWin->createWidget<MyGUI::Button>("Kenshi_Button1",
        PICK_W - 146, 132, 138, 24, MyGUI::Align::Default, "HFTClearGuardBtn");
    clearGuard->setCaption("No guard");
    clearGuard->setTextAlign(MyGUI::Align::Center);
    clearGuard->eventMouseButtonClick = MyGUI::newDelegate(PickOnClearGuardClicked);

    g_pickSourceNodeLabel = g_pickWin->createWidget<MyGUI::TextBox>("Kenshi_GenericTextBoxFlat",
        labelX, 162, PICK_W - 304, 24, MyGUI::Align::Default, "HFTSourceNodeLabel");
    g_pickSourceNodeLabel->setTextAlign(MyGUI::Align::Left | MyGUI::Align::VCenter);

    MyGUI::Button* setSrcNode = g_pickWin->createWidget<MyGUI::Button>("Kenshi_Button1",
        PICK_W - 292, 162, 140, 22, MyGUI::Align::Default, "HFTSetSourceNodeBtn");
    setSrcNode->setCaption("Src point");
    setSrcNode->setTextAlign(MyGUI::Align::Center);
    setSrcNode->eventMouseButtonClick = MyGUI::newDelegate(PickOnSetSourceNodeClicked);

    MyGUI::Button* clearRoute = g_pickWin->createWidget<MyGUI::Button>("Kenshi_Button1",
        PICK_W - 146, 162, 138, 22, MyGUI::Align::Default, "HFTClearRouteBtn");
    clearRoute->setCaption("Clear route");
    clearRoute->setTextAlign(MyGUI::Align::Center);
    clearRoute->eventMouseButtonClick = MyGUI::newDelegate(PickOnClearRouteClicked);

    g_pickTargetNodeLabel = g_pickWin->createWidget<MyGUI::TextBox>("Kenshi_GenericTextBoxFlat",
        labelX, 190, PICK_W - 304, 24, MyGUI::Align::Default, "HFTTargetNodeLabel");
    g_pickTargetNodeLabel->setTextAlign(MyGUI::Align::Left | MyGUI::Align::VCenter);

    MyGUI::Button* setTgtNode = g_pickWin->createWidget<MyGUI::Button>("Kenshi_Button1",
        PICK_W - 292, 190, 140, 22, MyGUI::Align::Default, "HFTSetTargetNodeBtn");
    setTgtNode->setCaption("Tgt point");
    setTgtNode->setTextAlign(MyGUI::Align::Center);
    setTgtNode->eventMouseButtonClick = MyGUI::newDelegate(PickOnSetTargetNodeClicked);

    g_pickSearch = g_pickWin->createWidget<MyGUI::EditBox>("Kenshi_EditBox",
        8, 222, PICK_W - 16, 28, MyGUI::Align::Default, "HFTPickSearch");
    g_pickSearch->setCaption("");
    g_pickSearch->eventEditTextChange = MyGUI::newDelegate(PickOnSearchChanged);

    g_pickScroll = g_pickWin->createWidget<MyGUI::ScrollView>("Kenshi_ScrollView",
        8, 258, PICK_W - 16, PICK_H - 306, MyGUI::Align::Default, "HFTPickScroll");
    if (g_pickScroll != NULL)
    {
        g_pickScroll->setVisibleVScroll(true);
        g_pickScroll->setVisibleHScroll(false);
        g_pickCanvas = g_pickScroll->getClientWidget();
        g_pickScroll->setCanvasSize(PICK_W - 16, PICK_H - 160);
    }

    MyGUI::Button* close = g_pickWin->createWidget<MyGUI::Button>("Kenshi_Button1",
        PICK_W / 2 - 55, PICK_H - 40, 110, 28, MyGUI::Align::Default, "HFTCloseBtn");
    close->setCaption("Close");
    close->setTextAlign(MyGUI::Align::Center);
    close->eventMouseButtonClick = MyGUI::newDelegate(PickOnCloseClicked);

    g_pickWin->setVisible(false);
}

static void OpenHaulFromToPicker(Character* c, Tasker* task)
{
    PickCreateWidgets();
    if (g_pickWin == NULL)
        return;

    g_pickCharacter = c;
    g_pickTasker = task;

    // If the job row has a building subject, use it as target automatically.
    if (c != NULL && task != NULL && IsValidPtr(task))
    {
        Building* targetFromTask = task->subject.getBuilding();
        if (targetFromTask != NULL && IsValidPtr(targetFromTask))
        {
            Building* curSource = GetSourceBuildingFor(c);
            if (curSource != NULL && curSource == targetFromTask)
            {
                HFTLog("Auto target ignored for " + CharacterName(c) + ": job-row target equals current source -> " + BuildingName(targetFromTask));
            }
            else
            {
                Building* oldTarget = GetTargetBuildingFor(c);
                bool sameTarget = (oldTarget != NULL && oldTarget == targetFromTask);
                bool hadManualNode = (g_targetNodeForChar.find(c) != g_targetNodeForChar.end());

                g_targetBuildingForChar[c] = targetFromTask;
                g_targetHandForChar[c] = hand(targetFromTask);

                if (!sameTarget || !hadManualNode)
                    g_targetNodeForChar[c] = targetFromTask->getPosition();

                HFTLog("Target from job row for " + CharacterName(c) + " -> " + BuildingName(targetFromTask)
                    + " | tgtPoint=" + VecStr(GetTargetNodeFor(c))
                    + (sameTarget && hadManualNode ? " [manual point preserved]" : " [auto point]"));
            }
        }
    }

    if (g_pickItems.empty())
        PickLoadItems();
    PickRebuildList();

    const MyGUI::IntSize& view = MyGUI::RenderManager::getInstance().getViewSize();
    int x = (view.width - PICK_W) / 2;
    int y = (view.height - PICK_H) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    g_pickWin->setPosition(x, y);
    g_pickWin->setVisible(true);

    HFTTrace("Opened picker for " + CharacterName(c));
}

static void PickOnItemClicked(MyGUI::WidgetPtr _sender)
{
    if (_sender == NULL || g_pickCharacter == NULL)
        return;
    size_t* pIdx = _sender->getUserData<size_t>(false);
    if (pIdx == NULL)
        return;
    size_t idx = *pIdx;
    if (idx >= g_pickItems.size())
        return;
    GameData* gd = g_pickItems[idx];
    if (gd == NULL)
        return;

    g_haulItemForChar[g_pickCharacter] = gd;
    g_stageForChar[g_pickCharacter] = (IsCaravanMode(g_pickCharacter) ? (CarrierHasWantedItem(g_pickCharacter) || CharacterHasWantedItem(g_pickCharacter)) : CharacterHasWantedItem(g_pickCharacter)) ? 1 : 0;
    g_routeConfiguredTickForChar[g_pickCharacter] = g_globalHFTTick;
    HFTLog("Haul item set for " + CharacterName(g_pickCharacter) + " -> " + gd->stringID + " / " + SafeGameDataName(gd)
        + (IsHFTConfigured(g_pickCharacter) ? " [configured]" : " [waiting for missing route data]"));
    LogRouteStatus(g_pickCharacter, "Route status after item");
    PickUpdateLabels();
}

static void PickOnSearchChanged(MyGUI::EditBox* _sender)
{
    PickRebuildList();
}

static void PickClose()
{
    if (g_pickWin != NULL)
        g_pickWin->setVisible(false);
}

static void PickOnCloseClicked(MyGUI::WidgetPtr _sender)
{
    PickClose();
}

// ---------------------------------------------------------------------------
// Orders panel hook: add H button beside permanent job rows
// ---------------------------------------------------------------------------

void (*OrdersPanel_update_orig)(OrdersPanel*, Character*) = NULL;
void OrdersPanel_update_hook(OrdersPanel* self, Character* c)
{
    g_ordersCharacter = c;
    if (OrdersPanel_update_orig != NULL)
        OrdersPanel_update_orig(self, c);
}

static std::map<MyGUI::Widget*, MyGUI::Button*> g_jobHButtons;
static std::map<MyGUI::Widget*, Tasker*> g_jobTaskForButton;
static std::map<MyGUI::Widget*, Character*> g_jobCharacterForButton;
static std::map<int, bool> g_seenOrderCellTaskKeys;


static bool TaskerBelongsToCharacter(Tasker* tk, Character* c)
{
    if (tk == NULL || c == NULL || !IsValidPtr(tk) || !IsValidPtr(c))
        return false;

    OrdersReceiver* or_ = c->getOrdersReciever();
    if (or_ == NULL || !IsValidPtr(or_))
        return false;

    char* base = (char*)or_;

    // Permanent jobs list. These offsets come from the same working SellingJob pattern.
    int pc = *(int*)(base + 0x90);
    Tasker** pa = *(Tasker***)(base + 0x98);
    if (pc >= 0 && pc < 64 && pa != NULL && IsValidPtr(pa))
    {
        for (int i = 0; i < pc; ++i)
        {
            if (pa[i] == tk)
                return true;
        }
    }

    // Temporary jobs list fallback.
    int jc = *(int*)(base + 0x78);
    Tasker** ja = *(Tasker***)(base + 0x80);
    if (jc >= 0 && jc < 64 && ja != NULL && IsValidPtr(ja))
    {
        for (int i = 0; i < jc; ++i)
        {
            if (ja[i] == tk)
                return true;
        }
    }

    return false;
}

static Character* FindOwnerForTasker(Tasker* tk)
{
    if (tk == NULL || !IsValidPtr(tk) || ou == NULL)
        return NULL;

    // Fast path: the current orders-panel character.
    if (g_ordersCharacter != NULL && TaskerBelongsToCharacter(tk, g_ordersCharacter))
        return g_ordersCharacter;

    const ogre_unordered_set<Character*>::type& chars = ou->getCharacterUpdateList();
    for (ogre_unordered_set<Character*>::type::const_iterator it = chars.begin(); it != chars.end(); ++it)
    {
        Character* c = *it;
        if (TaskerBelongsToCharacter(tk, c))
            return c;
    }

    return NULL;
}

void OnJobHButtonClicked(MyGUI::WidgetPtr _sender)
{
    Tasker* task = NULL;
    std::map<MyGUI::Widget*, Tasker*>::iterator it = g_jobTaskForButton.find(_sender);
    if (it != g_jobTaskForButton.end())
        task = it->second;

    Character* owner = FindOwnerForTasker(task);
    if (owner == NULL)
    {
        std::map<MyGUI::Widget*, Character*>::iterator cit = g_jobCharacterForButton.find(_sender);
        if (cit != g_jobCharacterForButton.end() && cit->second != NULL && IsValidPtr(cit->second))
            owner = cit->second;
    }
    if (owner == NULL)
        owner = g_ordersCharacter;

    if (owner == NULL || !IsValidPtr(owner))
    {
        HFTError("C button clicked, but no owner character could be resolved for the haul job row.");
        return;
    }

    if (task != NULL && IsValidPtr(task))
        g_characterForTasker[task] = owner;

    HFTLog("Opening route config for " + CharacterName(owner));
    OpenHaulFromToPicker(owner, task);
}

void (*OrderCellView_update_orig)(OrderCellView*, const MyGUI::IBDrawItemInfo&, OrderData*) = NULL;
void OrderCellView_update_hook(OrderCellView* self, const MyGUI::IBDrawItemInfo& info, OrderData* data)
{
    if (OrderCellView_update_orig != NULL)
        OrderCellView_update_orig(self, info, data);

    if (self == NULL || self->removeButton == NULL)
        return;
    MyGUI::Widget* w = self->getWidget();
    if (w == NULL)
        return;

    MyGUI::Button* hb = NULL;
    std::map<MyGUI::Widget*, MyGUI::Button*>::iterator it = g_jobHButtons.find(w);
    if (it != g_jobHButtons.end())
        hb = it->second;
    if (hb == NULL)
    {
        MyGUI::Widget* parent = self->removeButton->getParent();
        if (parent == NULL)
            return;
        hb = parent->createWidget<MyGUI::Button>("Kenshi_Button1",
            0, 0, self->removeButton->getWidth(), self->removeButton->getHeight(),
            MyGUI::Align::Default, "HaulFromToJobButton");
        hb->setCaption("C");
        hb->setTextAlign(MyGUI::Align::Center);
        hb->eventMouseButtonClick = MyGUI::newDelegate(OnJobHButtonClicked);
        g_jobHButtons[w] = hb;
    }

    int ex = self->removeButton->getLeft() - 3 - self->removeButton->getWidth();
    hb->setPosition(ex, self->removeButton->getTop());
    hb->setSize(self->removeButton->getWidth(), self->removeButton->getHeight());

    int key = -1;
    if (data != NULL && data->task != NULL)
        key = (int)data->task->key();

    if (key >= 0 && !g_seenOrderCellTaskKeys[key])
    {
        g_seenOrderCellTaskKeys[key] = true;
        HFTTrace("OrderCellView: visible permanent job row task=" + IntStr(key));
    }

    // v7: show E only for the vanilla Haul To job-row task found in the debug log.
    bool show = (data != NULL && data->task != NULL && key == HFT_HAUL_JOB_TASK_KEY);
    hb->setVisible(show);
    if (show)
    {
        Tasker* tk = const_cast<Tasker*>(data->task);
        g_jobTaskForButton[hb] = tk;

        Character* owner = FindOwnerForTasker(tk);
        if (owner == NULL)
            owner = g_ordersCharacter;

        if (owner != NULL && IsValidPtr(owner))
        {
            g_jobCharacterForButton[hb] = owner;
            g_characterForTasker[tk] = owner;
        }
    }
    else
    {
        g_jobTaskForButton.erase(hb);
        g_jobCharacterForButton.erase(hb);
    }
}

// ---------------------------------------------------------------------------
// Tasker score hook: suppress only configured vanilla Haul To job rows
// ---------------------------------------------------------------------------

float (*Tasker_score_orig)(Tasker*, AI*) = NULL;
float Tasker_score_hook(Tasker* self, AI* ai)
{
    float s = 0.0f;
    if (Tasker_score_orig != NULL)
        s = Tasker_score_orig(self, ai);

    if (self != NULL && (int)self->key() == HFT_HAUL_JOB_TASK_KEY)
    {
        Character* c = NULL;
        std::map<Tasker*, Character*>::iterator it = g_characterForTasker.find(self);
        if (it != g_characterForTasker.end() && it->second != NULL && IsValidPtr(it->second))
            c = it->second;

        if (c == NULL)
            c = FindOwnerForTasker(self);

        if (c != NULL && IsHFTConfigured(c) && TaskerMatchesConfiguredRoute(self, c))
        {
            g_characterForTasker[self] = c;
            g_lastHaulJobScoreTickForChar[c] = g_globalHFTTick;
            // Keep the job row visible as the user's "HaulFromTo" anchor, but stop
            // Kenshi's native haul AI from taking the same item back to the nearest storage.
            return 0.0f;
        }
    }

    return s;
}

// ---------------------------------------------------------------------------
// Save/load: store HaulFromTo data on the character AI state
// ---------------------------------------------------------------------------

typedef GameSaveState* (*SerialiseFn)(void* arg1, void* arg2, GameDataContainer* container, GameData* refList, PosRotPair* offsetPosToSubtract);
SerialiseFn serialise_orig = NULL;

static void SaveHandToAI(GameData* aiGD, const std::string& base, const hand& h)
{
    if (aiGD == NULL)
        return;
    if (!h.isNull())
    {
        aiGD->idata[base + "I"] = (int)h.index;
        aiGD->idata[base + "S"] = (int)h.serial;
        aiGD->idata[base + "TYPE"] = (int)h.type;
        aiGD->idata[base + "C"] = (int)h.container;
        aiGD->idata[base + "CS"] = (int)h.containerSerial;
    }
    else
    {
        aiGD->idata.erase(base + "I");
        aiGD->idata.erase(base + "S");
        aiGD->idata.erase(base + "TYPE");
        aiGD->idata.erase(base + "C");
        aiGD->idata.erase(base + "CS");
    }
}

static bool LoadHandFromAI(GameData* aiGD, const std::string& base, hand& out)
{
    if (aiGD == NULL)
        return false;
    auto iit = aiGD->idata.find(base + "I");
    auto sit = aiGD->idata.find(base + "S");
    auto tit = aiGD->idata.find(base + "TYPE");
    auto cit = aiGD->idata.find(base + "C");
    auto csit = aiGD->idata.find(base + "CS");

    if (iit == aiGD->idata.end() || sit == aiGD->idata.end() || tit == aiGD->idata.end())
        return false;

    out = hand((unsigned int)iit->second, (unsigned int)sit->second, (itemType)tit->second,
               cit != aiGD->idata.end() ? (unsigned int)cit->second : 0,
               csit != aiGD->idata.end() ? (unsigned int)csit->second : 0);
    return true;
}

static void SaveVecToAI(GameData* aiGD, const std::string& base, bool has, const Ogre::Vector3& v)
{
    if (aiGD == NULL)
        return;

    if (has)
    {
        aiGD->idata[base + "HAS"] = 1;
        aiGD->idata[base + "X"] = (int)(v.x * 100.0f);
        aiGD->idata[base + "Y"] = (int)(v.y * 100.0f);
        aiGD->idata[base + "Z"] = (int)(v.z * 100.0f);
    }
    else
    {
        aiGD->idata.erase(base + "HAS");
        aiGD->idata.erase(base + "X");
        aiGD->idata.erase(base + "Y");
        aiGD->idata.erase(base + "Z");
    }
}

static bool LoadVecFromAI(GameData* aiGD, const std::string& base, Ogre::Vector3& out)
{
    if (aiGD == NULL)
        return false;

    auto hit = aiGD->idata.find(base + "HAS");
    auto xit = aiGD->idata.find(base + "X");
    auto yit = aiGD->idata.find(base + "Y");
    auto zit = aiGD->idata.find(base + "Z");

    if (hit == aiGD->idata.end() || hit->second == 0 || xit == aiGD->idata.end() || yit == aiGD->idata.end() || zit == aiGD->idata.end())
        return false;

    out.x = ((float)xit->second) / 100.0f;
    out.y = ((float)yit->second) / 100.0f;
    out.z = ((float)zit->second) / 100.0f;
    return true;
}

GameSaveState* serialise_hook(void* arg1, void* arg2, GameDataContainer* container, GameData* refList, PosRotPair* offsetPosToSubtract)
{
    GameSaveState* result = serialise_orig(arg1, arg2, container, refList, offsetPosToSubtract);
    if (result == NULL)
        return result;

    Character* thisptr = (Character*)(result == (GameSaveState*)arg1 ? arg2 : arg1);
        if (thisptr == NULL || !IsValidPtr(thisptr))
            return result;
        GameData* aiGD = result->getState((itemType)GAMESTATE_AI);
        if (aiGD == NULL)
            return result;

        GameData* item = GetHaulItemFor(thisptr);
        if (item != NULL && !item->stringID.empty())
            aiGD->sdata["HFT_ITEM"] = item->stringID;
        else
            aiGD->sdata.erase("HFT_ITEM");

        hand src;
        std::map<Character*, hand>::iterator hs = g_sourceHandForChar.find(thisptr);
        if (hs != g_sourceHandForChar.end())
            src = hs->second;
        else
        {
            Building* b = GetSourceBuildingFor(thisptr);
            if (b != NULL)
                src = hand(b);
            else
                src.setNull();
        }
        SaveHandToAI(aiGD, "HFT_SRC_", src);

        hand tgt;
        std::map<Character*, hand>::iterator ht = g_targetHandForChar.find(thisptr);
        if (ht != g_targetHandForChar.end())
            tgt = ht->second;
        else
        {
            Building* b = GetTargetBuildingFor(thisptr);
            if (b != NULL)
                tgt = hand(b);
            else
                tgt.setNull();
        }
        SaveHandToAI(aiGD, "HFT_TGT_", tgt);

        hand car;
        std::map<Character*, hand>::iterator hc = g_carrierHandForChar.find(thisptr);
        if (hc != g_carrierHandForChar.end())
            car = hc->second;
        else
        {
            Character* carrier = GetCarrierFor(thisptr);
            if (carrier != NULL)
                car = hand(carrier);
            else
                car.setNull();
        }
        SaveHandToAI(aiGD, "HFT_CAR_", car);

        hand guardH;
        std::map<Character*, hand>::iterator hg = g_guardHandForChar.find(thisptr);
        if (hg != g_guardHandForChar.end())
            guardH = hg->second;
        else
        {
            Character* guard = GetGuardFor(thisptr);
            if (guard != NULL)
                guardH = hand(guard);
            else
                guardH.setNull();
        }
        SaveHandToAI(aiGD, "HFT_GUARD_", guardH);

        if (IsCaravanMode(thisptr))
            aiGD->idata["HFT_CARAVAN"] = 1;
        else
            aiGD->idata.erase("HFT_CARAVAN");

        std::map<Character*, Ogre::Vector3>::iterator sn = g_sourceNodeForChar.find(thisptr);
        SaveVecToAI(aiGD, "HFT_SRCNODE_", sn != g_sourceNodeForChar.end(), sn != g_sourceNodeForChar.end() ? sn->second : Ogre::Vector3::ZERO);

        std::map<Character*, Ogre::Vector3>::iterator tn = g_targetNodeForChar.find(thisptr);
        SaveVecToAI(aiGD, "HFT_TGTNODE_", tn != g_targetNodeForChar.end(), tn != g_targetNodeForChar.end() ? tn->second : Ogre::Vector3::ZERO);
    return result;
}

void (*loadFromSerialise_orig)(Character* thisptr, GameSaveState* state) = NULL;
void loadFromSerialise_hook(Character* thisptr, GameSaveState* state)
{
    if (loadFromSerialise_orig != NULL)
        loadFromSerialise_orig(thisptr, state);

    if (thisptr == NULL || state == NULL || !IsValidPtr(thisptr))
            return;
        GameData* aiGD = state->getState((itemType)GAMESTATE_AI);
        if (aiGD == NULL)
            return;

        auto itemIt = aiGD->sdata.find("HFT_ITEM");
        if (itemIt != aiGD->sdata.end() && !itemIt->second.empty() && ou != NULL)
        {
            std::string want = itemIt->second;
            GameData* found = NULL;
            lektor<GameData*> datas;
            ou->gamedata.getDataOfType(datas, ITEM);
            ou->gamedata.getDataOfType(datas, MAP_ITEM);
            ou->gamedata.getDataOfType(datas, NEST_ITEM);
            ou->gamedata.getDataOfType(datas, WEAPON);
            ou->gamedata.getDataOfType(datas, ARMOUR);
            ou->gamedata.getDataOfType(datas, CONTAINER);
            ou->gamedata.getDataOfType(datas, CROSSBOW);
            ou->gamedata.getDataOfType(datas, LIMB_REPLACEMENT);
            for (uint32_t i = 0; i < datas.count; i++)
            {
                if (datas.stuff[i] != NULL && datas.stuff[i]->stringID == want)
                {
                    found = datas.stuff[i];
                    break;
                }
            }
            if (found != NULL)
                g_haulItemForChar[thisptr] = found;
            else
                g_haulItemForChar.erase(thisptr);
        }
        else
        {
            g_haulItemForChar.erase(thisptr);
        }

        hand src;
        if (LoadHandFromAI(aiGD, "HFT_SRC_", src) && !src.isNull())
        {
            g_sourceHandForChar[thisptr] = src;
            Building* b = src.getBuilding();
            if (b != NULL && IsValidPtr(b))
                g_sourceBuildingForChar[thisptr] = b;
        }

        hand tgt;
        if (LoadHandFromAI(aiGD, "HFT_TGT_", tgt) && !tgt.isNull())
        {
            g_targetHandForChar[thisptr] = tgt;
            Building* b = tgt.getBuilding();
            if (b != NULL && IsValidPtr(b))
                g_targetBuildingForChar[thisptr] = b;
        }

        auto caravanIt = aiGD->idata.find("HFT_CARAVAN");
        if (caravanIt != aiGD->idata.end() && caravanIt->second != 0)
            g_caravanModeForChar[thisptr] = true;
        else
            g_caravanModeForChar.erase(thisptr);

        hand car;
        if (LoadHandFromAI(aiGD, "HFT_CAR_", car) && !car.isNull())
        {
            g_carrierHandForChar[thisptr] = car;
            RootObject* ro = car.getRootObject();
            if (ro != NULL && IsValidPtr(ro))
                g_carrierForChar[thisptr] = static_cast<Character*>(ro);
        }

        hand guardH;
        if (LoadHandFromAI(aiGD, "HFT_GUARD_", guardH) && !guardH.isNull())
        {
            g_guardHandForChar[thisptr] = guardH;
            RootObject* ro = guardH.getRootObject();
            if (ro != NULL && IsValidPtr(ro))
                g_guardForChar[thisptr] = static_cast<Character*>(ro);
        }

        Ogre::Vector3 srcNode;
        if (LoadVecFromAI(aiGD, "HFT_SRCNODE_", srcNode))
            g_sourceNodeForChar[thisptr] = srcNode;

        Ogre::Vector3 tgtNode;
        if (LoadVecFromAI(aiGD, "HFT_TGTNODE_", tgtNode))
            g_targetNodeForChar[thisptr] = tgtNode;

        if (IsHFTConfigured(thisptr))
            g_routeConfiguredTickForChar[thisptr] = g_globalHFTTick;
}

// ---------------------------------------------------------------------------
// Plugin entry point
// ---------------------------------------------------------------------------

static void HookResult(const char* name, int status)
{
    if (status != KenshiLib::SUCCESS)
        HFTError(std::string("failed to hook ") + name);
    else
        HFTLog(std::string("hooked ") + name);
}

__declspec(dllexport) void startPlugin()
{
    SetUnhandledExceptionFilter(HFTUnhandledExceptionFilter);
    HFTLog("startPlugin called. HaulFromTo v43c loaded.");

    HookResult("GameWorld::_NV_mainLoop_GPUSensitiveStuff",
        KenshiLib::AddHook(KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
                           (void*)&GameWorld_update_hook,
                           (void**)&GameWorld_update_orig));

    HookResult("PlayerInterface::newPlayerTaskSelectedCharacters",
        KenshiLib::AddHook(KenshiLib::GetRealAddress(&PlayerInterface::newPlayerTaskSelectedCharacters),
                           (void*)&newPlayerTaskSelectedCharacters_hook,
                           (void**)&newPlayerTaskSelectedCharacters_orig));

    HookResult("OrdersPanel::update",
        KenshiLib::AddHook(KenshiLib::GetRealAddress(&OrdersPanel::update),
                           (void*)&OrdersPanel_update_hook,
                           (void**)&OrdersPanel_update_orig));

    HookResult("OrderCellView::update",
        KenshiLib::AddHook(KenshiLib::GetRealAddress(&OrderCellView::update),
                           (void*)&OrderCellView_update_hook,
                           (void**)&OrderCellView_update_orig));

    HookResult("Tasker::score",
        KenshiLib::AddHook(KenshiLib::GetRealAddress(&Tasker::score),
                           (void*)&Tasker_score_hook,
                           (void**)&Tasker_score_orig));

    HookResult("Character::_NV_serialise",
        KenshiLib::AddHook(KenshiLib::GetRealAddress(&Character::_NV_serialise),
                           (void*)&serialise_hook,
                           (void**)&serialise_orig));

    HookResult("Character::_NV_loadFromSerialise",
        KenshiLib::AddHook(KenshiLib::GetRealAddress(&Character::_NV_loadFromSerialise),
                           (void*)&loadFromSerialise_hook,
                           (void**)&loadFromSerialise_orig));

    HFTLog("Ready v43c. Mod List Description Release enabled.");
    HFTLog("Debug toggle: create HaulFromTo_debug_on.txt next to the DLL to enable full logs.");
}
