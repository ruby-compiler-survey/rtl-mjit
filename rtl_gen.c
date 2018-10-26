/**********************************************************************

  rtl_gen.c - Code for Generation RTL from stack insns.

  Copyright (C) 2017, 2018 Vladimir Makarov vmakarov@redhat.com

**********************************************************************/

/* To generate RTL insns we passes stack insns several times.  First
   we calculate possible stack values on the label.  It is a forward
   data flow problem (the final fixed point is only temporaries on the
   emulated stack).  Then using this info we actually generate RTL
   insns on the 2nd pass.

   We emulate VM stack to generate RTL insns for another RTL insn
   operands in a lazy way.  Therefore the order of RTL insns for
   calculating some simple operands can be different from
   corresponding stack insns.  */

#include "internal.h"
#include "encindex.h"
#include <math.h>

/* Use nonzero to print debug info abou the generator.  */
#define RTL_GEN_DEBUG 1

#include "vm_core.h"
#include "iseq.h"

#include "insns.inc"
#include "insns_info.inc"
#include "gc.h"

#ifdef RTL_GEN_DEBUG
static int rtl_gen_debug_p = FALSE;
#endif

/* Long jump buffer used to finish the generator work in case of a
   failure.  */
static rb_jmpbuf_t rtl_gen_jump_buf;

/* Type used for label relative displacement during RTL
   generation.  */
typedef long REL_PC;

/*---------- Variable length arrays (VARR) --------------------------*/
/*------- This implementaion supports static type checking. -------- */

/* Asserts for VARR:  */
#if ENABLE_CHECKING
#define VARR_ASSERT_FAIL(OP,VARR) varr_assert_fail(OP,#VARR)

static inline void varr_assert_fail(const char *op, const char *var) {
    fprintf(stderr, "wrong %s for %s", op, var);
    assert(0);
}

#define VARR_ASSERT(EXPR,OP,T)					\
  (void)((EXPR) ? 0 : (VARR_ASSERT_FAIL(OP,VARR(T)), 0))

#else
#define VARR_ASSERT(EXPR,OP,T) ((void)(EXPR))
#endif

static void
varr_malloc_failure(void) {
    RUBY_LONGJMP(rtl_gen_jump_buf, 1);
}

/* Name of type for VARR of elements of type T.  */
#define VARR(T) VARR_##T
/* Name of function implementing OP for VARR of elements of type
   T.  */
#define VARR_OP(T, OP) VARR_##T##_##OP

/* Definition of type of VAR of elements of type T.  */
#define VARR_T(T)							      \
typedef struct VARR(T) {                                                      \
    size_t els_num; /* number of elements currently in VARR */  	      \
    size_t size;    /* curr size of container VARR in elements */             \
    T *varr;          /* the elements container */			      \
} VARR(T)

/* Default initial size for variable array elements container.  */
#define VARR_DEFAULT_SIZE 64

/* Definition of VARR of elements of type T and all its functions:  */
#define DEF_VARR(T)                                                            \
VARR_T(T);								       \
									       \
static inline void VARR_OP(T, create)(VARR(T) **varr_, size_t size_) {         \
    VARR(T) *va;						       	       \
    if (size_ == 0)							       \
        size_ = VARR_DEFAULT_SIZE;					       \
    *varr_ = va = (VARR(T) *) xmalloc(sizeof(VARR(T)));			       \
    if (va == NULL) varr_malloc_failure();				       \
    va->els_num = 0; va->size = size_;					       \
    va->varr = (T *) xmalloc(size_ * sizeof(T));			       \
    if (va->varr == NULL) varr_malloc_failure();			       \
}                                                                              \
                                                                               \
static inline void VARR_OP(T, destroy)(VARR(T) **varr_) {	               \
    VARR(T) *va = *varr_;						       \
    VARR_ASSERT(va && va->varr, "destroy", T);				       \
    free(va->varr);							       \
    free(va); 								       \
    *varr_ = NULL;							       \
}									       \
                                                                               \
static inline size_t VARR_OP(T, length)(const VARR(T) *varr_) {	               \
    VARR_ASSERT(varr_, "length", T);					       \
    return varr_->els_num;						       \
}                                                                              \
                                                                               \
static inline T *VARR_OP(T, addr)(const VARR(T) *varr_) {	               \
    VARR_ASSERT(varr_, "addr", T);					       \
    return &varr_->varr[0];						       \
}                                                                              \
                                                                               \
static inline T VARR_OP(T, last)(const VARR(T) *varr_) {                       \
    VARR_ASSERT(varr_ && varr_->varr && varr_->els_num, "last", T);	       \
    return varr_->varr[varr_->els_num - 1];				       \
}                                                                              \
                                                                               \
static inline T VARR_OP(T, get)(const VARR(T) *varr_, size_t ix_) { 	       \
    VARR_ASSERT(varr_ && varr_->varr && ix_ < varr_->els_num, "get", T);       \
    return varr_->varr[ix_];						       \
}                                                                              \
                                                                               \
static inline T VARR_OP(T, set)(const VARR(T) *varr_, size_t ix_, T obj_) {    \
    T old_obj_;								       \
    VARR_ASSERT(varr_ && varr_->varr && ix_ < varr_->els_num, "set", T);       \
    old_obj_ = varr_->varr[ix_];					       \
    varr_->varr[ix_] = obj_;						       \
    return old_obj_;							       \
}                                                                              \
                                                                               \
static inline void VARR_OP(T,trunc)(VARR(T) *varr_, size_t size_) {            \
    VARR_ASSERT(varr_ && varr_->varr && varr_->els_num >= size_, "trunc", T);  \
    varr_->els_num = size_;						       \
}									       \
                                                                               \
static inline void VARR_OP(T,expand)(VARR(T) *varr_, size_t size_) {	       \
    VARR_ASSERT(varr_  && varr_->varr, "expand", T);			       \
    if (varr_->size < size_) {						       \
	size_ += size_ / 2;						       \
	varr_->varr = (T *) xrealloc(varr_->varr, sizeof(T) * size_);	       \
        if (varr_->varr == NULL) varr_malloc_failure();			       \
	varr_->size = size_;						       \
    }                                                                          \
}									       \
									       \
static inline void VARR_OP(T, push)(VARR(T) *varr_, T obj_) {	               \
    T *slot_;								       \
    VARR_OP(T, expand)(varr_, varr_->els_num + 1);			       \
    slot_ = &varr_->varr[varr_->els_num++];				       \
    *slot_ = obj_;							       \
}                                                                              \
                                                                               \
static inline T VARR_OP(T, pop)(VARR(T) *varr_) {			       \
    T obj_;								       \
    VARR_ASSERT(varr_ && varr_->varr && varr_->els_num, "pop", T);	       \
    obj_ = varr_->varr[--varr_->els_num];				       \
    return obj_;							       \
}

/* Macros implementing operations for VARR V of elements of type T: */
#define VARR_CREATE(T, V, L) (VARR_OP(T, create)(&(V), L))
#define VARR_DESTROY(T, V) (VARR_OP(T, destroy)(&(V)))
#define VARR_LENGTH(T, V) (VARR_OP(T, length)(V))
#define VARR_ADDR(T, V) (VARR_OP(T, addr)(V))
#define VARR_LAST(T, V) (VARR_OP(T, last)(V))
#define VARR_GET(T, V, I) (VARR_OP(T, get)(V, I))
#define VARR_SET(T, V, I, O) (VARR_OP(T, set)(V, I, O))
#define VARR_TRUNC(T, V, S) (VARR_OP(T, trunc)(V, S))
#define VARR_EXPAND(T, V, S) (VARR_OP(T, expand)(V, S))
#define VARR_PUSH(T, V, O) (VARR_OP(T, push)(V, O))
#define VARR_POP(T, V) (VARR_OP(T, pop)(V))

/* Iseq currently being translated into RTL.  */
static rb_iseq_t *curr_iseq;

/* Definition of VARR of size_t elements.  */
DEF_VARR(size_t);

/* A stack of label positions in a stack insn sequence.  */
static VARR(size_t) *label_pos_stack;
/* Map: position in stack insn sequence -> index of first free slot in
   emulated VM stack.  */
static VARR(size_t) *pos_stack_free;

/* Definition of VARR of char elements.  */
DEF_VARR(char);

/* Label types: */
#define NO_LABEL 0
#define CONT_LABEL 1      /* Continuation label from a catch table  */
#define BRANCH_LABEL 2    /* Label from jump, conditional branch, or
			     opt_case_dispatch.  */

/* Map: position in stack insn sequence -> type of label at given
   position.  */
static VARR(char) *pos_label_type;
/* Map: position in stack insn sequence -> flag of that label at the
   position was processed at given iteration in function
   find_stack_values_on_labels.  */
static VARR(char) *label_processed_p;
/* Map: position in stack insn sequence -> flag of that the position
   is present in the catch table as a bound of the exception
   region. */
static VARR(char) *catch_bound_pos_p;
/* Map: position in stack insn sequence -> flag of that we should
   always put result of the insn at given position into a temp. */
static VARR(char) *use_only_temp_result_p;

/* Type of slot of the emulated VM stack.  */
enum slot_type {ANY, SELF, VAL, STR, LOC, TEMP};

/* Stack slot for emulated VM stack. */
struct stack_slot {
    enum slot_type mode;
    size_t source_insn_pos;
    union {
	VALUE val;     /* value */
	VALUE str;     /* string */
	vindex_t loc;  /* local var */
#ifndef NDEBUG
	vindex_t temp; /* local temp */
#endif
    } u;
};

typedef struct stack_slot stack_slot;

/* Definition of VARR of elements with type stack_slot.  */
DEF_VARR(stack_slot);

/* The emulated VM stack.  */
static VARR(stack_slot) *stack;

/* Max stack depth of the emulated VM stack.  */
static size_t max_stack_depth;

/* Map: var location index -> the current number of stack slots with
   given location in the emulated VM stack.  We need this map to
   process complicated stack insns generated for a multiple assignment
   (for example implementing a swap).  In this case we might need
   temporary variables to implement the assignemnt.  */
static VARR(size_t) *loc_stack_count;

/* Initiate LOC_STACK_COUNT and MAX_STACK_DEPTH.  */
static void
initialize_loc_stack_count(void) {
    size_t i, size;
    
    max_stack_depth = 0;
    VARR_TRUNC(size_t, loc_stack_count, 0);
    size = curr_iseq->body->local_table_size + VM_ENV_DATA_SIZE;
    for (i = 0; i < size; i++)
	VARR_PUSH(size_t, loc_stack_count, 0);
}

/* Decrease corresponding loc_stack_count element value for local var
   described by SLOT.  The slot will be changed into something
   else.  */
static void
prepare_stack_slot_rewrite(stack_slot *slot) {
    if (slot->mode == LOC) {
	assert(VARR_ADDR(size_t, loc_stack_count)[slot->u.loc] > 0);
	VARR_ADDR(size_t, loc_stack_count)[slot->u.loc]--;
    }
}

/* Increase loc_stack_count for a local variable in SLOT.  The slot
   will be pushed or changed into the local var.  */
static void
prepare_stack_slot_assign(stack_slot *slot) {
    if (slot->mode != LOC)
	return;
    VARR_ADDR(size_t, loc_stack_count)[slot->u.loc]++;
}

/* Pop and return a slot from the emulated VM stack.  Update value of
   loc_stack_count element corresponding to local var at the emulated
   VM stack slot.  */
static stack_slot
pop_stack_slot(void) {
    stack_slot slot;
    
    slot = VARR_POP(stack_slot, stack);
    if (slot.mode == LOC) {
	assert(VARR_ADDR(size_t, loc_stack_count)[slot.u.loc] > 0);
	VARR_ADDR(size_t, loc_stack_count)[slot.u.loc]--;
    }
    return slot;
}

/* Push SLOT to the emulated VM stack.  Update loc_stack_count if
   necessary through prepare_stack_slot_assign call.  */
static void
push_stack_slot(stack_slot slot) {
    size_t len = VARR_LENGTH(stack_slot, stack) + 1;

    assert(slot.mode != TEMP || slot.u.temp == -(vindex_t) len);
    if (slot.mode == LOC)
	prepare_stack_slot_assign(&slot);
    VARR_PUSH(stack_slot, stack, slot);
    if (max_stack_depth < len)
	max_stack_depth = len;
}

/* Truncate the stack to DEPTH length through pop_stack_slot to update
   loc_stack_count.  */
static void
trunc_stack(size_t depth) {
    while(VARR_LENGTH(stack_slot, stack) > depth)
	pop_stack_slot();
}

/* Change N-th element of the emulated stack slot onto SLOT.  */
static void
change_stack_slot(size_t n, stack_slot slot) {
    stack_slot *addr = VARR_ADDR(stack_slot, stack);
    
    assert(n < VARR_LENGTH(stack_slot, stack));
    prepare_stack_slot_rewrite(&addr[n]);
    prepare_stack_slot_assign(&slot);
    addr[n] = slot;
}

/* We are going to emulate assigning a value to a local var with index
   RES.  Check there is no slot with such var on the emulated VM
   stack.  Otherwise, emulate generation of RTL insns through calling
   ACTION to move the var value on the stack to temp vars.  Update the
   stack slots of the emulated VM stack.  */
static void
prepare_local_assign(vindex_t res, void (*action)(stack_slot *slot, vindex_t res)) {
    size_t i, len = VARR_LENGTH(stack_slot, stack);
    stack_slot *curr_slot;
    
    assert(res > 0);
    if (VARR_ADDR(size_t, loc_stack_count)[res] == 0)
	return;
    for (i = 0; i < len; i++) {
	curr_slot = &VARR_ADDR(stack_slot, stack)[i];
	if (curr_slot->mode == LOC)
	    action(curr_slot, -(vindex_t) i - 1);
    }
    assert(VARR_ADDR(size_t, loc_stack_count)[res] == 0);
}

/* Push value with MODE onto the emulated VM stack.  Use VAL as a
   parameter if necessary.  The value is a result of execution of
   stack insn at position SOURCE_INSN_POS.  */
static void
push_val(enum slot_type mode, VALUE val, size_t source_insn_pos) {
    stack_slot slot;
    
    slot.mode = mode;
    slot.source_insn_pos = source_insn_pos;
    switch (mode) {
    case VAL:
	slot.u.val = val;
	break;
    case STR:
	slot.u.str = val;
	break;
    case LOC:
	slot.u.loc = (vindex_t) val;
	break;
    case TEMP:
#ifndef NDEBUG
	slot.u.temp = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
#endif
	break;
    default:
	break;
    }
    push_stack_slot(slot);
}

#if RTL_GEN_DEBUG
/* Print stack slot S to stderr.  */
static void
print_stack_slot(stack_slot *s) {
    switch (s->mode) {
    case ANY:
	fprintf(stderr, " ANY");
	break;
    case SELF:
	fprintf(stderr, " SELF");
	break;
    case VAL:
	fprintf(stderr, " VAL(0x%lx)", (long unsigned) s->u.val);
	break;
    case STR:
	fprintf(stderr, " STR(0x%lx)", (long unsigned) s->u.str);
	break;
    case LOC:
	fprintf(stderr, " LOC(%ld)", (long) s->u.loc);
	break;
    case TEMP:
#ifndef NDEBUG
	fprintf(stderr, " TEMP(%ld)", (long) s->u.temp);
#else
	fprintf(stderr, " TEMP");
#endif
	break;
    default:
	assert(FALSE);
    }
}

