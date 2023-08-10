/*
    Copyright 2018, 2020, 2021, 2022, 2023 Joel Svensson    svenssonjoel@yahoo.se

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <lbm_memory.h>
#include <lbm_types.h>
#include "symrepr.h"
#include "heap.h"
#include "env.h"
#include "eval_cps.h"
#include "stack.h"
#include "fundamental.h"
#include "extensions.h"
#include "exp_kind.h"
#include "tokpar.h"
#include "qq_expand.h"
#include "lbm_variables.h"
#include "lbm_channel.h"
#include "print.h"
#include "platform_mutex.h"
#include "lbm_flat_value.h"
#include "lbm_flags.h"

#ifdef VISUALIZE_HEAP
#include "heap_vis.h"
#endif

#include <setjmp.h>

static jmp_buf error_jmp_buf;

#define S_TO_US(X) (lbm_uint)((X) * 1000000)

#define DEC_CONTINUATION(x) (((x) & ~LBM_CONTINUATION_INTERNAL) >> LBM_ADDRESS_SHIFT)
#define IS_CONTINUATION(x) (((x) & LBM_CONTINUATION_INTERNAL) == LBM_CONTINUATION_INTERNAL)
#define CONTINUATION(x) (((x) << LBM_ADDRESS_SHIFT) | LBM_CONTINUATION_INTERNAL)

#define DONE                  CONTINUATION(0)
#define SET_GLOBAL_ENV        CONTINUATION(1)
#define BIND_TO_KEY_REST      CONTINUATION(2)
#define IF                    CONTINUATION(3)
#define PROGN_REST            CONTINUATION(4)
#define APPLICATION_ARGS      CONTINUATION(5)
#define AND                   CONTINUATION(6)
#define OR                    CONTINUATION(7)
#define WAIT                  CONTINUATION(8)
#define MATCH                 CONTINUATION(9)
#define MATCH_MANY            CONTINUATION(10)
#define APPLICATION_START     CONTINUATION(11)
#define EVAL_R                CONTINUATION(12)
#define SET_VARIABLE          CONTINUATION(13)
#define RESUME                CONTINUATION(14)
#define CLOSURE_ARGS          CONTINUATION(15)
#define EXIT_ATOMIC           CONTINUATION(16)
#define READ_NEXT_TOKEN       CONTINUATION(17)
#define READ_APPEND_CONTINUE  CONTINUATION(18)
#define READ_EVAL_CONTINUE    CONTINUATION(19)
#define READ_EXPECT_CLOSEPAR  CONTINUATION(20)
#define READ_DOT_TERMINATE    CONTINUATION(21)
#define READ_DONE             CONTINUATION(22)
#define READ_QUOTE_RESULT     CONTINUATION(23)
#define READ_COMMAAT_RESULT   CONTINUATION(24)
#define READ_COMMA_RESULT     CONTINUATION(25)
#define READ_START_ARRAY      CONTINUATION(26)
#define READ_APPEND_ARRAY     CONTINUATION(27)
#define MAP_FIRST             CONTINUATION(28)
#define MAP_REST              CONTINUATION(29)
#define MATCH_GUARD           CONTINUATION(30)
#define TERMINATE             CONTINUATION(31)
#define PROGN_VAR             CONTINUATION(32)
#define SETQ                  CONTINUATION(33)
#define MOVE_TO_FLASH         CONTINUATION(34)
#define MOVE_VAL_TO_FLASH_DISPATCH CONTINUATION(35)
#define MOVE_LIST_TO_FLASH    CONTINUATION(36)
#define CLOSE_LIST_IN_FLASH   CONTINUATION(37)
#define QQ_EXPAND_START       CONTINUATION(38)
#define QQ_EXPAND             CONTINUATION(39)
#define QQ_APPEND             CONTINUATION(40)
#define QQ_EXPAND_LIST        CONTINUATION(41)
#define QQ_LIST               CONTINUATION(42)
#define KILL                  CONTINUATION(43)
#define NUM_CONTINUATIONS     44

#define FM_NEED_GC       -1
#define FM_NO_MATCH      -2
#define FM_PATTERN_ERROR -3

#define BL_OK             0
#define BL_NO_MEMORY     -1
#define BL_INCORRECT_KEY -2

#define FB_OK             0
#define FB_TYPE_ERROR    -1

const char* lbm_error_str_parse_eof = "End of parse stream.";
const char* lbm_error_str_parse_token = "Malformed token.";
const char* lbm_error_str_parse_dot = "Incorrect usage of '.'.";
const char* lbm_error_str_parse_close = "Expected closing parenthesis.";
const char* lbm_error_str_num_args = "Incorrect number of arguments.";
const char* lbm_error_str_forbidden_in_atomic = "Operation is forbidden in an atomic block.";
const char* lbm_error_str_no_number = "Argument(s) must be a number.";
const char* lbm_error_str_not_a_boolean = "Argument must be t or nil (true or false).";
const char* lbm_error_str_incorrect_arg = "Incorrect argument.";
const char* lbm_error_str_var_outside_progn = "Usage of var outside of progn.";
const char* lbm_error_str_flash_not_possible = "Value cannot be written to flash.";
const char* lbm_error_str_flash_error = "Error writing to flash";
const char* lbm_error_str_flash_full = "Flash memory is full";


#define WITH_GC(y, x)                           \
  (y) = (x);                                    \
  if (lbm_is_symbol_merror((y))) {              \
    gc();                                       \
    (y) = (x);                                  \
    if (lbm_is_symbol_merror((y))) {            \
      error_ctx(ENC_SYM_MERROR);                \
    }                                           \
    /* continue executing statements below */   \
  }
#define WITH_GC_RMBR_1(y, x, r)                 \
  (y) = (x);                                    \
  if (lbm_is_symbol_merror((y))) {              \
    add_roots_1(r);                             \
    lbm_gc_mark_phase();                        \
    gc();                                       \
    (y) = (x);                                  \
    if (lbm_is_symbol_merror((y))) {            \
      error_ctx(ENC_SYM_MERROR);                \
    }                                           \
    /* continue executing statements below */   \
  }

typedef struct {
  eval_context_t *first;
  eval_context_t *last;
} eval_context_queue_t;

static int gc(void);
void error_ctx(lbm_value);
static void enqueue_ctx(eval_context_queue_t *q, eval_context_t *ctx);
static bool mailbox_add_mail(eval_context_t *ctx, lbm_value mail);

// The currently executing context.
eval_context_t *ctx_running = NULL;
volatile bool  lbm_system_sleeping = false;

static volatile bool gc_requested = false;
void lbm_request_gc(void) {
  gc_requested = true;
}

/*
   On ChibiOs the CH_CFG_ST_FREQUENCY setting in chconf.h sets the
   resolution of the timer used for sleep operations.  If this is set
   to 10KHz the resolution is 100us.

   The CH_CFG_ST_TIMEDELTA specifies the minimum number of ticks that
   can be safely specified in a timeout directive (wonder if that
   means sleep-period). The timedelta is set to 2.

   If I have understood these correctly it means that the minimum
   sleep duration possible is 2 * 100us = 200us.
*/

#define EVAL_CPS_DEFAULT_STACK_SIZE 256
#define EVAL_CPS_MIN_SLEEP 200
#define EVAL_STEPS_QUOTA   10

static volatile uint32_t eval_steps_refill = EVAL_STEPS_QUOTA;
static uint32_t eval_steps_quota = EVAL_STEPS_QUOTA;

void lbm_set_eval_step_quota(uint32_t quota) {
  eval_steps_refill = quota;
}

static uint32_t          eval_cps_run_state = EVAL_CPS_STATE_RUNNING;
static volatile uint32_t eval_cps_next_state = EVAL_CPS_STATE_RUNNING;
static volatile uint32_t eval_cps_next_state_arg = 0;
static volatile bool     eval_cps_state_changed = false;

static void usleep_nonsense(uint32_t us) {
  (void) us;
}

static bool dynamic_load_nonsense(const char *sym, const char **code) {
  (void) sym;
  (void) code;
  return false;
}

static uint32_t timestamp_nonsense(void) {
  return 0;
}

static int printf_nonsense(const char *fmt, ...) {
  (void) fmt;
  return 0;
}

static void ctx_done_nonsense(eval_context_t *ctx) {
  (void) ctx;
}

static void (*usleep_callback)(uint32_t) = usleep_nonsense;
static uint32_t (*timestamp_us_callback)(void) = timestamp_nonsense;
static void (*ctx_done_callback)(eval_context_t *) = ctx_done_nonsense;
static int (*printf_callback)(const char *, ...) = printf_nonsense;
static bool (*dynamic_load_callback)(const char *, const char **) = dynamic_load_nonsense;

void lbm_set_usleep_callback(void (*fptr)(uint32_t)) {
  if (fptr == NULL) usleep_callback = usleep_nonsense;
  else usleep_callback = fptr;
}

void lbm_set_timestamp_us_callback(uint32_t (*fptr)(void)) {
  if (fptr == NULL) timestamp_us_callback = timestamp_nonsense;
  else timestamp_us_callback = fptr;
}

void lbm_set_ctx_done_callback(void (*fptr)(eval_context_t *)) {
  if (fptr == NULL) ctx_done_callback = ctx_done_nonsense;
  else ctx_done_callback = fptr;
}

void lbm_set_printf_callback(int (*fptr)(const char*, ...)){
  if (fptr == NULL) printf_callback = printf_nonsense;
  else printf_callback = fptr;
}

void lbm_set_dynamic_load_callback(bool (*fptr)(const char *, const char **)) {
  if (fptr == NULL) dynamic_load_callback = dynamic_load_nonsense;
  else  dynamic_load_callback = fptr;
}

static volatile lbm_event_t *lbm_events = NULL;
static unsigned int lbm_events_head = 0;
static unsigned int lbm_events_tail = 0;
static unsigned int lbm_events_max  = 0;
static bool         lbm_events_full = false;
static mutex_t      lbm_events_mutex;
static bool         lbm_events_mutex_initialized = false;
static volatile lbm_cid  lbm_event_handler_pid = -1;

lbm_cid lbm_get_event_handler_pid(void) {
  return lbm_event_handler_pid;
}

void lbm_set_event_handler_pid(lbm_cid pid) {
  lbm_event_handler_pid = pid;
}

static bool event_internal(lbm_event_type_t event_type, lbm_uint parameter, lbm_uint buf_ptr, lbm_uint buf_len) {
  bool r = false;
  if (lbm_events) {
    mutex_lock(&lbm_events_mutex);
    if (!lbm_events_full) {
      lbm_event_t event;
      event.type = event_type;
      event.parameter = parameter;
      event.buf_ptr = buf_ptr;
      event.buf_len = buf_len;
      lbm_events[lbm_events_head] = event;
      lbm_events_head = (lbm_events_head + 1) % lbm_events_max;
      lbm_events_full = lbm_events_head == lbm_events_tail;
      r = true;
    }
    mutex_unlock(&lbm_events_mutex);
  }
  return r;
}

bool lbm_event_unboxed(lbm_value unboxed) {
  lbm_uint t = lbm_type_of(unboxed);
  if (t == LBM_TYPE_SYMBOL ||
      t == LBM_TYPE_I ||
      t == LBM_TYPE_U ||
      t == LBM_TYPE_CHAR) {
    if (lbm_event_handler_pid > 0) {
      return event_internal(LBM_EVENT_FOR_HANDLER, 0, (lbm_uint)unboxed, 0);
    }
  }
  return false;
}

bool lbm_event(lbm_flat_value_t *fv) {
  if (lbm_event_handler_pid > 0) {
    return event_internal(LBM_EVENT_FOR_HANDLER, 0, (lbm_uint)fv->buf, fv->buf_size);
  }
  return false;
}

bool lbm_event_handler_exists(void) {
  return(lbm_event_handler_pid > 0);
}

static bool lbm_event_pop(lbm_event_t *event) {
  mutex_lock(&lbm_events_mutex);
  if (lbm_events_head == lbm_events_tail && !lbm_events_full) {
    mutex_unlock(&lbm_events_mutex);
    return false;
  }
  *event = lbm_events[lbm_events_tail];
  lbm_events_tail = (lbm_events_tail + 1) % lbm_events_max;
  lbm_events_full = false;
  mutex_unlock(&lbm_events_mutex);
  return true;
}

static bool              eval_running = false;
static volatile bool     blocking_extension = false;
static mutex_t           blocking_extension_mutex;
static bool              blocking_extension_mutex_initialized = false;
static lbm_uint          blocking_extension_timeout_us = 0;
static bool              blocking_extension_timeout = false;

static uint32_t          is_atomic = 0;
static volatile uint32_t wait_for = 0; // wake-up mask

void lbm_trigger_flags(uint32_t wait_for_flags) {
  wait_for |= wait_for_flags;
}

/* Process queues */
static eval_context_queue_t blocked  = {NULL, NULL};
static eval_context_queue_t queue    = {NULL, NULL};

/* one mutex for all queue operations */
mutex_t qmutex;
bool    qmutex_initialized = false;


// MODES
static volatile bool lbm_verbose = false;

void lbm_toggle_verbose(void) {
  lbm_verbose = !lbm_verbose;
}

void lbm_set_verbose(bool verbose) {
  lbm_verbose = verbose;
}

lbm_cid lbm_get_current_cid(void) {
  if (ctx_running)
    return ctx_running->id;
  else
    return -1;
}

eval_context_t *lbm_get_current_context(void) {
  return ctx_running;
}

/****************************************************/
/* Utilities used locally in this file              */

static void add_roots_1(lbm_value r1) {
  lbm_heap_state.gc_stack.data[lbm_heap_state.gc_stack.sp ++] = r1;
}

static void add_roots_2(lbm_value r1, lbm_value r2) {
  lbm_heap_state.gc_stack.data[lbm_heap_state.gc_stack.sp ++] = r1;
  lbm_heap_state.gc_stack.data[lbm_heap_state.gc_stack.sp ++] = r2;
}

static void add_roots_3(lbm_value r1, lbm_value r2, lbm_value r3) {
  lbm_heap_state.gc_stack.data[lbm_heap_state.gc_stack.sp ++] = r1;
  lbm_heap_state.gc_stack.data[lbm_heap_state.gc_stack.sp ++] = r2;
  lbm_heap_state.gc_stack.data[lbm_heap_state.gc_stack.sp ++] = r3;
}

static lbm_value cons_with_gc(lbm_value head, lbm_value tail, lbm_value remember) {
  lbm_value res = lbm_heap_allocate_cell(LBM_TYPE_CONS, head, tail);
  if (lbm_is_symbol_merror(res)) {
    add_roots_3(head, tail, remember);
    lbm_gc_mark_phase();
    gc();
    res = lbm_heap_allocate_cell(LBM_TYPE_CONS, head, tail);
    if (lbm_is_symbol_merror(res)) {
        error_ctx(ENC_SYM_MERROR);
    }
  }
  return res;
}

static lbm_uint *get_stack_ptr(eval_context_t *ctx, unsigned int n) {
  if (n > ctx->K.sp) {
    error_ctx(ENC_SYM_STACK_ERROR);
  }
  lbm_uint index = ctx->K.sp - n;
  return &ctx->K.data[index];
}

// pop_stack_ptr is safe when no GC is performed and
// the values of the stack will be dropped.
static lbm_uint *pop_stack_ptr(eval_context_t *ctx, unsigned int n) {
  if (n > ctx->K.sp) {
    error_ctx(ENC_SYM_STACK_ERROR);
  }
  ctx->K.sp -= n;
  return &ctx->K.data[ctx->K.sp];
}

static lbm_uint *stack_reserve(eval_context_t *ctx, unsigned int n) {
  if (ctx->K.sp + n >= ctx->K.size) {
    error_ctx(ENC_SYM_STACK_ERROR);
  }
  lbm_uint *ptr = &ctx->K.data[ctx->K.sp];
  ctx->K.sp += n;
  return ptr;
}

static void handle_flash_status(lbm_flash_status s) {
  if ( s == LBM_FLASH_FULL) {
    lbm_set_error_reason((char*)lbm_error_str_flash_full);
    error_ctx(ENC_SYM_EERROR);
  }
  if (s == LBM_FLASH_WRITE_ERROR) {
    lbm_set_error_reason((char*)lbm_error_str_flash_error);
    error_ctx(ENC_SYM_FATAL_ERROR);
  }
}

static void lift_array_flash(lbm_value flash_cell, char *data, lbm_uint num_elt) {

  lbm_array_header_t flash_array_header;
  flash_array_header.size = num_elt;
  flash_array_header.data = (lbm_uint*)data;
  lbm_uint flash_array_header_ptr;
  handle_flash_status(lbm_write_const_raw((lbm_uint*)&flash_array_header,
                                          sizeof(lbm_array_header_t) / sizeof(lbm_uint),
                                          &flash_array_header_ptr));
  handle_flash_status(write_const_car(flash_cell, flash_array_header_ptr));
  handle_flash_status(write_const_cdr(flash_cell, ENC_SYM_ARRAY_TYPE));
}

static void stack_push(lbm_stack_t *s, lbm_uint val) {
  if (s->sp < s->size) {
    s->data[s->sp++] = val;
    if (s->sp > s->max_sp) s->max_sp = s->sp;
    return;
  }
  error_ctx(ENC_SYM_STACK_ERROR);
}

static void stack_push_2(lbm_stack_t *s, lbm_uint v1, lbm_uint v2) {
  if (s->sp + 1 < s->size) {
    s->data[s->sp++] = v1;
    s->data[s->sp++] = v2;
    if (s->sp > s->max_sp) s->max_sp = s->sp;
    return;
  }
  error_ctx(ENC_SYM_STACK_ERROR);
}

static void stack_push_3(lbm_stack_t *s, lbm_uint v1, lbm_uint v2, lbm_uint v3) {
  if (s->sp + 2 < s->size) {
    s->data[s->sp++] = v1;
    s->data[s->sp++] = v2;
    s->data[s->sp++] = v3;
    if (s->sp > s->max_sp) s->max_sp = s->sp;
    return;
  }
  error_ctx(ENC_SYM_STACK_ERROR);
}

static void stack_push_4(lbm_stack_t *s, lbm_uint v1, lbm_uint v2, lbm_uint v3, lbm_uint v4) {
  if (s->sp + 3 < s->size) {
    s->data[s->sp++] = v1;
    s->data[s->sp++] = v2;
    s->data[s->sp++] = v3;
    s->data[s->sp++] = v4;
    if (s->sp > s->max_sp) s->max_sp = s->sp;
    return;
  }
  error_ctx(ENC_SYM_STACK_ERROR);
}

static void get_car_and_cdr(lbm_value a, lbm_value *a_car, lbm_value *a_cdr) {
  if (lbm_is_ptr(a)) {
    lbm_cons_t *cell = lbm_ref_cell(a);
    *a_car = cell->car;
    *a_cdr = cell->cdr;
  } else if (lbm_is_symbol_nil(a)) {
    *a_car = *a_cdr = ENC_SYM_NIL;
  } else {
    error_ctx(ENC_SYM_TERROR);
  }
}

/* car cdr caar cadr replacements that are evaluator safe. */
static lbm_value get_car(lbm_value a) {
  if (lbm_is_ptr(a)) {
    lbm_cons_t *cell = lbm_ref_cell(a);
    return cell->car;
  } else if (lbm_is_symbol_nil(a)) {
    return a;
  }
  error_ctx(ENC_SYM_TERROR);
  return(ENC_SYM_TERROR);
}

static lbm_value get_cdr(lbm_value a) {
  if (lbm_is_ptr(a)) {
    lbm_cons_t *cell = lbm_ref_cell(a);
    return cell->cdr;
  } else if (lbm_is_symbol_nil(a)) {
    return a;
  }
  error_ctx(ENC_SYM_TERROR);
  return(ENC_SYM_TERROR);
}

