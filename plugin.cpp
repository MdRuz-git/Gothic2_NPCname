#define gouthic2 
#include "plugin.h"
#include "UnionAfx.h"
#include <../UnionSDK/console/Console.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>
#include <set>
#include <algorithm>

using namespace Common;
using namespace UnionCore;
#ifdef gouthic1
using namespace Gothic_I_Classic;
#include <../Gothic_I_Classic/API/oNpc.h>
#include <../Gothic_I_Classic/API/oItem.h>
#include <../Gothic_I_Classic/API/zOption.h>
#endif
#ifdef gouthic2
using namespace Gothic_II_Addon;
#include <../Gothic_II_Addon/API/oNpc.h>
#include <../Gothic_II_Addon/API/oItem.h>
#include <../Gothic_II_Addon/API/zOption.h>
#endif

enum { MODE_ALL = 0, MODE_QUEST = 1, MODE_OFF = 2, MODE_NOTRADER = 3 };

static int   g_StartMode = MODE_ALL;
static int   g_Mode = MODE_ALL;
static float g_Radius = 3500.0f;
static bool  g_RunConditions = true;   // false = never execute any script, show names for any untold dialogue
static const bool g_PurityGate = true;      // true = only execute condition scripts proven read-only by bytecode scan
static bool  g_HideInFocus = true;
static float g_MinDistance = 0.0f;

static int  g_ChainDepth = 3;      // quest fixpoint hops, 0 = off



// 0 = off
// 1 = log classification changes + skipped (impure) condition scripts
// 2 = like 1, plus grey names for NPCs without any flag
// 3 = like 2, plus hash of global script variables around every real InfoConditions() call
static int   g_DebugMode = 0;
#define MD_DEBUG 0 //switch to 0 on release
#define MD_SHADOWVM 0 //useless


#if MD_SHADOWVM
static bool g_ShadowVm = true;     // evaluate gate-rejected conditions in the read-only shadow VM
static bool g_qDirty = false;      // chain depth changed -> rebuild quest index
#endif

static const float NN_HEAD_OFFSET = 20.0f;

static float       g_DistNear = 500.0f;
static float       g_DistMid = 1000.0f;
static std::string g_FontNear = "FONT_OLD_10_WHITE.TGA";
static std::string g_FontMid = "FONT_DEFAULT.TGA";
static std::string g_FontFar = "FONT_DEFAULT.TGA";

static bool g_fontsDirty = true;
static zSTRING* g_fontZ[3] = { nullptr, nullptr, nullptr };
static zSTRING* g_traderPrefix = nullptr;

static const DWORD NN_CACHE_MS = 4500;
static const int   NN_MAX_CHECKS_FRAME = 3;

static std::string g_ExtraPureCsv;       // from ini: ExtraPureExternals
static bool        g_extraPureDirty = true;

enum {
    NN_TRADER = 1,
    NN_QUEST = 2,
    NN_IMPORTANT = 4,
    NN_TALK = 8      // available, untold, but not a quest start (weak tier)
};

static zCOLOR NnColor(int flags) {
    if (flags & NN_IMPORTANT) return zCOLOR(80, 200, 255);
    if (flags & NN_QUEST)     return zCOLOR(255, 128, 0);
    if (flags & NN_TRADER)    return zCOLOR(255, 215, 0);
    if (flags & NN_TALK)      return zCOLOR(255, 205, 160);   // pale orange
    return zCOLOR(150, 150, 150);
}

static std::string NnLogPath() {
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string p = buf;
    size_t s = p.find_last_of("\\/");
    return (s == std::string::npos ? std::string() : p.substr(0, s + 1)) + "NpcNames.log";
}

static void NnLog(const std::string& line, bool truncate = false) {
    FILE* f = nullptr;
    errno_t err = fopen_s(&f, NnLogPath().c_str(), truncate ? "w" : "a");
    if (err != 0) return;
    if (f == nullptr) return;
    fprintf(f, "%s\n", line.c_str());
    fclose(f);
}
static bool KeyDownNow(int k) { return zinput && zinput->KeyPressed(k) != 0; }
static bool KeyEdge(int k) { return zinput && zinput->KeyToggled(k) != 0; }

static void LoadConfig() {
    if (!zoptions) return;

    g_DistNear = zoptions->ReadReal("NpcNames", "DistNear", 500.0f);
    g_DistMid = zoptions->ReadReal("NpcNames", "DistMid", 1000.0f);
    g_FontNear = zoptions->ReadString("NpcNames", "FontNear", "FONT_OLD_10_WHITE.TGA").ToChar();
    g_FontMid = zoptions->ReadString("NpcNames", "FontMid", "FONT_DEFAULT.TGA").ToChar();
    g_FontFar = zoptions->ReadString("NpcNames", "FontFar", "FONT_DEFAULT.TGA").ToChar();

    if (g_FontNear.empty()) g_FontNear = "FONT_OLD_10_WHITE.TGA";
    if (g_FontMid.empty())  g_FontMid = "FONT_DEFAULT.TGA";
    if (g_FontFar.empty())  g_FontFar = "FONT_DEFAULT.TGA";
    g_fontsDirty = true;

    g_StartMode = zoptions->ReadInt("NpcNames", "StartMode", 0);
    g_Radius = zoptions->ReadReal("NpcNames", "Radius", 1500.0f);
    g_RunConditions = zoptions->ReadBool("NpcNames", "RunConditions", true) != 0;
//    g_PurityGate = zoptions->ReadBool("NpcNames", "PurityGate", true) != 0;
    g_HideInFocus = zoptions->ReadBool("NpcNames", "HideInFocus", true) != 0;
    g_MinDistance = zoptions->ReadReal("NpcNames", "MinDistance", 0.0f);
    g_DebugMode = zoptions->ReadInt("NpcNames", "DebugMode", 0);

#if MD_SHADOWVM
    g_ShadowVm = zoptions->ReadBool("NpcNames", "ShadowVM", true) != 0;
    {
        const int oldDepth = g_ChainDepth;
        g_ChainDepth = zoptions->ReadInt("NpcNames", "QuestChainDepth", 2);
        if (g_ChainDepth < 0) g_ChainDepth = 0;
        if (g_ChainDepth > 4) g_ChainDepth = 4;
        if (oldDepth != g_ChainDepth) g_qDirty = true;
    }
#endif
    if (g_StartMode < 0 || g_StartMode > 3) g_StartMode = MODE_ALL;
    if (g_Radius < 100.0f)    g_Radius = 100.0f;
    if (g_MinDistance < 0.0f) g_MinDistance = 0.0f;

    g_ExtraPureCsv = zoptions->ReadString("NpcNames", "ExtraPureExternals", "").ToChar();
    g_extraPureDirty = true;
}

static void NnClearCache();

static bool NnGameHasFocus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static std::string g_Msg;
static DWORD       g_MsgUntil = 0;
static bool        g_ViewDirty = false;

static void NnHandleToggle() {
    static bool wasDown = false;

    bool down = (KeyDownNow(KEY_LSHIFT)
        && KeyEdge(KEY_K)
        && NnGameHasFocus());

    if (down && !wasDown) {
        static const int order[4] = { MODE_ALL, MODE_NOTRADER, MODE_QUEST, MODE_OFF };
        int k = 0;
        for (int i = 0; i < 4; ++i) if (order[i] == g_Mode) k = i;
        g_Mode = order[(k + 1) % 4];
        NnClearCache();

        g_Msg = (g_Mode == MODE_ALL) ? "NPC names: quest / talk / trader / important"
            : (g_Mode == MODE_NOTRADER) ? "NPC names: quest / talk / important"
            : (g_Mode == MODE_QUEST) ? "NPC names: quest / trader"
            : "NPC names: OFF";
        g_MsgUntil = GetTickCount() + 2000;
        g_ViewDirty = true;
        if (g_DebugMode >= 1) NnLog("[mode] " + g_Msg);
    }
    wasDown = down;
}

// ============================================================================
// Script symbol / bytecode helpers
// ============================================================================
static const int NN_SYM_FLOAT = 1, NN_SYM_INT = 2, NN_SYM_STRING = 3, NN_SYM_FUNC = 5;
static const int NN_SYM_INSTANCE = 7;
static const int NN_FLAG_CONST = 1, NN_FLAG_CLASSVAR = 4, NN_FLAG_EXTERNAL = 8;

// Bounds-checked symbol lookup in the game parser's symbol table.
static zCPar_Symbol* NnSym(int idx) {
    if (!parser || idx <= 0 || idx >= parser->symtab.GetNumInList()) return nullptr;
    return parser->GetSymbol(idx);
}

// Number of operand bytes that follow an opcode in the compiled script stack.
static int NnOperandSize(int tok) {
    switch (tok) {
    case zPAR_TOK_CALL: case zPAR_TOK_CALLEXTERN: case zPAR_TOK_PUSHINT:
    case zPAR_TOK_PUSHVAR: case zPAR_TOK_PUSHINST: case zPAR_TOK_JUMP:
    case zPAR_TOK_JUMPF: case zPAR_TOK_SETINSTANCE:
        return 4;
    case (zPAR_TOK_PUSHVAR | zPAR_TOK_FLAGARRAY):
        return 5;   // 4 byte symbol + 1 byte array index
    default:
        return 0;
    }
}static_assert((zPAR_TOK_PUSHVAR | zPAR_TOK_FLAGARRAY) == 0xF5, "array push opcode must be 0xF5");

// Opcodes that write to the symbol pushed right before them.
static bool NnIsAssignOp(int tok) {
    return tok == zPAR_OP_IS || tok == zPAR_OP_ISPLUS || tok == zPAR_OP_ISMINUS ||
        tok == zPAR_OP_ISMUL || tok == zPAR_OP_ISDIV ||
        tok == zPAR_TOK_ASSIGNSTR || tok == zPAR_TOK_ASSIGNSTRP ||
        tok == zPAR_TOK_ASSIGNFUNC || tok == zPAR_TOK_ASSIGNFLOAT ||
        tok == zPAR_TOK_ASSIGNINST;
}

// ============================================================================
// Purity gate: static proof that a condition script has no side effects
// ============================================================================
// Purity verdict per condition-function symbol (script bytecode never changes at runtime).
static std::unordered_map<int, bool> g_purity;

static const int NN_QUEST_SCAN_PER_FRAME = 15; //at 3 and depth=3 takes about 17sec to hit; 20-80 sweet spot
struct NnScanEnv {
    int createTopic = 0, setStatus = 0, addChoice = 0, knowsInfo = 0;
    int runningSym = 0, runningVal = 0; bool hasRunning = false;
    int noteSym = 0, noteVal = 0;       bool hasNote = false;
    int addEntry = 0;
};
struct NnInfoRec {
    oCInfo* info = nullptr;
    bool direct = false;          // body itself starts a quest
    bool quest = false;           // direct, or reached through the fixpoint
    int  level = 0;               // 0 none, 1 direct, 2.. = hops away from a direct quest start
    std::string how;
    std::vector<int> writes;      // global int vars written by the body
    std::vector<int> reads;       // global int vars read by the condition
    std::vector<int> knows;       // info instances whose told-state the condition tests
};
static std::vector<NnInfoRec> g_qRecs;
static std::unordered_map<oCInfo*, int> g_qIndexOf;
static size_t g_qPos = 0;
static bool g_qStarted = false, g_qBuilt = false;
static NnScanEnv g_qEnv;
static bool g_qEnvValid = false;



#if MD_SHADOWVM
#pragma region ShadowVM
// ---- quest classifier + shadow VM state (everything here is reset on every script reload) ----

static std::unordered_map<int, oCInfo*> g_infoByInst;
static bool g_infoByInstValid = false;
static std::unordered_map<int, int> g_shExt;          // external symbol idx -> shadow implementation id
static bool g_shTablesValid = false;
static int g_shSelf = -1, g_shOther = -1, g_shHero = -1, g_shPcHero = -1;
static int g_shBase = 0, g_shBaseState = 0;            // C_NPC class base inside oCNpc; 0 untried, 1 ok, -1 failed
static int g_shAgree = 0, g_shDisagree = 0, g_shSkipped = 0;
static std::unordered_set<int> g_shLogged;
#pragma endregion ShadowVM
#endif

static void NnQuestIndexReset() {
    g_qRecs.clear(); g_qIndexOf.clear(); g_qPos = 0;
    g_qStarted = false; g_qBuilt = false; g_qEnvValid = false;
#if MD_SHADOWVM
    g_qDirty = false;
    g_infoByInst.clear(); g_infoByInstValid = false;
    g_shExt.clear(); g_shTablesValid = false;
    g_shBase = 0; g_shBaseState = 0;
    g_shLogged.clear();
#endif
}

static std::unordered_set<std::string> g_extraPure;

struct NnExtStat { int clean = 0, dirty = 0; };
static std::map<std::string, NnExtStat> g_extStat;                       // external -> runs seen clean / dirty
static std::unordered_map<int, std::vector<std::string>> g_condExternals; // condition fn -> externals it can call

