﻿
#include <array>
#include <list>
#include <map>
#include <vector>
#include <Windows.h>
#include "def.h"
#include "allocator.h"
#include "pipemessage.h"


using Allocator::string;

PARAM* g_Param;

class SStream
{
public:
    SStream() { mBuf[0] = '\0'; }
    SStream& operator<<(const char* s)
    {
        if (s)
            strcat_s(mBuf, sizeof(mBuf), s);
        else
            strcat_s(mBuf, sizeof(mBuf), "(null passed)");
        return *this;
    }
    SStream& operator<<(const Allocator::string& s)
    {
        strcat_s(mBuf, sizeof(mBuf), s.c_str());
        return *this;
    }
    SStream& operator<<(int i)
    {
        char buf[32];
        sprintf_s(buf, sizeof(buf), "%d", i);
        strcat_s(mBuf, sizeof(mBuf), buf);
        return *this;
    }
    SStream& operator<<(unsigned long i)
    {
        char buf[32];
        sprintf_s(buf, sizeof(buf), "%u", i);
        strcat_s(mBuf, sizeof(mBuf), buf);
        return *this;
    }
    SStream& operator<<(unsigned __int64 i)
    {
        char buf[32];
        sprintf_s(buf, sizeof(buf), "%llu", i);
        strcat_s(mBuf, sizeof(mBuf), buf);
        return *this;
    }
    SStream& operator<<(void* p)
    {
        char buf[32];
        sprintf_s(buf, sizeof(buf), "0x%p", p);
        strcat_s(mBuf, sizeof(mBuf), buf);
        return *this;
    }

    SStream& operator<<(unsigned int i) { return operator<<(static_cast<unsigned long>(i)); }
    SStream& operator<<(HMODULE i) { return operator<<(static_cast<void*>(i)); }
    const char* str() const { return mBuf; }
private:
    char mBuf[1024];
};

#define PRINT_DEBUG_LOG

#ifdef PRINT_DEBUG_LOG
    #define Vlog(cond) do { \
        PARAM *param = (PARAM*)(LPVOID)PARAM::PARAM_ADDR; \
        if (!param->f_OutputDebugStringA) break; \
        SStream ml; \
        ml << " [" << param->dwProcessId << "." << param->f_GetCurrentThreadId() << "] " << cond << "\n"; \
        param->f_OutputDebugStringA(ml.str()); \
      } while (0)
#else
    #define Vlog(cond) ((void*)0)
#endif

void ProcessCmd(const PipeDefine::Message* msg);

class PipeLine
{
public:
    static PipeLine* msPipe;

    typedef std::vector<char, Allocator::allocator<char>> MsgStream;
    typedef std::list<MsgStream, Allocator::allocator<MsgStream>> ThreadMsgList;
    typedef std::map<DWORD, ThreadMsgList, std::less<DWORD>, Allocator::allocator<std::pair<const DWORD, ThreadMsgList>>> ThreadMsgMap;
    ThreadMsgMap*       mMsgReadBuffer{ nullptr };
    MsgStream*          mMsgWriteBuffer{ nullptr };
    CRITICAL_SECTION    csRead;
    CRITICAL_SECTION    csWrite;
    HANDLE              mPipe{ INVALID_HANDLE_VALUE };
    HANDLE              mReadThreadHandle{ NULL };
    HANDLE              mWriteThreadHandle{ NULL };
    bool                mStopWorkingThread{ false };
    bool                mThreadLockInited{ false };

    struct Lock
    {
        Lock(CRITICAL_SECTION* pcs, bool use)
        {
            PARAM *param = (PARAM*)(LPVOID)PARAM::PARAM_ADDR;
            mUse = use;
            mCS = pcs;
            if (use)
                param->f_RtlEnterCriticalSection(pcs);
        }
        ~Lock()
        {
            PARAM *param = (PARAM*)(LPVOID)PARAM::PARAM_ADDR;
            if (mUse)
                param->f_RtlLeaveCriticalSection(mCS);
        }

        bool mUse{ false };
        CRITICAL_SECTION* mCS{ nullptr };
    };

    PipeLine()
    {
        mMsgReadBuffer = (ThreadMsgMap*)Allocator::Malloc(sizeof(ThreadMsgMap));
        new (mMsgReadBuffer) ThreadMsgMap();
        mMsgWriteBuffer = (MsgStream*)Allocator::Malloc(sizeof(MsgStream));
        new (mMsgWriteBuffer) MsgStream();
        mWriteThreadHandle = NULL;
        mReadThreadHandle = NULL;
    }

    bool ConnectServer()
    {
        PARAM *param = (PARAM*)(LPVOID)PARAM::PARAM_ADDR;
        int busyRetry = 5;
        Vlog("[PipeLine::ConnectServer]");
        while (busyRetry--)
        {
            mPipe = param->f_CreateFileA(PipeDefine::PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
                0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL
            );
            if (mPipe != INVALID_HANDLE_VALUE)
                break;

            if (param->f_GetLastError() != ERROR_PIPE_BUSY)
            {
                Vlog("[PipeLine::ConnectServer] Could not open pipe. GLE: " << param->f_GetLastError());
                return false;
            }

            if (!param->f_WaitNamedPipeA(PipeDefine::PIPE_NAME, NMPWAIT_USE_DEFAULT_WAIT))
            {
                Vlog("[PipeLine::ConnectServer] Could not open pipe: 20 second wait timed out.");
                return false;
            }
        }

        DWORD dwMode = PIPE_READMODE_BYTE;
        if (!param->f_SetNamedPipeHandleState(mPipe, &dwMode, NULL, NULL))
        {
            Vlog("[PipeLine::ConnectServer] SetNamedPipeHandleState failed. GLE: " << param->f_GetLastError());
            return false;
        }

        Vlog("[PipeLine::ConnectServer] connected. " << mPipe);
        return mPipe != INVALID_HANDLE_VALUE;
    }

    bool CreateWorkThread()
    {
        PARAM *param = (PARAM*)(LPVOID)PARAM::PARAM_ADDR;
        param->f_RtlInitializeCriticalSection(&csRead);
        param->f_RtlInitializeCriticalSection(&csWrite);
        mWriteThreadHandle = param->f_CreateThread(0, 0, WriteThread, 0, 0, 0);
        mReadThreadHandle = param->f_CreateThread(0, 0, ReadThread, 0, 0, 0);
        Vlog("[PipeLine::CreateWorkThread] read-thread: " << mReadThreadHandle << ", write-thread: " << mWriteThreadHandle);
        mThreadLockInited = true;
        return true;
    }

