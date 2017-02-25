// See LICENSE for license details.

#include "pk.h"
#include "config.h"
#include "syscall.h"
#include "vm.h"

user_due_trap_handler g_user_memory_due_trap_handler = NULL; //MWG
due_candidates_t g_candidates; //MWG
due_cacheline_t g_cacheline; //MWG
char g_candidates_cstring[2048]; //MWG
char g_recovery_cstring[512]; //MWG

static void handle_illegal_instruction(trapframe_t* tf)
{
  tf->insn = *(uint16_t*)tf->epc;
  int len = insn_len(tf->insn);
  if (len == 4)
    tf->insn |= ((uint32_t)*(uint16_t*)(tf->epc + 2) << 16);
  else
    kassert(len == 2);

  dump_tf(tf);
  panic("An illegal instruction was executed!");
}

static void handle_breakpoint(trapframe_t* tf)
{
  dump_tf(tf);
  printk("Breakpoint!\n");
  tf->epc += 4;
}

static void handle_misaligned_fetch(trapframe_t* tf)
{
  dump_tf(tf);
  panic("Misaligned instruction access!");
}

void handle_misaligned_load(trapframe_t* tf)
{
  // TODO emulate misaligned loads and stores
  dump_tf(tf);
  panic("Misaligned load!");
}

void handle_misaligned_store(trapframe_t* tf)
{
  dump_tf(tf);
  panic("Misaligned store!");
}

static void segfault(trapframe_t* tf, uintptr_t addr, const char* type)
{
  dump_tf(tf);
  const char* who = (tf->status & MSTATUS_PRV1) ? "Kernel" : "User";
  panic("%s %s segfault @ %p", who, type, addr);
}

static void handle_fault_fetch(trapframe_t* tf)
{
  if (handle_page_fault(tf->badvaddr, PROT_EXEC) != 0)
    segfault(tf, tf->badvaddr, "fetch");
}

void handle_fault_load(trapframe_t* tf)
{
  if (handle_page_fault(tf->badvaddr, PROT_READ) != 0)
    segfault(tf, tf->badvaddr, "load");
}

void handle_fault_store(trapframe_t* tf)
{
  if (handle_page_fault(tf->badvaddr, PROT_WRITE) != 0)
    segfault(tf, tf->badvaddr, "store");
}

static void handle_syscall(trapframe_t* tf)
{
  tf->gpr[10] = do_syscall(tf->gpr[10], tf->gpr[11], tf->gpr[12], tf->gpr[13],
                           tf->gpr[14], tf->gpr[15], tf->gpr[17]);
  tf->epc += 4;
}

static void handle_interrupt(trapframe_t* tf)
{
  clear_csr(sip, SIP_SSIP);
}

void handle_trap(trapframe_t* tf)
{
  if ((intptr_t)tf->cause < 0)
    return handle_interrupt(tf);

  //MWG moved the following to pk.h
  //typedef void (*trap_handler)(trapframe_t*);

  const static trap_handler trap_handlers[] = {
    [CAUSE_MISALIGNED_FETCH] = handle_misaligned_fetch,
    [CAUSE_FAULT_FETCH] = handle_fault_fetch,
    [CAUSE_ILLEGAL_INSTRUCTION] = handle_illegal_instruction,
    [CAUSE_USER_ECALL] = handle_syscall,
    [CAUSE_BREAKPOINT] = handle_breakpoint,
    [CAUSE_MISALIGNED_LOAD] = handle_misaligned_load,
    [CAUSE_MISALIGNED_STORE] = handle_misaligned_store,
    [CAUSE_FAULT_LOAD] = handle_fault_load,
    [CAUSE_FAULT_STORE] = handle_fault_store,
    [CAUSE_MEMORY_DUE] = handle_memory_due, //MWG: this is non-standard
  };

  kassert(tf->cause < ARRAY_SIZE(trap_handlers) && trap_handlers[tf->cause]);

  trap_handlers[tf->cause](tf);
}

//MWG
void sys_register_user_memory_due_trap_handler(user_due_trap_handler fptr) {
   g_user_memory_due_trap_handler = fptr;
}

//MWG
int default_memory_due_trap_handler(trapframe_t* tf) {
  panic("Default pk memory DUE trap handler: panic()");
  return 0;
}