// Parses the comma/semicolon/space separated ini list into upper-case symbol names.
static void NnParseExtraPure() {
    g_extraPure.clear();
    std::string cur;
    for (size_t i = 0; i <= g_ExtraPureCsv.size(); ++i) {
        const char c = (i < g_ExtraPureCsv.size()) ? g_ExtraPureCsv[i] : ',';
        if (c == ',' || c == ';' || c == ' ') {
            if (!cur.empty()) { g_extraPure.insert(cur); cur.clear(); }
        }
        else cur += static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }
}

// Curated allowlist of externals that only read engine state.
// Excluded on purpose: setters (Npc_Set*, Npc_Give*, Npc_Remove*, Npc_Change*...), anything that
// writes SELF/OTHER/VICTIM/ITEM-style globals (Npc_GetTarget/GetNextTarget/GetInvItem*/GetDetectedMob,
// Wld_Detect*), anything that runs scripts (Npc_CheckInfo), RNG (Hlp_Random), news cache (Npc_HasNews),
// and every AI_*, Wld_Insert*/Set*, Log_Create/AddEntry/SetTopicStatus, Mis_Set*/Add*/Remove*, Mob_Create*/Set*.
static const std::unordered_set<std::string>& NnPureExternalSet() {
    static const std::unordered_set<std::string> ok = {

        // 100%: pure reads (decompiled wrapper + callee)
        "NPC_KNOWSINFO","NPC_ISINSTATE","NPC_GETTALENTSKILL",
        "NPC_GETDISTTOWP","NPC_GETNEARESTWP","NPC_GETDISTTONPC","NPC_ISDEAD",
        "NPC_GETEQUIPPEDARMOR","NPC_HASEQUIPPEDARMOR","NPC_GETBODYSTATE",
        "NPC_REFUSETALK",
        "HLP_GETINSTANCEID","HLP_ISITEM",
        "WLD_GETDAY","WLD_ISTIME","WLD_ISRAINING",
        "HLP_STRCMP","HLP_ISVALIDNPC",
        
        "NPC_HASITEMS", // Engine-sanctioned lazy unpack of the packed inventory (see notes). Remove this line to be strict.
        "HLP_GETNPC" //a store to a scratch symbol, which is safe because we only evaluate when the parser is idle.


        /*
        LEFT OFF: "MOB_HASITEMS" (8 infos) cant find virtual call that it makes which makes it not 100% safe
        
        Never seen in conditions / unaudited: NPC_ISPLAYER, NPC_GETTRUEGUILD, NPC_GETATTITUDE,
        MIS_GETSTATUS, LOG_GETTOPICSTATUS, WLD_GETTIMEMIN, ...
        
        State changers: NPC_CANSEENPC, 100% triggers it 
        assumed:
        NPC_CANSEEITEM, NPC_CANSEESOURCE,
        NPC_CANSEENPCFREELOS, NPC_HASDETECTEDNPC
        */
    };
    return ok;
}



static bool NnIsPureExternal(const char* n) {
    if (!n) return false;
    return NnPureExternalSet().count(n) != 0 || g_extraPure.count(n) != 0;
}

// Logs allowlist entries that do not exist in the loaded scripts or are not engine externals.
static void NnAuditAllowlist() {
    if (!parser) return;
    if (g_extraPureDirty) { NnParseExtraPure(); g_purity.clear(); g_extraPureDirty = false; }

    int ok = 0, missing = 0, notExt = 0;
    auto check = [&](const std::string& n, const char* src) {
        const int idx = parser->GetIndex(n.c_str());
        zCPar_Symbol* s = (idx > 0) ? NnSym(idx) : nullptr;
        if (!s) { ++missing; NnLog("[allowlist] missing in this game: " + n + " (" + src + ")"); }
        else if (s->type != NN_SYM_FUNC || !(s->flags & NN_FLAG_EXTERNAL)) { ++notExt;  NnLog("[allowlist] exists but is NOT an external(remove!): " + n + " (" + src + ")"); }
        else ++ok;
        };
    for (const std::string& n : NnPureExternalSet()) check(n, "builtin");
    for (const std::string& n : g_extraPure)         check(n, "ini");

    char b[128];
    sprintf_s(b, "[allowlist] audit: %d ok, %d missing, %d not-external", ok, missing, notExt);
    NnLog(b);
    NnLog("[parser] running-function marker = " + std::to_string(
        *reinterpret_cast<const int*>(reinterpret_cast<const char*>(parser) + 0x219C)));
}
        


struct NnPurityCtx {
    std::unordered_set<int> visited;   // bytecode positions already verified (or in progress)
    std::string reason;                // why the scan rejected the script
    std::set<std::string> externals;   // every external the script (and its callees) can call
};



// An assignment is harmless only if the target is a function local/parameter
// (name "FUNC.VAR" and not a class member). Globals have no '.', class members
// (self.aivar[..], other.attribute[..]) carry the class-var flag.
static bool NnIsLocalTarget(int symIdx) {
    zCPar_Symbol* s = NnSym(symIdx);
    if (!s) return false;
    if (s->flags & NN_FLAG_CLASSVAR) return false;      // class member: writes engine objects
    const char* nm = s->name.ToChar();
    const char* dot = nm ? std::strchr(nm, '.') : nullptr;
    if (!dot || dot == nm) return false;                // globals have no '.'
    // A function-local variable or parameter is named "FUNCTION.VARIABLE"; the owner must be a function.
    const std::string owner(nm, dot);
    const int oi = parser->GetIndex(owner.c_str());
    zCPar_Symbol* os = (oi > 0) ? NnSym(oi) : nullptr;
    return os && os->type == NN_SYM_FUNC;
}

// Walks a script function (and every script function it calls) and returns true only if it
// calls whitelisted externals and writes nothing except its own locals.
static bool NnScanPure(int pos, int depth, NnPurityCtx& cx) {
    if (!parser || !parser->stack.stack) { cx.reason = "no parser"; return false; }
    const zBYTE* code = parser->stack.stack;
    const int maxBytes = parser->stack.stacksize;
    if (pos < 0 || pos >= maxBytes) { cx.reason = "bad bytecode position"; return false; }
    if (!cx.visited.insert(pos).second) return true;
    if (depth <= 0) { cx.reason = "call depth exceeded"; return false; }

    int cur = pos, maxJump = 0, steps = 0, lastPush = -1;
    while (cur < maxBytes && steps++ < 8192) {
        const int tok = code[cur++];
        const int os = NnOperandSize(tok);
        if (cur + os > maxBytes) break;
        const int operand = (os >= 4) ? *reinterpret_cast<const int*>(code + cur) : 0;
        cur += os;

        if (tok == zPAR_TOK_RET) {
            // A RET located before the furthest forward jump target is an early return inside an if-block.
            if (cur > maxJump) return true;
            lastPush = -1;
        }
        else if (tok == zPAR_TOK_PUSHVAR || tok == (zPAR_TOK_PUSHVAR | zPAR_TOK_FLAGARRAY) ||
            tok == zPAR_TOK_PUSHINST) {
            lastPush = operand;
        }
        else if (tok == zPAR_TOK_JUMP || tok == zPAR_TOK_JUMPF) {
            if (operand > maxJump && operand < maxBytes) maxJump = operand;
            lastPush = -1;
        }
        else if (tok == zPAR_TOK_CALL) {
            if (!NnScanPure(operand, depth - 1, cx)) return false;
            lastPush = -1;
        }
        else if (tok == zPAR_TOK_CALLEXTERN) {
            zCPar_Symbol* fs = NnSym(operand);
            const char* nm = fs ? fs->name.ToChar() : nullptr;
            if (nm) cx.externals.insert(nm);
            if (!NnIsPureExternal(nm)) {
                cx.reason = std::string("external ") + (nm ? nm : "?");
                return false;
            }
            lastPush = -1;
        }
        else if (NnIsAssignOp(tok)) {
            // The engine pops the assignment target first, so the target must be the push immediately
            // before this opcode (lastPush is reset by every other opcode).
            if (!NnIsLocalTarget(lastPush)) {
                zCPar_Symbol* ts = (lastPush > 0) ? NnSym(lastPush) : nullptr;
                cx.reason = ts ? (std::string("writes ") + ts->name.ToChar())
                    : std::string("assign without a variable target");
                return false;
            }
            lastPush = -1;
        }
        else {
            lastPush = -1;
        }
    }
    cx.reason = "no RET / step limit";
    return false;
}



// True if the condition may be executed. No condition function counts as pure (nothing to execute).
static bool NnConditionIsPure(oCInfo* info) {
    // Re-parse the ini allowlist after (re)loading the config and drop verdicts made with the old list.
    if (g_extraPureDirty) { NnParseExtraPure(); g_purity.clear(); g_extraPureDirty = false; }

    const int fn = info->pd.conditions;
    if (fn <= 0) return true;

    auto it = g_purity.find(fn);
    if (it != g_purity.end()) return it->second;

    bool pure = false;
    NnPurityCtx cx;
    zCPar_Symbol* s = NnSym(fn);
    if (s && s->type == NN_SYM_FUNC && (s->flags & NN_FLAG_EXTERNAL) == 0)
        pure = NnScanPure(s->single_intdata, 8, cx);
    else
        cx.reason = "not a script function";

    g_purity[fn] = pure;
#if MD_DEBUG
    g_condExternals[fn].assign(cx.externals.begin(), cx.externals.end());
#endif
    if (!pure && g_DebugMode >= 1)
        NnLog(std::string("[impure-skip] ") + info->name.ToChar() + " : " + cx.reason);
    return pure;
}

// ============================================================================
// Debug verification: hash of all mutable global script variables
// ============================================================================
static std::vector<int> g_hashSyms;      // symbol indices of global, mutable int/float/string variables
static bool g_hashSymsValid = false;

static void NnBuildHashList() {
    g_hashSyms.clear();
    g_hashSymsValid = true;
    if (!parser) return;
    const int n = parser->symtab.GetNumInList();
    for (int i = 1; i < n; ++i) {
        zCPar_Symbol* s = parser->GetSymbol(i);
        if (!s) continue;
        if (s->type != NN_SYM_FLOAT && s->type != NN_SYM_INT && s->type != NN_SYM_STRING) continue;
        if (s->flags & (NN_FLAG_CONST | NN_FLAG_CLASSVAR | NN_FLAG_EXTERNAL)) continue;
        const char* nm = s->name.ToChar();
        if (!nm || std::strchr(nm, '.') != nullptr) continue;   // skip function locals / class members
        g_hashSyms.push_back(i);
    }
}

static unsigned NnHashScriptState() {
    if (!g_hashSymsValid) NnBuildHashList();
    unsigned h = 2166136261u;
    auto mix = [&](unsigned v) { h = (h ^ v) * 16777619u; };

    for (int idx : g_hashSyms) {
        zCPar_Symbol* s = parser->GetSymbol(idx);
        if (!s) continue;
        if (s->type == NN_SYM_STRING) {
            if (!s->stringdata) continue;
            for (unsigned k = 0; k < s->ele; ++k) {
                const char* c = s->stringdata[k].ToChar();
                while (c && *c) mix(static_cast<unsigned char>(*c++));
                mix(0xFFu);
            }
        }
        else if (s->ele <= 1) {
            mix(static_cast<unsigned>(s->single_intdata));
        }
        else if (s->intdata) {
            for (unsigned k = 0; k < s->ele; ++k) mix(static_cast<unsigned>(s->intdata[k]));
        }
    }
    return h;
}

// ============================================================================
// Parser instance guard
// ============================================================================

// Temporarily points SELF/OTHER/HERO to the NPC being checked and restores every
// instance pointer the condition scripts could touch (incl. VICTIM and ITEM) afterwards.
struct NnParserGuard {
    static const int N = 6;
    static const char* Name(int i) {
        static const char* n[N] = { "SELF", "OTHER", "HERO", "PC_HERO", "VICTIM", "ITEM" };
        return n[i];
    }

    bool  saved[N] = {};
    void* old[N] = {};
    bool  applied = false;

    void Apply(oCNpc* npc, oCNpc* hero) {
        if (!parser || applied) return;
        applied = true;

        for (int i = 0; i < N; ++i) {
            zCPar_Symbol* s = parser->GetSymbol(zSTRING(Name(i)));
            if (s) { old[i] = s->GetInstanceAdr(); saved[i] = true; }
        }

        if (saved[0]) parser->SetInstance("SELF", npc);
        if (saved[1]) parser->SetInstance("OTHER", hero);
        if (saved[2]) parser->SetInstance("HERO", hero);
        if (saved[3]) parser->SetInstance("PC_HERO", hero);
    }

    ~NnParserGuard() {
        if (!applied || !parser) return;
        for (int i = 0; i < N; ++i)
            if (saved[i]) parser->SetInstance(Name(i), old[i]);
    }
};

// ============================================================================
// Dialogue index (npc instance -> its oCInfo entries)
// ============================================================================
static std::unordered_map<int, std::vector<oCInfo*>> g_infosByNpc;
static bool g_infoIndexValid = false;