/* Print the emulated VM stack into stderr.  */
static void
print_stack(void) {
    size_t i;
    
    fprintf(stderr, "Stack:");
    for (i = 0; i < VARR_LENGTH(stack_slot, stack); i++) {
	print_stack_slot(&VARR_ADDR(stack_slot, stack)[i]);
    }
    fprintf(stderr, "\n");
}
#endif

/* Return non-zero if slots S1 and S2 are equal.  */
static int
stack_slot_eq(const stack_slot *s1, const stack_slot *s2) {
    if (s1->mode != s2->mode)
	return FALSE;
    switch (s1->mode) {
    case VAL:
	return s1->u.val == s2->u.val;
    case STR:
	return s1->u.str == s2->u.str;
    case LOC:
	return s1->u.loc == s2->u.loc;
    case TEMP:
#ifndef NDEBUG
	assert(s1->u.temp == s2->u.temp);
#endif
	return TRUE;
    default:
	return TRUE;
    }
}

/* Map label pos in a stack insn sequence -> start slot index in
   array saved_stack_slots. */
static VARR(size_t) *label_start_stack_slot;
/* Emulated VM stack slots at each label.  */
static VARR(stack_slot) *saved_stack_slots;

/* Save the emulated VM stack in saved_stack_slots and return start
   index of the saved slot there.  Use current stack DEPTH to
   check.  */
static size_t
save_stack_slots(size_t depth) {
    size_t i, len = VARR_LENGTH(stack_slot, stack);
    size_t start = VARR_LENGTH(stack_slot, saved_stack_slots);
    stack_slot slot;
    
    assert(len == depth);
    for (i = 0; i < len; i++) {
	slot = VARR_ADDR(stack_slot, stack)[i];
	VARR_PUSH(stack_slot, saved_stack_slots, slot);
    }
    return start;
}

/* Restore the emulated VM stack with given DEPTH from
   saved_stack_slots whose elements start with index START in
   saved_stack_slots.  */
static void
restore_stack_slots(size_t start, size_t depth) {
    size_t i;
    stack_slot *addr = &VARR_ADDR(stack_slot, saved_stack_slots)[start];
    
    trunc_stack(0);
    for (i = 0; i < depth; i++)
	push_stack_slot(addr[i]);
}

/* Update saved_stack_slots elements starting with index
   START_STACK_SLOT_INDEX from the current stack.  It means changing a
   slot in saved_stack_slots to TEMP if the corresponding slots in
   saved_stack_slots and in the current emulated VM stack are
   different.  Return TRUE if the change happened.  */
static int
update_saved_stack_slots(size_t start_stack_slot_index) {
    size_t i, len = VARR_LENGTH(stack_slot, stack);
    stack_slot *stack_addr = VARR_ADDR(stack_slot, stack);
    stack_slot *saved_addr = &VARR_ADDR(stack_slot, saved_stack_slots)[start_stack_slot_index];
    int changed_p = FALSE;
    
#if RTL_GEN_DEBUG
    if (rtl_gen_debug_p) {
	fprintf(stderr, "     ==Stack union before -- ");
	print_stack();
    }
#endif
    for (i = 0; i < len; i++)
	if (stack_addr[i].mode == ANY)
	    ;
	else if (saved_addr[i].mode == ANY) {
	    saved_addr[i] = stack_addr[i];
	    changed_p = TRUE;
	} else if (! stack_slot_eq(&saved_addr[i], &stack_addr[i])) {
	    if (saved_addr[i].mode != TEMP) {
		changed_p = TRUE;
		VARR_ADDR(char, use_only_temp_result_p)[saved_addr[i].source_insn_pos] = TRUE;
#if RTL_GEN_DEBUG
		if (rtl_gen_debug_p) {
		    fprintf(stderr, "     ==Put into temp at pos=%ld\n", saved_addr[i].source_insn_pos);
		}
#endif
	    } else if (stack_addr[i].mode != TEMP) {
		changed_p = TRUE;
		VARR_ADDR(char, use_only_temp_result_p)[stack_addr[i].source_insn_pos] = TRUE;
#if RTL_GEN_DEBUG
		if (rtl_gen_debug_p) {
		    fprintf(stderr, "     ==Put into temp at pos=%ld\n", stack_addr[i].source_insn_pos);
		}
#endif
	    }
	    saved_addr[i].mode = TEMP;
#ifndef NDEBUG
	    saved_addr[i].u.temp = -(vindex_t) i - 1;
#endif
	    change_stack_slot(i, saved_addr[i]);
	}
#if RTL_GEN_DEBUG
    if (rtl_gen_debug_p) {
	fprintf(stderr, "     ==Stack union after -- ");
	print_stack();
    }
#endif
    return changed_p;
}

/* Flag setup when saved stack slots are changed.  */
static int stack_on_label_change_p;

/* Process a new LABEL of TYPE with stack DEPTH at the LABEL.  Set up
   or update saved stack slots for the label, put the label on the
   label stack if we need to process it in function
   find_stack_values_on_labels.  */
static void
process_label(int type, size_t label, size_t depth) {
    int prev_type = VARR_ADDR(char, pos_label_type)[label];

#if RTL_GEN_DEBUG
    if (rtl_gen_debug_p) {
	assert(depth == VARR_LENGTH(stack_slot, stack));
	fprintf(stderr, " Processing label %lu, type=%d, depth=%lu\n",
		label, type, VARR_LENGTH(stack_slot, stack));
    }
#endif
    assert(type != NO_LABEL);
    if (prev_type < type)
	VARR_ADDR(char, pos_label_type)[label] = type;
    if (prev_type == NO_LABEL) {
	VARR_ADDR(size_t, pos_stack_free)[label] = depth + 1;
	VARR_ADDR(size_t, label_start_stack_slot)[label] = save_stack_slots(depth);
	stack_on_label_change_p = TRUE;
#if RTL_GEN_DEBUG
	if (rtl_gen_debug_p) {
	    fprintf(stderr, "   Setting up stack at Label %lu -- ", label);
	    print_stack();
	}
#endif
    } else {
	assert(VARR_ADDR(size_t, pos_stack_free)[label] == depth + 1);
	if (update_saved_stack_slots(VARR_ADDR(size_t, label_start_stack_slot)[label]))
	    stack_on_label_change_p = TRUE;
    }
    if (! VARR_ADDR(char, label_processed_p)[label]) {
	size_t i, *label_pos_addr;
	
	VARR_PUSH(size_t, label_pos_stack, label);
	label_pos_addr = VARR_ADDR(size_t, label_pos_stack);
	/* Keep the label stack ordered to process label with smaller
	   positions first.  It decreases the number of iterations in
	   function find_stack_values_on_labels.  We could decrease it
	   even more if we ordered labels according reverse postorder
	   in iseq control flow graph.  But this approach is simple
	   and pretty good enough for a typical iseq GFG.  */
	for (i = VARR_LENGTH(size_t, label_pos_stack) - 1; i > 0; i--) {
	    if (label_pos_addr[i - 1] >= label)
		break;
	    label_pos_addr[i] = label_pos_addr[i - 1];
	}
	label_pos_addr[i] = label;
	VARR_ADDR(char, label_processed_p)[label] = TRUE;
#if RTL_GEN_DEBUG
	if (rtl_gen_debug_p) {
	    fprintf(stderr, "   Add label %lu for processing\n", label);
	}
#endif
    }
}

/* Argument of function mark_labe_from_hash.  */
struct label_arg {
    REL_PC incr;  /* base for pc relative label value */ 
    size_t depth; /* Stack depth at the label */
};

/* Process a label whose offset is given by value VAL whose additional
   characteristics are in ARG.  The label is from opt_case_dispatch
   hash.  Return ST_CONTINUE to process other labels from the
   hash.  */
static int
mark_label_from_hash(VALUE key, VALUE val, VALUE arg) {
    struct label_arg *label_arg = (struct label_arg *) arg;
    
    process_label(BRANCH_LABEL, FIX2INT(val) + label_arg->incr, label_arg->depth);
    return ST_CONTINUE;
}

/* Process continuation labels from the current iseq catch table.  Set
   up CATCH_BOUND_POS_P too.  */
static void
setup_labels_from_catch_table(void) {
    size_t i, j, iseq_size, size, depth;
    const struct iseq_catch_table *table;
    const struct iseq_catch_table_entry *entries;
    char *bound_addr;
    int label_type;
    
    VARR_TRUNC(char, catch_bound_pos_p, 0);
    iseq_size = curr_iseq->body->iseq_size;
    for (i = 0; i < iseq_size; i++)
	VARR_PUSH(char, catch_bound_pos_p, FALSE);
    table = curr_iseq->body->catch_table;
    if (table == NULL)
	return;
    size = table->size;
    entries = table->entries;
    bound_addr = VARR_ADDR(char, catch_bound_pos_p);
    for (i = 0; i < size; i++) {
	/* See hack for these catch types in compile.c.  */
	depth = entries[i].sp;
	/* Currently there might be garbage in the entry.  So ignore it.  */
	if ((int) depth < 0 || entries[i].start >= iseq_size
	    || entries[i].end >= iseq_size
	    || entries[i].cont >= iseq_size)
	    continue;
	assert((int) depth >= 0
	       && entries[i].start < iseq_size
	       && entries[i].end < iseq_size
	       && entries[i].cont < iseq_size);
	bound_addr[entries[i].start] = TRUE;
	bound_addr[entries[i].end] = TRUE;
#if RTL_GEN_DEBUG
	if (rtl_gen_debug_p) {
	    fprintf(stderr, "start=%d, end=%d", entries[i].start, entries[i].end);
	    fprintf(stderr, ", sp=%d, CATCH_TYPE=%d, \n", entries[i].sp, entries[i].type);
	}
#endif
	label_type = CONT_LABEL;
	if (entries[i].type == CATCH_TYPE_RESCUE
	    || entries[i].type == CATCH_TYPE_NEXT
	    || entries[i].type == CATCH_TYPE_BREAK)
	    depth++;
	if (depth != 0) {
	    for (j = 0; j < depth - 1; j++)
		push_val(TEMP, 0, 0);
	    push_val(TEMP, 0, 0);
	}
	process_label(label_type, entries[i].cont, depth);
	trunc_stack(0);
    }
}

#if RTL_GEN_DEBUG
/* Print the label stack to stderr.  */
static void
print_label_pos_stack(void) {
    size_t i, len, pos;
    size_t *label_pos_stack_addr;
    char *pos_label_type_addr = VARR_ADDR(char, pos_label_type);

    len = VARR_LENGTH(size_t, label_pos_stack);
    label_pos_stack_addr = VARR_ADDR(size_t, label_pos_stack);
    fprintf(stderr, "Label stack");
    for (i = 0; i < len; i++) {
	pos = label_pos_stack_addr[i];
	fprintf(stderr, " %lu:t%d", pos, pos_label_type_addr[pos]);
    }
    fprintf(stderr, "\n");
}
#endif

/* Modify SLOT to be a temporary with index RES.  */
static void
make_temp(stack_slot *slot, vindex_t res) {
    assert(slot->mode == LOC && res < 0);
    prepare_stack_slot_rewrite(slot);
    slot->mode = TEMP;
#ifndef NDEBUG
    slot->u.temp = res;
#endif
}

/* Data structure used for tracking events and source lines.  */
typedef struct {
    int defined_p; /* flag of defined event */
    struct iseq_insn_info_entry info_entry; /* info entry for the event  */
} event_t;

/* Combine EVENT1 and EVENT2 if it is possible.  Return true in case
   of a success and the combined event through RES.  */
static int combined_event_p(event_t event1, event_t event2, event_t *res) {
    if (! event1.defined_p) {
	*res = event2;
    } else if (! event2.defined_p) {
	*res = event1;
    } else if (event1.info_entry.line_no != event2.info_entry.line_no) {
	return FALSE;
    } else {
	*res = event1;
	res->info_entry.events |= event2.info_entry.events;
    }
    return TRUE;
}

/* Definition of VARR of long elements.  */
DEF_VARR(long);
/* Map: pos in a stack insn sequence -> index in insn info positions
   and entries, -1 if there is no corresponding insn info.  */
static VARR(long) *insn_info_entry_ind;

/* Return an event attached to position POS in stack insn sequence.  */
static event_t pos_event(size_t pos) {
    long ind;
    event_t event;
    
    if (pos >= curr_iseq->body->iseq_size || (ind = VARR_GET(long, insn_info_entry_ind, pos)) < 0) {
	event.defined_p = FALSE;
    } else {
	event.defined_p = TRUE;
	event.info_entry = curr_iseq->body->insns_info.body[ind];
    }
    return event;
}

/* Update the emulated VM stack and its DEPTH by insn in CODE at
   position POS.  */
