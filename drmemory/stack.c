/* **********************************************************
 * Copyright (c) 2008-2009 VMware, Inc.  All rights reserved.
 * **********************************************************/

/* Dr. Memory: the memory debugger
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; 
 * version 2.1 of the License, and no later version.

 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/***************************************************************************
 * STACK ADJUSTMENT HANDLING
 */

#include "dr_api.h"
#include "drmemory.h"
#include "readwrite.h"
#include "fastpath.h"
#include "stack.h"
#include "shadow.h"
#include "heap.h"
#include "alloc.h"
#include "alloc_drmem.h"

#ifdef STATISTICS
uint adjust_esp_executions;
uint adjust_esp_fastpath;
uint stack_swaps;
uint stack_swap_triggers;
uint push_addressable;
uint push_addressable_heap;
uint push_addressable_mmap;
#endif

/***************************************************************************
 * STACK SWAP THRESHOLD ADJUSTMENTS
 *
 * If our -stack_swap_threshold is too big or too small we can easily have
 * false positives and/or false negatives so we try to handle unknown
 * stack regions and different sizes of stacks and of stack allocations
 * and deallocations.  Xref PR 525807.
 */

/* number of swap triggers that aren't really swaps before we increase
 * the swap threshold
 */
#define MAX_NUMBER_NON_SWAPS 32

/* we use the stack_swap_threshold for other parts of the code like
 * callstacks and Ki handling so don't let it get too small:
 * though now we use TYPICAL_STACK_MIN_SIZE for Ki and
 * a hardcoded constant for callstacks so making smaller:
 */
#define MIN_SWAP_THRESHOLD 2048

void
check_stack_size_vs_threshold(void *drcontext, size_t stack_size)
{
    /* It's better to have the threshold too small than too big, since
     * over-detecting swaps is much better than under-detecting
     * because we have a nice control point for verifying a swap.
     */
    if (stack_size < options.stack_swap_threshold) {
        /* If the app is near base of stack and swaps not to base of
         * adjacent-higher stack but to near its lowest addr then we
         * could have a quite small delta so go pretty small.
         * check_stack_swap() will bring it back up if there are a
         * lot of large allocs.
         * I originally based this on the stack size but really
         * it only depends on how close adjacent stacks are and how
         * near the end of the stack they get.  Now I just drop
         * to the min and count on check_stack_swap() to bring back up
         * if necessary.
         */
        size_t new_thresh = MIN_SWAP_THRESHOLD;
        if (new_thresh < MIN_SWAP_THRESHOLD)
            new_thresh = MIN_SWAP_THRESHOLD;
        if (new_thresh < options.stack_swap_threshold)
            update_stack_swap_threshold(drcontext, new_thresh);
    }
}

/* Retrieves the bounds for the malloc or mmap region containing addr.
 * If addr is in a small malloc this routine will fail.
 */
static bool
get_stack_region_bounds(byte *addr, byte **base OUT, size_t *size OUT)
{
    if (is_in_heap_region(addr)) {
        return malloc_large_lookup(addr, base, size);
    } else {
#ifdef LINUX
        /* see notes in handle_clone(): OS query not good enough */
        if (mmap_anon_lookup(addr, base, size))
            return true;
#endif
        return dr_query_memory(addr, base, size, NULL);
    }
}

static bool
check_stack_swap(byte *cur_xsp, byte *new_xsp)
{
    /* We check whether this is really a stack swap.  If it is we don't need to
     * do anything.  If it is not we need to handle as an alloc or dealloc to
     * avoid false positives and false negatives.  We also consider increasing
     * the threshold but it's easier to handle when too small than when too
     * large.  Xref PR 525807.
     */
    byte *stack_start;
    size_t stack_size;
    STATS_INC(stack_swap_triggers);
    if (get_stack_region_bounds(cur_xsp, &stack_start, &stack_size)) {
        if (new_xsp >= stack_start && new_xsp < stack_start + stack_size) {
            static int num_non_swaps;
            LOG(1, "stack adjust "PFX" to "PFX" is really intra-stack adjust\n",
                cur_xsp, new_xsp);
            /* Reluctantly increase the threshold linearly: better too small */
            if (num_non_swaps++ > MAX_NUMBER_NON_SWAPS) {
                num_non_swaps = 0;
                update_stack_swap_threshold(dr_get_current_drcontext(),
                                            options.stack_swap_threshold + PAGE_SIZE);
            }
            return false;
        }
    } else
        LOG(1, "WARNING: cannot determine stack bounds for "PFX"\n", cur_xsp);
    LOG(1, "stack swap "PFX" => "PFX"\n", cur_xsp, new_xsp);
    STATS_INC(stack_swaps);
    /* If don't know stack bounds: better to treat as swap, smaller chance
     * of false positives and better to have false negs than tons of pos
     */
    /* FIXME PR 542004: instead of waiting for push of addr memory and
     * handle_push_addressable(), we should mark below new_xsp as unaddr here:
     * but are we sure the app is using this as a stack?  It's possible it's in
     * an optimized loop and it's using xsp as a general-purpose register.
     */
    return true;
}

bool
handle_push_addressable(app_loc_t *loc, app_pc addr, app_pc start_addr,
                        size_t sz, dr_mcontext_t *mc)
{
    /* To detect unknown stacks, and attempt to prevent a too-large
     * stack swap threshold, when we see a push of addressable memory
     * we check whether we should act.  Xref PR 525807.
     * FIXME PR 542004: check on all esp adjusts for
     * addressable memory.
     * Note that a too-large stack swap threshold should usually
     * happen only for swaps between unknown stacks that were
     * allocated together and are similar sizes, so the unknown stack
     * handling's adjustment of the threshold is the only mechanism
     * here.  Swapping from a known stack to a nearby unknown stack of
     * a smaller size is not going to be detected: fortunately it's
     * rare, and we can tell users to use the -stack_swap_threshold
     * for those cases.  Risks include false positives and negatives.
     */
    bool handled = false;
    STATS_INC(push_addressable);
    /* We provide an option to disable if our handling isn't working
     * and we just want to get some performance and don't care about
     * false positives/negatives and have already tuned the stack swap
     * threshold.
     */
    if (options.check_push) {
        byte *stack_start;
        size_t stack_size;
        bool is_heap = false;
        /* we want to do two things:
         * 1) mark beyond-TOS as unaddressable
         * 2) make sure -stack_swap_threshold is small enough: malloc-based
         *    stacks are often small (PR 525807).  our check_stack_swap()
         *    handles a too-small threshold.
         */
        if (is_in_heap_region(addr)) {
            is_heap = true;
            LOG(1, "WARNING: "PFX" is treating heap memory "PFX" as a stack!\n",
                loc_to_print(loc), addr);
        } else {
            LOG(1, "WARNING: "PFX" is treating mmap memory "PFX" as a stack!\n",
                loc_to_print(loc), addr);
        }
        if (get_stack_region_bounds(addr, &stack_start, &stack_size)) {
            LOG(1, "assuming %s "PFX"-"PFX" is a stack\n",
                is_heap ? "large malloc" : "mmap",
                stack_start, stack_start + stack_size);
#ifdef STATISTICS
            if (is_heap)
                STATS_INC(push_addressable_heap);
            else
                STATS_INC(push_addressable_mmap);
#endif
            handled = true;
            /* We don't nec. know the stack bounds since some apps malloc
             * a struct that has some fields and then a stack, so we do one
             * page at a time.  Alternatives include (PR 542004):
             * - have an API where the app tells us its stack bounds: or if
             *   constant could just be a runtime option
             * - stop if hit a defined shadow value before the page size.  can
             *   only do this on 1st time to rule out stale stack values that
             *   can happen if swaps include rollbacks (e.g., swap to base like
             *   DR does w/ dstack, or longjmp from sigaltstack).  thus would
             *   need to remember every stack (and remove from data struct on
             *   dealloc).
             */
            shadow_set_range((addr - PAGE_SIZE < stack_start) ? stack_start :
                             /* stop at start_addr: don't mark what's being
                              * pushed as unaddr!
                              */
                             (addr - PAGE_SIZE), start_addr, SHADOW_UNADDRESSABLE);
            check_stack_size_vs_threshold(dr_get_current_drcontext(), stack_size);
        } else {
            ELOG(0, "ERROR: "PFX" pushing addressable memory: possible Dr. Memory bug\n",
                 loc_to_print(loc));
            if (options.pause_at_unaddressable)
                wait_for_user("pushing addressable memory!");
        }
    }
    return handled;
}

