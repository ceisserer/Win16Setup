/*
 * setup.c - Win16Setup: a graphical installer and uninstaller for Windows
 * 3.1 programs. Everything about the product comes from SETUP.INF next to
 * SETUP.EXE (see README.md for the format).
 *
 *   SETUP                     install; if installed: change options or remove
 *   SETUP /S [/D=dir] [/<switch>=item] [/[NO]LOAD] [/[NO]GROUP] [/[NO]START]
 *         [/[NO]<option>] [/RESTART]
 *                             silent install with defaults or these options
 *   SETUP /U [/S]             remove (asks first unless /S)
 *   SETUP /RESUME             finish a shell handover (the staged copy in
 *                             TEMP\W16STAGE does so by itself)
 *
 * Installing: stop the product's running programs ([Stop]), copy [Files],
 * write [Ini] entries, add the [Autostart] program to WIN.INI load=,
 * create the Program Manager [Group] over DDE, start [StartNow]. SETUP.EXE
 * and SETUP.INF are installed too, for changing options and removing; the
 * installation is recorded in WIN.INI [<product>] (InstallDir, Version,
 * Choice).
 *
 * [Options] adds check boxes for optional features. Each one can change INI
 * entries (also in SYSTEM.INI; the old values are kept in WIN.INI and put
 * back when the option is turned off or the product removed), add programs
 * to load=, start a program and ask for a restart of Windows.
 *
 * Shell handover: a product that is the running Windows shell (SYSTEM.INI
 * shell= in the installation directory) cannot be stopped - ending the
 * shell ends Windows. To replace or remove its files Setup copies itself
 * to TEMP\W16STAGE, makes Program Manager the shell, adds the copy to
 * WIN.INI run= (its command line is kept in WIN.INI) and restarts Windows; the resumed Setup puts
 * shell= back, does the work and restarts Windows again if needed.
 *
 * Removing with the installed copy: a running EXE cannot delete itself, so
 * SETUP copies itself to the TEMP directory under another module name
 * (W16UNINS: Windows would otherwise start a second instance of the running
 * module W16SETUP instead of loading the copy) and starts the copy with
 * /X=dir; it waits until W16SETUP has ended and removes the files. The
 * copy is deleted by the next run of any Win16Setup.
 *
 * A log of every run goes to SETUP.LOG in the installation directory
 * (W16UNINS.LOG in TEMP when removing).
 */
#include <windows.h>
#include <ddeml.h>
#include <dos.h>
#include <direct.h>
#include <string.h>
#include "setup.h"

#define MAX_FILES   128
#define MAX_ITEMS   16
#define MAX_GROUP   12
#define MAX_OPTIONS 4
#define MAX_PATHLEN 144
#define MAX_TEXT    300

#define MODNAME     "W16SETUP"
#define MODNAME_TMP "W16UNINS"
#define STAGE_DIR   "W16STAGE"

/* an [Options] entry: section [Option.<name>] */
typedef struct {
    char name[16];
    char sw[12];                    /* command line switch (/SW, /NOSW) */
    BOOL on;                        /* chosen */
    BOOL was_on;                    /* recorded in WIN.INI [<key>] */
    BOOL restart;                   /* changing it needs a restart of Windows */
} OptionDef;

typedef struct {
    char dir[MAX_PATHLEN];          /* without trailing '\' */
    char choice[16];
    BOOL load, group, start;
} Options;

static HINSTANCE g_inst;
static char g_src[MAX_PATHLEN];     /* directory of SETUP.EXE, with '\' */
static char g_inf[MAX_PATHLEN + 16];
static char g_product[40];
static char g_key[40];              /* WIN.INI section of the installation */
static char g_ver[24];
static char g_title[64];
static char g_files[MAX_FILES][64]; /* paths relative to the directory */
static int  g_nfiles;
static char g_items[MAX_ITEMS][16]; /* [Choice] */
static char g_names[MAX_ITEMS][48];
static int  g_nitems;
static char g_switch[8];            /* command line switch of the choice */
static char g_preview[64];          /* preview image pattern (%ITEM%) */
static char g_auto[64];             /* [Autostart] Program= */
static char g_group[40];            /* [Group] Name= */
static char g_run[64];              /* [StartNow] Program= */
static BOOL g_silent;
static BOOL g_nostart;              /* /NOSTART: start no programs */
static BOOL g_resumed;              /* second half of a shell handover */
static BOOL g_need_restart;         /* an option needing a restart changed */
static BOOL g_restart_now;          /* restart Windows when Setup ends */
static BOOL g_cmd_restart;          /* /RESTART: silent installs may restart */
static OptionDef g_opts[MAX_OPTIONS];
static int  g_nopts;
static BOOL g_installed;            /* WIN.INI [<key>] InstallDir= exists */
static Options g_opt;
static HWND g_back;                 /* blue background window */
static HWND g_prog;                 /* progress dialog */
static int  g_pct;
static HGLOBAL g_dib;               /* preview: BITMAPINFO + bits */
static UINT g_dib_bits;             /* offset of the bits in g_dib */
static HPALETTE g_pal;
static char g_buf[4096];
static char g_log[2048];

/* ---- small helpers ---------------------------------------------------- */

static void log_line(LPCSTR s)
{
    int n = lstrlen(g_log), m = lstrlen(s);
    if (n + m + 3 < sizeof(g_log)) {
        lstrcpy(g_log + n, s);
        lstrcpy(g_log + n + m, "\r\n");
    }
}

/* Appends the log to a file and empties it. */
static BOOL log_flush(LPCSTR path)
{
    HFILE f = _lopen(path, OF_WRITE);
    if (f == HFILE_ERROR)
        f = _lcreat(path, 0);
    if (f == HFILE_ERROR)
        return FALSE;
    _llseek(f, 0, 2);
    _lwrite(f, g_log, lstrlen(g_log));
    _lclose(f);
    g_log[0] = 0;
    return TRUE;
}

/* Logs a message and shows it unless silent. */
static void say(LPCSTR s, UINT icon)
{
    log_line(s);
    if (!g_silent)
        MessageBox(g_back, s, g_title, MB_OK | icon);
}

static void pump(void)
{
    MSG m;
    while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) {
        if (g_prog && IsDialogMessage(g_prog, &m))
            continue;
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
}

static void add_slash(char *dir)
{
    int n = lstrlen(dir);
    if (n && dir[n - 1] != '\\')
        lstrcpy(dir + n, "\\");
}

static void cut_slash(char *dir)
{
    int n = lstrlen(dir);
    if (n > 3 && dir[n - 1] == '\\')
        dir[n - 1] = 0;
}

/* Pointer to the file name part of a path. */
static char *base_name(char *path)
{
    char *p = path, *b = path;
    for (; *p; p++)
        if (*p == '\\' || *p == ':' || *p == '/')
            b = p + 1;
    return b;
}

static BOOL file_exists(LPCSTR path)
{
    OFSTRUCT of;
    return OpenFile(path, &of, OF_EXIST) != HFILE_ERROR;
}

static void file_delete(LPCSTR path)
{
    OFSTRUCT of;
    _dos_setfileattr(path, _A_NORMAL);
    OpenFile(path, &of, OF_DELETE);
}

static long file_size(LPCSTR path)
{
    long n;
    HFILE f = _lopen(path, OF_READ | OF_SHARE_DENY_NONE);
    if (f == HFILE_ERROR)
        return -1;
    n = _llseek(f, 0, 2);
    _lclose(f);
    return n;
}

/* Creates every directory on the way to a file. */
static void make_dirs(const char *file)
{
    char d[MAX_PATHLEN + 64];
    int i;
    lstrcpy(d, file);
    for (i = 3; d[i]; i++) {
        if (d[i] == '\\') {
            d[i] = 0;
            mkdir(d);
            d[i] = '\\';
        }
    }
}

/* Copies a file with its date and time. */
static BOOL copy_file(const char *from, const char *to)
{
    HFILE in, out;
    UINT n;
    unsigned date, time;
    BOOL ok = TRUE;
    in = _lopen(from, OF_READ | OF_SHARE_DENY_WRITE);
    if (in == HFILE_ERROR)
        return FALSE;
    _dos_setfileattr(to, _A_NORMAL);
    out = _lcreat(to, 0);
    if (out == HFILE_ERROR) {
        _lclose(in);
        return FALSE;
    }
    for (;;) {
        n = _lread(in, g_buf, sizeof(g_buf));
        if (n == (UINT)HFILE_ERROR) {
            ok = FALSE;
            break;
        }
        if (n == 0)
            break;
        if (_lwrite(out, g_buf, n) != n) {
            ok = FALSE;
            break;
        }
    }
    if (ok && _dos_getftime(in, &date, &time) == 0)
        _dos_setftime(out, date, time);
    _lclose(in);
    _lclose(out);
    if (!ok)
        file_delete(to);
    return ok;
}

/* Renames the module of an NE executable (same length): the first entry of
 * the resident name table. */
static BOOL patch_modname(const char *exe, const char *from, const char *to)
{
    HFILE f;
    long ne;
    WORD restab;
    BYTE len;
    char name[16];
    BOOL ok = FALSE;
    f = _lopen(exe, OF_READWRITE);
    if (f == HFILE_ERROR)
        return FALSE;
    if (_llseek(f, 0x3C, 0) == 0x3C && _lread(f, &ne, 4) == 4 && _llseek(f, ne + 0x26, 0) == ne + 0x26
        && _lread(f, &restab, 2) == 2 && _llseek(f, ne + restab, 0) == ne + restab
        && _lread(f, &len, 1) == 1 && len == (BYTE)lstrlen(from) && len < sizeof(name)
        && _lread(f, name, len) == len) {
        name[len] = 0;
        if (lstrcmpi(name, from) == 0) {
            _llseek(f, ne + restab + 1, 0);
            ok = _lwrite(f, to, len) == len;
        }
    }
    _lclose(f);
    return ok;
}

