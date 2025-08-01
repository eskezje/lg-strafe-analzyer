#define UNICODE
#define _UNICODE

#include <windows.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wchar.h>
#include <time.h>

typedef long long i64;

static LARGE_INTEGER g_freq; 
static LARGE_INTEGER g_start_qpc;

static bool  g_recording   = false;
static bool  g_a_down      = false;
static bool  g_d_down      = false;
static i64   g_last_ts     = 0;

static FILE *g_log = NULL;
static wchar_t g_log_filename[MAX_PATH] = {0};

// interval buckets
static i64 g_overlap_ticks     = 0; // A & D both held
static i64 g_strafe_a_ticks    = 0; // only A held
static i64 g_strafe_d_ticks    = 0; // only D held
static i64 g_undershoot_ticks  = 0; // no A or D

// per key stats
typedef struct {
    bool  down;
    i64   down_ts;          // QPC when key went down 
    i64   total_hold_ticks; // sum of hold durations
    int   presses;          // total key presses
} KeyData;

enum { IDX_W, IDX_A, IDX_S, IDX_D, KEY_COUNT };
static KeyData g_keys[KEY_COUNT] = {0};


static enum {
    MODE_LIVE,
    MODE_PARSE_FILE
} g_mode = MODE_LIVE;

static wchar_t g_parse_filename[MAX_PATH] = {0};

static int vk_to_idx(UINT vk) {
    switch (vk) {
        case 'W': return IDX_W;
        case 'A': return IDX_A;
        case 'S': return IDX_S;
        case 'D': return IDX_D;
        default:  return -1;
    }
}

static int char_to_idx(wchar_t c) {
    switch (c) {
        case L'W': return IDX_W;
        case L'A': return IDX_A;
        case L'S': return IDX_S;
        case L'D': return IDX_D;
        default:   return -1;
    }
}

// hepers
static inline double ticks_to_sec(i64 ticks) {
    return (double)ticks / (double)g_freq.QuadPart;
}

// write to recording file
static void accumulate_interval(i64 now)
{
    if (g_last_ts == 0) {
        g_last_ts = now;
        return;
    }
    i64 dt = now - g_last_ts;
    if (g_a_down && g_d_down)
        g_overlap_ticks += dt;
    else if (g_a_down && !g_d_down)
        g_strafe_a_ticks += dt;
    else if (!g_a_down && g_d_down)
        g_strafe_d_ticks += dt;
    else
        g_undershoot_ticks += dt;

    g_last_ts = now;
}

// generate a timestamped recording file
static void generate_log_filename(wchar_t *buffer, size_t buffer_size)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_s(&timeinfo, &now);
    
    swprintf_s(buffer, buffer_size, L"keylog_%04d%02d%02d_%02d%02d%02d.txt",
               timeinfo.tm_year + 1900,
               timeinfo.tm_mon + 1,
               timeinfo.tm_mday,
               timeinfo.tm_hour,
               timeinfo.tm_min,
               timeinfo.tm_sec);
}

// start new recording session
static bool start_recording_session(void)
{
    // close any existing log file
    if (g_log) {
        fclose(g_log);
        g_log = NULL;
    }
    
    //reset all stats for new session
    memset(g_keys, 0, sizeof(g_keys));
    g_overlap_ticks = g_strafe_a_ticks = g_strafe_d_ticks = g_undershoot_ticks = 0;
    g_a_down = g_d_down = false;
    g_last_ts = 0;
    
    // generate new file name
    generate_log_filename(g_log_filename, MAX_PATH);
    
    // open new log file
    g_log = _wfopen(g_log_filename, L"w, ccs=UTF-8");
    if (!g_log) {
        wprintf(L"Error: Cannot create %ls\n", g_log_filename);
        return false;
    }
    
    // write header with performance frequency
    fwprintf(g_log, L"# PerfFreq %llu\n", (unsigned long long)g_freq.QuadPart);
    fflush(g_log);
    
    wprintf(L"Created new log file: %ls\n", g_log_filename);
    return true;
}
static void log_event(i64 ts, UINT vk, const wchar_t *etype)
{
    if (g_log) {
        fwprintf(g_log, L"%lld %c %ls\n", (long long)(ts - g_start_qpc.QuadPart),
                 (wchar_t)vk, etype);
        fflush(g_log);
    }
}

