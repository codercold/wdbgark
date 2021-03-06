/*
    * WinDBG Anti-RootKit extension
    * Copyright � 2013-2015  Vyacheslav Rusakoff
    * 
    * This program is free software: you can redistribute it and/or modify
    * it under the terms of the GNU General Public License as published by
    * the Free Software Foundation, either version 3 of the License, or
    * (at your option) any later version.
    * 
    * This program is distributed in the hope that it will be useful,
    * but WITHOUT ANY WARRANTY; without even the implied warranty of
    * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    * GNU General Public License for more details.
    * 
    * You should have received a copy of the GNU General Public License
    * along with this program.  If not, see <http://www.gnu.org/licenses/>.

    * This work is licensed under the terms of the GNU GPL, version 3.  See
    * the COPYING file in the top-level directory.
*/

#include <string>
#include <algorithm>
#include <memory>

#include "wdbgark.hpp"
#include "manipulators.hpp"
#include "symbols.hpp"
#include "udis.hpp"

EXT_DECLARE_GLOBALS();

namespace wa {

WDbgArk::WDbgArk() : m_inited(false),
                     m_is_cur_machine64(false),
                     m_system_cb_commands(),
                     m_callout_names(),
                     m_gdt_selectors(),
                     m_hal_tbl_info(),
                     m_synthetic_symbols(),
                     m_obj_helper(nullptr),
                     m_color_hack(nullptr),
                     m_dummy_pdb(nullptr),
                     m_system_ver(nullptr),
                     m_symbols3_iface("The extension did not initialize properly."),
                     out(),
                     warn(),
                     err() {
#if defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
    // _CrtSetBreakAlloc( 143 );
#endif  // _DEBUG
}

WDbgArk::~WDbgArk() {
#if defined(_DEBUG)
    _CrtDumpMemoryLeaks();
#endif  // _DEBUG
}

bool WDbgArk::Init() {
    if ( IsInited() )
        return true;

#define REQ_IF(_If, _member) \
    if ( m_Client->QueryInterface(__uuidof(_If), reinterpret_cast<PVOID*>(&_member)) != S_OK ) { \
        _member.Set(nullptr); \
        err << wa::showminus << __FUNCTION__ << ": Failed to initialize interface" << endlerr; \
    }

    REQ_IF(IDebugSymbols3, m_symbols3_iface);
#undef REQ_IF

    m_is_cur_machine64 = IsCurMachine64();

    // get system version
    m_system_ver.reset(new WDbgArkSystemVer);

    if ( !m_system_ver->IsInited() ) {
        err << wa::showminus << __FUNCTION__ ": WDbgArkSystemVer init failed" << endlerr;
        return false;
    }

    m_Symbols->Reload("");  // revise debuggee modules list

    if ( !CheckSymbolsPath(true) )
        warn << wa::showqmark << __FUNCTION__ ": CheckSymbolsPath failed" << endlwarn;

    // it's a bad idea to do this in constructor's initialization list 'coz global class uninitialized
    m_obj_helper.reset(new WDbgArkObjHelper);

    if ( !m_obj_helper->IsInited() )
        warn << wa::showqmark << __FUNCTION__ ": WDbgArkObjHelper init failed" << endlwarn;

    m_color_hack.reset(new WDbgArkColorHack);

    if ( !m_color_hack->IsInited() )
        warn << wa::showqmark << __FUNCTION__ ": WDbgArkColorHack init failed" << endlwarn;

    m_dummy_pdb.reset(new WDbgArkDummyPdb);

    if ( !m_dummy_pdb->IsInited() )
        warn << wa::showqmark << __FUNCTION__ ": WDbgArkDummyPdb init failed" << endlwarn;

    InitCallbackCommands();
    InitCalloutNames();
    InitGDTSelectors();
    InitHalTables();

    if ( m_system_ver->GetStrictVer() >= W7RTM_VER && !FindDbgkLkmdCallbackArray() )
        warn << wa::showqmark << __FUNCTION__ ": FindDbgkLkmdCallbackArray failed" << endlwarn;

    m_inited = true;

    return m_inited;
}

void WDbgArk::InitCallbackCommands(void) {
    unsigned __int32 timer_routine_offset = 0;

    if ( GetFieldOffset("nt!_IO_TIMER", "TimerRoutine", reinterpret_cast<PULONG>(&timer_routine_offset)) != 0 )
        warn << wa::showqmark << __FUNCTION__ << ": GetFieldOffset failed with nt!_IO_TIMER.TimerRoutine" << endlwarn;

    unsigned __int32 le_size = GetTypeSize("nt!_LIST_ENTRY");

    m_system_cb_commands = { {
        { "image", { "nt!PspLoadImageNotifyRoutineCount", "nt!PspLoadImageNotifyRoutine", 0, 0, 0 } },
        { "process", { "nt!PspCreateProcessNotifyRoutineCount", "nt!PspCreateProcessNotifyRoutine", 0, 0, 0 } },
        { "thread", { "nt!PspCreateThreadNotifyRoutineCount", "nt!PspCreateThreadNotifyRoutine", 0, 0, 0 } },
        { "registry", { "nt!CmpCallBackCount", "nt!CmpCallBackVector", GetCmCallbackItemFunctionOffset(), 0, 0 } },
        { "bugcheck", { "", "nt!KeBugCheckCallbackListHead", le_size, 0, 0 } },
        { "bugcheckreason", { "", "nt!KeBugCheckReasonCallbackListHead", le_size, 0, 0 } },
        { "bugcheckaddpages", { "", "nt!KeBugCheckAddPagesCallbackListHead", le_size, 0, 0 } },
        { "bugcheckaddremovepages", { "", "nt!KeBugCheckAddRemovePagesCallbackListHead", le_size, 0, 0 } },
        { "powersetting", { "", "nt!PopRegisteredPowerSettingCallbacks", GetPowerCallbackItemFunctionOffset(), 0, 0 } },
        { "kdppower", { "", "nt!KdpPowerListHead", le_size, 0, 0 } },
        { "callbackdir", {} },
        { "shutdown", { "", "nt!IopNotifyShutdownQueueHead", 0, 0, 0 } },
        { "shutdownlast", { "", "nt!IopNotifyLastChanceShutdownQueueHead", 0, 0, 0 } },
        { "drvreinit", { "", "nt!IopDriverReinitializeQueueHead", le_size + m_PtrSize, 0, 0 } },
        { "bootdrvreinit", { "", "nt!IopBootDriverReinitializeQueueHead", le_size + m_PtrSize, 0, 0 } },
        { "fschange", { "", "nt!IopFsNotifyChangeQueueHead", le_size + m_PtrSize, 0, 0 } },
        { "nmi", { "", "nt!KiNmiCallbackListHead", m_PtrSize, 0, 0 } },
        { "logonsessionroutine", { "", "nt!SeFileSystemNotifyRoutinesHead", m_PtrSize, 0, 0 } },
        { "prioritycallback", { "nt!IopUpdatePriorityCallbackRoutineCount", "nt!IopUpdatePriorityCallbackRoutine", 0,
        0, 0 } },
        { "pnp", {} },
        { "lego", { "", "nt!PspLegoNotifyRoutine", 0, 0, 0 } },
        { "debugprint", { "", "nt!RtlpDebugPrintCallbackList", le_size, 0, 0 } },
        { "alpcplog", { "", "nt!AlpcpLogCallbackListHead", le_size, 0, 0 } },
        { "empcb", { "", "nt!EmpCallbackListHead", GetTypeSize("nt!_GUID"), 0, 0 } },
        { "ioperf", { "", "nt!IopPerfIoTrackingListHead", le_size, 0, 0 } },
        { "dbgklkmd", { "", "nt!DbgkLkmdCallbackArray", 0, 0, 0 } },
        { "ioptimer", { "", "nt!IopTimerQueueHead", timer_routine_offset, 0, 0 } } } };

    for ( auto cb_pair : m_system_cb_commands ) {
        unsigned __int64 offset_count = 0ULL;
        unsigned __int64 offset_head = 0ULL;

        if ( !cb_pair.second.list_count_name.empty() ) {
            if ( GetSymbolOffset(cb_pair.second.list_count_name.c_str(), true, &offset_count) )
                cb_pair.second.list_count_address = offset_count;
        }

        if ( !cb_pair.second.list_head_name.empty() ) {
            if ( GetSymbolOffset(cb_pair.second.list_head_name.c_str(), true, &offset_head) )
                cb_pair.second.list_head_address = offset_head;
        }
    }
}

void WDbgArk::InitCalloutNames(void) {
    if ( m_system_ver->GetStrictVer() <= W7SP1_VER ) {
        m_callout_names = { "nt!PspW32ProcessCallout", "nt!PspW32ThreadCallout", "nt!ExGlobalAtomTableCallout",
                            "nt!KeGdiFlushUserBatch", "nt!PopEventCallout", "nt!PopStateCallout",
                            "nt!PspW32JobCallout", "nt!ExDesktopOpenProcedureCallout",
                            "nt!ExDesktopOkToCloseProcedureCallout", "nt!ExDesktopCloseProcedureCallout",
                            "nt!ExDesktopDeleteProcedureCallout", "nt!ExWindowStationOkToCloseProcedureCallout",
                            "nt!ExWindowStationCloseProcedureCallout", "nt!ExWindowStationDeleteProcedureCallout",
                            "nt!ExWindowStationParseProcedureCallout", "nt!ExWindowStationOpenProcedureCallout",
                            "nt!IopWin32DataCollectionProcedureCallout", "nt!PopWin32InfoCallout" };
    }
}

void WDbgArk::InitGDTSelectors(void) {
    if ( m_is_cur_machine64 ) {
        m_gdt_selectors = { KGDT64_NULL, KGDT64_R0_CODE, KGDT64_R0_DATA, KGDT64_R3_CMCODE, KGDT64_R3_DATA,
                            KGDT64_R3_CODE, KGDT64_SYS_TSS, KGDT64_R3_CMTEB };
    } else {
        m_gdt_selectors = { KGDT_R0_CODE, KGDT_R0_DATA, KGDT_R3_CODE, KGDT_R3_DATA, KGDT_TSS, KGDT_R0_PCR, KGDT_R3_TEB,
                            KGDT_LDT, KGDT_DF_TSS, KGDT_NMI_TSS, KGDT_GDT_ALIAS, KGDT_CDA16, KGDT_CODE16,
                            KGDT_STACK16 };
    }
}

void WDbgArk::InitHalTables(void) {
    m_hal_tbl_info = { { { WXP_VER, { 0x15, 0x12, 0x0, 0x1 } },
                         { W2K3_VER, { 0x15, 0x13, 0x0, 0x1 } },
                         { VISTA_RTM_VER, { 0x16, 0x1B, 0x0, 0x1 } },
                         { VISTA_SP1_VER, { 0x18, 0x22, 0x0, 0x1 } },
                         { VISTA_SP2_VER, { 0x17, 0x23, 0x0, 0x1 } },
                         { W7RTM_VER, { 0x16, 0x2D, 0x0, 0x1 } },
                         { W7SP1_VER, { 0x16, 0x2D, 0x0, 0x1 } },
                         { W8RTM_VER, { 0x16, 0x5A, 0x0, 0x1 } },
                         { W81RTM_VER, { 0x16, 0x69, 0x0B, 0x1 } },
                         { W10RTM_VER, { 0x16, 0x71, 0x10, 0x1 } } } };
}

void WDbgArk::WalkAnyListWithOffsetToRoutine(const std::string &list_head_name,
                                             const unsigned __int64 offset_list_head,
                                             const unsigned __int32 link_offset,
                                             const bool is_double,
                                             const unsigned __int32 offset_to_routine,
                                             const std::string &type,
                                             const std::string &ext_info,
                                             walkresType* output_list) {
    unsigned __int64 list_head_offset_out = 0;
    unsigned __int64 offset               = offset_list_head;

    if ( !offset_to_routine ) {
        err << wa::showminus << __FUNCTION__ << ": invalid parameter" << endlerr;
        return;
    }

    if ( !offset ) {
        if ( !GetSymbolOffset(list_head_name.c_str(), true, &offset) ) {
            err << wa::showminus << __FUNCTION__ << ": failed to get " << list_head_name << endlerr;
            return;
        } else {
            list_head_offset_out = offset;
        }
    }

    try {
        ExtRemoteList list_head(offset, link_offset, is_double);

        for ( list_head.StartHead(); list_head.HasNode(); list_head.Next() ) {
            unsigned __int64 node = list_head.GetNodeOffset();
            ExtRemoteData structure_data(node + offset_to_routine, m_PtrSize);

            unsigned __int64 routine = structure_data.GetPtr();

            if ( routine ) {
                OutputWalkInfo info;

                info.address = routine;
                info.type = type;
                info.info = ext_info;
                info.list_head_name = list_head_name;
                info.object_address = node;
                info.list_head_address = list_head_offset_out;

                output_list->push_back(info);
            }
        }
    }
    catch ( const ExtRemoteException &Ex ) {
        err << wa::showminus << __FUNCTION__ << ": " << Ex.GetMessage() << endlerr;
    }
}

void WDbgArk::WalkAnyListWithOffsetToObjectPointer(const std::string &list_head_name,
                                                   const unsigned __int64 offset_list_head,
                                                   const bool is_double,
                                                   const unsigned __int32 offset_to_object_pointer,
                                                   void* context,
                                                   RemoteDataCallback callback) {
    unsigned __int64 offset = offset_list_head;

    if ( !offset_to_object_pointer ) {
        err << wa::showminus << __FUNCTION__ << ": invalid parameter offset_to_object_pointer" << endlerr;
        return;
    }

    if ( !offset && !GetSymbolOffset(list_head_name.c_str(), true, &offset) ) {
        err << wa::showminus << __FUNCTION__ << ": failed to get " << list_head_name << endlerr;
        return;
    }

    try {
        ExtRemoteList list_head(offset, 0, is_double);

        for ( list_head.StartHead(); list_head.HasNode(); list_head.Next() ) {
            ExtRemoteData object_pointer(list_head.GetNodeOffset() + offset_to_object_pointer, m_PtrSize);

            if ( !SUCCEEDED(callback(this, object_pointer, context)) ) {
                err << wa::showminus << __FUNCTION__ << ": error while invoking callback" << endlerr;
                return;
            }
        }
    }
    catch ( const ExtRemoteException &Ex ) {
        err << wa::showminus << __FUNCTION__ << ": " << Ex.GetMessage() << endlerr;
    }
}

void WDbgArk::WalkDirectoryObject(const unsigned __int64 directory_address,
                                  void* context,
                                  RemoteTypedCallback callback) {
    if ( !directory_address ) {
        err << wa::showminus << __FUNCTION__ << ": invalid directory address" << endlerr;
        return;
    }

    if ( !callback ) {
        err << wa::showminus << __FUNCTION__ << ": invalid callback address" << endlerr;
        return;
    }

    try {
        ExtRemoteTyped directory_object("nt!_OBJECT_DIRECTORY", directory_address, false, NULL, NULL);
        ExtRemoteTyped buckets = directory_object.Field("HashBuckets");

        const unsigned __int32 num_buckets = buckets.GetTypeSize() / m_PtrSize;

        for ( __int64 i = 0; i < num_buckets; i++ ) {
            for ( ExtRemoteTyped directory_entry = *buckets[i];
                  directory_entry.m_Offset;
                  directory_entry = *directory_entry.Field("ChainLink") ) {
                ExtRemoteTyped object = *directory_entry.Field("Object");

                if ( !SUCCEEDED(callback(this, object, context)) ) {
                    err << wa::showminus << __FUNCTION__ << ": error while invoking callback" << endlerr;
                    return;
                }
            }
        }
    }
    catch ( const ExtRemoteException &Ex ) {
        err << wa::showminus << __FUNCTION__ << ": " << Ex.GetMessage() << endlerr;
    }
}

void WDbgArk::WalkDeviceNode(const unsigned __int64 device_node_address,
                             void* context,
                             RemoteTypedCallback callback) {
    unsigned __int64 offset = device_node_address;

    if ( !callback ) {
        err << wa::showminus << __FUNCTION__ << ": invalid callback address" << endlerr;
        return;
    }

    try {
        if ( !offset ) {
            if ( !GetSymbolOffset("nt!IopRootDeviceNode", true, &offset) ) {
                err << wa::showminus << __FUNCTION__ << ": failed to get nt!IopRootDeviceNode" << endlerr;
                return;
            } else {
                ExtRemoteData device_node_ptr(offset, m_PtrSize);
                offset = device_node_ptr.GetPtr();
            }
        }

        ExtRemoteTyped device_node("nt!_DEVICE_NODE", offset, false, NULL, NULL);

        for ( ExtRemoteTyped child_node = *device_node.Field("Child");
              child_node.m_Offset;
              child_node = *child_node.Field("Sibling") ) {
            if ( !SUCCEEDED(callback(this, child_node, context)) ) {
                err << wa::showminus << __FUNCTION__ << ": error while invoking callback" << endlerr;
                return;
            }

            WalkDeviceNode(child_node.m_Offset, context, callback);
        }
    }
    catch ( const ExtRemoteException &Ex ) {
        err << wa::showminus << __FUNCTION__ << ": " << Ex.GetMessage() << endlerr;
    }
}

void WDbgArk::WalkAnyTable(const unsigned __int64 table_start,
                           const unsigned __int32 offset_table_skip_start,
                           const unsigned __int32 table_count,
                           const std::string &type,
                           walkresType* output_list,
                           bool break_on_null,
                           bool collect_null) {
    unsigned __int64 offset = table_start + offset_table_skip_start;

    try {
        for ( unsigned __int32 i = 0; i < table_count; i++ ) {
            ExtRemoteData data(offset + i * m_PtrSize, m_PtrSize);

            if ( data.GetPtr() || collect_null ) {
                OutputWalkInfo info;

                info.address = data.GetPtr();
                info.type = type;
                info.info.clear();
                info.list_head_name.clear();
                info.object_address = 0ULL;
                info.list_head_address = 0ULL;

                output_list->push_back(info);
            } else if ( break_on_null ) {
                break;
            }
        }
    }
    catch ( const ExtRemoteException &Ex ) {
        err << wa::showminus << __FUNCTION__ << ": " << Ex.GetMessage() << endlerr;
    }
}

void WDbgArk::AddSymbolPointer(const std::string &symbol_name,
                               const std::string &type,
                               const std::string &additional_info,
                               walkresType* output_list) {
    unsigned __int64 offset = 0;

    try {
        if ( GetSymbolOffset(symbol_name.c_str(), true, &offset) ) {
            unsigned __int64 symbol_offset = offset;

            ExtRemoteData routine_ptr(offset, m_PtrSize);
            offset = routine_ptr.GetPtr();

            if ( offset ) {
                OutputWalkInfo info;

                info.address = offset;
                info.type = type;
                info.info = additional_info;
                info.list_head_name = symbol_name;
                info.object_address = 0ULL;
                info.list_head_address = symbol_offset;

                output_list->push_back(info);
            }
        }
    }
    catch ( const ExtRemoteException &Ex ) {
        err << wa::showminus << __FUNCTION__ << ": " << Ex.GetMessage() << endlerr;
    }
}

/*
x86:

PAGE:006A4FCB                               ; __stdcall DbgkLkmdUnregisterCallback(x)
PAGE:006A4FCB                                               public _DbgkLkmdUnregisterCallback@4
PAGE:006A4FCB                               _DbgkLkmdUnregisterCallback@4 proc near
PAGE:006A4FCB
PAGE:006A4FCB                               arg_0           = dword ptr  8
PAGE:006A4FCB
PAGE:006A4FCB 8B FF                                         mov     edi, edi
PAGE:006A4FCD 55                                            push    ebp
PAGE:006A4FCE 8B EC                                         mov     ebp, esp
PAGE:006A4FD0 53                                            push    ebx
PAGE:006A4FD1 56                                            push    esi
PAGE:006A4FD2 57                                            push    edi
PAGE:006A4FD3 33 DB                                         xor     ebx, ebx
PAGE:006A4FD5 BF 20 5B 52 00                                mov     edi, offset dword_525B20 <-- !!!
PAGE:006A4FDA
PAGE:006A4FDA                               loc_6A4FDA:                             
PAGE:006A4FDA 57                                            push    edi
PAGE:006A4FDB E8 52 1C FC FF                                call    _ExReferenceCallBackBlock@4

x64:

PAGE:0000000140482150                               DbgkLkmdUnregisterCallback proc near
PAGE:0000000140482150
PAGE:0000000140482150                               arg_0           = qword ptr  8
PAGE:0000000140482150                               arg_8           = qword ptr  10h
PAGE:0000000140482150                               arg_10          = qword ptr  18h
PAGE:0000000140482150
PAGE:0000000140482150 48 89 5C 24 08                                mov     [rsp+arg_0], rbx
PAGE:0000000140482155 48 89 6C 24 10                                mov     [rsp+arg_8], rbp
PAGE:000000014048215A 48 89 74 24 18                                mov     [rsp+arg_10], rsi
PAGE:000000014048215F 57                                            push    rdi
PAGE:0000000140482160 41 54                                         push    r12
PAGE:0000000140482162 41 55                                         push    r13
PAGE:0000000140482164 48 83 EC 20                                   sub     rsp, 20h
PAGE:0000000140482168 33 FF                                         xor     edi, edi
PAGE:000000014048216A 48 8B E9                                      mov     rbp, rcx
PAGE:000000014048216D 4C 8D 2D 9C 1E D7 FF                          lea     r13, unk_1401F4010 <-- !!!
PAGE:0000000140482174 44 8D 67 01                                   lea     r12d, [rdi+1]
PAGE:0000000140482178
PAGE:0000000140482178                               loc_140482178:                          
PAGE:0000000140482178 8B F7                                         mov     esi, edi
PAGE:000000014048217A 48 C1 E6 04                                   shl     rsi, 4
PAGE:000000014048217E 49 03 F5                                      add     rsi, r13
PAGE:0000000140482181 48 8B CE                                      mov     rcx, rsi
PAGE:0000000140482184 E8 37 B1 F0 FF                                call    ExReferenceCallBackBlock

*/

bool WDbgArk::FindDbgkLkmdCallbackArray() {
    if ( m_system_ver->GetStrictVer() <= VISTA_SP2_VER ) {
        out << wa::showplus << __FUNCTION__ << ": unsupported Windows version" << endlout;
        return false;
    }

    unsigned __int64 symbol_offset = 0;

    if ( GetSymbolOffset("nt!DbgkLkmdCallbackArray", true, &symbol_offset) )
        return true;

    unsigned __int64 offset = 0;

    if ( !GetSymbolOffset("nt!DbgkLkmdUnregisterCallback", true, &offset) ) {
        err << wa::showminus << __FUNCTION__ << ": can't find nt!DbgkLkmdUnregisterCallback" << endlerr;
        return false;
    }

    std::unique_ptr<WDbgArkUdis> udis(new WDbgArkUdis(0, offset, MAX_INSN_LENGTH * 20));

    if ( !udis->IsInited() ) {
        err << wa::showminus << __FUNCTION__ << ": can't init Udis class" << endlerr;
        return false;
    }

    unsigned __int64 ret_address = 0;

    while ( udis->Disassemble() ) {
        if ( !m_is_cur_machine64 && udis->InstructionLength() == 5 && udis->InstructionMnemonic() == UD_Imov
             &&
             udis->InstructionOperand(0)->type == UD_OP_REG ) {
                 ret_address = static_cast<unsigned __int64>(udis->InstructionOperand(1)->lval.udword);
                 break;
        } else if ( m_is_cur_machine64 && udis->InstructionLength() == 7 && udis->InstructionMnemonic() == UD_Ilea
                    &&
                    udis->InstructionOperand(0)->type == UD_OP_REG ) {
            ret_address = udis->InstructionOffset() + udis->InstructionOperand(1)->lval.sdword +\
                udis->InstructionLength();
            break;
        }
    }

    if ( !ret_address ) {
        err << wa::showminus << __FUNCTION__ << ": disassembly failed" << endlerr;
        return false;
    }

    std::stringstream string_value;
    string_value << std::hex << std::showbase << ret_address;

    try {
        ret_address = g_Ext->EvalExprU64(string_value.str().c_str());
    }
    catch (const ExtStatusException &Ex) {
        err << wa::showminus << __FUNCTION__ << ": " << Ex.GetMessage() << endlerr;
        return false;
    }

    // do not reload nt module after that
    DEBUG_MODULE_AND_ID id;

    HRESULT hresult = m_Symbols3->AddSyntheticSymbol(ret_address,
                                                     m_PtrSize,
                                                     "DbgkLkmdCallbackArray",
                                                     DEBUG_ADDSYNTHSYM_DEFAULT,
                                                     &id);

    if ( !SUCCEEDED(hresult) ) {
        err << wa::showminus << __FUNCTION__ << ": failed to add synthetic symbol DbgkLkmdCallbackArray" << endlerr;
    } else {
        m_synthetic_symbols.push_back(id);
        return true;
    }

    return false;
}

void WDbgArk::RemoveSyntheticSymbols(void) {
    for ( auto id : m_synthetic_symbols ) {
        m_symbols3_iface->RemoveSyntheticSymbol(&id);
    }
}

}   // namespace wa