/* ---- SETUP.INF ---------------------------------------------------------- */

static int inf_get(LPCSTR section, LPCSTR key, LPCSTR def, char *buf, int size)
{
    return GetPrivateProfileString(section, key, def, buf, size, g_inf);
}

/* Entry n (1, 2, ...) of a numbered section. */
static BOOL inf_entry(LPCSTR section, int n, char *buf, int size)
{
    char key[8];
    wsprintf(key, "%d", n);
    return inf_get(section, key, "", buf, size) > 0;
}

/* Replaces %DIR%, %SRC%, %WINDIR%, %PRODUCT%, %VERSION%, %CHOICE% and
 * %ITEM%; "\n" becomes a line break. */
static void expand(LPCSTR in, char *out, int size, LPCSTR item)
{
    static const char *names[] = { "DIR", "SRC", "WINDIR", "PRODUCT", "VERSION", "CHOICE", "ITEM" };
    char val[MAX_PATHLEN], name[12];
    int n = 0, i, k;
    while (*in && n < size - 1) {
        if (in[0] == '\\' && in[1] == 'n') {
            if (n < size - 3) {
                out[n++] = '\r';
                out[n++] = '\n';
            }
            in += 2;
            continue;
        }
        if (*in == '%') {
            for (k = 1; k < sizeof(name) && in[k] && in[k] != '%'; k++)
                name[k - 1] = in[k];
            name[k - 1] = 0;
            if (in[k] == '%') {
                for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
                    if (lstrcmpi(name, names[i]) == 0)
                        break;
                val[0] = 0;
                switch (i) {
                case 0: lstrcpy(val, g_opt.dir); break;
                case 1: lstrcpy(val, g_src); cut_slash(val); break;
                case 2: GetWindowsDirectory(val, sizeof(val)); cut_slash(val); break;
                case 3: lstrcpy(val, g_product); break;
                case 4: lstrcpy(val, g_ver); break;
                case 5: lstrcpy(val, g_opt.choice); break;
                case 6: lstrcpy(val, item ? item : ""); break;
                default: i = -1;
                }
                if (i >= 0) {
                    lstrcpyn(out + n, val, size - n);
                    n += lstrlen(out + n);
                    in += k + 1;
                    continue;
                }
            }
        }
        out[n++] = *in++;
    }
    out[n] = 0;
}

static void inf_text(LPCSTR section, LPCSTR key, LPCSTR def, char *out, int size)
{
    char raw[MAX_TEXT];
    inf_get(section, key, def, raw, sizeof(raw));
    expand(raw, out, size, NULL);
}

/* Reads SETUP.INF (g_inf). */
static BOOL load_inf(void)
{
    char t[MAX_TEXT], *c;
    int i;
    g_nfiles = g_nitems = 0;
    if (!file_exists(g_inf))
        return FALSE;
    inf_get("Setup", "Product", "", g_product, sizeof(g_product));
    inf_get("Setup", "Version", "", g_ver, sizeof(g_ver));
    inf_get("Setup", "Key", g_product, g_key, sizeof(g_key));
    inf_text("Setup", "Title", "%PRODUCT% Setup", g_title, sizeof(g_title));
    for (i = 1; g_nfiles < MAX_FILES && inf_entry("Files", i, g_files[g_nfiles], sizeof(g_files[0])); i++)
        g_nfiles++;
    /* choice: "item" or "item,display name"; names may come from a file */
    inf_get("Choice", "Switch", "C", g_switch, sizeof(g_switch));
    AnsiUpper(g_switch);
    inf_get("Choice", "Preview", "", g_preview, sizeof(g_preview));
    for (i = 1; g_nitems < MAX_ITEMS && inf_entry("Choice", i, t, sizeof(t)); i++) {
        c = _fstrchr(t, ',');
        if (c)
            *c++ = 0;
        lstrcpyn(g_items[g_nitems], t, sizeof(g_items[0]));
        if (c) {
            lstrcpyn(g_names[g_nitems], c, sizeof(g_names[0]));
        } else {
            /* NameFrom=file,section,key (relative to the setup directory) */
            char spec[MAX_TEXT], file[MAX_PATHLEN + 64], *sec, *key;
            lstrcpy(g_names[g_nitems], t);
            inf_get("Choice", "NameFrom", "", spec, sizeof(spec));
            sec = _fstrchr(spec, ',');
            key = sec ? _fstrchr(sec + 1, ',') : NULL;
            if (key) {
                *sec++ = 0;
                *key++ = 0;
                expand(spec, t, sizeof(t), g_items[g_nitems]);
                wsprintf(file, "%s%s", (LPSTR)g_src, (LPSTR)t);
                GetPrivateProfileString(sec, key, g_items[g_nitems], g_names[g_nitems], sizeof(g_names[0]),
                                        file);
            }
        }
        g_nitems++;
    }
    g_nopts = 0;
    for (i = 1; g_nopts < MAX_OPTIONS && inf_entry("Options", i, t, sizeof(t)); i++) {
        OptionDef *o = &g_opts[g_nopts++];
        char sec[32];
        _fmemset(o, 0, sizeof(*o));
        lstrcpyn(o->name, t, sizeof(o->name));
        wsprintf(sec, "Option.%s", (LPSTR)o->name);
        inf_get(sec, "Switch", o->name, o->sw, sizeof(o->sw));
        AnsiUpper(o->sw);
        o->restart = GetPrivateProfileInt(sec, "Restart", 0, g_inf) != 0;
    }
    inf_get("Autostart", "Program", "", g_auto, sizeof(g_auto));
    inf_get("Group", "Name", "", g_group, sizeof(g_group));
    inf_get("StartNow", "Program", "", g_run, sizeof(g_run));
    return g_product[0] && g_nfiles > 0;
}

static int item_index(const char *name)
{
    int i;
    for (i = 0; i < g_nitems; i++)
        if (lstrcmpi(g_items[i], name) == 0)
            return i;
    return -1;
}

/* ---- WIN.INI ------------------------------------------------------------ */

/* Whether a list entry is the program: a match containing '\\' must be
 * the same path, otherwise the file names are compared. */
static BOOL same_program(const char *tok, const char *match)
{
    char a[128], b[128];
    lstrcpyn(a, tok, sizeof(a));
    lstrcpyn(b, match, sizeof(b));
    if (_fstrchr(b, '\\'))
        return lstrcmpi(a, b) == 0;
    return lstrcmpi(base_name(a), base_name(b)) == 0;
}

/* Rewrites a program list of WIN.INI [windows] (load=, run=) without the
 * entries for match, then adds add if given. Returns whether an entry was
 * there. */
static BOOL edit_list(LPCSTR key, const char *match, const char *add)
{
    char cur[256], out[256], tok[128];
    const char *p;
    int n;
    BOOL found = FALSE;
    GetProfileString("windows", key, "", cur, sizeof(cur));
    out[0] = 0;
    for (p = cur; *p;) {
        while (*p == ' ' || *p == ',' || *p == '\t')
            p++;
        for (n = 0; *p && *p != ' ' && *p != ',' && *p != '\t'; p++)
            if (n < sizeof(tok) - 1)
                tok[n++] = *p;
        tok[n] = 0;
        if (!n)
            continue;
        if (match && match[0] && same_program(tok, match)) {
            found = TRUE;
            continue;
        }
        if (lstrlen(out) + n + 2 < sizeof(out)) {
            if (out[0])
                lstrcat(out, " ");
            lstrcat(out, tok);
        }
    }
    if (add && lstrlen(out) + lstrlen(add) + 2 < sizeof(out)) {
        if (out[0])
            lstrcat(out, " ");
        lstrcat(out, add);
    }
    if (add || found)
        WriteProfileString("windows", key, out);
    return found;
}

static BOOL list_has(LPCSTR key, const char *match)
{
    char cur[256], tok[128];
    const char *p;
    int n;
    if (!match || !match[0])
        return FALSE;
    GetProfileString("windows", key, "", cur, sizeof(cur));
    for (p = cur; *p;) {
        while (*p == ' ' || *p == ',' || *p == '\t')
            p++;
        for (n = 0; *p && *p != ' ' && *p != ',' && *p != '\t'; p++)
            if (n < sizeof(tok) - 1)
                tok[n++] = *p;
        tok[n] = 0;
        if (n && same_program(tok, match))
            return TRUE;
    }
    return FALSE;
}

/* [Autostart]: load= without the program, plus exe if given */
static BOOL edit_load(const char *exe)
{
    return edit_list("load", g_auto, exe);
}

static BOOL load_has_program(void)
{
    return list_has("load", g_auto);
}

/* ---- Program Manager ---------------------------------------------------- */

HDDEDATA CALLBACK __export dde_callback(UINT type, UINT fmt, HCONV conv, HSZ s1, HSZ s2,
                                        HDDEDATA data, DWORD d1, DWORD d2)
{
    (void)type; (void)fmt; (void)conv; (void)s1; (void)s2; (void)data; (void)d1; (void)d2;
    return 0;
}

/* Runs Program Manager DDE commands; each one separately, so a failing
 * one (DeleteGroup of a missing group) does not stop the others. */