    void StopWorkingThread()
    {
        mStopWorkingThread = true;
        PARAM *param = (PARAM*)(LPVOID)PARAM::PARAM_ADDR;
        param->f_CloseHandle(mWriteThreadHandle);
        param->f_CloseHandle(mReadThreadHandle);
        mWriteThreadHandle = NULL;
        mReadThreadHandle = NULL;
    }

    bool Send(PipeDefine::PipeMsg type, const std::vector<char, Allocator::allocator<char>> & content)
    {
        // 尚未建立连接时将数据放在缓冲区中等待发送
        if (mPipe == INVALID_HANDLE_VALUE)
        {
            Vlog("[PipeLine::Send] pipe not ready.");
        }

        PARAM *param = (PARAM*)(LPVOID)PARAM::PARAM_ADDR;
        DWORD dummy = 0;
        std::vector<char, Allocator::allocator<char>> m(PipeDefine::Message::HeaderLength);
        PipeDefine::Message* ptr = (PipeDefine::Message*)m.data();
        ptr->type = type;
        ptr->tid = param->f_GetCurrentThreadId();
        ptr->ContentSize = content.size();
        m.insert(m.end(), content.begin(), content.end());
        Vlog("[PipeLine::Send] enqueue msg type: " << type << ", size: " << m.size());

        {
            Lock lk(&csWrite, mThreadLockInited);
            mMsgWriteBuffer->insert(mMsgWriteBuffer->end(), m.begin(), m.end());
        }

        if (mWriteThreadHandle == NULL || mStopWorkingThread)
        {
            WriteThread((LPVOID)1);
        }
        return true;
    }

    bool Recv(PipeDefine::PipeMsg & type, std::vector<char, Allocator::allocator<char>> & content)
    {
        if (mPipe == INVALID_HANDLE_VALUE)
        {
            Vlog("[PipeLine::Recv] pipe not ready.");
            return false;
        }

        Vlog("[PipeLine::Recv] try to receive a msg...");
        PARAM *param = (PARAM*)(LPVOID)PARAM::PARAM_ADDR;
        const DWORD tid = param->f_GetCurrentThreadId();
        bool wait = true;
        while (wait)
        {
            {
                Lock lk(&csRead, mThreadLockInited);
                ThreadMsgList& msgL = (*mMsgReadBuffer)[tid];
                if (!msgL.empty())
                {
                    MsgStream& m = msgL.front();
                    PipeDefine::Message* ptr = (PipeDefine::Message*)m.data();
                    type = ptr->type;
                    content.assign(ptr->Content, ptr->Content + ptr->ContentSize);
                    msgL.pop_front();
                    wait = false;
                }
            }

            if (wait)
            {
                if (mReadThreadHandle == NULL || mStopWorkingThread)
                {
                    ReadThread((LPVOID)1);
                }
            }
        }
        Vlog("[PipeLine::Recv] msg type: " << type << ", size: " << content.size());
        return true;
    }

    static DWORD WINAPI ReadThread(LPVOID pv)
    {
        PARAM *param = (PARAM*)(LPVOID)PARAM::PARAM_ADDR;
        std::vector<char, Allocator::allocator<char>> tmpBuffer(1024 * 1024);
        size_t partialMsgSize = 0;
        bool runOnce = ((int)pv == 1);
        Vlog("[PipeLine::ReadThread] enter, run once: " << runOnce);
        do
        {
RETRY_READ:
            DWORD bytesRead = 0;
            BOOL ret = param->f_ReadFile(msPipe->mPipe, tmpBuffer.data() + partialMsgSize, tmpBuffer.size() - partialMsgSize, &bytesRead, NULL);
            if (!ret)
            {
                Vlog("[PipeLine::ReadThread] pipe read header failed, err: " << param->f_GetLastError());
                break;
            }
            if (bytesRead == 0)
            {
                if (!runOnce)
                    continue;
                else
                {
                    Vlog("[PipeLine::ReadThread] 0 bytes read, err: " << param->f_GetLastError());
                    goto RETRY_READ;
                }
            }
            partialMsgSize += bytesRead;
            Vlog("[PipeLine::ReadThread] read bytes: " << bytesRead);

            // 解析
            PipeDefine::Message* ptr = (PipeDefine::Message*)tmpBuffer.data();
            while ((intptr_t)ptr - (intptr_t)tmpBuffer.data() < partialMsgSize)
            {
                if (partialMsgSize < PipeDefine::Message::HeaderLength || partialMsgSize < ptr->ContentSize + PipeDefine::Message::HeaderLength)
                {
                    Vlog("[PipeLine::ReadThread] message too short, size: " << tmpBuffer.size() << ", err: " << param->f_GetLastError());
                    break;
                }
                if (ptr->type < 0 || ptr->type >= PipeDefine::Pipe_Msg_Total)
                {
                    Vlog("[PipeLine::ReadThread] error msg type, message may corrupted, type: " << ptr->type << ", body size: " << ptr->ContentSize);
                    break;
                }

                if (ptr->tid != -1)
                {
                    Vlog("[PipeLine::ReadThread] enqueue msg: " << ptr->type << ", length: " << ptr->ContentSize);
                    {
                        Lock lk(&msPipe->csRead, msPipe->mThreadLockInited);
                        ThreadMsgList& msgV = (*msPipe->mMsgReadBuffer)[ptr->tid];
                        msgV.push_back(MsgStream((char*)ptr, (char*)ptr + ptr->ContentSize + PipeDefine::Message::HeaderLength));
                    }
                }
                else
                {
                    // 指令
                    ProcessCmd(ptr);
                }
                const size_t msgSize = ptr->ContentSize + PipeDefine::Message::HeaderLength;
                partialMsgSize -= msgSize;
                ptr = (PipeDefine::Message*)((size_t)ptr + msgSize);
            }
        } while (!runOnce && !msPipe->mStopWorkingThread);
        Vlog("[PipeLine::ReadThread] exit.");
        return 0;
    }

