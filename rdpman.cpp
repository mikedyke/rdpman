// rdpman - small portable RDP session manager (native Win32, no runtime dependencies).
// Sessions are plain .rdp files stored in a "sessions" folder next to the executable.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <uxtheme.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "ole32.lib")

typedef std::wstring ws;

// ---------------------------------------------------------------- globals
static HINSTANCE g_inst;
static HWND g_main, g_list, g_search, g_status;
static HFONT g_font;
static int g_dpi = 96;
static ws g_dataDir, g_iniPath;
static const wchar_t* APP = L"RDP Manager";

static int S(int v) { return MulDiv(v, g_dpi, 96); }

// ---------------------------------------------------------------- helpers
static ws Fmt(const wchar_t* f, ...) {
    wchar_t b[2048]; va_list a; va_start(a, f); _vsnwprintf_s(b, _countof(b), _TRUNCATE, f, a); va_end(a); return b;
}
static ws Lower(ws s) { if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size()); return s; }
static ws Trim(const ws& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"); if (a == ws::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n"); return s.substr(a, b - a + 1);
}
static bool IEq(const ws& a, const ws& b) { return lstrcmpiW(a.c_str(), b.c_str()) == 0; }
static bool IStarts(const ws& s, const wchar_t* p) { size_t n = wcslen(p); return s.size() >= n && _wcsnicmp(s.c_str(), p, n) == 0; }
static std::vector<ws> Split(const ws& s, wchar_t sep) {
    std::vector<ws> v; size_t p = 0;
    for (;;) { size_t q = s.find(sep, p); if (q == ws::npos) { v.push_back(s.substr(p)); break; } v.push_back(s.substr(p, q - p)); p = q + 1; }
    return v;
}
static ws ReplaceAll(ws s, const ws& a, const ws& b) {
    for (size_t p = 0; (p = s.find(a, p)) != ws::npos; p += b.size()) s.replace(p, a.size(), b);
    return s;
}
static ws GetText(HWND h) { int n = GetWindowTextLengthW(h); ws s(n, 0); if (n) GetWindowTextW(h, &s[0], n + 1); return s; }
static bool FileExists(const ws& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static void Msg(HWND o, const ws& t, UINT fl = MB_ICONINFORMATION) { MessageBoxW(o, t.c_str(), APP, fl); }
static std::vector<int> ParseIds(const ws& s) {
    std::vector<int> v;
    for (auto& p : Split(s, L',')) { ws t = Trim(p); if (!t.empty() && iswdigit(t[0])) v.push_back(_wtoi(t.c_str())); }
    return v;
}
static ws JoinIds(const std::vector<int>& v) { ws s; for (int i : v) { if (!s.empty()) s += L","; s += std::to_wstring(i); } return s; }

// ---------------------------------------------------------------- RDP property table
enum Kind { K_NONE, K_CHECK, K_COMBO, K_EDIT };
enum Page { P_NONE = -1, P_GENERAL = 0, P_DISPLAY = 1, P_LOCAL = 2, P_EXP = 3, P_ADV = 4, P_ALL = 5 };
struct PropDef {
    const wchar_t* key; wchar_t type; const wchar_t* def; const wchar_t* cat;
    int page; Kind kind; const wchar_t* label; const wchar_t* choices; const wchar_t* desc;
};
#define YN L"0=No|1=Yes"
static const PropDef g_defs[] = {
    // --- Connection
    {L"full address", 's', L"", L"Connection", P_GENERAL, K_EDIT, L"Computer (host or host:port):", 0, L"Name or IP address of the remote computer"},
    {L"server port", 'i', L"3389", L"Connection", P_GENERAL, K_EDIT, L"Port:", 0, L"TCP port of the remote computer"},
    {L"username", 's', L"", L"Connection", P_GENERAL, K_EDIT, L"User name (DOMAIN\\user or user@domain):", 0, L"User account used to sign in"},
    {L"domain", 's', L"", L"Connection", P_GENERAL, K_EDIT, L"Domain:", 0, L"Domain of the user account"},
    {L"prompt for credentials", 'i', L"0", L"Connection", P_GENERAL, K_CHECK, L"Always prompt for credentials", YN, L"Prompt for credentials even if saved ones exist"},
    {L"administrative session", 'i', L"0", L"Connection", P_GENERAL, K_CHECK, L"Connect to the administrative session (/admin)", YN, L"Connect to the administrative (console) session"},
    {L"password 51", 'b', L"", L"Connection", P_NONE, K_NONE, 0, 0, L"Saved password (DPAPI encrypted, bound to this Windows user and PC)"},
    {L"alternate full address", 's', L"", L"Connection", P_NONE, K_NONE, 0, 0, L"Alternate name or IP address of the remote computer"},
    {L"connect to console", 'i', L"0", L"Connection", P_NONE, K_NONE, 0, YN, L"Legacy: connect to the console session"},
    {L"prompt for credentials on client", 'i', L"0", L"Connection", P_NONE, K_NONE, 0, YN, L"Prompt for credentials on the client when the server does not support server authentication"},
    {L"public mode", 'i', L"0", L"Connection", P_NONE, K_NONE, 0, YN, L"Public mode: do not cache credentials or bitmaps"},
    {L"pcb", 's', L"", L"Connection", P_NONE, K_NONE, 0, 0, L"Pre-connection blob (e.g. Hyper-V VM id)"},
    {L"use redirection server name", 'i', L"0", L"Connection", P_NONE, K_NONE, 0, YN, L"Use the redirection server name"},
    {L"loadbalanceinfo", 's', L"", L"Connection", P_ADV, K_EDIT, L"Load balance info:", 0, L"Load balancing routing token sent to the RD Connection Broker"},
    {L"workspace id", 's', L"", L"Connection", P_NONE, K_NONE, 0, 0, L"RemoteApp and Desktop workspace id"},
    {L"enableworkspacereconnect", 'i', L"0", L"Connection", P_NONE, K_NONE, 0, YN, L"Reconnect to workspace resources"},
    {L"signscope", 's', L"", L"Connection", P_NONE, K_NONE, 0, 0, L"List of signed settings (written by rdpsign)"},
    {L"signature", 's', L"", L"Connection", P_NONE, K_NONE, 0, 0, L"File signature (written by rdpsign)"},
    // --- Security
    {L"authentication level", 'i', L"2", L"Security", P_ADV, K_COMBO, L"If server authentication fails:", L"0=Connect and don't warn me|1=Do not connect|2=Warn me|3=No requirement specified", L"Server authentication level"},
    {L"enablecredsspsupport", 'i', L"1", L"Security", P_ADV, K_CHECK, L"Use CredSSP (Network Level Authentication)", YN, L"Use the Credential Security Support Provider"},
    {L"promptcredentialonce", 'i', L"1", L"Security", P_ADV, K_CHECK, L"Use the same credentials for gateway and PC", YN, L"Use RD Gateway credentials for the remote computer"},
    {L"disableconnectionsharing", 'i', L"0", L"Security", P_ADV, K_CHECK, L"Always start a new connection (no sharing)", YN, L"Do not reconnect to an existing disconnected session / share connections"},
    {L"enablerdsaadauth", 'i', L"0", L"Security", P_ADV, K_CHECK, L"Use Microsoft Entra ID authentication", YN, L"Use Microsoft Entra authentication (web account sign-in)"},
    {L"negotiate security layer", 'i', L"1", L"Security", P_NONE, K_NONE, 0, YN, L"Negotiate the security layer"},
    {L"targetisaadjoined", 'i', L"0", L"Security", P_NONE, K_NONE, 0, YN, L"Target is Microsoft Entra joined (allows username/password sign-in)"},
    {L"kdcproxyname", 's', L"", L"Security", P_ADV, K_EDIT, L"KDC proxy name:", 0, L"Kerberos KDC proxy host name"},
    {L"rdgiskdcproxy", 'i', L"0", L"Security", P_NONE, K_NONE, 0, YN, L"The RD Gateway is also the KDC proxy"},
    {L"redirectwebauthn", 'i', L"1", L"Security", P_LOCAL, K_CHECK, L"WebAuthn (Windows Hello / security keys)", YN, L"Redirect WebAuthn requests to the local computer"},
    // --- Gateway
    {L"gatewayhostname", 's', L"", L"Gateway", P_ADV, K_EDIT, L"RD Gateway server:", 0, L"RD Gateway host name"},
    {L"gatewayusagemethod", 'i', L"0", L"Gateway", P_ADV, K_COMBO, L"RD Gateway usage:", L"0=Do not use a gateway|1=Always use the gateway|2=Use the gateway, bypass for local addresses|3=Use default gateway settings|4=Do not use a gateway, bypass for local addresses", L"When to use the RD Gateway"},
    {L"gatewaycredentialssource", 'i', L"0", L"Gateway", P_ADV, K_COMBO, L"RD Gateway logon method:", L"0=Ask for password (NTLM)|1=Smart card|2=Use my logged-on credentials|4=Allow me to select later|5=Cookie based", L"RD Gateway authentication method"},
    {L"gatewayprofileusagemethod", 'i', L"0", L"Gateway", P_ADV, K_COMBO, L"RD Gateway profile:", L"0=Use default (automatic) settings|1=Use the settings above", L"Use default or explicit RD Gateway settings"},
    {L"gatewaybrokeringtype", 'i', L"0", L"Gateway", P_NONE, K_NONE, 0, 0, L"RD Gateway brokering type"},
    // --- Display
    {L"screen mode id", 'i', L"2", L"Display", P_NONE, K_NONE, 0, L"1=Window|2=Full screen", L"Window or full screen"},
    {L"use multimon", 'i', L"0", L"Display", P_NONE, K_NONE, 0, YN, L"Use multiple monitors in full screen"},
    {L"selectedmonitors", 's', L"", L"Display", P_NONE, K_NONE, 0, 0, L"Comma separated monitor ids (see mstsc /l); the first one is the remote primary"},
    {L"desktopwidth", 'i', L"1920", L"Display", P_NONE, K_NONE, 0, 0, L"Remote desktop width in pixels (window mode)"},
    {L"desktopheight", 'i', L"1080", L"Display", P_NONE, K_NONE, 0, 0, L"Remote desktop height in pixels (window mode)"},
    {L"smart sizing", 'i', L"0", L"Display", P_DISPLAY, K_CHECK, L"Smart sizing (scale remote desktop to window)", YN, L"Scale the remote desktop to fit the client window"},
    {L"dynamic resolution", 'i', L"1", L"Display", P_DISPLAY, K_CHECK, L"Update resolution when the window is resized", YN, L"Dynamically update the remote resolution on resize"},
    {L"displayconnectionbar", 'i', L"1", L"Display", P_DISPLAY, K_CHECK, L"Show the connection bar in full screen", YN, L"Display the connection bar in full screen"},
    {L"pinconnectionbar", 'i', L"1", L"Display", P_DISPLAY, K_CHECK, L"Pin the connection bar", YN, L"Pin the connection bar in full screen"},
    {L"session bpp", 'i', L"32", L"Display", P_DISPLAY, K_COMBO, L"Color depth:", L"15=High Color (15 bit)|16=High Color (16 bit)|24=True Color (24 bit)|32=Highest Quality (32 bit)", L"Color depth of the remote session"},
    {L"desktopscalefactor", 'i', L"100", L"Display", P_DISPLAY, K_COMBO, L"Remote desktop scale factor:", L"100=100 %|125=125 %|150=150 %|175=175 %|200=200 %|250=250 %|300=300 %|400=400 %|500=500 %", L"DPI scale factor of the remote session"},
    {L"maximizetocurrentdisplays", 'i', L"0", L"Display", P_NONE, K_NONE, 0, YN, L"When maximizing, go full screen on the displays the window currently touches"},
    {L"singlemoninwindowedmode", 'i', L"0", L"Display", P_NONE, K_NONE, 0, YN, L"Switch to a single display when leaving full screen"},
    {L"span monitors", 'i', L"0", L"Display", P_NONE, K_NONE, 0, YN, L"Legacy: span one large desktop across monitors"},
    {L"desktop size id", 'i', L"", L"Display", P_NONE, K_NONE, 0, L"0=640 x 480|1=800 x 600|2=1024 x 768|3=1280 x 1024|4=1600 x 1200", L"Predefined remote desktop size"},
    {L"winposstr", 's', L"", L"Display", P_NONE, K_NONE, 0, 0, L"Window position (WINDOWPLACEMENT: 0,1,left,top,right,bottom)"},
    // --- Local resources
    {L"redirectclipboard", 'i', L"1", L"Local resources", P_LOCAL, K_CHECK, L"Clipboard", YN, L"Redirect the clipboard"},
    {L"redirectprinters", 'i', L"1", L"Local resources", P_LOCAL, K_CHECK, L"Printers", YN, L"Redirect printers"},
    {L"redirectsmartcards", 'i', L"1", L"Local resources", P_LOCAL, K_CHECK, L"Smart cards", YN, L"Redirect smart cards"},
    {L"redirectcomports", 'i', L"0", L"Local resources", P_LOCAL, K_CHECK, L"Serial (COM) ports", YN, L"Redirect COM ports"},
    {L"redirectposdevices", 'i', L"0", L"Local resources", P_LOCAL, K_CHECK, L"Point of Service devices", YN, L"Redirect Point of Service devices"},
    {L"redirectlocation", 'i', L"0", L"Local resources", P_LOCAL, K_CHECK, L"Location", YN, L"Redirect the location of the local device"},
    {L"audiocapturemode", 'i', L"0", L"Local resources", P_LOCAL, K_CHECK, L"Microphone (record from this computer)", YN, L"Redirect audio capture"},
    {L"audiomode", 'i', L"0", L"Local resources", P_LOCAL, K_COMBO, L"Remote audio playback:", L"0=Play on this computer|1=Play on the remote computer|2=Do not play", L"Where remote audio is played"},
    {L"audioqualitymode", 'i', L"0", L"Local resources", P_LOCAL, K_COMBO, L"Audio quality:", L"0=Dynamic|1=Medium|2=High (uncompressed)", L"Audio quality"},
    {L"keyboardhook", 'i', L"2", L"Local resources", P_LOCAL, K_COMBO, L"Apply Windows key combinations:", L"0=On this computer|1=On the remote computer|2=Only when using full screen", L"Where Windows key combinations (Alt+Tab ...) are applied"},
    {L"drivestoredirect", 's', L"", L"Local resources", P_LOCAL, K_EDIT, L"Drives ( * = all, DynamicDrives, C:\\;D:\\; ):", 0, L"Drives to redirect"},
    {L"devicestoredirect", 's', L"", L"Local resources", P_LOCAL, K_EDIT, L"Plug and Play devices ( * = all ):", 0, L"Plug and Play devices to redirect"},
    {L"usbdevicestoredirect", 's', L"", L"Local resources", P_LOCAL, K_EDIT, L"USB devices ( * = all ):", 0, L"RemoteFX USB devices to redirect"},
    {L"camerastoredirect", 's', L"", L"Local resources", P_LOCAL, K_EDIT, L"Cameras ( * = all ):", 0, L"Cameras to redirect"},
    {L"redirectdrives", 'i', L"0", L"Local resources", P_NONE, K_NONE, 0, YN, L"Legacy: redirect all drives"},
    {L"encode redirected video capture", 'i', L"1", L"Local resources", P_NONE, K_NONE, 0, YN, L"Encode redirected video"},
    {L"redirected video capture encoding quality", 'i', L"0", L"Local resources", P_NONE, K_NONE, 0, L"0=High compression|1=Medium compression|2=Low compression", L"Quality of encoded redirected video"},
    // --- Experience
    {L"connection type", 'i', L"7", L"Experience", P_EXP, K_COMBO, L"Connection speed:", L"1=Modem (56 kbps)|2=Low-speed broadband (256 kbps - 2 Mbps)|3=Satellite (2 - 16 Mbps, high latency)|4=High-speed broadband (2 - 10 Mbps)|5=WAN (10 Mbps or higher, high latency)|6=LAN (10 Mbps or higher)|7=Detect automatically", L"Connection type used to optimize performance"},
    {L"networkautodetect", 'i', L"1", L"Experience", P_EXP, K_CHECK, L"Detect network type automatically", YN, L"Automatic network type detection"},
    {L"bandwidthautodetect", 'i', L"1", L"Experience", P_EXP, K_CHECK, L"Detect bandwidth automatically", YN, L"Automatic bandwidth detection"},
    {L"compression", 'i', L"1", L"Experience", P_EXP, K_CHECK, L"Bulk compression", YN, L"Compress data sent to the client"},
    {L"autoreconnection enabled", 'i', L"1", L"Experience", P_EXP, K_CHECK, L"Reconnect if the connection is dropped", YN, L"Automatic reconnection"},
    {L"bitmapcachepersistenable", 'i', L"1", L"Experience", P_EXP, K_CHECK, L"Persistent bitmap caching", YN, L"Cache bitmaps on the local disk"},
    {L"allow font smoothing", 'i', L"1", L"Experience", P_EXP, K_CHECK, L"Font smoothing", YN, L"Allow font smoothing (ClearType)"},
    {L"allow desktop composition", 'i', L"1", L"Experience", P_EXP, K_CHECK, L"Desktop composition", YN, L"Allow desktop composition"},
    {L"disable wallpaper", 'i', L"0", L"Experience", P_EXP, K_CHECK, L"Disable desktop background", YN, L"Do not show the desktop wallpaper"},
    {L"disable full window drag", 'i', L"0", L"Experience", P_EXP, K_CHECK, L"Disable window contents while dragging", YN, L"Do not show window contents while dragging"},
    {L"disable menu anims", 'i', L"0", L"Experience", P_EXP, K_CHECK, L"Disable menu and window animation", YN, L"Disable menu animations"},
    {L"disable themes", 'i', L"0", L"Experience", P_EXP, K_CHECK, L"Disable visual styles", YN, L"Disable themes"},
    {L"disable cursor setting", 'i', L"0", L"Experience", P_EXP, K_CHECK, L"Disable cursor settings", YN, L"Disable cursor shadow / blinking settings"},
    {L"redirectdirectx", 'i', L"1", L"Experience", P_EXP, K_CHECK, L"DirectX redirection", YN, L"Redirect DirectX rendering"},
    {L"videoplaybackmode", 'i', L"1", L"Experience", P_EXP, K_CHECK, L"Efficient multimedia streaming for video", YN, L"Use RDP efficient multimedia streaming for video playback"},
    {L"autoreconnect max retries", 'i', L"20", L"Experience", P_EXP, K_EDIT, L"Maximum reconnection attempts:", 0, L"Number of automatic reconnection attempts"},
    {L"bitmapcachesize", 'i', L"1500", L"Experience", P_NONE, K_NONE, 0, 0, L"Bitmap cache size in KB"},
    // --- Programs / RemoteApp
    {L"alternate shell", 's', L"", L"Programs", P_ADV, K_EDIT, L"Start this program on connection:", 0, L"Program started instead of the desktop shell"},
    {L"shell working directory", 's', L"", L"Programs", P_ADV, K_EDIT, L"Start in folder:", 0, L"Working directory of the start program"},
    {L"remoteapplicationmode", 'i', L"0", L"RemoteApp", P_NONE, K_NONE, 0, YN, L"Launch as a RemoteApp session"},
    {L"remoteapplicationprogram", 's', L"", L"RemoteApp", P_NONE, K_NONE, 0, 0, L"RemoteApp alias or executable (e.g. ||notepad)"},
    {L"remoteapplicationname", 's', L"", L"RemoteApp", P_NONE, K_NONE, 0, 0, L"RemoteApp display name"},
    {L"remoteapplicationcmdline", 's', L"", L"RemoteApp", P_NONE, K_NONE, 0, 0, L"RemoteApp command line arguments"},
    {L"remoteapplicationexpandcmdline", 'i', L"1", L"RemoteApp", P_NONE, K_NONE, 0, L"0=Expand on the client|1=Expand on the server", L"Where environment variables in the command line are expanded"},
    {L"remoteapplicationexpandworkingdir", 'i', L"0", L"RemoteApp", P_NONE, K_NONE, 0, L"0=Expand on the client|1=Expand on the server", L"Where environment variables in the working dir are expanded"},
    {L"remoteapplicationfile", 's', L"", L"RemoteApp", P_NONE, K_NONE, 0, 0, L"File to open with the RemoteApp"},
    {L"remoteapplicationicon", 's', L"", L"RemoteApp", P_NONE, K_NONE, 0, 0, L"Icon file of the RemoteApp"},
    {L"disableremoteappcapscheck", 'i', L"0", L"RemoteApp", P_NONE, K_NONE, 0, YN, L"Skip the RemoteApp capability check"},
    {L"remoteappmousemoveinject", 'i', L"1", L"RemoteApp", P_NONE, K_NONE, 0, YN, L"Inject mouse moves for RemoteApp windows"},
    // --- rdpman's own keys (removed from the file handed to mstsc)
    {L"rdpman args", 's', L"", L"RDP Manager", P_ADV, K_EDIT, L"Extra mstsc arguments (e.g. /restrictedAdmin):", 0, L"Additional command line switches passed to mstsc.exe"},
    {L"rdpman group", 's', L"", L"RDP Manager", P_NONE, K_NONE, 0, 0, L"Group shown in the session list"},
    {L"rdpman notes", 's', L"", L"RDP Manager", P_NONE, K_NONE, 0, 0, L"Notes"},
    {L"rdpman monitors", 's', L"", L"RDP Manager", P_NONE, K_NONE, 0, 0, L"Monitor layout this session was set up for (id@x,y,width,height;...), used to detect changes"},
};
static const int NDEFS = _countof(g_defs);
static int DefIndex(const ws& key) { for (int i = 0; i < NDEFS; i++) if (lstrcmpiW(g_defs[i].key, key.c_str()) == 0) return i; return -1; }
static std::vector<std::pair<ws, ws>> Choices(const wchar_t* c) {
    std::vector<std::pair<ws, ws>> v; if (!c) return v;
    for (auto& p : Split(c, L'|')) { size_t e = p.find(L'='); v.push_back({ p.substr(0, e), p.substr(e + 1) }); }
    return v;
}

// ---------------------------------------------------------------- RDP file model
struct Line { ws key; wchar_t type; ws val; };   // type 0 = unparsed raw line kept verbatim in key
static bool ParseLine(const ws& raw, Line& out) {
    ws l = Trim(raw); if (l.empty()) return false;
    size_t p = l.find(L':');
    if (p != ws::npos && p > 0 && l.size() >= p + 3 && l[p + 2] == L':') {
        wchar_t t = towlower(l[p + 1]);
        if (t == 'i' || t == 's' || t == 'b') { out.key = Trim(l.substr(0, p)); out.type = t; out.val = l.substr(p + 3); return true; }
    }
    out.key = l; out.type = 0; out.val.clear(); return true;
}
struct RdpFile {
    std::vector<Line> lines;
    int Find(const wchar_t* k) const { for (size_t i = 0; i < lines.size(); i++) if (lines[i].type && lstrcmpiW(lines[i].key.c_str(), k) == 0) return (int)i; return -1; }
    bool Has(const wchar_t* k) const { return Find(k) >= 0; }
    ws Get(const wchar_t* k, const wchar_t* def = L"") const { int i = Find(k); return i < 0 ? def : lines[i].val; }
    int GetInt(const wchar_t* k, int def) const { int i = Find(k); return i < 0 || lines[i].val.empty() ? def : _wtoi(lines[i].val.c_str()); }
    void Set(const wchar_t* k, wchar_t type, const ws& v) { int i = Find(k); if (i >= 0) { lines[i].val = v; lines[i].type = type; } else lines.push_back({ k, type, v }); }
    void SetInt(const wchar_t* k, int v) { Set(k, 'i', std::to_wstring(v)); }
    void Remove(const wchar_t* k) { int i = Find(k); if (i >= 0) lines.erase(lines.begin() + i); }

    bool Load(const ws& path) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD n = GetFileSize(h, 0), rd = 0; std::vector<BYTE> b(n + 2, 0);
        ReadFile(h, b.data(), n, &rd, 0); CloseHandle(h); n = rd;
        ws text;
        if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) text.assign((wchar_t*)(b.data() + 2), (n - 2) / 2);
        else if (n >= 2 && b[1] == 0 && b[0] != 0) text.assign((wchar_t*)b.data(), n / 2);
        else {
            const char* p = (const char*)b.data(); int len = (int)n; UINT cp = CP_UTF8;
            if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) { p += 3; len -= 3; }
            int w = len ? MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, p, len, 0, 0) : 0;
            if (len && !w) { cp = CP_ACP; w = MultiByteToWideChar(cp, 0, p, len, 0, 0); }
            text.resize(w); if (w) MultiByteToWideChar(cp, 0, p, len, &text[0], w);
        }
        lines.clear();
        for (auto& l : Split(text, L'\n')) { Line ln; if (ParseLine(l, ln)) lines.push_back(ln); }
        return true;
    }
    bool Save(const ws& path, bool stripCustom = false) const {
        ws t;
        for (auto& l : lines) {
            if (stripCustom && IStarts(l.key, L"rdpman ")) continue;
            if (l.type) { t += l.key; t += L':'; t += l.type; t += L':'; t += l.val; } else t += l.key;
            t += L"\r\n";
        }
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD wr; const BYTE bom[] = { 0xFF, 0xFE };
        BOOL ok = WriteFile(h, bom, 2, &wr, 0) && WriteFile(h, t.data(), (DWORD)(t.size() * 2), &wr, 0);
        CloseHandle(h); return ok != 0;
    }
};