static BOOL progman(char (*cmds)[200], int n)
{
    DWORD inst = 0;
    FARPROC cb;
    HSZ hsz;
    HCONV conv;
    BOOL ok = FALSE;
    int i;
    cb = MakeProcInstance((FARPROC)dde_callback, g_inst);
    if (DdeInitialize(&inst, (PFNCALLBACK)cb, APPCMD_CLIENTONLY, 0) != DMLERR_NO_ERROR) {
        FreeProcInstance(cb);
        return FALSE;
    }
    hsz = DdeCreateStringHandle(inst, "PROGMAN", CP_WINANSI);
    conv = DdeConnect(inst, hsz, hsz, NULL);
    if (conv) {
        ok = TRUE;
        for (i = 0; i < n; i++) {
            HDDEDATA r = DdeClientTransaction((LPVOID)cmds[i], lstrlen(cmds[i]) + 1, conv, NULL, CF_TEXT,
                                             XTYP_EXECUTE, 10000, NULL);
            if (!r && i > 0)    /* the first command may fail (DeleteGroup) */
                ok = FALSE;
            log_line(cmds[i]);
        }
        DdeDisconnect(conv);
    }
    DdeFreeStringHandle(inst, hsz);
    DdeUninitialize(inst);
    FreeProcInstance(cb);
    return ok;
}

/* [Group]: Name= and items 1=command line,title[,icon file[,icon index]]. */
static BOOL make_group(void)
{
    static char c[MAX_GROUP + 2][200];
    char t[MAX_TEXT], e[MAX_TEXT];
    int i, n = 0;
    wsprintf(c[n++], "[DeleteGroup(%s)]", (LPSTR)g_group);
    wsprintf(c[n++], "[CreateGroup(%s)]", (LPSTR)g_group);
    for (i = 1; n < MAX_GROUP + 2 && inf_entry("Group", i, t, sizeof(t)); i++) {
        expand(t, e, sizeof(e), NULL);
        if (lstrlen(e) < 180)
            wsprintf(c[n++], "[AddItem(%s)]", (LPSTR)e);
    }
    return progman(c, n);
}

static void remove_group(void)
{
    static char c[1][200];
    if (!g_group[0])
        return;
    wsprintf(c[0], "[DeleteGroup(%s)]", (LPSTR)g_group);
    progman(c, 1);
}

/* ---- running programs --------------------------------------------------- */

/* Whether any module of a comma-separated list is loaded. */
static BOOL modules_loaded(const char *list)
{
    char t[MAX_TEXT], *p, *q;
    lstrcpy(t, list);
    for (p = t; *p; p = q) {
        q = _fstrchr(p, ',');
        if (q)
            *q++ = 0;
        else
            q = p + lstrlen(p);
        if (*p && GetModuleHandle(p))
            return TRUE;
    }
    return FALSE;
}

/* The module of the product that is the running Windows shell: SYSTEM.INI
 * shell= names a program in the installation directory and it is loaded. */
static BOOL shell_module(char *module)
{
    char sh[MAX_PATHLEN], dir[MAX_PATHLEN], *p, *b;
    GetPrivateProfileString("boot", "shell", "", sh, sizeof(sh), "SYSTEM.INI");
    for (p = sh; *p && *p != ' '; p++)
        ;
    *p = 0;
    b = base_name(sh);
    if (b == sh)
        return FALSE;
    lstrcpyn(module, b, 9);
    p = _fstrchr(module, '.');
    if (p)
        *p = 0;
    b[-1] = 0;
    lstrcpy(dir, g_opt.dir);
    cut_slash(dir);
    cut_slash(sh);
    if (lstrcmpi(sh, dir) != 0)
        return FALSE;
    return GetModuleHandle(module) != NULL;
}

/* Window=class (sent WM_CLOSE), Wait=modules (waited for) of a section. */
static BOOL stop_programs(LPCSTR section)
{
    char cls[64], wait[MAX_TEXT], m[MAX_TEXT];
    HWND w;
    DWORD t0 = GetTickCount();
    inf_get(section, "Window", "", cls, sizeof(cls));
    inf_get(section, "Wait", "", wait, sizeof(wait));
    w = cls[0] ? FindWindow(cls, NULL) : NULL;
    if (w || modules_loaded(wait)) {
        log_line("stopping a running program");
        if (w)
            PostMessage(w, WM_CLOSE, 0, 0);
        while ((cls[0] && FindWindow(cls, NULL)) || modules_loaded(wait)) {
            if (GetTickCount() - t0 > 15000) {
                wsprintf(m, "%s could not be stopped. Close it and run Setup again.", (LPSTR)g_product);
                say(m, MB_ICONSTOP);
                return FALSE;
            }
            pump();
        }
    }
    return TRUE;
}

/* [Stop] and the options' Window=/Wait=; [Running] 1=module,name: programs
 * the user has to close (except the product running as the shell, which is
 * dealt with by the shell handover). */
static BOOL check_running(void)
{
    char t[MAX_TEXT], m[MAX_TEXT], sec[32], shell[16], *name;
    int i;
    if (!stop_programs("Stop"))
        return FALSE;
    for (i = 0; i < g_nopts; i++) {
        wsprintf(sec, "Option.%s", (LPSTR)g_opts[i].name);
        if (!stop_programs(sec))
            return FALSE;
    }
    if (!shell_module(shell))
        shell[0] = 0;
    for (i = 1; inf_entry("Running", i, t, sizeof(t)); i++) {
        name = _fstrchr(t, ',');
        if (name)
            *name++ = 0;
        else
            name = t;
        if (shell[0] && lstrcmpi(t, shell) == 0)
            continue;
        if (GetModuleHandle(t)) {
            wsprintf(m, "Please close %s and run Setup again.", (LPSTR)name);
            say(m, MB_ICONSTOP);
            return FALSE;
        }
    }
    return TRUE;
}

/* ---- progress dialog -------------------------------------------------- */

static void progress(int pct, LPCSTR text)
{
    if (!g_prog)
        return;
    if (text)
        SetDlgItemText(g_prog, IDC_STATUS, text);
    if (pct != g_pct) {
        g_pct = pct;
        InvalidateRect(GetDlgItem(g_prog, IDC_GAUGE), NULL, FALSE);
    }
    UpdateWindow(g_prog);
    pump();
}

BOOL CALLBACK __export ProgressProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp; (void)lp;
    if (msg == WM_INITDIALOG) {
        SetWindowText(dlg, g_title);
        return TRUE;
    }
    return FALSE;
}

static void center(HWND w)
{
    RECT r;
    GetWindowRect(w, &r);
    SetWindowPos(w, NULL, (GetSystemMetrics(SM_CXSCREEN) - (r.right - r.left)) / 2,
                 (GetSystemMetrics(SM_CYSCREEN) - (r.bottom - r.top)) / 2, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER);
}

static void progress_open(void)
{
    FARPROC p;
    if (g_silent)
        return;
    p = MakeProcInstance((FARPROC)ProgressProc, g_inst);
    g_pct = -1;
    g_prog = CreateDialog(g_inst, MAKEINTRESOURCE(IDD_PROGRESS), g_back, (DLGPROC)p);
    if (g_prog) {
        center(g_prog);
        ShowWindow(g_prog, SW_SHOW);
        UpdateWindow(g_prog);
    }
}

static void progress_close(void)
{
    if (g_prog)
        DestroyWindow(g_prog);
    g_prog = NULL;
}

/* 3D sunken frame inside r; r becomes the inside. */
static void sunken(HDC dc, RECT *r)
{
    HBRUSH b;
    RECT e;
    b = CreateSolidBrush(GetSysColor(COLOR_BTNSHADOW));
    SetRect(&e, r->left, r->top, r->right - 1, r->top + 1);
    FillRect(dc, &e, b);
    SetRect(&e, r->left, r->top, r->left + 1, r->bottom - 1);
    FillRect(dc, &e, b);
    DeleteObject(b);
    b = GetStockObject(WHITE_BRUSH);
    SetRect(&e, r->left, r->bottom - 1, r->right, r->bottom);
    FillRect(dc, &e, b);
    SetRect(&e, r->right - 1, r->top, r->right, r->bottom);
    FillRect(dc, &e, b);
    b = GetStockObject(BLACK_BRUSH);
    SetRect(&e, r->left + 1, r->top + 1, r->right - 2, r->top + 2);
    FillRect(dc, &e, b);
    SetRect(&e, r->left + 1, r->top + 1, r->left + 2, r->bottom - 2);
    FillRect(dc, &e, b);
    InflateRect(r, -2, -2);
}

LRESULT CALLBACK __export GaugeProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT r, done, rest;
        char t[8];
        int x, pct = g_pct < 0 ? 0 : g_pct;
        HFONT old = SelectObject(dc, (HFONT)SendMessage(GetParent(w), WM_GETFONT, 0, 0));
        GetClientRect(w, &r);
        sunken(dc, &r);
        x = r.left + (int)((long)(r.right - r.left) * pct / 100);
        SetRect(&done, r.left, r.top, x, r.bottom);
        SetRect(&rest, x, r.top, r.right, r.bottom);
        wsprintf(t, "%d%%", pct);
        SetTextAlign(dc, TA_CENTER | TA_TOP);
        /* the text is white on the blue part and blue on the white part */
        SetBkColor(dc, RGB(0, 0, 128));
        SetTextColor(dc, RGB(255, 255, 255));
        ExtTextOut(dc, (r.left + r.right) / 2, r.top + 1, ETO_OPAQUE | ETO_CLIPPED, &done, t, lstrlen(t), NULL);
        SetBkColor(dc, RGB(255, 255, 255));
        SetTextColor(dc, RGB(0, 0, 128));
        ExtTextOut(dc, (r.left + r.right) / 2, r.top + 1, ETO_OPAQUE | ETO_CLIPPED, &rest, t, lstrlen(t), NULL);
        if (old)
            SelectObject(dc, old);
        EndPaint(w, &ps);
        return 0;
    }
    return DefWindowProc(w, msg, wp, lp);
}

/* ---- preview ---------------------------------------------------------- */