static lbm_value get_caar(lbm_value a) {
  if (lbm_is_ptr(a)) {
    lbm_cons_t *cell = lbm_ref_cell(a);
    lbm_value tmp = cell->car;
    if (lbm_is_ptr(tmp)) {
      return lbm_ref_cell(tmp)->car;
    } else if (lbm_is_symbol_nil(tmp)) {
      return tmp;
    }
  } else if (lbm_is_symbol_nil(a)) {
    return a;
  }
  error_ctx(ENC_SYM_TERROR);
  return(ENC_SYM_TERROR);
}

static lbm_value get_cadr(lbm_value a) {
  if (lbm_is_ptr(a)) {
    lbm_cons_t *cell = lbm_ref_cell(a);
    lbm_value tmp = cell->cdr;
    if (lbm_is_ptr(tmp)) {
      return lbm_ref_cell(tmp)->car;
    } else if (lbm_is_symbol_nil(tmp)) {
      return tmp;
    }
  } else if (lbm_is_symbol_nil(a)) {
    return a;
  }
  error_ctx(ENC_SYM_TERROR);
  return(ENC_SYM_TERROR);
}

static lbm_value get_cddr(lbm_value a) {
  if (lbm_is_ptr(a)) {
    lbm_cons_t *cell = lbm_ref_cell(a);
    lbm_value tmp = cell->cdr;
    if (lbm_is_ptr(tmp)) {
      return lbm_ref_cell(tmp)->cdr;
    } else if (lbm_is_symbol_nil(tmp)) {
      return tmp;
    }
  } else if (lbm_is_symbol_nil(a)) {
    return a;
  }
  error_ctx(ENC_SYM_TERROR);
  return(ENC_SYM_TERROR);
}


static lbm_value allocate_closure(lbm_value params, lbm_value body, lbm_value env) {

  if (lbm_heap_num_free() < 4) {
    gc();
    if (lbm_heap_num_free() < 4) {
      error_ctx(ENC_SYM_MERROR);
    }
  }
  lbm_value res = lbm_heap_state.freelist;
  if (lbm_type_of(res) == LBM_TYPE_CONS) {
    lbm_cons_t *cell = lbm_ref_cell(res);
    cell->car = ENC_SYM_CLOSURE;
    cell = lbm_ref_cell(cell->cdr);
    cell->car = params;
    cell = lbm_ref_cell(cell->cdr);
    cell->car = body;
    cell = lbm_ref_cell(cell->cdr);
    cell->car = env;
    lbm_heap_state.freelist = cell->cdr;
    cell->cdr = ENC_SYM_NIL;
    lbm_heap_state.num_alloc+=4;
  } else {
    error_ctx(ENC_SYM_FATAL_ERROR);
  }
  return res;
}

#define CLO_PARAMS 0
#define CLO_BODY   1
#define CLO_ENV    2
// (closure params exp env) -> [params, exp, env])
static void extract_closure(lbm_value closure, lbm_value *res) {
  lbm_value curr = get_cdr(closure);
  for (int i = 0; i < 3; i ++) {
    get_car_and_cdr(curr,&res[i],&curr);
  }
}

static void call_fundamental(lbm_uint fundamental, lbm_value *args, lbm_uint arg_count, eval_context_t *ctx) {
  lbm_value res;
  res = fundamental_table[fundamental](args, arg_count, ctx);
  if (lbm_is_error(res)) {
    if (lbm_is_symbol_merror(res)) {
      gc();
      res = fundamental_table[fundamental](args, arg_count, ctx);
    }
    if (lbm_is_error(res)) {
      error_ctx(res);
    }
  }
  lbm_stack_drop(&ctx->K, arg_count+1);
  ctx->app_cont = true;
  ctx->r = res;
}

// block_current_ctx blocks a context until it is
// woken up externally of a timeout period of time passes.
static void block_current_ctx(uint32_t state, lbm_uint sleep_us, uint32_t wait_mask, bool do_cont) {
  ctx_running->timestamp = timestamp_us_callback();
  ctx_running->sleep_us = sleep_us;
  ctx_running->state  = state;
  ctx_running->wait_mask = wait_mask;
  ctx_running->app_cont = do_cont;
  enqueue_ctx(&blocked, ctx_running);
  ctx_running = NULL;
}

lbm_flash_status lbm_write_const_array_padded(uint8_t *data, lbm_uint n, lbm_uint *res) {
  lbm_uint full_words = n / sizeof(lbm_uint);
  lbm_uint n_mod = n % sizeof(lbm_uint);

  if (n_mod == 0) { // perfect fit.
    return lbm_write_const_raw((lbm_uint*)data, full_words, res);
  } else {
    lbm_uint last_word = 0;
    memcpy(&last_word, &data[full_words * sizeof(lbm_uint)], n_mod);
    if (full_words >= 1) {
      lbm_flash_status s = lbm_write_const_raw((lbm_uint*)data, full_words, res);
      if ( s == LBM_FLASH_WRITE_OK) {
        lbm_uint dummy;
        s = lbm_write_const_raw(&last_word, 1, &dummy);
      }
      return s;
    } else {
      return lbm_write_const_raw(&last_word, 1, res);
    }
  }
}

/****************************************************/
/* Error message creation                           */

#define ERROR_MESSAGE_BUFFER_SIZE_BYTES 256

void print_environments(char *buf, unsigned int size) {

  lbm_value curr_g = lbm_get_env();
  lbm_value curr_l = ctx_running->curr_env;

  printf_callback("\tCurrent local environment:\n");
  while (lbm_type_of(curr_l) == LBM_TYPE_CONS) {

    lbm_print_value(buf, (size/2) - 1, lbm_caar(curr_l));
    lbm_print_value(buf + (size/2),size/2, lbm_cdr(lbm_car(curr_l)));
    printf_callback("\t%s = %s\n", buf, buf+(size/2));
    curr_l = lbm_cdr(curr_l);
  }

  printf_callback("\n\n");
  printf_callback("\tCurrent global environment:\n");
  while (lbm_type_of(curr_g) == LBM_TYPE_CONS) {

    lbm_print_value(buf, (size/2) - 1, lbm_caar(curr_g));
    lbm_print_value(buf + (size/2),size/2, lbm_cdr(lbm_car(curr_g)));
    printf_callback("\t%s = %s\n", buf, buf+(size/2));
    curr_g = lbm_cdr(curr_g);
  }
}


void print_error_message(lbm_value error, unsigned int row, unsigned int col, lbm_int row0, lbm_int row1) {
  if (!printf_callback) return;

  /* try to allocate a lbm_print_value buffer on the lbm_memory */
  char *buf = lbm_malloc_reserve(ERROR_MESSAGE_BUFFER_SIZE_BYTES);
  if (!buf) {
    printf_callback("Error: Not enough free memory to create a human readable error message\n");
    return;
  }

  lbm_print_value(buf, ERROR_MESSAGE_BUFFER_SIZE_BYTES, error);
  printf_callback("***\tError:\t%s\n", buf);
  if (lbm_is_symbol(error) &&
      error == ENC_SYM_NOT_FOUND) {
    lbm_print_value(buf, ERROR_MESSAGE_BUFFER_SIZE_BYTES, ctx_running->curr_exp);
    printf_callback("***\t\t%s\n",buf);
  } else {
    lbm_print_value(buf, ERROR_MESSAGE_BUFFER_SIZE_BYTES, ctx_running->curr_exp);
    printf_callback("***\tAt:\t%s\n",buf);
  }

  printf_callback("\n");

  if (lbm_is_symbol(error) &&
      error == ENC_SYM_RERROR) {
    printf_callback("***\t\tLine: %u\n", row);
    printf_callback("***\t\tColumn: %u\n", col);
  } else {
    printf_callback("***\tBetween rows: (-1 unknown) \n");
    printf_callback("***\t\tStart: %d\n", (int32_t)row0);
    printf_callback("***\t\tEnd:   %d\n", (int32_t)row1);
  }

  printf_callback("\n");

  if (ctx_running->error_reason) {
    printf_callback("Reason:\n\t%s\n\n", ctx_running->error_reason);
  }
  if (lbm_verbose) {
    lbm_print_value(buf, ERROR_MESSAGE_BUFFER_SIZE_BYTES, ctx_running->curr_exp);
    printf_callback("\tIn context: %d\n", ctx_running->id);
    printf_callback("\tCurrent intermediate result: %s\n\n", buf);

    print_environments(buf, ERROR_MESSAGE_BUFFER_SIZE_BYTES);
    printf_callback("\n\n");

    printf_callback("\tStack:\n");
    for (unsigned int i = 0; i < ctx_running->K.sp; i ++) {
      lbm_print_value(buf, ERROR_MESSAGE_BUFFER_SIZE_BYTES, ctx_running->K.data[i]);
      printf_callback("\t\t%s\n", buf);
    }
  }
  lbm_free(buf);
}

/****************************************************/
/* Tokenizing and parsing                           */

bool create_string_channel(char *str, lbm_value *res) {

  lbm_char_channel_t *chan = NULL;
  lbm_string_channel_state_t *st = NULL;

  st = (lbm_string_channel_state_t*)lbm_memory_allocate(sizeof(lbm_string_channel_state_t) / sizeof(lbm_uint) +1);
  if (st == NULL) {
    return false;
  }
  chan = (lbm_char_channel_t*)lbm_memory_allocate(sizeof(lbm_char_channel_t) / sizeof(lbm_uint) + 1);
  if (chan == NULL) {
    lbm_memory_free((lbm_uint*)st);
    return false;
  }

  lbm_create_string_char_channel(st, chan, str);
  lbm_value cell = lbm_heap_allocate_cell(LBM_TYPE_CHANNEL, (lbm_uint) chan, ENC_SYM_CHANNEL_TYPE);
  if (lbm_type_of(cell) == LBM_TYPE_SYMBOL) {
    lbm_memory_free((lbm_uint*)st);
    lbm_memory_free((lbm_uint*)chan);
    return false;
  }

  *res = cell;
  return true;
}

bool lift_char_channel(lbm_char_channel_t *chan , lbm_value *res) {
  lbm_value cell = lbm_heap_allocate_cell(LBM_TYPE_CHANNEL, (lbm_uint) chan, ENC_SYM_CHANNEL_TYPE);
  if (lbm_type_of(cell) == LBM_TYPE_SYMBOL) {
    return false;
  }
  *res = cell;
  return true;
}


/****************************************************/
/* Queue functions                                  */

static void queue_iterator_nm(eval_context_queue_t *q, ctx_fun f, void *arg1, void *arg2) {
  eval_context_t *curr;
  curr = q->first;

  while (curr != NULL) {
    f(curr, arg1, arg2);
    curr = curr->next;
  }
}

void lbm_running_iterator(ctx_fun f, void *arg1, void *arg2){
  mutex_lock(&qmutex);
  queue_iterator_nm(&queue, f, arg1, arg2);
  mutex_unlock(&qmutex);
}

void lbm_blocked_iterator(ctx_fun f, void *arg1, void *arg2){
  mutex_lock(&qmutex);
  queue_iterator_nm(&blocked, f, arg1, arg2);
  mutex_unlock(&qmutex);
}

static void enqueue_ctx_nm(eval_context_queue_t *q, eval_context_t *ctx) {
  if (q->last == NULL) {
    ctx->prev = NULL;
    ctx->next = NULL;
    q->first = ctx;
    q->last  = ctx;
  } else {
    ctx->prev = q->last;
    ctx->next = NULL;
    q->last->next = ctx;
    q->last = ctx;
  }
}

static void enqueue_ctx(eval_context_queue_t *q, eval_context_t *ctx) {
  mutex_lock(&qmutex);
  enqueue_ctx_nm(q,ctx);
  mutex_unlock(&qmutex);
}

static eval_context_t *lookup_ctx_nm(eval_context_queue_t *q, lbm_cid cid) {
  eval_context_t *curr;
  curr = q->first;
  while (curr != NULL) {
    if (curr->id == cid) {
      return curr;
    }
    curr = curr->next;
  }
  return NULL;
}

static bool drop_ctx_nm(eval_context_queue_t *q, eval_context_t *ctx) {

  bool res = false;
  if (q->first == NULL || q->last == NULL) {
    if (!(q->last == NULL && q->first == NULL)) {
      /* error state that should not happen */
      return res;
    }
    /* Queue is empty */
    return res;
  }

  eval_context_t *curr = q->first;
  while (curr) {
    if (curr->id == ctx->id) {
      res = true;
      eval_context_t *tmp = curr->next;
      if (curr->prev == NULL) {
        if (curr->next == NULL) {
          q->last = NULL;
          q->first = NULL;
        } else {
          q->first = tmp;
          tmp->prev = NULL;
        }
      } else { /* curr->prev != NULL */
        if (curr->next == NULL) {
          q->last = curr->prev;
          q->last->next = NULL;
        } else {
          curr->prev->next = tmp;
          tmp->prev = curr->prev;
        }
      }
      break;
    }
    curr = curr->next;
  }
  return res;
}

/* End execution of the running context. */
static void finish_ctx(void) {

  if (!ctx_running) {
    return;
  }
  /* Drop the continuation stack immediately to free up lbm_memory */
  lbm_stack_free(&ctx_running->K);
  ctx_done_callback(ctx_running);
  if (lbm_memory_ptr_inside((lbm_uint*)ctx_running->name)) {
    lbm_free(ctx_running->name);
  }
  if (lbm_memory_ptr_inside((lbm_uint*)ctx_running->error_reason)) {
    lbm_memory_free((lbm_uint*)ctx_running->error_reason);
  }
  lbm_memory_free((lbm_uint*)ctx_running->mailbox);
  lbm_memory_free((lbm_uint*)ctx_running);
  ctx_running = NULL;
}

static void context_exists(eval_context_t *ctx, void *cid, void *b) {
  if (ctx->id == *(lbm_cid*)cid) {
    *(bool*)b = true;
  }
}

bool lbm_wait_ctx(lbm_cid cid, lbm_uint timeout_ms) {

  bool exists;
  uint32_t i = 0;

  do {
    exists = false;
    lbm_blocked_iterator(context_exists, &cid, &exists);
    lbm_running_iterator(context_exists, &cid, &exists);

    if (ctx_running &&
        ctx_running->id == cid) {
      exists = true;
    }

    if (exists) {
       if (usleep_callback) {
         usleep_callback(1000);
       }
       if (timeout_ms > 0) i ++;
    }
  } while (exists && i < timeout_ms);

  if (exists) return false;
  return true;
}

int lbm_set_error_reason(char *error_str) {
  int r = 0;
  if (ctx_running) {
    ctx_running->error_reason = error_str;
    r = 1;
  }
  return r;
}

// Not possible to CONS_WITH_GC in error_ctx_base (potential loop)
static void error_ctx_base(lbm_value err_val, unsigned int row, unsigned int column) {
  ctx_running->r = err_val;

  if (ctx_running->flags & EVAL_CPS_CONTEXT_FLAG_TRAP) {
    if (lbm_heap_num_free() < 3) {
      gc();
    }

    if (lbm_heap_num_free() >= 3) {
      lbm_value msg = lbm_cons(err_val, ENC_SYM_NIL);
      msg = lbm_cons(lbm_enc_i(ctx_running->id), msg);
      msg = lbm_cons(ENC_SYM_EXIT_ERROR, msg);
      if (lbm_is_symbol_merror(msg)) {
        // If this happens something is pretty seriously wrong.
        print_error_message(err_val, row, column, ctx_running->row0, ctx_running->row1);
      } else {
        lbm_find_receiver_and_send(ctx_running->parent, msg);
      }
    }
  } else {
    print_error_message(err_val, row, column, ctx_running->row0, ctx_running->row1);
  }
  finish_ctx();
  longjmp(error_jmp_buf, 1);
}

void error_ctx(lbm_value err_val) {
  error_ctx_base(err_val, 0, 0);
}

static void read_error_ctx(unsigned int row, unsigned int column) {
  error_ctx_base(ENC_SYM_RERROR, row, column);
}

// successfully finish a context
static void ok_ctx(void) {
  if (ctx_running->flags & EVAL_CPS_CONTEXT_FLAG_TRAP) {
    lbm_value msg;
    WITH_GC(msg, lbm_heap_allocate_list_init(3,
                                             ENC_SYM_EXIT_OK,
                                             lbm_enc_i(ctx_running->id),
                                             ctx_running->r));
    lbm_find_receiver_and_send(ctx_running->parent, msg);
  }
  finish_ctx();
}

static eval_context_t *dequeue_ctx_nm(eval_context_queue_t *q) {
  if (q->last == NULL) {
    return NULL;
  }
  // q->first should only be NULL if q->last is.
  eval_context_t *res = q->first;

  if (q->first == q->last) { // One thing in queue
    q->first = NULL;
    q->last  = NULL;
   } else {
    q->first = q->first->next;
    q->first->prev = NULL;
  }
  res->prev = NULL;
  res->next = NULL;
  return res;
}

static void wake_up_ctxs_nm(void) {
  lbm_uint t_now;

  if (timestamp_us_callback) {
    t_now = timestamp_us_callback();
  } else {
    t_now = 0;
  }

  eval_context_queue_t *q = &blocked;
  eval_context_t *curr = q->first;

  while (curr != NULL) {
    lbm_uint t_diff;
    eval_context_t *next = curr->next;
    if (curr->state != LBM_THREAD_STATE_BLOCKED) {
      if ( curr->timestamp > t_now) {
        /* There was an overflow on the counter */
#ifndef LBM64
        t_diff = (0xFFFFFFFF - curr->timestamp) + t_now;
#else
        t_diff = (0xFFFFFFFFFFFFFFFF - curr->timestamp) + t_now;
#endif
      } else {
        t_diff = t_now - curr->timestamp;
      }

      if (t_diff >= curr->sleep_us) {
        eval_context_t *wake_ctx = curr;
        if (curr == q->last) {
          if (curr->prev) {
            q->last = curr->prev;
            q->last->next = NULL;
          } else {
            q->first = NULL;
            q->last = NULL;
          }
        } else if (curr->prev == NULL) {
          q->first = curr->next;
          q->first->prev = NULL;
        } else {
          curr->prev->next = curr->next;
          if (curr->next) {
            curr->next->prev = curr->prev;
          }
        }
        wake_ctx->next = NULL;
        wake_ctx->prev = NULL;
        if (curr->state == LBM_THREAD_STATE_TIMEOUT) {
          mailbox_add_mail(wake_ctx, ENC_SYM_TIMEOUT);
          wake_ctx->r = ENC_SYM_TIMEOUT;
        }
        wake_ctx->state = LBM_THREAD_STATE_READY;
        enqueue_ctx_nm(&queue, wake_ctx);
      }
    }
    curr = next;
  }
}

static void yield_ctx(lbm_uint sleep_us) {
  if (timestamp_us_callback) {
    ctx_running->timestamp = timestamp_us_callback();
    ctx_running->sleep_us = sleep_us;
    ctx_running->state = LBM_THREAD_STATE_SLEEPING;
  } else {
    ctx_running->timestamp = 0;
    ctx_running->sleep_us = 0;
    ctx_running->state = LBM_THREAD_STATE_SLEEPING;
  }
  ctx_running->r = ENC_SYM_TRUE;
  ctx_running->app_cont = true;
  enqueue_ctx(&blocked,ctx_running);
  ctx_running = NULL;
}

