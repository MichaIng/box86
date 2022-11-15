#define _GNU_SOURCE         /* See feature_test_macros(7) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <signal.h>

#include "box86context.h"
#include "elfloader.h"
#include "debug.h"
#include "x86trace.h"
#include "x86emu.h"
#include "librarian.h"
#include "bridge.h"
#include "library.h"
#include "callback.h"
#include "wrapper.h"
#include "myfts.h"
#include "threads.h"
#include "x86trace.h"
#include "signals.h"
#include <sys/mman.h>
#include "custommem.h"
#include "dictionnary.h"
#ifdef DYNAREC
#include "dynablock.h"
#include "dynarec/arm_lock_helper.h"
#endif



EXPORTDYN
void initAllHelpers(box86context_t* context)
{
    static int inited = 0;
    if(inited)
        return;
    my_context = context;
    init_pthread_helper();
    init_bridge_helper();
    init_signal_helper(context);
    inited = 1;
}

EXPORTDYN
void finiAllHelpers(box86context_t* context)
{
    static int finied = 0;
    if(finied)
        return;
    fini_pthread_helper(context);
    fini_signal_helper();
    fini_bridge_helper();
    fini_custommem_helper(context);
    finied = 1;
}

void x86Syscall(x86emu_t *emu);

/// maxval not inclusive
int getrand(int maxval)
{
    if(maxval<1024) {
        return ((random()&0x7fff)*maxval)/0x7fff;
    } 
        uint64_t r = random();
        r = (r*maxval) / RAND_MAX;
        return r;

}

void free_tlsdatasize(void* p)
{
    if(!p)
        return;
    tlsdatasize_t *data = (tlsdatasize_t*)p;
    box_free(data->ptr);
    box_free(p);
}

int unlockMutex()
{
    int ret = unlockCustommemMutex();
    int i;
    #define GO(A, B)                    \
        i = checkUnlockMutex(&A);       \
        if(i) {                         \
            ret|=(1<<B);                \
        }

    GO(my_context->mutex_once, 5)
    GO(my_context->mutex_once2, 6)
    GO(my_context->mutex_trace, 7)
    #ifdef DYNAREC
    GO(my_context->mutex_dyndump, 8)
    #else
    GO(my_context->mutex_lock, 8)
    #endif
    GO(my_context->mutex_tls, 9)
    GO(my_context->mutex_thread, 10)
    GO(my_context->mutex_bridge, 11)
    #undef GO

    return ret;
}

void relockMutex(int locks)
{
    #define GO(A, B)                    \
        if(locks&(1<<B))                \
            pthread_mutex_lock(&A);     \

    GO(my_context->mutex_once, 5)
    GO(my_context->mutex_once2, 6)
    GO(my_context->mutex_trace, 7)
    #ifdef DYNAREC
    GO(my_context->mutex_dyndump, 8)
    #else
    GO(my_context->mutex_lock, 8)
    #endif
    GO(my_context->mutex_tls, 9)
    GO(my_context->mutex_thread, 10)
    GO(my_context->mutex_bridge, 11)
    #undef GO
    relockCustommemMutex(locks);
}

static void init_mutexes(box86context_t* context)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&context->mutex_once, &attr);
    pthread_mutex_init(&context->mutex_once2, &attr);
    pthread_mutex_init(&context->mutex_trace, &attr);
#ifdef DYNAREC
    pthread_mutex_init(&context->mutex_dyndump, &attr);
#else
    pthread_mutex_init(&context->mutex_lock, &attr);
#endif
    pthread_mutex_init(&context->mutex_tls, &attr);
    pthread_mutex_init(&context->mutex_thread, &attr);
    pthread_mutex_init(&context->mutex_bridge, &attr);

    pthread_mutexattr_destroy(&attr);
}

static void atfork_child_box64context(void)
{
    // (re)init mutex if it was lock before the fork
    init_mutexes(my_context);
}

