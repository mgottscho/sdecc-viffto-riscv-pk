// See LICENSE for license details.

#include "pk.h"
#include "config.h"
#include "syscall.h"
#include "vm.h"

user_due_trap_handler g_user_memory_due_trap_handler = NULL; //MWG
due_candidates_t g_candidates; //MWG
due_cacheline_t g_cacheline; //MWG
char g_candidates_cstring[4096]; //MWG
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
       short float_regfile = decode_regfile(tf->insn);

       float_trapframe_t float_tf;
       if (set_float_trapframe(&float_tf))
          default_memory_due_trap_handler(tf);
       
       do_data_recovery(&recovered_value); //FIXME: inst recovery?
       int retval = g_user_memory_due_trap_handler(tf, &float_tf, &g_candidates, &g_cacheline, &recovered_value, load_size, load_dest_reg, float_regfile, load_message_offset); //May clobber recovered_value
       void* ptr = (void*)(badvaddr);
       switch (retval) {
         case 0: //User handler indicated success, use their specified value
         case 1: //User handler wants us to use the generic recovery policy. Use our specified value. FIXME: what if user clobbered it but doesn't want to use it?
             if (load_value_from_message(&recovered_value, &recovered_load_value, &g_cacheline, load_size, load_message_offset))
                 default_memory_due_trap_handler(tf);
             if (writeback_recovered_message(&recovered_value, &recovered_load_value, tf, load_dest_reg, float_regfile))
                 default_memory_due_trap_handler(tf);
             tf->epc += 4;
             return;
         case -1: //User handler wants us to use default safe handler (crash)
         default:
             default_memory_due_trap_handler(tf); 
             return;
       }
  }
  default_memory_due_trap_handler(tf); 
}