static void preview_free(void)
{
    if (g_dib)
        GlobalFree(g_dib);
    if (g_pal)
        DeleteObject(g_pal);
    g_dib = NULL;
    g_pal = NULL;
}

/* Loads the [Choice] Preview= image of an item from the setup directory (a
 * DIB below 64 KB; tools/w16pack.py preview makes one from a PNG). */
static void preview_load(const char *item)
{
    char rel[MAX_PATHLEN], path[MAX_PATHLEN + 64];
    BITMAPFILEHEADER fh;
    LPBITMAPINFOHEADER bi;
    HFILE f;
    long size;
    preview_free();
    if (!g_preview[0])
        return;
    expand(g_preview, rel, sizeof(rel), item);
    wsprintf(path, "%s%s", (LPSTR)g_src, (LPSTR)rel);
    f = _lopen(path, OF_READ | OF_SHARE_DENY_WRITE);
    if (f == HFILE_ERROR)
        return;
    size = _llseek(f, 0, 2) - sizeof(fh);
    _llseek(f, 0, 0);
    if (_lread(f, &fh, sizeof(fh)) != sizeof(fh) || fh.bfType != 0x4D42 || size <= 0 || size > 65520L
        || fh.bfOffBits < sizeof(fh) + sizeof(BITMAPINFOHEADER)) {
        _lclose(f);
        return;
    }
    g_dib = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!g_dib) {
        _lclose(f);
        return;
    }
    bi = (LPBITMAPINFOHEADER)GlobalLock(g_dib);
    if (_lread(f, bi, (UINT)size) != (UINT)size || bi->biSize != sizeof(BITMAPINFOHEADER)) {
        GlobalUnlock(g_dib);
        _lclose(f);
        preview_free();
        return;
    }
    _lclose(f);
    g_dib_bits = (UINT)(fh.bfOffBits - sizeof(fh));
    if (bi->biBitCount <= 8) {
        /* a palette for 256-colour displays */
        int i, n = bi->biClrUsed ? (int)bi->biClrUsed : 1 << bi->biBitCount;
        RGBQUAD FAR *q = (RGBQUAD FAR *)((LPSTR)bi + bi->biSize);
        HLOCAL h = LocalAlloc(LMEM_FIXED, sizeof(LOGPALETTE) + n * sizeof(PALETTEENTRY));
        if (h) {
            LOGPALETTE NEAR *lp = (LOGPALETTE NEAR *)LocalLock(h);
            lp->palVersion = 0x300;
            lp->palNumEntries = n;
            for (i = 0; i < n; i++) {
                lp->palPalEntry[i].peRed = q[i].rgbRed;
                lp->palPalEntry[i].peGreen = q[i].rgbGreen;
                lp->palPalEntry[i].peBlue = q[i].rgbBlue;
                lp->palPalEntry[i].peFlags = 0;
            }
            g_pal = CreatePalette((LOGPALETTE FAR *)lp);
            LocalUnlock(h);
            LocalFree(h);
        }
    }
    GlobalUnlock(g_dib);
}

LRESULT CALLBACK __export PreviewProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT r;
        HBRUSH b;
        GetClientRect(w, &r);
        sunken(dc, &r);
        b = CreateSolidBrush(GetSysColor(COLOR_APPWORKSPACE));
        FillRect(dc, &r, b);
        DeleteObject(b);
        if (g_dib) {
            LPBITMAPINFOHEADER bi = (LPBITMAPINFOHEADER)GlobalLock(g_dib);
            HPALETTE old = NULL;
            int bw = (int)bi->biWidth, bh = (int)bi->biHeight;
            int x = r.left + (r.right - r.left - bw) / 2, y = r.top + (r.bottom - r.top - bh) / 2;
            if (g_pal) {
                old = SelectPalette(dc, g_pal, FALSE);
                RealizePalette(dc);
            }
            SetDIBitsToDevice(dc, x, y, bw, bh, 0, 0, 0, bh, (LPSTR)bi + g_dib_bits, (LPBITMAPINFO)bi,
                              DIB_RGB_COLORS);
            if (old)
                SelectPalette(dc, old, FALSE);
            GlobalUnlock(g_dib);
        } else {
            HFONT old = SelectObject(dc, (HFONT)SendMessage(GetParent(w), WM_GETFONT, 0, 0));
            SetBkMode(dc, TRANSPARENT);
            DrawText(dc, "No preview", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (old)
                SelectObject(dc, old);
        }
        EndPaint(w, &ps);
        return 0;
    }
    if (msg == WM_PALETTECHANGED && (HWND)wp != w) {
        InvalidateRect(w, NULL, FALSE);
        return 0;
    }
    return DefWindowProc(w, msg, wp, lp);
}

/* ---- background window ------------------------------------------------ */

LRESULT CALLBACK __export BackProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT r, band;
        HFONT f, old;
        char t[MAX_TEXT];
        int i, h, bands = 64;
        GetClientRect(w, &r);
        h = r.bottom;
        /* the blue gradient of the Windows 3.1 setup programs */
        for (i = 0; i < bands; i++) {
            HBRUSH b = CreateSolidBrush(RGB(0, 0, 255 - i * 255 / bands));
            SetRect(&band, 0, (int)((long)h * i / bands), r.right, (int)((long)h * (i + 1) / bands));
            FillRect(dc, &band, b);
            DeleteObject(b);
        }
        SetBkMode(dc, TRANSPARENT);
        f = CreateFont(-(h / 12), 0, 0, 0, FW_BOLD, TRUE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, VARIABLE_PITCH | FF_ROMAN, "Times New Roman");
        old = SelectObject(dc, f);
        SetTextColor(dc, RGB(0, 0, 0));
        TextOut(dc, h / 30 + 3, h / 40 + 3, g_title, lstrlen(g_title));
        SetTextColor(dc, RGB(255, 255, 255));
        TextOut(dc, h / 30, h / 40, g_title, lstrlen(g_title));
        SelectObject(dc, old);
        DeleteObject(f);
        f = CreateFont(-(h / 32), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, VARIABLE_PITCH | FF_SWISS, "Arial");
        old = SelectObject(dc, f);
        inf_text("Setup", "Tagline", "Version %VERSION%", t, sizeof(t));
        TextOut(dc, h / 30, h / 40 + h / 9, t, lstrlen(t));
        SelectObject(dc, old);
        DeleteObject(f);
        EndPaint(w, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND)
        return 1;
    return DefWindowProc(w, msg, wp, lp);
}

/* ---- main dialog ------------------------------------------------------ */

static void ctl_rect(HWND dlg, int id, RECT *r)
{
    GetWindowRect(GetDlgItem(dlg, id), r);
    ScreenToClient(dlg, (POINT FAR *)&r->left);
    ScreenToClient(dlg, (POINT FAR *)&r->right);
}

