/********************************************************************************/
/*                                                                              */
/*                           TPM Main Program                                   */
/*                 Written by Ken Goldman, Stefan Berger                        */
/*                     IBM Thomas J. Watson Research Center                     */
/*                                                                              */
/* (c) Copyright IBM Corporation 2006, 2010, 2015, 2016.			*/
/*										*/
/* All rights reserved.								*/
/* 										*/
/* Redistribution and use in source and binary forms, with or without		*/
/* modification, are permitted provided that the following conditions are	*/
/* met:										*/
/* 										*/
/* Redistributions of source code must retain the above copyright notice,	*/
/* this list of conditions and the following disclaimer.			*/
/* 										*/
/* Redistributions in binary form must reproduce the above copyright		*/
/* notice, this list of conditions and the following disclaimer in the		*/
/* documentation and/or other materials provided with the distribution.		*/
/* 										*/
/* Neither the names of the IBM Corporation nor the names of its		*/
/* contributors may be used to endorse or promote products derived from		*/
/* this software without specific prior written permission.			*/
/* 										*/
/* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS		*/
/* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT		*/
/* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR	*/
/* A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT		*/
/* HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,	*/
/* SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT		*/
/* LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,	*/
/* DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY	*/
/* THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT		*/
/* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE	*/
/* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.		*/
/********************************************************************************/

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <getopt.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>

#include <libtpms/tpm_error.h>
#include <libtpms/tpm_library.h>
#include <libtpms/tpm_memory.h>

#include "main.h"
#include "swtpm_debug.h"
#include "swtpm_nvfile.h"
#include "common.h"
#include "logging.h"
#include "pidfile.h"
#include "tpmlib.h"
#include "utils.h"
#include "ctrlchannel.h"
#include "mainloop.h"

/* local variables */
static int notify_fd[2] = {-1, -1};

static struct libtpms_callbacks callbacks = {
    .sizeOfStruct            = sizeof(struct libtpms_callbacks),
    .tpm_nvram_init          = SWTPM_NVRAM_Init,
    .tpm_nvram_loaddata      = SWTPM_NVRAM_LoadData,
    .tpm_nvram_storedata     = SWTPM_NVRAM_StoreData,
    .tpm_nvram_deletename    = SWTPM_NVRAM_DeleteName,
    .tpm_io_init             = NULL,
};

static void sigterm_handler(int sig __attribute__((unused)))
{
    TPM_DEBUG("Terminating...\n");
    if (write(notify_fd[1], "T", 1) < 0) {
        logprintf(STDERR_FILENO, "Error: sigterm notification failed: %s\n",
                  strerror(errno));
    }
    mainloop_terminate = true;
}

static void usage(FILE *file, const char *prgname, const char *iface)
{
    fprintf(file,
    "Usage: %s %s [options]\n"
    "\n"
    "The following options are supported:\n"
    "\n"
    "-c|--chardev <device>\n"
    "                 : use the given character device\n"
    "-f|--fd <fd>     : use the given character device file descriptor\n"
    "-d|--daemon      : daemonize the TPM\n"
    "--ctrl type=[unixio|tcp][,path=<path>][,port=<port>][,fd=<filedescriptor]\n"
    "                 : TPM control channel using either UnixIO or TCP sockets;\n"
    "                   the path is only valid for Unixio channels; the port must\n"
    "                   be given in case the type is TCP; the TCP socket is bound\n"
    "                   to 127.0.0.1\n"
    "--log file=<path>|fd=<filedescriptor>\n"
    "                 : write the TPM's log into the given file rather than\n"
    "                   to the console; provide '-' for path to avoid logging\n"
    "--key file=<path>[,mode=aes-cbc][,format=hex|binary][,remove=[true|false]]\n"
    "                 : use an AES key for the encryption of the TPM's state\n"
    "                   files; use the given mode for the block encryption;\n"
    "                   the key is to be provided as a hex string or in binary\n"
    "                   format; the keyfile can be automatically removed using\n"
    "                   the remove parameter\n"
    "--key pwdfile=<path>[,mode=aes-cbc][,remove=[true|false]]\n"
    "                 : provide a passphrase in a file; the AES key will be\n"
    "                   derived from this passphrase\n"
    "--pid file=<path>\n"
    "                 : write the process ID into the given file\n"
    "--tpmstate dir=<dir>\n"
    "                 : set the directory where the TPM's state will be written\n"
    "                   into; the TPM_PATH environment variable can be used\n"
    "                   instead\n"
    "-r|--runas <user>: change to the given user\n"
    "-h|--help        : display this help screen and terminate\n"
    "\n",
    prgname, iface);
}

