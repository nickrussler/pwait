#include <sys/capability.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <signal.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define TRUE 1
#define FALSE 0

/* When the tracee exits, waitpid returns a status
 *  (PTRACE_EVENT_EXIT << 16) | (SIGTRAP << 8) | tracee_exit_code
 * so checking for this value above is how we know we are seeing
 * the process exit, and not just a random signal
 */
#define PTRACE_EXIT_SIGINFO_STATUS (SIGTRAP | (PTRACE_EVENT_EXIT << 8))

/**
 * The process ID being waited for
 */
static pid_t pid;

void dprint(const char* format, ...) {
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    fprintf(stderr, "%s\n", message);
}

void usage(const char* name) {
    fprintf(stderr, "Usage: %s pid\n", name);
}

int cap_free_safe(cap_t* p_capabilities) {
    int status = cap_free(*p_capabilities);
    if (status == -1) {
        dprint("freeing capability struct failed");
    }
    return status;
}

/**
 * Make a best effort to ensure that the process has the CAP_SYS_PTRACE
 * capability. If it already did, or if this function was able to set the
 * capability, this returns 1 (TRUE). Otherwise, this returns 0 (FALSE).
 *
 * The function is a bit long because it checks for error codes at every step,
 * but conceptually what it does is very straightforward:
 * 1. Check whether the CAP_SYS_PTRACE capability is supported by the kernel
 * 2. Check whether the process already has CAP_SYS_PTRACE set, and if so,
 *    return TRUE
 * 3. Check whether the process is allowed to give itself CAP_SYS_PTRACE, and
 *    if not, return FALSE
 * 4. Attempt to actually set CAP_SYS_PTRACE
 * 5. Check again to make sure the process has CAP_SYS_PTRACE, and return TRUE
 *    or FALSE to indicate whether it has it
 */
int prepare_capabilities(void) {
    cap_t capabilities;
    cap_value_t capability_to_add[1];
    cap_flag_value_t cap_sys_ptrace_status;

#ifndef CAP_SYS_PTRACE
    dprint("ptrace capability is not defined");
    return FALSE;
#else
    if (!CAP_IS_SUPPORTED(CAP_SYS_PTRACE)) {
        dprint("ptrace capability is not supported");
        return FALSE;
    }

    capability_to_add[0] = CAP_SYS_PTRACE;

    // check whether this process already has CAP_SYS_PTRACE set
    capabilities = cap_get_proc();
    if (capabilities == NULL) {
        dprint("getting capabilities of this process failed");
        return FALSE;
    }

    if (cap_get_flag(capabilities, CAP_SYS_PTRACE, CAP_EFFECTIVE, &cap_sys_ptrace_status) == -1) {
        dprint("checking effective capabilities failed");
        cap_free_safe(&capabilities);
        return FALSE;
    }

    if (cap_sys_ptrace_status == CAP_SET) {
        dprint("process has CAP_SYS_PTRACE");
        return TRUE;
    }
    else {
        dprint("process does not have CAP_SYS_PTRACE");
    }

    // see if we can set CAP_SYS_PTRACE
    if (cap_get_flag(capabilities, CAP_SYS_PTRACE, CAP_PERMITTED, &cap_sys_ptrace_status) == -1) {
        dprint("checking permitted capabilities failed");
        cap_free_safe(&capabilities);
        return FALSE;
    }

    if (cap_sys_ptrace_status == CAP_SET) {
        dprint("process is permitted to acquire CAP_SYS_PTRACE");
    }
    else {
        dprint("process is not permitted to acquire CAP_SYS_PTRACE");
        return FALSE;
    }

    // actually set CAP_SYS_PTRACE
    if (cap_set_flag(capabilities, CAP_EFFECTIVE, 1, capability_to_add, CAP_SET) == -1) {
        dprint("modifying capability structure failed");
        cap_free_safe(&capabilities);
        return FALSE;
    }

    if (cap_set_proc(capabilities) == -1) {
        dprint("setting capability failed");
        cap_free_safe(&capabilities);
        return FALSE;
    }

    // check whether the process now has CAP_SYS_PTRACE set
    if (cap_get_flag(capabilities, CAP_SYS_PTRACE, CAP_EFFECTIVE, &cap_sys_ptrace_status) == -1) {
        dprint("checking effective capabilities failed");
        cap_free_safe(&capabilities);
        return FALSE;
    }

    if (cap_sys_ptrace_status == CAP_SET) {
        dprint("process has CAP_SYS_PTRACE");
    }
    else {
        dprint("process does not have CAP_SYS_PTRACE");
    }

    // free the memory
    if (cap_free_safe(&capabilities) == -1) {
        return FALSE;
    }

    return cap_sys_ptrace_status == CAP_SET;
#endif
}