/***************************************************************************/

bool
instr_writes_esp(instr_t *inst)
{
    int i;
    for (i = 0; i < instr_num_dsts(inst); i++) {
        opnd_t opnd = instr_get_dst(inst, i);
        if (opnd_is_reg(opnd) && opnd_uses_reg(opnd, REG_ESP)) {
            /* opnd_uses_reg checks for sub-reg SP */
            return true;
        }
    }
    return false;
}

/* Handle an instruction at pc that writes to esp */
typedef enum {
    ESP_ADJUST_ABSOLUTE,
    ESP_ADJUST_FAST_FIRST = ESP_ADJUST_ABSOLUTE,
    ESP_ADJUST_NEGATIVE,
    ESP_ADJUST_POSITIVE,
    ESP_ADJUST_RET_IMMED, /* positive, but after a pop */
    ESP_ADJUST_FAST_LAST = ESP_ADJUST_RET_IMMED,
    ESP_ADJUST_AND, /* and with a mask */
    ESP_ADJUST_INVALID,
} esp_adjust_t;

/* PR 447537: adjust_esp's shared fast and slow paths */
static byte *shared_esp_slowpath;
static byte *shared_esp_fastpath[2][ESP_ADJUST_FAST_LAST+1];

static esp_adjust_t
get_esp_adjust_type(uint opc)
{
    switch (opc) {
    case OP_mov_st:
    case OP_mov_ld:
    case OP_leave:
    case OP_lea:
    case OP_xchg:
        return ESP_ADJUST_ABSOLUTE;
    case OP_inc:
    case OP_dec:
    case OP_add:
        return ESP_ADJUST_POSITIVE;
    case OP_sub:
        return ESP_ADJUST_NEGATIVE;
    case OP_ret:
        return ESP_ADJUST_RET_IMMED;
    case OP_enter:
        return ESP_ADJUST_NEGATIVE;
    case OP_and:
        return ESP_ADJUST_AND;
    default:
        return ESP_ADJUST_INVALID;
    }
}

/* N.B.: mcontext is not in consistent app state, for efficiency.
 * esp is guaranteed to hold app value, though.
 */
static void
handle_esp_adjust(esp_adjust_t type, reg_t val/*either relative delta, or absolute*/)
{
    ptr_int_t delta = (ptr_int_t) val;
    void *drcontext = dr_get_current_drcontext();
    dr_mcontext_t mc;
    STATS_INC(adjust_esp_executions);
    dr_get_mcontext(drcontext, &mc, NULL);
    if (type == ESP_ADJUST_ABSOLUTE) {
        LOG(3, "esp adjust absolute esp="PFX" => "PFX"\n", mc.esp, val);
        delta = val - mc.esp;
        /* Treat as a stack swap (vs ebp->esp, etc.) if a large change */
        if ((delta > options.stack_swap_threshold ||
             delta < -options.stack_swap_threshold) &&
            check_stack_swap((byte *)mc.xsp, (byte *)val)) {
            /* Stack swap: nothing to do */
            return;
        }
    } else if (type == ESP_ADJUST_AND) {
        ptr_int_t newval = mc.esp & val;
        delta = newval - mc.esp;
        LOG(3, "esp adjust and esp="PFX" delta=%d\n", mc.esp, delta);
        if ((delta > options.stack_swap_threshold ||
             delta < -options.stack_swap_threshold) &&
            check_stack_swap((byte *)mc.xsp, (byte *)newval)) {
            /* Stack swap: nothing to do */
            return;
        }
    } else {
        if (type == ESP_ADJUST_NEGATIVE)
            delta = -delta;
        /* We assume a swap would not happen w/ a relative adjustment */
        if (delta > options.stack_swap_threshold ||
            delta < -options.stack_swap_threshold) {
            LOG(1, "WARNING: relative stack adjustment %d > swap threshold\n",
                delta);
        }
        if (type == ESP_ADJUST_RET_IMMED)
            mc.esp += 4; /* pop of retaddr happens first */
        LOG(3, "esp adjust relative esp="PFX" delta=%d\n", mc.esp, delta);
    }
    if (delta != 0) {
        if (!SHADOW_STACK_POINTER()) {
            if (delta < 0) {
                /* zero out newly allocated stack space to avoid stale
                 * pointers from misleading our leak scan (PR 520916).
                 * yes, I realize it may not be perfectly transparent.
                 */
                memset((app_pc)(mc.esp + delta), 0, -delta);
            }
        } else {
            shadow_set_range((app_pc) (delta > 0 ? mc.esp : (mc.esp + delta)),
                             (app_pc) (delta > 0 ? (mc.esp + delta) : mc.esp),
                             delta > 0 ? SHADOW_UNADDRESSABLE : SHADOW_UNDEFINED);
        }
    }
}

static int
esp_spill_slot_base(void)
{
    /* for whole-bb, we can end up using 1-3 for whole-bb and 4-5 for
     * the required ecx+edx for these shared routines
     * FIXME: opt: we should we xchg w/ whole-bb in
     * instrument_esp_adjust_fastpath() like we do for esp slowpath,
     * and thus make use of a global eax: then should have at most
     * slot 4 used so can always use 5 here
     */
    if (whole_bb_spills_enabled())
        return SPILL_SLOT_6;
    else if (!SHADOW_STACK_POINTER()) {
        /* we don't have shared_esp_fastpath, and instrument slowpath only
         * uses slots 1 and 2
         */
        return SPILL_SLOT_3;
    } else
        return SPILL_SLOT_5;
}