// parse existing log file
static bool parse_log_file(const wchar_t *filename)
{
    FILE *f = _wfopen(filename, L"r, ccs=UTF-8");
    if (!f) {
        wprintf(L"Error: Cannot open file '%ls'\n", filename);
        return false;
    }

    wprintf(L"Parsing log file: %ls\n", filename);

    wchar_t line[256];
    int line_num = 0;
    bool found_freq = false;

    // reset all stats for parsing
    memset(g_keys, 0, sizeof(g_keys));
    g_overlap_ticks = g_strafe_a_ticks = g_strafe_d_ticks = g_undershoot_ticks = 0;
    g_a_down = g_d_down = false;
    g_last_ts = 0;

    while (fgetws(line, sizeof(line)/sizeof(wchar_t), f)) {
        line_num++;
        
        // Remove trailing newline character
        wchar_t *nl = wcschr(line, L'\n');
        if (nl) *nl = L'\0';
        if (wcslen(line) == 0) continue;

        // parse frequency
        if (line[0] == L'#') {
            unsigned long long freq_val;
            if (swscanf(line, L"# PerfFreq %llu", &freq_val) == 1) {
                g_freq.QuadPart = (LONGLONG)freq_val;
                found_freq = true;
                wprintf(L"Found frequency: %llu Hz\n", freq_val);
            }
            continue;
        }

        // parse key event line
        long long timestamp;
        wchar_t key_char, event_type[16];
        
        if (swscanf(line, L"%lld %lc %15ls", &timestamp, &key_char, event_type) != 3) {
            wprintf(L"Warning: Malformed line %d: %ls\n", line_num, line);
            continue;
        }

        int idx = char_to_idx(key_char);
        if (idx < 0) continue; // ignore non wasd keys

        bool is_down = (wcscmp(event_type, L"DOWN") == 0);
        bool is_up   = (wcscmp(event_type, L"UP") == 0);
        
        if (!is_down && !is_up) {
            wprintf(L"Warning: Unknown event type '%ls' on line %d\n", event_type, line_num);
            continue;
        }

        // For A & D we must bucket the interval before state changes
        if (key_char == L'A' || key_char == L'D')
            accumulate_interval(timestamp);

        KeyData *K = &g_keys[idx];

        if (is_down) {
            if (!K->down) {                // fresh press
                K->down    = true;
                K->down_ts = timestamp;
                K->presses++;
                if (key_char == L'A') g_a_down = true;
                if (key_char == L'D') g_d_down = true;
            }
        } else if (is_up) {
            if (K->down) {                 // valid release
                i64 hold = timestamp - K->down_ts;
                K->total_hold_ticks += hold;
                K->down = false;
                if (key_char == L'A') g_a_down = false;
                if (key_char == L'D') g_d_down = false;
            }
        }
    }

    fclose(f);

    if (!found_freq) {
        wprintf(L"Warning: No frequency information found in log file.\n");
        wprintf(L"Assuming standard frequency of 10MHz for calculations.\n");
        g_freq.QuadPart = 10000000;
    }

    wprintf(L"Parsed %d lines successfully.\n\n", line_num);
    return true;
}

// keyboard hook
#define TOGGLE_VK '1'  /* Alt+1 combo handled in hook */