    static DWORD WINAPI WriteThread(LPVOID pv)
    {
        PARAM *param = (PARAM*)(LPVOID)PARAM::PARAM_ADDR;
        bool runOnce = ((int)pv == 1);
        Vlog("[PipeLine::WriteThread] enter, run once: " << runOnce);
        do
        {
            if (msPipe->mPipe == INVALID_HANDLE_VALUE)
            {
                Vlog("[PipeLine::WriteThread] pipe not ready.");
                return 1;
            }

            if (!msPipe->mMsgWriteBuffer->empty())
            {
                {
                    Lock lk(&msPipe->csRead, msPipe->mThreadLockInited);
                    DWORD dummy = 0;
                    BOOL ret = param->f_WriteFile(msPipe->mPipe, msPipe->mMsgWriteBuffer->data(), msPipe->mMsgWriteBuffer->size(), &dummy, NULL);
                    if (!ret)
                        Vlog("[PipeLine::WriteThread] result: " << ret << ", bytes written: " << dummy << ", err: " << param->f_GetLastError());
                    msPipe->mMsgWriteBuffer->clear();
                }
            }
            param->f_Sleep(1);
        } while (!runOnce && !msPipe->mStopWorkingThread);
        Vlog("[PipeLine::WriteThread] exit.");
        return 0;
    }
};
PipeLine* PipeLine::msPipe;

class HookEntries
{
public:
    static HookEntries msEntries;
    struct Entry
    {
        struct Param
        {
            static constexpr UCHAR FLAG_BREAK_NEXT_TIME              = 1;
            static constexpr UCHAR FLAG_BREAK_WHEN_CALL_FROM         = 2;
            static constexpr UCHAR FLAG_BREAK_WHEN_REACH_INVOKE_TIME = 4;
            long     mInvokeCount{ 0 };
            UCHAR    mFlag{ 0 };
            long     mBreakReachInvokeTime{ 0 };
            LONG_PTR mBreakCallFromAddr{ 0 };
        };
        static constexpr size_t ByteCodeLength = 32;
        typedef void (__stdcall * FN_HookFunction)(uint32_t self_index, LONG_PTR call_from);
        uint32_t                mSelfIndex;
        char*                   mBytesCode;
        string                  mModuleName;
        string                  mFuncName;
        Param                   mParams;
        FN_HookFunction         mHookFunction;
        uint64_t                mOriginalVA;

        Entry() { mBytesCode = (char*)Allocator::MallocExe(ByteCodeLength); }

        void Reset(uint32_t index)
        {
            mHookFunction = CommonHookFunction;
            mSelfIndex = index;
            memset(mBytesCode, 0, ByteCodeLength);
            mModuleName.clear();
            mFuncName.clear();
        }
    };
    static void __stdcall CommonHookFunction(uint32_t self_index, LONG_PTR call_from)
    {
        Entry* e = msEntries.GetEntry(self_index);
        Vlog("[HookEntries::CommonHookFunction] entry: " << e
            << ", func name: " << (e ? e->mFuncName.c_str() : "<idx error>")
            << ", call from: " << (LPVOID)call_from << ", invoke time: " << (e ? e->mParams.mInvokeCount : -1) << ", flag: " << (e ? e->mParams.mFlag : -1));
        
        if (e)
        {
            _InlineInterlockedAdd(&e->mParams.mInvokeCount, 1);
            PARAM *param = (PARAM*)(LPVOID)PARAM::PARAM_ADDR;
            PipeDefine::msg::ApiInvoked msgApiInvoke;
            msgApiInvoke.module_name = e->mModuleName.c_str();
            msgApiInvoke.api_name = e->mFuncName.c_str();
            msgApiInvoke.times = e->mParams.mInvokeCount;
            msgApiInvoke.call_from = call_from;
            auto content = msgApiInvoke.Serial();
            PipeLine::msPipe->Send(PipeDefine::Pipe_C_Req_ApiInvoked, content);

            if (e->mFuncName == "ExitProcess")
            {
                // 临近退出，线程不再调度
                PipeLine::msPipe->mStopWorkingThread = true;
            }

            if (e->mParams.mFlag & Entry::Param::FLAG_BREAK_NEXT_TIME)
            {
                e->mParams.mFlag &= ~Entry::Param::FLAG_BREAK_NEXT_TIME;
                Vlog("[CommonHookFunction] int 3(next)");
                __asm int 3
            }
            if ((e->mParams.mFlag & Entry::Param::FLAG_BREAK_WHEN_CALL_FROM) && call_from == e->mParams.mBreakCallFromAddr)
            {
                e->mParams.mFlag &= ~Entry::Param::FLAG_BREAK_WHEN_CALL_FROM;
                Vlog("[CommonHookFunction] int 3(addr)");
                __asm int 3
            }
            if ((e->mParams.mFlag & Entry::Param::FLAG_BREAK_WHEN_REACH_INVOKE_TIME) && e->mParams.mInvokeCount == e->mParams.mBreakReachInvokeTime + 1) // 从 0 计数
            {
                e->mParams.mFlag &= ~Entry::Param::FLAG_BREAK_WHEN_REACH_INVOKE_TIME;
                Vlog("[CommonHookFunction] int 3(time)");
                __asm int 3
            }
        }
    }
    static NTSTATUS __stdcall LdrLoadDllHookFunction(PWCHAR PathToFile, ULONG Flags, PUNICODE_STRING ModuleFileName, PHANDLE ModuleHandle)
    {
        PARAM *param = (PARAM*)(LPVOID)PARAM::PARAM_ADDR;
        NTSTATUS s = param->f_LdrLoadDll(PathToFile, Flags, ModuleFileName, ModuleHandle);
        wchar_t wbuf[128] = { 0 };
        wcsncpy_s(wbuf, _countof(wbuf), ModuleFileName->Buffer, min(ModuleFileName->Length, _countof(wbuf) - 1));
        wbuf[min(ModuleFileName->Length, _countof(wbuf) - 1)] = L'\0';
        Vlog("[HookEntries::LdrLoadDllHookFunction] Name: " << wbuf << ", Handle: " << ModuleHandle);
        return s;
    }

    Entry* AddEntry()
    {
        Entry* ret = (Entry*)Allocator::Malloc(sizeof(Entry));
        ret->Entry::Entry();
        ret->Reset(mEntryArray.size());
        mEntryArray.push_back(ret);
        Vlog("[HookEntries::AddEntry] entry: " << ret << ", added count: " << mEntryArray.size());
        return ret;
    }

    Entry* GetEntry(uint32_t i)
    {
        return i < mEntryArray.size() ? mEntryArray[i] : nullptr;
    }

private:
    std::vector<Entry*, Allocator::allocator<Entry*>> mEntryArray;
};
HookEntries HookEntries::msEntries;