static const std::vector<oCInfo*>& NnInfosFor(int npcInstance) {
    static const std::vector<oCInfo*> empty;
    if (!g_infoIndexValid) {
        g_infosByNpc.clear();
        if (ogame && ogame->GetInfoManager()) {
            int guard = 0;
            for (zCListSort<oCInfo>* l = &ogame->GetInfoManager()->infoList;
                l && guard++ < 200000; l = l->next) {
                if (oCInfo* i = l->GetData()) g_infosByNpc[i->pd.npc].push_back(i);
            }
        }
        g_infoIndexValid = true;
    }
    auto it = g_infosByNpc.find(npcInstance);
    return it != g_infosByNpc.end() ? it->second : empty;
}



// ============================================================================
// NPC classification
// ============================================================================
static std::vector<int> g_snapInstSyms;     // global instance-typed symbols (SELF, OTHER, VICTIM, ITEM, ...)
static bool g_snapInstValid = false;
struct NnSnapshot { unsigned globals = 0, instances = 0, objects = 0, world = 0; };

static std::string NnDescribe(oCInfo* info) {
    std::string s = info->name.ToChar();
    s += " | \"";
    s += info->pd.description.ToChar();
    s += "\"";
    if (info->pd.important) s += " [important]";
    return s;
}

#if MD_DEBUG





static unsigned NnHashBytes(const void* p, size_t n, unsigned h) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 16777619u;
    return h;
}

// Hash of the object currently bound to every global instance symbol.
static unsigned NnHashInstances() {
    if (!g_snapInstValid) {
        g_snapInstSyms.clear();
        g_snapInstValid = true;
        const int n = parser->symtab.GetNumInList();
        for (int i = 1; i < n; ++i) {
            zCPar_Symbol* s = parser->GetSymbol(i);
            if (!s || s->type != NN_SYM_INSTANCE) continue;
            const char* nm = s->name.ToChar();
            if (nm && std::strchr(nm, '.') == nullptr)   // skip function parameters/locals
                g_snapInstSyms.push_back(i);
        }
    }
    unsigned h = 2166136261u;
    for (int idx : g_snapInstSyms) {
        zCPar_Symbol* s = parser->GetSymbol(idx);
        const size_t adr = s ? reinterpret_cast<size_t>(s->GetInstanceAdr()) : 0;
        h = NnHashBytes(&adr, sizeof(adr), h);
    }
    return h;
}

static NnSnapshot NnTakeSnapshot(oCNpc* npc) {
    NnSnapshot s;
    s.globals = NnHashScriptState();
    s.instances = NnHashInstances();

    unsigned h = 2166136261u;
    if (npc)             h = NnHashBytes(npc, sizeof(oCNpc), h);
    if (oCNpc::player)   h = NnHashBytes(oCNpc::player, sizeof(oCNpc), h);
    s.objects = h;

    // If either member doesn't exist in your SDK headers, delete that line.
    int cnt = 0;
    if (ogame && ogame->GetGameWorld()) {
        oCWorld* w = ogame->GetGameWorld();
        if (w->voblist_npcs)  cnt += w->voblist_npcs->GetNumInList();
        if (w->voblist_items) cnt += 1000003 * w->voblist_items->GetNumInList();
    }
    s.world = static_cast<unsigned>(cnt);
    return s;
}

// Compares snapshots, updates per-external statistics and logs dirty runs with their suspects.
static void NnCompareSnapshot(oCInfo* info, oCNpc* npc, const NnSnapshot& b) {
    const NnSnapshot a = NnTakeSnapshot(npc);

    std::string what;
    if (a.globals != b.globals)   what += "globals ";
    if (a.instances != b.instances) what += "instances ";
    if (a.objects != b.objects)   what += "objects ";
    if (a.world != b.world)     what += "world ";
    const bool dirty = !what.empty();

    std::string all, suspects;
    auto it = g_condExternals.find(info->pd.conditions);
    if (it != g_condExternals.end()) {
        for (const std::string& n : it->second) {
            NnExtStat& st = g_extStat[n];
            if (dirty) ++st.dirty; else ++st.clean;
            all += n + " ";
        }
        if (dirty)
            for (const std::string& n : it->second)
                if (g_extStat[n].clean == 0) suspects += n + " ";
    }

    if (dirty)
        NnLog(std::string("[STATE CHANGED] ") + what + "| condition of " + info->name.ToChar() +
            (g_PurityGate ? " (passed purity gate!)" : " (gate disabled)") +
            "\n    externals: " + all + "\n    prime suspects (never seen clean): " + suspects);
}

// Per-external verdicts. Call on exit and before reloads.
static void NnDumpExternalStats() {
#if MD_SHADOWVM
    NnLog("[verify] ---- allowlist coverage ----");
    {
        char sb[160];
        sprintf_s(sb, "[verify] shadow VM vs real: agree=%d DISAGREE=%d not-evaluable=%d", g_shAgree, g_shDisagree, g_shSkipped);
        NnLog(sb);
    }
#endif
    for (const std::string& n : NnPureExternalSet()) {
        auto it = g_extStat.find(n);
        if (it == g_extStat.end()) { NnLog("[verify] UNTESTED  " + n); continue; }
        char b[160];
        sprintf_s(b, "[verify] %s  %s  clean=%d dirty=%d",
            it->second.dirty ? "SUSPECT " : "ok      ", n.c_str(), it->second.clean, it->second.dirty);
        NnLog(b);
    }
}
#endif // MD_DEBUG

static void NnResetScriptCaches() {
    g_infoIndexValid = false;
    g_infosByNpc.clear();
    g_purity.clear();
    g_hashSymsValid = false;
    g_hashSyms.clear();

    NnQuestIndexReset();

#if MD_DEBUG
    g_snapInstValid = false;
    g_snapInstSyms.clear();
    g_condExternals.clear();
    //Do not clear g_extStat in the reset. The evidence should accumulate across level changes
#endif
}

#if MD_DEBUG
// Hash of the oCInfo's own state. Info_AddChoice / Info_ClearChoices / SetTold would change these.
static unsigned NnHashInfo(oCInfo* info) {
    unsigned h = 2166136261u;
    auto mix = [&](unsigned v) { h = (h ^ v) * 16777619u; };
    mix(static_cast<unsigned>(info->told));
    mix(static_cast<unsigned>(info->instance));
    mix(static_cast<unsigned>(info->pd.npc));
    mix(static_cast<unsigned>(info->pd.important));
    mix(static_cast<unsigned>(info->pd.conditions));
    mix(static_cast<unsigned>(info->pd.information));
    mix(static_cast<unsigned>(info->pd.trade));
    mix(static_cast<unsigned>(info->pd.permanent));
    mix(static_cast<unsigned>(info->listChoices.GetNumInList()));
    return h;
}
#endif


#if MD_DEBUG
// Logs which 4-byte words of an object changed between a saved copy and the live object.
// Limited to 60 reports in total so a noisy condition cannot flood the log.
static void NnLogRawDiff(oCInfo* info, const char* who,
    const std::vector<unsigned char>& before, const void* now)
{
    static int budget = 60;
    if (budget <= 0 || before.empty() || !now) return;

    const unsigned char* cur = static_cast<const unsigned char*>(now);
    int changed = 0;
    for (size_t off = 0; off + 4 <= before.size(); off += 4) {
        unsigned a, b;
        std::memcpy(&a, &before[off], 4);
        std::memcpy(&b, cur + off, 4);
        if (a == b) continue;
        if (changed == 0)
            NnLog(std::string("[diff] ") + who + " changed by condition of " + info->name.ToChar());
        if (changed < 16) {
            char buf[96];
            sprintf_s(buf, "    +0x%04X  %08X -> %08X", static_cast<unsigned>(off), a, b);
            NnLog(buf);
        }
        ++changed;
    }
    if (changed > 16) NnLog("    ... and " + std::to_string(changed - 16) + " more words");
    if (changed) --budget;
}
#endif


//found this using Ghidra + claude
// The global game parser lives at 0x00AB40C0 in gothic.exe executable. Field +0x219C holds the
// symbol index of the function currently being executed, or -1 when none is running.
// zCParser::CallFunc clears the parser's operand stack on entry, so calling a condition
// while a script is mid-execution would corrupt that script.
// 
// Field +0x219C of the game parser holds the symbol index of the script function currently being
// executed, or -1 when none is. CallFunc wipes the parser's operand stack on entry, so conditions must
// never be evaluated while a script is mid-execution.
static bool NnParserBusy()
{
    if (!parser) return true;
    const int curFunc = *reinterpret_cast<const int*>(reinterpret_cast<const char*>(parser) + 0x219C);
    return curFunc > 0;     // -1 (idle) and 0 (never set) both count as idle
}

#if MD_SHADOWVM
#pragma region ShadowVM2
// ============================================================================
// Shadow VM: evaluates a condition itself. It never calls the engine VM and never writes game memory.
// Anything it does not fully understand makes the condition "not evaluable" -> false.
// ============================================================================

enum NnShExt {
    SH_KNOWSINFO = 1, SH_HASITEMS, SH_TALENT, SH_REFUSETALK, SH_ISDEAD, SH_ISINSTATE,
    SH_DISTNPC, SH_GETDAY, SH_ISTIME, SH_STRCMP, SH_INSTID, SH_VALIDNPC, SH_GETNPC
};

// Symbol +0x1C is the class-member offset (the census showed SetOffset writes it).
static int NnSymOffset(zCPar_Symbol* s) {
    return *reinterpret_cast<const int*>(reinterpret_cast<const char*>(s) + 0x1C);
}

static oCInfo* NnInfoByInstance(int inst) {
    if (!g_infoByInstValid) {
        g_infoByInst.clear();
        g_infoByInstValid = true;
        if (ogame && ogame->GetInfoManager()) {
            int guard = 0;
            for (zCListSort<oCInfo>* l = &ogame->GetInfoManager()->infoList; l && guard++ < 200000; l = l->next)
                if (oCInfo* i = l->GetData()) if (i->instance > 0) g_infoByInst[i->instance] = i;
        }
    }
    auto it = g_infoByInst.find(inst);
    return it != g_infoByInst.end() ? it->second : nullptr;
}

static oCNpc* NnFindNpcByInstance(int inst) {
    if (inst <= 0 || !ogame || !ogame->GetGameWorld()) return nullptr;
    int guard = 0;
    for (zCListSort<oCNpc>* l = ogame->GetGameWorld()->voblist_npcs; l && guard++ < 20000; l = l->next) {
        oCNpc* n = l->GetData();
        if (n && n->GetInstance() == inst) return n;
    }
    return nullptr;
}

static void NnShadowBuildTables() {
    g_shExt.clear();
    g_shTablesValid = true;
    if (!parser) return;
    struct E { const char* n; int c; };
    static const E tbl[] = {
        { "NPC_KNOWSINFO", SH_KNOWSINFO }, { "NPC_HASITEMS", SH_HASITEMS },
        { "NPC_GETTALENTSKILL", SH_TALENT }, { "NPC_REFUSETALK", SH_REFUSETALK },
        { "NPC_ISDEAD", SH_ISDEAD }, { "NPC_ISINSTATE", SH_ISINSTATE },
        { "NPC_GETDISTTONPC", SH_DISTNPC }, { "WLD_GETDAY", SH_GETDAY }, { "WLD_ISTIME", SH_ISTIME },
        { "HLP_STRCMP", SH_STRCMP }, { "HLP_GETINSTANCEID", SH_INSTID },
        { "HLP_ISVALIDNPC", SH_VALIDNPC }, { "HLP_GETNPC", SH_GETNPC }
    };
    for (const E& e : tbl) { const int i = parser->GetIndex(e.n); if (i > 0) g_shExt[i] = e.c; }
    auto ix = [](const char* n) { const int i = parser->GetIndex(n); return i > 0 ? i : -1; };
    g_shSelf = ix("SELF"); g_shOther = ix("OTHER"); g_shHero = ix("HERO"); g_shPcHero = ix("PC_HERO");
}

// Self-calibrates where the C_NPC script class starts inside oCNpc, using C_NPC.ATTRIBUTE
// (the SDK knows &npc->attribute[0], the symbol knows its offset). Works whether symbol offsets are absolute or class-relative.
static void NnShadowInit(oCNpc* sample) {
    if (!parser) return;
    if (!g_shTablesValid) NnShadowBuildTables();
    if (g_shBaseState != 0 || !sample) return;
    g_shBaseState = -1;
    const int ai = parser->GetIndex("C_NPC.ATTRIBUTE");
    zCPar_Symbol* as = (ai > 0) ? NnSym(ai) : nullptr;
    if (as && (as->flags & NN_FLAG_CLASSVAR)) {
        const int base = static_cast<int>(reinterpret_cast<const char*>(&sample->attribute[0]) -
            reinterpret_cast<const char*>(sample)) - NnSymOffset(as);
        if (base >= 0 && base < static_cast<int>(sizeof(oCNpc))) { g_shBase = base; g_shBaseState = 1; }
    }
    if (g_DebugMode >= 1) {
        char b[120];
        sprintf_s(b, "[shadow] C_NPC class base in oCNpc = 0x%X (%s)", static_cast<unsigned>(g_shBase), g_shBaseState == 1 ? "ok" : "FAILED, class members unsupported");
        NnLog(b);
    }
}