static void
update_stack_by_insn(const VALUE *code, size_t pos, size_t *depth) {
    VALUE insn;
    size_t stack_insn_len;
    int result_p, temp_only_p;
    stack_slot slot;
#if RTL_GEN_DEBUG
    int processed_label_p;
#endif
    
    VARR_ADDR(size_t, pos_stack_free)[pos] = *depth + 1;
    insn = code[pos];
#if RTL_GEN_DEBUG
    if (rtl_gen_debug_p) {
	fprintf(stderr, "+%04lu %s: depth before=%lu", pos, insn_name(insn), *depth);
    }
#endif
    stack_insn_len = insn_len(insn);
    *depth = insn_stack_increase(*depth, insn, TRUE, &code[pos + 1]);
    result_p = FALSE;
    switch (insn) {
    case BIN(setlocal):
    case BIN(setlocal_WC_0):
    case BIN(setlocal_WC_1):
    case BIN(setspecial):
    case BIN(setinstancevariable):
    case BIN(setclassvariable):
    case BIN(setconstant):
    case BIN(setglobal):
    case BIN(setblockparam):
    case BIN(nop):
    case BIN(pop):
    case BIN(branchif):
    case BIN(branchunless):
    case BIN(branchnil):
    case BIN(opt_case_dispatch):
    case BIN(jump):
    case BIN(opt_call_c_function):
    case BIN(setn):
    case BIN(swap):
    case BIN(reverse):
    case BIN(adjuststack):
    case BIN(tracecoverage):
	assert(VARR_LENGTH(stack_slot, stack) >= *depth);
	trunc_stack(*depth);
	break;
    case BIN(dupn):
    case BIN(expandarray):
	/* Do nothing */
	break;
    default:
	result_p = TRUE;
	assert(*depth > 0 && VARR_LENGTH(stack_slot, stack) >= *depth - 1);
	trunc_stack(*depth - 1);
	break;
    }
#if RTL_GEN_DEBUG
    if (rtl_gen_debug_p) {
	processed_label_p = FALSE;
    }
#endif
    temp_only_p = VARR_ADDR(char, use_only_temp_result_p)[pos];
    if (! temp_only_p) {
	event_t event = pos_event(pos);
	event_t event2 = pos_event(pos + stack_insn_len);
	
	if (! combined_event_p(event, event2, &event))
	    /* If we can not combine the two attached events, we need
	       an insn to attach the 2nd event.  If we use a temporary
	       result, we will generate an RTL insn (except NOP and
	       some stack manipulation insns).  */
	    temp_only_p = VARR_ADDR(char, use_only_temp_result_p)[pos] = TRUE;
    }
    switch (insn) {
    case BIN(branchif):
    case BIN(branchunless):
    case BIN(branchnil):
    case BIN(getinlinecache):
    case BIN(jump): {
	if (result_p)
	    push_val(TEMP, 0, pos);
#if RTL_GEN_DEBUG
	if (rtl_gen_debug_p) {
	    fprintf(stderr, "\n");
	    processed_label_p = TRUE;
	}
#endif
	process_label(BRANCH_LABEL, code[pos + 1] + pos + stack_insn_len, *depth);
	break;
    }
    case BIN(opt_case_dispatch): {
	CDHASH hash = code[pos + 1];
	REL_PC incr = pos + stack_insn_len;
	struct label_arg arg;
		
#if RTL_GEN_DEBUG
	if (rtl_gen_debug_p) {
	    fprintf(stderr, "\n");
	    processed_label_p = TRUE;
	}
#endif
	arg.incr = incr;
	arg.depth = *depth;
	rb_hash_foreach(hash, mark_label_from_hash, (VALUE) &arg);
	process_label(BRANCH_LABEL, code[pos + 2] + incr, *depth); /* else label */
	break;
    }
    case BIN(putself):
	if (temp_only_p)
	    push_val(TEMP, 0, pos);
	else
	    push_val(SELF, Qnil, pos);
	break;
    case BIN(putnil):
	if (temp_only_p)
	    push_val(TEMP, 0, pos);
	else
	    push_val(VAL, Qnil, pos);
	break;
    case BIN(putobject):
	if (temp_only_p)
	    push_val(TEMP, 0, pos);
	else
	    push_val(VAL, code[pos + 1], pos);
	break;
    case BIN(putobject_INT2FIX_0_):
	if (temp_only_p)
	    push_val(TEMP, 0, pos);
	else
	    push_val(VAL, INT2FIX(0), pos);
	break;
    case BIN(putobject_INT2FIX_1_):
	if (temp_only_p)
	    push_val(TEMP, 0, pos);
	else
	    push_val(VAL, INT2FIX(1), pos);
	break;
    case BIN(getlocal):
	if (code[pos + 2] != 0 || temp_only_p)
	    push_val(TEMP, 0, pos);
	else
	    push_val(LOC, code[pos + 1], pos);
	break;
    case BIN(getlocal_WC_0):
	if (temp_only_p)
	    push_val(TEMP, 0, pos);
	else
	    push_val(LOC, code[pos + 1], pos);
	break;
    case BIN(getlocal_WC_1):
	push_val(TEMP, 0, pos);
	break;
    case BIN(setlocal):
	assert(! result_p);
	if (code[pos + 2] != 0)
	    break;
	/* Fall through */
    case BIN(setlocal_WC_0):
	prepare_local_assign(code[pos + 1], make_temp);
	break;
    case BIN(setn): {
	size_t len;
	rb_num_t n = code[pos + 1];

	len = VARR_LENGTH(stack_slot, stack);
	assert(len > n);
	slot = VARR_LAST(stack_slot, stack);
#ifndef NDEBUG
	if (slot.mode == TEMP)
	    slot.u.temp = n - (vindex_t) len;
#endif
	change_stack_slot(len - n - 1, slot);
	break;
    }
    case BIN(topn): {
	size_t len;
	rb_num_t n = code[pos + 1];
		
	len = VARR_LENGTH(stack_slot, stack);
	assert(len > n);
	slot = VARR_ADDR(stack_slot, stack)[len - n - 1];
	if (slot.mode == TEMP || temp_only_p)
	    push_val(TEMP, 0, pos);
	else
	    push_stack_slot(slot);
	break;
    }
    case BIN(dup): {
	slot = VARR_LAST(stack_slot, stack);
	if (slot.mode == TEMP || temp_only_p)
	    push_val(TEMP, 0, pos);
	else
	    push_stack_slot(slot);
	break;
    }
    case BIN(dupn): {
	size_t len;
	rb_num_t i, n = code[pos + 1];
		
	len = VARR_LENGTH(stack_slot, stack);
	assert(len >= n);
	for (i = 0; i < n; i++) {
	    slot = VARR_ADDR(stack_slot, stack)[len - n + i];
	    if (slot.mode == TEMP || temp_only_p)
		push_val(TEMP, 0, pos);
	    else
		push_stack_slot(slot);
	}
	break;
    }
    case BIN(swap): {
	vindex_t op = (vindex_t) VARR_LENGTH(stack_slot, stack);
	stack_slot slot2 = VARR_ADDR(stack_slot, stack)[op - 2];

	slot = VARR_ADDR(stack_slot, stack)[op - 1];
#ifndef NDEBUG
	if (slot.mode == TEMP)
	    slot.u.temp = -op + 1;
	if (slot2.mode == TEMP)
	    slot2.u.temp = -op;
#endif
	change_stack_slot(op - 1, slot2); 
	change_stack_slot(op - 2, slot); 
	break;
    }
    case BIN(reverse): {
	rb_num_t i, n = code[pos + 1];
	size_t len = VARR_LENGTH(stack_slot, stack);
		
	for (i = 0; i < n; i++) {
	    slot.mode = TEMP;
#ifndef NDEBUG
	    slot.u.temp = (vindex_t) i - (vindex_t) len;
#endif
	    change_stack_slot(len - i - 1, slot);
	}
	break;
    }
    case BIN(expandarray): {
	rb_num_t num = code[pos + 1];
	rb_num_t flag = code[pos + 2];
	rb_num_t i, cnt = num + (flag & 1 ? 1 : 0);
	size_t len = VARR_LENGTH(stack_slot, stack);

	assert(len > 0);
	trunc_stack(len - 1);
	for (i = 0; i < cnt; i++)
	    push_val(TEMP, 0, pos);
	break;
    }
    default:
	if (result_p)
	    push_val(TEMP, 0, pos);
	break;
    }
#if RTL_GEN_DEBUG
    if (rtl_gen_debug_p) {
	fprintf(stderr, processed_label_p ? " After insn " : ", after ");
	print_stack();
    }
#endif
    assert(*depth == VARR_LENGTH(stack_slot, stack));
}

/* Calculate the emulated VM stack values and depth at each label in
   the current iseq stack insn sequence.  It is a forward dataflow
   problem.  */
static void
find_stack_values_on_labels(void) {
    const VALUE *code = curr_iseq->body->iseq_encoded;
    size_t size = curr_iseq->body->iseq_size;
    VALUE insn;
    size_t pos, start_pos, stack_insn_len, depth;
    int niter;
#if RTL_GEN_DEBUG
    int type;
#endif

#if RTL_GEN_DEBUG
    if (rtl_gen_debug_p) {
	for (pos = 0; pos < size; pos += insn_len(code[pos]))
	    rb_iseq_disasm_insn(0, code, pos, curr_iseq, 0, -1);
    }
#endif
    VARR_TRUNC(size_t, pos_stack_free, 0);
    VARR_TRUNC(size_t, label_pos_stack, 0);
    VARR_TRUNC(char, pos_label_type, 0);
    VARR_TRUNC(char, label_processed_p, 0);
    VARR_TRUNC(char, use_only_temp_result_p, 0);
    trunc_stack(0);
    VARR_TRUNC(stack_slot, saved_stack_slots, 0);
    for (pos = 0; pos < size; pos++) {
	VARR_PUSH(size_t, pos_stack_free, 0); /* undefined */
	VARR_PUSH(char, pos_label_type, NO_LABEL);
	VARR_PUSH(char, label_processed_p, FALSE);
	VARR_PUSH(char, use_only_temp_result_p, FALSE);
	VARR_PUSH(size_t, label_start_stack_slot, 0);
    }
#if 0
    unsigned int *curr_insn_info_pos;
    
    for (pos = 0, curr_insn_info_pos = curr_iseq->body->insns_info.positions; pos < size; pos += stack_insn_len) {
	if (*curr_insn_info_pos == pos) {
	    VARR_ADDR(char, use_only_temp_result_p)[pos] = TRUE;
	    curr_insn_info_pos++;
	}
	insn = code[pos];
	stack_insn_len = insn_len(insn);
    }
#endif
    setup_labels_from_catch_table();
    niter = 0;
    do {
	niter++;
#if RTL_GEN_DEBUG
	if (rtl_gen_debug_p) {
	    fprintf(stderr, "+++++++++++++++Iteration = %d\n", niter);
	    type = BRANCH_LABEL;
	}
#endif
	stack_on_label_change_p = FALSE;
	pos = 0;
	depth = 0;
	while (TRUE) {
#if RTL_GEN_DEBUG
	    if (rtl_gen_debug_p) {
		size_t i;
		
		fprintf(stderr, "---Start at %lu(%d) label stack=", pos, type);
		for (i = 0; i < VARR_LENGTH(size_t, label_pos_stack); i++) {
		    size_t p = VARR_ADDR(size_t, label_pos_stack)[i];
		    fprintf(stderr, " %lu(%d)", p, VARR_ADDR(char, pos_label_type)[p]);
		}
		fprintf(stderr, "\n");
	    }
#endif
	    for (start_pos = pos; pos < size;) {
		if (VARR_ADDR(char, catch_bound_pos_p)[pos]) {
		    size_t i, len = VARR_LENGTH(stack_slot, stack);
		    stack_slot slot;
		    
		    for (i = 0; i < len; i++) {
			slot = VARR_ADDR(stack_slot, stack)[i];
			if (slot.mode != TEMP) {
			    slot.mode = TEMP;
			    VARR_ADDR(char, use_only_temp_result_p)[slot.source_insn_pos] = TRUE;
#ifndef NDEBUG
			    slot.u.temp = -(vindex_t) i - 1;
#endif
			    change_stack_slot(i, slot);

#if RTL_GEN_DEBUG
			    if (rtl_gen_debug_p) {
				fprintf(stderr, "     ==Make slot %ld temp at catch bound pos=%ld (producer insn pos=%ld)\n",
					i, pos, slot.source_insn_pos);
			    }
#endif
			}
		    }
		}
		if (pos != start_pos && VARR_ADDR(char, pos_label_type)[pos] != NO_LABEL) {
#if RTL_GEN_DEBUG
		    if (rtl_gen_debug_p) {
			fprintf(stderr, "Achieving label %lu(%d) by fall through\n", pos, type);
		    }
#endif
		    assert(depth + 1 == VARR_ADDR(size_t, pos_stack_free)[pos]);
		    if (update_saved_stack_slots(VARR_ADDR(size_t, label_start_stack_slot)[pos]))
			stack_on_label_change_p = TRUE;
		    else if (VARR_ADDR(char, label_processed_p)[pos])
			break;
#if RTL_GEN_DEBUG
		    if (rtl_gen_debug_p) {
			type = VARR_ADDR(char, pos_label_type)[pos];
		    }
#endif
		}
		insn = code[pos];
		stack_insn_len = insn_len(insn);
		update_stack_by_insn(code, pos, &depth);
		if (insn == BIN(jump) || insn == BIN(leave)/* || insn == BIN(throw)*/)
		    break;
		pos += stack_insn_len;
	    }
#if RTL_GEN_DEBUG
	    if (rtl_gen_debug_p) {
		print_label_pos_stack();
	    }
#endif
	    if (VARR_LENGTH(size_t, label_pos_stack) == 0)
		break;
	    pos = VARR_POP(size_t, label_pos_stack);
#if RTL_GEN_DEBUG
	    if (rtl_gen_debug_p) {
		type = VARR_ADDR(char, pos_label_type)[pos];
	    }
#endif
	    depth = VARR_ADDR(size_t, pos_stack_free)[pos];
	    assert(depth > 0);
	    depth--;
	    restore_stack_slots(VARR_ADDR(size_t, label_start_stack_slot)[pos], depth);
	}
	memset(VARR_ADDR(char, label_processed_p), FALSE, size);
    } while (stack_on_label_change_p);
}



/* Position of currently processed stack insn.  */
static size_t curr_source_insn_pos;

/* Map: position of stack insn in a stack insn sequence -> position of
   RTL insns corresponding to the stack insn in a RTL insn
   sequence.  */
static VARR(size_t) *new_insn_offsets;

/* Location of a label (a label insn field) in RTL insn sequence.  */
struct branch_target_loc {
    /* Position the next RTL insn.  */
    size_t next_insn_pc;
    /* Offset the label field relative to the next RTL insn position.
       It is a negative value.  */
    REL_PC offset;
};

typedef struct branch_target_loc branch_target_loc;

/* Definition of VARR of elements with type branch_target_loc.  */
DEF_VARR(branch_target_loc);

/* Locations of label fields in RTL insns.  We need to keep this to
   modify labels in generated RTL insns.  */
static VARR(branch_target_loc) *branch_target_locs;

/* Definition of VARR of elements with type VALUE.  */
DEF_VARR(VALUE);

/* A sequence of generated RTL insns.  */
static VARR(VALUE) *iseq_rtl;

DEF_VARR(int);
/* Map: stack insn position to the corresponding source line number.  */
static VARR(int) *source_pos2line;

DEF_VARR(unsigned);
/* RTL insn info positions beign generated.  */
static VARR(unsigned) *rtl_insn_event_positions;

DEF_VARR(event_t);
/* The corresponding insns_info entries for the above positions.  */
static VARR(event_t) *rtl_insn_events;

/* Return TRUE if RTL INSN never generate an exception.  */
static int
non_trapping_rtl_insn_p(VALUE insn) {
    /* ??? add more */
    return (insn == BIN(nop) || insn == BIN(var2var) || insn == BIN(temp2temp) || insn == BIN(loc2loc)
	    || insn == BIN(loc2temp) || insn == BIN(temp2loc) || insn == BIN(val2loc)
	    || insn == BIN(val2temp) || insn == BIN(self2var) || insn == BIN(iseq2var)); 
}

/* Return value of attribute leaf of RTL INSN with operands OPS.  */
static int
leaf_rtl_insn_p(VALUE insn, VALUE *ops) {
    return insn_leaf_flag(insn, ops);
}

/* Position in RTL insn stream correposding to a stack insn on a catch
   bound.  */
static size_t curr_catch_rtl_pos;

/* Add NOP before trapping RTL INSN with operands OPS on a catch
   bound.  */
static void add_nop_if_necessary(VALUE insn, VALUE *ops) {
    if (curr_catch_rtl_pos != VARR_LENGTH(VALUE, iseq_rtl)
	|| non_trapping_rtl_insn_p(insn) || ! leaf_rtl_insn_p(insn, ops))
	return;
    VARR_PUSH(VALUE, iseq_rtl, BIN(nop));
}

/* Append ARGC values to the RTL insn sequence.  Add NOP if
   necessary.  */
static void
append_vals(int argc, ...) {
    va_list argv;
    int i;
#define MAX_VALS_NUM 16
    VALUE vals[MAX_VALS_NUM];
    
    assert (argc > 0 && argc < MAX_VALS_NUM);
    va_start(argv, argc);
    for (i = 0; i < argc; i++)
	vals[i] = va_arg(argv, VALUE);
    va_end(argv);
    add_nop_if_necessary(vals[0], &vals[1]);
    for (i = 0; i < argc; i++)
	VARR_PUSH(VALUE, iseq_rtl, vals[i]);
}

/* Auxiliary append macros.  */
#define APPEND_INSN_OP0(insn_id) append_vals(1, (VALUE) (insn_id))
#define APPEND_INSN_OP1(insn_id, op1) append_vals(2, (VALUE) (insn_id), (VALUE) (op1))
#define APPEND_INSN_OP2(insn_id, op1, op2) \
    append_vals(3, (VALUE) (insn_id), (VALUE) (op1), (VALUE) (op2))
#define APPEND_INSN_OP3(insn_id, op1, op2, op3) \
    append_vals(4, (VALUE) (insn_id), (VALUE) (op1), (VALUE) (op2), (VALUE) (op3))
#define APPEND_INSN_OP4(insn_id, op1, op2, op3, op4)		\
    append_vals(5, (VALUE) (insn_id), (VALUE) (op1), (VALUE) (op2), (VALUE) (op3), (VALUE) (op4))
