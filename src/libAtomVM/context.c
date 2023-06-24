/*
 * This file is part of AtomVM.
 *
 * Copyright 2017 Davide Bettio <davide@uninstall.it>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0 OR LGPL-2.1-or-later
 */

#include "context.h"

#include <fenv.h>
#include <math.h>

#include "dictionary.h"
#include "globalcontext.h"
#include "list.h"
#include "mailbox.h"
#include "smp.h"
#include "synclist.h"
#include "sys.h"

#define IMPL_EXECUTE_LOOP
#include "opcodesswitch.h"
#undef IMPL_EXECUTE_LOOP

#define DEFAULT_STACK_SIZE 8
#define BYTES_PER_TERM (TERM_BITS / 8)

static void context_monitors_handle_terminate(Context *ctx);

Context *context_new(GlobalContext *glb)
{
    Context *ctx = malloc(sizeof(Context));
    if (IS_NULL_PTR(ctx)) {
        fprintf(stderr, "Failed to allocate memory: %s:%i.\n", __FILE__, __LINE__);
        return NULL;
    }
    ctx->cp = 0;

    if (UNLIKELY(memory_init_heap(&ctx->heap, DEFAULT_STACK_SIZE) != MEMORY_GC_OK)) {
        fprintf(stderr, "Failed to allocate memory: %s:%i.\n", __FILE__, __LINE__);
        free(ctx);
        return NULL;
    }
    ctx->e = ctx->heap.heap_end;

    context_clean_registers(ctx, 0);

    ctx->fr = NULL;

    ctx->min_heap_size = 0;
    ctx->max_heap_size = 0;
    ctx->has_min_heap_size = 0;
    ctx->has_max_heap_size = 0;

    mailbox_init(&ctx->mailbox);

    list_init(&ctx->dictionary);

    ctx->native_handler = NULL;

    ctx->saved_module = NULL;
    ctx->saved_ip = NULL;
    ctx->restore_trap_handler = NULL;

    ctx->leader = 0;

    timer_list_item_init(&ctx->timer_list_head, 0);

    list_init(&ctx->monitors_head);

    ctx->trap_exit = false;
#ifdef ENABLE_ADVANCED_TRACE
    ctx->trace_calls = 0;
    ctx->trace_call_args = 0;
    ctx->trace_returns = 0;
    ctx->trace_send = 0;
    ctx->trace_receive = 0;
#endif

    ctx->flags = NoFlags;
    ctx->platform_data = NULL;

    ctx->group_leader = term_from_local_process_id(INVALID_PROCESS_ID);

    ctx->bs = term_invalid_term();
    ctx->bs_offset = 0;

    ctx->exit_reason = NORMAL_ATOM;

    globalcontext_init_process(glb, ctx);

    return ctx;
}

void context_destroy(Context *ctx)
{
    // Another process can get an access to our mailbox until this point.
    synclist_remove(&ctx->global->processes_table, &ctx->processes_table_head);

    // Ensure process is not registered
    globalcontext_maybe_unregister_process_id(ctx->global, ctx->process_id);

    // When monitor message is sent, process is no longer in the table.
    context_monitors_handle_terminate(ctx);

    // Any other process released our mailbox, so we can clear it.
    mailbox_destroy(&ctx->mailbox, &ctx->heap);

    free(ctx->fr);

    memory_destroy_heap(&ctx->heap, ctx->global);

    dictionary_destroy(&ctx->dictionary);

    if (ctx->timer_list_head.head.next != &ctx->timer_list_head.head) {
        scheduler_cancel_timeout(ctx);
    }

    // Platform data is freed here to allow drivers to use the
    // globalcontext_get_process_lock lock to protect this pointer
    // Typically, another thread or an interrupt would call
    // globalcontext_get_process_lock before accessing platform_data.
    // Here, the context can no longer be acquired with
    // globalcontext_get_process_lock, so it's safe to free the pointer.
    if (ctx->platform_data) {
        free(ctx->platform_data);
    }

    free(ctx);
}

void context_process_kill_signal(Context *ctx, struct TermSignal *signal)
{
    // exit_reason is one of the roots when garbage collecting
    ctx->exit_reason = signal->signal_term;
    context_update_flags(ctx, ~NoFlags, Killed);
}