struct NnSlot { int val = 0; int sym = 0; int idx = 0; int inst = 0; bool isStr = false; std::string str; };

struct NnShadow {
    oCNpc* self = nullptr;
    oCNpc* hero = nullptr;
    std::vector<NnSlot> st;
    std::unordered_map<unsigned long long, int> ov;           // shadow writes (ints / instance refs)
    std::unordered_map<unsigned long long, std::string> ovs;  // shadow writes (strings)
    int curInst = 0;
    bool ok = true;
    std::string why;

    static unsigned long long Key(int inst, int sym, int idx) {
        return (static_cast<unsigned long long>(static_cast<unsigned>(inst)) << 40) |
            (static_cast<unsigned long long>(static_cast<unsigned>(sym)) << 8) |
            static_cast<unsigned long long>(idx & 0xFF);
    }
    void Fail(const std::string& w) { if (ok) { ok = false; why = w; } }
    void Push(const NnSlot& s) { if (st.size() >= 64) { Fail("stack overflow"); return; } st.push_back(s); }
    NnSlot Pop() {
        if (st.empty()) { Fail("stack underflow"); return NnSlot(); }
        NnSlot s = st.back(); st.pop_back(); return s;
    }
    void PushInt(int v) { NnSlot s; s.val = v; Push(s); }
    void PushRef(int sym, int idx) {
        NnSlot s; s.sym = sym; s.idx = idx;
        zCPar_Symbol* ps = NnSym(sym);
        if (ps && (ps->flags & NN_FLAG_CLASSVAR)) {
            if (curInst <= 0) { Fail("class member without instance"); return; }
            s.inst = curInst;
        }
        Push(s);
    }

    oCNpc* NpcOf(int instSym, bool required) {
        oCNpc* r = nullptr;
        if (instSym > 0) {
            if (instSym == g_shSelf) r = self;
            else if (instSym == g_shOther || instSym == g_shHero || instSym == g_shPcHero) r = hero;
            else if (self && self->GetInstance() == instSym) r = self;
            else if (hero && hero->GetInstance() == instSym) r = hero;
            else r = NnFindNpcByInstance(instSym);
        }
        if (!r && required) Fail("unresolved npc instance");
        return r;
    }
    oCNpc* NpcOfSlot(const NnSlot& s, bool required) {
        const int v = ReadInt(s);
        if (!ok) return nullptr;
        return NpcOf(v, required);
    }

    int ReadMember(zCPar_Symbol* y, int instSym, int idx) {
        const char* nm = y->name.ToChar();
        if (!nm || std::strncmp(nm, "C_NPC.", 6) != 0) { Fail("member of non-NPC class"); return 0; }
        if (g_shBaseState != 1) { Fail("class base unknown"); return 0; }
        if (idx < 0 || idx >= static_cast<int>(y->ele)) { Fail("member index"); return 0; }
        oCNpc* o = NpcOf(instSym, true);
        if (!o) return 0;
        const int off = g_shBase + NnSymOffset(y) + idx * 4;
        if (off < 0 || off + 4 > static_cast<int>(sizeof(oCNpc))) { Fail("member offset"); return 0; }
        return *reinterpret_cast<const int*>(reinterpret_cast<const char*>(o) + off);
    }

    int ReadInt(const NnSlot& s) {
        if (s.isStr) { Fail("string used as int"); return 0; }
        if (s.sym <= 0) return s.val;
        auto it = ov.find(Key(s.inst, s.sym, s.idx));
        if (it != ov.end()) return it->second;
        zCPar_Symbol* y = NnSym(s.sym);
        if (!y) { Fail("bad symbol"); return 0; }
        if (y->type == NN_SYM_INSTANCE || y->type == NN_SYM_FUNC) return s.sym;
        if (y->type != NN_SYM_INT) { Fail(std::string("type of ") + y->name.ToChar()); return 0; }
        if (y->flags & NN_FLAG_CLASSVAR) return ReadMember(y, s.inst, s.idx);
        if (y->ele <= 1) return y->single_intdata;
        if (!y->intdata || s.idx < 0 || s.idx >= static_cast<int>(y->ele)) { Fail("array index"); return 0; }
        return y->intdata[s.idx];
    }

    std::string ReadStr(const NnSlot& s) {
        if (s.isStr) return s.str;
        if (s.sym <= 0) { Fail("int used as string"); return std::string(); }
        auto it = ovs.find(Key(s.inst, s.sym, s.idx));
        if (it != ovs.end()) return it->second;
        zCPar_Symbol* y = NnSym(s.sym);
        if (!y || y->type != NN_SYM_STRING || !y->stringdata || s.idx < 0 || s.idx >= static_cast<int>(y->ele)) {
            Fail("string symbol"); return std::string();
        }
        const char* c = y->stringdata[s.idx].ToChar();
        return c ? std::string(c) : std::string();
    }

    void CallExt(int ext) {
        auto it = g_shExt.find(ext);
        if (it == g_shExt.end()) {
            zCPar_Symbol* f = NnSym(ext);
            Fail(std::string("external ") + (f ? f->name.ToChar() : "?"));
            return;
        }
        switch (it->second) {
        case SH_KNOWSINFO: {
            const int info = ReadInt(Pop());
            Pop();   // npc: Npc_KnowsInfo is evaluated on the global told flag
            if (!ok) return;
            oCInfo* i = NnInfoByInstance(info);
            PushInt((i && i->WasTold()) ? 1 : 0);
            break;
        }
        case SH_HASITEMS: {
            const int item = ReadInt(Pop());
            oCNpc* n = NpcOfSlot(Pop(), true);
            if (!ok || !n) return;
            PushInt(n->inventory2.GetAmount(item));   // if this does not compile: PushInt(n->IsInInv(item, 1) ? 1 : 0);
            break;
        }
        case SH_TALENT: {
            const int t = ReadInt(Pop());
            oCNpc* n = NpcOfSlot(Pop(), true);
            if (!ok || !n) return;
            PushInt(n->GetTalentSkill(t));
            break;
        }
        case SH_REFUSETALK: {
            oCNpc* n = NpcOfSlot(Pop(), true);
            if (!ok || !n) return;
            if (sizeof(oCNpc) < 0x944) { Fail("oCNpc too small"); return; }
            const float timer = *reinterpret_cast<const float*>(reinterpret_cast<const char*>(n) + 0x940);
            PushInt(timer > 0.0f ? 1 : 0);
            break;
        }
        case SH_ISDEAD: {
            oCNpc* n = NpcOfSlot(Pop(), true);
            if (!ok || !n) return;
            PushInt(n->IsDead() ? 1 : 0);
            break;
        }
        case SH_ISINSTATE: {
            const int fn = ReadInt(Pop());
            oCNpc* n = NpcOfSlot(Pop(), true);
            if (!ok || !n) return;
            PushInt(n->state.IsInState(fn) ? 1 : 0);
            break;
        }
        case SH_DISTNPC: {
            oCNpc* b = NpcOfSlot(Pop(), true);
            oCNpc* a = NpcOfSlot(Pop(), true);
            if (!ok || !a || !b) return;
            PushInt(static_cast<int>(a->GetDistanceToVob(*b)));
            break;
        }
        case SH_GETDAY: {
            if (!ogame || !ogame->GetWorldTimer()) { Fail("no world timer"); return; }
            PushInt(ogame->GetWorldTimer()->GetDay());
            break;
        }
        case SH_ISTIME: {
            const int m2 = ReadInt(Pop()); const int h2 = ReadInt(Pop());
            const int m1 = ReadInt(Pop()); const int h1 = ReadInt(Pop());
            if (!ok) return;
            if (!ogame || !ogame->GetWorldTimer()) { Fail("no world timer"); return; }
            PushInt(ogame->GetWorldTimer()->IsTimeBetween(h1, m1, h2, m2) ? 1 : 0);
            break;
        }
        case SH_STRCMP: {
            const std::string b = ReadStr(Pop());
            const std::string a = ReadStr(Pop());
            if (!ok) return;
            PushInt(_stricmp(a.c_str(), b.c_str()) == 0 ? 1 : 0);
            break;
        }
        case SH_INSTID: {
            const int inst = ReadInt(Pop());
            if (!ok) return;
            oCNpc* n = NpcOf(inst, false);
            if (!n) { Fail("instance id of unresolved object"); return; }
            PushInt(n->GetInstance());
            break;
        }
        case SH_VALIDNPC: {
            const int inst = ReadInt(Pop());
            if (!ok) return;
            PushInt(NpcOf(inst, false) ? 1 : 0);
            break;
        }
        case SH_GETNPC: {
            const int inst = ReadInt(Pop());
            if (!ok) return;
            NnSlot r; r.val = inst; Push(r);
            break;
        }
        default:
            Fail("external not implemented");
            break;
        }
    }

    bool Run(int startPos) {
        if (!parser || !parser->stack.stack) { Fail("no parser"); return false; }
        const zBYTE* code = parser->stack.stack;
        const int maxBytes = parser->stack.stacksize;
        if (startPos < 0 || startPos >= maxBytes) { Fail("bad start"); return false; }

        int retStack[8]; int depth = 0, pos = startPos, steps = 0;
        bool finished = false;

        while (ok && pos >= 0 && pos < maxBytes && steps++ < 4096) {
            const int tok = code[pos++];
            const int os = NnOperandSize(tok);
            if (pos + os > maxBytes) { Fail("truncated bytecode"); break; }
            const int operand = (os >= 4) ? *reinterpret_cast<const int*>(code + pos) : 0;
            const int arr = (os == 5) ? code[pos + 4] : 0;
            pos += os;

            if (tok == zPAR_TOK_RET) {
                if (depth > 0) { pos = retStack[--depth]; continue; }
                finished = true;
                break;
            }
            else if (tok == zPAR_TOK_PUSHINT) { PushInt(operand); }
            else if (tok == zPAR_TOK_PUSHINST) { PushRef(operand, 0); }
            else if (tok == zPAR_TOK_PUSHVAR || tok == (zPAR_TOK_PUSHVAR | zPAR_TOK_FLAGARRAY)) { PushRef(operand, arr); }
            else if (tok == zPAR_TOK_SETINSTANCE) {
                auto it = ov.find(Key(0, operand, 0));
                curInst = (it != ov.end()) ? it->second : operand;
            }
            else if (tok == zPAR_TOK_JUMP) {
                if (operand < 0 || operand >= maxBytes) { Fail("jump"); break; }
                pos = operand;
            }
            else if (tok == zPAR_TOK_JUMPF) {
                const int v = ReadInt(Pop());
                if (!ok) break;
                if (v == 0) {
                    if (operand < 0 || operand >= maxBytes) { Fail("jump"); break; }
                    pos = operand;
                }
            }
            else if (tok == zPAR_TOK_CALL) {
                if (operand < 0 || operand >= maxBytes || depth >= 8) { Fail("call"); break; }
                retStack[depth++] = pos;
                pos = operand;
            }
            else if (tok == zPAR_TOK_CALLEXTERN) { CallExt(operand); }
            else if (tok == zPAR_OP_IS || tok == zPAR_OP_ISPLUS || tok == zPAR_OP_ISMINUS ||
                tok == zPAR_OP_ISMUL || tok == zPAR_OP_ISDIV) {
                const NnSlot lhs = Pop();
                const int rhs = ReadInt(Pop());
                if (!ok) break;
                if (lhs.sym <= 0) { Fail("assign target"); break; }
                int nv = rhs;
                if (tok != zPAR_OP_IS) {
                    const int cur = ReadInt(lhs);
                    if (!ok) break;
                    if (tok == zPAR_OP_ISPLUS) nv = cur + rhs;
                    else if (tok == zPAR_OP_ISMINUS) nv = cur - rhs;
                    else if (tok == zPAR_OP_ISMUL) nv = cur * rhs;
                    else nv = rhs ? cur / rhs : 0;
                }
                ov[Key(lhs.inst, lhs.sym, lhs.idx)] = nv;
            }
            else if (tok == zPAR_TOK_ASSIGNSTR || tok == zPAR_TOK_ASSIGNSTRP) {
                const NnSlot lhs = Pop();
                const NnSlot rhs = Pop();
                const std::string v = ReadStr(rhs);
                if (!ok) break;
                if (lhs.sym <= 0) { Fail("assign target"); break; }
                ovs[Key(lhs.inst, lhs.sym, lhs.idx)] = v;
            }
            else if (tok == zPAR_TOK_ASSIGNFUNC || tok == zPAR_TOK_ASSIGNINST) {
                const NnSlot lhs = Pop();
                const NnSlot rhs = Pop();
                const int v = ReadInt(rhs);
                if (!ok) break;
                if (lhs.sym <= 0) { Fail("assign target"); break; }
                ov[Key(lhs.inst, lhs.sym, lhs.idx)] = v;
            }
            else if (tok == zPAR_TOK_ASSIGNFLOAT) { Fail("float assignment"); break; }
            else if (tok == zPAR_OP_UN_MINUS) { const int v = ReadInt(Pop()); if (ok) PushInt(-v); }
            else if (tok == zPAR_OP_UN_NOT) { const int v = ReadInt(Pop()); if (ok) PushInt(v == 0 ? 1 : 0); }
            else if (tok == zPAR_OP_UN_NEG) { const int v = ReadInt(Pop()); if (ok) PushInt(~v); }
            else if (tok <= zPAR_OP_ISDIV) {
                const int rhs = ReadInt(Pop());
                const int lhs = ReadInt(Pop());
                if (!ok) break;
                switch (tok) {
                case zPAR_OP_PLUS:      PushInt(lhs + rhs); break;
                case zPAR_OP_MINUS:     PushInt(lhs - rhs); break;
                case zPAR_OP_MUL:       PushInt(lhs * rhs); break;
                case zPAR_OP_DIV:       PushInt(rhs != 0 ? lhs / rhs : 0); break;
                case zPAR_OP_MOD:       PushInt(rhs != 0 ? lhs % rhs : 0); break;
                case zPAR_OP_OR:        PushInt(lhs | rhs); break;
                case zPAR_OP_AND:       PushInt(lhs & rhs); break;
                case zPAR_OP_LOWER:     PushInt(lhs < rhs ? 1 : 0); break;
                case zPAR_OP_HIGHER:    PushInt(lhs > rhs ? 1 : 0); break;
                case zPAR_OP_LOG_OR:    PushInt((lhs != 0 || rhs != 0) ? 1 : 0); break;
                case zPAR_OP_LOG_AND:   PushInt((lhs != 0 && rhs != 0) ? 1 : 0); break;
                case zPAR_OP_SHIFTL:    PushInt((rhs >= 0 && rhs < 32) ? (lhs << rhs) : 0); break;
                case zPAR_OP_SHIFTR:    PushInt((rhs >= 0 && rhs < 32) ? (lhs >> rhs) : 0); break;
                case zPAR_OP_LOWER_EQ:  PushInt(lhs <= rhs ? 1 : 0); break;
                case zPAR_OP_EQUAL:     PushInt(lhs == rhs ? 1 : 0); break;
                case zPAR_OP_NOTEQUAL:  PushInt(lhs != rhs ? 1 : 0); break;
                case zPAR_OP_HIGHER_EQ: PushInt(lhs >= rhs ? 1 : 0); break;
                default: Fail("operator"); break;
                }
            }
            else { Fail("opcode " + std::to_string(tok)); break; }
        }
        if (ok && !finished) Fail("no RET / step limit");
        return ok;
    }
};

