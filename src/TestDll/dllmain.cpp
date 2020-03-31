﻿// dllmain.cpp : 定义 DLL 应用程序的入口点。
#include "stdafx.h"
#include <Windows.h>

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH: {
        OutputDebugStringA("TestDll: execute DllMain\n");
        HANDLE hE = CreateEvent(0, 0, 0, 0);
        SetEvent(hE);
        static DWORD idx = GetSysColor(3);
        CloseHandle(hE);
        break;
    }
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

__declspec(dllexport) DWORD Print(int i)
{
    return GetTickCount();
}