/* N.B.: mcontext is not in consistent app state, for efficiency.
 * esp is guaranteed to hold app value, though.
 */
static void
handle_esp_adjust_shared_slowpath(reg_t val/*either relative delta, or absolute*/)
{
    /* Rather than force gen code to pass another arg we derive the type */
    esp_adjust_t type;
    /* Get the return address from this slowpath call */
    app_pc pc = (app_pc) get_own_tls_value(esp_spill_slot_base());
    instr_t inst;
    void *drcontext = dr_get_current_drcontext();

    /* We decode forward past eflags and register restoration, none of which
     * should reference esp.  The next instr is the app instr.
     */
    instr_init(drcontext, &inst);
    while (true) {
        pc = decode(drcontext, pc, &inst);
        ASSERT(instr_valid(&inst), "unknown suspect instr");
        if (instr_writes_esp(&inst)) {
            /* ret gets mangled: we'll skip the ecx save and hit the pop */
            if (instr_get_opcode(&inst) == OP_pop)
                type = get_esp_adjust_type(OP_ret);
            else {
                type = get_esp_adjust_type(instr_get_opcode(&inst));
                ASSERT(needs_esp_adjust(&inst), "found wrong esp-using instr");
            }
            handle_esp_adjust(type, val);
            break;
        }
        if (instr_is_cti(&inst)) {
            ASSERT(false, "somehow missed app esp-adjust instr");
            break;
        }
        instr_reset(drcontext, &inst);
    }
    instr_free(drcontext, &inst);
    /* paranoid: if didn't find the esp-adjust instr just skip the adjust call */
}

app_pc
generate_shared_esp_slowpath(void *drcontext, instrlist_t *ilist, app_pc pc)
{
    /* PR 447537: adjust_esp's shared_slowpath.
     * On entry:
     *   - ecx holds the val arg
     *   - edx holds the return address
     * Need retaddr in persistent storage: slot5 is guaranteed free.
     */
    PRE(ilist, NULL,
        INSTR_CREATE_mov_st
        (drcontext, spill_slot_opnd(drcontext, esp_spill_slot_base()),
         opnd_create_reg(REG_EDX)));
    dr_insert_clean_call(drcontext, ilist, NULL,
                         (void *) handle_esp_adjust_shared_slowpath, false, 1,
                         opnd_create_reg(REG_ECX));
    PRE(ilist, NULL,
        INSTR_CREATE_jmp_ind(drcontext,
                             spill_slot_opnd(drcontext, esp_spill_slot_base())));

    shared_esp_slowpath = pc;
    pc = instrlist_encode(drcontext, ilist, pc, false);
    instrlist_clear(drcontext, ilist);
    return pc;
}

/* assumes that inst does write to esp */
bool
needs_esp_adjust(instr_t *inst)
{
    /* implicit esp changes (e.g., push and pop) are handled during
     * the read/write: this is for explicit esp changes.
     * -leaks_only doesn't care about push, since it writes, or about pop,
     * since shrinking the stack is ignored there.
     */
    int opc = instr_get_opcode(inst);
    if ((opc_is_push(opc) || opc_is_pop(opc)) &&
        /* handle implicit esp adjustments that are not reads or writes */
        (opc != OP_ret || !opnd_is_immed_int(instr_get_src(inst, 0))) &&
        opc != OP_enter && opc != OP_leave) {
        /* esp changes are all reads or writes */
        return false;
    }
    /* -leaks_only doesn't care about shrinking the stack 
     * technically OP_leave doesn't have to shrink it: we assume it does
     * (just checking leaks: not huge risk)
     */
    if (!SHADOW_STACK_POINTER() &&
        (opc == OP_inc || opc == OP_ret || opc == OP_leave ||
         (opc == OP_add && opnd_is_immed_int(instr_get_src(inst, 0)) &&
          opnd_get_immed_int(instr_get_src(inst, 0)) >= 0) ||
         (opc == OP_sub && opnd_is_immed_int(instr_get_src(inst, 0)) &&
          opnd_get_immed_int(instr_get_src(inst, 0)) <= 0)))
        return false;
    /* We consider sysenter a pop for the hidden ret.  We ignore its write to esp. */
    if (opc == OP_sysenter)
        return false;
    /* We ignore stack changes due to int* */
    if (opc == OP_int || opc == OP_int3 || opc == OP_into)
        return false;
    /* Ignore "or esp,esp" (PR ) */
    if (opc == OP_or && opnd_is_reg(instr_get_src(inst, 0)) &&
        opnd_is_reg(instr_get_dst(inst, 0)) &&
        opnd_get_reg(instr_get_src(inst, 0)) == REG_XSP &&
        opnd_get_reg(instr_get_dst(inst, 0)) == REG_XSP)
        return false;
    return true;
}

/* Instrument an esp modification that is not also a read or write.
 * Returns whether instrumented.
 */
