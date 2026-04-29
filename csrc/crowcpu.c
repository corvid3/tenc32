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

#include "ccc.h"
#include "cci.h"
#include "crow.brbt/brbt.h"
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
  cnd_init(&out->pic.mobo_sleeper);
  out->pic.incoming_flags = 0;
  out->pic.currently_handled = 0;

  out->cpu.exception = -1;
  out->cpu.crs.cr0 = 0;
  out->cpu.crs.idtr = 0;
  out->cpu.crs.imask = -1U;
  out->cpu.halting = false;
  memset(out->cpu.registers, 0, sizeof out->cpu.registers);

  out->cci.devices = NULL;
  out->cci.devices_cap = 0;
  out->cci.listening_socket = -1;
  out->cci.last_connection_tick = 0;
  out->cci.last_update_tick = 0;

  tenc32_mmu_init(out);
  tenc32_init_pic(out);
  tenc32_init_ccc(out);

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

  brbt_for(&cpu->mmu.hardware_io, i, {
    struct tenc32_hardware_io* io = brbt_get(&cpu->mmu.hardware_io, i);
    if (io->cleanup)
      io->cleanup(io);
  });
  mtx_destroy(&cpu->pic.mutex);
  free(cpu->memory);
  free(cpu);
}

void
tenc32_trigger_hardware_interrupt(tenc32_motherboard_t* mobo, unsigned irq)
{
  mtx_lock(&mobo->pic.mutex);
  assert(irq < MAX_IRQ);
  mobo->pic.incoming_flags |= 1U << irq;
  mtx_unlock(&mobo->pic.mutex);

  tenc32_awake_mobo(mobo);
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
  unsigned new_stack = 0;
  mobo->pic.currently_handled = 0;

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
  unsigned addr = (id * 4) + mobo->cpu.crs.idtr;
  unsigned isr_addr = 0;

  if (!tenc32_read_word(mobo, addr, &isr_addr))
    return false;

  /* -1u is reserved "unimplemented" interrupt */
  if (isr_addr == -1U)
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
  mobo->cpu.registers[REGISTER_PC] = isr_addr;

  return true;
}

#define UADDI(x, y) ((y) < 0) ? ((x) - (unsigned)(-(y))) : ((x) + (unsigned)(y))

void
tenc32_insert_exception_callback(tenc32_motherboard_t* mobo,
                                 void (*callback)(tenc32_motherboard_t*,
                                                  unsigned))
{
  mobo->exception_callback = callback;
}

static void
check_isr(tenc32_motherboard_t* restrict mobo)
{
  enum
  {
    HARDWARE_INT_OFFSET = 0x20,
  };

  mtx_lock(&mobo->pic.mutex);

  /* do not reincur interrupts */
  if (mobo->pic.currently_handled)
    goto leave;

  uint32_t const transformed =
    (mobo->pic.incoming_flags &
     (mobo->cpu.crs.cr0 & TENC32_CR0_MASK ? mobo->cpu.crs.imask : -1U));
  // & ~mobo.pic.currently_handled

  if (!transformed)
    goto leave;

  mobo->cpu.halting = false;

  if (mobo->cpu.crs.cr0 & TENC32_CR0_INTERRUPT) {
    unsigned i = 0;
    for (; i < MAX_IRQ && (~transformed >> i) & 1U; i++)
      ;
    if (i != MAX_IRQ) {
      mobo->pic.currently_handled = 1;
      // mobo->pic.currently_handled |= 1U << i;
      trigger_isr(mobo, HARDWARE_INT_OFFSET + i);
      mobo->pic.incoming_flags &= ~(1U << i);
    }
  }

leave:
  mtx_unlock(&mobo->pic.mutex);
}