void context_process_process_info_request_signal(Context *ctx, struct BuiltInAtomRequestSignal *signal)
{
    Context *target = globalcontext_get_process_lock(ctx->global, signal->sender_pid);
    if (target) {
        term ret;
        if (context_get_process_info(ctx, &ret, signal->atom)) {
            mailbox_send_term_signal(target, TrapAnswerSignal, ret);
        } else {
            mailbox_send_built_in_atom_signal(target, TrapExceptionSignal, ret);
        }
        globalcontext_get_process_unlock(ctx->global, target);
    } // else: sender died
}

bool context_process_signal_trap_answer(Context *ctx, struct TermSignal *signal)
{
    context_update_flags(ctx, ~Trap, NoFlags);
    ctx->x[0] = signal->signal_term;
    return true;
}

void context_process_flush_monitor_signal(Context *ctx, uint64_t ref_ticks, bool info)
{
    context_update_flags(ctx, ~Trap, NoFlags);
    bool result = true;
    mailbox_reset(&ctx->mailbox);
    term msg;
    while (mailbox_peek(ctx, &msg)) {
        if (term_is_tuple(msg)
            && term_get_tuple_arity(msg) == 5
            && term_get_tuple_element(msg, 0) == DOWN_ATOM
            && term_is_reference(term_get_tuple_element(msg, 1))
            && term_to_ref_ticks(term_get_tuple_element(msg, 1)) == ref_ticks) {
            mailbox_remove_message(&ctx->mailbox, &ctx->heap);
            // If option info is combined with option flush, false is returned if a flush was needed, otherwise true.
            result = !info;
        } else {
            mailbox_next(&ctx->mailbox);
        }
    }
    mailbox_reset(&ctx->mailbox);
    ctx->x[0] = result ? TRUE_ATOM : FALSE_ATOM;
}

void context_update_flags(Context *ctx, int mask, int value) CLANG_THREAD_SANITIZE_SAFE
{
#ifndef AVM_NO_SMP
    enum ContextFlags expected = ctx->flags;
    enum ContextFlags desired;
    do {
        desired = (expected & mask) | value;
    } while (!ATOMIC_COMPARE_EXCHANGE_WEAK(&ctx->flags, &expected, desired));
#else
    ctx->flags = (ctx->flags & mask) | value;
#endif
}

size_t context_message_queue_len(Context *ctx)
{
    return mailbox_len(&ctx->mailbox);
}

size_t context_size(Context *ctx)
{
    size_t messages_size = mailbox_size(&ctx->mailbox);

    // TODO include ctx->platform_data
    return sizeof(Context)
        + messages_size
        + memory_heap_memory_size(&ctx->heap) * BYTES_PER_TERM;
}

bool context_get_process_info(Context *ctx, term *out, term atom_key)
{
    if (UNLIKELY(memory_ensure_free(ctx, 3) != MEMORY_GC_OK)) {
        *out = OUT_OF_MEMORY_ATOM;
        return false;
    }

    term ret = term_alloc_tuple(2, &ctx->heap);
    switch (atom_key) {
        // heap_size size in words of the heap of the process
        case HEAP_SIZE_ATOM: {
            term_put_tuple_element(ret, 0, HEAP_SIZE_ATOM);
            unsigned long value = memory_heap_memory_size(&ctx->heap) - context_stack_size(ctx);
            term_put_tuple_element(ret, 1, term_from_int32(value));
            break;
        }

        // stack_size stack size, in words, of the process
        case STACK_SIZE_ATOM: {
            term_put_tuple_element(ret, 0, STACK_SIZE_ATOM);
            unsigned long value = context_stack_size(ctx);
            term_put_tuple_element(ret, 1, term_from_int32(value));
            break;
        }

        // message_queue_len number of messages currently in the message queue of the process
        case MESSAGE_QUEUE_LEN_ATOM: {
            term_put_tuple_element(ret, 0, MESSAGE_QUEUE_LEN_ATOM);
            unsigned long value = context_message_queue_len(ctx);
            term_put_tuple_element(ret, 1, term_from_int32(value));
            break;
        }

        // memory size in bytes of the process. This includes call stack, heap, and internal structures.
        case MEMORY_ATOM: {
            term_put_tuple_element(ret, 0, MEMORY_ATOM);
            unsigned long value = context_size(ctx);
            term_put_tuple_element(ret, 1, term_from_int32(value));
            break;
        }

        default:
            *out = BADARG_ATOM;
            return false;
    }
    *out = ret;
    return true;
}