void freeCycleLog(box86context_t* ctx)
{
    if(cycle_log) {
        for(int i=0; i<cycle_log; ++i) {
            box_free(ctx->log_call[i]);
            box_free(ctx->log_ret[i]);
        }
        box_free(ctx->log_call);
        box_free(ctx->log_ret);
        ctx->log_call = NULL;
        ctx->log_ret = NULL;
    }
}
void initCycleLog(box86context_t* context)
{
    if(cycle_log) {
        context->log_call = (char**)box_calloc(cycle_log, sizeof(char*));
        context->log_ret = (char**)box_calloc(cycle_log, sizeof(char*));
        for(int i=0; i<cycle_log; ++i) {
            context->log_call[i] = (char*)box_calloc(256, 1);
            context->log_ret[i] = (char*)box_calloc(128, 1);
        }
    }
}

EXPORTDYN
box86context_t *NewBox86Context(int argc)
{
#ifdef BUILD_DYNAMIC
    if(my_context) {
        ++my_context->count;
        return my_context;
    }
#endif
    // init and put default values
    box86context_t *context = my_context = (box86context_t*)box_calloc(1, sizeof(box86context_t));

    initCycleLog(context);

#ifdef BUILD_LIB
    context->deferedInit = 0;
#else
    context->deferedInit = 1;
#endif
    context->sel_serial = 1;

    init_custommem_helper(context);

    context->maplib = NewLibrarian(context, 1);
    context->local_maplib = NewLibrarian(context, 1);
    context->versym = NewDictionnary();
    context->system = NewBridge();
    context->globaldefver = NewDefaultVersion();
    context->weakdefver = NewDefaultVersion();
    // create vsyscall
    context->vsyscall = AddBridge(context->system, iFEv, x86Syscall, 0, NULL);
    addAlternate((void*)0xffffe400, (void*)context->vsyscall);
#ifdef BUILD_LIB
    context->box86lib = RTLD_DEFAULT;   // not ideal
#else
    context->box86lib = dlopen(NULL, RTLD_NOW|RTLD_GLOBAL);
#endif
    context->dlprivate = NewDLPrivate();

    context->argc = argc;
    context->argv = (char**)box_calloc(context->argc+1, sizeof(char*));

    init_mutexes(context);
    pthread_atfork(NULL, NULL, atfork_child_box64context);

    pthread_key_create(&context->tlskey, free_tlsdatasize);

    InitFTSMap(context);

    for (int i=0; i<4; ++i) context->canary[i] = 1 +  getrand(255);
    context->canary[/*getrand(4)*/0] = 0;
    printf_log(LOG_DEBUG, "Setting up canary (for Stack protector) at GS:0x14, value:%08X\n", *(uint32_t*)context->canary);

    initAllHelpers(context);

    return context;
}