// Returns the condition result. evaluable=false means "do not trust the return value" (callers treat it as false).
static bool NnShadowEval(oCInfo* info, oCNpc* npc, bool& evaluable, std::string* why) {
    evaluable = false;
    oCNpc* hero = oCNpc::player;
    if (!info || !npc || !hero || !parser) return false;
    zCPar_Symbol* s = NnSym(info->pd.conditions);
    if (!s || s->type != NN_SYM_FUNC || (s->flags & NN_FLAG_EXTERNAL)) return false;

    NnShadowInit(npc);
    NnShadow vm;
    vm.self = npc;
    vm.hero = hero;
    vm.Run(s->single_intdata);
    if (!vm.ok) { if (why) *why = vm.why; return false; }

    bool result = false;                      // empty stack = fell off the false path = 0
    if (!vm.st.empty()) {
        const NnSlot top = vm.st.back();
        result = vm.ReadInt(top) != 0;
        if (!vm.ok) { if (why) *why = vm.why; return false; }
    }
    evaluable = true;
    return result;
}

static void NnShadowNote(oCInfo* info, bool evaluable, const std::string& why, bool r) {
    if (g_DebugMode < 1) return;
    if (!g_shLogged.insert(info->pd.conditions).second) return;   // once per condition function
    std::string msg = evaluable ? "[shadow-eval] " : "[shadow-skip] ";
    msg += info->name.ToChar();
    msg += " : ";
    if (evaluable) msg += r ? "true" : "false";
    else msg += why;
    NnLog(msg);
}

#if MD_DEBUG
// For conditions that passed the gate (so the REAL result exists): compare the shadow VM against the engine.
static void NnShadowCrossCheck(oCInfo* info, oCNpc* npc, bool real) {
    bool ev = false; std::string why;
    const bool sh = NnShadowEval(info, npc, ev, &why);
    if (!ev) { ++g_shSkipped; return; }
    if (sh == real) { ++g_shAgree; return; }
    ++g_shDisagree;
    static int budget = 80;
    if (budget-- > 0)
        NnLog(std::string("[shadow-MISMATCH] ") + info->name.ToChar() + " real=" + (real ? "1" : "0") + " shadow=" + (sh ? "1" : "0"));
}
#endif
#pragma endregion ShadowVM2
#endif


// Runs a dialogue's condition. Returns true if the dialogue is currently available.
// Only executes script code if the condition has been proven read-only (or the gate is disabled).
static bool NnEvalCondition(oCInfo* info, oCNpc* npc) {
    if (info->pd.conditions <= 0) return false;   // engine: CallFunc fails, result stays 0 (false)
    if (NnParserBusy()) {                         // never run the VM re-entrantly
        static bool warned = false;
        if (!warned && g_DebugMode >= 1) { warned = true; NnLog("[busy] parser reports a running script; condition skipped"); }
        return false;
    }

    if (g_PurityGate && !NnConditionIsPure(info)) {
#if MD_SHADOWVM
        if (!g_ShadowVm) return false;
        bool evaluable = false; std::string why;
        const bool r = NnShadowEval(info, npc, evaluable, &why);   // never calls the engine VM
        NnShadowNote(info, evaluable, why, r);
        return evaluable && r;
#else
        return false;
#endif
    }

#if MD_DEBUG
    // Full snapshot: global variables, instance bindings, NPC/hero bytes, world counts, the oCInfo itself.
    NnSnapshot before;
    unsigned infoBefore = 0;
    if (g_DebugMode >= 3) { before = NnTakeSnapshot(npc); infoBefore = NnHashInfo(info); }
#else
    // Release fallback: hash of global script variables only.
    unsigned before = 0;
    if (g_DebugMode >= 3) before = NnHashScriptState();
#endif


#if MD_DEBUG
    std::vector<unsigned char> rawNpcBefore, rawHeroBefore;
    if (g_DebugMode >= 3) {
        oCNpc* heroNow = oCNpc::player;
        if (npc)     rawNpcBefore.assign(reinterpret_cast<const unsigned char*>(npc),
            reinterpret_cast<const unsigned char*>(npc) + sizeof(oCNpc));
        if (heroNow) rawHeroBefore.assign(reinterpret_cast<const unsigned char*>(heroNow),
            reinterpret_cast<const unsigned char*>(heroNow) + sizeof(oCNpc));
    }
#endif

    // InfoConditions() is where the magic happens and also the possibility of a state corruption,
    // which is why most of the implementation is about guarding against calling it.
    const bool result = info->InfoConditions() != 0;

#if MD_DEBUG
    if (g_DebugMode >= 3) {
        NnLogRawDiff(info, "npc", rawNpcBefore, npc);
        NnLogRawDiff(info, "hero", rawHeroBefore, oCNpc::player);
    }
#endif

#if MD_DEBUG
    if (g_DebugMode >= 3) {
        NnCompareSnapshot(info, npc, before);
        if (NnHashInfo(info) != infoBefore)
            NnLog(std::string("[STATE CHANGED] oCInfo itself | condition of ") + info->name.ToChar() +
                (g_PurityGate ? " (passed purity gate!)" : " (gate disabled)"));
    }
#else
    if (g_DebugMode >= 3 && NnHashScriptState() != before)
        NnLog(std::string("[STATE CHANGED] condition of ") + info->name.ToChar() +
            (g_PurityGate ? " (passed purity gate!)" : " (gate disabled)"));
#endif

#if MD_DEBUG && MD_SHADOWVM
    if (g_DebugMode >= 1 && g_ShadowVm) NnShadowCrossCheck(info, npc, result);
#endif
    return result;
}


#pragma region Static quest-start classifier (bytecode scan, never runs the VM, only reads parser data
// ============================================================================
// Static quest-start classifier + fixpoint (bytecode scan, never runs the VM)
// ============================================================================
struct NnPush { int kind; int v; };   // kind 1 = PUSHINT literal, 2 = symbol push
struct NnScanOut {
    bool found = false;
    std::string how;
    std::vector<int> writes, reads, knows;
    std::unordered_set<int> visited;   // bytecode positions; also caps the number of functions scanned
    size_t cap = 64;                   // budget for the current subtree
};

static void NnResolveConstInt(const char* name, int& sym, int& val, bool& ok) {
    sym = 0; val = 0; ok = false;
    const int i = parser->GetIndex(name);
    zCPar_Symbol* s = (i > 0) ? NnSym(i) : nullptr;
    if (s && s->type == NN_SYM_INT) { sym = i; val = s->single_intdata; ok = true; }
}

//static bool NnPushIs(const NnPush& p, int sym, int val, bool has) {
//    if (!has) return false;
//    if (p.kind == 2) return p.v == sym;
//    return p.v == val;
//}

static bool NnIsGlobalIntVar(int idx) {
    zCPar_Symbol* s = NnSym(idx);
    if (!s || s->type != NN_SYM_INT) return false;
    if (s->flags & (NN_FLAG_CONST | NN_FLAG_CLASSVAR | NN_FLAG_EXTERNAL)) return false;
    const char* nm = s->name.ToChar();
    return nm && std::strchr(nm, '.') == nullptr;
}

static const NnScanEnv& NnScanEnvGet() {
    if (!g_qEnvValid && parser) {
        g_qEnv = NnScanEnv();
        g_qEnv.createTopic = parser->GetIndex("LOG_CREATETOPIC");
        g_qEnv.addEntry = parser->GetIndex("LOG_ADDENTRY");
        g_qEnv.setStatus = parser->GetIndex("LOG_SETTOPICSTATUS");
        g_qEnv.addChoice = parser->GetIndex("INFO_ADDCHOICE");
        g_qEnv.knowsInfo = parser->GetIndex("NPC_KNOWSINFO");
        NnResolveConstInt("LOG_RUNNING", g_qEnv.runningSym, g_qEnv.runningVal, g_qEnv.hasRunning);
        NnResolveConstInt("LOG_NOTE", g_qEnv.noteSym, g_qEnv.noteVal, g_qEnv.hasNote);
        g_qEnvValid = true;
    }
    return g_qEnv;
}

