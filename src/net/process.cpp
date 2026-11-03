#include "process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

bool process_start(process *p, const char *const *argv) {
    memset(p, 0, sizeof(*p));
    char line[4096];
    size_t at = 0;
    for (int i = 0; argv[i]; ++i) {
        size_t n = strlen(argv[i]);
        if (at + n + 4 >= sizeof(line)) return false;
        if (i) line[at++] = ' ';
        line[at++] = '"';
        memcpy(line + at, argv[i], n);
        at += n;
        line[at++] = '"';
    }
    line[at] = '\0';
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    p->handle = pi.hProcess;
    p->id = pi.dwProcessId;
    p->started = true;
    return true;
}
bool process_running(process *p) {
    if (!p->started) return false;
    return WaitForSingleObject(p->handle, 0) == WAIT_TIMEOUT;
}
void process_terminate(process *p) {
    if (!p->started) return;
    TerminateProcess(p->handle, 0);
    WaitForSingleObject(p->handle, 2000);
    process_forget(p);
}
void process_forget(process *p) {
    if (p->started) CloseHandle(p->handle);
    memset(p, 0, sizeof(*p));
}
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

bool process_start(process *p, const char *const *argv) {
    memset(p, 0, sizeof(*p));
    pid_t id = fork();
    if (id < 0) return false;
    if (id == 0) {
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    p->id = id;
    p->started = true;
    return true;
}
bool process_running(process *p) {
    if (!p->started) return false;
    int status;
    return waitpid(p->id, &status, WNOHANG) == 0;
}
void process_terminate(process *p) {
    if (!p->started) return;
    kill(p->id, SIGTERM);
    for (int i = 0; i < 200; ++i) {   // up to two seconds to go quietly
        int status;
        if (waitpid(p->id, &status, WNOHANG) != 0) {
            memset(p, 0, sizeof(*p));
            return;
        }
        usleep(10000);
    }
    kill(p->id, SIGKILL);
    int status;
    waitpid(p->id, &status, 0);
    memset(p, 0, sizeof(*p));
}
void process_forget(process *p) {
    if (p->started) {
        int status;
        waitpid(p->id, &status, WNOHANG);
    }
    memset(p, 0, sizeof(*p));
}
#endif