enum tenc32_step_val
tenc32_step(tenc32_motherboard_t* mobo)
{
  uint32_t instr_word = 0;

restart:
  if (mobo->poweroff)
    return TENC32_STEP_POWEROFF;

  check_isr(mobo);

  if (mobo->cpu.halting)
    return TENC32_STEP_HALT;

  /* interrupt priority:
   *   1. hardware
   *   2. exception
   *   3. software
   */
  if (mobo->cpu.exception != -1) {
    mobo->exception_callback(mobo, mobo->cpu.exception);
    if (!trigger_isr(mobo, mobo->cpu.exception))
      return TENC32_STEP_CRASH;
    mobo->cpu.exception = -1;
  }

  struct resolve res;
  if (!tenc32_read_word_ex(
        mobo, mobo->cpu.registers[REGISTER_PC], &instr_word, &res))
    return TENC32_STEP_OK;

  if (res.io_space)
    return TENC32_STEP_HALT;

  if (!res.segment->flags.execute) {
    mobo->cpu.exception = TENC32_INTERRUPT_SEGMENTATION_FAULT;
    goto restart;
  }

  tenc32_arch_decoded_instruction instr;

  if (!tenc32_arch_decode(&instr, instr_word)) {
    mobo->cpu.exception = TENC32_INTERRUPT_ILLEGAL_INSTRUCTION;
    EDPRINT("invalid instruction detected in step loop");
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
      break;

    case TENC32_DECODED_LOAD:
      // fprintf(stderr,
      //         "load: %#x\n",
      //         UADDI(mobo->cpu.registers[instr.payload.mem_reg_indirect.base],
      //               (signed)instr.payload.mem_reg_indirect.offset));
      switch (instr.addressing) {
        case TENC32_ADDRESSING_MEMORY_BASE_OFFSET_WORD:
          tenc32_read_word(
            mobo,
            (mobo->cpu.registers[instr.payload.mem_reg_indirect.base] +
             (unsigned)instr.payload.mem_reg_indirect.offset),
            &mobo->cpu.registers[instr.payload.mem_reg_indirect.reg]);
          break;

        case TENC32_ADDRESSING_MEMORY_BASE_OFFSET_BYTE:
          tenc32_read_byte(
            mobo,
            (mobo->cpu.registers[instr.payload.mem_reg_indirect.base] +
             (unsigned)instr.payload.mem_reg_indirect.offset),
            &mobo->cpu.registers[instr.payload.mem_reg_indirect.reg]);
          break;

        default:
          break;
      }
      break;

    case TENC32_DECODED_STORE:
      // fprintf(stderr,
      //         "store: %#x\n",
      //         UADDI(mobo->cpu.registers[instr.payload.mem_reg_indirect.base],
      //               (signed)instr.payload.mem_reg_indirect.offset));
      switch (instr.addressing) {
        case TENC32_ADDRESSING_MEMORY_BASE_OFFSET_WORD:
          tenc32_write_word(
            mobo,
            (mobo->cpu.registers[instr.payload.mem_reg_indirect.base] +
             (unsigned)instr.payload.mem_reg_indirect.offset),
            mobo->cpu.registers[instr.payload.mem_reg_indirect.reg]);
          break;

        case TENC32_ADDRESSING_MEMORY_BASE_OFFSET_BYTE:
          tenc32_write_byte(
            mobo,
            (mobo->cpu.registers[instr.payload.mem_reg_indirect.base] +
             (unsigned)instr.payload.mem_reg_indirect.offset),
            mobo->cpu.registers[instr.payload.mem_reg_indirect.reg]);
          break;

        default:
          break;
      }
      break;

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
      break;

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
      break;

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
      break;

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
      break;

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
      break;

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
      break;

    case TENC32_DECODED_NOT:
      mobo->cpu.registers[instr.payload.not.dest] =
        ~mobo->cpu.registers[instr.payload.not.src];
      break;

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
      break;

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
      break;

    case TENC32_DECODED_HALT:
      mobo->cpu.halting = true;
      mobo->cpu.registers[REGISTER_PC] += 4;
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
            mobo->cpu.registers[instr.payload.call_indirect.base] - 4;
          break;

        default:
          assert(false);
      }
      break;

    case TENC32_DECODED_SCR:
      switch ((enum tenc32_control_registers)instr.payload.scr.val) {
        case TENC32_CR0:
          mobo->cpu.crs.cr0 = mobo->cpu.registers[instr.payload.scr.reg];
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
      break;

    case TENC32_DECODED_LCR:
      switch ((enum tenc32_control_registers)instr.payload.lcr.val) {
        case TENC32_CR0:
          mobo->cpu.registers[instr.payload.lcr.reg] = mobo->cpu.crs.cr0;
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
      break;

    case TENC32_DECODED_SYSINTRET:
      trigger_isrt(mobo);
      return TENC32_STEP_OK;

    case TENC32_DECODED_SYSINT:
      trigger_isr(mobo, instr.payload.sysint.id);
      return TENC32_STEP_OK;

    case TENC32_DECODED_SYSJUMP:
      mobo->cpu.exception = TENC32_INTERRUPT_ILLEGAL_INSTRUCTION;
      break;

    case TENC32_DECODED_BRANCH_EQ: {
      if (mobo->cpu.registers[instr.payload.branch.lhs] ==
          mobo->cpu.registers[instr.payload.branch.rhs])
        mobo->cpu.registers[REGISTER_PC] =
          mobo->cpu.registers[REGISTER_PC] + instr.payload.branch.ip_addend - 4;
    } break;

    case TENC32_DECODED_BRANCH_NOTEQ: {
      if (mobo->cpu.registers[instr.payload.branch.lhs] !=
          mobo->cpu.registers[instr.payload.branch.rhs])
        mobo->cpu.registers[REGISTER_PC] =
          mobo->cpu.registers[REGISTER_PC] + instr.payload.branch.ip_addend - 4;
    } break;

    case TENC32_DECODED_BRANCH_IGREATER: {
      if ((int32_t)mobo->cpu.registers[instr.payload.branch.lhs] >
          (int32_t)mobo->cpu.registers[instr.payload.branch.rhs])
        mobo->cpu.registers[REGISTER_PC] =
          mobo->cpu.registers[REGISTER_PC] + instr.payload.branch.ip_addend - 4;
    } break;

    case TENC32_DECODED_BRANCH_IGREATEREQ: {
      if ((int32_t)mobo->cpu.registers[instr.payload.branch.lhs] >=
          (int32_t)mobo->cpu.registers[instr.payload.branch.rhs])
        mobo->cpu.registers[REGISTER_PC] =
          mobo->cpu.registers[REGISTER_PC] + instr.payload.branch.ip_addend - 4;
    } break;

    case TENC32_DECODED_BRANCH_UGREATER: {
      if ((uint32_t)mobo->cpu.registers[instr.payload.branch.lhs] >
          (uint32_t)mobo->cpu.registers[instr.payload.branch.rhs])
        mobo->cpu.registers[REGISTER_PC] =
          mobo->cpu.registers[REGISTER_PC] + instr.payload.branch.ip_addend - 4;
    } break;

    case TENC32_DECODED_BRANCH_UGREATEREQ: {
      if ((uint32_t)mobo->cpu.registers[instr.payload.branch.lhs] >=
          (uint32_t)mobo->cpu.registers[instr.payload.branch.rhs])
        mobo->cpu.registers[REGISTER_PC] =
          mobo->cpu.registers[REGISTER_PC] + instr.payload.branch.ip_addend - 4;
    } break;

    case TENC32_DECODED_BRANCH_ZERO: {
      if (mobo->cpu.registers[instr.payload.branch.lhs] == 0)
        mobo->cpu.registers[REGISTER_PC] =
          mobo->cpu.registers[REGISTER_PC] + instr.payload.branch.ip_addend - 4;
    } break;

    case TENC32_DECODED_BRANCH_NOT_ZERO: {
      if (mobo->cpu.registers[instr.payload.branch.lhs] != 0)
        mobo->cpu.registers[REGISTER_PC] =
          mobo->cpu.registers[REGISTER_PC] + instr.payload.branch.ip_addend - 4;
    } break;

    case TENC32_DECODED_SIGN_EXTEND:
      assert(false);
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

void
tenc32_halt_sleep(tenc32_motherboard_t* mobo)
{
  mtx_lock(&mobo->pic.mutex);
  if (!mobo->pic.incoming_flags) {
    cnd_wait(&mobo->pic.mobo_sleeper, &mobo->pic.mutex);
    mtx_unlock(&mobo->pic.mutex);
  } else {
    mtx_unlock(&mobo->pic.mutex);
  }
}

void
tenc32_awake_mobo(tenc32_motherboard_t* mobo)
{
  mtx_lock(&mobo->pic.mutex);
  cnd_signal(&mobo->pic.mobo_sleeper);
  mtx_unlock(&mobo->pic.mutex);
}
