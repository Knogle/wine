/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Read-only NT path regression probe. JSONL records describe observations,
 * not Windows-conformance verdicts; compare identical builds on both systems.
 * NtOpenFile cannot create files, and every file handle requests read access.
 * File identity and PE headers distinguish alias resolution from WOW64 mapping.
 */
#include <windows.h>
#include <winternl.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#ifndef OBJ_DONT_REPARSE
#define OBJ_DONT_REPARSE 0x1000
#endif
#ifndef OBJ_OPENLINK
#define OBJ_OPENLINK 0x0100
#endif
#ifndef FILE_OPEN_REPARSE_POINT
#define FILE_OPEN_REPARSE_POINT 0x00200000
#endif

typedef NTSTATUS (NTAPI *open_file_fn)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                                      PIO_STATUS_BLOCK, ULONG, ULONG);
typedef NTSTATUS (NTAPI *query_attributes_fn)(const OBJECT_ATTRIBUTES *, FILE_BASIC_INFORMATION *);
typedef NTSTATUS (NTAPI *query_full_attributes_fn)(const OBJECT_ATTRIBUTES *, FILE_NETWORK_OPEN_INFORMATION *);
typedef NTSTATUS (NTAPI *open_directory_fn)(PHANDLE, ACCESS_MASK, const OBJECT_ATTRIBUTES *);
typedef BOOL (WINAPI *disable_redirection_fn)(PVOID *);
typedef BOOL (WINAPI *revert_redirection_fn)(PVOID);

struct observation
{
    BOOL identity_valid;
    BY_HANDLE_FILE_INFORMATION identity;
    BOOL pe_valid;
    WORD machine;
    WORD optional_magic;
};

static open_file_fn open_file;
static query_attributes_fn query_attributes;
static query_full_attributes_fn query_full_attributes;
static open_directory_fn open_directory;
static const char *phase = "default";
static WCHAR dos_root[MAX_PATH + 8];
static WCHAR dos_system32[MAX_PATH + 40];
static WCHAR dos_syswow64[MAX_PATH + 40];

static unsigned long status_hex(NTSTATUS status)
{
    return (unsigned long)(uint32_t)status;
}

static void make_attributes(OBJECT_ATTRIBUTES *attributes, UNICODE_STRING *name,
                            const WCHAR *path, USHORT length, HANDLE root, ULONG flags)
{
    name->Buffer = (WCHAR *)path;
    name->Length = length;
    /* Deliberately exclude any terminator: NT names are byte-length delimited. */
    name->MaximumLength = length;
    ZeroMemory(attributes, sizeof(*attributes));
    attributes->Length = sizeof(*attributes);
    attributes->RootDirectory = root;
    attributes->ObjectName = name;
    attributes->Attributes = flags;
}

static USHORT name_length(const WCHAR *path)
{
    return (USHORT)(wcslen(path) * sizeof(WCHAR));
}

static void inspect_file(HANDLE file, struct observation *observation)
{
    BYTE dos[64], pe[26];
    DWORD count = 0, offset, error;
    LARGE_INTEGER position;
    BOOL ok;

    observation->identity_valid = GetFileInformationByHandle(file, &observation->identity);
    if (observation->identity_valid)
        printf(",\"volume\":\"%08lx\",\"file_id\":\"%08lx%08lx\",\"size\":\"%08lx%08lx\"",
               (unsigned long)observation->identity.dwVolumeSerialNumber,
               (unsigned long)observation->identity.nFileIndexHigh,
               (unsigned long)observation->identity.nFileIndexLow,
               (unsigned long)observation->identity.nFileSizeHigh,
               (unsigned long)observation->identity.nFileSizeLow);
    else
        printf(",\"identity_error\":%lu", (unsigned long)GetLastError());

    ok = ReadFile(file, dos, sizeof(dos), &count, NULL);
    error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok || count != sizeof(dos) || dos[0] != 'M' || dos[1] != 'Z')
    {
        printf(",\"dos_read_ok\":%d,\"dos_bytes\":%lu", ok, (unsigned long)count);
        if (!ok) printf(",\"dos_error\":%lu", (unsigned long)error);
        return;
    }
    CopyMemory(&offset, dos + 60, sizeof(offset));
    /* This is a header probe, not a parser for arbitrary large file offsets. */
    if (offset > 0x1000000)
    {
        printf(",\"pe_offset_rejected\":%lu", (unsigned long)offset);
        return;
    }
    position.QuadPart = offset;
    if (!SetFilePointerEx(file, position, NULL, FILE_BEGIN))
    {
        printf(",\"seek_error\":%lu", (unsigned long)GetLastError());
        return;
    }
    ok = ReadFile(file, pe, sizeof(pe), &count, NULL);
    error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok || count != sizeof(pe) || pe[0] != 'P' || pe[1] != 'E' || pe[2] || pe[3])
    {
        printf(",\"pe_read_ok\":%d,\"pe_bytes\":%lu", ok, (unsigned long)count);
        if (!ok) printf(",\"pe_error\":%lu", (unsigned long)error);
        return;
    }
    observation->pe_valid = TRUE;
    observation->machine = pe[4] | (pe[5] << 8);
    observation->optional_magic = pe[24] | (pe[25] << 8);
    printf(",\"machine\":\"%04x\",\"optional_magic\":\"%04x\"",
           observation->machine, observation->optional_magic);
}