int swtpm_chardev_main(int argc, char **argv, const char *prgname, const char *iface)
{
    TPM_RESULT rc = 0;
    int daemonize = FALSE;
    int opt, longindex;
    struct stat statbuf;
    struct mainLoopParams mlp = {
        .fd = -1,
        .flags = 0,
    };
    unsigned long val;
    char *end_ptr;
    char *keydata = NULL;
    char *logdata = NULL;
    char *piddata = NULL;
    char *tpmstatedata = NULL;
    char *ctrlchdata = NULL;
    char *runas = NULL;
#ifdef DEBUG
    time_t              start_time;
#endif
    static struct option longopts[] = {
        {"daemon"    ,       no_argument, 0, 'd'},
        {"help"      ,       no_argument, 0, 'h'},
        {"chardev"   , required_argument, 0, 'c'},
        {"fd"        , required_argument, 0, 'f'},
        {"runas"     , required_argument, 0, 'r'},
        {"log"       , required_argument, 0, 'l'},
        {"key"       , required_argument, 0, 'k'},
        {"pid"       , required_argument, 0, 'P'},
        {"tpmstate"  , required_argument, 0, 's'},
        {"ctrl"      , required_argument, 0, 'C'},
        {NULL        , 0                , 0, 0  },
    };

    while (TRUE) {
        opt = getopt_long(argc, argv, "dhc:f:r:", longopts, &longindex);

        if (opt == -1)
            break;

        switch (opt) {
        case 'd':
            daemonize = TRUE;
            break;

        case 'c':
            if (mlp.fd >= 0)
                continue;

            mlp.fd = open(optarg, O_RDWR);
            if (mlp.fd < 0) {
                fprintf(stderr, "Cannot open %s: %s\n",
                        optarg, strerror(errno));
                exit(1);
            }
            break;

        case 'f':
            if (mlp.fd >= 0)
                continue;

            errno = 0;
            val = strtoul(optarg, &end_ptr, 10);
            if (val != (unsigned int)val || errno || end_ptr[0] != '\0') {
                fprintf(stderr, "Cannot parse character device file descriptor.\n");
                exit(1);
            }
            mlp.fd = val;
            if (fstat(mlp.fd, &statbuf) != 0) {
                fprintf(stderr, "Cannot stat file descriptor: %s\n",
                        strerror(errno));
                exit(1);
            }
            /*
             * test for wrong file types; anonymous fd's do not seem to be any of the wrong
             * ones but are also not character devices
             */
            if (S_ISREG(statbuf.st_mode) || S_ISDIR(statbuf.st_mode) || S_ISBLK(statbuf.st_mode)
                || S_ISLNK(statbuf.st_mode)) {
                fprintf(stderr,
                        "Given file descriptor type is not supported.\n");
                exit(1);
            }
            mlp.flags |= MAIN_LOOP_FLAG_TERMINATE;

            break;

        case 'k':
            keydata = optarg;
            break;

        case 'l':
            logdata = optarg;
            break;

        case 'P':
            piddata = optarg;
            break;

        case 's':
            tpmstatedata = optarg;
            break;

        case 'C':
            ctrlchdata = optarg;
            break;

        case 'h':
            usage(stdout, prgname, iface);
            exit(EXIT_SUCCESS);

        case 'r':
            runas = optarg;
            break;

        default:
            usage(stderr, prgname, iface);
            exit(EXIT_FAILURE);
        }
    }

    if (mlp.fd < 0) {
        logprintf(STDERR_FILENO, "Error: Missing character device or file descriptor\n");
        return EXIT_FAILURE;
    }

    /* change process ownership before accessing files */
    if (runas) {
        if (change_process_owner(runas) < 0)
            return EXIT_FAILURE;
    }

    if (handle_log_options(logdata) < 0 ||
        handle_key_options(keydata) < 0 ||
        handle_pid_options(piddata) < 0 ||
        handle_tpmstate_options(tpmstatedata) < 0 ||
        handle_ctrlchannel_options(ctrlchdata, &mlp.cc) < 0)
        return EXIT_FAILURE;

    if (daemonize) {
       if (0 != daemon(0, 0)) {
           logprintf(STDERR_FILENO, "Error: Could not daemonize.\n");
           return EXIT_FAILURE;
       }
    }

    if (pidfile_write(getpid()) < 0) {
        return EXIT_FAILURE;
    }

    setvbuf(stdout, 0, _IONBF, 0);      /* output may be going through pipe */

#ifdef DEBUG
    /* initialization */
    start_time = time(NULL);
#endif

    TPM_DEBUG("main: Initializing TPM at %s", ctime(&start_time));

    TPM_DEBUG("Main: Compiled for %u auth, %u transport, and %u DAA session slots\n",
           tpmlib_get_tpm_property(TPMPROP_TPM_MIN_AUTH_SESSIONS),
           tpmlib_get_tpm_property(TPMPROP_TPM_MIN_TRANS_SESSIONS),
           tpmlib_get_tpm_property(TPMPROP_TPM_MIN_DAA_SESSIONS));
    TPM_DEBUG("Main: Compiled for %u key slots, %u owner evict slots\n",
           tpmlib_get_tpm_property(TPMPROP_TPM_KEY_HANDLES),
           tpmlib_get_tpm_property(TPMPROP_TPM_OWNER_EVICT_KEY_HANDLES));
    TPM_DEBUG("Main: Compiled for %u counters, %u saved sessions\n",
           tpmlib_get_tpm_property(TPMPROP_TPM_MIN_COUNTERS),
           tpmlib_get_tpm_property(TPMPROP_TPM_MIN_SESSION_LIST));
    TPM_DEBUG("Main: Compiled for %u family, %u delegate table entries\n",
           tpmlib_get_tpm_property(TPMPROP_TPM_NUM_FAMILY_TABLE_ENTRY_MIN),
           tpmlib_get_tpm_property(TPMPROP_TPM_NUM_DELEGATE_TABLE_ENTRY_MIN));
    TPM_DEBUG("Main: Compiled for %u total NV, %u savestate, %u volatile space\n",
           tpmlib_get_tpm_property(TPMPROP_TPM_MAX_NV_SPACE),
           tpmlib_get_tpm_property(TPMPROP_TPM_MAX_SAVESTATE_SPACE),
           tpmlib_get_tpm_property(TPMPROP_TPM_MAX_VOLATILESTATE_SPACE));
#if 0
    TPM_DEBUG("Main: Compiled for %u NV defined space\n",
           tpmlib_get_tpm_property(TPMPROP_TPM_MAX_NV_DEFINED_SIZE));
#endif

    if ((rc = tpmlib_start(&callbacks, 0)))
        goto error_no_tpm;

    if (install_sighandlers(notify_fd, sigterm_handler) < 0)
        goto error_no_sighandlers;

    rc = mainLoop(&mlp, notify_fd[0], &callbacks);

error_no_sighandlers:
    TPMLIB_Terminate();

error_no_tpm:
    pidfile_remove();

    close(notify_fd[0]);
    notify_fd[0] = -1;
    close(notify_fd[1]);
    notify_fd[1] = -1;

    /* Fatal initialization errors cause the program to abort */
    if (rc == 0) {
        return EXIT_SUCCESS;
    }
    else {
        TPM_DEBUG("main: TPM initialization failure %08x, exiting\n", rc);
        return EXIT_FAILURE;
    }
}
