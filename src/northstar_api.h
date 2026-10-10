// northstar_api.h: the parts of Northstar's plugin interface (plugins v3: PluginId001,
// PluginCallbacks001, NSSys001) and of Titanfall 2's engine and server script VM this plugin uses.
//
// Interfaces: declared as in R2Northstar/NorthstarLauncher primedev/plugins/interfaces/
// (IPluginId.h, IPluginCallbacks.h, sys/ISys.h). Game addresses: the same module offsets
// NorthstarLauncher itself binds (primedev/squirrel/squirrel.cpp for server.dll's Squirrel API,
// primedev/engine/r2engine.cpp for engine.dll's command buffer); Titanfall 2 no longer updates,
// so Northstar and this plugin load against the same binaries. The one address Northstar doesn't
// bind (the client's view angles) is checked byte for byte before use (plugin.cpp).
#pragma once
#include <windows.h>
#include <cstdint>

namespace ns {

// ---- the plugin interface -------------------------------------------------------------------

enum class PluginString : int { NAME = 0, LOG_NAME = 1, DEPENDENCY_NAME = 2 };
enum class PluginField : int { CONTEXT = 0, COLOR = 1 };
namespace PluginContext { enum : uint64_t { DEDICATED = 0x1, CLIENT = 0x2 }; }

struct PluginNorthstarData {
    HMODULE  pluginHandle;
    uint64_t size;
};

struct CSquirrelVM;

class IPluginId {
public:
    virtual const char* GetString(PluginString prop) = 0;
    virtual int64_t GetField(PluginField prop) = 0;
};

class IPluginCallbacks {
public:
    virtual void Init(HMODULE northstarModule, const PluginNorthstarData* initData, bool reloaded) = 0;
    virtual void Finalize() = 0;
    virtual bool Unload() = 0;
    virtual void OnSqvmCreated(CSquirrelVM* sqvm) = 0;
    virtual void OnSqvmDestroying(CSquirrelVM* sqvm) = 0;
    virtual void OnLibraryLoaded(HMODULE module, const char* name) = 0;
    virtual void RunFrame() = 0;
};

enum class LogLevel : int { INFO = 0, WARN, ERR };

class ISys {
public:
    virtual void Log(int64_t handle, LogLevel level, char* msg) = 0;
    virtual void Unload(int64_t handle) = 0;
    virtual void Reload(int64_t handle) = 0;
};

using CreateInterfaceFn = void* (*)(const char* name, int* status);

// ---- the script VM ----------------------------------------------------------------------------

// primedev/vscript/languages/squirrel_re/vsquirrel.h: vmContext at 0x3C.
enum class ScriptContext : int { SERVER = 0, CLIENT = 1, UI = 2 };
inline ScriptContext VmContext(const CSquirrelVM* vm) {
    return static_cast<ScriptContext>(*reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(vm) + 0x3C));
}

struct SQVM;
using HSQUIRRELVM = SQVM*;
using SQInteger = long;
using SQFunction = SQInteger (*)(HSQUIRRELVM);

enum class eSQReturnType : uint32_t { Float = 0x1, Vector = 0x3, Integer = 0x5, Boolean = 0x6, Default = 0x20, String = 0x21 };

struct SQFuncRegistration {
    const char*    squirrelFuncName;
    const char*    cppFuncName;
    const char*    helpText;
    const char*    returnTypeString;
    const char*    argTypes;
    uint32_t       unknown1;
    uint32_t       devLevel;
    const char*    shortNameMaybe;
    uint32_t       unknown2;
    eSQReturnType  returnType;
    uint32_t*      externalBufferPointer;
    uint64_t       externalBufferSize;
    uint64_t       unknown3;
    uint64_t       unknown4;
    SQFunction     funcPtr;
};

// server.dll's Squirrel API (Northstar's ServerSquirrel binding).
struct ServerSq {
    void (*pushfloat)(HSQUIRRELVM, float) = nullptr;
    void (*pushinteger)(HSQUIRRELVM, SQInteger) = nullptr;
    void (*pushvector)(HSQUIRRELVM, const float*) = nullptr;
    const char* (*getstring)(HSQUIRRELVM, SQInteger) = nullptr;
    SQInteger (*getinteger)(HSQUIRRELVM, SQInteger) = nullptr;
    float (*getfloat)(HSQUIRRELVM, SQInteger) = nullptr;
    const float* (*getvector)(HSQUIRRELVM, SQInteger) = nullptr;
    int64_t (*registerFunc)(CSquirrelVM*, SQFuncRegistration*, char) = nullptr;

    bool Bind(HMODULE server) {
        auto* b = reinterpret_cast<uint8_t*>(server);
        if (!b) return false;
        pushinteger = reinterpret_cast<decltype(pushinteger)>(b + 0x36A0);
        pushvector = reinterpret_cast<decltype(pushvector)>(b + 0x3780);
        pushfloat = reinterpret_cast<decltype(pushfloat)>(b + 0x3800);
        getstring = reinterpret_cast<decltype(getstring)>(b + 0x60A0);
        getinteger = reinterpret_cast<decltype(getinteger)>(b + 0x60C0);
        getfloat = reinterpret_cast<decltype(getfloat)>(b + 0x60E0);
        getvector = reinterpret_cast<decltype(getvector)>(b + 0x6120);
        registerFunc = reinterpret_cast<decltype(registerFunc)>(b + 0x1DD10);
        return true;
    }
};

// ---- the engine -------------------------------------------------------------------------------

// primedev/engine/r2engine.h
enum class ECommandTarget_t : int { CBUF_FIRST_PLAYER = 0 };
enum class cmd_source_t : int { kCommandSrcCode = 0 };

struct Engine {
    ECommandTarget_t (*cbufGetCurrentPlayer)() = nullptr;
    void (*cbufAddText)(ECommandTarget_t, const char*, cmd_source_t) = nullptr;
    void (*setViewAngles)(void* self, float* angles) = nullptr;  // only when its bytes match
    void* engineClient = nullptr;

    bool Bind(HMODULE engine);  // plugin.cpp
};

}  // namespace ns