static struct observation file_case(const char *id, const WCHAR *path, USHORT length,
                                    HANDLE root, ULONG flags, ULONG extra_options)
{
    struct observation observation;
    OBJECT_ATTRIBUTES attributes;
    UNICODE_STRING name;
    IO_STATUS_BLOCK io;
    FILE_BASIC_INFORMATION basic;
    FILE_NETWORK_OPEN_INFORMATION full;
    HANDLE file = NULL;
    NTSTATUS status;

    ZeroMemory(&observation, sizeof(observation));
    make_attributes(&attributes, &name, path, length, root, flags);
    ZeroMemory(&io, sizeof(io));
    io.Status = (NTSTATUS)0xdeadbeef;
    /* Synchronous, non-directory, existing file; never request write/delete. */
    status = open_file(&file, GENERIC_READ | SYNCHRONIZE, &attributes, &io,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      0x20 | 0x40 | extra_options);
    printf("{\"phase\":\"%s\",\"case\":\"%s\",\"api\":\"NtOpenFile\","
           "\"length\":%u,\"root\":%d,\"oa\":\"%08lx\",\"options\":\"%08lx\","
           "\"status\":\"%08lx\",\"io_status\":\"%08lx\",\"information\":%llu",
           phase, id, length, root != NULL, (unsigned long)flags,
           (unsigned long)(0x60 | extra_options), status_hex(status), status_hex(io.Status),
           (unsigned long long)io.Information);
    if (status >= 0)
    {
        inspect_file(file, &observation);
        CloseHandle(file);
    }
    puts("}");

    ZeroMemory(&basic, sizeof(basic));
    status = query_attributes(&attributes, &basic);
    printf("{\"phase\":\"%s\",\"case\":\"%s\",\"api\":\"NtQueryAttributesFile\","
           "\"status\":\"%08lx\"", phase, id, status_hex(status));
    if (status >= 0) printf(",\"attributes\":\"%08lx\"", (unsigned long)basic.FileAttributes);
    puts("}");

    ZeroMemory(&full, sizeof(full));
    status = query_full_attributes(&attributes, &full);
    printf("{\"phase\":\"%s\",\"case\":\"%s\",\"api\":\"NtQueryFullAttributesFile\","
           "\"status\":\"%08lx\"", phase, id, status_hex(status));
    if (status >= 0)
        printf(",\"attributes\":\"%08lx\",\"size\":%llu", (unsigned long)full.FileAttributes,
               (unsigned long long)full.EndOfFile.QuadPart);
    puts("}");
    return observation;
}

static void compare(const char *left_id, const struct observation *left,
                    const char *right_id, const struct observation *right)
{
    printf("{\"phase\":\"%s\",\"api\":\"compare\",\"left\":\"%s\",\"right\":\"%s\"",
           phase, left_id, right_id);
    if (left->identity_valid && right->identity_valid)
        printf(",\"same_file\":%d",
               left->identity.dwVolumeSerialNumber == right->identity.dwVolumeSerialNumber &&
               left->identity.nFileIndexHigh == right->identity.nFileIndexHigh &&
               left->identity.nFileIndexLow == right->identity.nFileIndexLow);
    else printf(",\"same_file\":null");
    if (left->pe_valid && right->pe_valid)
        printf(",\"same_pe_arch\":%d", left->machine == right->machine &&
               left->optional_magic == right->optional_magic);
    else printf(",\"same_pe_arch\":null");
    puts("}");
}