static lbm_cid lbm_create_ctx_parent(lbm_value program, lbm_value env, lbm_uint stack_size, lbm_cid parent, uint32_t context_flags, char *name) {

  if (!lbm_is_cons(program)) return -1;

  eval_context_t *ctx = NULL;

  ctx = (eval_context_t*)lbm_malloc(sizeof(eval_context_t));
  if (ctx == NULL) {
    add_roots_2(program, env);
    lbm_gc_mark_phase();
    gc();
    ctx = (eval_context_t*)lbm_malloc(sizeof(eval_context_t));
  }
  if (ctx == NULL) return -1;

  if (!lbm_stack_allocate(&ctx->K, stack_size)) {
    add_roots_2(program, env);
    lbm_gc_mark_phase();
    gc();
    if (!lbm_stack_allocate(&ctx->K, stack_size)) {
      lbm_memory_free((lbm_uint*)ctx);
      return -1;
    }
  }

  lbm_value *mailbox = NULL;
  mailbox = (lbm_value*)lbm_memory_allocate(EVAL_CPS_DEFAULT_MAILBOX_SIZE);
  if (mailbox == NULL) {
    add_roots_2(program, env);
    lbm_gc_mark_phase();
    gc();
    mailbox = (lbm_value *)lbm_memory_allocate(EVAL_CPS_DEFAULT_MAILBOX_SIZE);
  }
  if (mailbox == NULL) {
    lbm_stack_free(&ctx->K);
    lbm_memory_free((lbm_uint*)ctx);
    return -1;
  }

  // TODO: Limit names to 19 chars + 1 char for 0? (or something similar).
  if (name) {
    lbm_uint name_len = strlen(name) + 1;
    ctx->name = lbm_malloc(strlen(name) + 1);
    if (ctx->name == NULL) {
      add_roots_2(program, env);
      lbm_gc_mark_phase();
      gc();
      ctx->name = lbm_malloc(strlen(name) + 1);
    }
    if (ctx->name == NULL) {
      lbm_stack_free(&ctx->K);
      lbm_memory_free((lbm_uint*)mailbox);
      lbm_memory_free((lbm_uint*)ctx);
      return -1;
    }
    memcpy(ctx->name, name, name_len+1);
  } else {
     ctx->name = NULL;
  }

  lbm_int cid = lbm_memory_address_to_ix((lbm_uint*)ctx);

  ctx->program = lbm_cdr(program);
  ctx->curr_exp = lbm_car(program);
  ctx->curr_env = env;
  ctx->r = ENC_SYM_NIL;
  ctx->error_reason = NULL;
  ctx->mailbox = mailbox;
  ctx->mailbox_size = EVAL_CPS_DEFAULT_MAILBOX_SIZE;
  ctx->flags = context_flags;
  ctx->num_mail = 0;
  ctx->app_cont = false;
  ctx->timestamp = 0;
  ctx->sleep_us = 0;
  ctx->state = LBM_THREAD_STATE_READY;
  ctx->prev = NULL;
  ctx->next = NULL;

  ctx->row0 = -1;
  ctx->row1 = -1;

  ctx->id = cid;
  ctx->parent = parent;
  ctx->wait_mask = 0;

  if (!lbm_push(&ctx->K, DONE)) {
    lbm_memory_free((lbm_uint*)ctx->mailbox);
    lbm_stack_free(&ctx->K);
    lbm_memory_free((lbm_uint*)ctx);
    return -1;
  }

  enqueue_ctx(&queue,ctx);

  return ctx->id;
}

lbm_cid lbm_create_ctx(lbm_value program, lbm_value env, lbm_uint stack_size) {
  // Creates a parentless context.
  return lbm_create_ctx_parent(program,
                               env,
                               stack_size,
                               -1,
                               EVAL_CPS_CONTEXT_FLAG_NOTHING,
                               NULL);
}

bool lbm_mailbox_change_size(eval_context_t *ctx, lbm_uint new_size) {

  lbm_value *mailbox = NULL;
  mailbox = (lbm_value*)lbm_memory_allocate(new_size);
  if (mailbox == NULL) {
    gc();
    mailbox = (lbm_value *)lbm_memory_allocate(new_size);
  }
  if (mailbox == NULL) {
    return false;
  }

  for (lbm_uint i = 0; i < ctx->num_mail; i ++ ) {
    mailbox[i] = ctx->mailbox[i];
  }
  lbm_memory_free(ctx->mailbox);
  ctx->mailbox = mailbox;
  ctx->mailbox_size = (uint32_t)new_size;
  return true;
}

static void mailbox_remove_mail(eval_context_t *ctx, lbm_uint ix) {

  for (lbm_uint i = ix; i < ctx->num_mail-1; i ++) {
    ctx->mailbox[i] = ctx->mailbox[i+1];
  }
  ctx->num_mail --;
}

static bool mailbox_add_mail(eval_context_t *ctx, lbm_value mail) {

  if (ctx->num_mail >= ctx->mailbox_size) {
    mailbox_remove_mail(ctx, 0);
  }

  ctx->mailbox[ctx->num_mail] = mail;
  ctx->num_mail ++;
  return true;
}

/* Advance execution to the next expression in the program */
static void advance_ctx(eval_context_t *ctx) {
  if (lbm_is_cons(ctx->program)) {
    stack_push(&ctx->K, DONE);
    get_car_and_cdr(ctx->program, &ctx->curr_exp, &ctx->program);
    ctx->curr_env = ENC_SYM_NIL;
    ctx->app_cont = false;
  } else {
    if (ctx_running == ctx) {  // This should always be the case because of odd historical reasons.
      ok_ctx();
    }
  }
}

bool lbm_unblock_ctx(lbm_cid cid, lbm_flat_value_t *fv) {
  return event_internal(LBM_EVENT_UNBLOCK_CTX, (lbm_uint)cid, (lbm_uint)fv->buf, fv->buf_size);
}

bool lbm_unblock_ctx_unboxed(lbm_cid cid, lbm_value unboxed) {
  mutex_lock(&blocking_extension_mutex);
  bool r = false;
  lbm_uint t = lbm_type_of(unboxed);
  if (t == LBM_TYPE_SYMBOL ||
      t == LBM_TYPE_I ||
      t == LBM_TYPE_U ||
      t == LBM_TYPE_CHAR) {

    eval_context_t *found = NULL;
    mutex_lock(&qmutex);
    found = lookup_ctx_nm(&blocked, cid);
    if (found) {
      drop_ctx_nm(&blocked,found);
      found->r = unboxed;
      enqueue_ctx_nm(&queue,found);
      r = true;
    }
    mutex_unlock(&qmutex);
  }
  mutex_unlock(&blocking_extension_mutex);
  return r;
}

void lbm_block_ctx_from_extension_timeout(float s) {
  mutex_lock(&blocking_extension_mutex);
  blocking_extension = true;
  blocking_extension_timeout_us = S_TO_US(s);
  blocking_extension_timeout = true;
}
void lbm_block_ctx_from_extension(void) {
  mutex_lock(&blocking_extension_mutex);
  blocking_extension = true;
  blocking_extension_timeout_us = 0;
  blocking_extension_timeout = false;
}

void lbm_undo_block_ctx_from_extension(void) {
  blocking_extension = false;
  blocking_extension_timeout_us = 0;
  blocking_extension_timeout = false;
  mutex_unlock(&blocking_extension_mutex);
}

lbm_value lbm_find_receiver_and_send(lbm_cid cid, lbm_value msg) {
  mutex_lock(&qmutex);
  eval_context_t *found = NULL;
  bool found_blocked = false;

  found = lookup_ctx_nm(&blocked, cid);
  if (found) found_blocked = true;

  if (found == NULL) {
    found = lookup_ctx_nm(&queue, cid);
  }

  if (found) {
    if (!mailbox_add_mail(found, msg)) {
      mutex_unlock(&qmutex);
      return ENC_SYM_NIL;
    }

    if (found_blocked){
      drop_ctx_nm(&blocked,found);
      enqueue_ctx_nm(&queue,found);
    }
    mutex_unlock(&qmutex);
    return ENC_SYM_TRUE;
  }

  /* check the current context */
  if (ctx_running && ctx_running->id == cid) {
    if (!mailbox_add_mail(ctx_running, msg)) {
      mutex_unlock(&qmutex);
      return ENC_SYM_NIL;
    }
    mutex_unlock(&qmutex);
    return ENC_SYM_TRUE;
  }
  mutex_unlock(&qmutex);
  return ENC_SYM_NIL;
}

/* Pattern matching is currently implemented as a recursive
   function and make use of stack relative to the size of
   expressions that are being matched. */
static bool match(lbm_value p, lbm_value e, lbm_value *env, bool *gc) {

  lbm_value binding;

  if (lbm_is_match_binder(p)) {
    lbm_value var = get_cadr(p);
    lbm_value bindertype = get_car(p);

    if (!lbm_is_symbol(var)) return false;

    switch (lbm_dec_sym(bindertype)) {
    case SYM_MATCH_ANY:
      if (lbm_dec_sym(var) == SYM_DONTCARE) {
        return true;
      }
      break;
    default: /* this should be an error case */
      return false;
    }
    binding = lbm_cons(var, e);
    if ( lbm_type_of(binding) == LBM_TYPE_SYMBOL ) {
      *gc = true;
      return false;
    }
    *env = lbm_cons(binding, *env);
    if ( lbm_type_of(*env) == LBM_TYPE_SYMBOL ) {
      *gc = true;
      return false;
    }
    return true;
  }

  /* Comma-qualification experiment. */
  if (lbm_is_comma_qualified_symbol(p)) {
    lbm_value sym = get_cadr(p);
    lbm_value val = lbm_env_lookup(sym, *env);
    if (lbm_is_symbol(SYM_NOT_FOUND)) {
      return false;
    }
    return (val == e);
  }

  if (lbm_is_symbol(p)) {
    if (lbm_dec_sym(p) == SYM_DONTCARE) return true;
    return (p == e);
  }
  if (lbm_is_cons(p) &&
      lbm_is_cons(e) ) {

    lbm_value headp, tailp;
    lbm_value heade, taile;
    get_car_and_cdr(p, &headp, &tailp);
    get_car_and_cdr(e, &heade, &taile); // Static analysis warns, but execution does not
                                        // pass this point unless head and tail get initialized.
    if (!match(headp, heade, env, gc)) {
      return false;
    }
    return match (tailp, taile, env, gc);
  }
  return struct_eq(p, e);
}

static int find_match(lbm_value plist, lbm_value *earr, lbm_uint num, lbm_value *e, lbm_value *env) {

  lbm_value curr_p = plist;
  int n = 0;
  bool gc = false;
  for (int i = 0; i < (int)num; i ++ ) {
    lbm_value curr_e = earr[i];
    while (lbm_is_cons(curr_p)) {
      lbm_value me = get_car(curr_p);
      if (match(get_car(me), curr_e, env, &gc)) {
        if (gc) return FM_NEED_GC;
        *e = get_cadr(me);

        if (!lbm_is_symbol_nil(get_cadr(get_cdr(me)))) {
          return FM_PATTERN_ERROR;
        }
        return n;
      }
      curr_p = get_cdr(curr_p);
    }
    curr_p = plist;       /* search all patterns against next exp */
    n ++;
  }

  return FM_NO_MATCH;
}

/****************************************************/
/* Garbage collection                               */

static void mark_context(eval_context_t *ctx, void *arg1, void *arg2) {
  (void) arg1;
  (void) arg2;
  lbm_value roots[4] = { ctx->curr_env, ctx->curr_exp, ctx->program, ctx->r };
  lbm_gc_mark_aux(roots, 4);
  lbm_gc_mark_aux(ctx->mailbox, ctx->num_mail);
  lbm_gc_mark_aux(ctx->K.data, ctx->K.sp);
}

static int gc(void) {
  if (ctx_running) {
    ctx_running->state = ctx_running->state | LBM_THREAD_STATE_GC_BIT;
  }

  gc_requested = false;
  lbm_gc_state_inc();

  lbm_value *variables = lbm_get_variable_table();
  if (variables) {
    for (int i = 0; i < lbm_get_num_variables(); i ++) {
      add_roots_1(variables[i]);
      lbm_gc_mark_phase();
    }
  }
  // The freelist should generally be NIL when GC runs.
  lbm_nil_freelist();
  add_roots_1(lbm_get_env());
  lbm_gc_mark_phase();

  mutex_lock(&qmutex); // Lock the queues.
                       // Any concurrent messing with the queues
                       // while doing GC cannot possibly be good.
  queue_iterator_nm(&queue, mark_context, NULL, NULL);
  queue_iterator_nm(&blocked, mark_context, NULL, NULL);

  if (ctx_running) {
    mark_context(ctx_running, NULL, NULL);
  }
  mutex_unlock(&qmutex);

#ifdef VISUALIZE_HEAP
  heap_vis_gen_image();
#endif

  int r = lbm_gc_sweep_phase();
  lbm_heap_new_freelist_length();

  if (ctx_running) {
    ctx_running->state = ctx_running->state & ~LBM_THREAD_STATE_GC_BIT;
  }
  return r;
}

int lbm_perform_gc(void) {
  return gc();
}

/****************************************************/
/* Evaluation functions                             */

static void eval_symbol(eval_context_t *ctx) {
  lbm_uint s = lbm_dec_sym(ctx->curr_exp);
  if (s >= RUNTIME_SYMBOLS_START) {
    lbm_value res;
    if (lbm_env_lookup_b(&res, ctx->curr_exp, ctx->curr_env) ||
        lbm_env_lookup_b(&res, ctx->curr_exp, lbm_get_env())) {
      ctx->r =  res;
      ctx->app_cont = true;
      return;
    }
  } else {
    //special symbols and extensions can be handled the same way.
    if (s <= EXTENSION_SYMBOLS_END) {
      ctx->r = ctx->curr_exp;
      ctx->app_cont = true;
      return;
    }
    if (s <= VARIABLE_SYMBOLS_END) {
      ctx->r = lbm_get_var(s);
      ctx->app_cont = true;
      return;
    }
  }
  // Dynamic load attempt
  const char *sym_str = lbm_get_name_by_symbol(s);
  const char *code_str = NULL;
  if (!dynamic_load_callback(sym_str, &code_str)) {
    error_ctx(ENC_SYM_NOT_FOUND);
  } else {
    stack_push_3(&ctx->K, ctx->curr_env, ctx->curr_exp, RESUME);

    lbm_value chan;
    if (!create_string_channel((char *)code_str, &chan)) {
      gc();
      if (!create_string_channel((char *)code_str, &chan)) {
        error_ctx(ENC_SYM_MERROR);
      }
    }

    lbm_value loader = ENC_SYM_NIL;
    WITH_GC_RMBR_1(loader, lbm_heap_allocate_list_init(2,
                                                       ENC_SYM_READ,
                                                       chan), chan);
    lbm_value evaluator = ENC_SYM_NIL;
    WITH_GC_RMBR_1(evaluator, lbm_heap_allocate_list_init(2,
                                                          ENC_SYM_EVAL,
                                                          loader), loader);
    ctx->curr_exp = evaluator;
    ctx->curr_env = ENC_SYM_NIL; // dynamics should be evaluable in empty local env
  }
}

static void eval_quote(eval_context_t *ctx) {
  ctx->r = get_cadr(ctx->curr_exp);
  ctx->app_cont = true;
}

static void eval_selfevaluating(eval_context_t *ctx) {
  ctx->r = ctx->curr_exp;
  ctx->app_cont = true;
}

static void eval_progn(eval_context_t *ctx) {
  lbm_value exps = get_cdr(ctx->curr_exp);

  if (lbm_is_cons(exps)) {
    lbm_uint *sptr = stack_reserve(ctx, 4);
    sptr[0] = ctx->curr_env; // env to restore between expressions in progn
    sptr[1] = lbm_enc_u(0);   // Has env been copied (needed for progn local bindings)
    sptr[3] = PROGN_REST;
    get_car_and_cdr(exps, &ctx->curr_exp, &sptr[2]);
    if (lbm_is_symbol(sptr[2])) /* The only symbol it can be is nil */
      lbm_stack_drop(&ctx->K, 4);
  } else if (lbm_is_symbol_nil(exps)) {
    ctx->r = ENC_SYM_NIL;
    ctx->app_cont = true;
  } else {
    error_ctx(ENC_SYM_EERROR);
  }
}

static void eval_atomic(eval_context_t *ctx) {
  if (is_atomic) {
    lbm_set_error_reason("Atomic blocks cannot be nested!");
    error_ctx(ENC_SYM_EERROR);
  }

  stack_push(&ctx->K, EXIT_ATOMIC);
  is_atomic ++;
  eval_progn(ctx);
}


static void eval_callcc(eval_context_t *ctx) {
  lbm_value cont_array;
  if (!lbm_heap_allocate_array(&cont_array, ctx->K.sp * sizeof(lbm_uint))) {
    gc();
    if (!lbm_heap_allocate_array(&cont_array, ctx->K.sp * sizeof(lbm_uint))) {
      error_ctx(ENC_SYM_MERROR);
    }
  }
  lbm_array_header_t *arr = (lbm_array_header_t*)get_car(cont_array);
  memcpy(arr->data, ctx->K.data, ctx->K.sp * sizeof(lbm_uint));

  lbm_value acont = cons_with_gc(ENC_SYM_CONT, cont_array, ENC_SYM_NIL);

  /* Create an application */
  lbm_value fun_arg = get_cadr(ctx->curr_exp);
  lbm_value app = ENC_SYM_NIL;
  WITH_GC_RMBR_1(app, lbm_heap_allocate_list_init(2,
                                                  fun_arg,
                                                  acont), acont);

  ctx->curr_exp = app;
  ctx->app_cont = false;
}

// (define sym exp)
static void eval_define(eval_context_t *ctx) {
  lbm_value args = get_cdr(ctx->curr_exp);
  lbm_value key, rest_args;
  get_car_and_cdr(args, &key, &rest_args);
  lbm_value val_exp, rest_val;
  get_car_and_cdr(rest_args, &val_exp, &rest_val);
  lbm_uint *sptr = stack_reserve(ctx, 2);

  if (lbm_is_symbol(key) && lbm_is_symbol_nil(rest_val)) {
    lbm_uint sym_val = lbm_dec_sym(key);

    sptr[0] = key;

    if ((sym_val >= VARIABLE_SYMBOLS_START) &&
        (sym_val <  VARIABLE_SYMBOLS_END)) {
      sptr[1] = SET_VARIABLE;
      ctx->curr_exp = val_exp;
      return;
    } else if (sym_val >= RUNTIME_SYMBOLS_START) {
      sptr[1] = SET_GLOBAL_ENV;
      if (ctx->flags & EVAL_CPS_CONTEXT_FLAG_CONST) {
        stack_push(&ctx->K, MOVE_VAL_TO_FLASH_DISPATCH);
      }
      ctx->curr_exp = val_exp;
      return;
    }
  }
  error_ctx(ENC_SYM_EERROR);
}

// (lambda param-list body-exp) -> (closure param-list body-exp env)
static void eval_lambda(eval_context_t *ctx) {
  lbm_value cdr = get_cdr(ctx->curr_exp);
  ctx->r = allocate_closure(get_car(cdr), get_cadr(cdr), ctx->curr_env);
  ctx->app_cont = true;
}

static void eval_if(eval_context_t *ctx) {

  lbm_value cdr = get_cdr(ctx->curr_exp);
  lbm_value exp, cddr;
  get_car_and_cdr(cdr, &exp, &cddr);
  lbm_value then_branch = get_car(cddr);
  lbm_value else_branch = get_cadr(cddr);

  lbm_uint *sptr = stack_reserve(ctx, 4);
  sptr[0] = else_branch;
  sptr[1] = then_branch;
  sptr[2] = ctx->curr_env;
  sptr[3] = IF;
  ctx->curr_exp = exp;
}

