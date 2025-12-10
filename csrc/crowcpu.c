#include <arpa/inet.h>
#include <asm-generic/errno.h>
#include <assert.h>
#include <crow.crowcpu_arch/crowcpu_arch.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

#include "cci.h"
#include "cpc.h"
#include "crowcpu.h"
#include "mmu.h"
#include "mobo.h"
#include "pic.h"

#define longjmp(x, y) assert(false)

/* how many instruction cycles before we check a new connection */
#define CCI_CHECK_INTERVAL 1024

void
tenc32_default_configuration(tenc32_configuration_t* config)
{
  config->cci_con_ms = 1000;
  config->cci_update_ms = 1;
}

tenc32_motherboard_t*
tenc32_motherboard_create(tenc32_configuration_t* conf, size_t ram_size)
{
  tenc32_motherboard_t* out = malloc(sizeof *out);
  out->conf = *conf;

  if (ram_size < TENC32_MIN_RAM_SIZE)
    return NULL;

  out->poweroff = false;
  out->memory_size = ram_size;
  out->memory = malloc(ram_size);

  mtx_init(&out->pic.mutex, mtx_plain);
  out->pic.incoming_flags = 0;

  out->cpu.exception = -1u;
  out->cpu.crs.cr0 = 0;
  out->cpu.crs.flags = 0;
  out->cpu.crs.idtr = 0;
  out->cpu.crs.imask = -1u;

  out->cci.devices = NULL;
  out->cci.devices_cap = 0;
  out->cci.listening_socket = -1;
  out->cci.last_connection_tick = 0;
  out->cci.last_update_tick = 0;

  tenc32_mmu_init(out);
  tenc32_init_pic(out);
  tenc32_init_cpc(out);

  return out;
}

void
tenc32_motherboard_destroy(struct tenc32_motherboard_t* cpu)
{
  assert(cpu != NULL);

  if (cpu->cci.listening_socket != -1)
    close(cpu->cci.listening_socket);
  if (cpu->cci.devices != NULL)
    free(cpu->cci.devices);

  mtx_destroy(&cpu->pic.mutex);
  free(cpu->memory);
  free(cpu);
}

void
tenc32_trigger_hardware_interrupt(tenc32_motherboard_t* mobo, unsigned irq)
{
  mtx_lock(&mobo->pic.mutex);

  assert(irq < MAX_IRQ);
  mobo->pic.incoming_flags |= 1 << irq;

  mtx_unlock(&mobo->pic.mutex);
}

void
tenc32_trigger_nonmaskable_interrupt(tenc32_motherboard_t* mobo, unsigned value)
{
  mtx_lock(&mobo->pic.mutex);
  mtx_unlock(&mobo->pic.mutex);
  (void)value;
  assert(false);
}

void
tenc32_restart(struct tenc32_motherboard_t* mobo,
               char const (*bios)[TENC32_ROM_SIZE])
{
  mobo->cpu.registers[REGISTER_PC] = 0x0000;

  mobo->cpu.crs.cr0 = 0;
  mobo->cpu.crs.flags = 0;
  mobo->cpu.crs.idtr = 0;

  tenc32_mmu_reset(mobo);

  assert(mobo->memory_size >= TENC32_ROM_SIZE);
  memcpy(mobo->memory, (*bios), TENC32_ROM_SIZE);
}

static bool
push_stack(tenc32_motherboard_t* mobo, unsigned in)
{
  /* fault */
  /* allocate the space first */
  mobo->cpu.registers[REGISTER_STACK] += 4;
  return tenc32_write_word(mobo, mobo->cpu.registers[REGISTER_STACK] - 4, in);
}

static bool
pop_stack(tenc32_motherboard_t* mobo, unsigned* out)
{
  /* fault */
  if (!tenc32_read_word(mobo, mobo->cpu.registers[REGISTER_STACK] - 4, out))
    return false;

  /* deallocate the space last */
  mobo->cpu.registers[REGISTER_STACK] -= 4;
  return true;
}

static void
trigger_isrt(tenc32_motherboard_t* mobo)
{
  unsigned new_stack;

  if (!pop_stack(mobo, &mobo->cpu.crs.flags))
    return;
  if (!pop_stack(mobo, &new_stack))
    return;
  if (!pop_stack(mobo, &mobo->cpu.registers[REGISTER_PC]))
    return;
  mobo->cpu.registers[REGISTER_STACK] = new_stack;
}