//MWG
void handle_memory_due(trapframe_t* tf) {
  if (g_user_memory_due_trap_handler && !getDUECandidateMessages(&g_candidates) && !getDUECacheline(&g_cacheline)) {

       word_t recovered_value;
       word_t recovered_load_value;
       recovered_value.size = 0;
       recovered_load_value.size = 0;
       copy_word(&recovered_value, &(g_candidates.candidate_messages[0])); //Default: first candidate in list
       
       long badvaddr = tf->badvaddr;
       short msg_size = recovered_value.size;
       short load_size = (short)(read_csr(0x4)); //CSR_PENALTY_BOX_LOAD_SIZE
       short load_message_offset = badvaddr - (badvaddr & ~(msg_size-1));
       short load_dest_reg = decode_rd(tf->insn);
       /*if (offset+load_size > msg_size) { //TODO: load values spanning multiple memory messages
             default_memory_due_trap_handler(tf);
             tf->epc += 4;
             return;
       }*/
       if (load_value_from_message(&recovered_value, &recovered_load_value, &g_cacheline, load_size, load_message_offset))
           default_memory_due_trap_handler(tf);

       int retval = g_user_memory_due_trap_handler(tf, &g_candidates, &g_cacheline, &recovered_value, &recovered_load_value, load_dest_reg, load_message_offset); //May clobber recovered_value
       void* ptr = (void*)(badvaddr);
       switch (retval) {
         case 0: //User handler indicated success
             if (load_value_from_message(&recovered_value, &recovered_load_value, &g_cacheline, load_size, load_message_offset))
                 default_memory_due_trap_handler(tf);
             if (writeback_recovered_message(&recovered_value, &recovered_load_value, tf))
                 default_memory_due_trap_handler(tf);
             tf->epc += 4;
             return;
         case 1: //User handler wants us to use the generic recovery policy
             do_data_recovery(&recovered_value); //FIXME: inst recovery?
             if (load_value_from_message(&recovered_value, &recovered_load_value, &g_cacheline, load_size, load_message_offset))
                 default_memory_due_trap_handler(tf);
             if (writeback_recovered_message(&recovered_value, &recovered_load_value, tf))
                 default_memory_due_trap_handler(tf);
             tf->epc += 4;
             return;
         default: //User handler wants us to use default safe handler
             default_memory_due_trap_handler(tf);
             tf->epc += 4;
             return;
       }
  }
  default_memory_due_trap_handler(tf);
  tf->epc += 4;
}

//MWG
int getDUECandidateMessages(due_candidates_t* candidates) {
    //Magical Spike hook to compute candidates, so we don't have to re-implement in C
    asm volatile("custom2 0,%0,0,0;"
                 : 
                 : "r" (&g_candidates_cstring));

    //Parse returned value
    parse_sdecc_candidate_output(g_candidates_cstring, 2048, candidates);
    
    return 0; 
}

//MWG
int getDUECacheline(due_cacheline_t* cacheline) {
    if (!cacheline)
        return 1;

    unsigned long wordsize = read_csr(0x5); //CSR_PENALTY_BOX_MSG_SIZE
    unsigned long cacheline_size = read_csr(0x6); //CSR_PENALTY_BOX_CACHELINE_SIZE
    unsigned long blockpos = read_csr(0x7); //CSR_PENALTY_BOX_CACHELINE_BLKPOS
    unsigned long cl[cacheline_size/sizeof(unsigned long)];

    for (int i = 0; i < cacheline_size/sizeof(unsigned long); i++)
        cl[i] = read_csr(0x8); //CSR_PENALTY_BOX_CACHELINE_WORD. Hardware will give us a different 64-bit chunk every iteration. If we over-read, then something bad may happen in HW.

    unsigned long words_per_block = cacheline_size / wordsize;
    char* cl_cast = (char*)(cl);
    for (int i = 0; i < words_per_block; i++) {
        memcpy(cacheline->words[i].bytes, cl_cast+(i*wordsize), wordsize);
        cacheline->words[i].size = wordsize;
    }
    cacheline->blockpos = blockpos;
    cacheline->size = words_per_block;

    return 0; 
}

//MWG
void parse_sdecc_candidate_output(char* script_stdout, size_t len, due_candidates_t* candidates) {
      int count = 0;
      int k = 0;
      unsigned long wordsize = read_csr(0x5); //CSR_PENALTY_BOX_MSG_SIZE
      word_t w;
      w.size = wordsize;
      // Output is expected to be simply a bunch of rows, each with k=8*wordsize binary messages, e.g. '001010100101001...001010'
      do {
          for (size_t i = 0; i < wordsize; i++) {
              w.bytes[i] = 0;
              for (size_t j = 0; j < 8; j++) {
                  w.bytes[i] |= (script_stdout[k++] == '1' ? (1 << (8-j-1)) : 0);
              }
          }
          script_stdout[k++] = ','; //Change newline to comma in buffer so we can reuse it for data recovery insn
          copy_word(candidates->candidate_messages+count, &w);
          count++;
      } while(script_stdout[k] != '\0' && count < 64 && k < len);
      candidates->size = count;
      script_stdout[k-1] = '\0';
}

//MWG
void parse_sdecc_data_recovery_output(const char* script_stdout, word_t* w) {
      int k = 0;
      unsigned long wordsize = read_csr(0x5); //CSR_PENALTY_BOX_MSG_SIZE
      // Output is expected to be simply a bunch of rows, each with k=8*wordsize binary messages, e.g. '001010100101001...001010'
      do {
          for (size_t i = 0; i < wordsize; i++) {
              w->bytes[i] = 0;
              for (size_t j = 0; j < 8; j++) {
                  w->bytes[i] |= (script_stdout[k++] == '1' ? (1 << (8-j-1)) : 0);
              }
          }
          k++; //Skip newline
      } while(script_stdout[k] != '\0' && k < 8*wordsize);
      w->size = wordsize;
}