/* Places a control at x, *y and advances *y by its height plus gap. */
static void stack(HWND dlg, int id, int x, int *y, int gap)
{
    RECT r;
    ctl_rect(dlg, id, &r);
    SetWindowPos(GetDlgItem(dlg, id), NULL, x, *y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    *y += r.bottom - r.top + gap;
}

/* Makes a static text control as high as its text. */
static void fit_text(HWND dlg, int id)
{
    char t[MAX_TEXT];
    RECT r, c;
    HWND w = GetDlgItem(dlg, id);
    HDC dc = GetDC(w);
    HFONT old = SelectObject(dc, (HFONT)SendMessage(dlg, WM_GETFONT, 0, 0));
    GetWindowText(w, t, sizeof(t));
    ctl_rect(dlg, id, &r);
    SetRect(&c, 0, 0, r.right - r.left, 0);
    DrawText(dc, t, -1, &c, DT_CALCRECT | DT_WORDBREAK);
    if (old)
        SelectObject(dc, old);
    ReleaseDC(w, dc);
    SetWindowPos(w, NULL, 0, 0, r.right - r.left, c.bottom, SWP_NOMOVE | SWP_NOZORDER);
}

/* Lays out the dialog for the features SETUP.INF uses: the left column
 * (text, directory, choice, check boxes) stacked, the preview (320 x 200
 * plus frame) on the right if there is one, the buttons below. */
static void layout(HWND dlg)
{
    static const int checks[] = { IDC_OPT1, IDC_OPT2, IDC_OPT3, IDC_OPT4, IDC_LOAD, IDC_GROUP, IDC_START };
    RECT m, intro, btn, wr;
    int x, y, top, i, right, bottom, bw, bh, by, pw = 324, ph = 204;
    BOOL preview = g_preview[0] && g_nitems > 0;
    SetRect(&m, 0, 0, 8, 6);
    MapDialogRect(dlg, &m);
    ctl_rect(dlg, IDC_INTRO, &intro);
    ctl_rect(dlg, IDOK, &btn);
    bw = btn.right - btn.left;
    bh = btn.bottom - btn.top;
    x = intro.left;
    top = y = intro.top;
    fit_text(dlg, IDC_INTRO);
    stack(dlg, IDC_INTRO, x, &y, m.bottom);
    stack(dlg, IDC_DIRLABEL, x, &y, 2);
    stack(dlg, IDC_DIR, x, &y, m.bottom);
    if (g_nitems) {
        stack(dlg, IDC_CHOICELABEL, x, &y, 2);
        stack(dlg, IDC_CHOICE, x, &y, m.bottom);
    }
    for (i = 0; i < sizeof(checks) / sizeof(checks[0]); i++)   /* the dialog is not visible yet */
        if (GetWindowLong(GetDlgItem(dlg, checks[i]), GWL_STYLE) & WS_VISIBLE)
            stack(dlg, checks[i], x, &y, 2);
    right = intro.right;
    if (preview) {
        int px = intro.right + m.right;
        SetWindowPos(GetDlgItem(dlg, IDC_PREVIEW), NULL, px, top, pw, ph, SWP_NOZORDER);
        right = px + pw;
        by = top + ph + m.bottom;
        if (by < y - 2 - bh)
            by = y - 2 - bh;
    } else {
        by = y + m.bottom / 2;
    }
    if (right < 3 * bw + 2 * m.bottom + x)
        right = 3 * bw + 2 * m.bottom + x;
    SetWindowPos(GetDlgItem(dlg, IDCANCEL), NULL, right - bw, by, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    SetWindowPos(GetDlgItem(dlg, IDC_REMOVE), NULL, right - 2 * bw - m.bottom, by, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER);
    SetWindowPos(GetDlgItem(dlg, IDOK), NULL, right - 3 * bw - 2 * m.bottom, by, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER);
    bottom = by + bh;
    if (bottom < y - 2)
        bottom = y - 2;
    SetRect(&wr, 0, 0, right + m.right, bottom + m.bottom);
    AdjustWindowRectEx(&wr, GetWindowLong(dlg, GWL_STYLE), FALSE, GetWindowLong(dlg, GWL_EXSTYLE));
    SetWindowPos(dlg, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER);
    center(dlg);
}

/* A check box for an optional feature: label from SETUP.INF, hidden if the
 * feature is not used. */
static void feature(HWND dlg, int id, BOOL used, LPCSTR section, LPCSTR def, BOOL on)
{
    char t[MAX_TEXT];
    if (!used) {
        ShowWindow(GetDlgItem(dlg, id), SW_HIDE);
        return;
    }
    inf_text(section, "Label", def, t, sizeof(t));
    SetDlgItemText(dlg, id, t);
    CheckDlgButton(dlg, id, on);
}

BOOL CALLBACK __export MainProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    char t[MAX_TEXT];
    int i;
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SetWindowText(dlg, g_title);
        if (g_installed)
            inf_text("Setup", "WelcomeInstalled", "%PRODUCT% %VERSION% is installed in %DIR%.\\n\\nChange the "
                     "options and click Install, or click Remove to remove %PRODUCT%.", t, sizeof(t));
        else
            inf_text("Setup", "Welcome", "Setup installs %PRODUCT% %VERSION% into the directory below.", t,
                     sizeof(t));
        SetDlgItemText(dlg, IDC_INTRO, t);
        SetDlgItemText(dlg, IDC_DIR, g_opt.dir);
        SendDlgItemMessage(dlg, IDC_DIR, EM_LIMITTEXT, MAX_PATHLEN - 16, 0);
        if (g_installed) {
            /* moving an installation: remove it first */
            SendDlgItemMessage(dlg, IDC_DIR, EM_SETREADONLY, TRUE, 0);
            EnableWindow(GetDlgItem(dlg, IDC_REMOVE), TRUE);
        }
        if (g_nitems) {
            inf_text("Choice", "Label", "&Choice:", t, sizeof(t));
            SetDlgItemText(dlg, IDC_CHOICELABEL, t);
            for (i = 0; i < g_nitems; i++)
                SendDlgItemMessage(dlg, IDC_CHOICE, CB_ADDSTRING, 0, (LPARAM)(LPSTR)g_names[i]);
            i = item_index(g_opt.choice);
            SendDlgItemMessage(dlg, IDC_CHOICE, CB_SETCURSEL, i < 0 ? 0 : i, 0);
            preview_load(g_items[i < 0 ? 0 : i]);
        } else {
            ShowWindow(GetDlgItem(dlg, IDC_CHOICELABEL), SW_HIDE);
            ShowWindow(GetDlgItem(dlg, IDC_CHOICE), SW_HIDE);
        }
        if (!g_preview[0] || !g_nitems)
            ShowWindow(GetDlgItem(dlg, IDC_PREVIEW), SW_HIDE);
        for (i = 0; i < MAX_OPTIONS; i++) {
            char sec[32];
            if (i < g_nopts)
                wsprintf(sec, "Option.%s", (LPSTR)g_opts[i].name);
            feature(dlg, IDC_OPT1 + i, i < g_nopts, sec, i < g_nopts ? g_opts[i].name : "",
                    i < g_nopts && g_opts[i].on);
        }
        feature(dlg, IDC_LOAD, g_auto[0] != 0, "Autostart", "Start %PRODUCT% with &Windows", g_opt.load);
        feature(dlg, IDC_GROUP, g_group[0] != 0, "Group", "Create a Program Manager &group", g_opt.group);
        feature(dlg, IDC_START, g_run[0] != 0, "StartNow", "Start %PRODUCT% &now", g_opt.start);
        layout(dlg);
        return TRUE;
    case WM_COMMAND:
        switch (wp) {
        case IDC_CHOICE:
            if (HIWORD(lp) == CBN_SELCHANGE) {
                i = (int)SendDlgItemMessage(dlg, IDC_CHOICE, CB_GETCURSEL, 0, 0);
                if (i >= 0 && i < g_nitems)
                    preview_load(g_items[i]);
                InvalidateRect(GetDlgItem(dlg, IDC_PREVIEW), NULL, FALSE);
            }
            return TRUE;
        case IDOK:
            GetDlgItemText(dlg, IDC_DIR, g_opt.dir, sizeof(g_opt.dir));
            AnsiUpper(g_opt.dir);
            cut_slash(g_opt.dir);
            if (lstrlen(g_opt.dir) < 3 || g_opt.dir[1] != ':' || g_opt.dir[2] != '\\') {
                MessageBox(dlg, "Please enter a full path, for example C:\\PROGRAM.", g_title,
                           MB_OK | MB_ICONEXCLAMATION);
                SetFocus(GetDlgItem(dlg, IDC_DIR));
                return TRUE;
            }
            i = (int)SendDlgItemMessage(dlg, IDC_CHOICE, CB_GETCURSEL, 0, 0);
            if (i >= 0 && i < g_nitems)
                lstrcpy(g_opt.choice, g_items[i]);
            g_opt.load = g_auto[0] && IsDlgButtonChecked(dlg, IDC_LOAD);
            g_opt.group = g_group[0] && IsDlgButtonChecked(dlg, IDC_GROUP);
            g_opt.start = g_run[0] && IsDlgButtonChecked(dlg, IDC_START);
            for (i = 0; i < g_nopts; i++)
                g_opts[i].on = IsDlgButtonChecked(dlg, IDC_OPT1 + i) != 0;
            EndDialog(dlg, IDOK);
            return TRUE;
        case IDC_REMOVE:
        case IDCANCEL:
            EndDialog(dlg, wp);
            return TRUE;
        }
        break;
    case WM_QUERYNEWPALETTE:
    case WM_PALETTECHANGED:
        InvalidateRect(GetDlgItem(dlg, IDC_PREVIEW), NULL, FALSE);
        break;
    }
    return FALSE;
}

/* ---- install and remove ----------------------------------------------- */

/* [Ini] 1=file,section,key,value (expanded): private INI entries. */
static void write_ini(void)
{
    char t[MAX_TEXT], e[MAX_TEXT], *sec, *key, *val;
    int i;
    for (i = 1; inf_entry("Ini", i, t, sizeof(t)); i++) {
        expand(t, e, sizeof(e), NULL);
        sec = _fstrchr(e, ',');
        key = sec ? _fstrchr(sec + 1, ',') : NULL;
        val = key ? _fstrchr(key + 1, ',') : NULL;
        if (!val)
            continue;
        *sec++ = *key++ = *val++ = 0;
        WritePrivateProfileString(sec, key, val, e);
        log_line(e);
    }
}

/* ---- options ---------------------------------------------------------- */

/* Splits an expanded "file,section,key,value" in place. */
static BOOL split_ini(char *e, char **sec, char **key, char **val)
{
    *sec = _fstrchr(e, ',');
    *key = *sec ? _fstrchr(*sec + 1, ',') : NULL;
    *val = *key ? _fstrchr(*key + 1, ',') : NULL;
    if (!*val)
        return FALSE;
    *(*sec)++ = *(*key)++ = *(*val)++ = 0;
    return TRUE;
}

/* "prog[,minimized]" relative to the installation directory: full path */
static void option_program(LPCSTR section, LPCSTR key, char *path, char *extra)
{
    char t[MAX_TEXT], e[MAX_TEXT], *c;
    path[0] = 0;
    if (extra)
        extra[0] = 0;
    if (!inf_get(section, key, "", t, sizeof(t)))
        return;
    expand(t, e, sizeof(e), NULL);
    c = _fstrchr(e, ',');
    if (c) {
        *c++ = 0;
        if (extra)
            lstrcpyn(extra, c, 16);
    }
    if (e[0] && e[1] != ':' && e[0] != '\\')
        wsprintf(path, "%s\\%s", (LPSTR)g_opt.dir, (LPSTR)e);
    else
        lstrcpy(path, e);
}

/* Turns an option on: its IniN= entries are written (the old values are
 * kept in WIN.INI [<key>] Old.<name>.<n>, '>' + value or '*' for none),
 * its Load= program is added to load=. */
static void option_apply(OptionDef *o)
{
    char sec[32], ik[8], ok[40], t[MAX_TEXT], e[MAX_TEXT], cur[MAX_TEXT], saved[MAX_TEXT + 2];
    char *s, *k, *v;
    int n;
    wsprintf(sec, "Option.%s", (LPSTR)o->name);
    for (n = 1;; n++) {
        wsprintf(ik, "Ini%d", n);
        if (!inf_get(sec, ik, "", t, sizeof(t)))
            break;
        expand(t, e, sizeof(e), NULL);
        if (!split_ini(e, &s, &k, &v))
            continue;
        wsprintf(ok, "Old.%s.%d", (LPSTR)o->name, n);
        if (!GetProfileString(g_key, ok, "", saved, sizeof(saved))) {
            if (GetPrivateProfileString(s, k, "\1", cur, sizeof(cur), e) == 1 && cur[0] == 1)
                lstrcpy(saved, "*");
            else
                wsprintf(saved, ">%s", (LPSTR)cur);
            WriteProfileString(g_key, ok, saved);
        }
        WritePrivateProfileString(s, k, v, e);
        wsprintf(t, "option %s: %s [%s] %s=%s", (LPSTR)o->name, (LPSTR)e, (LPSTR)s, (LPSTR)k, (LPSTR)v);
        log_line(t);
    }
    option_program(sec, "Load", t, NULL);
    if (t[0])
        edit_list("load", t, t);
}