// ---------------------------------------------------------------- password (DPAPI, same format mstsc uses)
static ws EncryptPwd(const ws& p) {
    DATA_BLOB in = { (DWORD)(p.size() * sizeof(wchar_t)), (BYTE*)p.data() }, out = {};
    if (!CryptProtectData(&in, L"psw", 0, 0, 0, CRYPTPROTECT_UI_FORBIDDEN, &out)) return L"";
    ws hex; hex.reserve(out.cbData * 2);
    for (DWORD i = 0; i < out.cbData; i++) { wchar_t b[3]; swprintf_s(b, L"%02X", out.pbData[i]); hex += b; }
    LocalFree(out.pbData); return hex;
}
static bool DecryptPwd(const ws& hex, ws& pwd) {
    std::vector<BYTE> b;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) { wchar_t t[3] = { hex[i], hex[i + 1], 0 }; b.push_back((BYTE)wcstoul(t, 0, 16)); }
    if (b.empty()) return false;
    DATA_BLOB in = { (DWORD)b.size(), b.data() }, out = {};
    if (!CryptUnprotectData(&in, 0, 0, 0, 0, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    pwd.assign((wchar_t*)out.pbData, out.cbData / sizeof(wchar_t));
    while (!pwd.empty() && pwd.back() == 0) pwd.pop_back();
    SecureZeroMemory(out.pbData, out.cbData); LocalFree(out.pbData); return true;
}

// ---------------------------------------------------------------- monitors
// The ids mstsc uses for "selectedmonitors" (and shows with "mstsc /l") are the index of the
// display device in EnumDisplayDevices order (\\.\DISPLAY3 -> usually 2), NOT the
// EnumDisplayMonitors order, so ids can have gaps.
struct Mon { int id; RECT rc; bool primary; ws dev; };
static BOOL CALLBACK MonEnumProc(HMONITOR m, HDC, LPRECT, LPARAM lp) {
    MONITORINFOEXW mi; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(m, &mi)) return TRUE;
    Mon mon; mon.rc = mi.rcMonitor; mon.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0; mon.dev = mi.szDevice; mon.id = -1;
    DISPLAY_DEVICEW dd; dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(0, i, &dd, 0); i++) if (lstrcmpiW(dd.DeviceName, mi.szDevice) == 0) { mon.id = (int)i; break; }
    if (mon.id < 0) { size_t p = mon.dev.find_last_not_of(L"0123456789"); mon.id = _wtoi(mon.dev.c_str() + (p == ws::npos ? 0 : p + 1)) - 1; }
    ((std::vector<Mon>*)lp)->push_back(mon); return TRUE;
}
static std::vector<Mon> EnumMons() {
    std::vector<Mon> v; EnumDisplayMonitors(0, 0, MonEnumProc, (LPARAM)&v);
    std::sort(v.begin(), v.end(), [](const Mon& a, const Mon& b) { return a.id < b.id; });
    return v;
}
static const Mon* FindMon(const std::vector<Mon>& v, int id) { for (auto& m : v) if (m.id == id) return &m; return 0; }
static bool Adjacent(const RECT& a, const RECT& b) {
    bool yo = std::min(a.bottom, b.bottom) > std::max(a.top, b.top), xo = std::min(a.right, b.right) > std::max(a.left, b.left);
    return ((a.right == b.left || b.right == a.left) && yo) || ((a.bottom == b.top || b.bottom == a.top) && xo);
}
static bool Contiguous(const std::vector<Mon>& mons, const std::vector<int>& sel) {
    std::vector<const Mon*> s; for (int id : sel) if (auto m = FindMon(mons, id)) s.push_back(m);
    if (s.size() < 2) return true;
    std::vector<bool> seen(s.size(), false); std::vector<size_t> q = { 0 }; seen[0] = true; size_t n = 1;
    while (!q.empty()) { size_t c = q.back(); q.pop_back(); for (size_t i = 0; i < s.size(); i++) if (!seen[i] && Adjacent(s[c]->rc, s[i]->rc)) { seen[i] = true; n++; q.push_back(i); } }
    return n == s.size();
}