//MWG
void do_data_recovery(word_t* w) {
    //Magical Spike hook to recover, so we don't have to re-implement in C
    asm volatile("custom3 0,%0,%1,0;"
                 : 
                 : "r" (&g_recovery_cstring), "r" (&g_candidates_cstring));

    parse_sdecc_data_recovery_output(g_recovery_cstring, w);
}

//MWG
int copy_word(word_t* dest, word_t* src) {
   if (dest && src) {
       for (int i = 0; i < 32; i++)
           dest->bytes[i] = src->bytes[i];
       dest->size = src->size;

       return 0;
   }

   return 1;
}

//MWG
int copy_cacheline(due_cacheline_t* dest, due_cacheline_t* src) {
    if (dest && src) {
        for (int i = 0; i < 32; i++)
            copy_word(dest->words+i, src->words+i);
        dest->size = src->size;
        dest->blockpos = src->blockpos;

        return 0;
    }
    
    return 1;
}

//MWG
int copy_candidates(due_candidates_t* dest, due_candidates_t* src) {
    if (dest && src) {
        for (int i = 0; i < 64; i++)
            copy_word(dest->candidate_messages+i, src->candidate_messages+i);
        dest->size = src->size;
        
        return 0;
    }

    return 1;
}

//MWG
int copy_trapframe(trapframe_t* dest, trapframe_t* src) {
   if (dest && src) {
       for (int i = 0; i < 32; i++)
           dest->gpr[i] = src->gpr[i];
       dest->status = src->status;
       dest->epc = src->epc;
       dest->badvaddr = src->badvaddr;
       dest->cause = src->cause;
       dest->insn = src->insn;

       return 0;
   }

   return 1;
}

//MWG
unsigned decode_rd(long insn) {
   return (insn >> 7) & ((1 << 5)-1); 
}

//MWG
int load_value_from_message(word_t* recovered_message, word_t* load_value, due_cacheline_t* cl, unsigned load_size, int offset) {
    if (!recovered_message || !load_value || !cl)
        return -1;
    
    unsigned msg_size = recovered_message->size; 
    unsigned blockpos = cl->blockpos;

    // ----- Four cases to handle ----

    //Load value fits entirely inside message -- the expected common case
    if (offset >= 0 && offset+load_size <= msg_size) {
        memcpy(load_value->bytes, recovered_message->bytes+offset, load_size);
    
    //Load value starts inside message but extends beyond it
    } else if (offset >= 0 && offset+load_size > msg_size) {
        unsigned remain = load_size;
        memcpy(load_value->bytes, recovered_message->bytes+offset, msg_size-offset);
        remain -= msg_size-offset;
        unsigned transferred = load_size - remain;
        size_t curr_blockpos = blockpos+1;
        while (remain > 0) {
            if (msg_size > remain) {
                memcpy(load_value->bytes+transferred, cl->words[curr_blockpos].bytes, remain);
                remain = 0;
            } else {
                memcpy(load_value->bytes+transferred, cl->words[curr_blockpos].bytes, msg_size);
                remain -= msg_size;
            }
            transferred = load_size - remain;
        }
    } else {
        return -1;
    }

    //Load value starts before message but extends into and/or beyond it -- TODO

    //Load value starts before message and ends before it -- TODO -- Spike support?

    //Load value starts after message and ends after it -- TODO -- Spike support?

    load_value->size = load_size;
    return 0;
}

//MWG
int writeback_recovered_message(word_t* recovered_message, word_t* load_value, trapframe_t* tf) {
    if (!recovered_message || !load_value || !tf)
        return -1;
    
       
    unsigned long val;
    switch (load_value->size) {
        case 1:
            ; //shut up compiler
            unsigned char* tmp = (unsigned char*)(load_value->bytes);
            val = (unsigned long)(*tmp);
            break;
        case 2:
            ; //shut up compiler
            unsigned short* tmp2 = (unsigned short*)(load_value->bytes);
            val = (unsigned long)(*tmp2);
            break;
        case 4:
            ; //shut up compiler
            unsigned* tmp3 = (unsigned*)(load_value->bytes);
            val = (unsigned long)(*tmp3);
            break;
        case 8:
            ; //shut up compiler
            unsigned long* tmp4 = (unsigned long*)(load_value->bytes);
            val = *tmp4;
            break;
        default:
            return -1;
    }

    unsigned rd = decode_rd(tf->insn);
    tf->gpr[rd] = val; //Write to load value to trapframe
    
    unsigned msg_size = recovered_message->size; 
    void* badvaddr_msg = (void*)(tf->badvaddr & (~(1-msg_size)));
    memcpy(badvaddr_msg, recovered_message->bytes, msg_size); //Write message to main memory
    return 0;
}