static bool
instrument_esp_adjust_slowpath(void *drcontext, instrlist_t *bb, instr_t *inst,
                               bb_info_t *bi)
{
    /* implicit esp changes (e.g., push and pop) are handled during
     * the read/write: this is for explicit esp changes
     */
    int opc = instr_get_opcode(inst);
    opnd_t arg;
    esp_adjust_t type;
    
    if (!needs_esp_adjust(inst))
        return false;

    /* Call handle_esp_adjust */
    arg = instr_get_src(inst, 0); /* immed is 1st src */
    if (opc == OP_xchg) {
        if (opnd_is_reg(arg) && opnd_get_reg(arg) == REG_ESP) {
            arg = instr_get_src(inst, 1);
        }
    }

    if (!options.shared_slowpath &&
        (opnd_uses_reg(arg, REG_EAX) ||
         opnd_uses_reg(arg, REG_ESP) ||
         opc == OP_lea)) {
        ASSERT(!whole_bb_spills_enabled(), "spill slot conflict");
        /* Put value into tls slot since clean call setup will cause
         * eax and esp to no longer contain app values.
         * If is plain REG_EAX, could pull from pusha slot: but that's fragile.
         * For lea, we can't push the address: we must get it into a register.
         * FIXME: new dr_insert_clean_call() does support eax/esp args, right?
         */
        if (opnd_is_memory_reference(arg)) {
            /* Go through eax to get to tls */
            ASSERT(dr_max_opnd_accessible_spill_slot() >= SPILL_SLOT_1,
                   "DR spill slot not accessible");
            spill_reg(drcontext, bb, inst, REG_EAX, SPILL_SLOT_2);
            if (opc == OP_lea) {
                PRE(bb, inst,
                    INSTR_CREATE_lea(drcontext, opnd_create_reg(REG_EAX), arg));
            } else {
                PRE(bb, inst,
                    INSTR_CREATE_mov_ld(drcontext, opnd_create_reg(REG_EAX), arg));
            }
            spill_reg(drcontext, bb, inst, REG_EAX, SPILL_SLOT_1);
            restore_reg(drcontext, bb, inst, REG_EAX, SPILL_SLOT_2);
        } else {
            ASSERT(opnd_is_reg(arg), "internal error");
            spill_reg(drcontext, bb, inst, opnd_get_reg(arg), SPILL_SLOT_1);
        }
        arg = spill_slot_opnd(drcontext, SPILL_SLOT_1);
    } else if (opc == OP_inc || opc == OP_dec) {
        arg = OPND_CREATE_INT32(opc == OP_inc ? 1 : -1);
    } else if (opc == OP_ret) {
        ASSERT(opnd_is_immed_int(arg), "internal error");
        /* else should have returned up above */
        opnd_set_size(&arg, OPSZ_VARSTACK);
    } else if (opc == OP_enter) {
        /* frame pushes (including nested) are handled elsewhere as writes */
        ASSERT(opnd_is_immed_int(arg), "internal error");
    } else if (opc == OP_leave) {
        /* the pop is handled elsewhere as a write */
        arg = opnd_create_reg(REG_EBP);
    }

    type = get_esp_adjust_type(opc);
    if (type == ESP_ADJUST_INVALID) {
        per_thread_t *pt = (per_thread_t *) dr_get_tls_field(drcontext);
        ELOGPT(0, pt, "ERROR: new stack-adjusting instr: ");
        instr_disassemble(drcontext, inst, pt->f);
        ELOGPT(0, pt, "\n");
        ASSERT(false, "unhandled stack adjustment");
    }

    if (options.shared_slowpath) {
        instr_t *retaddr = INSTR_CREATE_label(drcontext);
        scratch_reg_info_t si1 = {REG_ECX, true, false, false, REG_NULL, SPILL_SLOT_1};
        scratch_reg_info_t si2 = {REG_EDX, true, false, false, REG_NULL, SPILL_SLOT_2};
        reg_id_t arg_tgt;
        if (opnd_is_immed_int(arg))
            opnd_set_size(&arg, OPSZ_PTR);
        if (bi->reg1.reg != REG_NULL) {
            /* use global scratch regs 
             * FIXME: opt: generalize and use for fastpath too: but more complex
             * there since have 3 scratches and any one could be the extra local.
             */
            if (bi->reg1.reg == REG_ECX || bi->reg2.reg == REG_ECX)
                si1.dead = true;
            else
                si1.xchg = (bi->reg1.reg == REG_EDX) ? bi->reg2.reg : bi->reg1.reg;
            if (bi->reg1.reg == REG_EDX || bi->reg2.reg == REG_EDX)
                si2.dead = true;
            else {
                si2.xchg = (bi->reg1.reg == REG_ECX) ? bi->reg2.reg :
                    ((si1.xchg == bi->reg1.reg) ? bi->reg2.reg : bi->reg1.reg);
            }
            /* restore from spill slot prior to setting up arg */
            if (opnd_uses_reg(arg, bi->reg1.reg)) {
                insert_spill_global(drcontext, bb, inst, &bi->reg1, false/*restore*/);
            } else if (opnd_uses_reg(arg, bi->reg2.reg)) {
                insert_spill_global(drcontext, bb, inst, &bi->reg2, false/*restore*/);
            }
            /* mark as used after the restore to avoid superfluous restore */
            mark_scratch_reg_used(drcontext, bb, bi, &bi->reg1);
            mark_scratch_reg_used(drcontext, bb, bi, &bi->reg2);
        } else {
            /* we assume regs are all holding app state and we can use arg directly */
        }
        /* if saving ecx via xchg we must do xchg after, else mess up app values */
        if (si1.xchg != REG_NULL)
            arg_tgt = si1.xchg;
        else {
            arg_tgt = REG_ECX;
            insert_spill_or_restore(drcontext, bb, inst, &si1, true/*save*/, false);
        }
        if (opnd_is_memory_reference(arg)) {
            if (opc == OP_lea) {
                PRE(bb, inst, INSTR_CREATE_lea(drcontext, opnd_create_reg(arg_tgt), arg));
            } else {
                PRE(bb, inst, INSTR_CREATE_mov_ld(drcontext, opnd_create_reg(arg_tgt),
                                                  arg));
            }
        } else
            PRE(bb, inst, INSTR_CREATE_mov_st(drcontext, opnd_create_reg(arg_tgt), arg));
        if (si1.xchg != REG_NULL) {
            /* now put arg into ecx, and saved ecx into dead xchg-w/ reg */
            insert_spill_or_restore(drcontext, bb, inst, &si1, true/*save*/, false);
        }
        /* spill/xchg edx after, since if xchg can mess up arg's app values */
        insert_spill_or_restore(drcontext, bb, inst, &si2, true/*save*/, false);
        /* we don't need to negate here since handle_adjust_esp() does that */
        PRE(bb, inst,
            INSTR_CREATE_mov_st(drcontext, opnd_create_reg(REG_EDX),
                                opnd_create_instr(retaddr)));
        PRE(bb, inst,
            INSTR_CREATE_jmp(drcontext, opnd_create_pc(shared_esp_slowpath)));
        PRE(bb, inst, retaddr);
        insert_spill_or_restore(drcontext, bb, inst, &si2, false/*restore*/, false);
        insert_spill_or_restore(drcontext, bb, inst, &si1, false/*restore*/, false);
    } else {
        dr_insert_clean_call(drcontext, bb, inst, (void *) handle_esp_adjust, false, 2, 
                             OPND_CREATE_INT32(type), arg);
    }
    return true;
}

/* Instrument an esp modification that is not also a read or write
 * Returns whether instrumented
 */