// Snapshot of the monitor geometry a session was set up for ("rdpman monitors:s:id@x,y,w,h;..."),
// used to notice later that the monitor setup no longer matches.
struct Snap { int id; RECT rc; };
static std::vector<Snap> ParseSnap(const ws& s) {
    std::vector<Snap> v;
    for (auto& p : Split(s, L';')) { int id, x, y, w, h; if (swscanf_s(p.c_str(), L"%d@%d,%d,%d,%d", &id, &x, &y, &w, &h) == 5) v.push_back({ id, { x, y, x + w, y + h } }); }
    return v;
}
static ws MakeSnap(const std::vector<Mon>& mons, const std::vector<int>& ids, bool all, const ws& old) {
    ws s; auto prev = ParseSnap(old);
    auto add = [&](int id, const RECT& r) { if (!s.empty()) s += L";"; s += Fmt(L"%d@%d,%d,%d,%d", id, r.left, r.top, r.right - r.left, r.bottom - r.top); };
    if (all) { for (auto& m : mons) add(m.id, m.rc); return s; }
    for (int id : ids) { if (auto m = FindMon(mons, id)) add(id, m->rc); else for (auto& p : prev) if (p.id == id) add(id, p.rc); }
    return s;
}
static ws RectText(const RECT& r) { return Fmt(L"%d x %d at %d,%d", r.right - r.left, r.bottom - r.top, r.left, r.top); }
// Returns an empty string if the attached monitors still match what the session was set up for,
// otherwise one line per problem.
static ws MonitorIssue(const RdpFile& f, const std::vector<Mon>& mons) {
    if (f.GetInt(L"screen mode id", 2) != 2 || !f.GetInt(L"use multimon", 0)) return L"";
    auto sel = ParseIds(f.Get(L"selectedmonitors")); auto snap = ParseSnap(f.Get(L"rdpman monitors")); ws out;
    auto line = [&](const ws& t) { if (!out.empty()) out += L"\n"; out += t; };
    auto snapOf = [&](int id) -> const Snap* { for (auto& s : snap) if (s.id == id) return &s; return 0; };
    auto changed = [&](const Mon& m) {
        const Snap* s = snapOf(m.id);
        if (s && !EqualRect(&s->rc, &m.rc)) line(Fmt(L"Monitor %d changed: was %s, now %s.", m.id, RectText(s->rc).c_str(), RectText(m.rc).c_str()));
    };
    if (sel.empty()) {                            // all monitors: compare with the set seen at setup time
        if (snap.empty()) return L"";
        for (auto& s : snap) if (!FindMon(mons, s.id)) line(Fmt(L"Monitor %d (%s) is no longer attached.", s.id, RectText(s.rc).c_str()));
        for (auto& m : mons) if (!snapOf(m.id)) line(Fmt(L"Monitor %d (%s) was added.", m.id, RectText(m.rc).c_str())); else changed(m);
        return out;
    }
    bool missing = false;
    for (int id : sel) {
        const Mon* m = FindMon(mons, id);
        if (m) { changed(*m); continue; }
        missing = true; const Snap* s = snapOf(id);
        line(s ? Fmt(L"Monitor %d (%s) is no longer attached.", id, RectText(s->rc).c_str()) : Fmt(L"Monitor %d is not attached.", id));
    }
    if (!missing && !Contiguous(mons, sel)) line(L"The selected monitors are not adjacent (mstsc needs adjacent monitors).");
    return out;
}