EXPORTDYN
void FreeBox86Context(box86context_t** context)
{
#ifdef BUILD_DYNAMIC
    if(--(*context)->count)
        return;
#endif
    if(!context)
        return;
    
    if(--(*context)->forked >= 0)
        return;

    box86context_t* ctx = *context;   // local copy to do the cleanning

    if(ctx->local_maplib)
        FreeLibrarian(&ctx->local_maplib, NULL);
    if(ctx->maplib)
        FreeLibrarian(&ctx->maplib, NULL);
    FreeDictionnary(&ctx->versym);

    for(int i=0; i<ctx->elfsize; ++i) {
        FreeElfHeader(&ctx->elfs[i]);
    }
    box_free(ctx->elfs);

    FreeFTSMap(ctx);

    FreeCollection(&ctx->box86_path);
    FreeCollection(&ctx->box86_ld_lib);
    FreeCollection(&ctx->box86_emulated_libs);
    // stop trace now
    if(ctx->dec)
        DeleteX86TraceDecoder(&ctx->dec);
    if(ctx->zydis)
        DeleteX86Trace(ctx);

    if(ctx->deferedInitList)
        box_free(ctx->deferedInitList);

    /*box_free(ctx->argv);*/
    
    /*for (int i=0; i<ctx->envc; ++i)
        box_free(ctx->envv[i]);
    box_free(ctx->envv);*/

    if(ctx->atfork_sz) {
        box_free(ctx->atforks);
        ctx->atforks = NULL;
        ctx->atfork_sz = ctx->atfork_cap = 0;
    }

    for(int i=0; i<MAX_SIGNAL; ++i)
        if(ctx->signals[i]!=0 && ctx->signals[i]!=1) {
            signal(i, SIG_DFL);
        }

    *context = NULL;                // bye bye my_context

    CleanStackSize(ctx);

#ifndef BUILD_LIB
    if(ctx->box86lib)
        dlclose(ctx->box86lib);
#endif

    FreeDLPrivate(&ctx->dlprivate);

//    box_free(ctx->stack); // don't free the stack, it's owned by main emu!

    box_free(ctx->fullpath);
    box_free(ctx->box86path);
    box_free(ctx->bashpath);

    FreeBridge(&ctx->system);

    freeGLProcWrapper(ctx);
    freeALProcWrapper(ctx);

    if(ctx->stack_clone)
        box_free(ctx->stack_clone);

    void* ptr;
    if ((ptr = pthread_getspecific(ctx->tlskey)) != NULL) {
        free_tlsdatasize(ptr);
        pthread_setspecific(ctx->tlskey, NULL);
    }
    pthread_key_delete(ctx->tlskey);

    if(ctx->tlsdata)
        box_free(ctx->tlsdata);

    for(int i=0; i<3; ++i) {
        if(ctx->segtls[i].present) {
            // key content not handled by box86, so not doing anything with it
            pthread_key_delete(ctx->segtls[i].key);
        }
    }

    free_neededlib(&ctx->neededlibs);

    if(ctx->emu_sig)
        FreeX86Emu(&ctx->emu_sig);
    FreeDefaultVersion(&ctx->globaldefver);
    FreeDefaultVersion(&ctx->weakdefver);

    finiAllHelpers(ctx);

    pthread_mutex_destroy(&ctx->mutex_once);
    pthread_mutex_destroy(&ctx->mutex_once2);
    pthread_mutex_destroy(&ctx->mutex_trace);
#ifdef DYNAREC
    pthread_mutex_destroy(&ctx->mutex_dyndump);
#else
    pthread_mutex_destroy(&ctx->mutex_lock);
#endif
    pthread_mutex_destroy(&ctx->mutex_tls);
    pthread_mutex_destroy(&ctx->mutex_thread);
    pthread_mutex_destroy(&ctx->mutex_bridge);

    freeCycleLog(ctx);

    box_free(ctx);
}

int AddElfHeader(box86context_t* ctx, elfheader_t* head) {
    int idx = ctx->elfsize;
    if(idx==ctx->elfcap) {
        // resize...
        ctx->elfcap += 16;
        ctx->elfs = (elfheader_t**)box_realloc(ctx->elfs, sizeof(elfheader_t*) * ctx->elfcap);
    }
    ctx->elfs[idx] = head;
    ctx->elfsize++;
    printf_log(LOG_DEBUG, "Adding \"%s\" as #%d in elf collection\n", ElfName(head), idx);
    return idx;
}

int AddTLSPartition(box86context_t* context, int tlssize) {
    int oldsize = context->tlssize;
    context->tlssize += tlssize;
    context->tlsdata = box_realloc(context->tlsdata, context->tlssize);
    memmove(context->tlsdata+tlssize, context->tlsdata, oldsize);   // move to the top, using memmove as regions will probably overlap
    memset(context->tlsdata, 0, tlssize);           // fill new space with 0 (not mandatory)
    // clean GS segment for current emu
    if(my_context) {
        ResetSegmentsCache(thread_get_emu());
        if(!(++context->sel_serial))
            ++context->sel_serial;
    }

    return -context->tlssize;   // negative offset
}