bool IsMemoryReadable(LPVOID addr)
{
    __try
    {
        char c = *(char*)addr;
        return true;
    }
    __except (1)
    {
        return false;
    }
}


void ProcessCmd(const PipeDefine::Message* msg)
{
    Vlog("[ProcessCmd] server cmd: " << msg->type);
    switch (msg->type)
    {
    case PipeDefine::Pipe_S_Req_SuspendProcess:
        break;

    case PipeDefine::Pipe_S_Req_ResumeProcess:
        break;

    case PipeDefine::Pipe_S_Req_SetBreakCondition: {
        PipeDefine::msg::SetBreakCondition sbc;
        PipeLine::MsgStream content(msg->Content, msg->Content + msg->ContentSize);
        sbc.Unserial(content);
        for (size_t i = 0; ; ++i)
        {
            HookEntries::Entry* e = HookEntries::msEntries.GetEntry(i);
            if (!e)
                break;
            if (e->mOriginalVA == sbc.func_addr)
            {
                Vlog("[ProcessCmd] found func va: " << e->mOriginalVA << "entry: " << e);
                if (sbc.break_call_from)
                {
                    e->mParams.mBreakCallFromAddr = sbc.call_from;
                    e->mParams.mFlag |= HookEntries::Entry::Param::FLAG_BREAK_WHEN_CALL_FROM;
                }
                if (sbc.break_invoke_time)
                {
                    e->mParams.mBreakReachInvokeTime = sbc.invoke_time;
                    e->mParams.mFlag |= HookEntries::Entry::Param::FLAG_BREAK_WHEN_REACH_INVOKE_TIME;
                }
                if (sbc.break_next_time)
                {
                    e->mParams.mFlag |= HookEntries::Entry::Param::FLAG_BREAK_NEXT_TIME;
                }
                break;
            }
        }
        break;
    }
    }
}


ULONG_PTR AddHookRoutine(const char* modname, HMODULE hmod, PVOID oldEntry, PVOID oldRvaPtr, const char* funcName)
{
    Vlog("[AddHookRoutine] module: " << hmod << ", name: " << funcName << ", entry: " << oldEntry << ", rva: " << (LPVOID)*(PDWORD)oldRvaPtr);
    HookEntries::Entry* e = HookEntries::msEntries.AddEntry();
    if (!e)
    {
        Vlog("[AddHookRoutine] add new entry failed, skip");
        return 0;
    }
    e->mModuleName = modname;
    e->mFuncName = funcName;
    e->mOriginalVA = (ULONG_PTR)hmod + (ULONG_PTR)*(PDWORD)oldRvaPtr;    

    if (strcmp(funcName, "LdrLoadDll"))
    {
        //
        // push edx
        // push ecx
        // push dword ptr [esp+8]   ; <--- original return addr as call from addr
        // push entry_index
        // push continue_offset     ; <--- new return addr
        // push hook_func
        // ret
        // pop ecx                  ; <--- here continue_offset
        // pop edx
        // push original_func
        // ret
        //

        e->mBytesCode[0] = '\x52';
        e->mBytesCode[1] = '\x51';
        e->mBytesCode[2] = '\xff';
        e->mBytesCode[3] = '\x74';
        e->mBytesCode[4] = '\x24';
        e->mBytesCode[5] = '\x08';
        e->mBytesCode[6] = '\x68';
        *(ULONG_PTR*)&e->mBytesCode[7] = (ULONG_PTR)e->mSelfIndex;
        e->mBytesCode[11] = '\x68';
        *(ULONG_PTR*)&e->mBytesCode[12] = (ULONG_PTR)&e->mBytesCode[22];
        e->mBytesCode[16] = '\x68';
        *(ULONG_PTR*)&e->mBytesCode[17] = (ULONG_PTR)e->mHookFunction;
        e->mBytesCode[21] = '\xc3';
        e->mBytesCode[22] = '\x59';
        e->mBytesCode[23] = '\x5a';
        e->mBytesCode[24] = '\x68';
        *(ULONG_PTR*)&e->mBytesCode[25] = (ULONG_PTR)oldEntry;
        e->mBytesCode[29] = '\xc3';
        e->mBytesCode[30] = '\xcc';
    }
    else
    {
        // for LdrLoadDll

        //
        // push <LdrLoadDllHookFunction>
        // ret
        //
        Vlog("[AddHookRoutine] handle for LdrLoadDll");
        e->mBytesCode[0] = '\x68';
        *(ULONG_PTR*)&e->mBytesCode[1] = (ULONG_PTR)HookEntries::LdrLoadDllHookFunction;
        e->mBytesCode[5] = '\xcc';
    }

    ULONG_PTR newRva = (ULONG_PTR)e->mBytesCode - (ULONG_PTR)hmod;
    Vlog("[AddHookRoutine] finish, new rva: " << (PVOID)newRva);
    return newRva;
}