static void relative_file_cases(const char *root_id, const WCHAR *root_path)
{
    OBJECT_ATTRIBUTES attributes;
    UNICODE_STRING name;
    IO_STATUS_BLOCK io;
    HANDLE root = NULL;
    NTSTATUS status;
    char id[80];
    const WCHAR *relative = L"System32\\ntdll.dll";

    make_attributes(&attributes, &name, root_path, name_length(root_path), NULL, 0x40);
    ZeroMemory(&io, sizeof(io));
    status = open_file(&root, FILE_LIST_DIRECTORY | SYNCHRONIZE, &attributes, &io,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0x21);
    printf("{\"phase\":\"%s\",\"case\":\"%s\",\"api\":\"NtOpenFile-directory\","
           "\"status\":\"%08lx\"}\n", phase, root_id, status_hex(status));
    if (status < 0) return;
    snprintf(id, sizeof(id), "%s-relative", root_id);
    file_case(id, relative, name_length(relative), root, 0x40, 0);
    CloseHandle(root);
}

static void object_root_case(void)
{
    OBJECT_ATTRIBUTES attributes;
    UNICODE_STRING name;
    HANDLE root = NULL;
    NTSTATUS status;
    const WCHAR *relative = L"SystemRoot\\System32\\ntdll.dll";

    if (!open_directory)
    {
        printf("{\"phase\":\"%s\",\"api\":\"NtOpenDirectoryObject\",\"skip\":\"unavailable\"}\n", phase);
        return;
    }
    make_attributes(&attributes, &name, L"\\", sizeof(WCHAR), NULL, 0x40);
    /* DIRECTORY_TRAVERSE opens the NT object directory, not a filesystem path. */
    status = open_directory(&root, 2, &attributes);
    printf("{\"phase\":\"%s\",\"case\":\"object-root\",\"api\":\"NtOpenDirectoryObject\","
           "\"status\":\"%08lx\"}\n", phase, status_hex(status));
    if (status < 0) return;
    file_case("object-root-relative", relative, name_length(relative), root, 0x40, 0);
    CloseHandle(root);
}

static void run_cases(void)
{
    static const struct { const char *id; const WCHAR *path; } cases[] = {
        {"alias-system32", L"\\SystemRoot\\System32\\ntdll.dll"},
        {"alias-syswow64", L"\\SystemRoot\\SysWOW64\\ntdll.dll"},
        {"lowercase-alias", L"\\systemroot\\System32\\ntdll.dll"},
        {"prefix-boundary-x", L"\\SystemRootX\\System32\\ntdll.dll"},
        {"prefix-boundary-32", L"\\SystemRoot32\\System32\\ntdll.dll"},
        {"missing-leaf", L"\\SystemRoot\\System32\\systemroot-probe-absent-7e37ec61.dll"},
        {"missing-intermediate", L"\\SystemRoot\\systemroot-probe-absent-7e37ec61\\ntdll.dll"}
    };
    static const struct { const char *id; ULONG attributes; ULONG options; } flags[] = {
        {"case-sensitive", 0, 0},
        {"dont-reparse", 0x40 | OBJ_DONT_REPARSE, 0},
        {"openlink", 0x40 | OBJ_OPENLINK, 0},
        {"file-open-reparse-point", 0x40, FILE_OPEN_REPARSE_POINT}
    };
    struct observation alias32, aliaswow, dos32, doswow, value;
    WCHAR unterminated[128];
    unsigned i;

    ZeroMemory(&alias32, sizeof(alias32));
    ZeroMemory(&aliaswow, sizeof(aliaswow));
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        value = file_case(cases[i].id, cases[i].path, name_length(cases[i].path), NULL, 0x40, 0);
        if (!i) alias32 = value;
        if (i == 1) aliaswow = value;
    }
    dos32 = file_case("dos-system32", dos_system32, name_length(dos_system32), NULL, 0x40, 0);
    doswow = file_case("dos-syswow64", dos_syswow64, name_length(dos_syswow64), NULL, 0x40, 0);
    compare("alias-system32", &alias32, "dos-system32", &dos32);
    compare("alias-syswow64", &aliaswow, "dos-syswow64", &doswow);
    /* Sentinel characters after Length must not become part of the NT name. */
    for (i = 0; i < sizeof(unterminated) / sizeof(unterminated[0]); ++i) unterminated[i] = L'X';
    CopyMemory(unterminated, cases[0].path, name_length(cases[0].path));
    file_case("length-delimited-no-nul", unterminated, name_length(cases[0].path), NULL, 0x40, 0);
    for (i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i)
    {
        char id[80];
        snprintf(id, sizeof(id), "alias-%s", flags[i].id);
        file_case(id, cases[0].path, name_length(cases[0].path), NULL, flags[i].attributes, flags[i].options);
        snprintf(id, sizeof(id), "dos-%s", flags[i].id);
        file_case(id, dos_system32, name_length(dos_system32), NULL, flags[i].attributes, flags[i].options);
    }
    relative_file_cases("alias-file-root", L"\\SystemRoot");
    relative_file_cases("dos-file-root", dos_root);
    object_root_case();
}