static void print_summary(void);

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT *kbd = (KBDLLHOOKSTRUCT*)lParam;
        UINT vk   = kbd->vkCode;
        bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        bool up   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);
        LARGE_INTEGER qpc_now;
        QueryPerformanceCounter(&qpc_now);

        /* Toggle recording with Alt+1 */
        if (vk == TOGGLE_VK && down && (GetAsyncKeyState(VK_MENU) & 0x8000)) {
            if (!g_recording) {          // start recording
                if (start_recording_session()) {
                    g_recording = true;
                    g_last_ts   = qpc_now.QuadPart;
                    wprintf(L"[REC] ON  (ticks start %lld)\n", (long long)(g_last_ts - g_start_qpc.QuadPart));
                } else {
                    wprintf(L"[ERROR] Failed to start recording session\n");
                }
            } else {                     // stop recording
                accumulate_interval(qpc_now.QuadPart);
                g_recording = false;
                if (g_log) {
                    fclose(g_log);
                    g_log = NULL;
                }
                wprintf(L"[REC] OFF (session saved to %ls)\n", g_log_filename);
                print_summary();
                wprintf(L"Press Alt+1 to start a new recording session...\n\n");
            }
            return CallNextHookEx(NULL, nCode, wParam, lParam);
        }

        // Handle W/A/S/D events only when recording
        int idx = vk_to_idx(vk);
        if (idx >= 0 && (down || up) && g_recording) {

            // For A & D we must bucket the interval before state changes
            if (vk == 'A' || vk == 'D')
                accumulate_interval(qpc_now.QuadPart);

            KeyData *K = &g_keys[idx];

            if (down) {
                if (!K->down) {                // fresh press
                    K->down       = true;
                    K->down_ts    = qpc_now.QuadPart;
                    K->presses++;
                    if (vk == 'A') g_a_down = true;
                    if (vk == 'D') g_d_down = true;
                    log_event(qpc_now.QuadPart, vk, L"DOWN");
                }
            } else if (up) {
                if (K->down) {                 // valid release
                    i64 hold = qpc_now.QuadPart - K->down_ts;
                    K->total_hold_ticks += hold;
                    K->down = false;
                    if (vk == 'A') g_a_down = false;
                    if (vk == 'D') g_d_down = false;
                    log_event(qpc_now.QuadPart, vk, L"UP");
                }
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// summary
static void print_summary(void)
{
    if (g_mode == MODE_LIVE) {
        LARGE_INTEGER qpc_now;
        QueryPerformanceCounter(&qpc_now);
        accumulate_interval(qpc_now.QuadPart);
    }

    // total timeline
    i64 total_ticks = g_overlap_ticks + g_strafe_a_ticks +
                      g_strafe_d_ticks + g_undershoot_ticks;

    if (total_ticks == 0) {
        wprintf(L"\nNo recording data – goodbye!\n");
        return;
    }

    // sections
    double over_s   = ticks_to_sec(g_overlap_ticks);
    double sa_s     = ticks_to_sec(g_strafe_a_ticks);
    double sd_s     = ticks_to_sec(g_strafe_d_ticks);
    double under_s  = ticks_to_sec(g_undershoot_ticks);
    double total_s  = ticks_to_sec(total_ticks);

    // bias: strafe A vs D time
    double bias_pct = (sa_s + sd_s) ? (sa_s - sd_s) / (sa_s + sd_s) * 100.0 : 0.0;

    /* key counts & averages */
    int   total_presses = 0;
    i64   total_hold    = 0;
    const wchar_t *names[KEY_COUNT] = {L"W", L"A", L"S", L"D"};

    for (int i = 0; i < KEY_COUNT; ++i) {
        total_presses += g_keys[i].presses;
        total_hold    += g_keys[i].total_hold_ticks;
    }

    wprintf(L"\n========== Quake WASD Session Stats ==========\n");
    if (g_mode == MODE_PARSE_FILE) {
        wprintf(L" Analyzed file: %ls\n", g_parse_filename);
    } else {
        wprintf(L" Log file: %ls\n", g_log_filename);
    }
    wprintf(L" Total recorded time: %.3f s\n", total_s);
    wprintf(L" Timer frequency: %llu Hz\n", (unsigned long long)g_freq.QuadPart);

    wprintf(L"\n-- Timeline buckets --\n");
    wprintf(L"  Overlap (A+D)      : %7.3f s  (%5.1f %%)\n", over_s , over_s  / total_s * 100.0);
    wprintf(L"  Strafe  A only     : %7.3f s  (%5.1f %%)\n", sa_s   , sa_s   / total_s * 100.0);
    wprintf(L"  Strafe  D only     : %7.3f s  (%5.1f %%)\n", sd_s   , sd_s   / total_s * 100.0);
    wprintf(L"  Undershoot (none)  : %7.3f s  (%5.1f %%)\n", under_s, under_s/ total_s * 100.0);

    wprintf(L"\n Bias toward A vs D (strafe time): %+5.1f %%\n",
            bias_pct);

    wprintf(L"\n-- Key counts & averages --\n");
    for (int i = 0; i < KEY_COUNT; ++i) {
        if (g_keys[i].presses == 0) continue;
        double avg_hold = ticks_to_sec(g_keys[i].total_hold_ticks) /
                          (double)g_keys[i].presses;
        wprintf(L"  %ls : presses %4d  (%4.1f %% of total)  | "
                L"avg hold %.3f s\n",
                names[i], g_keys[i].presses,
                (double)g_keys[i].presses / total_presses * 100.0,
                avg_hold);
    }

    if (total_presses) {
        double avg_all = ticks_to_sec(total_hold) / (double)total_presses;
        wprintf(L"\n Overall average hold: %.3f s  (all keys)\n", avg_all);
    }

    wprintf(L"==============================================\n\n");
}

/* atexit-handler */
static void cleanup(void)
{
    if (g_log) {
        fclose(g_log);
        g_log = NULL;
    }
}

// ctrl-C / close console handler
static BOOL WINAPI ConsoleHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT) {
        if (g_recording && g_mode == MODE_LIVE) {
            // Finish current recording session
            LARGE_INTEGER qpc_now;
            QueryPerformanceCounter(&qpc_now);
            accumulate_interval(qpc_now.QuadPart);
            g_recording = false;
            if (g_log) {
                fclose(g_log);
                g_log = NULL;
            }
            wprintf(L"\n[REC] Session saved to %ls\n", g_log_filename);
        }
        print_summary();
        // let default handler terminate
    }
    return FALSE;
}