void HookModuleExportTable(HMODULE hmod, const char* modname, const char* modpath)
{
    PARAM *param = (PARAM*)(LPVOID)PARAM::PARAM_ADDR;

    Vlog("[HookModuleExportTable] enter, hmod: " << (LPVOID)hmod);
    const char* lpImage = (const char*)hmod;
    PIMAGE_DOS_HEADER imDH = (PIMAGE_DOS_HEADER)lpImage;
    PIMAGE_NT_HEADERS imNH = (PIMAGE_NT_HEADERS)((char*)lpImage + imDH->e_lfanew);
    DWORD exportRVA = imNH->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    PIMAGE_EXPORT_DIRECTORY imED = (PIMAGE_EXPORT_DIRECTORY)(lpImage + exportRVA);
    long pExportSize = imNH->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
    if (pExportSize == 0 || (ULONG_PTR)imED <= (ULONG_PTR)lpImage)
    {
        Vlog("[HookModuleExportTable] export table not exist.");
        return;
    }

    // 创建虚拟导出表
    Vlog("[HookModuleExportTable] create new export table");
    __declspec(align(16)) SIZE_T RegionSize = pExportSize;
    __declspec(align(16)) LPVOID BaseAddress = 0;
    param->f_NtAllocateVirtualMemory(NtCurrentProcess(), &BaseAddress, 0, &RegionSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);

    PIMAGE_EXPORT_DIRECTORY imEDNew = (PIMAGE_EXPORT_DIRECTORY)BaseAddress;
    memcpy_s(imEDNew, pExportSize, imED, pExportSize);
    DWORD delta = (char*)imEDNew - (char*)imED;
    imEDNew->AddressOfFunctions    += delta;
    imEDNew->AddressOfNames        += delta;
    imEDNew->AddressOfNameOrdinals += delta;
    PDWORD oldFunc = (PDWORD)(lpImage + imED->AddressOfFunctions);
    PDWORD newFunc = (PDWORD)(lpImage + imEDNew->AddressOfFunctions);
    for (DWORD i = 0; i < imED->NumberOfFunctions; ++i)
    {
        newFunc[i] += delta;
    }

    __declspec(align(16)) DWORD  oldProtect1 = 0;
    __declspec(align(16)) LPVOID baseAddr1 = oldFunc;
    __declspec(align(16)) ULONG  sizeToProtect1 = imEDNew->NumberOfFunctions * sizeof(DWORD);
    param->f_NtProtectVirtualMemory(NtCurrentProcess(), &baseAddr1, &sizeToProtect1, PAGE_READWRITE, &oldProtect1);

    Vlog("[HookModuleExportTable] replace exdisting export table, count: " << imED->NumberOfFunctions);
    char tmpFuncNameBuffer[32] = { 0 };
    PDWORD lpNames    = imED->AddressOfNames ? (PDWORD)(lpImage + imED->AddressOfNames) : 0;
    PWORD  lpOrdinals = imED->AddressOfNameOrdinals ? (PWORD)(lpImage + imED->AddressOfNameOrdinals) : 0;
    for (DWORD i = 0; i < imED->NumberOfFunctions; ++i)
    {
        if (oldFunc[i] >= exportRVA && oldFunc[i] < exportRVA + pExportSize)
        {
            Vlog("[HookModuleExportTable] skip forword function: " << (lpImage + oldFunc[i]));
            continue;
        }

        // 查找名称
        const char* funcName = "";
        for (DWORD k = 0; k < imED->NumberOfNames; ++k)
        {
            if (lpOrdinals[k] == i)
            {
                funcName = lpImage + lpNames[k];
                break;
            }
        }
        if (funcName[0] == '\0')
        {
            sprintf_s(tmpFuncNameBuffer, sizeof(tmpFuncNameBuffer), "<ordinal %d>", i + imED->Base);
            funcName = tmpFuncNameBuffer;
        }

        if (strcmp(funcName, "LdrLoadDll"))
        {
            ULONG_PTR newRva = AddHookRoutine(modname, hmod, (PVOID)(lpImage + oldFunc[i]), &oldFunc[i], funcName);
            if (newRva)
                newFunc[i] = newRva;
            else
                newFunc[i] = oldFunc[i];
        }
        else
        {
            Vlog("[HookModuleExportTable] skip LdrLoadDll");
            newFunc[i] = oldFunc[i];
        }
    }

    __declspec(align(16)) DWORD  oldProtect2 = 0;
    __declspec(align(16)) LPVOID baseAddr2 = &imNH->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    __declspec(align(16)) ULONG  sizeToProtect2 = sizeof(DWORD);
    param->f_NtProtectVirtualMemory(NtCurrentProcess(), &baseAddr2, &sizeToProtect2, PAGE_READWRITE, &oldProtect2);
    imNH->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress = (DWORD)imEDNew - (DWORD)lpImage;
    if (oldProtect2 != 0)
    {
        baseAddr2 = &imNH->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        sizeToProtect2 = sizeof(DWORD);
        param->f_NtProtectVirtualMemory(NtCurrentProcess(), &baseAddr2, &sizeToProtect2, oldProtect2, &oldProtect2);
    }
    Vlog("[HookModuleExportTable] finish");
    if (oldProtect1 != 0)
    {
        LPVOID baseAddr1 = oldFunc;
        ULONG  sizeToProtect1 = imEDNew->NumberOfFunctions * sizeof(DWORD);
        param->f_NtProtectVirtualMemory(NtCurrentProcess(), &baseAddr1, &sizeToProtect1, oldProtect1, &oldProtect1);
    }
}

void CollectModuleInfo(HMODULE hmod, const char* modname, const char* modpath, PipeDefine::msg::ModuleApis& msgModuleApis)
{
    Vlog("[CollectModuleInfo] enter.");

    msgModuleApis.module_name = modname;
    msgModuleApis.module_path = modpath;
    msgModuleApis.module_base = (long long)hmod;

    const char* lpImage = (const char*)hmod;
    PIMAGE_DOS_HEADER imDH = (PIMAGE_DOS_HEADER)lpImage;
    PIMAGE_NT_HEADERS imNH = (PIMAGE_NT_HEADERS)((char*)lpImage + imDH->e_lfanew);
    DWORD exportRVA = imNH->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    PIMAGE_EXPORT_DIRECTORY imED = (PIMAGE_EXPORT_DIRECTORY)(lpImage + exportRVA);
    long pExportSize = imNH->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
    if (pExportSize == 0 || !IsMemoryReadable(imED))
    {
        Vlog("[CollectModuleInfo] export table empty or bad address, size: " << pExportSize << ", va: " << imED);
        return;
    }
    // 存在导出表
    if (imED->NumberOfFunctions <= 0)
    {
        Vlog("[CollectModuleInfo] number of functions <= 0.");
        return;
    }
    PWORD lpOrdinals = imED->AddressOfNameOrdinals ? (PWORD)(lpImage + imED->AddressOfNameOrdinals) : 0;
    PDWORD lpNames = imED->AddressOfNames ? (PDWORD)(lpImage + imED->AddressOfNames) : 0;
    PDWORD lpRvas = (PDWORD)(lpImage + imED->AddressOfFunctions);
    PIMAGE_SECTION_HEADER ish = (PIMAGE_SECTION_HEADER)(imNH + 1);
    int nsec = imNH->FileHeader.NumberOfSections;
    for (DWORD i = 0; i < imED->NumberOfFunctions; ++i)
    {
        PipeDefine::msg::ModuleApis::ApiDetail ad;
        DWORD rvafunc = lpRvas[i];
        DWORD oftName = 0;
        // 找出函数对应的名称
        if (lpNames && lpOrdinals)
        {
            for (DWORD k = 0; k < imED->NumberOfNames; ++k)
            {
                if (lpOrdinals[k] == i)
                {
                    oftName = lpNames[k];
                    break;
                }
            }
        }
        if (oftName)
            Vlog("orgRVA: " << rvafunc << ", apiBase: " << rvafunc << ", apiName: " << (lpImage + oftName));
        else
            Vlog("orgRVA: " << rvafunc << ", apiBase: " << rvafunc << ", apiName: ordinal " << i);

        bool dataExp = false;
        bool forwardApi = false;
        // 判断是否为转向函数导出
        if (!(rvafunc >= exportRVA && rvafunc < (exportRVA + pExportSize)))
        {
            // 如果不是转向函数则遍历整个区段判断是否为数据导出。
            // 由于是通过区段属性判断因此并非完全准确，但大部分情况下是准确的
            BOOL isDataExport = TRUE;
            PIMAGE_SECTION_HEADER ishcur;
            for (int j = 0; j < nsec; ++j)
            {
                ishcur = ish + j;
                if (rvafunc >= ishcur->VirtualAddress && rvafunc < (ishcur->VirtualAddress + ishcur->Misc.VirtualSize))
                {
                    if (ishcur->Characteristics & IMAGE_SCN_MEM_EXECUTE)
                    {
                        isDataExport = FALSE;
                        break;
                    }
                }
            }
            if (isDataExport)
                Vlog("dataApi: -");
            dataExp = !!isDataExport;
        }
        else
        {
            // 是转向函数，设定转向信息
            Vlog("redirectApi: " << lpImage + rvafunc);
            forwardApi = true;
        }

        ad.rva = rvafunc;
        ad.va = rvafunc + (DWORD)lpImage;
        if (oftName)
            ad.name = lpImage + oftName;
        else
        {
            SStream ml;
            ml << i;
            ad.name = string("ordinal ") + ml.str();
        }
        if (forwardApi)
            ad.forwardto = lpImage + rvafunc;
        ad.data_export = dataExp;
        ad.forward_api = forwardApi;
        msgModuleApis.apis.push_back(ad);
    }
    Vlog("[CollectModuleInfo] exit.");
}