static void context_monitors_handle_terminate(Context *ctx)
{
    struct ListHead *item;
    struct ListHead *tmp;
    MUTABLE_LIST_FOR_EACH (item, tmp, &ctx->monitors_head) {
        struct Monitor *monitor = GET_LIST_ENTRY(item, struct Monitor, monitor_list_head);
        int local_process_id = term_to_local_process_id(monitor->monitor_pid);
        Context *target = globalcontext_get_process_lock(ctx->global, local_process_id);
        if (IS_NULL_PTR(target)) {
            // TODO: we should scan for existing monitors when a context is destroyed
            // otherwise memory might be wasted for long living processes
            free(monitor);
            continue;
        }

        if (monitor->linked && (ctx->exit_reason != NORMAL_ATOM || target->trap_exit)) {
            if (target->trap_exit) {
                if (UNLIKELY(memory_ensure_free(ctx, TUPLE_SIZE(3)) != MEMORY_GC_OK)) {
                    // TODO: handle out of memory here
                    fprintf(stderr, "Cannot handle out of memory.\n");
                    globalcontext_get_process_unlock(ctx->global, target);
                    AVM_ABORT();
                }

                // Prepare the message on ctx's heap which will be freed afterwards.
                term info_tuple = term_alloc_tuple(3, &ctx->heap);
                term_put_tuple_element(info_tuple, 0, EXIT_ATOM);
                term_put_tuple_element(info_tuple, 1, term_from_local_process_id(ctx->process_id));
                term_put_tuple_element(info_tuple, 2, ctx->exit_reason);
                mailbox_send(target, info_tuple);
            } else {
                mailbox_send_term_signal(target, KillSignal, ctx->exit_reason);
            }
        } else if (!monitor->linked) {
            int required_terms = REF_SIZE + TUPLE_SIZE(5);
            if (UNLIKELY(memory_ensure_free(ctx, required_terms) != MEMORY_GC_OK)) {
                // TODO: handle out of memory here
                fprintf(stderr, "Cannot handle out of memory.\n");
                globalcontext_get_process_unlock(ctx->global, target);
                AVM_ABORT();
            }

            // Prepare the message on ctx's heap which will be freed afterwards.
            term ref = term_from_ref_ticks(monitor->ref_ticks, &ctx->heap);

            term info_tuple = term_alloc_tuple(5, &ctx->heap);
            term_put_tuple_element(info_tuple, 0, DOWN_ATOM);
            term_put_tuple_element(info_tuple, 1, ref);
            if (ctx->native_handler != NULL) {
                term_put_tuple_element(info_tuple, 2, PORT_ATOM);
            } else {
                term_put_tuple_element(info_tuple, 2, PROCESS_ATOM);
            }
            term_put_tuple_element(info_tuple, 3, term_from_local_process_id(ctx->process_id));
            term_put_tuple_element(info_tuple, 4, ctx->exit_reason);

            mailbox_send(target, info_tuple);
        }
        globalcontext_get_process_unlock(ctx->global, target);
        free(monitor);
    }
}

uint64_t context_monitor(Context *ctx, term monitor_pid, bool linked)
{
    uint64_t ref_ticks = globalcontext_get_ref_ticks(ctx->global);

    struct Monitor *monitor = malloc(sizeof(struct Monitor));
    if (IS_NULL_PTR(monitor)) {
        return 0;
    }
    monitor->monitor_pid = monitor_pid;
    monitor->ref_ticks = ref_ticks;
    monitor->linked = linked;
    list_append(&ctx->monitors_head, &monitor->monitor_list_head);

    return ref_ticks;
}

void context_demonitor(Context *ctx, term monitor_pid, bool linked)
{
    struct ListHead *item;
    LIST_FOR_EACH (item, &ctx->monitors_head) {
        struct Monitor *monitor = GET_LIST_ENTRY(item, struct Monitor, monitor_list_head);
        if ((monitor->monitor_pid == monitor_pid) && (monitor->linked == linked)) {
            list_remove(&monitor->monitor_list_head);
            free(monitor);
            return;
        }
    }
}