#define APPEND_INSN_OP5(insn_id, op1, op2, op3, op4, op5)		\
    append_vals(6, (VALUE) (insn_id), (VALUE) (op1), (VALUE) (op2), (VALUE) (op3), (VALUE) (op4), (VALUE) (op5))
#define APPEND_INSN_OP6(insn_id, op1, op2, op3, op4, op5, op6)		\
    append_vals(7, (VALUE) (insn_id), (VALUE) (op1), (VALUE) (op2), (VALUE) (op3), (VALUE) (op4), (VALUE) (op5), (VALUE) (op6))

/* Push a slot describing temp var with index RES to the emulated VM
   stack.  */
static void
push_temp_result(vindex_t res) {
    stack_slot slot;
    
    assert(res < 0);
    slot.mode = TEMP;
    slot.source_insn_pos = curr_source_insn_pos;
#ifndef NDEBUG
    assert(res == -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1);
    slot.u.temp = res;
#endif
    push_stack_slot(slot);
}

/* Push a slot describing top VM stack temporary var.  Return the
   temporary var index.  */
static vindex_t
new_top_stack_temp_var(void) {
    vindex_t res;
    
    res = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
    push_temp_result(res);
    return res;
}

/* Generate (zero or one) RTL insn to move value described by stack
   SLOT to a temporary or local var.  TOP is index of the stack slot
   in the emulated VM stack.  Return the index temporary or local var
   where the value of SLOT will be after that.  */
static vindex_t
to_var(stack_slot slot, vindex_t top) {
    assert(slot.mode != TEMP || top == slot.u.temp);
    if (slot.mode == LOC)
	return slot.u.loc;
    else if (slot.mode == SELF)
	APPEND_INSN_OP1(BIN(self2var), top);
    else if (slot.mode == VAL)
	APPEND_INSN_OP2(BIN(val2temp), top, slot.u.val);
    else if (slot.mode == STR)
	APPEND_INSN_OP2(BIN(str2var), top, slot.u.str);
    return top;
}

/* Generate (zero or more) RTL insns to move value described by SLOT
   to a temporary var with index RES.  STACK_P is TRUE if the slot is
   already in the emulated VM stack.  */
static void
to_temp(stack_slot *slot, vindex_t res, int stack_p) {
    assert(slot->mode != TEMP || res == slot->u.temp);
    if (slot->mode == LOC) {
	APPEND_INSN_OP2(BIN(loc2temp), res, slot->u.loc);
	if (stack_p)
	    prepare_stack_slot_rewrite(slot);
    } else if (slot->mode == SELF) {
	APPEND_INSN_OP1(BIN(self2var), res);
    } else if (slot->mode == VAL) {
	APPEND_INSN_OP2(BIN(val2temp), res, slot->u.val);
    } else if (slot->mode == STR) {
	APPEND_INSN_OP2(BIN(str2var), res, slot->u.str);
    }
    slot->mode = TEMP;
#ifndef NDEBUG
    slot->u.temp = res;
#endif
}

/* Add EVENT info for RTL being generated.  */
static void add_event(event_t event) {
    if (! event.defined_p)
	return;
    VARR_PUSH(unsigned, rtl_insn_event_positions, VARR_LENGTH(VALUE, iseq_rtl));
    VARR_PUSH(event_t, rtl_insn_events, event);
}

/* Pop a slot from the emulated VM stack.  Generate RTL insns to place
   the correponding value into a temporary var if the value is not
   there or into a local var.  Return index of the local or temporary
   variable. */
static vindex_t
get_var(void) {
    stack_slot slot;
    
    slot = pop_stack_slot();
    return to_var(slot, -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1);
}

/* As get_var but for two top slots.  Return indexes of the local or
   temporary variables through OP1 and OP2. */
static void
get_2vars(vindex_t *op1, vindex_t *op2) {
    stack_slot slot;
    
    slot = pop_stack_slot();
    *op2 = to_var(slot, -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1);
    slot = pop_stack_slot();
    *op1 = to_var(slot, -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1);
}

/* Generate (zero or more) RTL insns for stack insn getlocal with args
   IDX and LEVEL.  Always put the result into a temporary if
   TEMP_OMLY_P.  */
static void
get_local(lindex_t idx, rb_num_t level, int temp_only_p) {
    stack_slot slot;
    
    if (level == 0 && ! temp_only_p) {
	slot.mode = LOC;
	slot.source_insn_pos = curr_source_insn_pos;
	slot.u.loc = idx;
	push_stack_slot(slot);
    } else {
	vindex_t res = new_top_stack_temp_var();
	
	if (level == 0)
	    APPEND_INSN_OP2(BIN(loc2temp), res, idx);
	else
	    APPEND_INSN_OP3(BIN(uploc2temp), res, idx, level);
    }
}

/* Put a value decribed by SLOT into a temporary with index RES.  */
static void
move_to_temp(stack_slot *slot, vindex_t res) {
    to_temp(slot, res, TRUE);
}

/* Generate (one or more) RTL insns for stack insn setlocal with args
   IDX and LEVEL.  */
static void
set_local(lindex_t idx, rb_num_t level) {
    stack_slot slot;

    slot = pop_stack_slot();
    if (level == 0)
	prepare_local_assign(idx, move_to_temp);
    if (slot.mode == SELF) {
	if (level == 0)
	    APPEND_INSN_OP1(BIN(self2var), idx);
	else {
	    vindex_t op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	    
	    APPEND_INSN_OP1(BIN(self2var), op);
	    APPEND_INSN_OP3(BIN(var2uploc), idx, op, level);
	}
    } else if (slot.mode == VAL) {
	if (level == 0)
	    APPEND_INSN_OP2(BIN(val2loc), idx, slot.u.val);
	else
	    APPEND_INSN_OP3(BIN(val2uploc), idx, slot.u.val, level);
    } else if (slot.mode == STR) {
	if (level == 0) 
	    APPEND_INSN_OP2(BIN(str2var), idx, slot.u.str);
	else {
	    vindex_t op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	    
	    APPEND_INSN_OP2(BIN(str2var), op, slot.u.str);
	    APPEND_INSN_OP3(BIN(var2uploc), idx, op, level);
	}
    } else if (slot.mode == TEMP) {
	vindex_t op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	
	assert(op == slot.u.temp);
	if (level == 0)
	    APPEND_INSN_OP2(BIN(temp2loc), idx, op);
	else
	    APPEND_INSN_OP3(BIN(var2uploc), idx, op, level);
    } else {
	assert(slot.mode == LOC);
	if (level == 0)
	    APPEND_INSN_OP2(BIN(loc2loc), idx, slot.u.loc);
	else
	    APPEND_INSN_OP3(BIN(var2uploc), idx, slot.u.loc, level);
    }
}

/* Update the emulated VM stack for for stack insn putobject with arg
   V.  Always put the result into a temporary if TEMP_OMLY_P.  */
static void
putobject(VALUE v, int temp_only_p) {
    stack_slot slot;

    if (temp_only_p) {
	vindex_t res = new_top_stack_temp_var();
	
	APPEND_INSN_OP2(BIN(val2temp), res, v);
    } else {
	slot.mode = VAL;
	slot.source_insn_pos = curr_source_insn_pos;
	slot.u.val = v;
	push_stack_slot(slot);
    }
}

/* Generate RTL insns to put values on the emulated VM stack.  The
   values are described by ARGS_NUM top slots on the emulated VM
   stack.  */
static void
put_on_stack(vindex_t args_num) {
    vindex_t op, i;
    vindex_t len = VARR_LENGTH(stack_slot, stack);
    
    assert(args_num <= len);
    for (i = len - args_num; i < len; i++) {
	op = -i - 1;
	to_temp(&VARR_ADDR(stack_slot, stack)[i], op, TRUE);
    }
}

/* As above plus truncate the emulated VM stack correspondingly.  */
static void
put_args_on_stack(vindex_t args_num) {
    vindex_t len = VARR_LENGTH(stack_slot, stack);

    put_on_stack(args_num);
    trunc_stack(len - args_num);
}

/* Generate RTL insn RES_INSN for a special load stack insn with ARGS.
   ARG2_P is true if the stack insn has two args.  */
static void
specialized_load(enum ruby_vminsn_type res_insn, const VALUE *args, int arg2_p) {
    vindex_t res = new_top_stack_temp_var();
    
    if (arg2_p)
	APPEND_INSN_OP3(res_insn, res, args[0], args[1]);
    else
	APPEND_INSN_OP2(res_insn, res, args[0]);
}

/* Generate (one or more) RTL insns for a special store stack insn
   with ARGS.  The corresponding RTL insn which actually does a store
   has code RES_INSN.  */
static void
specialized_store(enum ruby_vminsn_type res_insn, const VALUE *args) {
    stack_slot slot;
    vindex_t op;
    
    slot = pop_stack_slot();
    op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
    op = to_var(slot, op);
    APPEND_INSN_OP2(res_insn, args[0], op);
}

/* Return call data without kwarg corresponding to call info CI of
   the current iseq.  Return NULL if CI is not for a call without kwarg.  */
static struct rb_call_data *
get_cd_data(CALL_INFO ci) {
    size_t ci_size = curr_iseq->body->ci_size;
    struct rb_call_info *ci_entries = curr_iseq->body->ci_entries;
    
    if (ci < ci_entries || (ci_entries + ci_size) <= ci)
	return NULL;
    return &curr_iseq->body->cd_entries[ci - ci_entries];
}

/* Return call data with kwarg corresponding to call info CI of the
   current iseq.  Return NULL if CI is not for call with kwarg.  */
static struct rb_call_data_with_kwarg *
get_cd_data_with_kw_arg(CALL_INFO ci) {
    struct rb_call_info_with_kwarg * cikw = (struct rb_call_info_with_kwarg *) ci;
    size_t ci_size = curr_iseq->body->ci_size;
    size_t cikw_size = curr_iseq->body->ci_kw_size;
    struct rb_call_info_with_kwarg *cikw_entries
	= (struct rb_call_info_with_kwarg *) (curr_iseq->body->ci_entries + ci_size);
    struct rb_call_data_with_kwarg *cdkw_entries
	= (struct rb_call_data_with_kwarg *) (curr_iseq->body->cd_entries + ci_size);
    
    if (cikw < cikw_entries || (cikw_entries + cikw_size) <= cikw)
	return NULL;
    return &cdkw_entries[cikw - cikw_entries];
}

/* Return call data corresponding to call info CI of the current iseq.  */
static struct rb_call_data *
get_cd(CALL_INFO ci, CALL_CACHE cc) {
    struct rb_call_data *cd;
    struct rb_call_data_with_kwarg *cdkw;

    if ((cd = get_cd_data(ci)) != NULL) {
	cd->call_info = *ci;
	if (cc != NULL)
	    cd->call_cache = *cc;
	cd->call_start = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	return cd;
    }
    if ((cdkw = get_cd_data_with_kw_arg(ci)) != NULL) {
	cdkw->call_info = *ci;
	if (cc != NULL)
	    cdkw->call_cache = *cc;
	cdkw->call_start = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	cdkw->kw_arg = ((struct rb_call_info_with_kwarg *) ci)->kw_arg;
	return (struct rb_call_data *)cdkw;
    }
    assert(FALSE);
    return NULL; /* to remove a compiler warning */
}

/* Generate RTL insns from the current iseq stack insn call whose
   operands are in ARGS.  BLOCK is a block in the call.  Zero means no
   block in the call.  */
static void
generate_call(const VALUE *args, VALUE block) {
    CALL_INFO ci = (CALL_INFO) args[0];
    CALL_CACHE cc = (CALL_CACHE) args[1];
    struct rb_call_data *cd;
    vindex_t args_num;
    stack_slot slot;
    int stack_block_p = ci->flag & VM_CALL_ARGS_BLOCKARG;
    
    args_num = ci->orig_argc + (stack_block_p ? 1: 0);
    put_args_on_stack(args_num);
    slot = pop_stack_slot();
    cd = get_cd(ci, cc);
    if (slot.mode == SELF) {
	if (block == 0 && ! stack_block_p)
	    APPEND_INSN_OP2(BIN(simple_call_self), cd, cd->call_start);
	else
	    APPEND_INSN_OP3(BIN(call_self), cd, cd->call_start, block);
    } else if (slot.mode == LOC) {
	if (block == 0 && ! stack_block_p)
	    APPEND_INSN_OP3(BIN(simple_call_recv), cd, cd->call_start, slot.u.loc);
	else
	    APPEND_INSN_OP4(BIN(call_recv), cd, cd->call_start, block, slot.u.loc);
    } else {
	to_temp(&slot, cd->call_start, FALSE);
	if (block == 0 && ! stack_block_p)
	    APPEND_INSN_OP2(BIN(simple_call), cd, cd->call_start);
	else
	    APPEND_INSN_OP3(BIN(call), cd, cd->call_start, block);
    }
    new_top_stack_temp_var();
}

/* Generate RTL insns from unary operator insn whose operands are in
   ARGS.  The corresponding RTL insn code is RES_INSN.  */
static void
generate_unary_op(const VALUE *args, enum ruby_vminsn_type res_insn) {
    CALL_INFO ci = (CALL_INFO) args[0];
    CALL_CACHE cc = (CALL_CACHE) args[1];
    struct rb_call_data *cd;
    vindex_t op, res;
    stack_slot slot;
    
    slot = pop_stack_slot();
    cd = get_cd(ci, cc);
    if (slot.mode == SELF) {
	op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	APPEND_INSN_OP1(BIN(self2var), op);
    } else if (slot.mode == VAL) {
	op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	APPEND_INSN_OP2(BIN(val2temp), op, slot.u.val);
    } else if (slot.mode == STR) {
	op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	APPEND_INSN_OP2(BIN(str2var), op, slot.u.str);
    } else {
	assert(slot.mode == LOC || slot.mode == TEMP);
	assert(slot.mode != TEMP || slot.u.temp == -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1);
	op = slot.mode == LOC ? slot.u.loc : -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
    }
    res = new_top_stack_temp_var();
    APPEND_INSN_OP3(res_insn, cd, res, op);
}

/* Return a variant of insn INSN_ID with an immediate operand (fixnum
   if FIXNUM_P or flonum if FLONUM_P or string otherwise).  Return NOP
   if there is no such insn.  */
static enum ruby_vminsn_type
make_imm_id(enum ruby_vminsn_type insn_id, int fixnum_p, int flonum_p) {
    if (!fixnum_p && !flonum_p)
	return insn_id == BIN(ind) ? BIN(inds) : insn_id == BIN(indset) ? BIN(indsets): BIN(nop);
    assert((fixnum_p && !flonum_p) || (!fixnum_p && flonum_p));
    switch (insn_id) {
    case BIN(plus): return fixnum_p ? BIN(plusi) : BIN(plusf);
    case BIN(minus): return fixnum_p ? BIN(minusi) : BIN(minusf);
    case BIN(mult): return fixnum_p ? BIN(multi) : BIN(multf);
    case BIN(eq): return fixnum_p ? BIN(eqi) : BIN(eqf);
    case BIN(ne): return fixnum_p ? BIN(nei) : BIN(nef);
    case BIN(lt): return fixnum_p ? BIN(lti) : BIN(ltf);
    case BIN(gt): return fixnum_p ? BIN(gti) : BIN(gtf);
    case BIN(le): return fixnum_p ? BIN(lei) : BIN(lef);
    case BIN(ge): return fixnum_p ? BIN(gei) : BIN(gef);
    case BIN(div): return fixnum_p ? BIN(divi) : BIN(divf);
    case BIN(mod): return fixnum_p ? BIN(modi) : BIN(modf);
    case BIN(or): return fixnum_p ? BIN(ori) : BIN(nop);
    case BIN(and): return fixnum_p ? BIN(andi) : BIN(nop);
    case BIN(ltlt): return fixnum_p ? BIN(ltlti) : BIN(nop);
    case BIN(ind): return fixnum_p ? BIN(indi) : BIN(nop);
    case BIN(indset): return fixnum_p ? BIN(indseti) : BIN(nop);
    default: return BIN(nop);
    }
}