static bool
instrument_esp_adjust_fastpath(void *drcontext, instrlist_t *bb, instr_t *inst,
                               bb_info_t *bi)
{
    /* implicit esp changes (e.g., push and pop) are handled during
     * the read/write: this is for explicit esp changes
     */
    int opc = instr_get_opcode(inst);
    opnd_t arg;
    instr_t *retaddr;
    fastpath_info_t mi;
    bool negate = false, absolute = false;
    bool eflags_live;
    esp_adjust_t type = get_esp_adjust_type(opc);
    reg_id_t reg_mod;
    
    if (!needs_esp_adjust(inst))
        return false;

    arg = instr_get_src(inst, 0); /* 1st src for nearly all cases */

    if (opc == OP_ret) {
        ASSERT(opnd_is_immed_int(arg), "internal error");
        /* else should have returned up above */
    } else if (opc == OP_inc) {
        arg = OPND_CREATE_INT32(1);
    } else if (opc == OP_dec) {
        arg = OPND_CREATE_INT32(-1);
    } else if (opc == OP_add) {
        /* all set */
    } else if (opc == OP_sub) {
        negate = true;
    } else if (opc == OP_enter) {
        negate = true;
    } else if (opc == OP_mov_st || opc == OP_mov_ld ||
               opc == OP_leave || opc == OP_lea) {
        absolute = true;
    } else if (opc == OP_xchg) {
        absolute = true;
        if (opnd_is_reg(arg) && opnd_uses_reg(arg, REG_ESP))
            arg = instr_get_src(inst, 1);
    } else {
        return instrument_esp_adjust_slowpath(drcontext, bb, inst, bi);
    }

    memset(&mi, 0, sizeof(mi));
    mi.bb = bi;

    /* set up regs and spill info */
    if (!SHADOW_STACK_POINTER()) {
        pick_scratch_regs(inst, &mi, false/*anything*/, false/*2 args only*/,
                          false/*3rd must be ecx*/, arg, opnd_create_null());
        reg_mod = mi.reg2.reg;
        mark_scratch_reg_used(drcontext, bb, bi, &mi.reg2);
        insert_spill_or_restore(drcontext, bb, inst, &mi.reg2, true/*save*/, false);
    } else {
        /* we can't have ecx using SPILL_SLOT_EFLAGS_EAX since shared fastpath
         * will use it, so we communicate that via mi.eax.
         * for whole_bb_spills_enabled() we also have to rule out eax, since
         * shared fastpath assumes edx, ebx, and ecx are the scratch regs.
         * FIXME: opt: we should we xchg w/ whole-bb like we do for esp slowpath:
         * then allow eax and xchg w/ it.  Must be careful about spill
         * ordering w/ arg retrieval if arg uses regs.
         */
        mi.eax.used = true;
        mi.eax.dead = false;
        pick_scratch_regs(inst, &mi, true/*must be abcd*/, true/*need 3rd reg*/,
                          true/*3rd must be ecx*/, arg,
                          opnd_create_reg(REG_EAX)/*no eax*/);
        reg_mod = mi.reg3.reg;
        ASSERT(mi.reg3.reg == REG_ECX, "shared_esp_fastpath reg error");
        ASSERT((mi.reg2.reg == REG_EBX && mi.reg1.reg == REG_EDX) ||
               (mi.reg2.reg == REG_EDX && mi.reg1.reg == REG_EBX),
               "shared_esp_fastpath reg error");
        mark_scratch_reg_used(drcontext, bb, bi, &mi.reg3);
        insert_spill_or_restore(drcontext, bb, inst, &mi.reg3, true/*save*/, false);
    }
    eflags_live = (!whole_bb_spills_enabled() && mi.aflags != EFLAGS_WRITE_6);
    if (SHADOW_STACK_POINTER()) {
        ASSERT(!eflags_live || mi.reg3.slot != SPILL_SLOT_EFLAGS_EAX,
               "shared_esp_fastpath slot error");
    }
    /* for whole-bb we can't use the SPILL_SLOT_EFLAGS_EAX */
    ASSERT(!whole_bb_spills_enabled() || !eflags_live, "eflags spill conflict");

    retaddr = INSTR_CREATE_label(drcontext);

    if (whole_bb_spills_enabled() && !opnd_is_immed_int(arg)) {
        /* restore from spill slot so we read app values for arg */
        if (opnd_uses_reg(arg, bi->reg1.reg)) {
            insert_spill_global(drcontext, bb, inst, &bi->reg1, false/*restore*/);
        } else if (opnd_uses_reg(arg, bi->reg2.reg)) {
            insert_spill_global(drcontext, bb, inst, &bi->reg2, false/*restore*/);
        }
    }

    mark_scratch_reg_used(drcontext, bb, bi, &mi.reg1);
    if (SHADOW_STACK_POINTER())
        mark_scratch_reg_used(drcontext, bb, bi, &mi.reg2);

    /* get arg first in case it uses another reg we're going to clobber */
    if (opc == OP_lea) {
        PRE(bb, inst, INSTR_CREATE_lea(drcontext, opnd_create_reg(reg_mod), arg));
        ASSERT(!negate, "esp adjust OP_lea error");
        ASSERT(type == ESP_ADJUST_ABSOLUTE, "esp adjust OP_lea error");
    } else if (opnd_is_immed_int(arg)) {
        if (negate) {
            /* PR 416446: can't use opnd_get_size(arg) since max negative is
             * too big for max positive.  We're enlarging to OPSZ_4 later anyway.
             */
            arg = opnd_create_immed_int(-opnd_get_immed_int(arg), OPSZ_4);
        }
        /* OP_ret has OPSZ_2 immed, and OP_add, etc. often have OPSZ_1 */
        opnd_set_size(&arg, OPSZ_4);
        PRE(bb, inst, INSTR_CREATE_mov_imm(drcontext, opnd_create_reg(reg_mod), arg));
    } else {
        PRE(bb, inst, INSTR_CREATE_mov_ld(drcontext, opnd_create_reg(reg_mod), arg));
        if (negate)
            PRE(bb, inst, INSTR_CREATE_neg(drcontext, opnd_create_reg(reg_mod)));
    }

    insert_spill_or_restore(drcontext, bb, inst, &mi.reg1, true/*save*/, false);
    if (!SHADOW_STACK_POINTER()) {
        instr_t *loop_repeat = INSTR_CREATE_label(drcontext);
        /* since we statically know we don't need slowpath (even if unaligned:
         * ok to write unaligned dwords via mov_st) and we only go in one
         * direction and don't need address translation, the loop is small
         * enough to inline
         */
        if (whole_bb_spills_enabled())
            mark_eflags_used(drcontext, bb, bi);
        else if (eflags_live)
            insert_save_aflags(drcontext, bb, inst, &mi.eax, mi.aflags);
        PRE(bb, inst,
            INSTR_CREATE_mov_ld(drcontext, opnd_create_reg(mi.reg1.reg),
                                opnd_create_reg(REG_XSP)));
        ASSERT(type != ESP_ADJUST_RET_IMMED, "ret ignored for -leaks_only");
        if (type != ESP_ADJUST_ABSOLUTE) {
            /* calculate the end of the loop */
            PRE(bb, inst,
                INSTR_CREATE_add(drcontext, opnd_create_reg(reg_mod),
                                 opnd_create_reg(mi.reg1.reg)));
        }
        /* only zero if allocating stack, not when deallocating */
        PRE(bb, inst,
            INSTR_CREATE_cmp(drcontext, opnd_create_reg(reg_mod),
                             opnd_create_reg(REG_XSP)));
        PRE(bb, inst,
            INSTR_CREATE_jcc(drcontext, OP_jge_short, opnd_create_instr(retaddr)));
        /* now we know we're decreasing stack addresses, so start zeroing
         * my intution says impact on scratch regs (#, flexibility) of rep stos
         * makes this loop preferable even if slightly bigger instru
         */
        PRE(bb, inst, loop_repeat);
        PRE(bb, inst,
            INSTR_CREATE_sub(drcontext, opnd_create_reg(mi.reg1.reg),
                             OPND_CREATE_INT8(4)));
        PRE(bb, inst,
            INSTR_CREATE_cmp(drcontext, opnd_create_reg(mi.reg1.reg),
                             opnd_create_reg(reg_mod)));
        PRE(bb, inst,
            INSTR_CREATE_jcc(drcontext, OP_jl_short, opnd_create_instr(retaddr)));
        PRE(bb, inst,
            INSTR_CREATE_mov_st(drcontext, OPND_CREATE_MEM32(mi.reg1.reg, 0),
                                OPND_CREATE_INT32(0)));
        PRE(bb, inst,
            INSTR_CREATE_jmp_short(drcontext, opnd_create_instr(loop_repeat)));
        PRE(bb, inst, retaddr);
        if (eflags_live)
            insert_restore_aflags(drcontext, bb, inst, &mi.eax, mi.aflags);
    } else {
        /* should we trade speed for space and move this spill/restore into
         * shared_fastpath? then need to nail down which of reg2 vs reg1 is which.
         */
        insert_spill_or_restore(drcontext, bb, inst, &mi.reg2, true/*save*/, false);
        
        PRE(bb, inst,
            INSTR_CREATE_mov_st(drcontext, opnd_create_reg(REG_EDX),
                                opnd_create_instr(retaddr)));
        ASSERT(type >= ESP_ADJUST_FAST_FIRST &&
               type <= ESP_ADJUST_FAST_LAST, "invalid type for esp fastpath");
        PRE(bb, inst,
            INSTR_CREATE_jmp(drcontext,
                             opnd_create_pc(shared_esp_fastpath
                                            /* don't trust true always being 1 */
                                            [eflags_live ? 1 : 0]
                                            [type])));
        PRE(bb, inst, retaddr);
    }

    insert_spill_or_restore(drcontext, bb, inst, &mi.reg3, false/*restore*/, false);
    insert_spill_or_restore(drcontext, bb, inst, &mi.reg2, false/*restore*/, false);
    insert_spill_or_restore(drcontext, bb, inst, &mi.reg1, false/*restore*/, false);
    return true;
}