static void eval_cond(eval_context_t *ctx) {
  lbm_value cond1 = get_cadr(ctx->curr_exp);

  if (lbm_is_symbol_nil(cond1)) {
    ctx->r = ENC_SYM_NIL;
    ctx->app_cont = true;
  } else {
    lbm_uint len = lbm_list_length(cond1);
    if (len != 2) {
      lbm_set_error_reason("Incorrect syntax in cond");
      error_ctx(ENC_SYM_EERROR);
    }
    lbm_value condition = get_car(cond1);
    lbm_value body = get_cadr(cond1);
    lbm_value rest;
    rest = cons_with_gc(ENC_SYM_COND, get_cddr(ctx->curr_exp), ENC_SYM_NIL);
    lbm_uint *sptr = stack_reserve(ctx, 4);
    sptr[0] = rest;
    sptr[1] = body;
    sptr[2] = ctx->curr_env;
    sptr[3] = IF;
    ctx->curr_exp = condition;
  }
}

static void eval_app_cont(eval_context_t *ctx) {
  lbm_stack_drop(&ctx->K, 1);
  ctx->app_cont = true;
}

// (var x (...)) - local binding inside of an progn
static void eval_var(eval_context_t *ctx) {
  lbm_value args = get_cdr(ctx->curr_exp);
  lbm_value sym = get_car(args);
  lbm_value v_exp = get_cadr(args);
  stack_push_2(&ctx->K, sym, PROGN_VAR);
  ctx->curr_exp = v_exp;
}

// (setq x (...)) - same as (set 'x (...)) or (setvar 'x (...))
static void eval_setq(eval_context_t *ctx) {
  lbm_value args = get_cdr(ctx->curr_exp);
  lbm_value sym = get_car(args);
  lbm_value v_exp = get_cadr(args);
  stack_push_2(&ctx->K, sym, SETQ);
  ctx->curr_exp = v_exp;
}

static void eval_move_to_flash(eval_context_t *ctx) {
  lbm_value args = get_cdr(ctx->curr_exp);
  stack_push_2(&ctx->K, args, MOVE_TO_FLASH);
  ctx->app_cont = true;
}

// Create a named location in an environment to later receive a value.
static int create_binding_location(lbm_value key, lbm_value *env) {

  if (lbm_is_symbol(key) &&
      (key == ENC_SYM_NIL ||
       key == ENC_SYM_DONTCARE))
    return BL_OK;

  if (lbm_type_of(key) == LBM_TYPE_SYMBOL) { // default case
    lbm_value binding;
    lbm_value new_env_tmp;
    binding = lbm_cons(key, ENC_SYM_NIL);
    new_env_tmp = lbm_cons(binding, *env);
    if (lbm_is_symbol(binding) || lbm_is_symbol(new_env_tmp)) {
      return BL_NO_MEMORY;
    }
    *env = new_env_tmp;
  } else if (lbm_is_cons(key)) { // deconstruct case
    int r = create_binding_location(get_car(key), env);
    if (r == BL_OK) {
      r = create_binding_location(get_cdr(key), env);
    }
    return r;
  }
  return BL_OK;
}

static void eval_let(eval_context_t *ctx) {
  lbm_value orig_env = ctx->curr_env;
  lbm_value binds    = get_cadr(ctx->curr_exp); // key value pairs.
  lbm_value exp      = get_cadr(get_cdr(ctx->curr_exp)); // exp to evaluate in the new env.

  lbm_value curr = binds;
  lbm_value new_env = orig_env;

  if (!lbm_is_cons(binds)) {
    // binds better be nil or there is a programmer error.
    ctx->curr_exp = exp;
    return;
  }

  // Implements letrec by "preallocating" the key parts
  while (lbm_is_cons(curr)) {
    lbm_value new_env_tmp = new_env;
    lbm_value key = get_caar(curr);
    int r = create_binding_location(key, &new_env_tmp);
    if (r < 0) {
      if (r == BL_NO_MEMORY) {
        new_env_tmp = new_env;
        add_roots_1(new_env);
        lbm_gc_mark_phase();
        gc();
        r = create_binding_location(key, &new_env_tmp);
      }
      if (r < 0) {
        if (r == BL_INCORRECT_KEY)
          error_ctx(ENC_SYM_TERROR);
        else if (r == BL_NO_MEMORY)
          error_ctx(ENC_SYM_MERROR);
        else
          error_ctx(ENC_SYM_FATAL_ERROR);
        return;
      }
    }
    new_env = new_env_tmp;
    curr = get_cdr(curr);
  }

  lbm_value key0 = get_caar(binds);
  lbm_value val0_exp = get_cadr(get_car(binds));

  lbm_uint *sptr = stack_reserve(ctx, 5);
  sptr[0] = exp;
  sptr[1] = get_cdr(binds);
  sptr[2] = new_env;
  sptr[3] = key0;
  sptr[4] = BIND_TO_KEY_REST;
  ctx->curr_exp = val0_exp;
  ctx->curr_env = new_env;
  return;
}

static void eval_and(eval_context_t *ctx) {
  lbm_value rest = get_cdr(ctx->curr_exp);
  if (lbm_is_symbol_nil(rest)) {
    ctx->app_cont = true;
    ctx->r = ENC_SYM_TRUE;
  } else {
    stack_push_2(&ctx->K, get_cdr(rest), AND);
    ctx->curr_exp = get_car(rest);
  }
}

static void eval_or(eval_context_t *ctx) {
  lbm_value rest = get_cdr(ctx->curr_exp);
  if (lbm_is_symbol_nil(rest)) {
    ctx->app_cont = true;
    ctx->r = ENC_SYM_NIL;
  } else {
    stack_push_2(&ctx->K, get_cdr(rest), OR);
    ctx->curr_exp = get_car(rest);
  }
}

/* pattern matching experiment */
/* format:                     */
/* (match e (pattern body)     */
/*          (pattern body)     */
/*          ...  )             */
static void eval_match(eval_context_t *ctx) {

  lbm_value rest = get_cdr(ctx->curr_exp);
  if (lbm_type_of(rest) == LBM_TYPE_SYMBOL &&
      rest == ENC_SYM_NIL) {
    /* Someone wrote the program (match) */
    ctx->app_cont = true;
    ctx->r = ENC_SYM_NIL;
  } else {
    lbm_value cdr_rest;
    get_car_and_cdr(rest, &ctx->curr_exp, &cdr_rest);
    stack_push_3(&ctx->K, cdr_rest, ctx->curr_env, MATCH);
  }
}

static void receive_base(eval_context_t *ctx, lbm_value pats, float timeout_time, bool timeout) {
   if (ctx->num_mail == 0) {
     if (timeout) {
       block_current_ctx(LBM_THREAD_STATE_TIMEOUT, S_TO_US(timeout_time), 0, false);
     } else {
       block_current_ctx(LBM_THREAD_STATE_BLOCKED,0,0, false);
     }
  } else {
    lbm_value *msgs = ctx->mailbox;
    lbm_uint  num   = ctx->num_mail;

    if (lbm_is_symbol_nil(pats)) {
      /* A receive statement without any patterns */
      ctx->app_cont = true;
      ctx->r = ENC_SYM_NIL;
    } else {
      /* The common case */
      lbm_value e;
      lbm_value new_env = ctx->curr_env;
      int n = find_match(pats, msgs, num, &e, &new_env);
      if (n == FM_NEED_GC) {
        gc();
        new_env = ctx->curr_env;
        n = find_match(pats, msgs, num, &e, &new_env);
        if (n == FM_NEED_GC) {
          error_ctx(ENC_SYM_MERROR);
        }
      }
      if (n == FM_PATTERN_ERROR) {
        lbm_set_error_reason("Incorrect pattern format for recv");
        error_ctx(ENC_SYM_EERROR);
      } else if (n >= 0 ) { /* Match */
        mailbox_remove_mail(ctx, (lbm_uint)n);
        ctx->curr_env = new_env;
        ctx->curr_exp = e;
      } else { /* No match  go back to sleep */
        ctx->r = ENC_SYM_NO_MATCH;
        if (timeout) {
          block_current_ctx(LBM_THREAD_STATE_TIMEOUT,S_TO_US(timeout_time),0,false);
        } else {
          block_current_ctx(LBM_THREAD_STATE_BLOCKED, 0, 0, false);
        }
      }
    }
  }
  return;
}

static void eval_receive_timeout(eval_context_t *ctx) {
  if (is_atomic) {
    lbm_set_error_reason((char*)lbm_error_str_forbidden_in_atomic);
    error_ctx(ENC_SYM_EERROR);
  }
  lbm_value timeout_val = get_car(get_cdr(ctx->curr_exp));
  if (!lbm_is_number(timeout_val)) {
    error_ctx(ENC_SYM_EERROR);
  }
  float timeout_time = lbm_dec_as_float(timeout_val);
  lbm_value pats = get_cdr(get_cdr(ctx->curr_exp));
  receive_base(ctx, pats, timeout_time, true);
}

static void eval_receive(eval_context_t *ctx) {

  if (is_atomic) {
    lbm_set_error_reason((char*)lbm_error_str_forbidden_in_atomic);
    error_ctx(ENC_SYM_EERROR);
  }
  lbm_value pats = get_cdr(ctx->curr_exp);
  receive_base(ctx, pats, 0, false);
}

/*********************************************************/
/*  Continuation functions                               */

/* cont_set_global_env
   sp-1 : Key-symbol
 */
static void cont_set_global_env(eval_context_t *ctx){

  lbm_value key;
  lbm_value val = ctx->r;

  lbm_pop(&ctx->K, &key);
  lbm_value new_env;
  // A key is a symbol and should not need to be remembered.
  WITH_GC(new_env, lbm_env_set(*lbm_get_env_ptr(),key,val));

  *lbm_get_env_ptr() = new_env;
  ctx->r = val;

  ctx->app_cont = true;

  return;
}

static void cont_set_var(eval_context_t *ctx) {
  lbm_value key;
  lbm_value val = ctx->r;
  lbm_pop(&ctx->K, &key);

  ctx->r = lbm_set_var(lbm_dec_sym(key), val);
  ctx->app_cont = true;
  return;
}

static void cont_resume(eval_context_t *ctx) {
  lbm_value exp;
  lbm_value env;
  lbm_pop_2(&ctx->K, &exp, &env);
  ctx->curr_exp = exp;
  ctx->curr_env = env;
}

static void cont_progn_rest(eval_context_t *ctx) {
  lbm_value *sptr = get_stack_ptr(ctx, 3);

  lbm_value rest = sptr[2];
  lbm_value env  = sptr[0];

  lbm_value rest_car, rest_cdr;
  get_car_and_cdr(rest, &rest_car, &rest_cdr);
  if (lbm_is_symbol_nil(rest_cdr)) {
    // allow for tail recursion
    ctx->curr_exp = rest_car;
    ctx->curr_env = env;
    lbm_stack_drop(&ctx->K, 3);
  } else {
    sptr[2] = rest_cdr;
    stack_push(&ctx->K, PROGN_REST);
    ctx->curr_exp = rest_car;
    ctx->curr_env = env;
  }
}

static void cont_wait(eval_context_t *ctx) {

  lbm_value cid_val;
  lbm_pop(&ctx->K, &cid_val);
  lbm_cid cid = (lbm_cid)lbm_dec_i(cid_val);

  bool exists = false;

  lbm_blocked_iterator(context_exists, &cid, &exists);
  lbm_running_iterator(context_exists, &cid, &exists);

  if (ctx_running->id == cid) {
    exists = true;
  }

  if (exists) {
    stack_push_2(&ctx->K, lbm_enc_i(cid), WAIT);
    ctx->r = ENC_SYM_TRUE;
    ctx->app_cont = true;
    yield_ctx(50000);
  } else {
    ctx->r = ENC_SYM_TRUE;
    ctx->app_cont = true;
  }
}

static lbm_value perform_setvar(lbm_value key, lbm_value val, lbm_value env) {

  lbm_uint s = lbm_dec_sym(key);
  lbm_value res = val;

  if (s >= VARIABLE_SYMBOLS_START &&
      s <  VARIABLE_SYMBOLS_END) {
    return lbm_set_var(s, val);
  } else if (s >= RUNTIME_SYMBOLS_START) {
    lbm_value new_env = lbm_env_modify_binding(env, key, val);
    if (lbm_is_symbol(new_env) && new_env == ENC_SYM_NOT_FOUND) {
      new_env = lbm_env_modify_binding(lbm_get_env(), key, val);
    }
    if (lbm_is_symbol(new_env) && new_env == ENC_SYM_NOT_FOUND) {
      new_env = lbm_env_set(lbm_get_env(), key, val);
      if (!lbm_is_symbol(new_env)) {
        *lbm_get_env_ptr() = new_env;
      } else {
        res = new_env;
      }
    }
  }
  return res;
}

static void apply_setvar(lbm_value *args, lbm_uint nargs, eval_context_t *ctx) {
  if (nargs == 2 && lbm_is_symbol(args[0])) {
    lbm_value res;
    WITH_GC(res, perform_setvar(args[0], args[1], ctx->curr_env));
    ctx->r = args[1];
    lbm_stack_drop(&ctx->K, nargs+1);
    ctx->app_cont = true;
  } else {
    error_ctx(ENC_SYM_EERROR);
  }
}

static void apply_read_base(lbm_value *args, lbm_uint nargs, eval_context_t *ctx, bool program, bool incremental) {
  if (nargs == 1) {
    lbm_value chan = ENC_SYM_NIL;
    if (lbm_type_of_functional(args[0]) == LBM_TYPE_ARRAY) {
      if (!create_string_channel(lbm_dec_str(args[0]), &chan)) {
        gc();
        if (!create_string_channel(lbm_dec_str(args[0]), &chan)) {
          error_ctx(ENC_SYM_MERROR);
        }
      }
    } else if (lbm_type_of(args[0]) == LBM_TYPE_CHANNEL) {
      chan = args[0];
    } else {
      error_ctx(ENC_SYM_EERROR);
    }
    lbm_value *sptr = get_stack_ptr(ctx, 2);

    sptr[0] = chan;
    sptr[1] = READ_DONE;

    if (program) {
      if (incremental) {
        stack_push_3(&ctx->K, chan, ctx->curr_env, READ_EVAL_CONTINUE);
      } else {
        stack_push_4(&ctx->K, ENC_SYM_NIL, ENC_SYM_NIL, chan, READ_APPEND_CONTINUE);
      }
    }
    stack_push_3(&ctx->K, chan, lbm_enc_u(1), READ_NEXT_TOKEN);
    ctx->app_cont = true;
  } else {
    lbm_set_error_reason((char*)lbm_error_str_num_args);
    error_ctx(ENC_SYM_EERROR);
  }
}

static void apply_read_program(lbm_value *args, lbm_uint nargs, eval_context_t *ctx) {
  apply_read_base(args,nargs,ctx,true,false);
}

static void apply_read_eval_program(lbm_value *args, lbm_uint nargs, eval_context_t *ctx) {
  ctx->flags |= EVAL_CPS_CONTEXT_FLAG_INCREMENTAL_READ;
  apply_read_base(args,nargs,ctx,true,true);
}

static void apply_read(lbm_value *args, lbm_uint nargs, eval_context_t *ctx) {
  apply_read_base(args,nargs,ctx,false,false);
}

static void apply_spawn_base(lbm_value *args, lbm_uint nargs, eval_context_t *ctx, uint32_t context_flags) {

  lbm_uint stack_size = EVAL_CPS_DEFAULT_STACK_SIZE;
  lbm_uint closure_pos = 0;
  char *name = NULL;

  if (nargs >= 2 &&
      lbm_is_number(args[0]) &&
      lbm_is_closure(args[1])) {
    stack_size = lbm_dec_as_u32(args[0]);
    closure_pos = 1;
  } else if (nargs >= 2 &&
             lbm_is_array_r(args[0]) &&
             lbm_is_closure(args[1])) {
    name = lbm_dec_str(args[0]);
    closure_pos = 1;
  }else if (nargs >= 3 &&
             lbm_is_array_r(args[0]) &&
             lbm_is_number(args[1]) &&
             lbm_is_closure(args[2])) {
    stack_size = lbm_dec_as_u32(args[1]);
    closure_pos = 2;
    name = lbm_dec_str(args[0]);
  }

  if (!lbm_is_closure(args[closure_pos]) ||
      nargs < 1) {
    error_ctx(ENC_SYM_EERROR);
  }

  lbm_value cl[3];
  extract_closure(args[closure_pos], cl);
  lbm_value curr_param = cl[CLO_PARAMS];
  lbm_value clo_env    = cl[CLO_ENV];
  lbm_uint i = closure_pos + 1;
  while (lbm_is_cons(curr_param) && i <= nargs) {
    lbm_value entry = cons_with_gc(get_car(curr_param), args[i], clo_env);
    lbm_value aug_env = cons_with_gc(entry, clo_env,ENC_SYM_NIL);
    clo_env = aug_env;
    curr_param = get_cdr(curr_param);
    i ++;
  }

  lbm_stack_drop(&ctx->K, nargs+1);

  lbm_value program = cons_with_gc(cl[CLO_BODY], ENC_SYM_NIL, clo_env);

  lbm_cid cid = lbm_create_ctx_parent(program,
                                      clo_env,
                                      stack_size,
                                      lbm_get_current_cid(),
                                      context_flags,
                                      name);
  ctx->r = lbm_enc_i(cid);
  ctx->app_cont = true;
}

static void apply_spawn(lbm_value *args, lbm_uint nargs, eval_context_t *ctx) {
  apply_spawn_base(args,nargs,ctx, EVAL_CPS_CONTEXT_FLAG_NOTHING);
}

static void apply_spawn_trap(lbm_value *args, lbm_uint nargs, eval_context_t *ctx) {
  apply_spawn_base(args,nargs,ctx, EVAL_CPS_CONTEXT_FLAG_TRAP);
}

static void apply_yield(lbm_value *args, lbm_uint nargs, eval_context_t *ctx) {
  if (is_atomic) {
    lbm_set_error_reason((char*)lbm_error_str_forbidden_in_atomic);
    error_ctx(ENC_SYM_EERROR);
  }
  if (nargs == 1 && lbm_is_number(args[0])) {
    lbm_uint ts = lbm_dec_as_u32(args[0]);
    lbm_stack_drop(&ctx->K, nargs+1);
    yield_ctx(ts);
  } else {
    error_ctx(ENC_SYM_EERROR);
  }
}

static void apply_wait(lbm_value *args, lbm_uint nargs, eval_context_t *ctx) {
  if (nargs == 1 && lbm_type_of(args[0]) == LBM_TYPE_I) {
    lbm_cid cid = (lbm_cid)lbm_dec_i(args[0]);
    lbm_stack_drop(&ctx->K, nargs+1);
    stack_push_2(&ctx->K, lbm_enc_i(cid), WAIT);
    ctx->r = ENC_SYM_TRUE;
    ctx->app_cont = true;
    yield_ctx(50000);
  } else {
    error_ctx(ENC_SYM_EERROR);
  }
}

static void apply_wait_for(lbm_value *args, lbm_uint nargs, eval_context_t *ctx) {
  if (nargs == 1 && lbm_is_number(args[0])) {
    uint32_t w = lbm_dec_as_u32(args[0]);
    lbm_stack_drop(&ctx->K, nargs+1);
    if (w != 0) {
      block_current_ctx(LBM_THREAD_STATE_BLOCKED, 0, w, true);
    } else {
      ctx->r = ENC_SYM_NIL;
      ctx->app_cont = true;
    }
  } else {
    error_ctx(ENC_SYM_EERROR);
  }
}

static void apply_eval(lbm_value *args, lbm_uint nargs, eval_context_t *ctx) {
  if ( nargs == 1) {
    ctx->curr_exp = args[0];
    lbm_stack_drop(&ctx->K, nargs+1);
  } else {
    lbm_set_error_reason((char*)lbm_error_str_num_args);
    error_ctx(ENC_SYM_EERROR);
  }
}