/* Generate RTL insns for operands of general RTL insn RES_INSN from
   an binary operator insn whose call related operands are in ARGS.
   Return RTL insn operands and call data through RES, OP, OP2, and
   CD.  Return RTL insn code which will be actually used.  */
static enum ruby_vminsn_type
get_binary_ops(enum ruby_vminsn_type res_insn, const VALUE *args,
	       vindex_t *res, vindex_t *op, VALUE *op2, struct rb_call_data **cd) {
    CALL_INFO ci = (CALL_INFO) args[0];
    CALL_CACHE cc = (CALL_CACHE) args[1];
    enum ruby_vminsn_type imm_insn;
    stack_slot slot, slot2;

    slot2 = pop_stack_slot();
    slot = pop_stack_slot();
    *res = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
    *cd = get_cd(ci, cc);
    *op = to_var(slot, *res);
    if (slot2.mode == SELF) {
	*op2 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 2;
	APPEND_INSN_OP1(BIN(self2var), *op2);
    } else if (slot2.mode == VAL) {
	imm_insn = BIN(nop);
	if (FIXNUM_P(slot2.u.val))
	    imm_insn = make_imm_id(res_insn, TRUE, FALSE);
	else if (FLONUM_P(slot2.u.val))
	    imm_insn = make_imm_id(res_insn, FALSE, TRUE); 
	if (imm_insn != BIN(nop)) {
	    *op2 = slot2.u.val;
	    res_insn = imm_insn;
	} else {
	    *op2 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 2;
	    APPEND_INSN_OP2(BIN(val2temp), *op2, slot2.u.val);
	}
    } else if (slot2.mode == STR) {
	imm_insn = make_imm_id(res_insn, FALSE, FALSE);
	if (imm_insn != BIN(nop)) {
	    *op2 = slot2.u.str;
	    res_insn = imm_insn;
	} else {
	    *op2 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 2;
	    APPEND_INSN_OP2(BIN(str2var), *op2, slot2.u.str);
	}
    } else {
	assert(slot2.mode == LOC || slot2.mode == TEMP);
	assert(slot2.mode != TEMP || slot2.u.temp == -(vindex_t) VARR_LENGTH(stack_slot, stack) - 2);
	*op2 = slot2.mode == LOC ? slot2.u.loc : -(vindex_t) VARR_LENGTH(stack_slot, stack) - 2;
    }
    return res_insn;
}

/* Return a simple insn code for INSN_ID.  If there is no
   corresponding simple insn, return nop.  */
static enum ruby_vminsn_type
get_simple_insn (enum ruby_vminsn_type insn_id) {
    switch (insn_id) {
    case BIN(plus): return BIN(splus);
    case BIN(minus): return BIN(sminus);
    case BIN(mult): return BIN(smult);
    case BIN(eq): return BIN(seq);
    case BIN(ne): return BIN(sne);
    case BIN(lt): return BIN(slt);
    case BIN(gt): return BIN(sgt);
    case BIN(le): return BIN(sle);
    case BIN(ge): return BIN(sge);
    case BIN(div): return BIN(sdiv);
    case BIN(mod): return BIN(smod);
    case BIN(or): return BIN(sor);
    case BIN(and): return BIN(sand);
    default: return BIN(nop);
    }
}

/* Generate RTL insns from a binary operator insn whose operands are
   in ARGS.  The corresponding general RTL insn code is RES_INSN. */
static void
generate_bin_op(const VALUE *args, enum ruby_vminsn_type res_insn) {
    enum ruby_vminsn_type simple_insn;
    struct rb_call_data *cd;
    vindex_t op, res;
    VALUE op2;
    
    res_insn = get_binary_ops(res_insn, args, &res, &op, &op2, &cd);
    push_temp_result(res);
    if (res == op && (vindex_t) op2 + 1 == op && (simple_insn = get_simple_insn(res_insn)) != BIN(nop))
	APPEND_INSN_OP2(simple_insn, cd, res);
    else
	APPEND_INSN_OP4(res_insn, cd, res, op, op2);
}

/* Return an RTL compare branch insn code.  The original RTL compare
   insn code is CMP_INSN.  BT_P is true if we are combining with
   branch on true.  */
static enum ruby_vminsn_type
get_bcmp_insn(enum ruby_vminsn_type cmp_insn, int bt_p) {
    switch (cmp_insn) {
    case BIN(eq): return bt_p ? BIN(bteq): BIN(bfeq);
    case BIN(ne): return bt_p ? BIN(btne): BIN(bfne);
    case BIN(lt): return bt_p ? BIN(btlt): BIN(bflt);
    case BIN(gt): return bt_p ? BIN(btgt): BIN(bfgt);
    case BIN(le): return bt_p ? BIN(btle): BIN(bfle);
    case BIN(ge): return bt_p ? BIN(btge): BIN(bfge);
    case BIN(eqi): return bt_p ? BIN(bteqi): BIN(bfeqi);
    case BIN(nei): return bt_p ? BIN(btnei): BIN(bfnei);
    case BIN(lti): return bt_p ? BIN(btlti): BIN(bflti);
    case BIN(gti): return bt_p ? BIN(btgti): BIN(bfgti);
    case BIN(lei): return bt_p ? BIN(btlei): BIN(bflei);
    case BIN(gei): return bt_p ? BIN(btgei): BIN(bfgei);
    case BIN(eqf): return bt_p ? BIN(bteqf): BIN(bfeqf);
    case BIN(nef): return bt_p ? BIN(btnef): BIN(bfnef);
    case BIN(ltf): return bt_p ? BIN(btltf): BIN(bfltf);
    case BIN(gtf): return bt_p ? BIN(btgtf): BIN(bfgtf);
    case BIN(lef): return bt_p ? BIN(btlef): BIN(bflef);
    case BIN(gef): return bt_p ? BIN(btgef): BIN(bfgef);
    default:
	assert(FALSE);
    }
    return BIN(nop); /* to remove a compiler warning */
}

/* Add an RTL insn to have the same value of the emulated VM stack
   slot with index N as the corresponding saved stack slot value for
   label at position POS.  */
static void
tune_stack_slot(size_t pos, size_t n) {
    size_t start = VARR_ADDR(size_t, label_start_stack_slot)[pos];
    stack_slot *saved_slots_addr = &VARR_ADDR(stack_slot, saved_stack_slots)[start];
    stack_slot *slots_addr = VARR_ADDR(stack_slot, stack);
    
    assert(VARR_ADDR(char, pos_label_type)[pos] != NO_LABEL);
#if RTL_GEN_DEBUG
    if (rtl_gen_debug_p) {
	fprintf(stderr, "   ==Adjusting stack slot %lu -- before:", (long unsigned) n);
	print_stack_slot(&slots_addr[n]);
    }
#endif
    if (saved_slots_addr[n].mode != TEMP || slots_addr[n].mode == TEMP || slots_addr[n].mode == ANY) {
#if RTL_GEN_DEBUG
	if (rtl_gen_debug_p) {
	    fprintf(stderr, " -- no change\n");
	}
#endif
    } else {
	to_temp(&slots_addr[n], -(vindex_t) n - 1, TRUE);
#if RTL_GEN_DEBUG
	if (rtl_gen_debug_p) {
	    fprintf(stderr, ", after:");
	    print_stack_slot(&slots_addr[n]);
	    fprintf(stderr, "\n");
	}
#endif
    }
    assert(slots_addr[n].mode == ANY || saved_slots_addr[n].mode == ANY
	   || stack_slot_eq(&saved_slots_addr[n], &slots_addr[n]));
}

/* Add RTL insns to have the same emulated VM stack values as the
   saved stack values for label at position POS.  Setup emulated VM
   stack slots as saved ones if RETORE_P.  */
static void
tune_stack(size_t pos, int label_type, int restore_p) {
    size_t i;
    size_t depth = VARR_ADDR(size_t, pos_stack_free)[pos];
    size_t len = VARR_LENGTH(stack_slot, stack);
    
    assert(VARR_ADDR(char, pos_label_type)[pos] != NO_LABEL && depth != 0);
    depth--;
    if (restore_p) {
	size_t start = VARR_ADDR(size_t, label_start_stack_slot)[pos];
	stack_slot *saved_slots_addr = &VARR_ADDR(stack_slot, saved_stack_slots)[start];
	
	trunc_stack(0);
	for (i = 0; i < depth; i++) {
	  stack_slot slot = saved_slots_addr[i];

	  if (slot.mode == ANY && 0) {
	    slot.mode = TEMP;
	    slot.u.temp = -(vindex_t) i - 1;
#if RTL_GEN_DEBUG
	    if (rtl_gen_debug_p) {
		fprintf(stderr, "-->Changing ANY on TEMP (%ld)\n", slot.u.temp);
	    }
#endif
	  }
	  push_stack_slot(slot);
	}
#if RTL_GEN_DEBUG
	if (rtl_gen_debug_p) {
	    fprintf(stderr, "-->Restoring stack at pos %lu. ", (long unsigned) pos);
	    print_stack();
	}
#endif
	return;
    }
    assert(len == depth);
    for (i = 0; i < depth; i++) {
	tune_stack_slot(pos, i);
    }
}

/* Generate RTL insns from a stack comparison insn in CODE at
   position POS.  The corresponding general RTL comparison insn code
   is RES_INSN.  Combine with the next branch insn if possible.
   Return position of the next stack insn should be processed after
   that.  */
static size_t
generate_rel_op(const VALUE *code, size_t pos, enum ruby_vminsn_type res_insn) {
    int bt_p;
    size_t len = insn_len(code[pos]);
    const VALUE *args = &code[pos + 1];
    enum ruby_vminsn_type next_insn = code[pos + len];
    size_t dest, next_insn_len = insn_len(next_insn);
    vindex_t op, res;
    VALUE op2;
    struct rb_call_data *cd;
    branch_target_loc loc;

    if (VARR_ADDR(char, pos_label_type)[pos] != NO_LABEL
	|| (next_insn != BIN(branchif) && next_insn != BIN(branchunless))) {
	generate_bin_op((res_insn == BIN(ne) ? args + 2 : args), res_insn);
	return len;
    }
    res_insn = get_binary_ops(res_insn, (res_insn == BIN(ne) ? args + 2 : args), &res, &op, &op2, &cd);
    bt_p = next_insn == BIN(branchif);
    res_insn = get_bcmp_insn(res_insn, bt_p);
    dest = code[pos + len + 1] + pos + len + next_insn_len;
    tune_stack(dest, BRANCH_LABEL, FALSE);
    APPEND_INSN_OP6(res_insn, bt_p ? BIN(cont_btcmp) : BIN(cont_bfcmp),
		    dest, cd, res, op, op2);
    loc.next_insn_pc = VARR_LENGTH(VALUE, iseq_rtl);
    loc.offset = 5;
    VARR_PUSH(branch_target_loc, branch_target_locs, loc);
    return len + next_insn_len;
}

/* Generate RTL insns from a stack insn opt_aref (STR == Qnil in
   this case) or opt_aref_with (STR is the first operand) whose call
   related operands are in ARGS.  */
static void
generate_aset_op(const VALUE *args, VALUE str) {
    CALL_INFO ci = (CALL_INFO) args[0];
    CALL_CACHE cc = (CALL_CACHE) args[1];
    struct rb_call_data *cd;
    vindex_t op, op3, res;
    VALUE op2;
    enum ruby_vminsn_type res_insn = BIN(indset);
    enum ruby_vminsn_type imm_insn;
    stack_slot slot, slot2, slot3;
    
    slot3 = pop_stack_slot();
    if (str == Qnil)
	slot2 = pop_stack_slot();
    else { /* to remove warnings */
	slot2.mode = VAL;
	slot2.u.val = 1;
    }
    slot = pop_stack_slot();
    res = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
    cd = get_cd(ci, cc);
    op = to_var(slot, res);
    if (str != Qnil) {
	imm_insn = make_imm_id(res_insn, FALSE, FALSE);
	assert(imm_insn != BIN(nop));
	op2 = str;
	res_insn = imm_insn;
    } else if (slot2.mode == SELF) {
	op2 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 2;
	APPEND_INSN_OP1(BIN(self2var), op2);
    } else if (slot2.mode == VAL) {
	imm_insn = BIN(nop);
	if (FIXNUM_P(slot2.u.val))
	    imm_insn = make_imm_id(res_insn, TRUE, FALSE);
	else if (FLONUM_P(slot2.u.val))
	    imm_insn = make_imm_id(res_insn, FALSE, TRUE); 
	if (imm_insn != BIN(nop)) {
	    op2 = slot2.u.val;
	    res_insn = imm_insn;
	} else {
	    op2 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 2;
	    APPEND_INSN_OP2(BIN(val2temp), op2, slot2.u.val);
	}
    } else if (slot2.mode == STR) {
	op2 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 2;
	APPEND_INSN_OP2(BIN(str2var), op2, slot2.u.str);
    } else {
	assert(slot2.mode == LOC || slot2.mode == TEMP);
	assert(slot2.mode != TEMP || slot2.u.temp == -(vindex_t) VARR_LENGTH(stack_slot, stack) - 2);
	op2 = slot2.mode == LOC ? slot2.u.loc : -(vindex_t) VARR_LENGTH(stack_slot, stack) - 2;
    }
    op3 = to_var(slot3, -(vindex_t) VARR_LENGTH(stack_slot, stack) - 2 - (str == Qnil ? 1 : 0));
    res = new_top_stack_temp_var();
    APPEND_INSN_OP5(res_insn, cd, res, op, op2, op3);
}

/* Info how to update label offsets in hash of opt_case_dispatch
   insn.  */
struct hash_label_transform_arg {
    VALUE hash;  /* hash from opt_case_dispatch insn  */
    int map_p;   /* if true use new_insn_offsets firstly */
    REL_PC decr; /* decrement the label offset by it secondly */
};

/* Return updated label OFFSET using info in ARG.  */
static REL_PC
update_case_hash(REL_PC offset, struct hash_label_transform_arg *arg) {
    if (arg->map_p) {
	offset = VARR_ADDR(size_t, new_insn_offsets)[offset];
    }
    return offset - arg->decr;
}

/* Change label value VAL in HASH using KEY info in ARG_PTR.  Return
   flag to continue updates of labels in the hash table.  */
static int
transform_hash_offset(VALUE key, VALUE val, void *arg_ptr) {
    struct hash_label_transform_arg *arg
	= (struct hash_label_transform_arg *) arg_ptr;

    rb_hash_aset(arg->hash, key, INT2FIX(update_case_hash(FIX2INT(val), arg)));
    return ST_CONTINUE;
}

/* Change label values in HASH using info in ARG.  */
static void
change_hash_values(VALUE hash, struct hash_label_transform_arg *arg) {
    rb_hash_foreach(hash, transform_hash_offset, (VALUE)arg);
    rb_hash_rehash(hash);
    OBJ_FREEZE(hash);
    RBASIC_CLEAR_CLASS(hash);
}