// "Identify": flash the mstsc monitor id on every monitor
static LRESULT CALLBACK IdentProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_TIMER: DestroyWindow(h); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
        HBRUSH br = CreateSolidBrush(RGB(0, 99, 177)); FillRect(dc, &rc, br); DeleteObject(br);
        HFONT f = CreateFontW(-(rc.bottom * 6 / 10), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ of = SelectObject(dc, f); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        ws t = std::to_wstring((int)GetWindowLongPtrW(h, GWLP_USERDATA));
        DrawTextW(dc, t.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of); DeleteObject(f); EndPaint(h, &ps); return 0; }
    }
    return DefWindowProcW(h, m, w, l);
}
static void IdentifyMonitors() {
    for (auto& m : EnumMons()) {
        int mw = m.rc.right - m.rc.left, mh = m.rc.bottom - m.rc.top, sz = std::min(mw, mh) / 3;
        HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"RdpManIdent", L"", WS_POPUP,
            m.rc.left + (mw - sz) / 2, m.rc.top + (mh - sz) / 2, sz, sz, 0, 0, g_inst, 0);
        SetWindowLongPtrW(h, GWLP_USERDATA, m.id);
        ShowWindow(h, SW_SHOWNOACTIVATE); SetTimer(h, 1, 2500, 0);
    }
}
static void RunMstsc(const ws& args) {
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    ws cmd = Fmt(L"\"%s\\mstsc.exe\" %s", sys, args.c_str());
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
    if (CreateProcessW(0, &cmd[0], 0, 0, FALSE, 0, 0, 0, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    else Msg(g_main, L"Could not start mstsc.exe.", MB_ICONERROR);
}

// ---------------------------------------------------------------- UI plumbing
static HWND Ctl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), parent, (HMENU)(INT_PTR)id, g_inst, 0);
}
static BOOL CALLBACK SetFontProc(HWND h, LPARAM f) { SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE); return TRUE; }
static void ApplyFont(HWND h) { EnumChildWindows(h, SetFontProc, (LPARAM)g_font); }
static void MakeFont() {
    if (g_font) DeleteObject(g_font);
    NONCLIENTMETRICSW n = { sizeof(n) }; SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(n), &n, 0, g_dpi);
    g_font = CreateFontIndirectW(&n.lfMessageFont);
}
static HWND CreateModal(HWND owner, const wchar_t* cls, const wchar_t* title, int cw, int ch) {
    DWORD st = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
    RECT r = { 0, 0, S(cw), S(ch) }; AdjustWindowRectExForDpi(&r, st, FALSE, ex, g_dpi);
    RECT o; GetWindowRect(owner, &o);
    int w = r.right - r.left, h = r.bottom - r.top, x = o.left + (o.right - o.left - w) / 2, y = o.top + (o.bottom - o.top - h) / 2;
    MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi);
    x = std::max((int)mi.rcWork.left, std::min(x, (int)mi.rcWork.right - w)); y = std::max((int)mi.rcWork.top, std::min(y, (int)mi.rcWork.bottom - h));
    return CreateWindowExW(ex, cls, title, st, x, y, w, h, owner, 0, g_inst, 0);
}
static void RunModal(HWND dlg) {
    HWND owner = GetWindow(dlg, GW_OWNER);
    EnableWindow(owner, FALSE); ShowWindow(dlg, SW_SHOW);
    MSG m;
    while (IsWindow(dlg)) {
        if (!GetMessageW(&m, 0, 0, 0)) { PostQuitMessage((int)m.wParam); break; }
        if (!IsDialogMessageW(dlg, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
}
static void EndModal(HWND dlg) { HWND o = GetWindow(dlg, GW_OWNER); EnableWindow(o, TRUE); SetActiveWindow(o); DestroyWindow(dlg); }

// ---------------------------------------------------------------- value prompt
struct PromptState { ws* val; bool ok; HWND combo; std::vector<std::pair<ws, ws>> ch; };
static PromptState* P;
static LRESULT CALLBACK PromptProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case DM_GETDEFID: return MAKELONG(IDOK, DC_HASDEFID);
    case WM_CLOSE: EndModal(h); return 0;
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            ws t = GetText(P->combo);
            for (auto& c : P->ch) if (t == c.first + L" - " + c.second) t = c.first;
            *P->val = t; P->ok = true; EndModal(h);
        } else if (LOWORD(w) == IDCANCEL) EndModal(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static bool Prompt(HWND owner, const ws& title, const ws& text, ws& val, const wchar_t* choices) {
    PromptState st = { &val, false, 0, Choices(choices) }; PromptState* prev = P; P = &st;
    HWND h = CreateModal(owner, L"RdpManPrompt", title.c_str(), 480, 150);
    Ctl(h, L"STATIC", text.c_str(), 0, 12, 10, 456, 50, -1);
    st.combo = Ctl(h, L"COMBOBOX", L"", (st.ch.empty() ? CBS_SIMPLE : CBS_DROPDOWN) | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, 12, 68, 456, st.ch.empty() ? 24 : 300, 10);
    ws cur = val;
    for (auto& c : st.ch) { ws s = c.first + L" - " + c.second; ComboBox_AddString(st.combo, s.c_str()); if (c.first == val) cur = s; }
    SetWindowTextW(st.combo, cur.c_str());
    Ctl(h, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 296, 112, 82, 26, IDOK);
    Ctl(h, L"BUTTON", L"Cancel", WS_TABSTOP, 386, 112, 82, 26, IDCANCEL);
    ApplyFont(h); SetFocus(st.combo); RunModal(h);
    P = prev; return st.ok;
}

// ---------------------------------------------------------------- session editor
enum {
    IDC_NAME = 100, IDC_GROUP, IDC_PWD, IDC_NOTES, IDC_MODE0 = 110, IDC_MODE1, IDC_MODE2, IDC_MODE3, IDC_RSIZE, IDC_PICK,
    IDC_IDENT, IDC_REFRESH, IDC_MSTSCL, IDC_MONSTAT, IDC_FILTER, IDC_ALLLIST, IDC_ALLEDIT, IDC_ALLUNSET, IDC_ALLADD, IDC_TAB = 130,
    IDC_DEF0 = 2000
};
enum { MODE_WINDOW, MODE_FULL, MODE_ALL, MODE_SELECTED };
static const int PW = 650, PH = 500;      // page size in 96-dpi units

struct Editor {
    HWND hwnd = 0, tab = 0, pages[6] = {}, picker = 0, allList = 0, monStat = 0, allDesc = 0;
    RdpFile f, orig; ws name, origName; bool ok = false, loading = false, pwdDirty = false;
    HWND ctl[NDEFS] = {};
    std::vector<Mon> mons; std::vector<int> sel;
    ws issue;                 // what changed in the monitor setup since the session was saved
    int curPage = 0;
};
static Editor* E;

static int CurMode() { for (int i = 0; i < 4; i++) if (IsDlgButtonChecked(E->pages[P_DISPLAY], IDC_MODE0 + i)) return i; return MODE_FULL; }
static void SetMode(int m) { CheckRadioButton(E->pages[P_DISPLAY], IDC_MODE0, IDC_MODE3, IDC_MODE0 + m); }

static void UpdateMonStatus() {
    ws s; int mode = CurMode();
    if (mode == MODE_SELECTED) {
        ws missing; for (int id : E->sel) if (!FindMon(E->mons, id)) missing += (missing.empty() ? L"" : L", ") + std::to_wstring(id);
        if (E->sel.empty()) s = L"Click the monitors the session should use. The first one you click becomes the primary monitor of the remote session.";
        else {
            s = L"selectedmonitors = " + JoinIds(E->sel) + L"   (first = remote primary)";
            if (!E->issue.empty()) s += L"\nMonitor setup changed since this session was saved: " + ReplaceAll(E->issue, L"\n", L" ") + L" Click the monitors to use now.";
            else if (!missing.empty()) s += L"\nMonitor " + missing + L" is not attached right now (dropped when you change the selection).";
            else if (!Contiguous(E->mons, E->sel)) s += L"\nWarning: the selected monitors are not adjacent - mstsc needs adjacent monitors.";
        }
    } else if (mode == MODE_ALL) {
        s = L"The session spans all monitors attached at connect time.";
        if (!E->issue.empty()) s += L"\nMonitor setup changed since this session was saved: " + ReplaceAll(E->issue, L"\n", L" ");
    }
    else if (mode == MODE_FULL) s = L"Full screen on a single monitor chosen by mstsc. To pin a specific monitor, use \"selected monitors\" and click just that one.";
    else s = L"The session opens in a window of the given size.";
    SetWindowTextW(E->monStat, s.c_str());
    InvalidateRect(E->picker, 0, TRUE);
}

// --- monitor picker control
static void PickerLayout(HWND h, std::vector<RECT>& out) {
    out.clear(); if (E->mons.empty()) return;
    RECT c; GetClientRect(h, &c); RECT b = E->mons[0].rc;
    for (auto& m : E->mons) UnionRect(&b, &b, &m.rc);
    int mg = S(8); double bw = b.right - b.left, bh = b.bottom - b.top;
    double sc = std::min((c.right - 2 * mg) / bw, (c.bottom - 2 * mg) / bh);
    int ox = (int)((c.right - bw * sc) / 2), oy = (int)((c.bottom - bh * sc) / 2);
    for (auto& m : E->mons) {
        RECT r = { ox + (int)((m.rc.left - b.left) * sc), oy + (int)((m.rc.top - b.top) * sc), ox + (int)((m.rc.right - b.left) * sc), oy + (int)((m.rc.bottom - b.top) * sc) };
        InflateRect(&r, -S(2), -S(2)); out.push_back(r);
    }
}
static LRESULT CALLBACK PickerProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT c; GetClientRect(h, &c);
        FillRect(dc, &c, GetSysColorBrush(COLOR_WINDOW)); FrameRect(dc, &c, GetSysColorBrush(COLOR_ACTIVEBORDER));
        std::vector<RECT> rs; PickerLayout(h, rs); int mode = CurMode();
        HGDIOBJ of = SelectObject(dc, g_font); SetBkMode(dc, TRANSPARENT);
        for (size_t i = 0; i < rs.size(); i++) {
            const Mon& mon = E->mons[i];
            int order = -1; for (size_t k = 0; k < E->sel.size(); k++) if (E->sel[k] == mon.id) order = (int)k;
            bool on = mode == MODE_ALL || (mode == MODE_SELECTED && order >= 0);
            HBRUSH br = CreateSolidBrush(on ? RGB(0, 120, 215) : RGB(228, 228, 228)); FillRect(dc, &rs[i], br); DeleteObject(br);
            br = CreateSolidBrush(on ? RGB(0, 64, 128) : RGB(140, 140, 140)); FrameRect(dc, &rs[i], br); DeleteObject(br);
            ws t = Fmt(L"Monitor %d\n%d x %d", mon.id, mon.rc.right - mon.rc.left, mon.rc.bottom - mon.rc.top);
            if (mon.primary) t += L"\n(local primary)";
            if (mode == MODE_SELECTED && order == 0) t += L"\n\x2713 remote primary";
            else if (mode == MODE_SELECTED && order > 0) t += Fmt(L"\n\x2713 #%d", order + 1);
            SetTextColor(dc, on ? RGB(255, 255, 255) : RGB(40, 40, 40));
            RECT tr = rs[i]; DrawTextW(dc, t.c_str(), -1, &tr, DT_CENTER | DT_CALCRECT | DT_NOPREFIX);
            int th = tr.bottom - tr.top; tr = rs[i]; tr.top += std::max(0, (int)(tr.bottom - tr.top - th) / 2);
            DrawTextW(dc, t.c_str(), -1, &tr, DT_CENTER | DT_NOPREFIX);
        }
        SelectObject(dc, of); EndPaint(h, &ps); return 0; }
    case WM_LBUTTONDOWN: {
        std::vector<RECT> rs; PickerLayout(h, rs); POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        for (size_t i = 0; i < rs.size(); i++) if (PtInRect(&rs[i], pt)) {
            if (CurMode() != MODE_SELECTED) SetMode(MODE_SELECTED);
            auto& sel = E->sel; int id = E->mons[i].id;
            sel.erase(std::remove_if(sel.begin(), sel.end(), [](int s) { return !FindMon(E->mons, s); }), sel.end());
            auto it = std::find(sel.begin(), sel.end(), id);
            if (it != sel.end()) sel.erase(it); else sel.push_back(id);
            E->issue.clear(); UpdateMonStatus(); break;
        }
        return 0; }
    }
    return DefWindowProcW(h, m, w, l);
}

static LRESULT CALLBACK PageProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND: case WM_NOTIFY: return SendMessageW(GetParent(h), m, w, l);
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: SetBkColor((HDC)w, GetSysColor(COLOR_WINDOW)); return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    return DefWindowProcW(h, m, w, l);
}

static int LayoutAuto(int page, int y) {
    HWND parent = E->pages[page]; int col = 0, cw = (PW - 28) / 2;
    for (int i = 0; i < NDEFS; i++) {
        const PropDef& d = g_defs[i]; if (d.page != page) continue;
        if (d.kind == K_CHECK) {
            E->ctl[i] = Ctl(parent, L"BUTTON", d.label, BS_AUTOCHECKBOX | WS_TABSTOP, 14 + col * cw, y, cw - 6, 20, IDC_DEF0 + i);
            if (++col == 2) { col = 0; y += 24; }
        } else {
            if (col) { col = 0; y += 24; }
            Ctl(parent, L"STATIC", d.label, SS_NOPREFIX, 14, y + 3, 250, 18, -1);
            if (d.kind == K_COMBO) {
                E->ctl[i] = Ctl(parent, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 268, y, PW - 282, 300, IDC_DEF0 + i);
                for (auto& c : Choices(d.choices)) ComboBox_AddString(E->ctl[i], c.second.c_str());
            } else E->ctl[i] = Ctl(parent, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 268, y, PW - 282, 22, IDC_DEF0 + i, WS_EX_CLIENTEDGE);
            y += 28;
        }
    }
    if (col) y += 24;
    return y;
}