static void apply_eval_program(lbm_value *args, lbm_uint nargs, eval_context_t *ctx) {
  if (nargs == 1) {
    lbm_value prg = args[0];
    lbm_value app_cont;
    lbm_value app_cont_prg;
    lbm_value new_prg;
    lbm_value prg_copy;

    WITH_GC(prg_copy, lbm_list_copy(-1, prg));
    lbm_stack_drop(&ctx->K, nargs+1);

    if (ctx->K.sp > nargs+2) { // if there is a continuation
      app_cont = cons_with_gc(ENC_SYM_APP_CONT, ENC_SYM_NIL, prg_copy);
      app_cont_prg = cons_with_gc(app_cont, ENC_SYM_NIL, prg_copy);
      new_prg = lbm_list_append(app_cont_prg, ctx->program);
      new_prg = lbm_list_append(prg_copy, new_prg);
    } else {
      new_prg = lbm_list_append(prg_copy, ctx->program);
    }
    if (!lbm_is_list(new_prg)) {
      error_ctx(ENC_SYM_EERROR);
    }
    stack_push(&ctx->K, DONE);
    ctx->program = get_cdr(new_prg);
    ctx->curr_exp = get_car(new_prg);
  } else {
    lbm_set_error_reason((char*)lbm_error_str_num_args);
    error_ctx(ENC_SYM_EERROR);
  }
}

static void apply_send(lbm_value *args, lbm_uint nargs, eval_context_t *ctx) {
  if (nargs == 2) {
    if (lbm_type_of(args[0]) == LBM_TYPE_I) {
      lbm_cid cid = (lbm_cid)lbm_dec_i(args[0]);
      lbm_value msg = args[1];
      lbm_value status = lbm_find_receiver_and_send(cid, msg);
      /* return the status */
      lbm_stack_drop(&ctx->K, nargs+1);
      ctx->r = status;
      ctx->app_cont = true;
    } else {
      error_ctx(ENC_SYM_TERROR);
    }
  } else {
    lbm_set_error_reason((char*)lbm_error_str_num_args);
    error_ctx(ENC_SYM_EERROR);
  }
}

static void apply_ok(lbm_value *args, lbm_uint nargs, eval_context_t *ctx) {
  lbm_value ok_val = ENC_SYM_TRUE;
  if (nargs >= 1) {
    ok_val = args[0];
  }
  ctx->r = ok_val;
  ok_ctx();
}

static void apply_error(lbm_value *args, lbm_uint nargs, eval_context_t *ctx) {
  (void) ctx;
  lbm_value err_val = ENC_SYM_EERROR;
  if (nargs >= 1) {
    err_val = args[0];
  }
  error_ctx(err_val);
}

// (map f arg-list)
static void apply_map(lbm_value *args, lbm_uint nargs, eval_context_t *ctx) {
  if (nargs == 2 && lbm_is_list(args[1])) {
    if (lbm_is_symbol_nil(args[1])) {
      lbm_stack_drop(&ctx->K, 3);
      ctx->r = ENC_SYM_NIL;
      ctx->app_cont = true;
      return;
    }
    lbm_value *sptr = get_stack_ptr(ctx, 3);

    lbm_value f = args[0];
    lbm_value h = get_car(args[1]);
    lbm_value t = get_cdr(args[1]);

    lbm_value appli_1;
    lbm_value appli;
    WITH_GC(appli_1, lbm_heap_allocate_list(2));
    WITH_GC_RMBR_1(appli, lbm_heap_allocate_list(2), appli_1);

    lbm_value appli_0 = get_cdr(appli_1);

    lbm_set_car_and_cdr(appli_0, h, ENC_SYM_NIL);
    lbm_set_car(appli_1, ENC_SYM_QUOTE);

    lbm_set_car_and_cdr(get_cdr(appli), appli_1, ENC_SYM_NIL);
    lbm_set_car(appli, f);

    stack_push_4(&ctx->K, ENC_SYM_NIL, appli, appli_0, MAP_FIRST);
    sptr[0] = t;     // reuse stack space
    sptr[1] = ctx->curr_env;
    sptr[2] = ENC_SYM_NIL;
    ctx->curr_exp = appli;
  } else if (nargs == 1) {
    // Partial application, create a closure.
    lbm_uint sym;
    if (lbm_str_to_symbol("x", &sym)) {
      lbm_value *sptr = get_stack_ptr(ctx, 2);
      // Store params and body on stack temporarily to keep them safe from gc.
      sptr[0] = cons_with_gc(lbm_enc_sym(sym), ENC_SYM_NIL,ENC_SYM_NIL);
      WITH_GC(sptr[1], lbm_heap_allocate_list_init(3,
                                                ENC_SYM_MAP,
                                                args[0],
                                                lbm_enc_sym(sym)));
      ctx->r = allocate_closure(sptr[0], sptr[1], ENC_SYM_NIL);
      lbm_stack_drop(&ctx->K, 2);
      ctx->app_cont = true;
    } else {
      error_ctx(ENC_SYM_FATAL_ERROR);
    }
  } else {
    lbm_set_error_reason((char*)lbm_error_str_num_args);
    error_ctx(ENC_SYM_EERROR);
  }
}

static void apply_reverse(lbm_value *args, lbm_uint nargs, eval_context_t *ctx) {
  if (nargs == 1 && lbm_is_list(args[0])) {
    lbm_value curr = args[0];

    lbm_value new_list = ENC_SYM_NIL;
    while (lbm_is_cons(curr)) {
      lbm_value tmp = cons_with_gc(get_car(curr), new_list, ENC_SYM_NIL);
      new_list = tmp;
      curr = get_cdr(curr);
    }
    lbm_stack_drop(&ctx->K, 2);
    ctx->r = new_list;
    ctx->app_cont = true;
  } else {
    lbm_set_error_reason("Reverse requires a list argument");
    error_ctx(ENC_SYM_EERROR);
  }
}

static void apply_flatten(lbm_value *args, lbm_uint nargs, eval_context_t *ctx) {
  if (nargs == 1) {

    lbm_value v = flatten_value(args[0]);
    if ( v == ENC_SYM_MERROR) {
      gc();
      v = flatten_value(args[0]);
    }

    if (lbm_is_symbol(v)) {
      error_ctx(v);
    } else {
      lbm_stack_drop(&ctx->K, 2);
      ctx->r = v;
      ctx->app_cont = true;
    }
    return;
  }
  error_ctx(ENC_SYM_TERROR);
}

static void apply_unflatten(lbm_value *args, lbm_uint nargs, eval_context_t *ctx) {
  if(nargs == 1 && lbm_type_of(args[0]) == LBM_TYPE_ARRAY) {
    lbm_array_header_t *array;
    array = (lbm_array_header_t *)lbm_car(args[0]);

    lbm_flat_value_t fv;
    fv.buf = (uint8_t*)array->data;
    fv.buf_size = array->size;
    fv.buf_pos = 0;

    lbm_value res;

    ctx->r = ENC_SYM_NIL;
    if (lbm_unflatten_value(&fv, &res)) {
      ctx->r =  res;
    }
    lbm_stack_drop(&ctx->K, 2);
    ctx->app_cont = true;
    return;
  }
  error_ctx(ENC_SYM_TERROR);
}

static void apply_kill(lbm_value *args, lbm_uint nargs, eval_context_t *ctx) {
  if (nargs == 2 && lbm_is_number(args[0])) {
    lbm_cid cid = lbm_dec_as_i32(args[0]);

    if (ctx->id == cid) {
      ctx->r = args[1];
      finish_ctx();
      return;
    }
    mutex_lock(&qmutex);
    eval_context_t *found = NULL;
    found = lookup_ctx_nm(&blocked, cid);
    if (found)
      drop_ctx_nm(&blocked, found);
    else
      found = lookup_ctx_nm(&queue, cid);
    if (found)
      drop_ctx_nm(&queue, found);

    if (found) {
      found->K.data[found->K.sp - 1] = KILL;
      found->r = args[1];
      found->app_cont = true;
      enqueue_ctx_nm(&queue,found);
      ctx->r = ENC_SYM_TRUE;
    } else {
      ctx->r = ENC_SYM_NIL;
    }
    lbm_stack_drop(&ctx->K, 3);
    ctx->app_cont = true;
    mutex_unlock(&qmutex);
    return;
  }
  error_ctx(ENC_SYM_TERROR);
}

/***************************************************/
/* Application lookup table                        */

typedef void (*apply_fun)(lbm_value *, lbm_uint, eval_context_t *);
static const apply_fun fun_table[] =
  {
   apply_setvar,
   apply_read,
   apply_read_program,
   apply_read_eval_program,
   apply_spawn,
   apply_spawn_trap,
   apply_yield,
   apply_wait,
   apply_eval,
   apply_eval_program,
   apply_send,
   apply_ok,
   apply_error,
   apply_map,
   apply_reverse,
   apply_wait_for,
   apply_flatten,
   apply_unflatten,
   apply_kill,
  };

/***************************************************/
/* Application of function that takes arguments    */
/* passed over the stack.                          */


static void application(eval_context_t *ctx, lbm_value *fun_args, lbm_uint arg_count) {
  /* If arriving here, we know that the fun is a symbol.
   *  and can be a built in operation or an extension.
   */
  lbm_value fun = fun_args[0];

  lbm_uint fun_val = lbm_dec_sym(fun);
  lbm_uint apply_val = fun_val - APPLY_FUNS_START;
  lbm_uint fund_val  = fun_val - FUNDAMENTALS_START;

  if (apply_val <= (APPLY_FUNS_END - APPLY_FUNS_START)) {
    fun_table[apply_val](&fun_args[1], arg_count, ctx);
  } else if (fund_val <= (FUNDAMENTALS_END - FUNDAMENTALS_START)) {
    call_fundamental(fund_val, &fun_args[1], arg_count, ctx);
  } else {
    // It may be an extension
    extension_fptr f = lbm_get_extension(fun_val);
    if (f == NULL) {
      error_ctx(ENC_SYM_EERROR);
    }

    lbm_value ext_res;
    WITH_GC(ext_res, f(&fun_args[1], arg_count));
    if (lbm_is_error(ext_res)) { //Error other than merror
      error_ctx(ext_res);
    }
    lbm_stack_drop(&ctx->K, arg_count + 1);

    if (blocking_extension) {
      blocking_extension = false;
      if (blocking_extension_timeout) {
        block_current_ctx(LBM_THREAD_STATE_TIMEOUT, blocking_extension_timeout_us, 0, true);
      } else {
        block_current_ctx(LBM_THREAD_STATE_BLOCKED, 0, 0, true);
      }
      mutex_unlock(&blocking_extension_mutex);
    } else {
      ctx->app_cont = true;
      ctx->r = ext_res;
    }
  }
}

static void cont_closure_application_args(eval_context_t *ctx) {
  lbm_uint* sptr = get_stack_ptr(ctx, 5);

  lbm_value arg_env = (lbm_value)sptr[0];
  lbm_value exp     = (lbm_value)sptr[1];
  lbm_value clo_env = (lbm_value)sptr[2];
  lbm_value params  = (lbm_value)sptr[3];
  lbm_value args    = (lbm_value)sptr[4];

  lbm_value car_params, cdr_params;
  get_car_and_cdr(params, &car_params, &cdr_params);

  lbm_value ls;
  WITH_GC(ls, lbm_heap_allocate_list(2));
  lbm_value entry = ls;
  lbm_value aug_env = get_cdr(ls);
  lbm_cons_t *c1 = lbm_ref_cell(entry);
  c1->car = car_params;
  c1->cdr = ctx->r;
  lbm_cons_t *c2 = lbm_ref_cell(aug_env);
  c2->car = entry;
  c2->cdr = clo_env;
  clo_env = aug_env;

  bool a_nil = lbm_is_symbol_nil(args);
  bool p_nil = lbm_is_symbol_nil(cdr_params);

  if (!a_nil && !p_nil) {
    // evaluate the next argument.
    lbm_value car_args, cdr_args;
    get_car_and_cdr(args, &car_args, &cdr_args);
    sptr[2] = clo_env;
    sptr[3] = cdr_params;
    sptr[4] = cdr_args;
    stack_push(&ctx->K, CLOSURE_ARGS);
    ctx->curr_exp = car_args;
    ctx->curr_env = arg_env;
  } else if (a_nil && p_nil) {
    // Arguments and parameters match up in number
    lbm_stack_drop(&ctx->K, 5);
    ctx->curr_env = clo_env;
    ctx->curr_exp = exp;
    ctx->app_cont = false;
  } else if (!a_nil && p_nil) {
    // Application with extra arguments
    lbm_set_error_reason((char*)lbm_error_str_num_args);
    error_ctx(ENC_SYM_EERROR);
  } else {
    // Ran out of arguments, but there are still parameters.
    lbm_value new_env = lbm_list_append(arg_env,clo_env);
    sptr[0] = new_env; // keep safe from GC. Overwriting arg_env (safe as subset).
    ctx->r = allocate_closure(cdr_params, exp, new_env);
    lbm_stack_drop(&ctx->K, 5);
    ctx->app_cont = true;
  }
}

static void cont_application_args(eval_context_t *ctx) {
  lbm_uint *sptr = get_stack_ptr(ctx, 3);

  lbm_value env = sptr[0];
  lbm_value rest = sptr[1];
  lbm_value count = sptr[2];

  ctx->curr_env = env;
  sptr[0] = ctx->r; // Function 1st then Arguments
  if (lbm_is_cons(rest)) {
    lbm_cons_t *cell = lbm_ref_cell(rest);
    sptr[1] = env;
    sptr[2] = cell->cdr;
    stack_push_2(&ctx->K, count + (1 << LBM_VAL_SHIFT), APPLICATION_ARGS);
    ctx->curr_exp = cell->car;
  } else {
    // No more arguments
    lbm_stack_drop(&ctx->K, 2);
    lbm_uint nargs = lbm_dec_u(count);
    lbm_value *args = get_stack_ptr(ctx, (uint32_t)(nargs + 1));
    application(ctx,args, nargs);
  }
}

static void cont_and(eval_context_t *ctx) {
  lbm_value rest;
  lbm_value arg = ctx->r;
  lbm_pop(&ctx->K, &rest);
  if (lbm_is_symbol_nil(arg)) {
    ctx->app_cont = true;
    ctx->r = ENC_SYM_NIL;
  } else if (lbm_is_symbol_nil(rest)) {
    ctx->app_cont = true;
  } else {
    stack_push_2(&ctx->K, get_cdr(rest), AND);
    ctx->curr_exp = get_car(rest);
  }
}

static void cont_or(eval_context_t *ctx) {
  lbm_value rest;
  lbm_value arg = ctx->r;
  lbm_pop(&ctx->K, &rest);
  if (!lbm_is_symbol_nil(arg)) {
    ctx->app_cont = true;
  } else if (lbm_is_symbol_nil(rest)) {
    ctx->app_cont = true;
    ctx->r = ENC_SYM_NIL;
  } else {
    stack_push_2(&ctx->K, get_cdr(rest), OR);
    ctx->curr_exp = get_car(rest);
  }
}

static int fill_binding_location(lbm_value key, lbm_value value, lbm_value env) {
  if (lbm_type_of(key) == LBM_TYPE_SYMBOL) {
    if (key == ENC_SYM_DONTCARE) return FB_OK;
    lbm_env_modify_binding(env,key,value);
    return FB_OK;
  } else if (lbm_is_cons(key) &&
             lbm_is_cons(value)) {
    int r = fill_binding_location(get_car(key), get_car(value), env);
    if (r == FB_OK) {
      r = fill_binding_location(get_cdr(key), get_cdr(value), env);
    }
    return r;
  }
  return FB_TYPE_ERROR;
}

static void cont_bind_to_key_rest(eval_context_t *ctx) {

  lbm_value *sptr = get_stack_ptr(ctx, 4);

  lbm_value rest = sptr[1];
  lbm_value env  = sptr[2];
  lbm_value key  = sptr[3];

  if (fill_binding_location(key, ctx->r, env) < 0) {
    lbm_set_error_reason("Incorrect type of name/key in let-binding");
    error_ctx(ENC_SYM_TERROR);
  }

  if (lbm_is_cons(rest)) {
    lbm_value keyn = get_caar(rest);
    lbm_value valn_exp = get_cadr(get_car(rest));

    sptr[1] = get_cdr(rest);
    sptr[3] = keyn;
    stack_push(&ctx->K, BIND_TO_KEY_REST);
    ctx->curr_exp = valn_exp;
    ctx->curr_env = env;
  } else {
    // Otherwise evaluate the expression in the populated env
    ctx->curr_exp = sptr[0];
    ctx->curr_env = env;
    lbm_stack_drop(&ctx->K, 4);
  }
}

static void cont_if(eval_context_t *ctx) {

  lbm_value arg = ctx->r;

  lbm_value *sptr = pop_stack_ptr(ctx, 3);

  ctx->curr_env = sptr[2];
  if (lbm_is_symbol_nil(arg)) {
    ctx->curr_exp = sptr[0]; // else branch
  } else {
    ctx->curr_exp = sptr[1]; // then branch
  }
}

static void cont_match_many(eval_context_t *ctx) {

  lbm_value r = ctx->r;

  if (lbm_type_of(r) == LBM_TYPE_SYMBOL &&
      (lbm_dec_sym(r) == SYM_NO_MATCH)) {
    lbm_value *old_stack = get_stack_ptr(ctx, 3);
    lbm_value rest_msgs = old_stack[2];
    lbm_value pats = old_stack[1];
    lbm_value exp = old_stack[0];

    if (lbm_type_of(rest_msgs) == LBM_TYPE_SYMBOL &&
        lbm_dec_sym(rest_msgs) == SYM_NIL) {

      ctx->curr_exp = exp;

    } else {
      /* try match the next one */
      lbm_value car_rest_msgs;
      lbm_value cdr_rest_msgs;
      get_car_and_cdr(rest_msgs, &car_rest_msgs, &cdr_rest_msgs);
      lbm_uint *sptr = stack_reserve(ctx, 3);
      old_stack[2] = cdr_rest_msgs;
      sptr[0] = MATCH_MANY;
      sptr[1] = get_cdr(pats);
      sptr[2] = MATCH;
      ctx->r = car_rest_msgs;
      ctx->app_cont = true;
    }
  } else {
  /* I think the else branch will be "do nothing" here. */
  /* We should just continue executing with the result in ctx->r already*/
    lbm_stack_drop(&ctx->K, 3);
    ctx->app_cont = true;
  }
}

static void cont_match(eval_context_t *ctx) {
  lbm_value e = ctx->r;
  lbm_value patterns;
  lbm_value new_env;
  bool  do_gc = false;
  lbm_pop_2(&ctx->K, &new_env, &patterns); // restore enclosing environment
  ctx->curr_env = new_env;

  if (lbm_is_symbol_nil(patterns)) {
    /* no more patterns */
    ctx->r = ENC_SYM_NO_MATCH;
    ctx->app_cont = true;
  } else if (lbm_is_cons(patterns)) {
    lbm_value match_case = get_car(patterns);
    lbm_value pattern = get_car(match_case);
    lbm_value n1      = get_cadr(match_case);
    lbm_value n2      = get_cadr(get_cdr(match_case));
    lbm_value body;
    bool check_guard = false;
    if (lbm_is_symbol_nil(n2)) { // TODO: Not a very robust check.
      body = n1;
    } else {
      body = n2;
      check_guard = true;
    }

    bool is_match = match(pattern, e, &new_env, &do_gc);
    if (do_gc) {
      add_roots_2(patterns, e);
      gc();
      do_gc = false;
      new_env = ctx->curr_env;
      is_match = match(pattern, e, &new_env, &do_gc);
      if (do_gc) {
        error_ctx(ENC_SYM_MERROR);
      }
    }
    if (is_match) {
      if (check_guard) {
        lbm_value *sptr = stack_reserve(ctx,7);
        sptr[0] = get_cdr(patterns);
        sptr[1] = ctx->curr_env;
        sptr[2] = MATCH;
        sptr[3] = new_env;
        sptr[4] = body;
        sptr[5] = e;
        sptr[6] = MATCH_GUARD;
        ctx->curr_env = new_env;
        ctx->curr_exp = n1; // The guard
      } else {
        ctx->curr_env = new_env;
        ctx->curr_exp = body;
      }
    } else {
      /* set up for checking of next pattern */
      stack_push_3(&ctx->K, get_cdr(patterns),ctx->curr_env, MATCH);
      /* leave r unaltered */
      ctx->app_cont = true;
    }
  } else {
    error_ctx(ENC_SYM_TERROR);
  }
}