static void
generate_shared_esp_fastpath_helper(void *drcontext, instrlist_t *bb,
                                    bool eflags_live, esp_adjust_t type)
{
    fastpath_info_t mi;
    instr_t *loop_pop_repeat, *loop_push, *loop_push_repeat, *loop_done, *restore;
    instr_t *loop_next_shadow, *loop_shadow_lookup, *shadow_lookup;

    loop_pop_repeat = INSTR_CREATE_label(drcontext);
    loop_push = INSTR_CREATE_label(drcontext);
    loop_push_repeat = INSTR_CREATE_label(drcontext);
    loop_done = INSTR_CREATE_label(drcontext);
    loop_next_shadow = INSTR_CREATE_label(drcontext);
    loop_shadow_lookup = INSTR_CREATE_label(drcontext);
    shadow_lookup = INSTR_CREATE_label(drcontext);
    restore = INSTR_CREATE_label(drcontext);

    memset(&mi, 0, sizeof(mi));
    mi.slowpath = INSTR_CREATE_label(drcontext);
    /* we do not optimize for OF */
    mi.aflags = (!eflags_live ? 0 : EFLAGS_WRITE_6);
    mi.eax.reg = REG_EAX;
    mi.eax.used = true;
    mi.eax.dead = false;
    mi.eax.xchg = REG_NULL;
    /* for whole-bb we shouldn't end up using this spill slot */
    mi.eax.slot = SPILL_SLOT_EFLAGS_EAX;
    mi.reg1.reg = REG_EDX;
    mi.reg2.reg = REG_EBX;
    mi.reg3.reg = REG_ECX;

    /* save the 2 args for retrieval at end */
    PRE(bb, NULL,
        INSTR_CREATE_mov_st
        (drcontext, spill_slot_opnd(drcontext, esp_spill_slot_base()+1), opnd_create_reg(REG_ECX)));
    PRE(bb, NULL,
        INSTR_CREATE_mov_st
        (drcontext, spill_slot_opnd(drcontext, esp_spill_slot_base()), opnd_create_reg(REG_EDX)));

    if (eflags_live)
        insert_save_aflags(drcontext, bb, NULL, &mi.eax, mi.aflags);

    /* the initial address to look up in the shadow table is cur esp */
    PRE(bb, NULL,
        INSTR_CREATE_mov_ld(drcontext, opnd_create_reg(mi.reg1.reg),
                            opnd_create_reg(REG_ESP)));
    if (type == ESP_ADJUST_RET_IMMED) {
        /* pop of retaddr happens first (handled in definedness routines) */
        PRE(bb, NULL,
            INSTR_CREATE_add(drcontext, opnd_create_reg(mi.reg1.reg), OPND_CREATE_INT8(4)));
    }

    /* for absolute, calculate the delta */
    if (type == ESP_ADJUST_ABSOLUTE) {
        PRE(bb, NULL,
            INSTR_CREATE_sub(drcontext, opnd_create_reg(mi.reg3.reg),
                             opnd_create_reg(mi.reg1.reg)));
        /* Treat as a stack swap if a large change.
         * We assume a swap would not happen w/ a relative adjustment.
         */
        PRE(bb, NULL,
            INSTR_CREATE_cmp(drcontext, opnd_create_reg(mi.reg3.reg),
                             OPND_CREATE_INT32(options.stack_swap_threshold)));
        /* We need to verify whether it's a real swap */
        add_jcc_slowpath(drcontext, bb, NULL, OP_jg/*short doesn't reach*/, &mi);
        PRE(bb, NULL,
            INSTR_CREATE_cmp(drcontext, opnd_create_reg(mi.reg3.reg),
                             OPND_CREATE_INT32(-options.stack_swap_threshold)));
        /* We need to verify whether it's a real swap */
        add_jcc_slowpath(drcontext, bb, NULL, OP_jl_short, &mi);
    }

    /* Ensure the size is 4-aligned so our loop works out */
    PRE(bb, NULL,
        INSTR_CREATE_test(drcontext, opnd_create_reg(mi.reg3.reg),
                          OPND_CREATE_INT32(0x3)));
    add_jcc_slowpath(drcontext, bb, NULL, OP_jnz_short, &mi);

    PRE(bb, NULL, loop_shadow_lookup);
    /* To support crossing 64K blocks we must decrement xsp prior to translating
     * instead of decrementing the translation
     */
    PRE(bb, NULL,
        INSTR_CREATE_cmp(drcontext, opnd_create_reg(mi.reg3.reg), OPND_CREATE_INT32(0)));
    PRE(bb, NULL,
        INSTR_CREATE_jcc(drcontext, OP_jg_short, opnd_create_instr(shadow_lookup)));
    PRE(bb, NULL,
        INSTR_CREATE_jcc(drcontext, OP_je_short, opnd_create_instr(loop_done)));
    PRE(bb, NULL,
        INSTR_CREATE_sub(drcontext, opnd_create_reg(mi.reg1.reg), OPND_CREATE_INT8(4)));
    PRE(bb, NULL, shadow_lookup);
    mi.memsz = 4;
    add_shadow_table_lookup(drcontext, bb, NULL, &mi, false/*need addr*/,
                            false, false/*bail if not aligned*/, false,
                            mi.reg1.reg, mi.reg2.reg, mi.reg3.reg);
    /* now addr of shadow byte is in reg1 and offs within shadow block is in reg2 */

    /* we need separate loops for inc vs dec */
    PRE(bb, NULL,
        INSTR_CREATE_cmp(drcontext, opnd_create_reg(mi.reg3.reg), OPND_CREATE_INT32(0)));
    PRE(bb, NULL,
        INSTR_CREATE_jcc(drcontext, OP_jl_short, opnd_create_instr(loop_push)));
    /* we tested equality above */

    /* reg1 has address of shadow table for cur esp, and address is aligned to 4.
     * now compute the new esp, and then mark in between as unaddressable/undefined.
     * one shadow byte == 4 stack bytes at a time.
     * verify still within same 64K-covering shadow block, else bail.
     */

    /******* increasing loop *******/
    /* calculate end of shadow block */
    PRE(bb, NULL, INSTR_CREATE_neg(drcontext, opnd_create_reg(mi.reg2.reg)));
    PRE(bb, NULL, INSTR_CREATE_add(drcontext, opnd_create_reg(mi.reg2.reg),
                                   opnd_create_reg(mi.reg1.reg)));
    PRE(bb, NULL, INSTR_CREATE_add(drcontext, opnd_create_reg(mi.reg2.reg),
                                   OPND_CREATE_INT32(get_shadow_block_size())));
    /* loop for increasing stack addresses = pop */
    PRE(bb, NULL, loop_pop_repeat);
    PRE(bb, NULL,
        INSTR_CREATE_mov_st(drcontext, OPND_CREATE_MEM8(mi.reg1.reg, 0),
                            OPND_CREATE_INT8((char)SHADOW_DWORD_UNADDRESSABLE)));
    PRE(bb, NULL, INSTR_CREATE_inc(drcontext, opnd_create_reg(mi.reg1.reg)));
    PRE(bb, NULL,
        INSTR_CREATE_sub(drcontext, opnd_create_reg(mi.reg3.reg), OPND_CREATE_INT8(4)));
    /* cmp to 0 via smaller instr test-with-self */
    PRE(bb, NULL,
        INSTR_CREATE_test(drcontext, opnd_create_reg(mi.reg3.reg),
                          opnd_create_reg(mi.reg3.reg)));
    PRE(bb, NULL,
        INSTR_CREATE_jcc(drcontext, OP_je_short, opnd_create_instr(loop_done)));
    /* check for end of shadow block after decrementing count and checking for done */
    PRE(bb, NULL,
        INSTR_CREATE_cmp(drcontext, opnd_create_reg(mi.reg1.reg),
                         opnd_create_reg(mi.reg2.reg)));
    PRE(bb, NULL,
        INSTR_CREATE_jcc(drcontext, OP_jge_short, opnd_create_instr(loop_next_shadow)));
    PRE(bb, NULL, INSTR_CREATE_jmp_short(drcontext, opnd_create_instr(loop_pop_repeat)));


    /******* shadow block boundary handler, shared by both loops *******/
    PRE(bb, NULL, loop_next_shadow);
    /* PR 503778: handle moving off the end of this shadow block
     * hit end => loop back to shadow lookup (size still aligned).  first:
     * - put esp in reg1 and then add (stored count - remaining count), w/o
     *   touching reg3 which will still hold remaining count
     * Note that if new shadow lookup fails we'll re-do the already-completed
     * loop iters in the slowpath.
     */
    /* the initial address to look up in the shadow table is cur esp */
    PRE(bb, NULL,
        INSTR_CREATE_mov_ld(drcontext, opnd_create_reg(mi.reg1.reg),
                            opnd_create_reg(REG_ESP)));
    if (type == ESP_ADJUST_RET_IMMED) {
        /* pop of retaddr happens first (handled in definedness routines) */
        PRE(bb, NULL,
            INSTR_CREATE_add(drcontext, opnd_create_reg(mi.reg1.reg),
                             OPND_CREATE_INT8(4)));
    }
    PRE(bb, NULL,
        INSTR_CREATE_add
        (drcontext, opnd_create_reg(mi.reg1.reg),
         spill_slot_opnd(drcontext, esp_spill_slot_base()+1)));
    PRE(bb, NULL,
        INSTR_CREATE_sub(drcontext, opnd_create_reg(mi.reg1.reg),
                         opnd_create_reg(mi.reg3.reg)));
    PRE(bb, NULL, INSTR_CREATE_jmp_short(drcontext,
                                         opnd_create_instr(loop_shadow_lookup)));


    /******* decreasing loop *******/
    PRE(bb, NULL, loop_push);
    /* calculate start of shadow block */
    PRE(bb, NULL, INSTR_CREATE_neg(drcontext, opnd_create_reg(mi.reg2.reg)));
    PRE(bb, NULL, INSTR_CREATE_add(drcontext, opnd_create_reg(mi.reg2.reg),
                                   opnd_create_reg(mi.reg1.reg)));
    /* loop for decreasing stack addresses = push */
    PRE(bb, NULL, loop_push_repeat);
    /* we decremented xsp pre-xl8 so store before dec */
    PRE(bb, NULL,
        INSTR_CREATE_mov_st(drcontext, OPND_CREATE_MEM8(mi.reg1.reg, 0),
                            OPND_CREATE_INT8((char)SHADOW_DWORD_UNDEFINED)));
    PRE(bb, NULL, INSTR_CREATE_dec(drcontext, opnd_create_reg(mi.reg1.reg)));
    PRE(bb, NULL,
        INSTR_CREATE_add(drcontext, opnd_create_reg(mi.reg3.reg), OPND_CREATE_INT8(4)));
    /* cmp to 0 via smaller instr test-with-self */
    PRE(bb, NULL,
        INSTR_CREATE_test(drcontext, opnd_create_reg(mi.reg3.reg),
                          opnd_create_reg(mi.reg3.reg)));
    PRE(bb, NULL,
        INSTR_CREATE_jcc(drcontext, OP_je_short, opnd_create_instr(loop_done)));
    /* Ensure we haven't gone off the start of this shadow block */
    PRE(bb, NULL,
        INSTR_CREATE_cmp(drcontext, opnd_create_reg(mi.reg1.reg),
                         opnd_create_reg(mi.reg2.reg)));
    PRE(bb, NULL,
        INSTR_CREATE_jcc(drcontext, OP_jl_short, opnd_create_instr(loop_next_shadow)));
    PRE(bb, NULL,
        INSTR_CREATE_jmp_short(drcontext, opnd_create_instr(loop_push_repeat)));

    PRE(bb, NULL, loop_done);
#ifdef STATISTICS
    if (options.statistics) {
        PRE(bb, NULL,
            INSTR_CREATE_inc(drcontext,
                             OPND_CREATE_MEM32(REG_NULL, (int)&adjust_esp_fastpath)));
    }
#endif
    PRE(bb, NULL, INSTR_CREATE_jmp_short(drcontext, opnd_create_instr(restore)));

    PRE(bb, NULL, mi.slowpath);
    if (options.shared_slowpath) {
        /* note that handle_special_shadow_fault() assumes the first restore
         * from tls after a faulting store is the first instr of the slowpath
         */
        /* note that we aren't restoring regs saved at call site.
         * we only need app esp value in slowpath callee so it works out.
         * FIXME: are we ever crashing as app might, when referencing our val arg?
         * then need to go back to caller, restore, then to slowpath?
         */
        PRE(bb, NULL,
            INSTR_CREATE_mov_ld
            (drcontext, opnd_create_reg(REG_ECX),
             spill_slot_opnd(drcontext, esp_spill_slot_base()+1)));
        /* we use tailcall to avoid two indirect jumps, at cost of extra eflags
         * restore: shared_slowpath will ret to our caller 
         */
        PRE(bb, NULL,
            INSTR_CREATE_mov_ld
            (drcontext, opnd_create_reg(REG_EDX),
             spill_slot_opnd(drcontext, esp_spill_slot_base())));
        if (type == ESP_ADJUST_NEGATIVE) {
            /* slowpath does its own negation */
            PRE(bb, NULL, INSTR_CREATE_neg(drcontext, opnd_create_reg(REG_ECX)));
        }
        /* since not returning here, must restore flags */
        if (eflags_live)
            insert_restore_aflags(drcontext, bb, NULL, &mi.eax, mi.aflags);
        PRE(bb, NULL,
            INSTR_CREATE_jmp(drcontext, opnd_create_pc(shared_esp_slowpath)));
    } else {
        dr_insert_clean_call(drcontext, bb, NULL,
                             (void *) handle_esp_adjust_shared_slowpath, false, 1,
                             spill_slot_opnd(drcontext, esp_spill_slot_base()+1));
    }

    PRE(bb, NULL, restore);
    if (eflags_live)
        insert_restore_aflags(drcontext, bb, NULL, &mi.eax, mi.aflags);
    PRE(bb, NULL,
        INSTR_CREATE_jmp_ind(drcontext,
                             spill_slot_opnd(drcontext, esp_spill_slot_base())));
}

