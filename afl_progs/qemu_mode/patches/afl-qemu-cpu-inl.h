/*
   american fuzzy lop - high-performance binary-only instrumentation
   -----------------------------------------------------------------

   Written by Andrew Griffiths <agriffiths@google.com> and
              Michal Zalewski <lcamtuf@google.com>

   Idea & design very much by Andrew Griffiths.

   TCG instrumentation and block chaining support by Andrea Biondo
                                      <andrea.biondo965@gmail.com>

   Copyright 2015, 2016, 2017 Google Inc. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   This code is a shim patched into the separately-distributed source
   code of QEMU 2.10.0. It leverages the built-in QEMU tracing functionality
   to implement AFL-style instrumentation and to take care of the remaining
   parts of the AFL fork server logic.

   The resulting QEMU binary is essentially a standalone instrumentation
   tool; for an example of how to leverage it for other purposes, you can
   have a look at afl-showmap.c.

 */

/*
	This file has been modified from the original AFL version to incorporate into
	Killerbeez.  Specifically, the fork server has been modified to match the
  Killerbeez fork server protocol.
 */

#include <sys/shm.h>
#include "../../config.h"
#include "../../../instrumentation/forkserver_internal.h"

/***************************
 * VARIOUS AUXILIARY STUFF *
 ***************************/

/* This snippet kicks in when the instruction pointer is positioned at
   _start and does the usual forkserver stuff, not very different from
   regular instrumentation injected via afl-as.h. */

#define AFL_QEMU_CPU_SNIPPET2 do { \
    if(itb->pc == afl_entry_point) { \
      afl_setup(); \
      afl_forkserver(cpu); \
    } \
  } while (0)

/* We use one additional file descriptor to relay "needs translation"
   messages between the child and the fork server. */

#define TSL_FD QEMU_TSL_FD //import it from forkserver_internal.h

/* This is equivalent to afl-as.h: */

unsigned char dummy[65536];
unsigned char *afl_area_ptr = dummy; /* Exported for afl_gen_trace */

/* Exported variables populated by the code patched into elfload.c: */

abi_ulong afl_entry_point, /* ELF entry point (_start) */
          afl_start_code,  /* .text start pointer      */
          afl_end_code;    /* .text end pointer        */

/* Set in the child process in forkserver mode: */

static unsigned char afl_fork_child;
unsigned int afl_forksrv_pid;

/* Instrumentation ratio: */

unsigned int afl_inst_rms = MAP_SIZE; /* Exported for afl_gen_trace */

/* Function declarations. */

static void afl_setup(void);
static void afl_forkserver(CPUState*);

static void afl_wait_tsl(CPUState*, int);
static void afl_request_tsl(target_ulong, target_ulong, uint32_t, TranslationBlock*, int);

/* Data structures passed around by the translate handlers: */

struct afl_tb {
  target_ulong pc;
  target_ulong cs_base;
  uint32_t flags;
};

struct afl_tsl {
  struct afl_tb tb;
  char is_chain;
};

struct afl_chain {
  struct afl_tb last_tb;
  int tb_exit;
};

/* Some forward decls: */

TranslationBlock *tb_htable_lookup(CPUState*, target_ulong, target_ulong, uint32_t);
static inline TranslationBlock *tb_find(CPUState*, TranslationBlock*, int);

/*************************
 * ACTUAL IMPLEMENTATION *
 *************************/

/* Set up SHM region and initialize other stuff. */

static void afl_setup(void) {

  char *id_str = getenv(SHM_ENV_VAR),
       *inst_r = getenv("AFL_INST_RATIO");

  int shm_id;

  if (inst_r) {

    unsigned int r;

    r = atoi(inst_r);

    if (r > 100) r = 100;
    if (!r) r = 1;

    afl_inst_rms = MAP_SIZE * r / 100;

  }

  if (id_str) {

    shm_id = atoi(id_str);
    afl_area_ptr = shmat(shm_id, NULL, 0);

    if (afl_area_ptr == (void*)-1) exit(1);

    /* With AFL_INST_RATIO set to a low value, we want to touch the bitmap
       so that the parent doesn't give up on us. */

    if (inst_r) afl_area_ptr[0] = 1;


  }

  if (getenv("AFL_INST_LIBS")) {

    afl_start_code = 0;
    afl_end_code   = (abi_ulong)-1;

  }

  /* pthread_atfork() seems somewhat broken in util/rcu.c, and I'm
     not entirely sure what is the cause. This disables that
     behaviour, and seems to work alright? */

  rcu_disable_atfork();

}