int wait_using_waitpid() {
    pid_t returned_pid;
    int waitpid_return_status;

    do {
        returned_pid = waitpid(pid, &waitpid_return_status, 0);
        if (returned_pid == -1) {
            dprint("Error waiting for process %d", pid);
            return FALSE;
        }
        if (returned_pid != pid) {
            dprint("waitpid returned wrong process ID %d (expected %d)", returned_pid, pid);
            return FALSE;
        }
        dprint("return status %x", waitpid_return_status);
    } while (!(WIFSTOPPED(waitpid_return_status) && (waitpid_return_status >> 8 == PTRACE_EXIT_SIGINFO_STATUS)));
    return TRUE;
}

int wait_using_waitid() {
    siginfo_t siginfo;

    do {
        siginfo.si_pid = 0;
        if (waitid(P_PID, pid, &siginfo, WEXITED) != 0) {
            dprint("failed to wait on process %d", pid);
            return FALSE;
        }

        if (siginfo.si_pid == 0) {
            dprint("failed to connect to process %d", pid);
            return FALSE;
        }
        dprint("siginfo status %x", siginfo.si_status);
    } while (!(siginfo.si_code == CLD_TRAPPED && siginfo.si_status == PTRACE_EXIT_SIGINFO_STATUS));
    return TRUE;
}

/**
 * Get the exit status of the traced process, once we know it has exited
 */
int get_tracee_exit_status() {
    unsigned long tracee_exit_status;
    if (ptrace(PTRACE_GETEVENTMSG, pid, NULL, &tracee_exit_status) == -1) {
        dprint("Error getting process %d exit status", pid);
        return -1;
    }
    else {
        return (int)tracee_exit_status;
    }
}

void detach(const int signal) {
    ptrace(PTRACE_DETACH, pid, 0, 0);
}

int main(const int argc, const char** argv) {
    char* endptr;
    long ptrace_return;
    int tracee_exit_code;
    struct sigaction siga, oldsiga_term, oldsiga_int;

    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    if (!prepare_capabilities()) {
        return 1;
    }

    pid = strtol(argv[1], &endptr, 0);
    if (argv[1] == endptr) {
        dprint("First argument must be a numeric PID");
        return 1;
    }
    if (pid < 1) {
        dprint("Invalid process ID %d passed as first argument", pid);
        return 1;
    }

    /* Set up a signal handler so that if the program receives a SIGINT (Ctrl+C)
     * or SIGTERM, it will detach from the tracee
     */
    siga.sa_handler = detach;
    sigaction(SIGTERM, &siga, &oldsiga_term);
    sigaction(SIGINT, &siga, &oldsiga_int);

    dprint("Attempting to set ptrace on process %d", pid);
#ifdef PTRACE_SEIZE
    // valid since Linux kernel 3.4
    ptrace_return = ptrace(PTRACE_SEIZE, pid, NULL, PTRACE_O_TRACEEXIT);
#else
    ptrace_return = ptrace(PTRACE_ATTACH, pid, NULL, NULL);
#endif
    if (ptrace_return == -1) {
        dprint("Error setting ptrace on process %d", pid);
        return 1;
    }

    if (!wait_using_waitpid(pid)) {
        // wait failed
        return 1;
    }
    dprint("Wait successful");

    // Reset the signal handler (hopefully TERM or INT doesn't come right here)
    sigaction(SIGTERM, &oldsiga_term, NULL);
    sigaction(SIGINT, &oldsiga_int, NULL);

    // get the process's exit status
    tracee_exit_code = get_tracee_exit_status(pid);
    dprint("Got exit code");

    if (tracee_exit_code == -1) {
        return 1;
    }
    printf("Process %d exited with code %d\n", pid, tracee_exit_code);
    return 0;
}