app_pc
generate_shared_esp_fastpath(void *drcontext, instrlist_t *ilist, app_pc pc)
{
    /* PR 447537: adjust_esp's shared fastpath
     * On entry:
     *   - ecx holds the val arg
     *   - edx holds the return address
     * Uses slot5 and slot6.
     * We have multiple versions for {eflags,adjust-type}.
     */
    int eflags_live;
    esp_adjust_t type;
    ASSERT(ESP_ADJUST_FAST_FIRST == 0, "esp enum error");
    for (eflags_live = 0; eflags_live < 2; eflags_live++) {
        for (type = ESP_ADJUST_FAST_FIRST; type <= ESP_ADJUST_FAST_LAST; type++) {
            shared_esp_fastpath[eflags_live][type] = pc;
            generate_shared_esp_fastpath_helper(drcontext, ilist, eflags_live, type);
            pc = instrlist_encode(drcontext, ilist, pc, true);
            instrlist_clear(drcontext, ilist);
        }
    }
    return pc;
}

/* Caller has made the memory writable and holds a lock */
void
esp_fastpath_update_swap_threshold(void *drcontext, int new_threshold)
{
    int eflags_live;
    byte *pc, *end_pc;
    instr_t inst;
    instr_init(drcontext, &inst);
    for (eflags_live = 0; eflags_live < 2; eflags_live++) {
        /* only ESP_ADJUST_ABSOLUTE checks for a stack swap: swaps aren't relative */
        int found = 0;
        pc = shared_esp_fastpath[eflags_live][ESP_ADJUST_ABSOLUTE];
        end_pc = (ESP_ADJUST_ABSOLUTE == ESP_ADJUST_FAST_LAST ?
                  (eflags_live == 1 ? ((byte *)ALIGN_FORWARD(pc, PAGE_SIZE)) :
                   shared_esp_fastpath[eflags_live+1][0]) :
                  shared_esp_fastpath[eflags_live][ESP_ADJUST_ABSOLUTE+1]);
        LOG(3, "updating swap threshold in gencode "PFX"-"PFX"\n", pc, end_pc);
        do {
            pc = decode(drcontext, pc, &inst);
            if (instr_get_opcode(&inst) == OP_cmp &&
                opnd_is_reg(instr_get_src(&inst, 0)) &&
                opnd_is_immed_int(instr_get_src(&inst, 1))) {
                ptr_int_t immed = opnd_get_immed_int(instr_get_src(&inst, 1));
                LOG(3, "found cmp ending @"PFX" immed="PIFX"\n", pc, immed);
                if (immed == options.stack_swap_threshold) {
                    /* could replace through IR and re-encode but want to
                     * check cache line
                     */
                    if (CROSSES_ALIGNMENT(pc-4, 4, proc_get_cache_line_size())) {
                        /* not that worried: not worth suspend-world */
                        LOG(1, "WARNING: updating gencode across cache line!\n");
                    }
                    /* immed is always last */
                    ASSERT(*(int*)(pc-4) == options.stack_swap_threshold, "imm last?");
                    *(int*)(pc-4) = new_threshold;
                    found++;
                } else if (immed == -options.stack_swap_threshold) {
                    if (CROSSES_ALIGNMENT(pc-4, 4, proc_get_cache_line_size())) {
                        /* not that worried: not worth suspend-world */
                        LOG(1, "WARNING: updating gencode across cache line!\n");
                    }
                    ASSERT(*(int*)(pc-4) == -options.stack_swap_threshold, "imm last?");
                    *(int*)(pc-4) = -new_threshold;
                    found++;
                }
            }
            instr_reset(drcontext, &inst);
            if (found >= 2)
                break;
        } while (pc < end_pc);
        ASSERT(found == 2, "cannot find both threshold cmps in esp fastpath!");
    }
    instr_free(drcontext, &inst);
}

/* Instrument an esp modification that is not also a read or write
 * Returns whether instrumented
 */
bool
instrument_esp_adjust(void *drcontext, instrlist_t *bb, instr_t *inst, bb_info_t *bi)
{
    if (options.esp_fastpath)
        return instrument_esp_adjust_fastpath(drcontext, bb, inst, bi);
    else
        return instrument_esp_adjust_slowpath(drcontext, bb, inst, bi);
}