static void cont_exit_atomic(eval_context_t *ctx) {
  is_atomic --;
  ctx->app_cont = true;
}

static void cont_map_first(eval_context_t *ctx) {

  lbm_value *sptr = get_stack_ptr(ctx, 6);

  lbm_value ls  = sptr[0];
  lbm_value env = sptr[1];

  lbm_value elt = cons_with_gc(ctx->r, ENC_SYM_NIL, ENC_SYM_NIL);
  sptr[2] = elt; // head of result list
  sptr[3] = elt; // tail of result list
  if (lbm_is_cons(ls)) {
    lbm_value next, rest;
    get_car_and_cdr(ls, &next, &rest);
    sptr[0] = rest;
    stack_push(&ctx->K, MAP_REST);
    lbm_set_car(sptr[5], next); // new arguments
    ctx->curr_exp = sptr[4];
    ctx->curr_env = env;
  } else {
    ctx->r = sptr[2];
    ctx->curr_env = env;
    lbm_stack_drop(&ctx->K, 6);
    ctx->app_cont = true;
  }
}

static void cont_map_rest(eval_context_t *ctx) {
  lbm_value *sptr = get_stack_ptr(ctx, 6);

  lbm_value ls  = sptr[0];
  lbm_value env = sptr[1];
  lbm_value t   = sptr[3];
  lbm_value elt = cons_with_gc(ctx->r, ENC_SYM_NIL, ENC_SYM_NIL);
  lbm_set_cdr(t, elt);

  sptr[3] = elt; // update tail of result list.
  if (lbm_is_cons(ls)) {
    lbm_value next, rest;
    get_car_and_cdr(ls, &next, &rest);
    sptr[0] = rest;
    stack_push(&ctx->K, MAP_REST);
    lbm_set_car(sptr[5], next); // new arguments

    ctx->curr_exp = sptr[4];
    ctx->curr_env = env;
  } else {
    ctx->r = sptr[2]; //head of result list
    ctx->curr_env = env;
    lbm_stack_drop(&ctx->K, 6);
    ctx->app_cont = true;
  }
}

static void cont_match_guard(eval_context_t *ctx) {
  if (lbm_is_symbol_nil(ctx->r)) {
    lbm_value e;
    lbm_pop(&ctx->K, &e);
    lbm_stack_drop(&ctx->K, 2);
    ctx->r = e;
    ctx->app_cont = true;
  } else {
    lbm_value body;
    lbm_value env;
    lbm_stack_drop(&ctx->K, 1);
    lbm_pop_2(&ctx->K, &body, &env);
    lbm_stack_drop(&ctx->K, 3);
    ctx->curr_env = env;
    ctx->curr_exp = body;
  }
}

static void cont_terminate(eval_context_t *ctx) {
  error_ctx(ctx->r);
}

/****************************************************/
/*   READER                                         */

static void read_finish(lbm_char_channel_t *str, eval_context_t *ctx) {

  /* Tokenizer reached "end of file"
     The parser could be in a state where it needs
     more tokens to correctly finish an expression.

     Three cases
     1. The program / expression is malformed and the context should die.
     2. We are finished reading a program and should close off the
     internal representation with a closing parenthesis. Then
     apply continuation.
     3. We are finished reading an expression and should
     apply the continuation.


     In case 3, we should find the READ_DONE at sp - 1.
     In case 2, we should find the READ_DONE at sp - 5.

  */

  ctx->flags &= ~EVAL_CPS_CONTEXT_FLAG_INCREMENTAL_READ;

  if (lbm_is_symbol(ctx->r)) {
    lbm_uint sym_val = lbm_dec_sym(ctx->r);
    if (sym_val >= TOKENIZER_SYMBOLS_START &&
        sym_val <= TOKENIZER_SYMBOLS_END) {
      read_error_ctx(lbm_channel_row(str), lbm_channel_column(str));
    }
  }

  if (ctx->K.data[ctx->K.sp-1] == READ_DONE &&
      lbm_dec_u(ctx->K.data[ctx->K.sp-3]) == 0) {
    /* successfully finished reading an expression  (CASE 3) */
    ctx->app_cont = true;
  } else if (ctx->K.sp > 4  && ctx->K.data[ctx->K.sp - 4] == READ_DONE) {
    lbm_value env;
    lbm_value s;
    lbm_value sym;
    lbm_pop_3(&ctx->K, &sym, &env, &s);
    ctx->curr_env = env;
    ctx->app_cont = true; // Program evaluated and result is in ctx->r.
  } else if (ctx->K.sp > 5 && ctx->K.data[ctx->K.sp - 5] == READ_DONE) {
    /* successfully finished reading a program  (CASE 2) */
    ctx->r = ENC_SYM_CLOSEPAR;
    ctx->app_cont = true;
  } else {
    /* Parsing failed */
    if (lbm_channel_row(str) == 1 &&
        lbm_channel_column(str) == 1 ){
      // eof at empty stream.
      ctx->r = ENC_SYM_NIL;
      ctx->app_cont = true;
    } else {
      lbm_set_error_reason((char*)lbm_error_str_parse_eof);
      read_error_ctx(lbm_channel_row(str), lbm_channel_column(str));
    }
    lbm_channel_reader_close(str);
  }
}

/* cont_read_next_token
   sp-2 : Stream
   sp-1 : Grab row
*/
static void cont_read_next_token(eval_context_t *ctx) {
  lbm_value *sptr = get_stack_ptr(ctx, 2);
  lbm_value stream = sptr[0];
  lbm_value grab_row0 = sptr[1];

  lbm_char_channel_t *chan = lbm_dec_channel(stream);
  if (chan == NULL || chan->state == NULL) {
    error_ctx(ENC_SYM_FATAL_ERROR);
  }

  if (!lbm_channel_more(chan) && lbm_channel_is_empty(chan)) {
    lbm_stack_drop(&ctx->K, 2);
    read_finish(chan, ctx);
    return;
  }
  /* Eat whitespace and comments */
  if (!tok_clean_whitespace(chan)) {
    sptr[0] = stream;
    sptr[1] = lbm_enc_u(0);
    stack_push(&ctx->K, READ_NEXT_TOKEN);
    yield_ctx(EVAL_CPS_MIN_SLEEP);
    return;
  }
  /* After eating whitespace we may be at end of file/stream */
  if (!lbm_channel_more(chan) && lbm_channel_is_empty(chan)) {
    lbm_stack_drop(&ctx->K, 2);
    read_finish(chan, ctx);
    return;
  }

  if (lbm_dec_u(grab_row0)) {
    ctx->row0 = (int32_t)lbm_channel_row(chan);
  }

  /* Attempt to extract tokens from the character stream */
  int n = 0;
  lbm_value res;
  unsigned int string_len = 0;

  /*
   * SYNTAX
   */
  uint32_t match;
  n = tok_syntax(chan, &match);
  if (n > 0) {
    if (!lbm_channel_drop(chan, (unsigned int)n)) {
      error_ctx(ENC_SYM_FATAL_ERROR);
    }
    ctx->app_cont = true;
    lbm_uint do_next;
    switch(match) {
    case TOKOPENPAR:
      sptr[0] = ENC_SYM_NIL;
      sptr[1] = ENC_SYM_NIL;
      stack_push_2(&ctx->K,
                   stream,
                   READ_APPEND_CONTINUE);
      stack_push_3(&ctx->K, stream, lbm_enc_u(0), READ_NEXT_TOKEN);
      ctx->r = ENC_SYM_OPENPAR;
      return;
    case TOKCLOSEPAR: {
      lbm_stack_drop(&ctx->K, 2);
      ctx->r = ENC_SYM_CLOSEPAR;
    } return;
    case TOKOPENBRACK:
      sptr[0] = stream;
      sptr[1] = READ_START_ARRAY;
      //stack_push_2(&ctx->K, stream, READ_START_ARRAY);
      stack_push_3(&ctx->K, stream, lbm_enc_u(0), READ_NEXT_TOKEN);
      ctx->r = ENC_SYM_OPENBRACK;
      return;
    case TOKCLOSEBRACK:
      lbm_stack_drop(&ctx->K, 2);
      ctx->r = ENC_SYM_CLOSEBRACK;
      return;
    case TOKDOT:
      lbm_stack_drop(&ctx->K, 2);
      ctx->r = ENC_SYM_DOT;
      return;
    case TOKDONTCARE:
      lbm_stack_drop(&ctx->K, 2);
      ctx->r = ENC_SYM_DONTCARE;
      return;
    case TOKQUOTE:
      do_next = READ_QUOTE_RESULT;
      break;
    case TOKBACKQUOTE:
      sptr[0] = QQ_EXPAND_START;
      sptr[1] = stream;
      stack_push_2(&ctx->K, lbm_enc_u(0), READ_NEXT_TOKEN);
      ctx->app_cont = true;
      return;
    case TOKCOMMAAT:
      do_next = READ_COMMAAT_RESULT;
      break;
    case TOKCOMMA:
      do_next = READ_COMMA_RESULT;
      break;
    case TOKMATCHANY:
      lbm_stack_drop(&ctx->K, 2);
      ctx->r = ENC_SYM_MATCH_ANY;
      return;
    case TOKOPENCURL:
      sptr[0] = ENC_SYM_NIL;
      sptr[1] = ENC_SYM_NIL;
      stack_push_2(&ctx->K,
                   stream,
                   READ_APPEND_CONTINUE);
      ctx->r = ENC_SYM_PROGN;
      return;
    case TOKCLOSECURL:
      lbm_stack_drop(&ctx->K, 2);
      ctx->r = ENC_SYM_CLOSEPAR;
      return;
    case TOKCONSTSTART:
      ctx->flags |= EVAL_CPS_CONTEXT_FLAG_CONST;
      sptr[0] = stream;
      sptr[1] = lbm_enc_u(0);
      stack_push(&ctx->K, READ_NEXT_TOKEN);
      ctx->app_cont = true;
      return;
    case TOKCONSTEND:
      ctx->flags &= ~EVAL_CPS_CONTEXT_FLAG_CONST;
      sptr[0] = stream;
      sptr[1] = lbm_enc_u(0);
      stack_push(&ctx->K, READ_NEXT_TOKEN);
      ctx->app_cont = true;
      return;
    case TOKCONSTSYMSTR:
      ctx->flags |= EVAL_CPS_CONTEXT_FLAG_CONST_SYMBOL_STRINGS;
      sptr[0] = stream;
      sptr[1] = lbm_enc_u(0);
      stack_push(&ctx->K, READ_NEXT_TOKEN);
      ctx->app_cont = true;
      return;
    default:
      read_error_ctx(lbm_channel_row(chan), lbm_channel_column(chan));
    }
    sptr[0] = do_next;
    sptr[1] = stream;
    stack_push_2(&ctx->K, lbm_enc_u(0), READ_NEXT_TOKEN);
    ctx->app_cont = true;
    return;
  } else if (n < 0) goto retry_token;

  /*
   *  STRING
   */
  n = tok_string(chan, &string_len);
  if (n >= 2) {
    lbm_channel_drop(chan, (unsigned int)n);
    if (!lbm_heap_allocate_array(&res, (unsigned int)(string_len+1))) {
      gc();
      if (!lbm_heap_allocate_array(&res, (unsigned int)(string_len+1))) {
        error_ctx(ENC_SYM_MERROR);
      }
    }
    lbm_array_header_t *arr = (lbm_array_header_t*)get_car(res);
    char *data = (char*)arr->data;
    memset(data,0, string_len + 1);
    memcpy(data, tokpar_sym_str, string_len);
    lbm_stack_drop(&ctx->K, 2);
    ctx->r = res;
    ctx->app_cont = true;
    return;
  } else if (n < 0) goto retry_token;

  /*
   * FLOAT
   */
  token_float f_val;
  n = tok_double(chan, &f_val);
  if (n > 0) {
    lbm_channel_drop(chan, (unsigned int) n);
    switch(f_val.type) {
    case TOKTYPEF32:
      WITH_GC(res, lbm_enc_float((float)f_val.value));
      break;
    case TOKTYPEF64:
      res = lbm_enc_double(f_val.value);
      break;
    default:
      read_error_ctx(lbm_channel_row(chan), lbm_channel_column(chan));
    }
    lbm_stack_drop(&ctx->K, 2);
    ctx->r = res;
    ctx->app_cont = true;
    return;
  } else if (n < 0) goto retry_token;

  /*
   * INTEGER
   */
  token_int int_result;
  n = tok_integer(chan, &int_result);
  if (n > 0) {
    lbm_channel_drop(chan, (unsigned int)n);
    switch(int_result.type) {
    case TOKTYPEBYTE:
      res = lbm_enc_char((char)(int_result.negative ? -int_result.value : int_result.value));
      break;
    case TOKTYPEI:
      res = lbm_enc_i((lbm_int)(int_result.negative ? -int_result.value : int_result.value));
      break;
    case TOKTYPEU:
      res = lbm_enc_u((lbm_uint)(int_result.negative ? -int_result.value : int_result.value));
      break;
    case TOKTYPEI32:
      WITH_GC(res, lbm_enc_i32((int32_t)(int_result.negative ? -int_result.value : int_result.value)));
      break;
    case TOKTYPEU32:
      WITH_GC(res,lbm_enc_u32((uint32_t)(int_result.negative ? -int_result.value : int_result.value)));
      break;
    case TOKTYPEI64:
      WITH_GC(res,lbm_enc_i64((int64_t)(int_result.negative ? -int_result.value : int_result.value)));
      break;
    case TOKTYPEU64:
      WITH_GC(res,lbm_enc_u64((uint64_t)(int_result.negative ? -int_result.value : int_result.value)));
      break;
    default:
      read_error_ctx(lbm_channel_row(chan), lbm_channel_column(chan));
    }
    lbm_stack_drop(&ctx->K, 2);
    ctx->r = res;
    ctx->app_cont = true;
    return;
  } else if (n < 0) goto retry_token;

  /*
   * SYMBOL
   */
  n = tok_symbol(chan);
  if (n > 0) {
    lbm_channel_drop(chan, (unsigned int) n);
    lbm_uint symbol_id;

    if (lbm_get_symbol_by_name(tokpar_sym_str, &symbol_id)) {
      res = lbm_enc_sym(symbol_id);
    } else {
      int r = 0;
      if (strncmp(tokpar_sym_str,"ext-",4) == 0) {
        r = lbm_add_extension_symbol(tokpar_sym_str, &symbol_id);
      } else if (tokpar_sym_str[0] == '#') {
        r = lbm_add_variable_symbol(tokpar_sym_str, &symbol_id);
      } else {
        if (ctx->flags & EVAL_CPS_CONTEXT_FLAG_CONST_SYMBOL_STRINGS &&
            ctx->flags & EVAL_CPS_CONTEXT_FLAG_INCREMENTAL_READ) {
          r = lbm_add_symbol_flash(tokpar_sym_str, &symbol_id);
        } else {
          r = lbm_add_symbol(tokpar_sym_str, &symbol_id);
          if (!r) {
            gc();
            r = lbm_add_symbol(tokpar_sym_str, &symbol_id);
          }
        }
      }
      if (r) {
        res = lbm_enc_sym(symbol_id);
      } else {
        read_error_ctx(lbm_channel_row(chan), lbm_channel_column(chan));
      }
    }
    lbm_stack_drop(&ctx->K, 2);
    ctx->r = res;
    ctx->app_cont = true;
    return;
  } else if (n < 0) goto retry_token;

  /*
   * CHAR
   */
  char c_val;
  n = tok_char(chan, &c_val);
  if(n > 0) {
    lbm_channel_drop(chan,(unsigned int) n);
    lbm_stack_drop(&ctx->K, 2);
    ctx->r = lbm_enc_char(c_val);
    ctx->app_cont = true;
    return;
  }else if (n < 0) goto retry_token;

  read_error_ctx(lbm_channel_row(chan), lbm_channel_column(chan));

 retry_token:
  if (n == TOKENIZER_NEED_MORE) {
    sptr[0] = stream;
    sptr[1] = lbm_enc_u(0);
    stack_push(&ctx->K, READ_NEXT_TOKEN);
    yield_ctx(EVAL_CPS_MIN_SLEEP);
    return;
  }
  read_error_ctx(lbm_channel_row(chan), lbm_channel_column(chan));
}

static void cont_read_start_array(eval_context_t *ctx) {
  lbm_value *sptr = get_stack_ptr(ctx, 1);
  lbm_value stream = sptr[0];

  lbm_char_channel_t *str = lbm_dec_channel(stream);
  if (str == NULL || str->state == NULL) {
    error_ctx(ENC_SYM_FATAL_ERROR);
  }

  lbm_uint num_free = lbm_memory_longest_free();
  lbm_uint initial_size = (lbm_uint)((float)num_free * 0.9);
  if (initial_size == 0) {
    gc();
    num_free = lbm_memory_longest_free();
    initial_size = (lbm_uint)((float)num_free * 0.9);
    if (initial_size == 0) {
      lbm_channel_reader_close(str);
      error_ctx(ENC_SYM_MERROR);
    }
  }

  if (lbm_is_number(ctx->r)) {
    lbm_value array;
    initial_size = sizeof(lbm_uint) * initial_size;

    if (!lbm_heap_allocate_array(&array, initial_size)) {
      lbm_set_error_reason("Out of memory while reading.");
      lbm_channel_reader_close(str);
      error_ctx(ENC_SYM_FATAL_ERROR);
    }

    sptr[0] = array;
    stack_push_3(&ctx->K, lbm_enc_u(initial_size), lbm_enc_u(0), stream);
    stack_push(&ctx->K, READ_APPEND_ARRAY);
    ctx->app_cont = true;
  } else {
    lbm_channel_reader_close(str);
    read_error_ctx(lbm_channel_row(str), lbm_channel_column(str));
  }
}

static void cont_read_append_array(eval_context_t *ctx) {
  lbm_uint *sptr = get_stack_ptr(ctx, 4);

  lbm_value array  = sptr[0];
  lbm_value size   = lbm_dec_as_u32(sptr[1]);
  lbm_value ix     = lbm_dec_as_u32(sptr[2]);
  lbm_value stream = sptr[3];

  if (ix >= (size - 1)) {
    error_ctx(ENC_SYM_MERROR);
  }

  lbm_array_header_t *arr = (lbm_array_header_t*)get_car(array); // TODO: Check

  if (lbm_is_number(ctx->r)) {
    ((uint8_t*)arr->data)[ix] = (uint8_t)lbm_dec_as_u32(ctx->r);

    sptr[2] = lbm_enc_u(ix + 1);
    stack_push_4(&ctx->K, READ_APPEND_ARRAY, stream, lbm_enc_u(0), READ_NEXT_TOKEN);
    ctx->app_cont = true;
  } else if (lbm_is_symbol(ctx->r) && lbm_dec_sym(ctx->r) == SYM_CLOSEBRACK) {
    lbm_uint array_size = ix / sizeof(lbm_uint);

    if (ix % sizeof(lbm_uint) != 0) {
      array_size = array_size + 1;
    }
    lbm_memory_shrink((lbm_uint*)arr->data, array_size);
    arr->size = ix;
    lbm_stack_drop(&ctx->K, 4);
    ctx->r = array;
    ctx->app_cont = true;
  } else {
    error_ctx(ENC_SYM_TERROR);
  }
}