// body=true : looks for quest starts, collects global writes, follows Info_AddChoice handlers.
// body=false: collects global reads and Npc_KnowsInfo targets (used on condition functions).
static void NnScanFn(int pos, int depth, const NnScanEnv& e, NnScanOut& o, bool body) {
    if (!parser || !parser->stack.stack) return;
    const zBYTE* code = parser->stack.stack;
    const int maxBytes = parser->stack.stacksize;
    if (pos < 0 || pos >= maxBytes || depth <= 0) return;
    if (o.visited.size() >= o.cap || o.visited.size() >= 300) return;
    if (!o.visited.insert(pos).second) return;

    auto hit = [&](const std::string& w) { if (!o.found) { o.found = true; o.how = w; } };

    std::vector<NnPush> run;
    std::vector<int> cbs;     // Info_AddChoice callbacks: scanned first
    std::vector<int> calls;   // plain script calls: scanned afterwards
    int cur = pos, maxJump = 0, steps = 0;
    while (cur < maxBytes && steps++ < 4096) {
        const int tok = code[cur++];
        const int os = NnOperandSize(tok);
        if (cur + os > maxBytes) break;
        const int operand = (os >= 4) ? *reinterpret_cast<const int*>(code + cur) : 0;
        cur += os;

        const bool isVarPush = (tok == zPAR_TOK_PUSHVAR || tok == (zPAR_TOK_PUSHVAR | zPAR_TOK_FLAGARRAY));
        if (tok == zPAR_TOK_PUSHINT || isVarPush || tok == zPAR_TOK_PUSHINST) {
            if (!body && isVarPush && NnIsGlobalIntVar(operand)) o.reads.push_back(operand);
            if (run.size() >= 8) run.erase(run.begin());
            NnPush p = { tok == zPAR_TOK_PUSHINT ? 1 : 2, operand };
            run.push_back(p);
            continue;
        }

        if (tok == zPAR_TOK_RET) {
            if (cur > maxJump) break;
        }
        else if (tok == zPAR_TOK_JUMP || tok == zPAR_TOK_JUMPF) {
            if (operand > maxJump && operand < maxBytes) maxJump = operand;
        }
        else if (tok == zPAR_TOK_CALL) {
            if (calls.size() < 32) calls.push_back(operand);
        }
        else if (tok == zPAR_TOK_CALLEXTERN) {
            if (body) {
                if (e.createTopic > 0 && operand == e.createTopic)      hit("Log_CreateTopic");
                else if (e.addEntry > 0 && operand == e.addEntry)       hit("Log_AddEntry");
                else if (e.setStatus > 0 && operand == e.setStatus)     hit("Log_SetTopicStatus");
                else if (e.addChoice > 0 && operand == e.addChoice) {
                    for (size_t i = 0; i < run.size(); ++i) {
                        zCPar_Symbol* fs = NnSym(run[i].v);
                        if (fs && fs->type == NN_SYM_FUNC && !(fs->flags & NN_FLAG_EXTERNAL))
                            cbs.push_back(fs->single_intdata);
                    }
                }
            }
            else if (e.knowsInfo > 0 && operand == e.knowsInfo && !run.empty()) {
                zCPar_Symbol* is = NnSym(run.back().v);
                if (is && is->type == NN_SYM_INSTANCE) o.knows.push_back(run.back().v);
            }
        }
        else if (body && NnIsAssignOp(tok)) {
            if (!run.empty() && run.back().kind == 2) {
                const int lv = run.back().v;
                if (tok == zPAR_OP_IS) {
                    zCPar_Symbol* ls = NnSym(lv);
                    const char* nm = ls ? ls->name.ToChar() : nullptr;
                    if (nm && std::strncmp(nm, "MIS_", 4) == 0 && !std::strchr(nm, '.'))
                        hit(std::string(nm) + " = ...");
                }
                if (NnIsGlobalIntVar(lv)) o.writes.push_back(lv);
            }
        }
        run.clear();
    }

    // each child gets its own budget so one fat callee (e.g. an Ikarus-based AI_Output)
    // cannot starve the others
    auto descend = [&](int p) {
        const size_t savedCap = o.cap;
        o.cap = o.visited.size() + 48;
        NnScanFn(p, depth - 1, e, o, body);
        o.cap = savedCap;
        };
    for (size_t i = 0; i < cbs.size(); ++i)   descend(cbs[i]);
    for (size_t i = 0; i < calls.size(); ++i) descend(calls[i]);
}