void HookLoadedModules()
{
    PARAM *param = (PARAM*)(LPVOID)PARAM::PARAM_ADDR;

    tagMODULEENTRY32 me32;
    HANDLE hModuleSnap = param->f_CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, param->dwProcessId);
    if (hModuleSnap == INVALID_HANDLE_VALUE)
    {
        Vlog("[HookLoadedModules] CreateToolhelp32Snapshot failed.");
        return;
    }

    me32.dwSize = sizeof(tagMODULEENTRY32);
    if (!param->f_Module32First(hModuleSnap, &me32))
    {
        Vlog("[HookLoadedModules] Module32First failed.");
        return;
    }

    do
    {
        Vlog("[HookLoadedModules] process: " << me32.szModule << ", image base: " << (LPVOID)me32.modBaseAddr << ", path: " << me32.szExePath);
        PipeDefine::msg::ModuleApis msgModuleApis;
        CollectModuleInfo((HMODULE)me32.modBaseAddr, me32.szModule, me32.szExePath, msgModuleApis);

        auto content = msgModuleApis.Serial();
        PipeLine::msPipe->Send(PipeDefine::Pipe_C_Req_ModuleApiList, content);
        PipeDefine::PipeMsg recv_type;
        content.clear();
        PipeLine::msPipe->Recv(recv_type, content);
        PipeDefine::msg::ApiFilter filter;
        filter.Unserial(content);
        Vlog("[HookLoadedModules] reply filter count: " << std::count_if(filter.apis.begin(), filter.apis.end(), [](PipeDefine::msg::ApiFilter::Api& a) { return a.filter; }));

        HookModuleExportTable((HMODULE)me32.modBaseAddr, me32.szModule, me32.szExePath);

    } while (param->f_Module32Next(hModuleSnap, &me32));

    param->f_CloseHandle(hModuleSnap);
}