int main(void)
{
    HMODULE nt = GetModuleHandleW(L"ntdll.dll"), kernel = GetModuleHandleW(L"kernel32.dll");
    disable_redirection_fn disable = (void *)GetProcAddress(kernel, "Wow64DisableWow64FsRedirection");
    revert_redirection_fn revert = (void *)GetProcAddress(kernel, "Wow64RevertWow64FsRedirection");
    WCHAR windows_directory[MAX_PATH];
    SYSTEM_INFO info;
    PVOID previous = NULL;
    BOOL wow = FALSE;
    UINT length;

    open_file = (void *)GetProcAddress(nt, "NtOpenFile");
    query_attributes = (void *)GetProcAddress(nt, "NtQueryAttributesFile");
    query_full_attributes = (void *)GetProcAddress(nt, "NtQueryFullAttributesFile");
    open_directory = (void *)GetProcAddress(nt, "NtOpenDirectoryObject");
    if (!open_file || !query_attributes || !query_full_attributes)
    {
        puts("{\"error\":\"required-native-api-unavailable\"}");
        return 2;
    }
    length = GetWindowsDirectoryW(windows_directory, MAX_PATH);
    if (!length || length >= MAX_PATH)
    {
        puts("{\"error\":\"windows-directory-unavailable-or-too-long\"}");
        return 2;
    }
    wcscpy(dos_root, L"\\??\\");
    wcscat(dos_root, windows_directory);
    wcscpy(dos_system32, dos_root);
    wcscat(dos_system32, L"\\System32\\ntdll.dll");
    wcscpy(dos_syswow64, dos_root);
    wcscat(dos_syswow64, L"\\SysWOW64\\ntdll.dll");
    GetNativeSystemInfo(&info);
    if (!IsWow64Process(GetCurrentProcess(), &wow))
    {
        printf("{\"error\":\"IsWow64Process-failed\",\"win32_error\":%lu}\n", (unsigned long)GetLastError());
        return 2;
    }
    printf("{\"probe\":\"systemroot-regression-v1\",\"pointer_bits\":%u,\"native_arch\":%u,\"wow64\":%d}\n",
           (unsigned)(sizeof(void *) * 8), (unsigned)info.wProcessorArchitecture, wow);
    run_cases();
    if (wow && disable && revert)
    {
        if (disable(&previous))
        {
            BOOL restored;
            phase = "redirection-disabled";
            run_cases();
            restored = revert(previous);
            printf("{\"api\":\"Wow64RevertWow64FsRedirection\",\"ok\":%d}\n", restored);
            if (!restored) return 3;
        }
        else printf("{\"api\":\"Wow64DisableWow64FsRedirection\",\"error\":%lu}\n", (unsigned long)GetLastError());
    }
    else printf("{\"phase\":\"redirection-disabled\",\"skip\":\"not-wow64-or-api-unavailable\"}\n");
    /* Zero means collection completed, not that every observed path succeeded. */
    puts("{\"collection_complete\":true,\"conformance_verdict\":null}");
    return 0;
}