/* Generate an RTL ret insn from stack insn leave.  */
static void
generate_leave(void) {
    stack_slot slot;
    vindex_t op = -(vindex_t) VARR_LENGTH(stack_slot, stack);

    slot = VARR_LAST(stack_slot, stack);
    if (slot.mode == SELF) {
	APPEND_INSN_OP1(BIN(self2var), op);
	APPEND_INSN_OP1(BIN(temp_ret), op);
    } else if (slot.mode == VAL) {
	APPEND_INSN_OP1(BIN(val_ret), slot.u.val);
    } else if (slot.mode == STR) {
	APPEND_INSN_OP2(BIN(str2var), op, slot.u.str);
	APPEND_INSN_OP1(BIN(temp_ret), op);
    } else if (slot.mode == TEMP) {
	assert(slot.u.temp == op);
	APPEND_INSN_OP1(BIN(temp_ret), op);
    } else {
	assert(slot.mode == LOC);
	APPEND_INSN_OP1(BIN(loc_ret), slot.u.loc);
    }
}

/* True if we are currently processing an unreachable stack insn.  */
static int unreachable_code_p;

/* Generate (zero or more) RTL insns for stack insn of the current
   iseq at the current position in CODE.  The stack insn was not
   modified for direct threading so far.  Update CURR_SOURCE_INSN_POS
   to the position of the next stack insn should be processed after
   that.  The previous insn (or nop if it is the first insn) is passed
   by PREV_INSN.  */ 