static void cont_read_append_continue(eval_context_t *ctx) {
  lbm_value *sptr = get_stack_ptr(ctx, 3);

  lbm_value first_cell = sptr[0];
  lbm_value last_cell  = sptr[1];
  lbm_value stream     = sptr[2];

  lbm_char_channel_t *str = lbm_dec_channel(stream);
  if (str == NULL || str->state == NULL) {
    error_ctx(ENC_SYM_FATAL_ERROR);
  }

  if (lbm_type_of(ctx->r) == LBM_TYPE_SYMBOL) {

    switch(lbm_dec_sym(ctx->r)) {
    case SYM_CLOSEPAR:
      if (lbm_type_of(last_cell) == LBM_TYPE_CONS) {
        lbm_set_cdr(last_cell, ENC_SYM_NIL); // terminate the list
        ctx->r = first_cell;
      } else {
        ctx->r = ENC_SYM_NIL;
      }
      lbm_stack_drop(&ctx->K, 3);
      /* Skip reading another token and apply the continuation */
      ctx->app_cont = true;
      return;
    case SYM_DOT:
      stack_push(&ctx->K, READ_DOT_TERMINATE);
      stack_push_3(&ctx->K, stream, lbm_enc_u(0), READ_NEXT_TOKEN);
      ctx->app_cont = true;
      return;
    }
  }
  lbm_value new_cell = cons_with_gc(ctx->r, ENC_SYM_NIL, ENC_SYM_NIL);
  if (lbm_is_symbol_merror(new_cell)) {
    lbm_channel_reader_close(str);
    read_error_ctx(lbm_channel_row(str), lbm_channel_column(str));
    return;
  }
  if (lbm_type_of(last_cell) == LBM_TYPE_CONS) {
    lbm_set_cdr(last_cell, new_cell);
    last_cell = new_cell;
  } else {
    first_cell = last_cell = new_cell;
  }
  sptr[0] = first_cell;
  sptr[1] = last_cell;
  sptr[2] = stream;    // unchanged.
  stack_push(&ctx->K, READ_APPEND_CONTINUE);
  stack_push_3(&ctx->K, stream, lbm_enc_u(0), READ_NEXT_TOKEN);
  ctx->app_cont = true;
}

static void cont_read_eval_continue(eval_context_t *ctx) {
  lbm_value env;
  lbm_value stream;
  lbm_pop_2(&ctx->K, &env, &stream);

  lbm_char_channel_t *str = lbm_dec_channel(stream);
  if (str == NULL || str->state == NULL) {
    error_ctx(ENC_SYM_FATAL_ERROR);
  }

  ctx->row1 = (lbm_int)str->row(str);

  if (lbm_type_of(ctx->r) == LBM_TYPE_SYMBOL) {

    switch(lbm_dec_sym(ctx->r)) {
    case SYM_CLOSEPAR:
      ctx->app_cont = true;
      return;
    case SYM_DOT:
      stack_push(&ctx->K, READ_DOT_TERMINATE);
      stack_push_3(&ctx->K, stream, lbm_enc_u(0), READ_NEXT_TOKEN);
      ctx->app_cont = true;
      return;
    }
  }

  stack_push_3(&ctx->K, stream, env, READ_EVAL_CONTINUE);
  stack_push_3(&ctx->K, stream, lbm_enc_u(1), READ_NEXT_TOKEN);

  ctx->app_cont = false;
  ctx->curr_env = env;
  ctx->curr_exp = ctx->r;
}

static void cont_read_expect_closepar(eval_context_t *ctx) {
  lbm_value res;
  lbm_value stream;

  lbm_pop_2(&ctx->K, &res, &stream);

  lbm_char_channel_t *str = lbm_dec_channel(stream);
  if (str == NULL || str->state == NULL) {
    error_ctx(ENC_SYM_FATAL_ERROR);
  }

  if (lbm_type_of(ctx->r) == LBM_TYPE_SYMBOL &&
      lbm_dec_sym(ctx->r) == SYM_CLOSEPAR) {
    ctx->r = res;
    ctx->app_cont = true;
  } else {
    lbm_channel_reader_close(str);
    lbm_set_error_reason((char*)lbm_error_str_parse_close);
    read_error_ctx(lbm_channel_row(str), lbm_channel_column(str));
  }
}

static void cont_read_dot_terminate(eval_context_t *ctx) {
  lbm_value *sptr = get_stack_ptr(ctx, 3);

  lbm_value first_cell = sptr[0];
  lbm_value last_cell  = sptr[1];
  lbm_value stream = sptr[2];

  lbm_char_channel_t *str = lbm_dec_channel(stream);
  if (str == NULL || str->state == NULL) {
    error_ctx(ENC_SYM_FATAL_ERROR);
  }

  lbm_stack_drop(&ctx->K ,3);

  if (lbm_type_of(ctx->r) == LBM_TYPE_SYMBOL &&
      (lbm_dec_sym(ctx->r) == SYM_CLOSEPAR ||
       lbm_dec_sym(ctx->r) == SYM_DOT)) {
    lbm_channel_reader_close(str);
    lbm_set_error_reason((char*)lbm_error_str_parse_dot);
    read_error_ctx(lbm_channel_row(str), lbm_channel_column(str));
  } else {
    if (lbm_is_cons(last_cell)) {
      lbm_set_cdr(last_cell, ctx->r);
      ctx->r = first_cell;
      stack_push_3(&ctx->K,
                   stream,
                   ctx->r,
                   READ_EXPECT_CLOSEPAR);
      stack_push_3(&ctx->K, stream, lbm_enc_u(0), READ_NEXT_TOKEN);
      ctx->app_cont = true;
    } else {
      lbm_channel_reader_close(str);
      lbm_set_error_reason((char*)lbm_error_str_parse_dot);
      read_error_ctx(lbm_channel_row(str), lbm_channel_column(str));
    }
  }
}

static void cont_read_done(eval_context_t *ctx) {
  lbm_value stream;
  lbm_pop(&ctx->K, &stream);

  lbm_char_channel_t *str = lbm_dec_channel(stream);
  if (str == NULL || str->state == NULL) {
    error_ctx(ENC_SYM_FATAL_ERROR);
  }

  lbm_channel_reader_close(str);
  if (lbm_is_symbol(ctx->r)) {
    lbm_uint sym_val = lbm_dec_sym(ctx->r);
    if (sym_val >= TOKENIZER_SYMBOLS_START &&
        sym_val <= TOKENIZER_SYMBOLS_END) {
      read_error_ctx(lbm_channel_row(str), lbm_channel_column(str));
    }
  }

  ctx->row0 = -1;
  ctx->row1 = -1;
  ctx->app_cont = true;
}

static void cont_read_quote_result(eval_context_t *ctx) {
  lbm_value cell;
  WITH_GC(cell, lbm_heap_allocate_list_init(2,
                                            ENC_SYM_QUOTE,
                                            ctx->r));
  ctx->r = cell;
  ctx->app_cont = true;
}

static void cont_read_commaat_result(eval_context_t *ctx) {
  lbm_value cell2 = cons_with_gc(ctx->r,ENC_SYM_NIL, ENC_SYM_NIL);
  lbm_value cell1 = cons_with_gc(ENC_SYM_COMMAAT, cell2, ENC_SYM_NIL);
  ctx->r = cell1;
  ctx->app_cont = true;
}

static void cont_read_comma_result(eval_context_t *ctx) {
  lbm_value cell2 = cons_with_gc(ctx->r,ENC_SYM_NIL,ENC_SYM_NIL);
  lbm_value cell1 = cons_with_gc(ENC_SYM_COMMA, cell2, ENC_SYM_NIL);
  ctx->r = cell1;
  ctx->app_cont = true;
}

static void cont_application_start(eval_context_t *ctx) {

  /* sptr[0] = env
   * sptr[1] = args
   * ctx->r  = function
   */

  if (lbm_is_symbol(ctx->r)) {
    stack_push(&ctx->K, lbm_enc_u(0));
    cont_application_args(ctx);
  } else if (lbm_is_cons(ctx->r)) {
    lbm_uint *sptr = get_stack_ptr(ctx, 2);
    lbm_value args = (lbm_value)sptr[1];
    switch (get_car(ctx->r)) {
    case ENC_SYM_CLOSURE: {
      lbm_value cl[3];
      extract_closure(ctx->r, cl);
      lbm_value arg_env = (lbm_value)sptr[0];
      lbm_value arg0, arg_rest;
      get_car_and_cdr(args, &arg0, &arg_rest);
      sptr[1] = cl[CLO_BODY];
      if (lbm_is_symbol_nil(args)) {
        if (lbm_is_symbol_nil(cl[CLO_PARAMS])) {
          // No param closure
          ctx->curr_exp = cl[CLO_BODY];
          ctx->curr_env = cl[CLO_ENV];
          ctx->app_cont = false;
        } else {
          ctx->app_cont = true;
        }
        lbm_stack_drop(&ctx->K, 2);
      } else {
        lbm_value *reserved = stack_reserve(ctx, 4);
        reserved[0] = cl[CLO_ENV];
        reserved[1] = cl[CLO_PARAMS];
        reserved[2] = arg_rest;
        reserved[3] = CLOSURE_ARGS;
        ctx->curr_exp = arg0;
        ctx->curr_env = arg_env;
        ctx->app_cont = false;
      }
    } break;
    case ENC_SYM_CONT:{
      /* Continuation created using call-cc.
       * ((SYM_CONT . cont-array) arg0 )
       */
      lbm_value c = get_cdr(ctx->r); /* should be the continuation array*/

      if (!lbm_is_array_r(c)) {
        error_ctx(ENC_SYM_FATAL_ERROR);
      }

      lbm_uint arg_count = lbm_list_length(args);
      lbm_value arg = ENC_SYM_NIL;
      switch (arg_count) {
      case 0:
        arg = ENC_SYM_NIL;
        break;
      case 1:
        arg = get_car(args);
        break;
      default:
        lbm_set_error_reason((char*)lbm_error_str_num_args);
        error_ctx(ENC_SYM_EERROR);
      }
      lbm_stack_clear(&ctx->K);

      lbm_array_header_t *arr = (lbm_array_header_t*)get_car(c);

      ctx->K.sp = arr->size / sizeof(lbm_uint);
      memcpy(ctx->K.data, arr->data, arr->size);

      ctx->curr_exp = arg;
      ctx->app_cont = false;
      break;
    }
    case ENC_SYM_MACRO:{
      /*
       * Perform macro expansion.
       * Macro expansion is really just evaluation in an
       * environment augmented with the unevaluated expressions passed
       * as arguments.
       */
      lbm_value env = (lbm_value)sptr[0];

      lbm_value curr_param = get_cadr(ctx->r);
      lbm_value curr_arg = args;
      lbm_value expand_env = env;
      while (lbm_is_cons(curr_param) &&
             lbm_is_cons(curr_arg)) {
        lbm_value car_curr_param, cdr_curr_param;
        lbm_value car_curr_arg, cdr_curr_arg;
        get_car_and_cdr(curr_param, &car_curr_param, &cdr_curr_param);
        get_car_and_cdr(curr_arg, &car_curr_arg, &cdr_curr_arg);

        lbm_value entry = cons_with_gc(car_curr_param, car_curr_arg, expand_env);
        lbm_value aug_env = cons_with_gc(entry, expand_env,ENC_SYM_NIL);
        expand_env = aug_env;

        curr_param = cdr_curr_param;
        curr_arg   = cdr_curr_arg;
      }
      /* Two rounds of evaluation is performed.
       * First to instantiate the arguments into the macro body.
       * Second to evaluate the resulting program.
       */
      sptr[1] = EVAL_R;
      lbm_value exp = get_cadr(get_cdr(ctx->r));
      ctx->curr_exp = exp;
      ctx->curr_env = expand_env;

      ctx->app_cont = false;
    } break;
    default:
      error_ctx(ENC_SYM_EERROR);
    }
  } else {
    error_ctx(ENC_SYM_EERROR);
  }
}

static void cont_eval_r(eval_context_t* ctx) {
  lbm_value env;
  lbm_pop(&ctx->K, &env);
  ctx->curr_exp = ctx->r;
  ctx->curr_env = env;
  ctx->app_cont = false;
}

/* progn + var stack
   sp-5 : env
   sp-4 : 0
   sp-3 : rest
   sp-2 : PROGN_REST
   sp-1 : symbol
 */
static void cont_progn_var(eval_context_t* ctx) {

  lbm_value sym;

  lbm_pop(&ctx->K, &sym);

  if (ctx->K.sp >= 4) { // could potentially be inside of an progn
    lbm_value sv = ctx->K.data[ctx->K.sp - 1];
    if (IS_CONTINUATION(sv) &&
        (sv == PROGN_REST)) {
      lbm_uint  sp = ctx->K.sp;
      uint32_t  is_copied = lbm_dec_as_u32(ctx->K.data[sp - 3]);
      if (is_copied == 0) {
        lbm_value env;
        WITH_GC(env, lbm_env_copy_spine(ctx->K.data[sp - 4]));
        ctx->K.data[sp - 3] = lbm_enc_u(1);
        ctx->K.data[sp - 4] = env;
      }
      lbm_value new_env = ctx->K.data[sp - 4];
      lbm_value tmp;
      WITH_GC(tmp, lbm_env_set_functional(new_env, sym, ctx->r));
      ctx->K.data[ctx->K.sp - 4] = tmp;
      ctx->app_cont = true; // return to progn body
      return;
    }
  }
  lbm_set_error_reason((char*)lbm_error_str_var_outside_progn);
  error_ctx(ENC_SYM_EERROR);
}

static void cont_setq(eval_context_t *ctx) {
  lbm_value sym;
  lbm_pop(&ctx->K, &sym);
  lbm_value res;
  WITH_GC(res, perform_setvar(sym, ctx->r, ctx->curr_env));
  ctx->r = res;
  ctx->app_cont = true;
}

lbm_flash_status request_flash_storage_cell(lbm_value val, lbm_value *res) {

  lbm_value flash_cell;
  lbm_flash_status s = lbm_allocate_const_cell(&flash_cell);
  if (s != LBM_FLASH_WRITE_OK)
    return s;
  lbm_value new_val = val;
  new_val &= ~LBM_PTR_VAL_MASK; // clear the value part of the ptr
  new_val |= (flash_cell & LBM_PTR_VAL_MASK);
  new_val |= LBM_PTR_TO_CONSTANT_BIT;
  *res = new_val;
  return s;
}

static void cont_move_to_flash(eval_context_t *ctx) {

  lbm_value args;
  lbm_pop(&ctx->K, &args);

  if (lbm_is_symbol_nil(args)) {
    // Done looping over arguments. return true.
    ctx->r = ENC_SYM_TRUE;
    ctx->app_cont = true;
    return;
  }

  lbm_value first_arg, rest;
  get_car_and_cdr(args, &first_arg, &rest);

  lbm_value val;
  if (lbm_is_symbol(first_arg) && lbm_env_lookup_b(&val, first_arg, lbm_get_env())) {
    // Prepare to copy the rest of the arguments when done with first.
    stack_push_2(&ctx->K, rest, MOVE_TO_FLASH);
    if (lbm_is_ptr(val) &&
        (!(val & LBM_PTR_TO_CONSTANT_BIT))) {
      stack_push_2(&ctx->K, first_arg, SET_GLOBAL_ENV);
      stack_push(&ctx->K, MOVE_VAL_TO_FLASH_DISPATCH);
      ctx->r = val;
    }
    ctx->app_cont = true;
    return;
  }
  error_ctx(ENC_SYM_EERROR);
}

static void cont_move_val_to_flash_dispatch(eval_context_t *ctx) {

  lbm_value val = ctx->r;

  if (lbm_is_cons(val)) {
    lbm_value flash_cell = ENC_SYM_NIL;
    handle_flash_status(request_flash_storage_cell(val, &flash_cell));
    stack_push_4(&ctx->K, flash_cell, flash_cell, get_cdr(val), MOVE_LIST_TO_FLASH);
    stack_push(&ctx->K, MOVE_VAL_TO_FLASH_DISPATCH);
    ctx->r = get_car(val);
    ctx->app_cont = true;
    return;
  }

  if (lbm_is_ptr(val) && (val & LBM_PTR_TO_CONSTANT_BIT)) {
    ctx->r = val;
    ctx->app_cont = true;
    return;
  }

  if (lbm_is_ptr(val)) {
    // Request a flash storage cell.
    lbm_value flash_cell = ENC_SYM_NIL;
    handle_flash_status(request_flash_storage_cell(val, &flash_cell));
    ctx->r = flash_cell;
    lbm_cons_t *ref = lbm_ref_cell(val);
    if (lbm_type_of(ref->cdr) == LBM_TYPE_SYMBOL) {
      switch (lbm_dec_sym(ref->cdr)) {
      case SYM_RAW_I_TYPE: /* fall through */
      case SYM_RAW_U_TYPE:
      case SYM_RAW_F_TYPE:
        handle_flash_status(write_const_car(flash_cell, ref->car));
        handle_flash_status(write_const_cdr(flash_cell, ref->cdr));
        break;
      case SYM_IND_I_TYPE: /* fall through */
      case SYM_IND_U_TYPE:
      case SYM_IND_F_TYPE: {
#ifndef LBM64
        /* 64 bit values are in lbm mem on 32bit platforms. */
        lbm_uint *lbm_mem_ptr = (lbm_uint*)ref->car;
        lbm_uint flash_ptr;

        handle_flash_status(lbm_write_const_raw(lbm_mem_ptr, 2, &flash_ptr));
        handle_flash_status(write_const_car(flash_cell, flash_ptr));
        handle_flash_status(write_const_cdr(flash_cell, ref->cdr));
#else
        // There are no indirect types in LBM64
        error_ctx(ENC_SYM_FATAL_ERROR);
#endif
      } break;
      case SYM_ARRAY_TYPE: {
        lbm_array_header_t *arr = (lbm_array_header_t*)ref->car;
        // arbitrary address: flash_arr.
        lbm_uint flash_arr;
        handle_flash_status(lbm_write_const_array_padded((uint8_t*)arr->data, arr->size, &flash_arr));
        lift_array_flash(flash_cell,
                         (char *)flash_arr,
                         arr->size);
      } break;
      case SYM_CHANNEL_TYPE: /* fall through */
      case SYM_CUSTOM_TYPE:
        lbm_set_error_reason((char *)lbm_error_str_flash_not_possible);
        error_ctx(ENC_SYM_EERROR);
      }
    } else {
      error_ctx(ENC_SYM_FATAL_ERROR);
    }
    ctx->r = flash_cell;
    ctx->app_cont = true;
    return;
  }
  ctx->r = val;
  ctx->app_cont = true;
}

