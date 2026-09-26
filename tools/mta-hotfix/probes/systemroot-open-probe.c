/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Reproduce MTA's read-only ntdll lookup without loading its private code.
 * The explicit NT paths distinguish object-manager path handling from a
 * missing file, while identical open parameters make Wine/Windows comparable.
 */
#include <windows.h>
#include <winternl.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

typedef NTSTATUS (NTAPI *create_file_fn)(PHANDLE, ACCESS_MASK,
    POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG,
    ULONG, ULONG, ULONG, PVOID, ULONG);

static int probe(create_file_fn create_file, const WCHAR *path)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attrs;
    IO_STATUS_BLOCK io;
    HANDLE file = NULL;
    NTSTATUS status;
    BYTE magic[2] = {0};
    DWORD count = 0;
    BOOL read_ok;

    name.Length = (USHORT)(wcslen(path) * sizeof(WCHAR));
    name.MaximumLength = name.Length + sizeof(WCHAR);
    name.Buffer = (WCHAR *)path;
    ZeroMemory(&attrs, sizeof(attrs));
    attrs.Length = sizeof(attrs);
    attrs.ObjectName = &name;
    attrs.Attributes = 0x40; /* OBJ_CASE_INSENSITIVE, as in the MTA trace. */
    ZeroMemory(&io, sizeof(io));
    io.Status = (NTSTATUS)0xdeadbeef;

    /* FILE_OPEN never creates a file; access is read-only plus synchronize. */
    status = create_file(&file, 0x80100000, &attrs, &io, NULL, FILE_ATTRIBUTE_NORMAL,
                         1, 1, 0x60, NULL, 0);
    printf("path=%ls status=%08lx io_status=%08lx information=%lu",
           path, (unsigned long)(uint32_t)status,
           (unsigned long)(uint32_t)io.Status, (unsigned long)io.Information);
    if (status >= 0)
    {
        /* Reading only the DOS magic verifies this is an accessible image. */
        read_ok = ReadFile(file, magic, sizeof(magic), &count, NULL);
        printf(" read_ok=%d bytes=%lu magic=%02x%02x", read_ok,
               (unsigned long)count, (unsigned)magic[0], (unsigned)magic[1]);
        CloseHandle(file);
    }
    putchar('\n');
    return status >= 0;
}

int main(void)
{
    static const WCHAR *const paths[] = {
        L"\\SystemRoot\\SysWOW64\\ntdll.dll",
        L"\\SystemRoot\\System32\\ntdll.dll",
        L"\\??\\C:\\windows\\system32\\ntdll.dll",
        L"\\??\\C:\\windows\\syswow64\\ntdll.dll"
    };
    create_file_fn create_file;
    SYSTEM_INFO info;
    unsigned int i;

    /* The native signature is known; avoid casting FARPROC directly between
     * incompatible function types on x64 MinGW. */
    create_file = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                               "NtCreateFile");
    if (!create_file)
    {
        puts("NtCreateFile unavailable");
        return 1;
    }
    GetNativeSystemInfo(&info);
    printf("systemroot-open-probe pointer_bits=%u native_arch=%u\n",
           (unsigned)(sizeof(void *) * 8), (unsigned)info.wProcessorArchitecture);
    puts("desired_access=80100000 file_attributes=80 share_access=1 disposition=1 options=60 oa_attributes=40 root_directory=NULL");
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i)
        probe(create_file, paths[i]);
    return 0;
}