/* Fork server logic, invoked once we hit _start. */
static int forkserver_installed = 0;
static void afl_forkserver(CPUState *cpu) {
  static int response = 0x41414141;
  char command;
  int child_pid = -1;
  int t_fd[2];

  if (forkserver_installed == 1)
    return;
  forkserver_installed = 1;
  //if (!afl_area_ptr) return;

  /* Tell the parent that we're alive. If the parent doesn't want
     to talk, assume that we're not running in forkserver mode. */
  if(write(FORKSRV_TO_FUZZER, &response, sizeof(int)) != sizeof(int))
    return;

  afl_forksrv_pid = getpid();

  /* All right, let's await orders... */

  while (1) {

    // Wait for parent by reading from the pipe. Exit if read fails.
    if(read(FUZZER_TO_FORKSRV, &command, sizeof(command)) != sizeof(command))
      _exit(1);

    switch(command) {

      case EXIT:
      case RUN: //QEMU doesn't do the single RUN/FORK commands
      case FORK: //but instead only implements FORK_RUN
        _exit(0);
        break;

      case FORK_RUN:

        /* Establish a channel with child to grab translation commands. We'll
           read from t_fd[0], child will write to TSL_FD. */
        if (pipe(t_fd) || dup2(t_fd[1], TSL_FD) < 0) exit(3);
        close(t_fd[1]);

        child_pid = fork();
        if (child_pid < 0) exit(4);

        if (!child_pid) {

          /* Child process. Close descriptors and run free. */
          afl_fork_child = 1;
          close(FUZZER_TO_FORKSRV);
          close(FORKSRV_TO_FUZZER);
          close(t_fd[0]);
          return;
        }

        /* Parent. */
        response = child_pid;
        if(write(FORKSRV_TO_FUZZER, &response, sizeof(int)) != sizeof(int))
          _exit(1);

        close(TSL_FD);

        /* Collect translation requests until child dies and closes the pipe. */
        afl_wait_tsl(cpu, t_fd[0]);
        break;

      case GET_STATUS:
        /* Get and relay exit status to parent. */
        if(waitpid(child_pid, &response, 0) < 0)
          _exit(1);
        if(write(FORKSRV_TO_FUZZER, &response, sizeof(int)) != sizeof(int))
          _exit(1);
        break;
    }
  }
}

/* This code is invoked whenever QEMU decides that it doesn't have a
   translation of a particular block and needs to compute it, or when it
   decides to chain two TBs together. When this happens, we tell the parent to
   mirror the operation, so that the next fork() has a cached copy. */

static void afl_request_tsl(target_ulong pc, target_ulong cb, uint32_t flags,
                            TranslationBlock *last_tb, int tb_exit) {

  struct afl_tsl t;
  struct afl_chain c;

  if (!afl_fork_child) return;

  t.tb.pc      = pc;
  t.tb.cs_base = cb;
  t.tb.flags   = flags;
  t.is_chain   = (last_tb != NULL);

  if (write(TSL_FD, &t, sizeof(struct afl_tsl)) != sizeof(struct afl_tsl))
    return;

  if (t.is_chain) {
    c.last_tb.pc      = last_tb->pc;
    c.last_tb.cs_base = last_tb->cs_base;
    c.last_tb.flags   = last_tb->flags;
    c.tb_exit         = tb_exit;

    if (write(TSL_FD, &c, sizeof(struct afl_chain)) != sizeof(struct afl_chain))
      return;
  }

}

/* This is the other side of the same channel. Since timeouts are handled by
   afl-fuzz simply killing the child, we can just wait until the pipe breaks. */

static void afl_wait_tsl(CPUState *cpu, int fd) {

  struct afl_tsl t;
  struct afl_chain c;
  TranslationBlock *tb, *last_tb;

  while (1) {

    /* Broken pipe means it's time to return to the fork server routine. */

    if (read(fd, &t, sizeof(struct afl_tsl)) != sizeof(struct afl_tsl))
      break;

    tb = tb_htable_lookup(cpu, t.tb.pc, t.tb.cs_base, t.tb.flags);

    if(!tb) {
      mmap_lock();
      tb_lock();
      tb = tb_gen_code(cpu, t.tb.pc, t.tb.cs_base, t.tb.flags, 0);
      mmap_unlock();
      tb_unlock();
    }

    if (t.is_chain) {
      if (read(fd, &c, sizeof(struct afl_chain)) != sizeof(struct afl_chain))
        break;

      last_tb = tb_htable_lookup(cpu, c.last_tb.pc, c.last_tb.cs_base,
                                 c.last_tb.flags);
      if (last_tb) {
        tb_lock();
        if (!tb->invalid) {
          tb_add_jump(last_tb, c.tb_exit, tb);
        }
        tb_unlock();
      }
    }

  }

  close(fd);

}