// command line argument parsing
static void print_usage(const wchar_t *prog_name)
{
    wprintf(L"Usage:\n");
    wprintf(L"  %ls                    - Live recording mode\n", prog_name);
    wprintf(L"  %ls <filename>         - Parse existing log file\n", prog_name);
    wprintf(L"  %ls -f <filename>      - Parse existing log file (explicit)\n", prog_name);
    wprintf(L"\nLive mode controls:\n");
    wprintf(L"  Alt+1      - Toggle recording on/off\n");
    wprintf(L"  Ctrl-C   - Show stats and exit\n");
}

static bool parse_arguments(int argc, wchar_t *argv[])
{
    if (argc == 1) {
        // no args, default to live mode
        g_mode = MODE_LIVE;
        return true;
    }
    
    if (argc == 2) {
        // Single argument - treat as filename unless it's -h or --help
        if (wcscmp(argv[1], L"-h") == 0 || wcscmp(argv[1], L"--help") == 0) {
            print_usage(argv[0]);
            return false;
        }
        
        g_mode = MODE_PARSE_FILE;
        wcscpy_s(g_parse_filename, MAX_PATH, argv[1]);
        return true;
    }
    
    if (argc == 3) {
        // Two arguments - check for -f option
        if (wcscmp(argv[1], L"-f") == 0) {
            g_mode = MODE_PARSE_FILE;
            wcscpy_s(g_parse_filename, MAX_PATH, argv[2]);
            return true;
        }
    }
    
    wprintf(L"Error: Invalid arguments\n\n");
    print_usage(argv[0]);
    return false;
}

// main entry point
int wmain(int argc, wchar_t *argv[])
{
    // parse command line arguments
    if (!parse_arguments(argc, argv)) {
        return (argc > 1 && 
                (wcscmp(argv[1], L"-h") == 0 || wcscmp(argv[1], L"--help") == 0)) ? 0 : 1;
    }

    // setting up qpc (high resolution timer)
    if (!QueryPerformanceFrequency(&g_freq) ||
        !QueryPerformanceCounter(&g_start_qpc))
    {
        fwprintf(stderr, L"QPC not supported\n");
        return 1;
    }

    if (g_mode == MODE_PARSE_FILE) {
        // parse existing file
        if (!parse_log_file(g_parse_filename)) {
            return 1;
        }
        print_summary();
        return 0;
    }

    // live recording mode

    wprintf(L"Quake WASD Logger + Stats\n"
            L" - Start recording  : Alt+1 (creates new file)\n"
            L" - Stop recording   : Alt+1 (saves current session)\n"
            L" - Exit / summary   : Ctrl-C\n"
            L"\nWaiting for Alt+1 to start first recording session...\n\n");

    // install low-level keyboard hook
    HHOOK hHook = SetWindowsHookExW(WH_KEYBOARD_LL,
                                    LowLevelKeyboardProc,
                                    GetModuleHandleW(NULL),
                                    0);
    if (!hHook) {
        fwprintf(stderr, L"Failed to install keyboard hook\n");
        if (g_log) {
            fclose(g_log);
            g_log = NULL;
        }
        return 1;
    }

    // console ctrl-handler & atexit cleanup
    if (!SetConsoleCtrlHandler(ConsoleHandler, TRUE)) {
        fwprintf(stderr, L"Failed to set console control handler\n");
        if (g_log) {
            fclose(g_log);
            g_log = NULL;
        }
        UnhookWindowsHookEx(hHook);
        return 1;
    }
    
    atexit(cleanup);

    // message loop
    MSG msg;
    int result;
    while ((result = GetMessageW(&msg, NULL, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Check if GetMessage encountered an error
    if (result < 0) {
        fwprintf(stderr, L"Error in message loop\n");
    }

    // Cleanup hook before exit
    UnhookWindowsHookEx(hHook);
    return (result < 0) ? 1 : 0;
}