// model -> controls
static void Push() {
    E->loading = true; const RdpFile& f = E->f;
    for (int i = 0; i < NDEFS; i++) {
        HWND c = E->ctl[i]; if (!c) continue; const PropDef& d = g_defs[i];
        ws v = f.Has(d.key) ? f.Get(d.key) : d.def;
        if (d.kind == K_CHECK) Button_SetCheck(c, _wtoi(v.c_str()) ? BST_CHECKED : BST_UNCHECKED);
        else if (d.kind == K_EDIT) SetWindowTextW(c, v.c_str());
        else {
            auto ch = Choices(d.choices); int sel = -1;
            while (ComboBox_GetCount(c) > (int)ch.size()) ComboBox_DeleteString(c, ComboBox_GetCount(c) - 1);
            for (size_t k = 0; k < ch.size(); k++) if (ch[k].first == v) sel = (int)k;
            if (sel < 0) sel = ComboBox_AddString(c, v.c_str());
            ComboBox_SetCurSel(c, sel);
        }
    }
    SetWindowTextW(GetDlgItem(E->pages[P_GENERAL], IDC_GROUP), f.Get(L"rdpman group").c_str());
    SetWindowTextW(GetDlgItem(E->pages[P_GENERAL], IDC_NOTES), ReplaceAll(f.Get(L"rdpman notes"), L"\\n", L"\r\n").c_str());
    if (!E->pwdDirty) {
        HWND pe = GetDlgItem(E->pages[P_GENERAL], IDC_PWD); ws pwd;
        bool has = f.Has(L"password 51"), dec = has && DecryptPwd(f.Get(L"password 51"), pwd);
        SetWindowTextW(pe, dec ? pwd.c_str() : L"");
        Edit_SetCueBannerText(pe, has && !dec ? L"saved for another user/PC - type to replace" : L"not saved - mstsc will ask");
    }
    int mode = MODE_WINDOW;
    if (f.GetInt(L"screen mode id", 2) == 2) mode = f.GetInt(L"use multimon", 0) ? (f.Get(L"selectedmonitors").empty() ? MODE_ALL : MODE_SELECTED) : MODE_FULL;
    SetMode(mode);
    if (mode == MODE_SELECTED) E->sel = ParseIds(f.Get(L"selectedmonitors"));
    SetWindowTextW(GetDlgItem(E->pages[P_DISPLAY], IDC_RSIZE), Fmt(L"%d x %d", f.GetInt(L"desktopwidth", 1920), f.GetInt(L"desktopheight", 1080)).c_str());
    E->loading = false;
    UpdateMonStatus();
}
// controls -> model
// Settings the file does not contain stay absent unless the user changed them (force = write everything).
static void Pull(bool force = false) {
    RdpFile& f = E->f;
    for (int i = 0; i < NDEFS; i++) {
        HWND c = E->ctl[i]; if (!c) continue; const PropDef& d = g_defs[i]; ws v;
        if (d.kind == K_CHECK) v = Button_GetCheck(c) == BST_CHECKED ? L"1" : L"0";
        else if (d.kind == K_EDIT) { v = Trim(GetText(c)); if (v.empty()) { f.Remove(d.key); continue; } }
        else { auto ch = Choices(d.choices); int s = ComboBox_GetCurSel(c); v = s >= 0 && s < (int)ch.size() ? ch[s].first : GetText(c); }
        if (!force && !f.Has(d.key) && v == d.def) continue;
        f.Set(d.key, d.type, v);
    }
    ws g = Trim(GetText(GetDlgItem(E->pages[P_GENERAL], IDC_GROUP)));
    if (g.empty()) f.Remove(L"rdpman group"); else f.Set(L"rdpman group", 's', g);
    ws n = ReplaceAll(ReplaceAll(Trim(GetText(GetDlgItem(E->pages[P_GENERAL], IDC_NOTES))), L"\r", L""), L"\n", L"\\n");
    if (n.empty()) f.Remove(L"rdpman notes"); else f.Set(L"rdpman notes", 's', n);
    if (E->pwdDirty) {
        ws p = GetText(GetDlgItem(E->pages[P_GENERAL], IDC_PWD));
        if (p.empty()) f.Remove(L"password 51"); else f.Set(L"password 51", 'b', EncryptPwd(p));
        SecureZeroMemory(&p[0], p.size() * sizeof(wchar_t));
    }
    int mode = CurMode();
    f.SetInt(L"screen mode id", mode == MODE_WINDOW ? 1 : 2);
    f.SetInt(L"use multimon", mode == MODE_ALL || mode == MODE_SELECTED ? 1 : 0);
    if (mode == MODE_SELECTED) f.Set(L"selectedmonitors", 's', JoinIds(E->sel)); else f.Remove(L"selectedmonitors");
    if (mode == MODE_SELECTED || mode == MODE_ALL) f.Set(L"rdpman monitors", 's', MakeSnap(E->mons, E->sel, mode == MODE_ALL, f.Get(L"rdpman monitors")));
    else f.Remove(L"rdpman monitors");
    ws sz = GetText(GetDlgItem(E->pages[P_DISPLAY], IDC_RSIZE)); wchar_t* e = 0;
    int w = wcstol(sz.c_str(), &e, 10); while (*e && !iswdigit(*e)) e++; int h = wcstol(e, 0, 10);
    if (w >= 200 && h >= 200 && (force || mode == MODE_WINDOW || f.Has(L"desktopwidth"))) { f.SetInt(L"desktopwidth", w); f.SetInt(L"desktopheight", h); }
}

// --- "All settings" list
static void RefreshAll() {
    HWND lv = E->allList; int keep = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv); ListView_RemoveAllGroups(lv);
    std::vector<ws> cats; ws filter = Lower(Trim(GetText(GetDlgItem(E->pages[P_ALL], IDC_FILTER))));
    auto add = [&](const wchar_t* cat, const ws& key, const ws& val, const ws& def, const wchar_t* desc, LPARAM lp) {
        if (!filter.empty() && Lower(key + L" " + val + L" " + desc + L" " + cat).find(filter) == ws::npos) return;
        int g = -1; for (size_t i = 0; i < cats.size(); i++) if (cats[i] == cat) g = (int)i;
        if (g < 0) {
            g = (int)cats.size(); cats.push_back(cat);
            LVGROUP lg = { sizeof(lg) }; lg.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_STATE; lg.pszHeader = (LPWSTR)cat; lg.iGroupId = g;
            lg.state = LVGS_COLLAPSIBLE; lg.stateMask = LVGS_COLLAPSIBLE; ListView_InsertGroup(lv, -1, &lg);
        }
        LVITEMW it = {}; it.mask = LVIF_TEXT | LVIF_PARAM | LVIF_GROUPID; it.iItem = ListView_GetItemCount(lv); it.pszText = (LPWSTR)key.c_str(); it.lParam = lp; it.iGroupId = g;
        int n = ListView_InsertItem(lv, &it);
        ListView_SetItemText(lv, n, 1, (LPWSTR)val.c_str()); ListView_SetItemText(lv, n, 2, (LPWSTR)def.c_str()); ListView_SetItemText(lv, n, 3, (LPWSTR)desc);
    };
    auto pretty = [](const PropDef& d, const ws& v) { for (auto& c : Choices(d.choices)) if (c.first == v) return v + L"  (" + c.second + L")"; return v; };
    for (int i = 0; i < NDEFS; i++) {
        const PropDef& d = g_defs[i]; bool has = E->f.Has(d.key);
        ws v = !has ? L"" : d.type == 'b' ? L"(encrypted)" : pretty(d, E->f.Get(d.key));
        add(d.cat, Fmt(L"%s:%c", d.key, d.type), v, pretty(d, d.def), d.desc, i);
    }
    for (size_t i = 0; i < E->f.lines.size(); i++) {
        const Line& l = E->f.lines[i];
        if (l.type && DefIndex(l.key) >= 0) continue;
        add(L"Custom / unknown", l.type ? Fmt(L"%s:%c", l.key.c_str(), l.type) : l.key, l.val, L"", L"", 0x10000 + (LPARAM)i);
    }
    ListView_EnableGroupView(lv, TRUE);
    if (keep >= 0) { ListView_SetItemState(lv, keep, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); ListView_EnsureVisible(lv, keep, FALSE); }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0); InvalidateRect(lv, 0, TRUE);
}
static LPARAM AllSel() {
    int i = ListView_GetNextItem(E->allList, -1, LVNI_SELECTED); if (i < 0) return -1;
    LVITEMW it = {}; it.mask = LVIF_PARAM; it.iItem = i; ListView_GetItem(E->allList, &it); return it.lParam;
}
static void AllEdit() {
    LPARAM lp = AllSel(); if (lp < 0) return;
    if (lp < 0x10000) {
        const PropDef& d = g_defs[lp];
        if (d.type == 'b') { Msg(E->hwnd, L"The password is edited on the General tab."); return; }
        ws v = E->f.Has(d.key) ? E->f.Get(d.key) : d.def;
        ws text = Fmt(L"%s\n\nType: %s", d.desc, d.type == 'i' ? L"integer" : L"string");
        if (!Prompt(E->hwnd, Fmt(L"%s:%c", d.key, d.type), text, v, d.choices)) return;
        v = Trim(v);
        if (d.type == 'i' && !v.empty()) { wchar_t* e; wcstol(v.c_str(), &e, 10); if (*e) { Msg(E->hwnd, L"This setting needs an integer value.", MB_ICONWARNING); return; } }
        E->f.Set(d.key, d.type, v);
    } else {
        Line& l = E->f.lines[lp - 0x10000];
        ws raw = l.type ? Fmt(L"%s:%c:%s", l.key.c_str(), l.type, l.val.c_str()) : l.key;
        if (!Prompt(E->hwnd, L"Custom line", L"Raw line of the .rdp file in the form\nname:type:value   (type is i = integer, s = string, b = binary)", raw, 0)) return;
        Line n; if (ParseLine(raw, n)) l = n;
    }
    RefreshAll();
}
static void AllUnset() {
    LPARAM lp = AllSel(); if (lp < 0) return;
    if (lp < 0x10000) E->f.Remove(g_defs[lp].key); else E->f.lines.erase(E->f.lines.begin() + (lp - 0x10000));
    RefreshAll();
}
static void AllAdd() {
    ws raw;
    if (!Prompt(E->hwnd, L"Add custom setting", L"Raw line of the .rdp file in the form\nname:type:value   (type is i = integer, s = string, b = binary)", raw, 0)) return;
    Line n; if (!ParseLine(raw, n)) return;
    if (n.type) E->f.Set(n.key.c_str(), n.type, n.val); else E->f.lines.push_back(n);
    RefreshAll();
}

static void ShowPage(int n) {
    for (int i = 0; i < 6; i++) ShowWindow(E->pages[i], i == n ? SW_SHOW : SW_HIDE);
    E->curPage = n; TabCtrl_SetCurSel(E->tab, n);
}
static bool EditorAccept() {
    if (E->curPage != P_ALL) Pull();
    ws name = Trim(GetText(GetDlgItem(E->pages[P_GENERAL], IDC_NAME)));
    auto fail = [](int page, HWND focus, const wchar_t* msg) { Push(); ShowPage(page); if (focus) SetFocus(focus); Msg(E->hwnd, msg, MB_ICONWARNING); return false; };
    HWND nameCtl = GetDlgItem(E->pages[P_GENERAL], IDC_NAME);
    if (name.empty()) return fail(P_GENERAL, nameCtl, L"Please enter a name for the session.");
    if (name.find_first_of(L"\\/:*?\"<>|") != ws::npos || name.back() == L'.') return fail(P_GENERAL, nameCtl, L"The name must not contain any of  \\ / : * ? \" < > |  or end with a dot.");
    if (!IEq(name, E->origName) && FileExists(g_dataDir + L"\\" + name + L".rdp")) return fail(P_GENERAL, nameCtl, L"A session with this name already exists.");
    if (Trim(E->f.Get(L"full address")).empty()) return fail(P_GENERAL, E->ctl[DefIndex(L"full address")], L"Please enter the computer to connect to.");
    if (E->f.GetInt(L"screen mode id", 2) == 2 && E->f.GetInt(L"use multimon", 0) && E->f.Has(L"selectedmonitors") && ParseIds(E->f.Get(L"selectedmonitors")).empty())
        return fail(P_DISPLAY, 0, L"Please click at least one monitor, or choose another display mode.");
    E->name = name; E->ok = true; return true;
}

static LRESULT CALLBACK EditorProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case DM_GETDEFID: return MAKELONG(IDOK, DC_HASDEFID);
    case WM_CLOSE: EndModal(h); return 0;
    case WM_DISPLAYCHANGE: E->mons = EnumMons(); E->issue = MonitorIssue(E->orig, E->mons); UpdateMonStatus(); break;
    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)l;
        if (nh->idFrom == IDC_TAB) {
            if (nh->code == TCN_SELCHANGING) { if (E->curPage != P_ALL) Pull(); return FALSE; }
            if (nh->code == TCN_SELCHANGE) { int n = TabCtrl_GetCurSel(E->tab); if (n == P_ALL) RefreshAll(); else Push(); ShowPage(n); }
        } else if (nh->idFrom == IDC_ALLLIST && (nh->code == NM_DBLCLK || nh->code == NM_RETURN)) AllEdit();
        else if (nh->idFrom == IDC_ALLLIST && nh->code == LVN_ITEMCHANGED) {
            LPARAM lp = AllSel(); SetWindowTextW(E->allDesc, lp >= 0 && lp < 0x10000 ? g_defs[lp].desc : L"");
        }
        return 0; }
    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        if (E->loading) return 0;
        switch (id) {
        case IDOK: if (GetFocus() == E->allList) AllEdit(); else if (EditorAccept()) EndModal(h); break;
        case IDCANCEL: EndModal(h); break;
        case IDC_PWD: if (code == EN_CHANGE) E->pwdDirty = true; break;
        case IDC_MODE0: case IDC_MODE1: case IDC_MODE2: case IDC_MODE3: UpdateMonStatus(); break;
        case IDC_RSIZE: if (code == CBN_SELCHANGE || code == CBN_EDITCHANGE) { SetMode(MODE_WINDOW); UpdateMonStatus(); } break;
        case IDC_IDENT: IdentifyMonitors(); break;
        case IDC_REFRESH: E->mons = EnumMons(); E->issue = MonitorIssue(E->orig, E->mons); UpdateMonStatus(); break;
        case IDC_MSTSCL: RunMstsc(L"/l"); break;
        case IDC_FILTER: if (code == EN_CHANGE) RefreshAll(); break;
        case IDC_ALLEDIT: AllEdit(); break;
        case IDC_ALLUNSET: AllUnset(); break;
        case IDC_ALLADD: AllAdd(); break;
        }
        return 0; }
    }
    return DefWindowProcW(h, m, w, l);
}