/* Turns an option off (or removes it): entries still holding our value get
 * their old value back, the Load= program leaves load=. */
static void option_revert(OptionDef *o)
{
    char sec[32], ik[8], ok[40], t[MAX_TEXT], e[MAX_TEXT], cur[MAX_TEXT], saved[MAX_TEXT + 2];
    char *s, *k, *v;
    int n;
    wsprintf(sec, "Option.%s", (LPSTR)o->name);
    for (n = 1;; n++) {
        wsprintf(ik, "Ini%d", n);
        if (!inf_get(sec, ik, "", t, sizeof(t)))
            break;
        expand(t, e, sizeof(e), NULL);
        if (!split_ini(e, &s, &k, &v))
            continue;
        wsprintf(ok, "Old.%s.%d", (LPSTR)o->name, n);
        GetProfileString(g_key, ok, "", saved, sizeof(saved));
        GetPrivateProfileString(s, k, "", cur, sizeof(cur), e);
        if (saved[0] && lstrcmpi(cur, v) == 0) {
            WritePrivateProfileString(s, k, saved[0] == '>' ? saved + 1 : NULL, e);
            wsprintf(t, "option %s: %s [%s] %s restored", (LPSTR)o->name, (LPSTR)e, (LPSTR)s, (LPSTR)k);
        } else {
            wsprintf(t, "option %s: %s [%s] %s changed meanwhile, left", (LPSTR)o->name, (LPSTR)e, (LPSTR)s,
                     (LPSTR)k);
        }
        log_line(t);
        WriteProfileString(g_key, ok, NULL);
    }
    option_program(sec, "Load", t, NULL);
    if (t[0])
        edit_list("load", t, NULL);
}

/* Applies the chosen options and records them in WIN.INI [<key>]. */
static void apply_options(void)
{
    char ok[40];
    int i;
    for (i = 0; i < g_nopts; i++) {
        OptionDef *o = &g_opts[i];
        if (o->on)
            option_apply(o);
        else if (o->was_on)
            option_revert(o);
        wsprintf(ok, "Option.%s", (LPSTR)o->name);
        WriteProfileString(g_key, ok, o->on ? "1" : "0");
        if (o->restart && o->on != o->was_on)
            g_need_restart = TRUE;
    }
}

/* Starts the Run= programs of the options that are on. */
static void run_options(void)
{
    char sec[32], path[MAX_PATHLEN + 64], show[16], t[MAX_TEXT];
    UINT rc;
    int i;
    if (g_nostart)
        return;
    for (i = 0; i < g_nopts; i++) {
        if (!g_opts[i].on)
            continue;
        wsprintf(sec, "Option.%s", (LPSTR)g_opts[i].name);
        option_program(sec, "Run", path, show);
        if (!path[0])
            continue;
        rc = WinExec(path, lstrcmpi(show, "minimized") == 0 ? SW_SHOWMINNOACTIVE : SW_SHOWNORMAL);
        wsprintf(t, "started %s (%u)", (LPSTR)path, rc);
        log_line(t);
    }
}

/* Asks to restart Windows if an option that needs it changed (silent: only
 * with /RESTART). The restart happens when Setup ends. */
static void offer_restart(void)
{
    if (!g_need_restart || g_resumed)
        return;
    if (g_silent ? !g_cmd_restart
                 : MessageBox(g_back, "Windows must be restarted for the changes to take effect.\r\n\r\n"
                              "Restart Windows now?", g_title, MB_YESNO | MB_ICONQUESTION) != IDYES) {
        log_line("restart of Windows postponed");
        return;
    }
    log_line("restarting Windows");
    g_restart_now = TRUE;
}

/* ---- shell handover --------------------------------------------------- */

static void stage_dir(char *out)
{
    GetTempFileName(GetTempDrive(0), "W16", 0, out);
    file_delete(out);
    lstrcpy(base_name(out), STAGE_DIR);
}

/* Copies SETUP.EXE, SETUP.INF and (all) the product's files to the stage
 * directory. */
static BOOL stage_copy(const char *stage, BOOL all)
{
    static const char *own[] = { "SETUP.EXE", "SETUP.INF" };
    char from[MAX_PATHLEN + 64], to[MAX_PATHLEN + 64], t[MAX_TEXT];
    int i, total = all ? g_nfiles + 2 : 2;
    progress_open();
    for (i = 0; i < total; i++) {
        LPCSTR rel = i < 2 ? own[i] : g_files[i - 2];
        wsprintf(from, "%s%s", (LPSTR)g_src, (LPSTR)rel);
        wsprintf(to, "%s\\%s", (LPSTR)stage, (LPSTR)rel);
        wsprintf(t, "Preparing %s", (LPSTR)rel);
        progress(i * 100 / total, t);
        make_dirs(to);
        if (!copy_file(from, to)) {
            progress_close();
            log_line(to);
            return FALSE;
        }
    }
    progress_close();
    return TRUE;
}

/* The command line that repeats the chosen installation. */
static void resume_args(char *cmd, BOOL remove)
{
    char t[64];
    int i;
    if (remove) {
        lstrcpy(cmd, "/U");
    } else {
        wsprintf(cmd, "/D=%s", (LPSTR)g_opt.dir);
        if (g_nitems && g_opt.choice[0]) {
            wsprintf(t, " /%s=%s", (LPSTR)g_switch, (LPSTR)g_opt.choice);
            lstrcat(cmd, t);
        }
        for (i = 0; i < g_nopts; i++) {
            wsprintf(t, g_opts[i].on ? " /%s" : " /NO%s", (LPSTR)g_opts[i].sw);
            lstrcat(cmd, t);
        }
        if (g_auto[0])
            lstrcat(cmd, g_opt.load ? " /LOAD" : " /NOLOAD");
        if (g_group[0])
            lstrcat(cmd, g_opt.group ? " /GROUP" : " /NOGROUP");
        if (g_run[0])
            lstrcat(cmd, g_opt.start ? " /START" : " /NOSTART");
    }
    if (g_silent)
        lstrcat(cmd, " /S");
}

/* The product is the running shell: continue after a restart of Windows
 * with Program Manager as the shell. */