//MWG
int getDUECandidateMessages(due_candidates_t* candidates) {
    //Magical Spike hook to compute candidates, so we don't have to re-implement in C
    asm volatile("custom2 0,%0,0,0;"
                 : 
                 : "r" (&g_candidates_cstring));

    //Parse returned value
    parse_sdecc_candidate_output(g_candidates_cstring, 4096, candidates);
    
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
int copy_float_trapframe(float_trapframe_t* dest, float_trapframe_t* src) {
   if (dest && src) {
       for (int i = 0; i < 32; i++)
           dest->fpr[i] = src->fpr[i];
       return 0;
   }

   return 1;
}

//MWG
unsigned decode_rd(long insn) {
   return (insn >> 7) & ((1 << 5)-1); 
}

//MWG
short decode_regfile(long insn) {
    //FLW: 0x2007 match, FLD: 0x3007
    return ((insn & MATCH_FLW) == MATCH_FLW || (insn & MATCH_FLD) == MATCH_FLD);
}

//MWG
int load_value_from_message(word_t* recovered_message, word_t* load_value, due_cacheline_t* cl, unsigned load_size, int offset) {
    if (!recovered_message || !load_value || !cl)
        return -1;
    
    unsigned msg_size = recovered_message->size; 
    unsigned blockpos = cl->blockpos;

    // ----- Four cases to handle ----

    //Load value fits entirely inside message -- the expected common case (e.g., we load an aligned int (32-bits) and messages are at least 32-bits
    if (offset >= 0 && offset+load_size <= msg_size) {
        memcpy(load_value->bytes, recovered_message->bytes+offset, load_size);
    
    //Load value starts inside message but extends beyond it (e.g., we load an aligned unsigned long (64-bits) but messages are only 32-bits
    } else if (offset >= 0 && offset < msg_size && offset+load_size > msg_size) {
        unsigned remain = load_size;
        size_t curr_blockpos = blockpos+1;
        unsigned transferred = 0;
        memcpy(load_value->bytes, recovered_message->bytes+offset, msg_size-offset);
        remain -= msg_size-offset;
        transferred = load_size - remain;
        while (remain > 0) {
            if (msg_size > remain) {
                memcpy(load_value->bytes+transferred, cl->words[curr_blockpos].bytes, remain);
                remain = 0;
            } else {
                memcpy(load_value->bytes+transferred, cl->words[curr_blockpos].bytes, msg_size);
                remain -= msg_size;
            }
            transferred = load_size - remain;
            curr_blockpos++;
        }

    //Load value starts before message but ends within it (e.g., we load an aligned unsigned long (64-bits) but messages are only 32-bits
    } else if (offset < 0 && offset+load_size > 0 && offset+load_size < msg_size) {
        unsigned remain = load_size;
        unsigned transferred = 0;
        int curr_blockpos = blockpos + offset/msg_size; //Negative offset
        if (curr_blockpos < 0 || curr_blockpos > cl->size) //Something went wrong
            return -1;

        while (curr_blockpos < blockpos) {
            memcpy(load_value->bytes+transferred, cl->words[curr_blockpos].bytes, msg_size);
            curr_blockpos++;
            remain -= msg_size;
            transferred = load_size - remain;
        }
        memcpy(load_value->bytes+transferred, recovered_message->bytes, remain);
        remain = 0;
        transferred = load_size - remain;
        curr_blockpos++;

    //Load value starts before message but ends after it (e.g., we load an unaligned unsigned long (64-bits) but messages are only 16-bits)
    } else if (offset < 0 && offset+load_size > msg_size) {
        unsigned remain = load_size;
        unsigned transferred = 0;
        int curr_blockpos = blockpos + offset/msg_size; //Negative offset
        if (curr_blockpos < 0 || curr_blockpos > cl->size) //Something went wrong
            return -1;

        while (curr_blockpos < blockpos) {
            memcpy(load_value->bytes+transferred, cl->words[curr_blockpos].bytes, msg_size);
            curr_blockpos++;
            remain -= msg_size;
            transferred = load_size - remain;
        }
        memcpy(load_value->bytes+transferred, recovered_message->bytes, msg_size);
        remain -= msg_size;
        transferred = load_size - remain;
        curr_blockpos++;
        while (remain > 0) {
            if (msg_size > remain) {
                memcpy(load_value->bytes+transferred, cl->words[curr_blockpos].bytes, remain);
                remain = 0;
            } else {
                memcpy(load_value->bytes+transferred, cl->words[curr_blockpos].bytes, msg_size);
                remain -= msg_size;
            }
            transferred = load_size - remain;
            curr_blockpos++;
        }

    //Load value starts before message and ends before it (e.g., DUE on a cacheline word that was not the demand load)
    } else if (offset+load_size < 0) {
        unsigned remain = load_size;
        unsigned transferred = 0;
        int curr_blockpos = blockpos + offset/msg_size; //Negative offset
        if (curr_blockpos < 0 || curr_blockpos > cl->size) //Something went wrong
            return -1;

        while (remain > 0) {
            if (msg_size > remain) {
                memcpy(load_value->bytes+transferred, cl->words[curr_blockpos].bytes, remain);
                remain = 0;
            } else {
                memcpy(load_value->bytes+transferred, cl->words[curr_blockpos].bytes, msg_size);
                remain -= msg_size;
            }
            transferred = load_size - remain;
            curr_blockpos++;
        }

    //Load value starts after message and ends after it -- TODO (e.g., DUE on a cacheline word that was not the demand load)
    } else if (offset > msg_size) {
        unsigned remain = load_size;
        unsigned transferred = 0;
        int curr_blockpos = blockpos + offset/msg_size; //positive offset
        if (curr_blockpos < 0 || curr_blockpos > cl->size) //Something went wrong
            return -1;

        while (remain > 0) {
            if (msg_size > remain) {
                memcpy(load_value->bytes+transferred, cl->words[curr_blockpos].bytes, remain);
                remain = 0;
            } else {
                memcpy(load_value->bytes+transferred, cl->words[curr_blockpos].bytes, msg_size);
                remain -= msg_size;
            }
            transferred = load_size - remain;
            curr_blockpos++;
        }
    
    } else { //Something went wrong
        return -1; 
    }

    load_value->size = load_size;
    return 0;
}

//MWG
int writeback_recovered_message(word_t* recovered_message, word_t* load_value, trapframe_t* tf, unsigned rd, short float_regfile) {
    if (!recovered_message || !load_value || !tf || rd < 0 || rd > 32)
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

    if (float_regfile) {
        //Floating-point registers are not part of the trapframe, so I suppose we should just write the register directly.
        if (set_float_register(rd, val)) {
            return -1;
        }
    } else {
        tf->gpr[rd] = val; //Write load value to trapframe
    }

    unsigned long msg_size = recovered_message->size; 
    void* badvaddr_msg = (void*)((unsigned long)(tf->badvaddr) & (~(msg_size-1));
    memcpy(badvaddr_msg, recovered_message->bytes, msg_size); //Write message to main memory
    return 0;
}

int set_float_register(unsigned frd, unsigned long raw_value) {
    switch (frd) {
        case 0: //f0
            asm volatile("fmv.d.x f0, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 1: //f1
            asm volatile("fmv.d.x f1, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 2: //f2
            asm volatile("fmv.d.x f2, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 3: //f3
            asm volatile("fmv.d.x f3, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 4: //f4
            asm volatile("fmv.d.x f4, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 5: //f5
            asm volatile("fmv.d.x f5, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 6: //f6
            asm volatile("fmv.d.x f6, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 7: //f7
            asm volatile("fmv.d.x f7, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 8: //f8
            asm volatile("fmv.d.x f8, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 9: //f9
            asm volatile("fmv.d.x f9, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 10: //f10
            asm volatile("fmv.d.x f10, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 11: //f11
            asm volatile("fmv.d.x f11, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 12: //f12
            asm volatile("fmv.d.x f12, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 13: //f13
            asm volatile("fmv.d.x f13, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 14: //f14
            asm volatile("fmv.d.x f14, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 15: //f15
            asm volatile("fmv.d.x f15, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 16: //f16
            asm volatile("fmv.d.x f16, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 17: //f17
            asm volatile("fmv.d.x f17, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 18: //f18
            asm volatile("fmv.d.x f18, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 19: //f19
            asm volatile("fmv.d.x f19, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 20: //f20
            asm volatile("fmv.d.x f20, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 21: //f21
            asm volatile("fmv.d.x f21, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 22: //f22
            asm volatile("fmv.d.x f22, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 23: //f23
            asm volatile("fmv.d.x f23, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 24: //f24
            asm volatile("fmv.d.x f24, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 25: //f25
            asm volatile("fmv.d.x f25, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 26: //f26
            asm volatile("fmv.d.x f26, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 27: //f27
            asm volatile("fmv.d.x f27, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 28: //f28
            asm volatile("fmv.d.x f28, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 29: //f29
            asm volatile("fmv.d.x f29, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 30: //f30
            asm volatile("fmv.d.x f30, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        case 31: //f31
            asm volatile("fmv.d.x f31, %0;"
                         :
                         : "r" (raw_value));
            return 0;
        default: //Bad register
            return -1;
    }
}

int get_float_register(unsigned frd, unsigned long* raw_value) {
    if (!raw_value)
        return -1;

    unsigned long tmp; 
    switch (frd) {
        case 0: //f0
            asm volatile("fmv.x.d %0, f0;"
                         : "=r" (tmp)
                         :);
            break;
        case 1: //f1
            asm volatile("fmv.x.d %0, f1;"
                         : "=r" (tmp)
                         :);
            break;
        case 2: //f2
            asm volatile("fmv.x.d %0, f2;"
                         : "=r" (tmp)
                         :);
            break;
        case 3: //f3
            asm volatile("fmv.x.d %0, f3;"
                         : "=r" (tmp)
                         :);
            break;
        case 4: //f4
            asm volatile("fmv.x.d %0, f4;"
                         : "=r" (tmp)
                         :);
            break;
        case 5: //f5
            asm volatile("fmv.x.d %0, f5;"
                         : "=r" (tmp)
                         :);
            break;
        case 6: //f6
            asm volatile("fmv.x.d %0, f6;"
                         : "=r" (tmp)
                         :);
            break;
        case 7: //f7
            asm volatile("fmv.x.d %0, f7;"
                         : "=r" (tmp)
                         :);
            break;
        case 8: //f8
            asm volatile("fmv.x.d %0, f8;"
                         : "=r" (tmp)
                         :);
            break;
        case 9: //f9
            asm volatile("fmv.x.d %0, f9;"
                         : "=r" (tmp)
                         :);
            break;
        case 10: //f10
            asm volatile("fmv.x.d %0, f10;"
                         : "=r" (tmp)
                         :);
            break;
        case 11: //f11
            asm volatile("fmv.x.d %0, f11;"
                         : "=r" (tmp)
                         :);
            break;
        case 12: //f12
            asm volatile("fmv.x.d %0, f12;"
                         : "=r" (tmp)
                         :);
            break;
        case 13: //f13
            asm volatile("fmv.x.d %0, f13;"
                         : "=r" (tmp)
                         :);
            break;
        case 14: //f14
            asm volatile("fmv.x.d %0, f14;"
                         : "=r" (tmp)
                         :);
            break;
        case 15: //f15
            asm volatile("fmv.x.d %0, f15;"
                         : "=r" (tmp)
                         :);
            break;
        case 16: //f16
            asm volatile("fmv.x.d %0, f16;"
                         : "=r" (tmp)
                         :);
            break;
        case 17: //f17
            asm volatile("fmv.x.d %0, f17;"
                         : "=r" (tmp)
                         :);
            break;
        case 18: //f18
            asm volatile("fmv.x.d %0, f18;"
                         : "=r" (tmp)
                         :);
            break;
        case 19: //f19
            asm volatile("fmv.x.d %0, f19;"
                         : "=r" (tmp)
                         :);
            break;
        case 20: //f20
            asm volatile("fmv.x.d %0, f20;"
                         : "=r" (tmp)
                         :);
            break;
        case 21: //f21
            asm volatile("fmv.x.d %0, f21;"
                         : "=r" (tmp)
                         :);
            break;
        case 22: //f22
            asm volatile("fmv.x.d %0, f22;"
                         : "=r" (tmp)
                         :);
            break;
        case 23: //f23
            asm volatile("fmv.x.d %0, f23;"
                         : "=r" (tmp)
                         :);
            break;
        case 24: //f24
            asm volatile("fmv.x.d %0, f24;"
                         : "=r" (tmp)
                         :);
            break;
        case 25: //f25
            asm volatile("fmv.x.d %0, f25;"
                         : "=r" (tmp)
                         :);
            break;
        case 26: //f26
            asm volatile("fmv.x.d %0, f26;"
                         : "=r" (tmp)
                         :);
            break;
        case 27: //f27
            asm volatile("fmv.x.d %0, f27;"
                         : "=r" (tmp)
                         :);
            break;
        case 28: //f28
            asm volatile("fmv.x.d %0, f28;"
                         : "=r" (tmp)
                         :);
            break;
        case 29: //f29
            asm volatile("fmv.x.d %0, f29;"
                         : "=r" (tmp)
                         :);
            break;
        case 30: //f30
            asm volatile("fmv.x.d %0, f30;"
                         : "=r" (tmp)
                         :);
            break;
        case 31: //f31
            asm volatile("fmv.x.d %0, f31;"
                         : "=r" (tmp)
                         :);
            break;
        default: //Bad register
            return -1;
    }

    *raw_value = tmp;
    return 0;
}

//MWG
int set_float_trapframe(float_trapframe_t* float_tf) {
    if (!float_tf)
        return -1;

    unsigned long raw_value;
    for (int i = 0; i < 32; i++) {
        if (get_float_register(i, &raw_value))
            return -1;
        float_tf->fpr[i] = raw_value;
    }

    return 0;
}