static bool EditSession(HWND owner, RdpFile& f, ws& name, bool isNew, int startPage, const std::vector<ws>& groups) {
    Editor ed; E = &ed; ed.f = ed.orig = f; ed.issue = MonitorIssue(f, EnumMons()); ed.name = name; ed.origName = isNew ? L"" : name; ed.mons = EnumMons();
    HWND h = ed.hwnd = CreateModal(owner, L"RdpManEditor", isNew ? L"New session" : (L"Edit session - " + name).c_str(), 680, 596);
    ed.tab = Ctl(h, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP, 8, 8, 664, 540, IDC_TAB);
    SendMessageW(ed.tab, WM_SETFONT, (WPARAM)g_font, TRUE);
    const wchar_t* titles[] = { L"General", L"Display", L"Local resources", L"Experience", L"Advanced", L"All settings" };
    for (int i = 0; i < 6; i++) { TCITEMW ti = {}; ti.mask = TCIF_TEXT; ti.pszText = (LPWSTR)titles[i]; TabCtrl_InsertItem(ed.tab, i, &ti); }
    RECT tr; GetWindowRect(ed.tab, &tr); MapWindowPoints(0, h, (POINT*)&tr, 2); TabCtrl_AdjustRect(ed.tab, FALSE, &tr);
    for (int i = 0; i < 6; i++) {
        ed.pages[i] = CreateWindowExW(WS_EX_CONTROLPARENT, L"RdpManPage", L"", WS_CHILD | WS_CLIPCHILDREN, tr.left, tr.top, tr.right - tr.left, tr.bottom - tr.top, h, 0, g_inst, 0);
        SetWindowPos(ed.pages[i], HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    // General
    HWND pg = ed.pages[P_GENERAL];
    Ctl(pg, L"STATIC", L"Session name:", 0, 14, 15, 250, 18, -1);
    HWND nameCtl = Ctl(pg, L"EDIT", name.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 268, 12, PW - 282, 22, IDC_NAME, WS_EX_CLIENTEDGE);
    Ctl(pg, L"STATIC", L"Group:", 0, 14, 43, 250, 18, -1);
    HWND grp = Ctl(pg, L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_SORT | WS_VSCROLL | WS_TABSTOP, 268, 40, PW - 282, 300, IDC_GROUP);
    for (auto& g : groups) ComboBox_AddString(grp, g.c_str());
    int y = LayoutAuto(P_GENERAL, 68) + 4;
    Ctl(pg, L"STATIC", L"Password (optional):", 0, 14, y + 3, 250, 18, -1);
    Ctl(pg, L"EDIT", L"", ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP, 268, y, PW - 282, 22, IDC_PWD, WS_EX_CLIENTEDGE);
    Ctl(pg, L"STATIC", L"Saved passwords are encrypted with Windows DPAPI and only work for this Windows user on this PC. "
        L"Leave empty to let mstsc ask (it can remember credentials itself).", 0, 268, y + 26, PW - 282, 48, -1);
    y += 80;
    Ctl(pg, L"STATIC", L"Notes:", 0, 14, y + 3, 250, 18, -1);
    Ctl(pg, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP, 268, y, PW - 282, PH - y - 14, IDC_NOTES, WS_EX_CLIENTEDGE);
    // Display
    HWND pd = ed.pages[P_DISPLAY];
    Ctl(pd, L"BUTTON", L"Window, remote desktop size:", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 14, 10, 250, 20, IDC_MODE0);
    Ctl(pd, L"BUTTON", L"Full screen on one monitor (mstsc default)", BS_AUTORADIOBUTTON, 14, 34, 320, 20, IDC_MODE1);
    Ctl(pd, L"BUTTON", L"Full screen on all monitors", BS_AUTORADIOBUTTON, 14, 58, 320, 20, IDC_MODE2);
    Ctl(pd, L"BUTTON", L"Full screen on selected monitors - click to select:", BS_AUTORADIOBUTTON, 14, 82, 320, 20, IDC_MODE3);
    HWND size = Ctl(pd, L"COMBOBOX", L"", CBS_DROPDOWN | WS_VSCROLL | WS_GROUP | WS_TABSTOP, 268, 8, 150, 300, IDC_RSIZE);
    for (auto s : { L"800 x 600", L"1024 x 768", L"1280 x 720", L"1280 x 1024", L"1366 x 768", L"1440 x 900", L"1600 x 900", L"1600 x 1200", L"1920 x 1080", L"1920 x 1200", L"2560 x 1440", L"3440 x 1440", L"3840 x 2160" }) ComboBox_AddString(size, s);
    ed.picker = Ctl(pd, L"RdpManMonPick", L"", 0, 14, 108, PW - 28, 170, IDC_PICK);
    ed.monStat = Ctl(pd, L"STATIC", L"", SS_NOPREFIX, 14, 284, PW - 28, 68, IDC_MONSTAT);
    Ctl(pd, L"BUTTON", L"Identify", WS_TABSTOP, PW - 14 - 290, 76, 90, 26, IDC_IDENT);
    Ctl(pd, L"BUTTON", L"Refresh", WS_TABSTOP, PW - 14 - 194, 76, 90, 26, IDC_REFRESH);
    Ctl(pd, L"BUTTON", L"mstsc /l", WS_TABSTOP, PW - 14 - 98, 76, 98, 26, IDC_MSTSCL);
    LayoutAuto(P_DISPLAY, 360);
    LayoutAuto(P_LOCAL, 14); LayoutAuto(P_EXP, 14); LayoutAuto(P_ADV, 14);
    // All settings
    HWND pa = ed.pages[P_ALL];
    Ctl(pa, L"STATIC", L"Filter:", 0, 14, 15, 44, 18, -1);
    Ctl(pa, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 60, 12, 240, 22, IDC_FILTER, WS_EX_CLIENTEDGE);
    Ctl(pa, L"STATIC", L"Empty value = not set (mstsc default applies).", 0, 312, 15, PW - 326, 18, -1);
    ed.allList = Ctl(pa, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, 14, 42, PW - 28, PH - 42 - 46, IDC_ALLLIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(ed.allList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP | LVS_EX_LABELTIP);
    SetWindowTheme(ed.allList, L"Explorer", 0);
    const wchar_t* cols[] = { L"Setting", L"Value", L"Default", L"Description" }; int cw[] = { 210, 170, 120, 420 };
    for (int i = 0; i < 4; i++) { LVCOLUMNW c = {}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = (LPWSTR)cols[i]; c.cx = S(cw[i]); ListView_InsertColumn(ed.allList, i, &c); }
    Ctl(pa, L"BUTTON", L"Edit value...", WS_TABSTOP, 14, PH - 38, 110, 26, IDC_ALLEDIT);
    Ctl(pa, L"BUTTON", L"Unset", WS_TABSTOP, 130, PH - 38, 90, 26, IDC_ALLUNSET);
    Ctl(pa, L"BUTTON", L"Add custom...", WS_TABSTOP, 226, PH - 38, 110, 26, IDC_ALLADD);
    ed.allDesc = Ctl(pa, L"STATIC", L"", SS_NOPREFIX, 348, PH - 40, PW - 362, 34, -1);
    // bottom buttons
    Ctl(h, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 496, 558, 84, 28, IDOK);
    Ctl(h, L"BUTTON", L"Cancel", WS_TABSTOP, 588, 558, 84, 28, IDCANCEL);
    ApplyFont(h);
    Push();
    if (isNew) Pull(true);          // make every curated setting explicit in new files
    if (startPage == P_ALL) RefreshAll();
    ShowPage(startPage);
    SetFocus(startPage == P_GENERAL ? nameCtl : ed.tab);
    RunModal(h);
    if (ed.ok) { f = ed.f; name = ed.name; }
    E = 0; return ed.ok;
}

// ---------------------------------------------------------------- sessions / main window
struct Session { ws name, path; RdpFile f; };
static std::vector<Session> g_sessions;
enum {
    IDM_NEW = 200, IDM_EDIT, IDM_DUP, IDM_DELETE, IDM_IMPORT, IDM_EXPORT, IDM_FOLDER, IDM_EXIT, IDM_RELOAD, IDM_MSTSCL, IDM_IDENT,
    IDM_MSTSC, IDM_ABOUT, IDM_FOCUSSEARCH, IDM_EXPORTALL, IDC_LIST = 300, IDC_SEARCH
};

static void LoadSessions() {
    g_sessions.clear();
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((g_dataDir + L"\\*.rdp").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            Session s; s.name = fd.cFileName; s.name.resize(s.name.size() - 4); s.path = g_dataDir + L"\\" + fd.cFileName;
            if (s.f.Load(s.path)) g_sessions.push_back(s);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(g_sessions.begin(), g_sessions.end(), [](const Session& a, const Session& b) { return lstrcmpiW(a.name.c_str(), b.name.c_str()) < 0; });
}
static ws DisplaySummary(const RdpFile& f) {
    if (f.GetInt(L"screen mode id", 2) != 2) return Fmt(L"Window %d x %d", f.GetInt(L"desktopwidth", 0), f.GetInt(L"desktopheight", 0));
    if (f.GetInt(L"use multimon", 0)) {
        ws s = f.Get(L"selectedmonitors"); s = s.empty() ? L"All monitors" : L"Monitors " + s;
        return MonitorIssue(f, EnumMons()).empty() ? s : L"[!] " + s + L" - setup changed";
    }
    return L"Full screen";
}
static std::vector<int> SelectedSessions() {
    std::vector<int> v;
    for (int i = -1; (i = ListView_GetNextItem(g_list, i, LVNI_SELECTED)) >= 0;) { LVITEMW it = {}; it.mask = LVIF_PARAM; it.iItem = i; ListView_GetItem(g_list, &it); v.push_back((int)it.lParam); }
    return v;
}
static std::vector<ws> AllGroups() {
    std::vector<ws> g;
    for (auto& s : g_sessions) { ws x = s.f.Get(L"rdpman group"); if (!x.empty() && std::find_if(g.begin(), g.end(), [&](const ws& y) { return IEq(x, y); }) == g.end()) g.push_back(x); }
    std::sort(g.begin(), g.end(), [](const ws& a, const ws& b) { return lstrcmpiW(a.c_str(), b.c_str()) < 0; });
    return g;
}
static void RefreshList(const ws& selectName = L"") {
    ws keep = selectName;
    if (keep.empty()) { auto s = SelectedSessions(); if (!s.empty() && s[0] < (int)g_sessions.size()) keep = g_sessions[s[0]].name; }
    LoadSessions();
    ws filter = Lower(Trim(GetText(g_search)));
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list); ListView_RemoveAllGroups(g_list);
    auto groups = AllGroups(); bool grouped = !groups.empty();
    if (grouped) {
        groups.push_back(L"(no group)");
        for (size_t i = 0; i < groups.size(); i++) {
            LVGROUP lg = { sizeof(lg) }; lg.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_STATE; lg.pszHeader = (LPWSTR)groups[i].c_str(); lg.iGroupId = (int)i;
            lg.state = LVGS_COLLAPSIBLE; lg.stateMask = LVGS_COLLAPSIBLE; ListView_InsertGroup(g_list, -1, &lg);
        }
    }
    ListView_EnableGroupView(g_list, grouped);
    int shown = 0, selItem = -1;
    for (size_t i = 0; i < g_sessions.size(); i++) {
        const Session& s = g_sessions[i]; const RdpFile& f = s.f;
        ws grp = f.Get(L"rdpman group"), host = f.Get(L"full address"), user = f.Get(L"username"), notes = ReplaceAll(f.Get(L"rdpman notes"), L"\\n", L" ");
        ws dom = f.Get(L"domain"); if (!dom.empty() && !user.empty()) user = dom + L"\\" + user;
        ws gw = f.GetInt(L"gatewayusagemethod", 0) == 1 || f.GetInt(L"gatewayusagemethod", 0) == 2 ? f.Get(L"gatewayhostname") : L"";
        if (!filter.empty() && Lower(s.name + L" " + host + L" " + user + L" " + grp + L" " + notes + L" " + gw).find(filter) == ws::npos) continue;
        LVITEMW it = {}; it.mask = LVIF_TEXT | LVIF_PARAM | (grouped ? LVIF_GROUPID : 0); it.iItem = shown; it.pszText = (LPWSTR)s.name.c_str(); it.lParam = (LPARAM)i;
        if (grouped) { it.iGroupId = (int)groups.size() - 1; for (size_t g = 0; g < groups.size(); g++) if (IEq(groups[g], grp)) it.iGroupId = (int)g; }
        int n = ListView_InsertItem(g_list, &it);
        ws disp = DisplaySummary(f);
        ListView_SetItemText(g_list, n, 1, (LPWSTR)host.c_str()); ListView_SetItemText(g_list, n, 2, (LPWSTR)user.c_str());
        ListView_SetItemText(g_list, n, 3, (LPWSTR)disp.c_str()); ListView_SetItemText(g_list, n, 4, (LPWSTR)gw.c_str());
        ListView_SetItemText(g_list, n, 5, (LPWSTR)notes.c_str());
        if (IEq(s.name, keep)) selItem = n;
        shown++;
    }
    if (selItem < 0 && shown) selItem = 0;
    if (selItem >= 0) { ListView_SetItemState(g_list, selItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); ListView_EnsureVisible(g_list, selItem, FALSE); }
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0); InvalidateRect(g_list, 0, TRUE);
    SetWindowTextW(g_status, Fmt(L"  %d of %d session(s)   |   %s", shown, (int)g_sessions.size(), g_dataDir.c_str()).c_str());
}

static ws TempDir() { wchar_t t[MAX_PATH]; GetTempPathW(MAX_PATH, t); ws d = ws(t) + L"rdpman"; CreateDirectoryW(d.c_str(), 0); return d; }
static void CleanTemp(ULONGLONG olderThanSec) {
    ws d = TempDir(); WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((d + L"\\*.rdp").c_str(), &fd); if (h == INVALID_HANDLE_VALUE) return;
    FILETIME now; GetSystemTimeAsFileTime(&now); ULARGE_INTEGER n = { now.dwLowDateTime, now.dwHighDateTime };
    do { ULARGE_INTEGER w = { fd.ftLastWriteTime.dwLowDateTime, fd.ftLastWriteTime.dwHighDateTime };
        if (n.QuadPart - w.QuadPart > olderThanSec * 10000000ULL) DeleteFileW((d + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void CmdEdit(int idx, int page);
static bool Connect(int idx) {
    Session& s = g_sessions[idx]; RdpFile f = s.f;
    if (Trim(f.Get(L"full address")).empty()) { Msg(g_main, L"\"" + s.name + L"\" has no computer configured.", MB_ICONWARNING); return false; }
    auto mons = EnumMons(); ws issue = MonitorIssue(f, mons);
    if (!issue.empty() && !f.Get(L"selectedmonitors").empty()) {      // selected monitors no longer match the attached ones
        auto sel = ParseIds(f.Get(L"selectedmonitors")); std::vector<int> avail;
        for (int id : sel) if (FindMon(mons, id)) avail.push_back(id);
        ws use = avail.empty() ? L"one full screen monitor" : L"monitor " + JoinIds(avail);
        ws q = Fmt(L"The monitor setup no longer matches session \"%s\" (set up for monitors %s):\n\n%s\n\n", s.name.c_str(), JoinIds(sel).c_str(), issue.c_str());
        int r;
        if (g_main) {
            q += Fmt(L"Yes\t= connect anyway using %s\nNo\t= choose the monitors now\nCancel\t= do not connect", use.c_str());
            r = MessageBoxW(g_main, q.c_str(), APP, MB_YESNOCANCEL | MB_ICONWARNING);
            if (r == IDNO) { CmdEdit(idx, P_DISPLAY); return false; }
        } else {
            q += Fmt(L"Connect anyway using %s?", use.c_str());
            r = MessageBoxW(0, q.c_str(), APP, MB_YESNO | MB_ICONWARNING);
        }
        if (r != IDYES) return false;
        if (avail.empty()) { f.Remove(L"selectedmonitors"); f.SetInt(L"use multimon", 0); } else f.Set(L"selectedmonitors", 's', JoinIds(avail));
    }
    ws tmp = TempDir() + L"\\" + s.name + L".rdp";
    if (!f.Save(tmp, true)) { Msg(g_main, L"Could not write " + tmp, MB_ICONERROR); return false; }
    RunMstsc(L"\"" + tmp + L"\" " + f.Get(L"rdpman args"));
    return true;
}
static ws UniqueName(const ws& base) {
    ws n = base; for (int i = 2; FileExists(g_dataDir + L"\\" + n + L".rdp"); i++) n = Fmt(L"%s (%d)", base.c_str(), i);
    return n;
}
static void CmdNew() {
    RdpFile f; ws name;
    if (!EditSession(g_main, f, name, true, P_GENERAL, AllGroups())) return;
    if (!f.Save(g_dataDir + L"\\" + name + L".rdp")) Msg(g_main, L"Could not save the session.", MB_ICONERROR);
    RefreshList(name);
}
static void CmdEdit(int idx, int page) {
    Session s = g_sessions[idx]; ws name = s.name;
    if (!EditSession(g_main, s.f, name, false, page, AllGroups())) { RefreshList(); return; }
    ws path = g_dataDir + L"\\" + name + L".rdp";
    if (!s.f.Save(path)) { Msg(g_main, L"Could not save the session.", MB_ICONERROR); return; }
    if (!IEq(name, s.name)) DeleteFileW(s.path.c_str());
    else if (name != s.name) MoveFileW(s.path.c_str(), path.c_str());   // case-only rename
    RefreshList(name);
}
static void CmdDelete() {
    auto sel = SelectedSessions(); if (sel.empty()) return;
    ws q = sel.size() == 1 ? L"Delete session \"" + g_sessions[sel[0]].name + L"\"?" : Fmt(L"Delete %d sessions?", (int)sel.size());
    if (MessageBoxW(g_main, (q + L"\n\nThe file is moved to the recycle bin where possible.").c_str(), APP, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    ws list; for (int i : sel) { list += g_sessions[i].path; list.push_back(0); } list.push_back(0);
    SHFILEOPSTRUCTW op = {}; op.hwnd = g_main; op.wFunc = FO_DELETE; op.pFrom = list.c_str(); op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    SHFileOperationW(&op); RefreshList();
}
// Import existing .rdp files (any encoding mstsc writes); every line is kept, known or not.
static void ImportFiles(const std::vector<ws>& files) {
    ws last; int n = 0; ws failed;
    for (auto& src : files) {
        RdpFile f; ws base = src.substr(src.find_last_of(L'\\') + 1);
        bool isRdp = base.size() > 4 && IEq(base.substr(base.size() - 4), L".rdp");
        if (!isRdp || !f.Load(src) || f.lines.empty()) { failed += L"\n" + base; continue; }
        base.resize(base.size() - 4);
        ws name = UniqueName(base);
        if (f.Save(g_dataDir + L"\\" + name + L".rdp")) { n++; last = name; } else failed += L"\n" + base;
    }
    RefreshList(last);
    if (!failed.empty()) Msg(g_main, Fmt(L"%d file(s) imported. Not imported (not a readable .rdp file):", n) + failed, MB_ICONWARNING);
}
static void CmdImport() {
    std::vector<wchar_t> buf(32768, 0); OPENFILENAMEW o = { sizeof(o) }; o.hwndOwner = g_main;
    o.lpstrFilter = L"Remote Desktop files (*.rdp)\0*.rdp\0All files\0*.*\0"; o.lpstrFile = buf.data(); o.nMaxFile = (DWORD)buf.size();
    o.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY; o.lpstrTitle = L"Import .rdp files";
    if (!GetOpenFileNameW(&o)) return;
    std::vector<ws> files; ws first = buf.data(); const wchar_t* p = buf.data() + first.size() + 1;
    if (!*p) files.push_back(first); else for (; *p; p += wcslen(p) + 1) files.push_back(first + L"\\" + p);
    ImportFiles(files);
}
static void CmdExportAll() {
    BROWSEINFOW bi = {}; bi.hwndOwner = g_main; bi.lpszTitle = L"Export all sessions as plain .rdp files to this folder:"; bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pid = SHBrowseForFolderW(&bi); if (!pid) return;
    wchar_t dir[MAX_PATH]; BOOL ok = SHGetPathFromIDListW(pid, dir); CoTaskMemFree(pid); if (!ok) return;
    int n = 0, skipped = 0;
    for (auto& s : g_sessions) {
        ws dst = ws(dir) + L"\\" + s.name + L".rdp";
        if (FileExists(dst) && MessageBoxW(g_main, (dst + L"\nalready exists. Overwrite?").c_str(), APP, MB_YESNO | MB_ICONQUESTION) != IDYES) { skipped++; continue; }
        if (s.f.Save(dst, true)) n++; else skipped++;
    }
    Msg(g_main, Fmt(L"%d session(s) exported, %d skipped.", n, skipped));
}
static void CmdExport(int idx) {
    const Session& s = g_sessions[idx]; wchar_t buf[MAX_PATH]; wcscpy_s(buf, (s.name + L".rdp").c_str());
    OPENFILENAMEW o = { sizeof(o) }; o.hwndOwner = g_main; o.lpstrFilter = L"Remote Desktop files (*.rdp)\0*.rdp\0"; o.lpstrFile = buf; o.nMaxFile = MAX_PATH;
    o.lpstrDefExt = L"rdp"; o.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY; o.lpstrTitle = L"Export as plain .rdp file";
    if (GetSaveFileNameW(&o) && !s.f.Save(buf, true)) Msg(g_main, L"Could not write the file.", MB_ICONERROR);
}

static void LayoutMain() {
    RECT c; GetClientRect(g_main, &c); SendMessageW(g_status, WM_SIZE, 0, 0);
    RECT sr; GetWindowRect(g_status, &sr); int sh = sr.bottom - sr.top, top = S(42);
    MoveWindow(g_list, 0, top, c.right, std::max(0, (int)c.bottom - top - sh), TRUE);
}
static void SavePlacement() {
    WINDOWPLACEMENT wp = { sizeof(wp) }; GetWindowPlacement(g_main, &wp); RECT& r = wp.rcNormalPosition;
    ws v = Fmt(L"%d,%d,%d,%d,%d", r.left, r.top, r.right, r.bottom, wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
    WritePrivateProfileStringW(L"window", L"placement", v.c_str(), g_iniPath.c_str());
    ws cw; for (int i = 0; i < 6; i++) cw += std::to_wstring(MulDiv(ListView_GetColumnWidth(g_list, i), 96, g_dpi)) + L",";
    WritePrivateProfileStringW(L"window", L"columns", cw.c_str(), g_iniPath.c_str());
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g_main = h;
        HMENU mb = CreateMenu(), mf = CreatePopupMenu(), ms = CreatePopupMenu(), mt = CreatePopupMenu(), mh = CreatePopupMenu();
        AppendMenuW(mf, MF_STRING, IDM_NEW, L"&New session...\tCtrl+N"); AppendMenuW(mf, MF_STRING, IDM_IMPORT, L"&Import .rdp files...\tCtrl+I");
        AppendMenuW(mf, MF_STRING, IDM_EXPORT, L"&Export as .rdp...\tCtrl+E"); AppendMenuW(mf, MF_STRING, IDM_EXPORTALL, L"Export &all to folder...");
        AppendMenuW(mf, MF_SEPARATOR, 0, 0);
        AppendMenuW(mf, MF_STRING, IDM_FOLDER, L"Open &sessions folder"); AppendMenuW(mf, MF_STRING, IDM_RELOAD, L"&Reload\tF5");
        AppendMenuW(mf, MF_SEPARATOR, 0, 0); AppendMenuW(mf, MF_STRING, IDM_EXIT, L"E&xit");
        AppendMenuW(ms, MF_STRING, IDOK, L"&Connect\tEnter"); AppendMenuW(ms, MF_STRING, IDM_EDIT, L"&Edit...\tF2");
        AppendMenuW(ms, MF_STRING, IDM_DUP, L"D&uplicate\tCtrl+D"); AppendMenuW(ms, MF_STRING, IDM_DELETE, L"&Delete\tDel");
        AppendMenuW(mt, MF_STRING, IDM_IDENT, L"&Identify monitors"); AppendMenuW(mt, MF_STRING, IDM_MSTSCL, L"&List monitors (mstsc /l)");
        AppendMenuW(mt, MF_STRING, IDM_MSTSC, L"&Open Remote Desktop Connection (mstsc)");
        AppendMenuW(mh, MF_STRING, IDM_ABOUT, L"&About");
        AppendMenuW(mb, MF_POPUP, (UINT_PTR)mf, L"&File"); AppendMenuW(mb, MF_POPUP, (UINT_PTR)ms, L"&Session");
        AppendMenuW(mb, MF_POPUP, (UINT_PTR)mt, L"&Tools"); AppendMenuW(mb, MF_POPUP, (UINT_PTR)mh, L"&Help"); SetMenu(h, mb);
        g_search = Ctl(h, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 8, 10, 220, 23, IDC_SEARCH, WS_EX_CLIENTEDGE);
        Edit_SetCueBannerText(g_search, L"Search (Ctrl+F)");
        int x = 236; struct { const wchar_t* t; int id, w; } b[] = { {L"Connect", IDOK, 90}, {L"New...", IDM_NEW, 76}, {L"Edit...", IDM_EDIT, 76}, {L"Duplicate", IDM_DUP, 84}, {L"Delete", IDM_DELETE, 76} };
        for (auto& bb : b) { Ctl(h, L"BUTTON", bb.t, (bb.id == IDOK ? BS_DEFPUSHBUTTON : 0) | WS_TABSTOP, x, 8, bb.w, 27, bb.id); x += bb.w + 6; }
        g_list = Ctl(h, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_TABSTOP, 0, 42, 100, 100, IDC_LIST);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        SetWindowTheme(g_list, L"Explorer", 0);
        const wchar_t* cols[] = { L"Name", L"Computer", L"User", L"Display", L"Gateway", L"Notes" }; int cw[] = { 200, 180, 140, 240, 110, 150 };
        wchar_t saved[128]; GetPrivateProfileStringW(L"window", L"columns", L"", saved, 128, g_iniPath.c_str()); auto sv = ParseIds(saved);
        for (int i = 0; i < 6; i++) { LVCOLUMNW c = {}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = (LPWSTR)cols[i]; c.cx = S(sv.size() == 6 && sv[i] > 10 ? sv[i] : cw[i]); ListView_InsertColumn(g_list, i, &c); }
        g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, h, 0, g_inst, 0);
        ApplyFont(h); RefreshList(); SetFocus(g_list);
        return 0; }
    case WM_SIZE: LayoutMain(); return 0;
    case WM_DISPLAYCHANGE: if (!E) RefreshList(); break;     // monitor added/removed/rearranged: re-check all sessions
    case WM_DROPFILES: {
        HDROP d = (HDROP)w; std::vector<ws> files; UINT n = DragQueryFileW(d, 0xFFFFFFFF, 0, 0);
        for (UINT i = 0; i < n; i++) { wchar_t p[MAX_PATH * 2]; if (DragQueryFileW(d, i, p, _countof(p))) files.push_back(p); }
        DragFinish(d); ImportFiles(files); return 0; }
    case WM_GETMINMAXINFO: ((MINMAXINFO*)l)->ptMinTrackSize = { S(680), S(300) }; return 0;
    case WM_SETFOCUS: SetFocus(g_list); return 0;
    case DM_GETDEFID: return MAKELONG(IDOK, DC_HASDEFID);
    case WM_DPICHANGED: {
        int old = g_dpi; g_dpi = HIWORD(w); MakeFont(); RECT* r = (RECT*)l;
        for (HWND c = GetWindow(h, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            if (c == g_list || c == g_status) continue;
            RECT cr; GetWindowRect(c, &cr); MapWindowPoints(0, h, (POINT*)&cr, 2);
            MoveWindow(c, MulDiv(cr.left, g_dpi, old), MulDiv(cr.top, g_dpi, old), MulDiv(cr.right - cr.left, g_dpi, old), MulDiv(cr.bottom - cr.top, g_dpi, old), TRUE);
        }
        for (int i = 0; i < 6; i++) ListView_SetColumnWidth(g_list, i, MulDiv(ListView_GetColumnWidth(g_list, i), g_dpi, old));
        ApplyFont(h); SetWindowPos(h, 0, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE); LayoutMain();
        return 0; }
    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)l; if (nh->idFrom != IDC_LIST) break;
        if (nh->code == NM_DBLCLK) SendMessageW(h, WM_COMMAND, IDOK, 0);
        else if (nh->code == LVN_KEYDOWN) {
            WORD k = ((NMLVKEYDOWN*)l)->wVKey;
            if (k == VK_DELETE) CmdDelete(); else if (k == VK_INSERT) CmdNew();
        } else if (nh->code == NM_RCLICK) {
            HMENU pm = CreatePopupMenu(); bool any = !SelectedSessions().empty(); UINT g = any ? 0 : MF_GRAYED;
            AppendMenuW(pm, MF_STRING | g, IDOK, L"Connect"); AppendMenuW(pm, MF_STRING | g, IDM_EDIT, L"Edit..."); AppendMenuW(pm, MF_STRING | g, IDM_DUP, L"Duplicate");
            AppendMenuW(pm, MF_STRING | g, IDM_EXPORT, L"Export as .rdp..."); AppendMenuW(pm, MF_SEPARATOR, 0, 0); AppendMenuW(pm, MF_STRING | g, IDM_DELETE, L"Delete");
            AppendMenuW(pm, MF_SEPARATOR, 0, 0); AppendMenuW(pm, MF_STRING, IDM_NEW, L"New session...");
            SetMenuDefaultItem(pm, IDOK, FALSE);
            POINT pt; GetCursorPos(&pt); TrackPopupMenu(pm, TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, 0); DestroyMenu(pm);
        }
        return 0; }
    case WM_COMMAND: {
        int id = LOWORD(w); auto sel = SelectedSessions();
        switch (id) {
        case IDOK: for (int i : sel) if (!Connect(i)) break; break;
        case IDCANCEL: if (GetWindowTextLengthW(g_search)) { SetWindowTextW(g_search, L""); SetFocus(g_list); } break;
        case IDC_SEARCH: if (HIWORD(w) == EN_CHANGE) RefreshList(); break;
        case IDM_FOCUSSEARCH: SetFocus(g_search); Edit_SetSel(g_search, 0, -1); break;
        case IDM_NEW: CmdNew(); break;
        case IDM_EDIT: if (!sel.empty()) CmdEdit(sel[0], P_GENERAL); break;
        case IDM_DUP: if (!sel.empty()) { const Session& s = g_sessions[sel[0]]; ws n = UniqueName(s.name + L" - copy"); s.f.Save(g_dataDir + L"\\" + n + L".rdp"); RefreshList(n); } break;
        case IDM_DELETE: CmdDelete(); break;
        case IDM_IMPORT: CmdImport(); break;
        case IDM_EXPORT: if (!sel.empty()) CmdExport(sel[0]); break;
        case IDM_EXPORTALL: CmdExportAll(); break;
        case IDM_FOLDER: ShellExecuteW(h, L"open", g_dataDir.c_str(), 0, 0, SW_SHOWNORMAL); break;
        case IDM_RELOAD: RefreshList(); break;
        case IDM_IDENT: IdentifyMonitors(); break;
        case IDM_MSTSCL: RunMstsc(L"/l"); break;
        case IDM_MSTSC: RunMstsc(L""); break;
        case IDM_EXIT: SendMessageW(h, WM_CLOSE, 0, 0); break;
        case IDM_ABOUT: Msg(h, L"RDP Manager 1.0\n\nPortable manager for Remote Desktop sessions.\nSessions are ordinary .rdp files stored in:\n" + g_dataDir +
            L"\n\nConnections are opened with the Windows Remote Desktop client (mstsc.exe).\n\nCommand line:  rdpman.exe \"session name\"  connects directly."); break;
        }
        return 0; }
    case WM_CLOSE: SavePlacement(); DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void InitPaths() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(0, exe, MAX_PATH); ws dir = exe; dir.resize(dir.find_last_of(L'\\'));
    auto usable = [](const ws& d) {
        CreateDirectoryW(d.c_str(), 0);
        HANDLE h = CreateFileW((d + L"\\.rdpman-write-test").c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, 0);
        if (h == INVALID_HANDLE_VALUE) return false; CloseHandle(h); return true;
    };
    if (!usable(dir + L"\\sessions")) {          // exe folder is read-only: fall back to the user profile
        wchar_t ad[MAX_PATH]; SHGetFolderPathW(0, CSIDL_APPDATA, 0, 0, ad); dir = ws(ad) + L"\\rdpman"; CreateDirectoryW(dir.c_str(), 0); usable(dir + L"\\sessions");
    }
    g_dataDir = dir + L"\\sessions"; g_iniPath = dir + L"\\rdpman.ini";
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    g_inst = inst; g_dpi = GetDpiForSystem();
    INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES }; InitCommonControlsEx(&ic);
    InitPaths(); CleanTemp(24 * 3600);

    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1) {                               // rdpman.exe "session name" -> connect and exit
        LoadSessions();
        for (size_t i = 0; i < g_sessions.size(); i++) if (IEq(g_sessions[i].name, argv[1])) return Connect((int)i) ? 0 : 1;
        Msg(0, ws(L"Session not found: ") + argv[1], MB_ICONERROR); return 2;
    }

    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH); HICON big = 0, small_ = 0;
    ExtractIconExW((ws(sys) + L"\\mstsc.exe").c_str(), 0, &big, &small_, 1);
    WNDCLASSEXW wc = { sizeof(wc) }; wc.hInstance = inst; wc.hCursor = LoadCursorW(0, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpfnWndProc = MainProc; wc.lpszClassName = L"RdpManMain"; wc.hIcon = big; wc.hIconSm = small_; RegisterClassExW(&wc);
    wc.hIcon = wc.hIconSm = 0;
    wc.lpfnWndProc = EditorProc; wc.lpszClassName = L"RdpManEditor"; RegisterClassExW(&wc);
    wc.lpfnWndProc = PromptProc; wc.lpszClassName = L"RdpManPrompt"; RegisterClassExW(&wc);
    wc.lpfnWndProc = IdentProc; wc.lpszClassName = L"RdpManIdent"; RegisterClassExW(&wc);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpfnWndProc = PageProc; wc.lpszClassName = L"RdpManPage"; RegisterClassExW(&wc);
    wc.lpfnWndProc = PickerProc; wc.lpszClassName = L"RdpManMonPick"; wc.hCursor = LoadCursorW(0, IDC_HAND); RegisterClassExW(&wc);

    // restore the window on the monitor it was on (dpi of that monitor decides the scale)
    wchar_t pl[128]; GetPrivateProfileStringW(L"window", L"placement", L"", pl, 128, g_iniPath.c_str());
    int v[5] = {}; bool havePl = swscanf_s(pl, L"%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4]) == 5 && v[2] > v[0] && v[3] > v[1];
    RECT want = { v[0], v[1], v[2], v[3] };
    HMONITOR mon = havePl ? MonitorFromRect(&want, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) { typedef HRESULT(WINAPI* GDFM)(HMONITOR, int, UINT*, UINT*); GDFM fn = (GDFM)GetProcAddress(shcore, "GetDpiForMonitor"); UINT dx, dy; if (fn && SUCCEEDED(fn(mon, 0, &dx, &dy))) g_dpi = dx; }
    MakeFont();
    HWND h = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_ACCEPTFILES, L"RdpManMain", APP, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S(1040), S(560), 0, 0, inst, 0);
    if (!h) return 1;
    if (havePl) { WINDOWPLACEMENT wp = { sizeof(wp) }; wp.rcNormalPosition = want; wp.showCmd = v[4] ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL; SetWindowPlacement(h, &wp); }
    else ShowWindow(h, show);
    UpdateWindow(h);

    ACCEL acc[] = { {FVIRTKEY | FCONTROL, 'N', IDM_NEW}, {FVIRTKEY | FCONTROL, 'D', IDM_DUP}, {FVIRTKEY | FCONTROL, 'F', IDM_FOCUSSEARCH}, {FVIRTKEY | FCONTROL, 'I', IDM_IMPORT},
        {FVIRTKEY | FCONTROL, 'E', IDM_EXPORT}, {FVIRTKEY, VK_F2, IDM_EDIT}, {FVIRTKEY, VK_F5, IDM_RELOAD} };
    HACCEL ha = CreateAcceleratorTableW(acc, _countof(acc));
    MSG msg;
    while (GetMessageW(&msg, 0, 0, 0)) {
        if (GetAncestor(msg.hwnd, GA_ROOT) == g_main && (TranslateAcceleratorW(g_main, ha, &msg) || IsDialogMessageW(g_main, &msg))) continue;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    CleanTemp(120);
    return 0;
}