ULONG_PTR MiniGetFunctionAddress(ULONG_PTR phModule, const char* pProcName)
{
    PIMAGE_DOS_HEADER pimDH;
    PIMAGE_NT_HEADERS pimNH;
    PIMAGE_EXPORT_DIRECTORY pimED;
    ULONG_PTR pResult = 0;
    PDWORD pAddressOfNames;
    PWORD  pAddressOfNameOrdinals;
    DWORD i;
    if (!phModule)
        return 0;
    pimDH = (PIMAGE_DOS_HEADER)phModule;
    pimNH = (PIMAGE_NT_HEADERS)((char*)phModule + pimDH->e_lfanew);
    pimED = (PIMAGE_EXPORT_DIRECTORY)(phModule + pimNH->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    if (pimNH->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size == 0 || (ULONG_PTR)pimED <= phModule)
        return 0;
    if ((ULONG_PTR)pProcName < 0x10000)
    {
        if ((ULONG_PTR)pProcName >= pimED->NumberOfFunctions + pimED->Base || (ULONG_PTR)pProcName < pimED->Base)
            return 0;
        pResult = phModule + ((PDWORD)(phModule + pimED->AddressOfFunctions))[(ULONG_PTR)pProcName - pimED->Base];
    }
    else
    {
        pAddressOfNames = (PDWORD)(phModule + pimED->AddressOfNames);
        for (i = 0; i < pimED->NumberOfNames; ++i)
        {
            char* pExportName = (char*)(phModule + pAddressOfNames[i]);
            if (!strcmp(pProcName, pExportName))
            {
                pAddressOfNameOrdinals = (PWORD)(phModule + pimED->AddressOfNameOrdinals);
                pResult = phModule + ((PDWORD)(phModule + pimED->AddressOfFunctions))[pAddressOfNameOrdinals[i]];
                break;
            }
        }
    }
    return pResult;
}

void GetModules()
{
    PARAM *param = (PARAM*)(LPVOID)PARAM::PARAM_ADDR;
    //param->f_LdrLoadDll = (FN_LdrLoadDll)MiniGetFunctionAddress((ULONG_PTR)param->ntdllBase, "LdrLoadDll");
    param->f_LdrInitializeThunk = (FN_LdrInitializeThunk)MiniGetFunctionAddress((ULONG_PTR)param->ntdllBase, "LdrInitializeThunk");
    wchar_t buffer[MAX_PATH] = L"kernelbase.dll";
    UNICODE_STRING name = { 0 };
    name.Length = static_cast<USHORT>(wcslen(buffer) * sizeof(wchar_t));
    name.Buffer = buffer;
    name.MaximumLength = static_cast<USHORT>(sizeof(buffer));
    HANDLE hKernel = 0;
    NTSTATUS status = param->f_LdrLoadDll(0, 0, &name, &hKernel);
    param->kernelBase = (LPVOID)hKernel;
    param->f_GetProcAddress = (FN_GetProcAddress)MiniGetFunctionAddress((ULONG_PTR)hKernel, "GetProcAddress");
}

void GetApis(bool ntdllOnly)
{
    PARAM *param = (PARAM*)(LPVOID)PARAM::PARAM_ADDR;

#define GET_NTDLL_API(fn) do { \
        if (!param->f_ ## fn) \
            param->f_ ## fn = (FN_ ## fn)MiniGetFunctionAddress((ULONG_PTR)param->ntdllBase, # fn); \
        if (!param->f_ ## fn) \
            throw; \
    } while (0)

    GET_NTDLL_API(NtSuspendProcess);
    GET_NTDLL_API(NtResumeProcess);
    GET_NTDLL_API(NtAllocateVirtualMemory);
    GET_NTDLL_API(RtlCreateHeap);
    GET_NTDLL_API(RtlAllocateHeap);
    GET_NTDLL_API(RtlFreeHeap);
    GET_NTDLL_API(NtProtectVirtualMemory);
    GET_NTDLL_API(RtlInitializeCriticalSection);
    GET_NTDLL_API(RtlEnterCriticalSection);
    GET_NTDLL_API(RtlLeaveCriticalSection);

    if (ntdllOnly)
        return;

    if (!param->f_GetProcAddress)
        param->f_GetProcAddress = (FN_GetProcAddress)MiniGetFunctionAddress((ULONG_PTR)param->kernelBase, "GetProcAddress");

    param->f_GetModuleHandleA = (FN_GetModuleHandleA)param->f_GetProcAddress((HMODULE)param->kernelBase, "GetModuleHandleA");
    param->f_OpenThread = (FN_OpenThread)param->f_GetProcAddress((HMODULE)param->kernelBase, "OpenThread");
    param->f_SuspendThread = (FN_SuspendThread)param->f_GetProcAddress((HMODULE)param->kernelBase, "SuspendThread");
    param->f_SetThreadContext = (FN_SetThreadContext)param->f_GetProcAddress((HMODULE)param->kernelBase, "SetThreadContext");
    param->f_ResumeThread = (FN_ResumeThread)param->f_GetProcAddress((HMODULE)param->kernelBase, "ResumeThread");
    param->f_CloseHandle = (FN_CloseHandle)param->f_GetProcAddress((HMODULE)param->kernelBase, "CloseHandle");
    param->f_CreateThread = (FN_CreateThread)param->f_GetProcAddress((HMODULE)param->kernelBase, "CreateThread");
    param->f_OutputDebugStringA = (FN_OutputDebugStringA)param->f_GetProcAddress((HMODULE)param->kernelBase, "OutputDebugStringA");

    param->kernel32 = (LPVOID)param->f_GetModuleHandleA("kernel32.dll");
    param->f_CreateToolhelp32Snapshot = (FN_CreateToolhelp32Snapshot)param->f_GetProcAddress((HMODULE)param->kernel32, "CreateToolhelp32Snapshot");
    param->f_Module32First = (FN_Module32First)param->f_GetProcAddress((HMODULE)param->kernel32, "Module32First");
    param->f_Module32Next = (FN_Module32Next)param->f_GetProcAddress((HMODULE)param->kernel32, "Module32Next");
    param->f_GetProcessHeap = (FN_GetProcessHeap)param->f_GetProcAddress((HMODULE)param->kernel32, "GetProcessHeap");
    param->f_CreateFileA = (FN_CreateFileA)param->f_GetProcAddress((HMODULE)param->kernel32, "CreateFileA");
    param->f_ReadFile = (FN_ReadFile)param->f_GetProcAddress((HMODULE)param->kernel32, "ReadFile");
    param->f_WriteFile = (FN_WriteFile)param->f_GetProcAddress((HMODULE)param->kernel32, "WriteFile");
    param->f_WaitNamedPipeA = (FN_WaitNamedPipeA)param->f_GetProcAddress((HMODULE)param->kernel32, "WaitNamedPipeA");
    param->f_SetNamedPipeHandleState = (FN_SetNamedPipeHandleState)param->f_GetProcAddress((HMODULE)param->kernel32, "SetNamedPipeHandleState");
    param->f_GetLastError = (FN_GetLastError)param->f_GetProcAddress((HMODULE)param->kernel32, "GetLastError");
    param->f_GetCurrentThreadId = (FN_GetCurrentThreadId)param->f_GetProcAddress((HMODULE)param->kernel32, "GetCurrentThreadId");
    param->f_Sleep = (FN_Sleep)param->f_GetProcAddress((HMODULE)param->kernel32, "Sleep");
}

void BuildPipe(bool connect)
{
    Vlog("[BuildPipe] enter. connect: " << connect);
    if (!PipeLine::msPipe)
    {
        PipeLine::msPipe = (PipeLine*)Allocator::Malloc(sizeof(PipeLine));
        new (PipeLine::msPipe) PipeLine();
    }
    if (connect)
        PipeLine::msPipe->ConnectServer();
    Vlog("[BuildPipe] exit.");
}

void DebugMessage()
{
    PARAM *param = (PARAM*)(LPVOID)PARAM::PARAM_ADDR;

    wchar_t buffer[MAX_PATH] = L"User32.dll";
    UNICODE_STRING name = { 0 };
    name.Length = static_cast<USHORT>(wcslen(buffer) * sizeof(wchar_t));
    name.Buffer = buffer;
    name.MaximumLength = static_cast<USHORT>(sizeof(buffer));
    HANDLE hUser32 = 0;
    NTSTATUS status = param->f_LdrLoadDll(0, 0, &name, &hUser32);
    FN_MessageBoxA pMessageBox = (FN_MessageBoxA)param->f_GetProcAddress((HMODULE)hUser32, "MessageBoxA");
    char buf[32];
    sprintf_s(buf, sizeof(buf), "pid: %#x", param->dwProcessId);
    pMessageBox(0, "I'm here", buf, MB_ICONINFORMATION);

    PipeDefine::msg::Init msgInit;
    msgInit.dummy = 0xaa55ccdd;
    auto content = msgInit.Serial();
    PipeLine::msPipe->Send(PipeDefine::Pipe_C_Req_Inited, content);
    PipeDefine::PipeMsg recv_type;
    content.clear();
    PipeLine::msPipe->Recv(recv_type, content);
    msgInit.Unserial(content);
    Vlog("[DebugMessage] dummy: " << (LPVOID)msgInit.dummy);
}

DWORD WINAPI Recover(LPVOID pv)
{
    PARAM *param = (PARAM*)(LPVOID)PARAM::PARAM_ADDR;

    Vlog("[Recover]");

    HANDLE hT = param->f_OpenThread(THREAD_ALL_ACCESS, FALSE, param->dwThreadId);
    param->f_SuspendThread(hT);
    param->f_SetThreadContext(hT, &param->ctx);
    param->f_ResumeThread(hT);
    param->f_CloseHandle(hT);
    return 0;
}

void ContinueExe()
{
    PARAM *param = (PARAM*)(LPVOID)PARAM::PARAM_ADDR;
    param->f_CreateThread(0, 0, Recover, 0, 0, 0);
    while (1) {}
}

NTSTATUS NTAPI HookLdrLoadDllPad(PWCHAR PathToFile, ULONG Flags, PUNICODE_STRING ModuleFileName, PHANDLE ModuleHandle)
{
    PARAM *param = (PARAM*)(LPVOID)PARAM::PARAM_ADDR;
    NTSTATUS ret = param->f_LdrLoadDll(PathToFile, Flags, ModuleFileName, ModuleHandle);

    if (!param->bNtdllInited)
    {
        GetApis(true);
        Allocator::InitAllocator();
        BuildPipe(false);

        PipeDefine::msg::ModuleApis msgModuleApis;
        CollectModuleInfo((HMODULE)param->ntdllBase, "ntdll.dll", "ntdll.dll", msgModuleApis);
        //auto content = msgModuleApis.Serial();
        //PipeLine::msPipe->Send(PipeDefine::Pipe_C_Req_ModuleApiList, content);

        HookModuleExportTable((HMODULE)param->ntdllBase, "ntdll.dll", "ntdll.dll");

        param->bNtdllInited = true;
    }

    if (!param->kernelBase)
    {
        wchar_t buffer[MAX_PATH] = L"kernelbase.dll";
        UNICODE_STRING name = { 0 };
        name.Length = static_cast<USHORT>(wcslen(buffer) * sizeof(wchar_t));
        name.Buffer = buffer;
        name.MaximumLength = static_cast<USHORT>(sizeof(buffer));
        HANDLE hKernel = 0;
        NTSTATUS status = param->f_LdrLoadDll(0, 0, &name, &hKernel);
        param->kernelBase = (LPVOID)hKernel;
    }
    if (!param->kernel32)
    {
        wchar_t buffer[MAX_PATH] = L"kernel32.dll";
        UNICODE_STRING name = { 0 };
        name.Length = static_cast<USHORT>(wcslen(buffer) * sizeof(wchar_t));
        name.Buffer = buffer;
        name.MaximumLength = static_cast<USHORT>(sizeof(buffer));
        HANDLE hKernel = 0;
        NTSTATUS status = param->f_LdrLoadDll(0, 0, &name, &hKernel);
        param->kernel32 = (LPVOID)hKernel;
    }
    if (!param->bOthersInited && param->kernel32 && param->kernelBase)
    {
        GetApis(false);
        BuildPipe(true);
        param->bOthersInited = true;

        const int preLoadCount = 2;
        HMODULE preLoadAddr[preLoadCount] = { (HMODULE)param->kernelBase, (HMODULE)param->kernel32 };
        const char* preLoadName[preLoadCount] = { "kernelbase.dll", "kernel32.dll" };
        for (int i = 0; i < preLoadCount; ++i)
        {
            PipeDefine::msg::ModuleApis msgModuleApis;
            CollectModuleInfo(preLoadAddr[i], preLoadName[i], preLoadName[i], msgModuleApis);
            auto content = msgModuleApis.Serial();
            PipeLine::msPipe->Send(PipeDefine::Pipe_C_Req_ModuleApiList, content);
            PipeDefine::PipeMsg recv_type;
            content.clear();
            PipeLine::msPipe->Recv(recv_type, content);
            PipeDefine::msg::ApiFilter filter;
            filter.Unserial(content);
            Vlog("[HookLdrLoadDllPad] reply filter count: " << std::count_if(filter.apis.begin(), filter.apis.end(), [](PipeDefine::msg::ApiFilter::Api& a) { return a.filter; }));
            HookModuleExportTable(preLoadAddr[i], preLoadName[i], preLoadName[i]);
        }
    }

    Vlog("[HookLdrLoadDllPad] ret value: " << ret << ", base: " << *ModuleHandle);
    // for this new loaded dll
    if (ret == 0 && *ModuleHandle != param->kernel32 && *ModuleHandle != param->kernelBase)
    {
        string moduleName(ModuleFileName->Buffer, ModuleFileName->Buffer + ModuleFileName->Length);
        Vlog("[HookLdrLoadDllPad] dll: " << moduleName.c_str() << ", base: " << *ModuleHandle);
        PipeDefine::msg::ModuleApis msgModuleApis;
        CollectModuleInfo((HMODULE)*ModuleHandle, moduleName.c_str(), moduleName.c_str(), msgModuleApis);

        auto content = msgModuleApis.Serial();
        PipeLine::msPipe->Send(PipeDefine::Pipe_C_Req_ModuleApiList, content);
        PipeDefine::PipeMsg recv_type;
        content.clear();
        PipeLine::msPipe->Recv(recv_type, content);
        PipeDefine::msg::ApiFilter filter;
        filter.Unserial(content);
        Vlog("[HookLdrLoadDllPad] reply filter count: " << std::count_if(filter.apis.begin(), filter.apis.end(), [](PipeDefine::msg::ApiFilter::Api& a) { return a.filter; }));

        HookModuleExportTable((HMODULE)*ModuleHandle, moduleName.c_str(), moduleName.c_str());
    }
    else
    {
        Vlog("[HookLdrLoadDllPad] skip.");
    }
    return ret;
}

void Entry()
{
    PipeLine::msPipe->CreateWorkThread();
    DebugMessage();
    ContinueExe();
}

#pragma optimize("", off)
void Alias(const void* var) {
    if (0) {
        Entry();
    }
}
#pragma optimize("", on)

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID lpReserved)
{
    if (dwReason == DLL_PROCESS_DETACH)
    {
        Alias(0);
    }

    return TRUE;
}