#if MD_DEBUG
// ---- TEMP DIAG ----
static void NnDbgDisasm(const std::string& label, int pos, int maxTok) {
    if (!parser || !parser->stack.stack) return;
    const zBYTE* code = parser->stack.stack;
    const int maxBytes = parser->stack.stacksize;
    int cur = pos, maxJump = 0;
    for (int n = 0; n < maxTok && cur >= 0 && cur < maxBytes; ++n) {
        const int at = cur;
        const int tok = code[cur++];
        const int osz = NnOperandSize(tok);
        if (cur + osz > maxBytes) break;
        const int operand = (osz >= 4) ? *reinterpret_cast<const int*>(code + cur) : 0;
        cur += osz;

        zCPar_Symbol* s = nullptr;
        if (tok == zPAR_TOK_PUSHVAR || tok == (zPAR_TOK_PUSHVAR | zPAR_TOK_FLAGARRAY) ||
            tok == zPAR_TOK_PUSHINST || tok == zPAR_TOK_CALLEXTERN)
            s = NnSym(operand);

        char line[220];
        sprintf_s(line, "[dis] %s +%04d tok=0x%02X operand=%d symtype=%d  %s",
            label.c_str(), at - pos, tok, operand, s ? s->type : -1, s ? s->name.ToChar() : "");
        NnLog(line);

        if ((tok == zPAR_TOK_JUMP || tok == zPAR_TOK_JUMPF) && operand > maxJump && operand < maxBytes) maxJump = operand;
        if (tok == zPAR_TOK_RET && cur > maxJump) break;
    }
}
#endif
static std::vector<int> NnUniq(std::vector<int> v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

static void NnScanInfo(oCInfo* info, NnInfoRec& r, bool bodyOnly) {
    r.info = info;
    const NnScanEnv& e = NnScanEnvGet();
    zCPar_Symbol* bs = NnSym(info->pd.information);
    if (bs && bs->type == NN_SYM_FUNC && !(bs->flags & NN_FLAG_EXTERNAL)) {
        NnScanOut o;
        NnScanFn(bs->single_intdata, 6, e, o, true);
        r.direct = o.found;
        r.how = o.how;
        r.writes = NnUniq(o.writes);
    }
    if (bodyOnly) return;
    zCPar_Symbol* cs = NnSym(info->pd.conditions);
    if (cs && cs->type == NN_SYM_FUNC && !(cs->flags & NN_FLAG_EXTERNAL)) {
        NnScanOut o;
        NnScanFn(cs->single_intdata, 6, e, o, false);
        r.reads = NnUniq(o.reads);
        r.knows = NnUniq(o.knows);
    }
}

// Info I is quest-ish if (a) its body writes a global that the condition of a quest-ish info J reads, or
// (b) J's condition tests Npc_KnowsInfo(I). Each iteration extends the chain by one hop.
// Variables with a large fan-out (Kapitel-style globals) are ignored as links.
static const int NN_VAR_FANOUT = 12;

static void NnQuestFixpoint() {
    const int n = static_cast<int>(g_qRecs.size());
    std::unordered_map<int, std::vector<int>> writers;
    std::unordered_map<int, int> readers, byInst;
    for (int i = 0; i < n; ++i) {
        const NnInfoRec& r = g_qRecs[i];
        if (r.info && r.info->instance > 0) byInst[r.info->instance] = i;
        for (int v : r.writes) writers[v].push_back(i);
        for (int v : r.reads) ++readers[v];
    }

    std::vector<int> frontier;
    for (int i = 0; i < n; ++i) {
        NnInfoRec& r = g_qRecs[i];
        r.quest = r.direct;
        r.level = r.direct ? 1 : 0;
        if (r.direct) frontier.push_back(i);
    }

    for (int hop = 0; hop < g_ChainDepth && !frontier.empty(); ++hop) {
        std::vector<int> next;
        for (size_t f = 0; f < frontier.size(); ++f) {
            const int j = frontier[f];
            const std::vector<int>& reads = g_qRecs[j].reads;
            const std::vector<int>& knows = g_qRecs[j].knows;
            const char* jn = g_qRecs[j].info->name.ToChar();

            for (int v : reads) {
                if (readers[v] > NN_VAR_FANOUT) continue;
                auto w = writers.find(v);
                if (w == writers.end() || static_cast<int>(w->second.size()) > NN_VAR_FANOUT) continue;
                zCPar_Symbol* vs = NnSym(v);
                for (int i : w->second) {
                    NnInfoRec& t = g_qRecs[i];
                    if (t.quest) continue;
                    t.quest = true; t.level = hop + 2; next.push_back(i);
                    t.how = std::string("sets ") + (vs ? vs->name.ToChar() : "?") + ", read by " + jn;
                }
            }
            for (int k : knows) {
                auto b = byInst.find(k);
                if (b == byInst.end()) continue;
                NnInfoRec& t = g_qRecs[b->second];
                if (t.quest) continue;
                t.quest = true; t.level = hop + 2; next.push_back(b->second);
                t.how = std::string("told before ") + jn;
            }
        }
        frontier.swap(next);
    }
}

// Incremental build: scans NN_QUEST_SCAN_PER_FRAME infos per call, then runs the fixpoint once.
// fills in g_qRecs; one NnInfoRec per oCInfo and then run the chain-depth fixpoint 
static void NnQuestIndexStep(int budget) {
#if MD_SHADOWVM
    if (g_qDirty) { NnQuestIndexReset(); NnClearCache(); }
#endif

    if (g_qBuilt || !parser || !ogame || !ogame->GetInfoManager()) return;

    if (!g_qStarted) {
        g_qRecs.clear(); g_qIndexOf.clear(); g_qPos = 0;
        int guard = 0;
        for (zCListSort<oCInfo>* l = &ogame->GetInfoManager()->infoList; l && guard++ < 200000; l = l->next) {
            oCInfo* i = l->GetData();
            if (!i) continue;
            NnInfoRec r; r.info = i;
            g_qIndexOf[i] = static_cast<int>(g_qRecs.size());
            g_qRecs.push_back(r);
        }
        if (g_qRecs.empty()) return;      // scripts not ready yet, retry next frame
        g_qStarted = true;
    }

    while (g_qPos < g_qRecs.size() && budget-- > 0) {
        NnScanInfo(g_qRecs[g_qPos].info, g_qRecs[g_qPos], false);
        ++g_qPos;
    }
    if (g_qPos < g_qRecs.size()) return;

    NnQuestFixpoint();
    g_qBuilt = true;
    NnClearCache();                        // colours must be recomputed with the final verdicts

    if (g_DebugMode >= 1) {
        int c[8] = {};
        for (size_t i = 0; i < g_qRecs.size(); ++i) {
            const NnInfoRec& r = g_qRecs[i];
            ++c[r.quest ? (r.level < 7 ? r.level : 7) : 0];
        }
        char b[200];
        sprintf_s(b, "[quest] index built: infos=%d direct=%d L2=%d L3=%d L4+=%d chainDepth=%d",
            static_cast<int>(g_qRecs.size()), c[1], c[2], c[3], c[4] + c[5] + c[6] + c[7], g_ChainDepth);
        NnLog(b);

        // ---- TEMP DIAG ----
        {
            const int ai = parser->GetIndex("INFO_ADDCHOICE");
            zCPar_Symbol* as = (ai > 0) ? NnSym(ai) : nullptr;
            char d[220];
            sprintf_s(d, "[diag] INFO_ADDCHOICE idx=%d envIdx=%d type=%d flags=0x%X external=%d",
                ai, NnScanEnvGet().addChoice, as ? as->type : -1,
                as ? static_cast<unsigned>(as->flags) : 0u,
                (as && (as->flags & NN_FLAG_EXTERNAL)) ? 1 : 0);
            NnLog(d);
#if MD_DEBUG
            for (size_t i = 0; i < g_qRecs.size(); ++i) {
                const NnInfoRec& r = g_qRecs[i];
                if (!r.info) continue;
                std::string nm = r.info->name.ToChar();
                for (size_t k = 0; k < nm.size(); ++k) nm[k] = static_cast<char>(toupper(static_cast<unsigned char>(nm[k])));
                if (nm.find("LEXX_SQ207") == std::string::npos) continue;

                std::string wl, rl, kl;
                for (int v : r.writes) { zCPar_Symbol* s = NnSym(v); wl += s ? s->name.ToChar() : "?"; wl += ' '; }
                for (int v : r.reads) { zCPar_Symbol* s = NnSym(v); rl += s ? s->name.ToChar() : "?"; rl += ' '; }
                for (int v : r.knows) { zCPar_Symbol* s = NnSym(v); kl += s ? s->name.ToChar() : "?"; kl += ' '; }

                NnLog("[diag] " + nm + " direct=" + std::to_string((int)r.direct) +
                    " quest=" + std::to_string((int)r.quest) + " level=" + std::to_string(r.level) +
                    " writes=" + std::to_string(r.writes.size()) + " [" + wl + "]" +
                    " reads=[" + rl + "] knows=[" + kl + "] how=" + r.how);

                zCPar_Symbol* bs = NnSym(r.info->pd.information);
                if (bs && bs->type == NN_SYM_FUNC && !(bs->flags & NN_FLAG_EXTERNAL))
                    NnDbgDisasm(nm, bs->single_intdata, 150);

            }
#endif
        }
    }
}
static void NnQuestIndexFinish() { NnQuestIndexStep(1 << 30); }

struct NnQuestVerdict { bool yes = false; int level = 0; std::string how; };

// Unsure / truncated scan = "not quest-ish". Until the index is complete only the direct (body) verdict is used.
static NnQuestVerdict NnQuestVerdictFor(oCInfo* info) {
    NnQuestVerdict v;
    if (!info) return v;
    auto it = g_qIndexOf.find(info);
    if (it != g_qIndexOf.end()) {
        const NnInfoRec& r = g_qRecs[it->second];
        if (g_qBuilt) { v.yes = r.quest; v.level = r.level; v.how = r.how; return v; }
        if (static_cast<size_t>(it->second) < g_qPos) { v.yes = r.direct; v.level = r.direct ? 1 : 0; v.how = r.how; return v; }
    }
    NnInfoRec tmp;
    NnScanInfo(info, tmp, true);
    v.yes = tmp.direct; v.level = tmp.direct ? 1 : 0; v.how = tmp.how;
    return v;
}

static bool NnInfoStartsQuest(oCInfo* info) { return NnQuestVerdictFor(info).yes; }






static int NnClassifyNpc(oCNpc* npc, bool includeImportant, std::string* why) {
    oCNpc* hero = oCNpc::player;
    if (!npc || !hero || !ogame) return 0;

    const int npcInstance = npc->GetInstance();
    int flags = 0;

    struct Cand { oCInfo* info; bool important; };
    std::vector<Cand> candidates;

    const std::vector<oCInfo*>& infos = NnInfosFor(npcInstance);
    for (size_t k = 0; k < infos.size(); ++k) {
        oCInfo* info = infos[k];
        if (!info) continue;

        if (info->pd.trade) { flags |= NN_TRADER; continue; }
        if (info->pd.permanent) continue;
        if (info->WasTold()) continue;

        const bool imp = info->pd.important != 0;
        if (imp && !includeImportant) continue;
        if (!imp && info->pd.description.IsEmpty()) continue;

        Cand c = { info, imp };
        candidates.push_back(c);
    }

    if (candidates.empty()) return flags;

    // No script execution at all: any untold dialogue counts.
    if (!g_RunConditions) {
        for (size_t i = 0; i < candidates.size(); ++i)
            if (candidates[i].important) {
                if (why) *why = NnDescribe(candidates[i].info);
                return flags | NN_IMPORTANT;
            }
        for (size_t i = 0; i < candidates.size(); ++i)
            if (NnInfoStartsQuest(candidates[i].info)) {
                if (why) *why = NnDescribe(candidates[i].info) + " {quest-start}";
                return flags | NN_QUEST;
            }
        if (why) *why = NnDescribe(candidates[0].info);
        return flags | NN_TALK;
    }

    NnParserGuard guard;

    if (NnParserBusy()) return flags;

    guard.Apply(npc, hero);

    bool questFound = false, talkFound = false, importantFound = false;

    for (size_t i = 0; i < candidates.size(); ++i) {
        oCInfo* info = candidates[i].info;
        const bool imp = candidates[i].important;

        if (importantFound) break;
        if (!imp && questFound) continue;

        if (!NnEvalCondition(info, npc)) continue;

        if (imp) {
            importantFound = true;
            if (why) *why = NnDescribe(info);
        }
        else if (NnInfoStartsQuest(info)) {
            questFound = true;
            if (why) *why = NnDescribe(info) + " {quest-start: " + NnQuestVerdictFor(info).how + "}";
        }
        else if (!talkFound) {
            talkFound = true;
            if (why && why->empty()) *why = NnDescribe(info);
        }
    }
    if (importantFound) return flags | NN_IMPORTANT;
    if (questFound)     return flags | NN_QUEST;
    return talkFound ? (flags | NN_TALK) : flags;
}

#pragma endregion




#if MD_DEBUG
// ============================================================================
// CENSUS: which externals do real condition scripts actually call? (debug only)
// Hotkey: Ctrl+Shift+F10 -> writes [census] lines to NpcNames.log
// ============================================================================

// Implementation address of a script external. Ghidra on the exe showed that the symbol field at
// +0x1C is written by SetOffset (also used by DefineExternal). Both candidate words are logged,
// and whichever one lies in the exe's code range is returned.
static unsigned NnExtAddr(zCPar_Symbol* s, unsigned* rawContent = nullptr, unsigned* rawOffset = nullptr) {
    const char* b = reinterpret_cast<const char*>(s);
    const unsigned content = *reinterpret_cast<const unsigned*>(b + 0x18);
    const unsigned offset = *reinterpret_cast<const unsigned*>(b + 0x1C);
    if (rawContent) *rawContent = content;
    if (rawOffset)  *rawOffset = offset;
    auto inCode = [](unsigned v) { return v >= 0x00401000u && v < 0x00800000u; };
    if (inCode(offset))  return offset;
    if (inCode(content)) return content;
    return 0;
}

struct NnCensusCtx {
    std::unordered_set<int> visited;
    std::set<std::string>   externals;   // every external reachable from the condition
    std::set<std::string>   writes;      // every non-local symbol assigned
    bool truncated = false;              // depth / step limit hit, result incomplete
};

// Same decoding rules as NnScanPure, but it never stops at the first problem: it collects everything.
static void NnCensusWalk(int pos, int depth, NnCensusCtx& cx) {
    if (!parser || !parser->stack.stack) return;
    const zBYTE* code = parser->stack.stack;
    const int maxBytes = parser->stack.stacksize;
    if (pos < 0 || pos >= maxBytes) return;
    if (!cx.visited.insert(pos).second) return;
    if (depth <= 0) { cx.truncated = true; return; }

    int cur = pos, maxJump = 0, steps = 0, lastPush = -1;
    while (cur < maxBytes && steps++ < 8192) {
        const int tok = code[cur++];
        const int os = NnOperandSize(tok);
        if (cur + os > maxBytes) break;
        const int operand = (os >= 4) ? *reinterpret_cast<const int*>(code + cur) : 0;
        cur += os;

        if (tok == zPAR_TOK_RET) {
            if (cur > maxJump) return;
            lastPush = -1;
        }
        else if (tok == zPAR_TOK_PUSHVAR || tok == (zPAR_TOK_PUSHVAR | zPAR_TOK_FLAGARRAY) ||
            tok == zPAR_TOK_PUSHINST) {
            lastPush = operand;
        }
        else if (tok == zPAR_TOK_JUMP || tok == zPAR_TOK_JUMPF) {
            if (operand > maxJump && operand < maxBytes) maxJump = operand;
            lastPush = -1;
        }
        else if (tok == zPAR_TOK_CALL) {
            NnCensusWalk(operand, depth - 1, cx);
            lastPush = -1;
        }
        else if (tok == zPAR_TOK_CALLEXTERN) {
            zCPar_Symbol* fs = NnSym(operand);
            if (fs && fs->name.ToChar()) cx.externals.insert(fs->name.ToChar());
            lastPush = -1;
        }
        else if (NnIsAssignOp(tok)) {
            if (!NnIsLocalTarget(lastPush)) {
                zCPar_Symbol* ts = (lastPush > 0) ? NnSym(lastPush) : nullptr;
                cx.writes.insert(ts ? ts->name.ToChar() : "<no target>");
            }
            lastPush = -1;
        }
        else lastPush = -1;
    }
    cx.truncated = true;
}

static void NnDumpPackedFlags()
{
    auto* w = ogame ? ogame->GetGameWorld() : nullptr;
    if (!w || !w->voblist_npcs) return;

    int total = 0, nonzero = 0;
    std::map<int, int> hist;
    for (auto* l = w->voblist_npcs->next; l; l = l->next) {
        oCNpc* n = l->data;
        if (!n) continue;
        int v = *(int*)((char*)&n->inventory2 + 0xA4);
        ++total;
        if (v) ++nonzero;
        ++hist[v];
    }
    int hv = player ? *(int*)((char*)&player->inventory2 + 0xA4) : -1;

    char buf[160];
    sprintf_s(buf, "[census] inv+0xA4: npcs=%d nonzero=%d hero=%d", total, nonzero, hv);
    NnLog(buf);

    for (auto& kv : hist) {
        sprintf_s(buf, "[census]   value %d -> %d npcs", kv.first, kv.second);
        NnLog(buf);
    }
}

static void NnDumpWorldSlot()
{
    if (!ogame) return;
    auto* w = ogame->GetGameWorld();
    if (!w) return;
    DWORD vt = *(DWORD*)w;
    char buf[160];
    sprintf_s(buf, "[census] oCWorld vtbl=0x%08X slot+0x4C=0x%08X slot+0x68=0x%08X",
        vt, *(DWORD*)(vt + 0x4C), *(DWORD*)(vt + 0x68));
    NnLog(buf);
}

static void NnDumpInvSlots()
{
    if (!player) return;
    DWORD vt = *(DWORD*)&player->inventory2;   // inventory is embedded at npc+0x668, first member = vptr
    char buf[160];
    sprintf_s(buf, "[census] oCNpcInventory vtbl=0x%08X  +0xC4=0x%08X +0xC8=0x%08X +0xCC=0x%08X",
        vt, *(DWORD*)(vt + 0xC4), *(DWORD*)(vt + 0xC8), *(DWORD*)(vt + 0xCC));
    NnLog(buf);
}

static void NnQuestCensus() {
    if (!parser || !ogame || !ogame->GetInfoManager()) return;
    NnQuestIndexFinish();   // debug only: finish the incremental build in one go
    int total = 0, byLevel[8] = {};
    int seen1 = 0, seen2 = 0, seenNo = 0, shown1 = 0, shown2 = 0, shownNo = 0;
    for (size_t i = 0; i < g_qRecs.size(); ++i) {
        const NnInfoRec& r = g_qRecs[i];
        oCInfo* info = r.info;
        if (!info || info->pd.trade || info->pd.permanent) continue;
        if (!info->pd.important && info->pd.description.IsEmpty()) continue;
        ++total;
        const int lv = r.quest ? (r.level < 7 ? r.level : 7) : 0;
        ++byLevel[lv];

        const std::string line = std::string(info->name.ToChar()) + " | \"" + info->pd.description.ToChar() + "\"";
        if (lv == 1) { if (seen1++ % 10 == 0 && shown1 < 40) { ++shown1; NnLog("[quest] L1  " + line + " <- " + r.how); } }
        else if (lv >= 2) { if (seen2++ % 5 == 0 && shown2 < 60) { ++shown2; NnLog("[quest] L" + std::to_string(lv) + "  " + line + " <- " + r.how); } }
        else if (seenNo++ % 50 == 0 && shownNo < 40) { ++shownNo; NnLog("[quest] no  " + line); }
    }
    char b[240];
    sprintf_s(b, "[quest] ===== candidates=%d direct=%d L2=%d L3=%d L4+=%d none=%d chainDepth=%d =====",
        total, byLevel[1], byLevel[2], byLevel[3], byLevel[4] + byLevel[5] + byLevel[6] + byLevel[7], byLevel[0], g_ChainDepth);
    NnLog(b);
}

static void NnRunCensus() {
    if (!parser || !ogame || !ogame->GetInfoManager()) return;

    struct Row { int infos = 0; std::string sample; };
    std::map<std::string, Row> ext, wr;
    int total = 0, scanned = 0, truncated = 0;

    int guard = 0;
    for (zCListSort<oCInfo>* l = &ogame->GetInfoManager()->infoList; l && guard++ < 200000; l = l->next) {
        oCInfo* info = l->GetData();
        if (!info) continue;
        ++total;
        zCPar_Symbol* s = NnSym(info->pd.conditions);
        if (!s || s->type != NN_SYM_FUNC || (s->flags & NN_FLAG_EXTERNAL)) continue;
        ++scanned;

        NnCensusCtx cx;
        NnCensusWalk(s->single_intdata, 8, cx);
        if (cx.truncated) ++truncated;
        for (const std::string& n : cx.externals) { Row& r = ext[n]; if (r.infos++ == 0) r.sample = info->name.ToChar(); }
        for (const std::string& n : cx.writes) { Row& r = wr[n];  if (r.infos++ == 0) r.sample = info->name.ToChar(); }
    }

    char b[200];
    sprintf_s(b, "[census] ===== %d infos, %d with script condition, %d truncated, %d distinct externals =====",
        total, scanned, truncated, static_cast<int>(ext.size()));
    NnLog(b);

    std::vector<std::pair<std::string, Row>> sorted(ext.begin(), ext.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const std::pair<std::string, Row>& x, const std::pair<std::string, Row>& y) { return x.second.infos > y.second.infos; });

    for (auto& kv : sorted) {
        const int idx = parser->GetIndex(kv.first.c_str());
        zCPar_Symbol* s = (idx > 0) ? NnSym(idx) : nullptr;
        const unsigned addr = s ? NnExtAddr(s) : 0;
        sprintf_s(b, "[census] %-9s %-34s addr=0x%08X infos=%-5d e.g. ",
            NnIsPureExternal(kv.first.c_str()) ? "LISTED" : "UNLISTED", kv.first.c_str(), addr, kv.second.infos);
        NnLog(std::string(b) + kv.second.sample);
    }

    NnLog("[census] ----- non-local writes found in condition scripts (impure) -----");
    for (auto& kv : wr) {
        sprintf_s(b, "[census] WRITES %-40s infos=%-5d e.g. ", kv.first.c_str(), kv.second.infos);
        NnLog(std::string(b) + kv.second.sample);
    }

    // allowlist entries that no condition ever uses -> no need to audit them
    std::string unused;
    for (const std::string& n : NnPureExternalSet()) if (!ext.count(n)) unused += n + " ";
    NnLog("[census] listed but never used by any condition: " + unused);

    // calibration: known state-changing external, compare with the FUN_ pushed next to 006d5e5f in Ghidra
    const int ci = parser->GetIndex("NPC_CANSEENPC");
    if (zCPar_Symbol* cs = (ci > 0) ? NnSym(ci) : nullptr) {
        unsigned a = 0, o = 0;
        const unsigned pick = NnExtAddr(cs, &a, &o);
        sprintf_s(b, "[census] CALIBRATION NPC_CANSEENPC: +0x18=0x%08X +0x1C=0x%08X picked=0x%08X", a, o, pick);
        NnLog(b);
    }
    NnDumpPackedFlags();
    NnDumpWorldSlot();
    NnDumpInvSlots();
    NnQuestCensus();
    NnLog("[census] ===== done =====");
}


