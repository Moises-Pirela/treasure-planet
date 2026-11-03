// Child processes, for the orchestrator: start one with arguments, see whether it
// still runs, ask it to stop, make it stop. Windows and POSIX.
#ifndef TP_PROCESS_H
#define TP_PROCESS_H

#include <stdbool.h>
#include <stdint.h>

struct process {
    bool started;
#ifdef _WIN32
    void *handle;
    uint32_t id;
#else
    int id;
#endif
};

// `argv` is NULL-terminated; argv[0] is the executable. The child inherits stderr.
bool process_start(process *p, const char *const *argv);
bool process_running(process *p);
void process_terminate(process *p);   // SIGTERM / TerminateProcess, then forget it
void process_forget(process *p);      // when it ended on its own

#endif