/* on false, trigger a double-fault and kill the computer */
static bool
trigger_isr(tenc32_motherboard_t* mobo, unsigned id)
{
  // printf("EXCEPTION %i TRIGGERED\n", id);
  /* make sure to unset halting mode */
  mobo->cpu.halting = false;
  assert(id < 64);
  unsigned addr = id * 4 + mobo->cpu.crs.idtr;
  unsigned isr_addr;

  if (!tenc32_read_word(mobo, addr, &isr_addr))
    return false;

  /* -1u is reserved "unimplemented" interrupt */
  if (isr_addr == -1u)
    return false;

  /* TODO: if we're in user mode, transfer to kernel
   * mode, swap the stacks */
  unsigned saved_stack = mobo->cpu.registers[REGISTER_STACK];

  /* return true as to not crash the computer,
   * but disregard the interrupt and queue a SIGILL
   */
  if (mobo->cpu.crs.cr0 & TENC32_CR0_MODE)
    return EDPRINT("attempting to trigger an ISR in user mode"),
           mobo->cpu.exception = TENC32_INTERRUPT_ILLEGAL_INSTRUCTION, true;

  if (!push_stack(mobo, mobo->cpu.registers[REGISTER_PC]))
    return false;
  if (!push_stack(mobo, saved_stack))
    return false;
  if (!push_stack(mobo, mobo->cpu.crs.flags))
    return false;
  mobo->cpu.registers[REGISTER_PC] = isr_addr;

  return true;
}

#define UADDI(x, y) (y < 0) ? (x - (-y)) : (x + y)

void
tenc32_insert_exception_callback(tenc32_motherboard_t* mobo,
                                 void (*callback)(tenc32_motherboard_t*))
{
  mobo->exception_callback = callback;
}