static void NnHandleCensusHotkey() {
    static bool wasDown = false;
    const bool down = (KeyEdge(KEY_V))
        && KeyDownNow(KEY_LSHIFT)
        && NnGameHasFocus();
    if (down && !wasDown) NnRunCensus();
    wasDown = down;
}
#endif // MD_DEBUG





struct NnEntry { int flags; DWORD time; };
static std::unordered_map<oCNpc*, NnEntry> nnCache;
static DWORD nnLastPurge = 0;

static void NnClearCache() { nnCache.clear(); }

static int NnGetFlags(oCNpc* npc, int& budget, DWORD now) {
    auto it = nnCache.find(npc);

    if (it != nnCache.end() && (now - it->second.time) < NN_CACHE_MS)
        return it->second.flags;

    if (budget <= 0)
        return it != nnCache.end() ? it->second.flags : 0;
    --budget;

    int oldFlags = (it != nnCache.end()) ? it->second.flags : 0;

    std::string why;
    NnEntry e;
    e.flags = NnClassifyNpc(npc, g_Mode == MODE_ALL || g_Mode == MODE_NOTRADER, g_DebugMode >= 1 ? &why : nullptr);
    e.time = now;
    nnCache[npc] = e;

    if (g_DebugMode >= 1 && e.flags != oldFlags) {
        char head[160];
        sprintf_s(head, "trader=%d quest=%d talk=%d important=%d",
            (e.flags & NN_TRADER) ? 1 : 0,
            (e.flags & NN_QUEST) ? 1 : 0,
            (e.flags & NN_TALK) ? 1 : 0,
            (e.flags & NN_IMPORTANT) ? 1 : 0);
        NnLog(std::string(npc->GetName(0).ToChar()) + " : " + head +
            (why.empty() ? "" : "  <- " + why));
    }
    return e.flags;
}

static void NnPurgeCache() {
    DWORD now = GetTickCount();
    if ((now - nnLastPurge) < 10000) return;
    nnLastPurge = now;
    for (auto it = nnCache.begin(); it != nnCache.end();) {
        if ((now - it->second.time) > 10000) it = nnCache.erase(it);
        else ++it;
    }
}

static void NnRebuildFonts() {
    const std::string* src[3] = { &g_FontNear, &g_FontMid, &g_FontFar };
    for (int i = 0; i < 3; ++i) {
        if (!g_fontZ[i]) g_fontZ[i] = new zSTRING(src[i]->c_str());
        else *g_fontZ[i] = src[i]->c_str();
    }
    if (!g_traderPrefix) g_traderPrefix = new zSTRING("$ ");
    g_fontsDirty = false;
}

static zCView* pNameView = nullptr;

static void NnDraw() {
    const bool msgActive = GetTickCount() < g_MsgUntil;
    const bool active = (g_Mode != MODE_OFF);

    if (!active && !msgActive && !g_ViewDirty) return;

    if (!pNameView) {
        pNameView = new zCView(0, 0, 8192, 8192);
        screen->InsertItem(pNameView);
    }
    pNameView->ClrPrintwin();
    g_ViewDirty = false;

    if (msgActive) {
        zSTRING m(g_Msg.c_str());
        pNameView->SetFont("FONT_OLD_10_WHITE.TGA");
        pNameView->SetFontColor(zCOLOR(255, 255, 255));
        pNameView->Print(4096 - pNameView->FontSize(m) / 2, 1200, m);
        g_ViewDirty = true;
    }

    oCNpc* hero = oCNpc::player;
    if (!active || !ogame || !hero) return;

    zCCamera* cam = zCCamera::activeCam;
    if (!cam) return;

    NnPurgeCache();
    NnQuestIndexStep(NN_QUEST_SCAN_PER_FRAME);

    if (zCMenu::GetActive()) return; // stops drawing while ESC menu is active

    int budget = NN_MAX_CHECKS_FRAME;
    const DWORD now = GetTickCount();

    oCNpc* focusNpc = g_HideInFocus ? hero->GetFocusNpc() : nullptr;


    if (g_fontsDirty || !g_traderPrefix) NnRebuildFonts();

    zCListSort<oCNpc>* list = ogame->GetGameWorld() ? ogame->GetGameWorld()->voblist_npcs : nullptr;

    const zVEC3  heroPos = hero->GetPositionWorld();
    const float  radius2 = g_Radius * g_Radius;
    const float  minDist2 = g_MinDistance * g_MinDistance;
    const float  near2 = g_DistNear * g_DistNear;
    const float  mid2 = g_DistMid * g_DistMid;
    int curTier = -1;   // font currently set on the view (msg block above may have changed it)

    while (list) {
        oCNpc* npc = list->GetData();
        list = list->next;

        if (!npc || npc == hero) continue;
        if (focusNpc && npc == focusNpc) continue;
        if (npc->attribute[0] <= 0) continue;

        const zVEC3 npcPos = npc->GetPositionWorld();
        const float dx = npcPos[0] - heroPos[0];
        const float dy = npcPos[1] - heroPos[1];
        const float dz = npcPos[2] - heroPos[2];
        const float dist2 = dx * dx + dy * dy + dz * dz;
        if (dist2 > radius2) continue;
        if (dist2 < minDist2) continue;

        zVEC3 pos = npcPos;
        pos[1] = npc->bbox3D.maxs[1] + NN_HEAD_OFFSET;
        zVEC3 camPos = cam->trafoView * pos;
        if (camPos[2] <= 10.0f) continue;

        int px = 0, py = 0;
        cam->Project(&camPos, px, py);
        int vx = screen->anx(px);
        int vy = screen->any(py);
        if (vx < 0 || vx > 8191 || vy < 0 || vy > 8191) continue;

        const int att = npc->GetAttitude(hero);          // now only for on-screen NPCs
        if (att == NPC_ATT_HOSTILE || att == NPC_ATT_ANGRY) continue;

        int flags = NnGetFlags(npc, budget, now);
        if (g_Mode == MODE_QUEST)    flags &= ~NN_TALK;
        if (g_Mode == MODE_NOTRADER) flags &= ~NN_TRADER;
        if (flags == 0 && g_DebugMode < 2) continue;

        zSTRING npcName = npc->GetName(0);
        if (npcName.IsEmpty()) continue;
        zSTRING name = npcName;
        if (flags & NN_TRADER) name = *g_traderPrefix + npcName;

        const int tier = (dist2 < near2) ? 0 : (dist2 < mid2) ? 1 : 2;
        if (tier != curTier) { pNameView->SetFont(*g_fontZ[tier]); curTier = tier; }
        pNameView->SetFontColor(NnColor(flags));

        const int w = pNameView->FontSize(name);
        pNameView->Print(vx - w / 2, vy - pNameView->FontY(), name);
    }
}

static void LoadBegin() {
#if MD_DEBUG
    //if (g_DebugMode >= 3) 
    NnDumpExternalStats();
#endif
    NnClearCache();
    NnResetScriptCaches();
    if (pNameView) pNameView->ClrPrintwin();
}
static void LoadEnd() {
    NnClearCache();
    NnResetScriptCaches();

#if MD_DEBUG
    //if (g_DebugMode >= 1) 
    NnAuditAllowlist();
#endif
}

cexport void Game_Init() {
    LoadConfig();
    g_Mode = g_StartMode;
    if (g_DebugMode >= 1)
    {
        NnLog("[NpcNames] log started; If the log shows an [impure-skip] for a harmless external, put its name in the ini like so in gothic.ini: ExtraPureExternals=WLD_ISRAINING,NPC_REFUSETALK",
            true);
    }
}
cexport void Game_Entry() {}
cexport void Game_Exit() {
#if MD_DEBUG
    //if (g_DebugMode >= 3) 
    NnDumpExternalStats();
#endif
}
cexport void Game_PreLoop() {}
cexport void Game_Loop() {
    if (!screen) return;
    NnHandleToggle();

#if MD_DEBUG
    NnHandleCensusHotkey();
#endif

    NnDraw();
}
cexport void Game_PostLoop() {}
cexport void Game_MenuLoop() {}
cexport void Game_SaveBegin() {}
cexport void Game_SaveEnd() {}
cexport void Game_LoadBegin_NewGame() { LoadBegin(); }
cexport void Game_LoadEnd_NewGame() { LoadEnd(); }
cexport void Game_LoadBegin_SaveGame() { LoadBegin(); }
cexport void Game_LoadEnd_SaveGame() { LoadEnd(); }
cexport void Game_LoadBegin_ChangeLevel() { LoadBegin(); }
cexport void Game_LoadEnd_ChangeLevel() { LoadEnd(); }
cexport void Game_LoadBegin_Trigger() {}
cexport void Game_LoadEnd_Trigger() {}
cexport void Game_Pause() {}
cexport void Game_Unpause() {}
cexport void Game_DefineExternals() {}
cexport void Game_ApplyOptions() { LoadConfig(); }

/*OLD set:
* static const std::unordered_set<std::string> ok = {
        // --- Npc_ queries ---
"NPC_KNOWSINFO","NPC_HASITEMS","NPC_ISDEAD",
"NPC_GETDISTTONPC","NPC_GETDISTTOWP","NPC_GETDISTTOPLAYER","NPC_GETDISTTOITEM",
"NPC_GETHEIGHTTONPC","NPC_GETHEIGHTTOITEM",
"NPC_GETTRUEGUILD","NPC_GETGUILDATTITUDE","NPC_GETATTITUDE","NPC_GETPERMATTITUDE",
"NPC_GETTALENTSKILL","NPC_GETTALENTVALUE",
"NPC_ISINSTATE","NPC_WASINSTATE","NPC_ISINROUTINE","NPC_ISPLAYER","NPC_ISINFIGHTMODE",
"NPC_ISAIMING","NPC_ISNEAR","NPC_ISONFP","NPC_ISWAYBLOCKED","NPC_ISINCUTSCENE",
"NPC_ISVOICEACTIVE","NPC_ISDRAWINGWEAPON","NPC_ISDRAWINGSPELL",
"NPC_GETBODYSTATE","NPC_HASBODYFLAG","NPC_GETSTATETIME","NPC_HASOFFERED","NPC_KNOWSPLAYER",
"NPC_OWNEDBYNPC","NPC_OWNEDBYGUILD","NPC_ISDETECTEDMOBOWNEDBYNPC","NPC_ISDETECTEDMOBOWNEDBYGUILD",
"NPC_GETNEARESTWP","NPC_GETNEXTWP",

"NPC_AREWESTRONGER","NPC_HASSPELL",
"NPC_GETACTIVESPELL","NPC_GETACTIVESPELLCAT","NPC_GETACTIVESPELLLEVEL","NPC_GETACTIVESPELLISSCROLL",
"NPC_GETLASTHITSPELLID","NPC_GETLASTHITSPELLCAT",
"NPC_GETREADIEDWEAPON","NPC_GETEQUIPPEDARMOR","NPC_GETEQUIPPEDMELEEWEAPON","NPC_GETEQUIPPEDRANGEDWEAPON",
"NPC_HASEQUIPPEDARMOR","NPC_HASEQUIPPEDWEAPON","NPC_HASEQUIPPEDMELEEWEAPON","NPC_HASEQUIPPEDRANGEDWEAPON",
"NPC_HASREADIEDWEAPON","NPC_HASREADIEDMELEEWEAPON","NPC_HASREADIEDRANGEDWEAPON",
"NPC_HASRANGEDWEAPONWITHAMMO","NPC_REFUSETALK",
"NPC_ISPLAYERINMYROOM","NPC_ISINPLAYERSROOM","NPC_WASPLAYERINMYROOM","NPC_GETPORTALOWNER",
// --- Hlp_ / Mob_ / Mis_ / Log_ queries ---
"HLP_STRCMP","HLP_ISVALIDNPC","HLP_ISVALIDITEM","HLP_ISITEM","HLP_GETNPC","HLP_GETINSTANCEID",
"HLP_CUTSCENEPLAYED","HLP_GETHEROSTATUS","HLP_HASINSLOT",
"MOB_HASITEMS","MIS_GETSTATUS","MIS_ONTIME","LOG_GETTOPICSTATUS",
// --- Wld_ queries ---
"WLD_ISTIME","WLD_GETDAY","WLD_GETTIMEMIN","WLD_ISRAINING",
"WLD_ISFPAVAILABLE","WLD_ISNEXTFPAVAILABLE","WLD_ISMOBAVAILABLE","WLD_GETMOBSTATE",
"WLD_GETGUILDATTITUDE","WLD_GETPLAYERPORTALGUILD","WLD_GETPLAYERPORTALOWNER",
"WLD_GETFORMERPLAYERPORTALGUILD","WLD_GETFORMERPLAYERPORTALOWNER",
// --- pure value conversion ---
"INTTOSTRING","INTTOFLOAT","FLOATTOINT","FLOATTOSTRING","CONCATSTRINGS",
}
*/