static BOOL handover(BOOL remove)
{
    char stage[MAX_PATHLEN], t[MAX_TEXT], cmd[MAX_TEXT], sh[MAX_PATHLEN];
    wsprintf(t, "%s is the Windows shell and cannot be %s while it runs.\r\n\r\nSetup will restart "
                "Windows with Program Manager, finish there and then restart Windows once more.",
             (LPSTR)g_product, (LPSTR)(remove ? "removed" : "updated"));
    log_line("shell handover");
    if (!g_silent && MessageBox(g_back, t, g_title, MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
        return FALSE;
    stage_dir(stage);
    if (lstrcmpi(stage, g_opt.dir) == 0 || !stage_copy(stage, !remove)) {
        say("Setup could not copy itself to the TEMP directory and cannot continue.", MB_ICONSTOP);
        return FALSE;
    }
    resume_args(cmd, remove);
    WriteProfileString(g_key, "Resume", cmd);
    GetPrivateProfileString("boot", "shell", "progman.exe", sh, sizeof(sh), "SYSTEM.INI");
    WriteProfileString(g_key, "ResumeShell", sh);
    WritePrivateProfileString("boot", "shell", "progman.exe", "SYSTEM.INI");
    /* run= takes no arguments: the staged copy finds Resume= in WIN.INI */
    wsprintf(t, "%s\\SETUP.EXE", (LPSTR)stage);
    edit_list("run", t, t);
    log_line(t);
    g_restart_now = TRUE;
    return TRUE;
}

/* The first thing a resumed Setup does: undo the handover's changes and
 * return the command line to carry out. */
static void resume_begin(char *cmd, int size)
{
    char sh[MAX_PATHLEN], self[MAX_PATHLEN + 16];
    GetProfileString(g_key, "Resume", "", cmd, size);
    if (GetProfileString(g_key, "ResumeShell", "", sh, sizeof(sh)))
        WritePrivateProfileString("boot", "shell", sh, "SYSTEM.INI");
    WriteProfileString(g_key, "Resume", NULL);
    WriteProfileString(g_key, "ResumeShell", NULL);
    wsprintf(self, "%sSETUP.EXE", (LPSTR)g_src);
    edit_list("run", self, NULL);
    log_line("resuming after the shell handover");
    log_line(cmd);
}

static BOOL install(void)
{
    char dir[MAX_PATHLEN], from[MAX_PATHLEN + 64], to[MAX_PATHLEN + 64], t[MAX_TEXT];
    static const char *own[] = { "SETUP.EXE", "SETUP.INF" };
    long need = 0, done = 0, n;
    struct diskfree_t df;
    int i, total;
    BOOL same;

    lstrcpy(dir, g_opt.dir);
    add_slash(dir);
    wsprintf(t, "install %s %s to %s, choice %s, load %d, group %d, start %d", (LPSTR)g_product, (LPSTR)g_ver,
             (LPSTR)dir, (LPSTR)g_opt.choice, g_opt.load, g_opt.group, g_opt.start);
    log_line(t);
    same = lstrcmpi(dir, g_src) == 0;
    if (!same && shell_module(t))
        return handover(FALSE);
    if (!check_running())
        return FALSE;

    total = g_nfiles + 2;
    if (!same) {
        for (i = 0; i < total; i++) {
            wsprintf(from, "%s%s", (LPSTR)g_src, (LPSTR)(i < g_nfiles ? g_files[i] : own[i - g_nfiles]));
            n = file_size(from);
            if (n < 0) {
                wsprintf(t, "The file %s is missing. Setup cannot continue.", (LPSTR)from);
                say(t, MB_ICONSTOP);
                return FALSE;
            }
            need += n;
        }
        if (_dos_getdiskfree(dir[0] - (dir[0] >= 'a' ? 'a' : 'A') + 1, &df) == 0) {
            long avail = (long)df.avail_clusters * df.sectors_per_cluster * df.bytes_per_sector;
            if (avail < need + 32768L) {
                wsprintf(t, "Drive %c: has %ld KB free; %s needs %ld KB.", dir[0], avail / 1024,
                         (LPSTR)g_product, need / 1024 + 32);
                say(t, MB_ICONSTOP);
                return FALSE;
            }
        }
        progress_open();
        for (i = 0; i < total; i++) {
            LPCSTR rel = i < g_nfiles ? g_files[i] : own[i - g_nfiles];
            wsprintf(from, "%s%s", (LPSTR)g_src, (LPSTR)rel);
            wsprintf(to, "%s%s", (LPSTR)dir, (LPSTR)rel);
            wsprintf(t, "Copying %s", (LPSTR)rel);
            progress(need ? (int)(done * 100 / need) : 0, t);
            make_dirs(to);
            if (!copy_file(from, to)) {
                progress_close();
                wsprintf(t, "Cannot copy %s to %s.", (LPSTR)from, (LPSTR)to);
                say(t, MB_ICONSTOP);
                return FALSE;
            }
            done += file_size(to);
        }
        wsprintf(t, "Setting up %s", (LPSTR)g_product);
        progress(100, t);
    }
    log_line("files copied");

    write_ini();
    WriteProfileString(g_key, "InstallDir", g_opt.dir);
    WriteProfileString(g_key, "Version", g_ver);
    WriteProfileString(g_key, "Choice", g_opt.choice[0] ? g_opt.choice : NULL);
    apply_options();
    if (g_auto[0]) {
        wsprintf(t, "%s%s", (LPSTR)dir, (LPSTR)g_auto);
        edit_load(g_opt.load ? t : NULL);
        log_line(g_opt.load ? "WIN.INI load= updated" : "WIN.INI load= cleared");
    }

    if (g_group[0]) {
        if (g_opt.group) {
            if (!make_group())
                log_line("Program Manager is not running: no group created");
        } else if (same) {
            remove_group();
        }
    }
    progress_close();

    run_options();
    if (g_opt.start) {
        UINT rc;
        char show[16];
        inf_get("StartNow", "Show", "normal", show, sizeof(show));
        wsprintf(t, "%s%s", (LPSTR)dir, (LPSTR)g_run);
        rc = WinExec(t, lstrcmpi(show, "minimized") == 0 ? SW_SHOWMINNOACTIVE : SW_SHOWNORMAL);
        wsprintf(t, "started %s%s (%u)", (LPSTR)dir, (LPSTR)g_run, rc);
        log_line(t);
    }
    inf_text("Setup", "Done", "%PRODUCT% %VERSION% has been installed in %DIR%.", t, sizeof(t));
    say(t, MB_ICONINFORMATION);
    offer_restart();
    return TRUE;
}

/* Removes the WIN.INI entries and the Program Manager group. */
static void unregister(void)
{
    int i;
    for (i = 0; i < g_nopts; i++)
        if (g_opts[i].was_on) {
            option_revert(&g_opts[i]);
            if (g_opts[i].restart)
                g_need_restart = TRUE;
        }
    if (g_auto[0])
        edit_load(NULL);
    WriteProfileString(g_key, NULL, NULL);
    remove_group();
    log_line("WIN.INI entries and Program Manager group removed");
}

/* Deletes the installed files (as listed in dir\SETUP.INF, plus its
 * [Uninstall] list) and the directories that are empty afterwards. */
static BOOL remove_files(const char *dirname, BOOL quiet)
{
    char dir[MAX_PATHLEN], path[MAX_PATHLEN + 64], t[MAX_TEXT];
    static const char *own[] = { "SETUP.EXE", "SETUP.INF", "SETUP.LOG" };
    static char dirs[MAX_FILES][64];
    int i, j, nd, best;
    BOOL ok;
    lstrcpy(dir, dirname);
    add_slash(dir);
    wsprintf(g_inf, "%sSETUP.INF", (LPSTR)dir);
    if (!load_inf()) {
        wsprintf(t, "%s does not contain an installation (SETUP.INF is missing).", (LPSTR)dirname);
        if (quiet)
            log_line(t);
        else
            say(t, MB_ICONSTOP);
        return FALSE;
    }
    for (i = 0; i < g_nfiles; i++) {
        wsprintf(path, "%s%s", (LPSTR)dir, (LPSTR)g_files[i]);
        file_delete(path);
    }
    for (i = 1; inf_entry("Uninstall", i, t, sizeof(t)); i++) {
        wsprintf(path, "%s%s", (LPSTR)dir, (LPSTR)t);
        file_delete(path);
        if (base_name(t) != t && g_nfiles < MAX_FILES)
            lstrcpy(g_files[g_nfiles++], t);    /* for its directory */
    }
    for (i = 0; i < sizeof(own) / sizeof(own[0]); i++) {
        wsprintf(path, "%s%s", (LPSTR)dir, (LPSTR)own[i]);
        file_delete(path);
    }
    /* the sub-directories of the listed files, deepest first */
    nd = 0;
    for (i = 0; i < g_nfiles; i++) {
        lstrcpy(t, g_files[i]);
        for (;;) {
            char *b = base_name(t);
            if (b == t)
                break;
            b[-1] = 0;
            for (j = 0; j < nd && lstrcmpi(dirs[j], t) != 0; j++)
                ;
            if (j == nd && nd < MAX_FILES)
                lstrcpy(dirs[nd++], t);
        }
    }
    while (nd > 0) {
        for (best = 0, i = 1; i < nd; i++)
            if (lstrlen(dirs[i]) > lstrlen(dirs[best]))
                best = i;
        wsprintf(path, "%s%s", (LPSTR)dir, (LPSTR)dirs[best]);
        rmdir(path);
        lstrcpy(dirs[best], dirs[--nd]);
    }
    /* the task may have been started in the directory: leave it */
    GetWindowsDirectory(t, sizeof(t));
    AnsiUpper(t);
    _chdrive(t[0] - 'A' + 1);
    chdir(t);
    ok = rmdir(dirname) == 0;
    wsprintf(t, ok ? "removed %s" : "removed the files; %s is not empty", (LPSTR)dirname);
    log_line(t);
    if (quiet)
        return TRUE;
    if (ok)
        wsprintf(t, "%s has been removed.", (LPSTR)g_product);
    else
        wsprintf(t, "%s has been removed. The directory %s contains other files and was not deleted.",
                 (LPSTR)g_product, (LPSTR)dirname);
    say(t, MB_ICONINFORMATION);
    return TRUE;
}

/* The path of the copy in TEMP (W16UNINS.EXE / .LOG). */
static void temp_path(char *out, LPCSTR name)
{
    GetTempFileName(GetTempDrive(0), "W16", 0, out);
    file_delete(out);
    lstrcpy(base_name(out), name);
}

/* Starts a copy of SETUP.EXE in TEMP that removes dir after this instance
 * has ended. */
static BOOL spawn_remover(const char *dir)
{
    char tmp[MAX_PATHLEN], self[MAX_PATHLEN], cmd[MAX_PATHLEN * 2 + 16];
    temp_path(tmp, MODNAME_TMP ".EXE");
    GetModuleFileName(g_inst, self, sizeof(self));
    if (!copy_file(self, tmp) || !patch_modname(tmp, MODNAME, MODNAME_TMP)) {
        log_line("cannot copy SETUP.EXE to TEMP");
        return FALSE;
    }
    wsprintf(cmd, "%s /X=%s%s%s%s", (LPSTR)tmp, (LPSTR)dir, (LPSTR)(g_silent ? " /S" : ""),
             (LPSTR)(g_need_restart ? " /R" : ""), (LPSTR)(g_cmd_restart ? " /RESTART" : ""));
    log_line(cmd);
    return WinExec(cmd, SW_SHOWNORMAL) >= 32;
}

static BOOL uninstall(void)
{
    char t[MAX_TEXT], dir[MAX_PATHLEN], module[16];
    BOOL ok;
    wsprintf(t, "Remove %s from %s?", (LPSTR)g_product, (LPSTR)g_opt.dir);
    log_line(t);
    if (!g_silent && !g_resumed && MessageBox(g_back, t, g_title, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return FALSE;
    if (shell_module(module))
        return handover(TRUE);
    if (!check_running())
        return FALSE;
    unregister();
    lstrcpy(dir, g_opt.dir);
    add_slash(dir);
    if (lstrcmpi(dir, g_src) == 0) {
        if (spawn_remover(g_opt.dir))
            return TRUE;
        say("Setup could not remove the files. Delete the directory by hand.", MB_ICONSTOP);
        return FALSE;
    }
    ok = remove_files(g_opt.dir, FALSE);
    offer_restart();
    return ok;
}

/* Deletes the copy staged for a shell handover, unless it is still to run. */
static void stage_cleanup(void)
{
    char stage[MAX_PATHLEN], exe[MAX_PATHLEN + 16];
    stage_dir(stage);
    wsprintf(exe, "%s\\SETUP.EXE", (LPSTR)stage);
    if (!file_exists(exe) || list_has("run", exe))
        return;
    lstrcpy(exe, stage);
    add_slash(exe);
    if (lstrcmpi(exe, g_src) == 0)
        return;
    remove_files(stage, TRUE);
}

/* ---- WinMain ---------------------------------------------------------- */

static void register_classes(void)
{
    WNDCLASS wc;
    memset(&wc, 0, sizeof(wc));
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpfnWndProc = PreviewProc;
    wc.lpszClassName = "W16Preview";
    RegisterClass(&wc);
    wc.lpfnWndProc = GaugeProc;
    wc.lpszClassName = "W16Gauge";
    RegisterClass(&wc);
    wc.lpfnWndProc = BackProc;
    wc.hIcon = LoadIcon(g_inst, MAKEINTRESOURCE(IDI_SETUP));
    wc.lpszClassName = "W16SetupBack";
    RegisterClass(&wc);
}

/* Whether /NAME or /NAME=value is on the command line; the value (upper
 * case) goes to val. */
static BOOL option(LPCSTR cmd, LPCSTR name, char *val, int size)
{
    char tok[MAX_PATHLEN];
    int n, k = lstrlen(name);
    while (*cmd) {
        while (*cmd == ' ')
            cmd++;
        for (n = 0; *cmd && *cmd != ' '; cmd++)
            if (n < sizeof(tok) - 1)
                tok[n++] = *cmd;
        tok[n] = 0;
        if (n <= k || (tok[0] != '/' && tok[0] != '-'))
            continue;
        AnsiUpper(tok);
        if (_fstrncmp(tok + 1, name, k) == 0 && (tok[k + 1] == 0 || tok[k + 1] == '=')) {
            if (val)
                lstrcpyn(val, tok[k + 1] ? tok + k + 2 : "", size);
            return TRUE;
        }
    }
    return FALSE;
}

int PASCAL WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    char t[MAX_PATHLEN + 32], logp[MAX_PATHLEN + 16];
    static char rcmd[MAX_TEXT];
    int rc = 0, i;
    BOOL remove = FALSE;
    FARPROC dp;
    (void)show;

    if (prev) {
        HWND w = FindWindow("W16SetupBack", NULL);
        if (w)
            BringWindowToTop(w);
        return 0;
    }
    g_inst = inst;
    GetModuleFileName(inst, g_src, sizeof(g_src));
    *base_name(g_src) = 0;
    g_silent = option(cmd, "S", NULL, 0);
    register_classes();
    lstrcpy(g_title, "Setup");

    /* second stage of removing, running from TEMP */
    if (option(cmd, "X", g_opt.dir, sizeof(g_opt.dir))) {
        DWORD t0 = GetTickCount();
        while (GetModuleHandle(MODNAME) && GetTickCount() - t0 < 15000)
            pump();
        remove_files(g_opt.dir, FALSE);
        g_need_restart = option(cmd, "R", NULL, 0);
        g_cmd_restart = option(cmd, "RESTART", NULL, 0);
        offer_restart();
        wsprintf(logp, "%s%s.LOG", (LPSTR)g_src, (LPSTR)MODNAME_TMP);
        log_flush(logp);
        if (g_restart_now)
            ExitWindows(EW_RESTARTWINDOWS, 0);
        return 0;
    }

    /* the copy left in TEMP by the last removal */
    if (!GetModuleHandle(MODNAME_TMP)) {
        temp_path(t, MODNAME_TMP ".EXE");
        file_delete(t);
    }
    /* the copy left in TEMP by a shell handover */
    stage_cleanup();

    wsprintf(g_inf, "%sSETUP.INF", (LPSTR)g_src);
    if (!load_inf()) {
        say("SETUP.INF is missing or damaged. Setup cannot continue.", MB_ICONSTOP);
        return 1;
    }
    /* the staged copy started from run= after a shell handover */
    stage_dir(t);
    add_slash(t);
    if (option(cmd, "RESUME", NULL, 0)
        || (lstrcmpi(t, g_src) == 0 && GetProfileString(g_key, "Resume", "", logp, sizeof(logp)))) {
        resume_begin(rcmd, sizeof(rcmd));
        cmd = rcmd;
        g_resumed = TRUE;
        g_silent = option(cmd, "S", NULL, 0);
    }

    /* defaults: the current installation or SETUP.INF */
    g_installed = GetProfileString(g_key, "InstallDir", "", g_opt.dir, sizeof(g_opt.dir)) > 0;
    if (!g_installed)
        inf_get("Setup", "DefaultDir", "C:\\PROGRAM", g_opt.dir, sizeof(g_opt.dir));
    inf_get("Choice", "Default", g_nitems ? g_items[0] : "", g_opt.choice, sizeof(g_opt.choice));
    g_opt.load = GetPrivateProfileInt("Autostart", "Default", 1, g_inf);
    g_opt.group = GetPrivateProfileInt("Group", "Default", 1, g_inf);
    g_opt.start = GetPrivateProfileInt("StartNow", "Default", 1, g_inf);
    if (g_installed) {
        GetProfileString(g_key, "Choice", "", t, sizeof(t));
        if (item_index(t) >= 0)
            lstrcpy(g_opt.choice, t);
        g_opt.load = load_has_program();
    }
    if (option(cmd, "D", t, sizeof(t)) && t[0]) {
        lstrcpy(g_opt.dir, t);
        cut_slash(g_opt.dir);
    }
    if (g_nitems && option(cmd, g_switch, t, sizeof(t)) && item_index(t) >= 0)
        lstrcpy(g_opt.choice, t);
    for (i = 0; i < g_nopts; i++) {
        OptionDef *o = &g_opts[i];
        char sec[32], v[4];
        wsprintf(sec, "Option.%s", (LPSTR)o->name);
        o->on = GetPrivateProfileInt(sec, "Default", 1, g_inf) != 0;
        if (g_installed && GetProfileString(g_key, sec, "", v, sizeof(v))) {
            o->was_on = v[0] == '1';
            o->on = o->was_on;
        }
        wsprintf(t, "NO%s", (LPSTR)o->sw);
        if (option(cmd, o->sw, NULL, 0))
            o->on = TRUE;
        if (option(cmd, t, NULL, 0))
            o->on = FALSE;
    }
    if (option(cmd, "LOAD", NULL, 0))
        g_opt.load = TRUE;
    if (option(cmd, "GROUP", NULL, 0))
        g_opt.group = TRUE;
    if (option(cmd, "START", NULL, 0))
        g_opt.start = TRUE;
    if (option(cmd, "NOLOAD", NULL, 0))
        g_opt.load = FALSE;
    if (option(cmd, "NOGROUP", NULL, 0))
        g_opt.group = FALSE;
    g_nostart = option(cmd, "NOSTART", NULL, 0);
    if (g_nostart)
        g_opt.start = FALSE;
    g_cmd_restart = option(cmd, "RESTART", NULL, 0);
    g_opt.load = g_opt.load && g_auto[0];
    g_opt.group = g_opt.group && g_group[0];
    g_opt.start = g_opt.start && g_run[0];
    remove = option(cmd, "U", NULL, 0);

    if (!g_silent) {
        g_back = CreateWindow("W16SetupBack", g_title, WS_POPUP | WS_CLIPCHILDREN, 0, 0,
                              GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), NULL, NULL, inst,
                              NULL);
        ShowWindow(g_back, SW_SHOW);
        UpdateWindow(g_back);
    }

    if (!remove && !g_silent && !g_resumed) {
        dp = MakeProcInstance((FARPROC)MainProc, inst);
        i = DialogBox(inst, MAKEINTRESOURCE(IDD_MAIN), g_back, (DLGPROC)dp);
        FreeProcInstance(dp);
        preview_free();
        if (i == IDCANCEL || i < 0) {
            log_line("cancelled");
            rc = 1;
        }
        remove = i == IDC_REMOVE;
    }
    if (rc == 0) {
        if (remove) {
            if (!g_installed) {
                lstrcpy(g_opt.dir, g_src);
                cut_slash(g_opt.dir);
            }
            rc = uninstall() ? 0 : 1;
            /* the log goes to TEMP (the directory is about to go) */
            temp_path(logp, MODNAME_TMP ".LOG");
        } else {
            rc = install() ? 0 : 1;
            wsprintf(logp, "%s\\SETUP.LOG", (LPSTR)g_opt.dir);
        }
    } else {
        wsprintf(logp, "%s\\SETUP.LOG", (LPSTR)g_opt.dir);
    }
    if (g_resumed && rc == 0) {
        /* back to the product as the shell: Windows has to restart */
        GetPrivateProfileString("boot", "shell", "progman.exe", t, sizeof(t), "SYSTEM.INI");
        if (lstrcmpi(base_name(t), "PROGMAN.EXE") != 0) {
            wsprintf(t, "Setup will now restart Windows to start %s.", (LPSTR)g_product);
            log_line(t);
            if (!g_silent)
                MessageBox(g_back, t, g_title, MB_OK | MB_ICONINFORMATION);
            g_restart_now = TRUE;
        }
    }
    if (!log_flush(logp)) {
        /* no installation directory */
        GetWindowsDirectory(logp, sizeof(logp) - 16);
        add_slash(logp);
        lstrcat(logp, "W16SETUP.LOG");
        log_flush(logp);
    }
    if (g_back)
        DestroyWindow(g_back);
    if (g_restart_now && !ExitWindows(EW_RESTARTWINDOWS, 0) && !g_silent)
        MessageBox(NULL, "Windows could not be restarted because a program refused to close. Restart "
                   "Windows to complete the installation.", g_title, MB_OK | MB_ICONEXCLAMATION);
    return rc;
}