static void
translate_stack_insn(const VALUE *code, enum ruby_vminsn_type prev_insn) {
    VALUE insn;
    size_t stack_insn_len;
    stack_slot slot;
    branch_target_loc loc;
    int label_type, temp_only_p, nop_p;
    size_t rtl_pos, pos = curr_source_insn_pos;
    event_t event, last_event;
    
    insn = code[pos];
    stack_insn_len = insn_len(insn);
    label_type = VARR_ADDR(char, pos_label_type)[pos];
    if (prev_insn == BIN(jump) || prev_insn == BIN(leave)/* || prev_insn == BIN(throw)*/)
	unreachable_code_p = TRUE;
    if (label_type != NO_LABEL) {
#if RTL_GEN_DEBUG
	if (rtl_gen_debug_p) {
	    fprintf(stderr, "Label %lu, type = %d, depth = %lu\n",
		    pos, label_type, VARR_LENGTH(stack_slot, stack));
	}
#endif
	tune_stack(pos, label_type, unreachable_code_p);
	unreachable_code_p = FALSE;
    }
#if RTL_GEN_DEBUG
    if (rtl_gen_debug_p) {
	fprintf(stderr, "*%04lu %s%s - ", pos, insn_name(insn), (unreachable_code_p && label_type == NO_LABEL ? " unreachable" : ""));
    }
#endif
    if (unreachable_code_p) {
	VARR_ADDR(size_t, new_insn_offsets)[pos] = VARR_LENGTH(VALUE, iseq_rtl);
#if RTL_GEN_DEBUG
	if (rtl_gen_debug_p) {
	    fprintf(stderr, "\n");
	}
#endif
	if (VARR_ADDR(char, catch_bound_pos_p)[pos])
	    APPEND_INSN_OP0(BIN(nop));
	curr_source_insn_pos += stack_insn_len;
	return;
    }
    rtl_pos = VARR_LENGTH(VALUE, iseq_rtl);
    if (VARR_ADDR(char, catch_bound_pos_p)[pos])
	curr_catch_rtl_pos = rtl_pos;
    event = pos_event(pos);
    nop_p = FALSE;
    if (event.defined_p) {
	if (VARR_LENGTH(unsigned, rtl_insn_event_positions) != 0
	    && VARR_LAST(unsigned, rtl_insn_event_positions) == rtl_pos) {
	    last_event = VARR_LAST(event_t, rtl_insn_events);
	    if (combined_event_p(last_event, event, &last_event)) {
		VARR_SET(event_t, rtl_insn_events, VARR_LENGTH(event_t, rtl_insn_events) - 1, last_event);
		event.defined_p = FALSE;
	    } else {
		/* We can not attach two events to the same RTL insn.
		   It might happen for NOP or stack manipulation
		   insns, e.g. pop.  */
		APPEND_INSN_OP0(BIN(nop));
		nop_p = TRUE;
		add_event(event);
	    }
	}  else {
	    add_event(event);
	}
    }
    VARR_ADDR(size_t, new_insn_offsets)[pos] = VARR_LENGTH(VALUE, iseq_rtl);
    // line_no for  rtl_pos ????
    temp_only_p = VARR_ADDR(char, use_only_temp_result_p)[pos];
    switch (insn) {
    case BIN(getlocal):
	get_local(code[pos + 1], code[pos + 2], temp_only_p);
	break;
    case BIN(setlocal):
	set_local(code[pos + 1], code[pos + 2]);
	break;
    case BIN(getspecial):
	specialized_load(BIN(special2var), &code[pos + 1], TRUE);
	break;
    case BIN(setspecial):
	specialized_store(BIN(var2special), &code[pos + 1]);
	break;
    case BIN(getinstancevariable):
	specialized_load(BIN(ivar2var), &code[pos + 1], TRUE);
	break;
    case BIN(setinstancevariable): {
	vindex_t op;
	
	slot = pop_stack_slot();
	if (slot.mode == VAL) {
	    op = slot.u.val;
	    APPEND_INSN_OP3(BIN(val2ivar), code[pos + 1], code[pos + 2], op);
	} else {
	    op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	    op = to_var(slot, op);
	    if (op < 0)
	      APPEND_INSN_OP3(BIN(temp2ivar), code[pos + 1], code[pos + 2], op);
	    else
	      APPEND_INSN_OP3(BIN(loc2ivar), code[pos + 1], code[pos + 2], op);
	}
	break;
    }
    case BIN(getclassvariable):
	specialized_load(BIN(cvar2var), &code[pos + 1], FALSE);
	break;
    case BIN(setclassvariable):
	specialized_store(BIN(var2cvar), &code[pos + 1]);
	break;
    case BIN(getconstant): {
	vindex_t res, op;
	
	slot = pop_stack_slot();
	res = new_top_stack_temp_var();
	if (slot.mode == VAL && (slot.u.val == Qnil || slot.u.val == rb_cObject)) {
	    APPEND_INSN_OP3(BIN(const_ld_val), code[pos + 1], res, slot.u.val);
	} else {
	    op = to_var(slot, res);
	    APPEND_INSN_OP3(BIN(const2var), code[pos + 1], res, op);
	}
	break;
    }
    case BIN(setconstant): {
	vindex_t op1, op2;
	
	slot = pop_stack_slot();
	op2 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	op2 = to_var(slot, op2);
	slot = pop_stack_slot();
	op1 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	op1 = to_var(slot, op1);
	APPEND_INSN_OP3(BIN(var2const), code[pos + 1], op1, op2);
	break;
    }
    case BIN(getglobal):
	specialized_load(BIN(global2var), &code[pos + 1], FALSE);
	break;
    case BIN(setglobal):
	specialized_store(BIN(var2global), &code[pos + 1]);
	break;
    case BIN(putnil):
	putobject(Qnil, temp_only_p);
	break;
    case BIN(putself):
	if (temp_only_p) {
	    vindex_t res = new_top_stack_temp_var();

	    APPEND_INSN_OP1(BIN(self2var), res);
	} else {
	    slot.mode = SELF;
	    slot.source_insn_pos = pos;
	    push_stack_slot(slot);
	}
	break;
    case BIN(putobject):
	putobject(code[pos + 1], temp_only_p);
	break;
    case BIN(putspecialobject):
	/* Fall through: */
    case BIN(putiseq): {
      	vindex_t res;
	
	res = new_top_stack_temp_var();
	APPEND_INSN_OP2(insn == BIN(putspecialobject) ? BIN(specialobj2var) : BIN(iseq2var),
			res, code[pos + 1]);
	break;
    }
    case BIN(getblockparam):
    case BIN(getblockparamproxy): {
	vindex_t res;
	
	res = new_top_stack_temp_var();
	APPEND_INSN_OP3(insn == BIN(getblockparam) ? BIN(get_block_param) : BIN(get_block_param_proxy),
			res, code[pos + 1], code[pos + 2]);
	break;
    }
    case BIN(setblockparam): {
	vindex_t op;
	
	op = get_var();
	APPEND_INSN_OP3(BIN(set_block_param), code[pos + 1], code[pos + 2], op);
	break;
    }
    case BIN(putstring): {
	vindex_t res = new_top_stack_temp_var();

	APPEND_INSN_OP2(BIN(str2var), res, code[pos + 1]);
	break;
    }
    case BIN(concatstrings): {
	rb_num_t cnt = code[pos + 1];
	vindex_t res;
	
	put_args_on_stack(cnt);
	res = new_top_stack_temp_var();
	APPEND_INSN_OP2(BIN(concat_strings), res, cnt);
	break;
    }
    case BIN(tostring): {
	vindex_t op1, op2, res;

	get_2vars(&op1, &op2);
	assert(op2 < 0);
	res = new_top_stack_temp_var();
	APPEND_INSN_OP3(BIN(to_string), res, op1, op2);
	break;
    }
    case BIN(freezestring): {
	rb_num_t debug_info = code[pos + 1];
	vindex_t str_op;
	
	slot = pop_stack_slot();
	str_op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	to_temp(&slot, str_op, FALSE);
	push_stack_slot(slot);
	APPEND_INSN_OP2(BIN(freeze_string), str_op, debug_info);
	break;
    }
    case BIN(toregexp): {
	rb_num_t opt = code[pos + 1];
	rb_num_t cnt = code[pos + 2];
	vindex_t res;
	
	put_args_on_stack(cnt);
	res = new_top_stack_temp_var();
	APPEND_INSN_OP3(BIN(to_regexp), res, opt, cnt);
	break;
    }
    case BIN(newarray):
    case BIN(newhash): {
	rb_num_t cnt = code[pos + 1];
	vindex_t res;
	
	put_args_on_stack(cnt);
	res = new_top_stack_temp_var();
	APPEND_INSN_OP3(insn == BIN(newarray) ? BIN(make_array) : BIN(make_hash), res, res, cnt);
	break;
    }
    case BIN(duparray): {
	VALUE ary = code[pos + 1];
	vindex_t res;

	res = new_top_stack_temp_var();
	APPEND_INSN_OP2(BIN(clone_array), res, ary);
	break;
    }
    case BIN(expandarray): {
	rb_num_t num = code[pos + 1];
	rb_num_t flag = code[pos + 2];
	long i, cnt = num + (flag & 1 ? 1 : 0);
	vindex_t ary;
	
	slot = pop_stack_slot();
	ary = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	to_temp(&slot, ary, FALSE);
	push_stack_slot(slot);
	ary = get_var();
	for (i = 0; i < cnt; i++)
	    new_top_stack_temp_var();
	APPEND_INSN_OP3(BIN(spread_array), ary, num, flag);
	break;
    }
    case BIN(concatarray): {
	vindex_t op1, op2, res;

	get_2vars(&op1, &op2);
	res = new_top_stack_temp_var();
	APPEND_INSN_OP3(BIN(concat_array), res, op1, op2);
	break;
    }
    case BIN(splatarray): {
	rb_num_t flag = code[pos + 1];
	vindex_t op, res;

	op = get_var();
	res = new_top_stack_temp_var();
	APPEND_INSN_OP3(BIN(splat_array), res, op, flag);
	break;
    }
    case BIN(newrange): {
	rb_num_t flag = code[pos + 1];
	vindex_t op1, op2, res;

	get_2vars(&op1, &op2);
	res = new_top_stack_temp_var();
	APPEND_INSN_OP4(BIN(make_range), res, op1, op2, flag);
	break;
    }
    case BIN(pop):
	pop_stack_slot();
	break;
    case BIN(dup): {
	vindex_t op;
	
	slot = VARR_LAST(stack_slot, stack);
	slot.source_insn_pos = pos;
	op = - (vindex_t) VARR_LENGTH(stack_slot, stack);
	if (slot.mode == TEMP) {
	    assert(slot.u.temp == op);
	    APPEND_INSN_OP2(BIN(temp2temp), op - 1, op);
#ifndef NDEBUG
	    slot.u.temp = op - 1;
#endif
	}
	slot.source_insn_pos = pos;
	push_stack_slot(slot);
	break;
    }
    case BIN(dupn): {
	size_t len;
	rb_num_t i, n = code[pos + 1];

	len = VARR_LENGTH(stack_slot, stack);
	assert(len >= n);
	for (i = 0; i < n; i++) {
	    vindex_t opi = len - n + i;
	    
	    slot = VARR_ADDR(stack_slot, stack)[opi];
	    if (slot.mode == TEMP) {
		assert(slot.u.temp == -opi - 1);
		APPEND_INSN_OP2(BIN(temp2temp), - (vindex_t) VARR_LENGTH(stack_slot, stack) - 1, - opi - 1);
#ifndef NDEBUG
		slot.u.temp = - (vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
#endif
	    }
	    slot.source_insn_pos = pos;
	    push_stack_slot(slot);
	}
	break;
    }
    case BIN(swap): {
	stack_slot slot2;
	vindex_t op;
	
	slot2 = pop_stack_slot();
	slot = pop_stack_slot();
	op = - (vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	if (slot.mode != TEMP || slot2.mode != TEMP) {
	    if (slot2.mode == TEMP) {
		APPEND_INSN_OP2(BIN(temp2temp), op, op - 1);
#ifndef NDEBUG
		slot2.u.temp = op;
#endif
	    }
	    push_stack_slot(slot2);
	    if (slot.mode == TEMP) {
		APPEND_INSN_OP2(BIN(temp2temp), op - 1, op);
#ifndef NDEBUG
		slot.u.temp = op - 1;
#endif
	    }
	    push_stack_slot(slot);
	} else {
	    APPEND_INSN_OP2(BIN(var_swap), op, op - 1);
	    push_stack_slot(slot);
	    push_stack_slot(slot2);
	}
	break;
    }
    case BIN(reverse): {
	rb_num_t n = code[pos + 1];

	put_on_stack(n);
	APPEND_INSN_OP2(BIN(temp_reverse), n, -(vindex_t) (VARR_LENGTH(stack_slot, stack) - n) - 1);
	break;
    }
    case BIN(reput):
	/* just ignore the stack caching for now.  */
	abort();
	break;
    case BIN(topn): {
	size_t len;
	rb_num_t n = code[pos + 1];
	vindex_t opi;
	
	len = VARR_LENGTH(stack_slot, stack);
	assert(len > n);
	opi = len - n - 1;
	slot = VARR_ADDR(stack_slot, stack)[opi];
	if (slot.mode == TEMP) {
	    assert(slot.u.temp == -opi - 1);
	    APPEND_INSN_OP2(BIN(temp2temp), - (vindex_t) VARR_LENGTH(stack_slot, stack) - 1, - opi - 1);
#ifndef NDEBUG
	    slot.u.temp = - (vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
#endif
	}
	push_stack_slot(slot);
	slot.source_insn_pos = pos;
	break;
    }
    case BIN(setn): {
	size_t len;
	rb_num_t n = code[pos + 1];
	long i;
	
	assert(n > 0);
	len = VARR_LENGTH(stack_slot, stack);
	assert(len > n);
	slot = VARR_LAST(stack_slot, stack);
	assert(slot.mode != TEMP || slot.u.temp == - (vindex_t) VARR_LENGTH(stack_slot, stack));
	i = len - n - 1;
#ifndef NDEBUG
	if (slot.mode == TEMP)
	    slot.u.temp = -i - 1;
#endif
	slot.source_insn_pos = pos;
	change_stack_slot(i, slot);
	//to_temp(nth_slot, -i - 1, TRUE);
	if (slot.mode == TEMP)
	    APPEND_INSN_OP2(BIN(temp2temp), -i - 1, - (vindex_t) VARR_LENGTH(stack_slot, stack));
	break;
    }
    case BIN(adjuststack): {
	rb_num_t i, n = code[pos + 1];

	assert(VARR_LENGTH(stack_slot, stack) >= n);
	for (i = 0; i < n; i++)
	    pop_stack_slot();
	break;
    }
    case BIN(defined): {
	VALUE op;
	vindex_t res;
	enum ruby_vminsn_type insn = BIN(val_defined_p);

	slot = pop_stack_slot();
	if (slot.mode == VAL) {
	    op = slot.u.val;
	} else if (slot.mode == STR) {
	    op = slot.u.str;
	} else {
	    op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	    op = to_var(slot, op);
	    insn = BIN(defined_p);
	}
	res = new_top_stack_temp_var();
	APPEND_INSN_OP5(insn, res, op, code[pos + 1], code[pos + 2], code[pos + 3]);
	break;
    }
    case BIN(checkmatch): {
	vindex_t op1, op2, res;
	
	slot = pop_stack_slot();
	op2 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	op2 = to_var(slot, op2);
	slot = pop_stack_slot();
	op1 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	op1 = to_var(slot, op1);
	res = new_top_stack_temp_var();
	APPEND_INSN_OP4(BIN(check_match), res, op1, op2, code[pos + 1]);
	break;
    }
    case BIN(checkkeyword): {
	vindex_t res;
	
	res = new_top_stack_temp_var();
	APPEND_INSN_OP3(BIN(check_keyword), res, code[pos + 1], code[pos + 2]);
	/* ??? combining with branch */
	break;
    }
    case BIN(checktype): {
	vindex_t op, res;
	
	slot = pop_stack_slot();
	op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	op = to_var(slot, op);
	res = new_top_stack_temp_var();
	APPEND_INSN_OP3(BIN(check_type), res, op, code[pos + 1]);
	/* ??? combining with branch */
	break;
    }
    case BIN(defineclass): {
	vindex_t op1, op2, res;
	
	slot = pop_stack_slot();
	op2 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	op2 = to_var(slot, op2);
	slot = pop_stack_slot();
	op1 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	op1 = to_var(slot, op1);
	res = new_top_stack_temp_var();
	APPEND_INSN_OP6(BIN(define_class), code[pos + 1], code[pos + 2], code[pos + 3],
			op1, op2, res);
	break;
    }
    case BIN(send):
	generate_call(&code[pos + 1], code[pos + 3]);
	break;
    case BIN(opt_str_freeze):
    case BIN(opt_str_uminus): {
	VALUE str = code[pos + 1];
	CALL_INFO ci = (CALL_INFO) code[pos + 2];
	CALL_CACHE cc = (CALL_CACHE) code[pos + 3];
	struct rb_call_data *cd = get_cd(ci, cc);
	vindex_t res;

	res = new_top_stack_temp_var();
	APPEND_INSN_OP3(insn == BIN(opt_str_freeze) ? BIN(str_freeze_call) : BIN(str_uminus), cd, res, str);
	break;
    }
    case BIN(opt_newarray_max):
    case BIN(opt_newarray_min): {
	rb_num_t num = code[pos + 1];
	vindex_t start;
	enum ruby_vminsn_type res_insn;

	put_args_on_stack(num);
	start = new_top_stack_temp_var();
	res_insn = (insn == BIN(opt_newarray_max)
		    ? BIN(new_array_max) : BIN(new_array_min));
	APPEND_INSN_OP3(res_insn, start, start, num);
	break;
    }
    case BIN(opt_send_without_block):
	generate_call(&code[pos + 1], 0);
	break;
    case BIN(invokesuper): {
	CALL_INFO ci = (CALL_INFO) code[pos + 1];
	CALL_CACHE cc = (CALL_CACHE) code[pos + 2];
	VALUE block = code[pos + 3];
	struct rb_call_data *cd;
	vindex_t args_num, op;
	int stack_block_p = ci->flag & VM_CALL_ARGS_BLOCKARG;

	args_num = ci->orig_argc + (stack_block_p ? 1: 0);
	put_args_on_stack(args_num);
	slot = pop_stack_slot();
	cd = get_cd(ci, cc);
	if (slot.mode == VAL) {
	    APPEND_INSN_OP4(BIN(call_super_val), cd, cd->call_start, block, slot.u.val);
	} else {
	    op = to_var(slot, -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1);
	    APPEND_INSN_OP4(BIN(call_super), cd, cd->call_start, block, op);
	}
	push_temp_result(cd->call_start);
	break;
    }
    case BIN(invokeblock): {
	CALL_INFO ci = (CALL_INFO) code[pos + 1];
	struct rb_call_data *cd;
	vindex_t args_num;
    
	args_num = ci->orig_argc;
	put_args_on_stack(args_num);
	cd = get_cd(ci, NULL);
	APPEND_INSN_OP2(BIN(call_block), cd, cd->call_start);
	new_top_stack_temp_var();
	break;
    }
    case BIN(leave): {
	generate_leave();
	break;
    }
    case BIN(throw): {
	rb_num_t throw_state = code[pos + 1];

	/* Insns after raise are unreachable so do not pop the
	   stack.  */
	slot = VARR_LAST(stack_slot, stack);
	if (slot.mode == VAL) {
	    APPEND_INSN_OP2(BIN(raise_except_val), slot.u.val, throw_state);
	} else {
	    vindex_t op = -(vindex_t) VARR_LENGTH(stack_slot, stack);

	    op = to_var(slot, op);
	    APPEND_INSN_OP2(BIN(raise_except), op, throw_state);
	}
	break;
    }
    case BIN(jump): {
	size_t dest = code[pos + 1] + pos + stack_insn_len;
	
	if (code[dest] == BIN(leave)) {
	    generate_leave();
	} else {
#if RTL_GEN_DEBUG
	    if (rtl_gen_debug_p) {
		fprintf(stderr, "\n");
	    }
#endif
	    tune_stack(dest, BRANCH_LABEL, FALSE);
	    APPEND_INSN_OP1(BIN(goto), dest);
	    loc.next_insn_pc = VARR_LENGTH(VALUE, iseq_rtl);
	    loc.offset = 1;
	    VARR_PUSH(branch_target_loc, branch_target_locs, loc);
	}
	break;
    }
    case BIN(branchif):
    case BIN(branchunless):
    case BIN(branchnil): {
	vindex_t op;
	size_t dest;
	enum ruby_vminsn_type res_insn;

	res_insn = (insn == BIN(branchif) ? BIN(bt) : insn == BIN(branchunless) ? BIN(bf) : BIN(bnil));
	slot = pop_stack_slot();
	op = to_var(slot, -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1);
	dest = code[pos + 1] + pos + stack_insn_len;
#if RTL_GEN_DEBUG
	if (rtl_gen_debug_p) {
	    fprintf(stderr, "\n");
	}
#endif
	tune_stack(dest, BRANCH_LABEL, FALSE);
	APPEND_INSN_OP2(res_insn, dest, op);
	loc.offset = 2;
	loc.next_insn_pc = VARR_LENGTH(VALUE, iseq_rtl);
	VARR_PUSH(branch_target_loc, branch_target_locs, loc);
	break;
    }
    case BIN(getinlinecache): {
	vindex_t res;
	VALUE next_insn, next_next_insn;
	size_t next_insn_len, next_next_insn_len;
	
	res = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	next_insn = code[pos + stack_insn_len];
	next_insn_len = insn_len(next_insn);
	if (VARR_ADDR(char, pos_label_type)[pos + stack_insn_len] == NO_LABEL
	    && next_insn == BIN(getconstant)
	    && (next_next_insn = code[pos + stack_insn_len + next_insn_len]) == BIN(setinlinecache)
	    && VARR_ADDR(char, pos_label_type)[pos + stack_insn_len + next_insn_len] == NO_LABEL
	    && (next_next_insn_len = insn_len(next_next_insn)) + next_insn_len == code[pos + 1]
	    && code[pos + 2] == code[pos + stack_insn_len + next_insn_len + 1] /* ic */) {
	    APPEND_INSN_OP4(BIN(const_cached_val_ld), res, Qnil, code[pos + stack_insn_len + 1] /* id */, code[pos + 2] /* ic */);
	    stack_insn_len += next_insn_len + next_next_insn_len;
	} else {
	    APPEND_INSN_OP3(BIN(get_inline_cache), code[pos + 1] + pos + stack_insn_len, res, code[pos + 2]);
	    loc.next_insn_pc = VARR_LENGTH(VALUE, iseq_rtl);
	    loc.offset = 3;
	    VARR_PUSH(branch_target_loc, branch_target_locs, loc);
	}
	push_temp_result(res);
	break;
    }
    case BIN(setinlinecache): {
	vindex_t op;
	
	slot = pop_stack_slot();
	op = to_var(slot, -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1);
	APPEND_INSN_OP2(BIN(set_inline_cache), op, code[pos + 1]);
	push_temp_result(op);
	break;
    }
    case BIN(once): {
	vindex_t res = new_top_stack_temp_var();
	
	APPEND_INSN_OP3(BIN(run_once), res, code[pos + 1], code[pos + 2]);
	break;
    }
    case BIN(opt_case_dispatch): {
	CDHASH hash = code[pos + 1];
	vindex_t op;
	REL_PC incr = pos + stack_insn_len;
	struct hash_label_transform_arg arg;

	hash = rb_hash_dup(hash);
	iseq_add_mark_object_compile_time(curr_iseq, hash);
	arg.hash = hash;
	arg.map_p = FALSE;
	arg.decr = -incr;
	change_hash_values(hash, &arg);
	slot = pop_stack_slot();
	op = to_var(slot, -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1);
	APPEND_INSN_OP3(BIN(case_dispatch), op, hash, code[pos + 2] + incr);
	loc.next_insn_pc = VARR_LENGTH(VALUE, iseq_rtl);
	loc.offset = 1;
	VARR_PUSH(branch_target_loc, branch_target_locs, loc);
	loc.next_insn_pc = VARR_LENGTH(VALUE, iseq_rtl);
	loc.offset = 0; /* it means the hash table */
	VARR_PUSH(branch_target_loc, branch_target_locs, loc);
	break;
    }
    case BIN(opt_plus):
	generate_bin_op(&code[pos + 1], BIN(plus));
	break;
    case BIN(opt_minus):
	generate_bin_op(&code[pos + 1], BIN(minus));
	break;
    case BIN(opt_mult):
	generate_bin_op(&code[pos + 1], BIN(mult));
	break;
    case BIN(opt_div):
	generate_bin_op(&code[pos + 1], BIN(div));
	break;
    case BIN(opt_or):
	generate_bin_op(&code[pos + 1], BIN(or));
	break;
    case BIN(opt_and):
	generate_bin_op(&code[pos + 1], BIN(and));
	break;
    case BIN(opt_mod):
	generate_bin_op(&code[pos + 1], BIN(mod));
	break;
    case BIN(opt_eq):
	stack_insn_len = generate_rel_op(code, pos, BIN(eq));
	break;
    case BIN(opt_neq):
	stack_insn_len = generate_rel_op(code, pos, BIN(ne));
	break;
    case BIN(opt_lt):
	stack_insn_len = generate_rel_op(code, pos, BIN(lt));
	break;
    case BIN(opt_le):
	stack_insn_len = generate_rel_op(code, pos, BIN(le));
	break;
    case BIN(opt_gt):
	stack_insn_len = generate_rel_op(code, pos, BIN(gt));
	break;
    case BIN(opt_ge):
	stack_insn_len = generate_rel_op(code, pos, BIN(ge));
	break;
    case BIN(opt_ltlt):
	generate_bin_op(&code[pos + 1], BIN(ltlt));
	break;
    case BIN(opt_aref):
	generate_bin_op(&code[pos + 1], BIN(ind));
	break;
    case BIN(opt_aset):
	generate_aset_op(&code[pos + 1], Qnil);
	break;
    case BIN(opt_aset_with):
	generate_aset_op(&code[pos + 2], code[pos + 1]);
	break;
    case BIN(opt_aref_with):
	slot.mode = STR;
	slot.source_insn_pos = pos;
	slot.u.str = code[pos + 1];
	push_stack_slot(slot);
	generate_bin_op(&code[pos + 2], BIN(ind));
	break;
    case BIN(opt_length):
	generate_unary_op(&code[pos + 1], BIN(length));
	break;
    case BIN(opt_size):
	generate_unary_op(&code[pos + 1], BIN(size));
	break;
    case BIN(opt_empty_p):
	generate_unary_op(&code[pos + 1], BIN(empty_p));
	break;
    case BIN(opt_succ):
	generate_unary_op(&code[pos + 1], BIN(succ));
	break;
    case BIN(opt_not):
	generate_unary_op(&code[pos + 1], BIN(not));
	break;
    case BIN(intern): {
	vindex_t op, res;

	slot = pop_stack_slot();
	op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	op = to_var(slot, op);
	res = new_top_stack_temp_var();
	APPEND_INSN_OP2(BIN(str2sym), res, op);
	break;
    }
    case BIN(opt_regexpmatch1): {
	VALUE regexp = code[pos + 1];
	vindex_t res, op;
	
	slot = pop_stack_slot();
	op = to_var(slot, -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1);
	res = new_top_stack_temp_var();
	APPEND_INSN_OP3(BIN(regexp_match1), res, regexp, op);
    	break;
    }
    case BIN(opt_regexpmatch2): {
	vindex_t op, op2, res;
	stack_slot slot2;
	CALL_INFO ci = (CALL_INFO) code[pos + 1];
	CALL_CACHE cc = (CALL_CACHE) code[pos + 2];
	struct rb_call_data *cd;

	slot2 = pop_stack_slot();
	slot = pop_stack_slot();
	res = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	cd = get_cd(ci, cc);
	op = to_var(slot, res);
	op2 = to_var(slot2, res - 1);
	APPEND_INSN_OP4(BIN(regexp_match2), cd, res, op, op2);
	push_temp_result(res);
	break;
    }
    case BIN(opt_call_c_function): {
	vindex_t args_num = curr_iseq->body->param.size + 1;

	put_on_stack(args_num);
	APPEND_INSN_OP2(BIN(call_c_func), code[pos + 1], args_num);
	break;
    }
    case BIN(bitblt):
    case BIN(answer):
	assert(FALSE);
	break;
    case BIN(tracecoverage):
	APPEND_INSN_OP2(BIN(trace_coverage), code[pos + 1], code[pos + 2]);
	break;
    case BIN(nop):
	if (! nop_p && VARR_ADDR(char, catch_bound_pos_p)[pos])
	    APPEND_INSN_OP0(BIN(nop));
	break;
    case BIN(getlocal_WC_0):
	get_local(code[pos + 1], 0, temp_only_p);
	break;
    case BIN(getlocal_WC_1):
	get_local(code[pos + 1], 1, temp_only_p);
	break;
    case BIN(setlocal_WC_0):
	set_local(code[pos + 1], 0);
	break;
    case BIN(setlocal_WC_1):
	set_local(code[pos + 1], 1);
	break;
    case BIN(putobject_INT2FIX_0_):
	putobject(INT2FIX(0), temp_only_p);
	break;
    case BIN(putobject_INT2FIX_1_):
	putobject(INT2FIX(1), temp_only_p);
	break;
    default:
	assert(FALSE);
	break;
    }
#if RTL_GEN_DEBUG
    if (rtl_gen_debug_p) {
	fprintf(stderr, " After ");
	print_stack();
    }
#endif
    curr_source_insn_pos += stack_insn_len;
}

/* Generate RTL insns from stack insns of the current iseq.  */
static void
translate(void){
    const VALUE *code = curr_iseq->body->iseq_encoded;
    int line_no = -1;
    size_t pos, i, size;
    enum ruby_vminsn_type insn, prev_insn;
    
    VARR_TRUNC(branch_target_loc, branch_target_locs, 0);
    VARR_TRUNC(size_t, new_insn_offsets, 0);
    trunc_stack(0);
    VARR_TRUNC(VALUE, iseq_rtl, 0);
    VARR_TRUNC(int, source_pos2line, 0);
    VARR_TRUNC(unsigned, rtl_insn_event_positions, 0);
    VARR_TRUNC(event_t, rtl_insn_events, 0);
    size = curr_iseq->body->iseq_size;
    for (pos = i = 0; pos < size; pos++) {
	VARR_PUSH(size_t, new_insn_offsets, 0);
	if (i < curr_iseq->body->insns_info.size && pos == curr_iseq->body->insns_info.positions[i]) {
	    line_no = curr_iseq->body->insns_info.body[i].line_no;
	    i++;
	}
	VARR_PUSH(int, source_pos2line, line_no);
    }
#if RTL_GEN_DEBUG
    if (rtl_gen_debug_p) {
	fprintf(stderr, "++++++++++++++Translating\n");
    }
#endif
    unreachable_code_p = FALSE;
    for (prev_insn = BIN(nop), curr_source_insn_pos = 0; curr_source_insn_pos < size; prev_insn = insn) {
	insn = code[curr_source_insn_pos];
	translate_stack_insn(code, prev_insn);
    }
    trunc_stack(0);
 }

/* Create an insn info table of the generated RTL of the current iseq.
   Return FALSE if we failed to do this.  */
static int
create_rtl_insn_info_table(void) {
    size_t i, nel, pos, next_insn_pos, rtl_insn_info_size;
    int no_event_next_insn_p;
    struct iseq_insn_info_entry *entries;
    unsigned int *positions;
    event_t event;
#if 0
    {
	size_t len, size;
	
	fprintf (stderr, "------------------\n");
	for (pos = 0; pos < curr_iseq->body->iseq_size; pos += len) {
	    enum ruby_vminsn_type insn = curr_iseq->body->iseq_encoded[pos];
	    
	    fprintf (stderr, "%lu -> %lu\n", pos, VARR_ADDR(size_t, new_insn_offsets)[pos]);
	    len = insn_len (insn);
	}
	size = curr_iseq->body->insns_info.size;
	for (i = 0; i < size; i++) {
	    fprintf (stderr, "pos=%u:lno=%d,events=0x%x\n", curr_iseq->body->insns_info.positions[i],
		     curr_iseq->body->insns_info.body[i].line_no, curr_iseq->body->insns_info.body[i].events);
	}
    }
#endif
    rtl_insn_info_size = VARR_LENGTH(unsigned, rtl_insn_event_positions);
    assert(rtl_insn_info_size == VARR_LENGTH(event_t, rtl_insn_events));
    if (rtl_insn_info_size != 0 && VARR_GET(unsigned, rtl_insn_event_positions, 0) != 0)
	/* The first RTL insn info position can be non-zero as we can
	   shift its position a bit skipping non-throwing RTL insns.
	   For correct tracing, we need info for zero position insns.
	   Shift the position back.  */
	VARR_SET(unsigned, rtl_insn_event_positions, 0, 0);
    /* Calculate additional entries to switch off line events: */
    for (i = nel = 0; i < rtl_insn_info_size; i++) {
	pos = VARR_GET(unsigned, rtl_insn_event_positions, i);
	event = VARR_GET(event_t, rtl_insn_events, i);
	next_insn_pos = pos + insn_len(curr_iseq->body->rtl_encoded[pos]);
	no_event_next_insn_p = (i + 1 < rtl_insn_info_size
				&& next_insn_pos != VARR_GET(unsigned, rtl_insn_event_positions, i + 1));
	if ((event.info_entry.events & RUBY_EVENT_LINE)
	    && (no_event_next_insn_p
		|| (i + 1 == rtl_insn_info_size && next_insn_pos < curr_iseq->body->rtl_size)))
	    nel++; /* for switch off line event */
    }
    curr_iseq->body->rtl_insns_info.size = rtl_insn_info_size + nel;
    if ((curr_iseq->body->rtl_insns_info.body
	 = entries = ALLOC_N(struct iseq_insn_info_entry, rtl_insn_info_size + nel)) == NULL)
	return FALSE;
    if ((curr_iseq->body->rtl_insns_info.positions
	 = positions = ALLOC_N(unsigned, rtl_insn_info_size + nel)) == NULL) {
	free(entries);
	return FALSE;
    }
    /* Copy entries and add new entries to switch off line events: */
    for (i = nel = 0; i < rtl_insn_info_size; i++) {
	positions[i + nel] = pos = VARR_GET(unsigned, rtl_insn_event_positions, i);
	entries[i + nel] = (event = VARR_GET(event_t, rtl_insn_events, i)).info_entry;
	next_insn_pos = pos + insn_len(curr_iseq->body->rtl_encoded[pos]);
	no_event_next_insn_p = (i + 1 < rtl_insn_info_size
				&& next_insn_pos != VARR_GET(unsigned, rtl_insn_event_positions, i + 1));
	if ((event.info_entry.events & RUBY_EVENT_LINE)
	    && (no_event_next_insn_p
		|| (i + 1 == rtl_insn_info_size && next_insn_pos < curr_iseq->body->rtl_size))) {
	    nel++; /* for switch off line event */
	    positions[i + nel] = next_insn_pos;
	    entries[i + nel].line_no = entries[i + nel - 1].line_no;
	    entries[i + nel].events = 0;
	}
	if (entries[i + nel].events & RUBY_EVENT_LINE) {
	    pos = positions[i + nel];
	    pos += insn_len(curr_iseq->body->rtl_encoded[pos]);
	    if ((i + 1 < rtl_insn_info_size && VARR_GET(unsigned, rtl_insn_event_positions, i + 1) != pos)
		|| (i + 1 == rtl_insn_info_size && pos < curr_iseq->body->rtl_size)) {
		nel++;
		positions[i + nel] = pos;
		entries[i + nel].line_no = entries[i + nel - 1].line_no;
		entries[i + nel].events = 0;
	    }
	}
    }
    assert(curr_iseq->body->rtl_insns_info.size == rtl_insn_info_size  + nel);
#if 0
    for (i = 0; i < rtl_insn_info_size + nel; i++)
	fprintf (stderr, "pos=%u:lno=%d,events=0x%x\n", curr_iseq->body->rtl_insns_info.positions[i],
		 curr_iseq->body->rtl_insns_info.body[i].line_no, curr_iseq->body->rtl_insns_info.body[i].events);
#endif
    return TRUE;
}

/* Create a catch table of the generated RTL of the current iseq.
   Return FALSE if we failed to do this.  Set up except_p for the iseq
   too.  */
static int
create_rtl_catch_table(void) {
    size_t i, size;
    const struct iseq_catch_table *table;
    struct iseq_catch_table *rtl_table;
    const struct iseq_catch_table_entry *entries;
    struct iseq_catch_table_entry *rtl_entries;
    const size_t *addr;
    
    table = curr_iseq->body->catch_table;
    if (table == NULL)
	return TRUE;
    size = table->size;
    curr_iseq->body->rtl_catch_table = rtl_table = xmalloc(iseq_catch_table_bytes(size));
    if (rtl_table == NULL)
	return FALSE;
    entries = table->entries;
    rtl_table->size = size;
    rtl_entries = rtl_table->entries;
    addr = VARR_ADDR(size_t, new_insn_offsets);
    for (i = 0; i < size; i++) {
	rtl_entries[i] = entries[i];
	/* Currently there might be garbage in the entry.  So don't
	   translate it.  */
	if (entries[i].start >= curr_iseq->body->iseq_size
	    || entries[i].end >= curr_iseq->body->iseq_size
	    || entries[i].cont >= curr_iseq->body->iseq_size)
	    continue;
	rtl_entries[i].start = addr[rtl_entries[i].start];
	rtl_entries[i].end = addr[rtl_entries[i].end];
	rtl_entries[i].cont = addr[rtl_entries[i].cont];
    }
    return TRUE;
}

/* Create call data for RTL part of the current iseq.  Return FALSE if
   we failed to do this.  */
static int
create_cd_data(void) {
    struct rb_call_data *call_data_addr;

    call_data_addr
      = ((struct rb_call_data *)
	 ruby_xmalloc(sizeof(struct rb_call_data) * curr_iseq->body->ci_size +
		      sizeof(struct rb_call_data_with_kwarg) * curr_iseq->body->ci_kw_size));
    if (call_data_addr == NULL)
	return FALSE;
    curr_iseq->body->cd_size = curr_iseq->body->ci_size;
    curr_iseq->body->cd_entries = call_data_addr;
    curr_iseq->body->cd_kw_size = curr_iseq->body->ci_kw_size;
    return TRUE;
}

/* Modify optional param code offsets for generated RTL of the current iseq.  */
static void
setup_opt_table(void) {
    int i, opt_num = curr_iseq->body->param.opt_num;
    VALUE *opt_table = curr_iseq->body->param.opt_table;
    
    if (opt_num == 0)
	return;
    for (i = 0; i <= opt_num; i++)
	opt_table[i] = VARR_ADDR(size_t, new_insn_offsets)[opt_table[i]];
}

/* Entry function to generate RTL parts of ISEQ from stack insns.
   Return FALSE if we failed to generate RTL.  */
int
rtl_gen(rb_iseq_t *iseq) {
    size_t i, size, pos;
    branch_target_loc loc;
    REL_PC dest, new_dest;
    
    if (RUBY_SETJMP(rtl_gen_jump_buf) != 0)
	return FALSE;
    curr_iseq = iseq;
    if (! create_cd_data())
	return FALSE;
    initialize_loc_stack_count();
#if RTL_GEN_DEBUG
    if (rtl_gen_debug_p) {
	fprintf(stderr, "------------%s@%s------------\n",
		RSTRING_PTR(curr_iseq->body->location.label), RSTRING_PTR(rb_iseq_path(curr_iseq)));
    }
#endif
    VARR_TRUNC(long, insn_info_entry_ind, 0);
    size = curr_iseq->body->iseq_size;
    /* Initiate INSN_INFO_ENTRY_IND: */
    for (pos = i = 0; pos < size; pos++) {
	VARR_PUSH(long, insn_info_entry_ind, -1);
	if (i < curr_iseq->body->insns_info.size && pos == curr_iseq->body->insns_info.positions[i]) {
	    /* Ignore positions to switch off line events -- we will add them later.  */
	    if (curr_iseq->body->insns_info.body[i].events != 0
		|| i == 0
		|| (curr_iseq->body->insns_info.body[i - 1].events & RUBY_EVENT_LINE) == 0
		|| curr_iseq->body->insns_info.body[i - 1].line_no != curr_iseq->body->insns_info.body[i].line_no)
		VARR_SET(long, insn_info_entry_ind, pos, i);
	    i++;
	}
    }
    /* First pass on stack insns:  */
    find_stack_values_on_labels();
    /* Second pass on stack insns: */
    curr_catch_rtl_pos = SIZE_MAX;
    translate();
    curr_iseq->body->rtl_encoded = ALLOC_N(VALUE, VARR_LENGTH(VALUE, iseq_rtl));
    curr_iseq->body->rtl_size = VARR_LENGTH(VALUE, iseq_rtl);
    curr_iseq->body->temp_vars_num = max_stack_depth;
    if (curr_iseq->body->rtl_encoded == NULL)
      return FALSE;
    MEMMOVE(curr_iseq->body->rtl_encoded, VARR_ADDR(VALUE, iseq_rtl), VALUE, curr_iseq->body->rtl_size);
    /* Change branch destinations:  */
    for (i = 0; i < VARR_LENGTH(branch_target_loc, branch_target_locs); i++) {
	loc = VARR_ADDR(branch_target_loc, branch_target_locs)[i];
	if (loc.offset == 0) {
	    CDHASH hash = curr_iseq->body->rtl_encoded[loc.next_insn_pc - 2];
	    struct hash_label_transform_arg arg;

	    arg.hash = hash;
	    arg.map_p = TRUE;
	    arg.decr = loc.next_insn_pc;
	    RBASIC(hash)->flags &= ~(VALUE) RUBY_FL_FREEZE;
	    change_hash_values(hash, &arg);
	    RB_OBJ_FREEZE(hash);
	} else {
	    dest = curr_iseq->body->rtl_encoded[loc.next_insn_pc - loc.offset];
	    new_dest = VARR_ADDR(size_t, new_insn_offsets)[dest];
	    curr_iseq->body->rtl_encoded[loc.next_insn_pc - loc.offset] = new_dest - loc.next_insn_pc;
	}
    }
    setup_opt_table();
    if (! create_rtl_insn_info_table())
	return FALSE;
    return create_rtl_catch_table();
}

/* Initiate stack insns to RTL generator.  Return FALSE if we failed
   to do this.  */
int
rtl_gen_init(void) {
#if RTL_GEN_DEBUG
    rtl_gen_debug_p = getenv("MRI_RTL_GEN_DEBUG") != NULL;
#endif
    if (RUBY_SETJMP(rtl_gen_jump_buf) != 0)
	return FALSE;
    VARR_CREATE(branch_target_loc, branch_target_locs, 0);
    VARR_CREATE(size_t, new_insn_offsets, 0);
    VARR_CREATE(stack_slot, stack, 0);
    VARR_CREATE(stack_slot, saved_stack_slots, 0);
    VARR_CREATE(size_t, label_start_stack_slot, 0);
    VARR_CREATE(size_t, loc_stack_count, 0);
    VARR_CREATE(VALUE, iseq_rtl, 0);
    VARR_CREATE(size_t, pos_stack_free, 0);
    VARR_CREATE(size_t, label_pos_stack, 0);
    VARR_CREATE(char, pos_label_type, 0);
    VARR_CREATE(char, label_processed_p, 0);
    VARR_CREATE(char, catch_bound_pos_p, 0);
    VARR_CREATE(char, use_only_temp_result_p, 0);
    VARR_CREATE(int, source_pos2line, 0);
    VARR_CREATE(unsigned, rtl_insn_event_positions, 0);
    VARR_CREATE(event_t, rtl_insn_events, 0);
    VARR_CREATE(long, insn_info_entry_ind, 0);
    return TRUE;
}

/* Finish stack insns to RTL generator.  */
void
rtl_gen_finish(void) {
    VARR_DESTROY(branch_target_loc, branch_target_locs);
    VARR_DESTROY(size_t, new_insn_offsets);
    VARR_DESTROY(stack_slot, stack);
    VARR_DESTROY(stack_slot, saved_stack_slots);
    VARR_DESTROY(size_t, label_start_stack_slot);
    VARR_DESTROY(size_t, loc_stack_count);
    VARR_DESTROY(VALUE, iseq_rtl);
    VARR_DESTROY(size_t, pos_stack_free);
    VARR_DESTROY(size_t, label_pos_stack);
    VARR_DESTROY(char, pos_label_type);
    VARR_DESTROY(char, label_processed_p);
    VARR_DESTROY(char, catch_bound_pos_p);
    VARR_DESTROY(char, use_only_temp_result_p);
    VARR_DESTROY(int, source_pos2line);
    VARR_DESTROY(unsigned, rtl_insn_event_positions);
    VARR_DESTROY(event_t, rtl_insn_events);
    VARR_DESTROY(long, insn_info_entry_ind);
}