static void cont_move_list_to_flash(eval_context_t *ctx) {

  // ctx->r holds the value that should go in car

  lbm_value *sptr = get_stack_ptr(ctx, 3);

  lbm_value fst = sptr[0];
  lbm_value lst = sptr[1];
  lbm_value val = sptr[2];

  handle_flash_status(write_const_car(lst, ctx->r));

  if (lbm_is_cons(val)) {
    // prepare cell for rest of list
    lbm_value rest_cell = ENC_SYM_NIL;
    handle_flash_status(request_flash_storage_cell(val, &rest_cell));
    handle_flash_status(write_const_cdr(lst, rest_cell));
    sptr[1] = rest_cell;
    sptr[2] = get_cdr(val);
    stack_push(&ctx->K, MOVE_LIST_TO_FLASH);
    stack_push(&ctx->K, MOVE_VAL_TO_FLASH_DISPATCH);
    ctx->r = get_car(val);
  } else {
    sptr[0] = fst;
    sptr[1] = lst;
    sptr[2] = CLOSE_LIST_IN_FLASH;
    stack_push(&ctx->K, MOVE_VAL_TO_FLASH_DISPATCH);
    ctx->r =  val;
  }
  ctx->app_cont = true;
}

static void cont_close_list_in_flash(eval_context_t *ctx) {
  lbm_value fst;
  lbm_value lst;
  lbm_pop_2(&ctx->K, &lst, &fst);
  lbm_value val = ctx->r;
  handle_flash_status(write_const_cdr(lst, val));
  ctx->r = fst;
  ctx->app_cont = true;
}

static void cont_qq_expand_start(eval_context_t *ctx) {
  stack_push_2(&ctx->K, ctx->r, QQ_EXPAND);
  ctx->r = ENC_SYM_NIL;
  ctx->app_cont = true;
}

lbm_value quote_it(lbm_value qquoted) {
  if (lbm_is_symbol(qquoted) &&
      lbm_is_special(qquoted)) return qquoted;

  lbm_value val = cons_with_gc(qquoted, ENC_SYM_NIL, ENC_SYM_NIL);
  return cons_with_gc(ENC_SYM_QUOTE, val, ENC_SYM_NIL);
}

bool is_append(lbm_value a) {
  return (lbm_is_cons(a) &&
          lbm_is_symbol(lbm_car(a)) &&
          (lbm_dec_sym(lbm_car(a)) == SYM_APPEND));
}

lbm_value append(lbm_value front, lbm_value back) {
  if (lbm_is_symbol_nil(front)) return back;
  if (lbm_is_symbol_nil(back)) return front;

  if (lbm_is_quoted_list(front) &&
      lbm_is_quoted_list(back)) {
    lbm_value f = lbm_car(lbm_cdr(front));
    lbm_value b = lbm_car(lbm_cdr(back));
    return quote_it(lbm_list_append(f, b));
  }

  if (is_append(back) &&
      lbm_is_quoted_list(lbm_car(lbm_cdr(back))) &&
       lbm_is_quoted_list(front)) {
    lbm_value ql = lbm_car(lbm_cdr(back));
    lbm_value f = lbm_car(lbm_cdr(front));
    lbm_value b = lbm_car(lbm_cdr(ql));

    lbm_value v = lbm_list_append(f, b);
    lbm_set_car(lbm_cdr(ql), v);
    return back;
  }

  if (is_append(back)) {
    back  = lbm_cdr(back);
    lbm_value new = cons_with_gc(front, back, ENC_SYM_NIL);
    return cons_with_gc(ENC_SYM_APPEND, new, ENC_SYM_NIL);
  }

  lbm_value t0, t1;

  t0 = cons_with_gc(back, ENC_SYM_NIL, ENC_SYM_NIL);
  t1 = cons_with_gc(front, t0, ENC_SYM_NIL);
  return cons_with_gc(ENC_SYM_APPEND, t1, ENC_SYM_NIL);
}

/* Bawden's qq-expand implementation
(define (qq-expand x)
  (cond ((tag-comma? x)
         (tag-data x))
        ((tag-comma-atsign? x)
         (error "Illegal"))
        ((tag-backquote? x)
         (qq-expand
          (qq-expand (tag-data x))))
        ((pair? x)
         `(append
           ,(qq-expand-list (car x))
           ,(qq-expand (cdr x))))
        (else `',x)))
 */
static void cont_qq_expand(eval_context_t *ctx) {
  lbm_value qquoted;
  lbm_pop(&ctx->K, &qquoted);

  switch(lbm_type_of(qquoted)) {
  case LBM_TYPE_CONS: {
    lbm_value car_val = lbm_car(qquoted);
    lbm_value cdr_val = lbm_cdr(qquoted);
    if (lbm_type_of(car_val) == LBM_TYPE_SYMBOL &&
        lbm_dec_sym(car_val) == SYM_COMMA) {
      ctx->r = append(ctx->r, lbm_car(cdr_val));
      ctx->app_cont = true;
    } else if (lbm_type_of(car_val) == LBM_TYPE_SYMBOL &&
               lbm_dec_sym(car_val) == SYM_COMMAAT) {
      error_ctx(ENC_SYM_RERROR);
    } else {
      stack_push_2(&ctx->K, ctx->r,  QQ_APPEND);
      stack_push_2(&ctx->K, cdr_val, QQ_EXPAND);
      stack_push_2(&ctx->K, car_val, QQ_EXPAND_LIST);
      ctx->app_cont = true;
      ctx->r = ENC_SYM_NIL;
    }

  } break;
  default: {
    lbm_value res = quote_it(qquoted);
    ctx->r = append(ctx->r, res);
    ctx->app_cont = true;
  }
  }
}

static void cont_qq_append(eval_context_t *ctx) {
  lbm_value head;
  lbm_pop(&ctx->K, &head);
  ctx->r = append(head, ctx->r);
  ctx->app_cont = true;
}

/* Bawden's qq-expand-list implementation
(define (qq-expand-list x)
  (cond ((tag-comma? x)
         `(list ,(tag-data x)))
        ((tag-comma-atsign? x)
         (tag-data x))
        ((tag-backquote? x)
         (qq-expand-list
          (qq-expand (tag-data x))))
        ((pair? x)
         `(list
           (append
            ,(qq-expand-list (car x))
            ,(qq-expand (cdr x)))))
        (else `'(,x))))
*/

static void cont_qq_expand_list(eval_context_t* ctx) {
  lbm_value l;
  lbm_pop(&ctx->K, &l);

  switch(lbm_type_of(l)) {
  case LBM_TYPE_CONS: {
    lbm_value car_val = lbm_car(l);
    lbm_value cdr_val = lbm_cdr(l);
    if (lbm_type_of(car_val) == LBM_TYPE_SYMBOL &&
        lbm_dec_sym(car_val) == SYM_COMMA) {
      lbm_value tl;
      WITH_GC(tl, lbm_cons(lbm_car(cdr_val), ENC_SYM_NIL));
      lbm_value tmp;
      WITH_GC_RMBR_1(tmp, lbm_cons(ENC_SYM_LIST, tl), tl);
      ctx->r = append(ctx->r, tmp);
      ctx->app_cont = true;
      return;
    } else if (lbm_type_of(car_val) == LBM_TYPE_SYMBOL &&
               lbm_dec_sym(car_val) == SYM_COMMAAT) {
      ctx->r = lbm_car(cdr_val);
      ctx->app_cont = true;
      return;
    } else {
      stack_push(&ctx->K, QQ_LIST);
      stack_push_2(&ctx->K, ctx->r,  QQ_APPEND);
      stack_push_2(&ctx->K, cdr_val, QQ_EXPAND);
      stack_push_2(&ctx->K, car_val, QQ_EXPAND_LIST);
      ctx->app_cont = true;
      ctx->r = ENC_SYM_NIL;
    }

  } break;
  default: {
    lbm_value a_list = cons_with_gc(l, ENC_SYM_NIL, ENC_SYM_NIL);
    lbm_value tl = cons_with_gc(a_list, ENC_SYM_NIL, ENC_SYM_NIL);
    lbm_value tmp = cons_with_gc(ENC_SYM_QUOTE, tl, ENC_SYM_NIL);
    ctx->r = append(ctx->r, tmp);
    ctx->app_cont = true;
  }
  }
}

static void cont_qq_list(eval_context_t *ctx) {
  lbm_value val = ctx->r;
  lbm_value apnd_app = cons_with_gc(val, ENC_SYM_NIL, ENC_SYM_NIL);
  lbm_value tmp = cons_with_gc(ENC_SYM_LIST, apnd_app, ENC_SYM_NIL);
  ctx->r = tmp;
  ctx->app_cont = true;
}

static void cont_kill(eval_context_t *ctx) {
  (void) ctx;
  finish_ctx();
}

/*********************************************************/
/* Continuations table                                   */
typedef void (*cont_fun)(eval_context_t *);

static const cont_fun continuations[NUM_CONTINUATIONS] =
  { advance_ctx,  // CONT_DONE
    cont_set_global_env,
    cont_bind_to_key_rest,
    cont_if,
    cont_progn_rest,
    cont_application_args,
    cont_and,
    cont_or,
    cont_wait,
    cont_match,
    cont_match_many,
    cont_application_start,
    cont_eval_r,
    cont_set_var,
    cont_resume,
    cont_closure_application_args,
    cont_exit_atomic,
    cont_read_next_token,
    cont_read_append_continue,
    cont_read_eval_continue,
    cont_read_expect_closepar,
    cont_read_dot_terminate,
    cont_read_done,
    cont_read_quote_result,
    cont_read_commaat_result,
    cont_read_comma_result,
    cont_read_start_array,
    cont_read_append_array,
    cont_map_first,
    cont_map_rest,
    cont_match_guard,
    cont_terminate,
    cont_progn_var,
    cont_setq,
    cont_move_to_flash,
    cont_move_val_to_flash_dispatch,
    cont_move_list_to_flash,
    cont_close_list_in_flash,
    cont_qq_expand_start,
    cont_qq_expand,
    cont_qq_append,
    cont_qq_expand_list,
    cont_qq_list,
    cont_kill,
  };

/*********************************************************/
/* Evaluators lookup table (special forms)               */
typedef void (*evaluator_fun)(eval_context_t *);

static const evaluator_fun evaluators[] =
  {
   eval_quote,
   eval_define,
   eval_progn,
   eval_lambda,
   eval_if,
   eval_let,
   eval_and,
   eval_or,
   eval_match,
   eval_receive,
   eval_receive_timeout,
   eval_callcc,
   eval_atomic,
   eval_selfevaluating, // macro
   eval_selfevaluating, // cont
   eval_selfevaluating, // closure
   eval_cond,
   eval_app_cont,
   eval_var,
   eval_setq,
   eval_move_to_flash,
  };


/*********************************************************/
/* Evaluator step function                               */

static void evaluation_step(void){
  eval_context_t *ctx = ctx_running;
#ifdef VISUALIZE_HEAP
  heap_vis_gen_image();
#endif

  if (ctx->app_cont) {
    lbm_value k;
    lbm_pop(&ctx->K, &k);
    ctx->app_cont = false;

    lbm_uint decoded_k = DEC_CONTINUATION(k);

    if (decoded_k < NUM_CONTINUATIONS) {
      continuations[decoded_k](ctx);
    } else {
      error_ctx(ENC_SYM_FATAL_ERROR);
    }
    return;
  }

  lbm_uint exp_type = lbm_type_of_functional(ctx->curr_exp);
  if (exp_type == LBM_TYPE_SYMBOL) {
    eval_symbol(ctx);
    return;
  }
  if (exp_type == LBM_TYPE_CONS) {
    lbm_value head = lbm_ref_cell(ctx->curr_exp)->car;

    if (lbm_type_of(head) == LBM_TYPE_SYMBOL) {

      lbm_value eval_index = lbm_dec_sym(head) - SPECIAL_FORMS_START;

      if (eval_index <= (SPECIAL_FORMS_END - SPECIAL_FORMS_START)) {
        evaluators[eval_index](ctx);
        return;
      }
    }
    /*
     * At this point head can be anything. It should evaluate
     * into a form that can be applied (closure, symbol, ...) though.
     */
    lbm_value *reserved = stack_reserve(ctx, 3);
    reserved[0] = ctx->curr_env;
    reserved[1] = lbm_ref_cell(ctx->curr_exp)->cdr;
    reserved[2] = APPLICATION_START;

    ctx->curr_exp = head; // evaluate the function
    return;
  }

  eval_selfevaluating(ctx);
  return;
}

void lbm_pause_eval(void ) {
  eval_cps_next_state_arg = 0;
  eval_cps_next_state = EVAL_CPS_STATE_PAUSED;
  if (eval_cps_next_state != eval_cps_run_state) eval_cps_state_changed = true;
}

void lbm_pause_eval_with_gc(uint32_t num_free) {
  eval_cps_next_state_arg = num_free;
  eval_cps_next_state = EVAL_CPS_STATE_PAUSED;
  if (eval_cps_next_state != eval_cps_run_state) eval_cps_state_changed = true;
}

void lbm_continue_eval(void) {
  eval_cps_next_state = EVAL_CPS_STATE_RUNNING;
  if (eval_cps_next_state != eval_cps_run_state) eval_cps_state_changed = true;
}

void lbm_kill_eval(void) {
  eval_cps_next_state = EVAL_CPS_STATE_KILL;
  if (eval_cps_next_state != eval_cps_run_state) eval_cps_state_changed = true;
}

uint32_t lbm_get_eval_state(void) {
  return eval_cps_run_state;
}

// Will wake up thread that is sleeping as well.
// Not sure this is good behavior.
static void handle_event_unblock_ctx(lbm_cid cid, lbm_value v) {
  eval_context_t *found = NULL;
  mutex_lock(&qmutex);

  found = lookup_ctx_nm(&blocked, cid);
  if (found) {
    drop_ctx_nm(&blocked,found);
    if (lbm_is_error(v)) {
      lbm_uint trash;
      lbm_pop(&found->K, &trash);
      lbm_push(&found->K, TERMINATE);
      found->app_cont = true;
    }
    found->r = v;
    enqueue_ctx_nm(&queue,found);
  }
  mutex_unlock(&qmutex);
}

static lbm_value get_event_value(lbm_event_t *e) {
 lbm_value v;
  if (e->buf_len > 0) {
    lbm_flat_value_t fv;
    fv.buf = (uint8_t*)e->buf_ptr;
    fv.buf_size = e->buf_len;
    fv.buf_pos = 0;
    if (!lbm_unflatten_value(&fv, &v)) {
      lbm_set_flags(LBM_FLAG_HANDLER_EVENT_DELIVERY_FAILED);
      v = ENC_SYM_EERROR;
    }
  } else {
    v = (lbm_value)e->buf_ptr;
  }
  return v;
}

static void process_events(void) {

  if (!lbm_events) return;
  lbm_event_t e;

  while (lbm_event_pop(&e)) {

    lbm_value event_val = get_event_value(&e);
    switch(e.type) {
    case LBM_EVENT_UNBLOCK_CTX:
      handle_event_unblock_ctx((lbm_cid)e.parameter, event_val);
      break;
    case LBM_EVENT_FOR_HANDLER:
      if (lbm_event_handler_pid >= 0) {
        lbm_find_receiver_and_send(lbm_event_handler_pid, event_val);
      }
      break;
    }
  }
}

static void process_waiting_nm(void) {

  uint32_t wait_flags = wait_for;   // Should ideally be atomic
  wait_for = wait_flags ^ wait_for; //

  eval_context_queue_t *q = &blocked;

  eval_context_t *curr = q->first;
  while (curr != NULL) {
    eval_context_t *next = curr->next; // grab here
    if (curr->wait_mask & wait_flags) {
      eval_context_t *ctx = curr;
      if (curr == q->last) {
        if (curr->prev) {
          q->last = curr->prev;
          q->last->next = NULL;
        } else {
          q->first = NULL;
          q->last = NULL;
        }
      } else if (curr->prev == NULL) {
        q->first = curr->next;
        if (q->first) {
          q->first->prev = NULL;
        }
      } else {
        curr->prev->next = curr->next;
        if (curr->next) {
          curr->next->prev = curr->prev;
        }
      }
      ctx->wait_mask = 0;
      ctx->r = ENC_SYM_TRUE; // woken up task receives true.
      enqueue_ctx_nm(&queue, ctx); // changes meaning of curr->next.
    }
    curr = next;
  }
}

/* eval_cps_run can be paused
   I think it would be better use a mailbox for
   communication between other threads and the run_eval
   but for now a set of variables will be used. */
void lbm_run_eval(void){

  setjmp(error_jmp_buf);

  while (eval_running) {
    eval_cps_state_changed = false;
    switch (eval_cps_next_state) {
    case EVAL_CPS_STATE_PAUSED:
      if (eval_cps_run_state != EVAL_CPS_STATE_PAUSED) {
        if (lbm_heap_num_free() < eval_cps_next_state_arg) {
          gc();
        }
        eval_cps_next_state_arg = 0;
      }
      eval_cps_run_state = EVAL_CPS_STATE_PAUSED;
      usleep_callback(EVAL_CPS_MIN_SLEEP);
      continue; /* jump back to start of eval_running loop */
    case EVAL_CPS_STATE_KILL:
      eval_running = false;
      continue;
    default: // running state
      eval_cps_run_state = eval_cps_next_state;
      break;
    }
    while (true) {
      if (eval_steps_quota && ctx_running) {
        eval_steps_quota--;
        evaluation_step();
      } else {
        if (eval_cps_state_changed) break;
        eval_steps_quota = eval_steps_refill;
        if (is_atomic) {
          if (!ctx_running) {
            lbm_set_flags(LBM_FLAG_ATOMIC_MALFUNCTION);
            is_atomic = 0;
          }
        } else {
          if (gc_requested) {
            gc();
          }
          process_events();
          mutex_lock(&qmutex);
          if (wait_for) {
            process_waiting_nm();
          }
          if (ctx_running) {
            enqueue_ctx_nm(&queue, ctx_running);
            ctx_running = NULL;
          }
          wake_up_ctxs_nm();
          ctx_running = dequeue_ctx_nm(&queue);
          mutex_unlock(&qmutex);
          if (!ctx_running) {
            lbm_system_sleeping = true;
            //Fixed sleep interval to poll events regularly.
            usleep_callback(EVAL_CPS_MIN_SLEEP);
            lbm_system_sleeping = false;
          }
        }
      }
    }
  }
}

lbm_cid lbm_eval_program(lbm_value lisp) {
  return lbm_create_ctx(lisp, ENC_SYM_NIL, 256);
}

lbm_cid lbm_eval_program_ext(lbm_value lisp, unsigned int stack_size) {
  return lbm_create_ctx(lisp, ENC_SYM_NIL, stack_size);
}

int lbm_eval_init() {
  int res = 1;

  if (!qmutex_initialized) {
    mutex_init(&qmutex);
    qmutex_initialized = true;
  }
  if (!lbm_events_mutex_initialized) {
    mutex_init(&lbm_events_mutex);
    lbm_events_mutex_initialized = true;
  }
  if (!blocking_extension_mutex_initialized) {
    mutex_init(&blocking_extension_mutex);
    blocking_extension_mutex_initialized = true;
  }

  mutex_lock(&qmutex);
  mutex_lock(&lbm_events_mutex);

  blocked.first = NULL;
  blocked.last = NULL;
  queue.first = NULL;
  queue.last = NULL;
  ctx_running = NULL;

  eval_cps_run_state = EVAL_CPS_STATE_RUNNING;

  mutex_unlock(&lbm_events_mutex);
  mutex_unlock(&qmutex);

  *lbm_get_env_ptr() = ENC_SYM_NIL;
  eval_running = true;

  return res;
}

bool lbm_eval_init_events(unsigned int num_events) {

  mutex_lock(&lbm_events_mutex);
  lbm_events = (lbm_event_t*)lbm_malloc(num_events * sizeof(lbm_event_t));
  bool r = false;
  if (lbm_events) {
    lbm_events_max = num_events;
    lbm_events_head = 0;
    lbm_events_tail = 0;
    lbm_events_full = false;
    lbm_event_handler_pid = -1;
    r = true;
  }
  mutex_unlock(&lbm_events_mutex);
  return r;
}