enum tenc32_step_val
tenc32_step(tenc32_motherboard_t* mobo)
{
  uint32_t instr_word;

  if (mobo->poweroff)
    return TENC32_STEP_POWEROFF;

  if (mobo->cpu.crs.cr0 & TENC32_CR0_INTERRUPT) {
    mtx_lock(&mobo->pic.mutex);
    uint32_t transformed =
      (mobo->pic.incoming_flags &
       (mobo->cpu.crs.cr0 & TENC32_CR0_MASK ? mobo->cpu.crs.imask : -1u));
    if (transformed) {
      /* dont retrigger currently handled interrupts! */
      transformed &= ~mobo->pic.currently_handled;

      unsigned i;
      for (i = 0; i < MAX_IRQ && (~transformed >> i) & 1; i++)
        ;
      if (i != MAX_IRQ) {
        mobo->pic.currently_handled |= 1 << i;
        trigger_isr(mobo, 0x20 + i);
      }
    }
    mtx_unlock(&mobo->pic.mutex);
  }

  if (mobo->cpu.halting)
    return TENC32_STEP_HALT;

  /* interrupt priority:
   *   1. hardware
   *   2. exception
   *   3. software
   */
  if (mobo->cpu.exception != -1) {
    mobo->exception_callback(mobo);
    if (!trigger_isr(mobo, mobo->cpu.exception))
      return TENC32_STEP_CRASH;
    mobo->cpu.exception = -1;
  }

  if (!tenc32_read_word(mobo, mobo->cpu.registers[REGISTER_PC], &instr_word))
    return TENC32_STEP_OK;

  tenc32_arch_decoded_instruction instr;

  if (!tenc32_arch_decode(&instr, instr_word)) {
    mobo->cpu.exception = TENC32_INTERRUPT_ILLEGAL_INSTRUCTION;
    EDPRINT("invalid instruction detected in step loop");
    goto OK_END;
  }

  switch (instr.condition) {
    case TENC32_COND_UNCONDITIONAL:
      break;

    case TENC32_COND_GREATER:
      if (!GETFLAG(mobo->cpu, TENC32_FLAGS_NEGATIVE) &&
          !GETFLAG(mobo->cpu, TENC32_FLAGS_ZERO))
        break;
      else
        goto OK_END;
    case TENC32_COND_LESS:
      if (GETFLAG(mobo->cpu, TENC32_FLAGS_NEGATIVE) &&
          !GETFLAG(mobo->cpu, TENC32_FLAGS_ZERO))
        break;
      else
        goto OK_END;
    case TENC32_COND_EQUAL:
      if (GETFLAG(mobo->cpu, TENC32_FLAGS_ZERO))
        break;
      else
        goto OK_END;
    case TENC32_COND_NOT_EQUAL:
      if (!GETFLAG(mobo->cpu, TENC32_FLAGS_ZERO))
        break;
      else
        goto OK_END;
    case TENC32_COND_GREATER_EQ:
      if (!GETFLAG(mobo->cpu, TENC32_FLAGS_NEGATIVE) ||
          GETFLAG(mobo->cpu, TENC32_FLAGS_ZERO))
        break;
      else
        goto OK_END;
    case TENC32_COND_LESS_EQ:
      if (GETFLAG(mobo->cpu, TENC32_FLAGS_NEGATIVE) ||
          GETFLAG(mobo->cpu, TENC32_FLAGS_ZERO))
        break;
      else
        goto OK_END;
  }

  switch (instr.instruction) {
    case TENC32_DECODED_MOVE:
      switch (instr.addressing) {
        case TENC32_ADDRESSING_MOVE_REG_REG:
          mobo->cpu.registers[instr.payload.move_reg_reg.dest] =
            mobo->cpu.registers[instr.payload.move_reg_reg.src];
          break;

        case TENC32_ADDRESSING_MOVE_REG_IMM:
          mobo->cpu.registers[instr.payload.move_reg_imm.dest] =
            instr.payload.move_reg_imm.imm;
          break;

        default:
          break;
      }
      goto OK_END;

    case TENC32_DECODED_LOAD:
      // fprintf(stderr,
      //         "load: %#x\n",
      //         UADDI(mobo->cpu.registers[instr.payload.mem_reg_indirect.base],
      //               (signed)instr.payload.mem_reg_indirect.offset));
      switch (instr.addressing) {
        case TENC32_ADDRESSING_MEMORY_BASE_OFFSET_WORD:
          tenc32_read_word(
            mobo,
            UADDI(mobo->cpu.registers[instr.payload.mem_reg_indirect.base],
                  (signed)instr.payload.mem_reg_indirect.offset),
            &mobo->cpu.registers[instr.payload.mem_reg_indirect.reg]);
          break;

        case TENC32_ADDRESSING_MEMORY_BASE_OFFSET_BYTE:
          tenc32_read_byte(
            mobo,
            UADDI(mobo->cpu.registers[instr.payload.mem_reg_indirect.base],
                  (signed)instr.payload.mem_reg_indirect.offset),
            &mobo->cpu.registers[instr.payload.mem_reg_indirect.reg]);
          break;

        default:
          break;
      }
      goto OK_END;

    case TENC32_DECODED_STORE:
      // fprintf(stderr,
      //         "store: %#x\n",
      //         UADDI(mobo->cpu.registers[instr.payload.mem_reg_indirect.base],
      //               (signed)instr.payload.mem_reg_indirect.offset));
      switch (instr.addressing) {
        case TENC32_ADDRESSING_MEMORY_BASE_OFFSET_WORD:
          tenc32_write_word(
            mobo,
            UADDI(mobo->cpu.registers[instr.payload.mem_reg_indirect.base],
                  (signed)instr.payload.mem_reg_indirect.offset),
            mobo->cpu.registers[instr.payload.mem_reg_indirect.reg]);
          break;

        case TENC32_ADDRESSING_MEMORY_BASE_OFFSET_BYTE:
          tenc32_write_byte(
            mobo,
            UADDI(mobo->cpu.registers[instr.payload.mem_reg_indirect.base],
                  (signed)instr.payload.mem_reg_indirect.offset),
            mobo->cpu.registers[instr.payload.mem_reg_indirect.reg]);
          break;

        default:
          break;
      }
      goto OK_END;

    case TENC32_DECODED_ADD:
      switch (instr.addressing) {
        case TENC32_ADDRESSING_ARITHMETIC_IMMEDIATE_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic_constant_reg.dest] =
            instr.payload.arithmetic_constant_reg.lhs +
            mobo->cpu.registers[instr.payload.arithmetic_constant_reg.rhs];
          break;

        case TENC32_ADDRESSING_ARITHMETIC_REGISTER_IMMEDIATE:
          mobo->cpu.registers[instr.payload.arithmetic_reg_constant.dest] =
            mobo->cpu.registers[instr.payload.arithmetic_reg_constant.lhs] +
            instr.payload.arithmetic_reg_constant.rhs;
          break;

        case TENC32_ADDRESSING_ARITHMETIC_REGISTER_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic.dest] =
            mobo->cpu.registers[instr.payload.arithmetic.lhs] +
            mobo->cpu.registers[instr.payload.arithmetic.rhs];
          break;

        default:
          break;
      }
      goto OK_END;

    case TENC32_DECODED_SUB:
      switch (instr.addressing) {
        case TENC32_ADDRESSING_ARITHMETIC_IMMEDIATE_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic_constant_reg.dest] =
            instr.payload.arithmetic_constant_reg.lhs -
            mobo->cpu.registers[instr.payload.arithmetic_constant_reg.rhs];
          break;

        case TENC32_ADDRESSING_ARITHMETIC_REGISTER_IMMEDIATE:
          mobo->cpu.registers[instr.payload.arithmetic_reg_constant.dest] =
            mobo->cpu.registers[instr.payload.arithmetic_reg_constant.lhs] -
            instr.payload.arithmetic_reg_constant.rhs;
          break;

        case TENC32_ADDRESSING_ARITHMETIC_REGISTER_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic.dest] =
            mobo->cpu.registers[instr.payload.arithmetic.lhs] -
            mobo->cpu.registers[instr.payload.arithmetic.rhs];
          break;

        default:
          break;
      }
      goto OK_END;

    case TENC32_DECODED_MUL:
      switch (instr.addressing) {
        case TENC32_ADDRESSING_ARITHMETIC_IMMEDIATE_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic_constant_reg.dest] =
            instr.payload.arithmetic_constant_reg.lhs *
            mobo->cpu.registers[instr.payload.arithmetic_constant_reg.rhs];
          break;

        case TENC32_ADDRESSING_ARITHMETIC_REGISTER_IMMEDIATE:
          mobo->cpu.registers[instr.payload.arithmetic_reg_constant.dest] =
            mobo->cpu.registers[instr.payload.arithmetic_reg_constant.lhs] *
            instr.payload.arithmetic_reg_constant.rhs;
          break;

        case TENC32_ADDRESSING_ARITHMETIC_REGISTER_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic.dest] =
            mobo->cpu.registers[instr.payload.arithmetic.lhs] *
            mobo->cpu.registers[instr.payload.arithmetic.rhs];
          break;

        default:
          break;
      }
      goto OK_END;

    case TENC32_DECODED_TEST:
      switch (instr.addressing) {
        unsigned res;
        unsigned lhs;
        unsigned rhs;

        case TENC32_ADDRESSING_ARITHMETIC_IMMEDIATE_REGISTER:
          lhs = instr.payload.arithmetic_constant_reg.lhs;
          rhs = mobo->cpu.registers[instr.payload.arithmetic_constant_reg.rhs];
          goto compute;

        case TENC32_ADDRESSING_ARITHMETIC_REGISTER_IMMEDIATE:
          lhs = mobo->cpu.registers[instr.payload.arithmetic_reg_constant.lhs];
          rhs = instr.payload.arithmetic_reg_constant.rhs;
          goto compute;

        case TENC32_ADDRESSING_ARITHMETIC_REGISTER_REGISTER:
          lhs = mobo->cpu.registers[instr.payload.arithmetic.lhs];
          rhs = mobo->cpu.registers[instr.payload.arithmetic.rhs];
          goto compute;

        compute:
          res = lhs - rhs;

          if (res == 0) {
            mobo->cpu.crs.flags |= TENC32_FLAGS_ZERO;
            mobo->cpu.crs.flags &= ~TENC32_FLAGS_NEGATIVE;
            break;
          }

          mobo->cpu.crs.flags &= ~TENC32_FLAGS_ZERO;

          if (res > 0 && res > lhs)
            mobo->cpu.crs.flags &= ~TENC32_FLAGS_NEGATIVE;
          if (res > 0 && res < lhs)
            mobo->cpu.crs.flags |= TENC32_FLAGS_NEGATIVE;

        default:
          break;
      }
      goto OK_END;

    case TENC32_DECODED_AND:
      switch (instr.addressing) {
        case TENC32_ADDRESSING_ARITHMETIC_IMMEDIATE_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic_constant_reg.dest] =
            instr.payload.arithmetic_constant_reg.lhs &
            mobo->cpu.registers[instr.payload.arithmetic_constant_reg.rhs];
          break;

        case TENC32_ADDRESSING_ARITHMETIC_REGISTER_IMMEDIATE:
          mobo->cpu.registers[instr.payload.arithmetic_reg_constant.dest] =
            mobo->cpu.registers[instr.payload.arithmetic_reg_constant.lhs] &
            instr.payload.arithmetic_reg_constant.rhs;
          break;

        case TENC32_ADDRESSING_ARITHMETIC_REGISTER_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic.dest] =
            mobo->cpu.registers[instr.payload.arithmetic.lhs] &
            mobo->cpu.registers[instr.payload.arithmetic.rhs];
          break;

        default:
          break;
      }
      goto OK_END;

    case TENC32_DECODED_OR:
      switch (instr.addressing) {
        case TENC32_ADDRESSING_ARITHMETIC_IMMEDIATE_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic_constant_reg.dest] =
            instr.payload.arithmetic_constant_reg.lhs |
            mobo->cpu.registers[instr.payload.arithmetic_constant_reg.rhs];
          break;

        case TENC32_ADDRESSING_ARITHMETIC_REGISTER_IMMEDIATE:
          mobo->cpu.registers[instr.payload.arithmetic_reg_constant.dest] =
            mobo->cpu.registers[instr.payload.arithmetic_reg_constant.lhs] |
            instr.payload.arithmetic_reg_constant.rhs;
          break;

        case TENC32_ADDRESSING_ARITHMETIC_REGISTER_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic.dest] =
            mobo->cpu.registers[instr.payload.arithmetic.lhs] |
            mobo->cpu.registers[instr.payload.arithmetic.rhs];
          break;

        default:
          break;
      }
      goto OK_END;

    case TENC32_DECODED_XOR:
      switch (instr.addressing) {
        case TENC32_ADDRESSING_ARITHMETIC_IMMEDIATE_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic_constant_reg.dest] =
            instr.payload.arithmetic_constant_reg.lhs ^
            mobo->cpu.registers[instr.payload.arithmetic_constant_reg.rhs];
          break;

        case TENC32_ADDRESSING_ARITHMETIC_REGISTER_IMMEDIATE:
          mobo->cpu.registers[instr.payload.arithmetic_reg_constant.dest] =
            mobo->cpu.registers[instr.payload.arithmetic_reg_constant.lhs] ^
            instr.payload.arithmetic_reg_constant.rhs;
          break;

        case TENC32_ADDRESSING_ARITHMETIC_REGISTER_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic.dest] =
            mobo->cpu.registers[instr.payload.arithmetic.lhs] ^
            mobo->cpu.registers[instr.payload.arithmetic.rhs];
          break;

        default:
          break;
      }
      goto OK_END;

    case TENC32_DECODED_NOT:
      mobo->cpu.registers[instr.payload.not.dest] =
        ~mobo->cpu.registers[instr.payload.not.src];
      goto OK_END;

    case TENC32_DECODED_SHIFT_LEFT:
      switch (instr.addressing) {
        case TENC32_ADDRESSING_ARITHMETIC_IMMEDIATE_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic_constant_reg.dest] =
            instr.payload.arithmetic_constant_reg.lhs
            << mobo->cpu.registers[instr.payload.arithmetic_constant_reg.rhs];
          break;

        case TENC32_ADDRESSING_ARITHMETIC_REGISTER_IMMEDIATE:
          mobo->cpu.registers[instr.payload.arithmetic_reg_constant.dest] =
            mobo->cpu.registers[instr.payload.arithmetic_reg_constant.lhs]
            << instr.payload.arithmetic_reg_constant.rhs;
          break;

        case TENC32_ADDRESSING_ARITHMETIC_REGISTER_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic.dest] =
            mobo->cpu.registers[instr.payload.arithmetic.lhs]
            << mobo->cpu.registers[instr.payload.arithmetic.rhs];
          break;

        default:
          break;
      }
      goto OK_END;

    case TENC32_DECODED_SHIFT_RIGHT:
      switch (instr.addressing) {
        case TENC32_ADDRESSING_ARITHMETIC_IMMEDIATE_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic_constant_reg.dest] =
            instr.payload.arithmetic_constant_reg.lhs >>
            mobo->cpu.registers[instr.payload.arithmetic_constant_reg.rhs];
          break;

        case TENC32_ADDRESSING_ARITHMETIC_REGISTER_IMMEDIATE:
          mobo->cpu.registers[instr.payload.arithmetic_reg_constant.dest] =
            mobo->cpu.registers[instr.payload.arithmetic_reg_constant.lhs] >>
            instr.payload.arithmetic_reg_constant.rhs;
          break;

        case TENC32_ADDRESSING_ARITHMETIC_REGISTER_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic.dest] =
            mobo->cpu.registers[instr.payload.arithmetic.lhs] >>
            mobo->cpu.registers[instr.payload.arithmetic.rhs];
          break;

        default:
          break;
      }
      goto OK_END;
    case TENC32_DECODED_HALT:
      return TENC32_STEP_HALT;

    case TENC32_DECODED_CALL:
      mobo->cpu.registers[REGISTER_LEAF] = mobo->cpu.registers[REGISTER_PC];

      switch (instr.addressing) {
        case TENC32_ADDRESSING_CALL_IMMEDIATE:
          mobo->cpu.registers[REGISTER_PC] +=
            instr.payload.call_direct.offset - 4;
          break;

        case TENC32_ADDRESSING_CALL_INDIRECT:
          mobo->cpu.registers[REGISTER_PC] =
            mobo->cpu.registers[instr.payload.call_indirect.base];
          break;

        default:
          break;
      }
      goto OK_END;
    case TENC32_DECODED_SCR:
      switch ((enum tenc32_control_registers)instr.payload.scr.val) {
        case TENC32_CR0:
          mobo->cpu.crs.cr0 = mobo->cpu.registers[instr.payload.scr.reg];
          break;

        case TENC32_FLAGS:
          mobo->cpu.crs.flags = mobo->cpu.registers[instr.payload.scr.reg];
          break;

        case TENC32_CRI:
          mobo->cpu.crs.idtr = mobo->cpu.registers[instr.payload.scr.reg];
          break;

        case TENC32_IMASK:
          mobo->cpu.crs.imask = mobo->cpu.registers[instr.payload.scr.reg];
          break;

        default:
          assert(false);
      }
      goto OK_END;
    case TENC32_DECODED_LCR:
      switch ((enum tenc32_control_registers)instr.payload.lcr.val) {
        case TENC32_CR0:
          mobo->cpu.registers[instr.payload.lcr.reg] = mobo->cpu.crs.cr0;
          break;

        case TENC32_FLAGS:
          mobo->cpu.registers[instr.payload.lcr.reg] = mobo->cpu.crs.flags;
          break;

        case TENC32_CRI:
          mobo->cpu.registers[instr.payload.lcr.reg] = mobo->cpu.crs.idtr;
          break;

        case TENC32_IMASK:
          mobo->cpu.registers[instr.payload.lcr.reg] = mobo->cpu.crs.imask;
          break;

        default:
          assert(false);
      }
      goto OK_END;
    case TENC32_DECODED_SYSINTRET:
      trigger_isrt(mobo);
      goto OK_END;
    case TENC32_DECODED_SYSINT:
      trigger_isr(mobo, instr.payload.sysint.id);
      goto OK_END;
    case TENC32_DECODED_SYSJUMP:
      mobo->cpu.exception = TENC32_INTERRUPT_ILLEGAL_INSTRUCTION;
      goto OK_END;
  }

OK_END:
  mobo->cpu.registers[REGISTER_PC] += 4;
  return TENC32_STEP_OK;
}

void
tenc32_dump_registers(tenc32_motherboard_t* mobo)
{
  fprintf(stderr, "REGISTERS ");

  fprintf(stderr,
          "%#10x %#10x %#10x PC LEAF STACK",
          mobo->cpu.registers[REGISTER_PC],
          mobo->cpu.registers[REGISTER_LEAF],
          mobo->cpu.registers[REGISTER_STACK]);

  fprintf(stderr, "\nSCRATCH   ");
  for (unsigned i = REGISTER_S0; i < REGISTER_S4; i++)
    fprintf(stderr, "%#10x ", mobo->cpu.registers[i]);

  fprintf(stderr, "\nWORKING   ");
  for (unsigned i = REGISTER_R0; i < REGISTER_R7; i++)
    fprintf(stderr, "%#10x ", mobo->cpu.registers[i]);

  fputc('\n', stderr);
  fputc('\n', stderr);
}
