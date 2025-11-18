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
#include <time.h>
#include <unistd.h>

#include "cci.h"
#include "crowcpu.h"
#include "mmu.h"
#include "mobo.h"

#define longjmp(x, y) assert(false)

/* how many instruction cycles before we check a new connection */
#define CCI_CHECK_INTERVAL 1024

static jmp_buf fault_jump;

void
crowcpu_default_configuration(crowcpu_configuration_t* config)
{
  config->cci_con_ms = 1000;
  config->cci_update_ms = 1;
}

void
crowcpu_motherboard_send_irq(crowcpu_motherboard_t* mobo, unsigned short ir)
{
  assert(false);
  assert(ir < CROWCPU_MAX_DEFINABLE_INTERRUPTS);
  mobo->cpu.interrupt_requests[ir] = true;
}

crowcpu_motherboard_t*
crowcpu_motherboard_create(crowcpu_configuration_t* conf, size_t ram_size)
{
  crowcpu_motherboard_t* out = malloc(sizeof *out);
  out->conf = *conf;

  if (ram_size < CROWCPU_MIN_RAM_SIZE)
    return NULL;

  out->memory_size = ram_size;
  out->memory = malloc(ram_size);

  out->cpu.flags.mode = 0;
  out->cpu.interrupt_table_size = 0;

  out->cci.devices = NULL;
  out->cci.devices_cap = 0;
  out->cci.listening_socket = -1;
  out->cci.last_connection_tick = 0;
  out->cci.last_update_tick = 0;

  crowcpu_mmu_init(out);

  return out;
}

void
crowcpu_motherboard_destroy(struct crowcpu_motherboard_t* cpu)
{
  assert(cpu != NULL);

  if (cpu->cci.listening_socket != -1)
    close(cpu->cci.listening_socket);
  if (cpu->cci.devices != NULL)
    free(cpu->cci.devices);

  free(cpu->memory);
  free(cpu);
}

void
crowcpu_restart(struct crowcpu_motherboard_t* mobo,
                char const (*bios)[CROWCPU_ROM_SIZE])
{
  mobo->cpu.registers[REGISTER_PC] = 0x0000;

  mobo->cpu.flags.mode = 0;
  mobo->cpu.interrupt_table_size = 0;

  crowcpu_mmu_reset(mobo);

  assert(mobo->memory_size >= CROWCPU_ROM_SIZE);
  memcpy(mobo->memory, (*bios), CROWCPU_ROM_SIZE);
}

enum crowcpu_step_val
crowcpu_step(crowcpu_motherboard_t* mobo)
{
begin:;
  uint32_t instr_word;

  if (!crowcpu_read_word(mobo, mobo->cpu.registers[REGISTER_PC], &instr_word)) {
    crowcpu_motherboard_send_irq(mobo, CROWCPU_INTERRUPT_ILLEGAL_INSTRUCTION);
    return CROWCPU_STEP_OK;
  }

  crowcpu_arch_decoded_instruction instr;
  mobo->cpu.registers[REGISTER_PC] += 4;

  if (setjmp(fault_jump) != 0) {
    /* trigger fault condition */
    /* TODO */
  }

  if (!crowcpu_arch_decode(&instr, instr_word)) {
    crowcpu_motherboard_send_irq(mobo, CROWCPU_INTERRUPT_ILLEGAL_INSTRUCTION);
    goto begin;
  }

  switch (instr.condition) {
    case CROWCPU_COND_UNCONDITIONAL:
      break;

    case CROWCPU_COND_GREATER:
      if (!mobo->cpu.flags.test_lt && !mobo->cpu.flags.test_eq)
        break;
      else
        return CROWCPU_STEP_OK;

    case CROWCPU_COND_LESS:
      if (mobo->cpu.flags.test_lt && !mobo->cpu.flags.test_eq)
        break;
      else
        return CROWCPU_STEP_OK;

    case CROWCPU_COND_EQUAL:
      if (mobo->cpu.flags.test_eq)
        break;
      else
        return CROWCPU_STEP_OK;

    case CROWCPU_COND_NOT_EQUAL:
      if (!mobo->cpu.flags.test_eq)
        break;
      else
        return CROWCPU_STEP_OK;

    case CROWCPU_COND_GREATER_EQ:
      if (!mobo->cpu.flags.test_lt || mobo->cpu.flags.test_eq)
        break;
      else
        return CROWCPU_STEP_OK;

    case CROWCPU_COND_LESS_EQ:
      if (mobo->cpu.flags.test_lt || mobo->cpu.flags.test_eq)
        break;
      else
        return CROWCPU_STEP_OK;
  }

  switch (instr.instruction) {
    case CROWCPU_DECODED_MOVE:
      switch (instr.addressing) {
        case CROWCPU_ADDRESSING_MOVE_REG_REG:
          mobo->cpu.registers[instr.payload.move_reg_reg.dest] =
            mobo->cpu.registers[instr.payload.move_reg_reg.src];
          break;

        case CROWCPU_ADDRESSING_MOVE_REG_IMM:
          mobo->cpu.registers[instr.payload.move_reg_imm.dest] =
            instr.payload.move_reg_imm.imm;
          break;

        default:
          break;
      }
      break;

    case CROWCPU_DECODED_LOAD:
      switch (instr.addressing) {
        case CROWCPU_ADDRESSING_MEMORY_BASE_OFFSET_WORD:
          crowcpu_read_word(
            mobo,
            mobo->cpu.registers[instr.payload.mem_reg_indirect.base] +
              instr.payload.mem_reg_indirect.offset,
            &mobo->cpu.registers[instr.payload.mem_reg_indirect.reg]);
          break;

        case CROWCPU_ADDRESSING_MEMORY_BASE_OFFSET_BYTE:
          crowcpu_read_byte(
            mobo,
            mobo->cpu.registers[instr.payload.mem_reg_indirect.base] +
              instr.payload.mem_reg_indirect.offset,
            &mobo->cpu.registers[instr.payload.mem_reg_indirect.reg]);
          break;

        default:
          break;
      }
      break;

    case CROWCPU_DECODED_STORE:
      switch (instr.addressing) {
        case CROWCPU_ADDRESSING_MEMORY_BASE_OFFSET_WORD:
          crowcpu_write_word(
            mobo,
            mobo->cpu.registers[instr.payload.mem_reg_indirect.base] +
              instr.payload.mem_reg_indirect.offset,
            mobo->cpu.registers[instr.payload.mem_reg_indirect.reg]);
          break;

        case CROWCPU_ADDRESSING_MEMORY_BASE_OFFSET_BYTE:
          crowcpu_write_byte(
            mobo,
            mobo->cpu.registers[instr.payload.mem_reg_indirect.base] +
              instr.payload.mem_reg_indirect.offset,
            mobo->cpu.registers[instr.payload.mem_reg_indirect.reg]);
          break;

        default:
          break;
      }
      break;

    case CROWCPU_DECODED_ADD:
      switch (instr.addressing) {
        case CROWCPU_ADDRESSING_ARITHMETIC_IMMEDIATE_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic_constant_reg.dest] =
            instr.payload.arithmetic_constant_reg.lhs +
            mobo->cpu.registers[instr.payload.arithmetic_constant_reg.rhs];
          break;

        case CROWCPU_ADDRESSING_ARITHMETIC_REGISTER_IMMEDIATE:
          mobo->cpu.registers[instr.payload.arithmetic_reg_constant.dest] =
            mobo->cpu.registers[instr.payload.arithmetic_reg_constant.lhs] +
            instr.payload.arithmetic_reg_constant.rhs;
          break;

        case CROWCPU_ADDRESSING_ARITHMETIC_REGISTER_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic.dest] =
            mobo->cpu.registers[instr.payload.arithmetic.lhs] +
            mobo->cpu.registers[instr.payload.arithmetic.rhs];
          break;

        default:
          break;
      }
      break;

    case CROWCPU_DECODED_SUB:
      switch (instr.addressing) {
        case CROWCPU_ADDRESSING_ARITHMETIC_IMMEDIATE_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic_constant_reg.dest] =
            instr.payload.arithmetic_constant_reg.lhs -
            mobo->cpu.registers[instr.payload.arithmetic_constant_reg.rhs];
          break;

        case CROWCPU_ADDRESSING_ARITHMETIC_REGISTER_IMMEDIATE:
          mobo->cpu.registers[instr.payload.arithmetic_reg_constant.dest] =
            mobo->cpu.registers[instr.payload.arithmetic_reg_constant.lhs] -
            instr.payload.arithmetic_reg_constant.rhs;
          break;

        case CROWCPU_ADDRESSING_ARITHMETIC_REGISTER_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic.dest] =
            mobo->cpu.registers[instr.payload.arithmetic.lhs] -
            mobo->cpu.registers[instr.payload.arithmetic.rhs];
          break;

        default:
          break;
      }
      break;

    case CROWCPU_DECODED_TEST:
      switch (instr.addressing) {
        unsigned res;
        unsigned lhs;
        unsigned rhs;

        case CROWCPU_ADDRESSING_ARITHMETIC_IMMEDIATE_REGISTER:
          lhs = instr.payload.arithmetic_constant_reg.lhs;
          rhs = mobo->cpu.registers[instr.payload.arithmetic_constant_reg.rhs];
          goto compute;

        case CROWCPU_ADDRESSING_ARITHMETIC_REGISTER_IMMEDIATE:
          lhs = mobo->cpu.registers[instr.payload.arithmetic_reg_constant.lhs];
          rhs = instr.payload.arithmetic_reg_constant.rhs;
          goto compute;

        case CROWCPU_ADDRESSING_ARITHMETIC_REGISTER_REGISTER:
          lhs = mobo->cpu.registers[instr.payload.arithmetic.lhs];
          rhs = mobo->cpu.registers[instr.payload.arithmetic.rhs];
          goto compute;

        compute:
          res = lhs - rhs;

          if (res == 0) {
            mobo->cpu.flags.test_eq = true, mobo->cpu.flags.test_lt = false;
            break;
          }

          mobo->cpu.flags.test_eq = false;

          if (res > 0 && res > lhs)
            mobo->cpu.flags.test_lt = false;
          if (res > 0 && res < lhs)
            mobo->cpu.flags.test_lt = true;

        default:
          break;
      }
      break;

    case CROWCPU_DECODED_AND:
      switch (instr.addressing) {
        case CROWCPU_ADDRESSING_ARITHMETIC_IMMEDIATE_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic_constant_reg.dest] =
            instr.payload.arithmetic_constant_reg.lhs &
            mobo->cpu.registers[instr.payload.arithmetic_constant_reg.rhs];
          break;

        case CROWCPU_ADDRESSING_ARITHMETIC_REGISTER_IMMEDIATE:
          mobo->cpu.registers[instr.payload.arithmetic_reg_constant.dest] =
            mobo->cpu.registers[instr.payload.arithmetic_reg_constant.lhs] &
            instr.payload.arithmetic_reg_constant.rhs;
          break;

        case CROWCPU_ADDRESSING_ARITHMETIC_REGISTER_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic.dest] =
            mobo->cpu.registers[instr.payload.arithmetic.lhs] &
            mobo->cpu.registers[instr.payload.arithmetic.rhs];
          break;

        default:
          break;
      }
      break;

    case CROWCPU_DECODED_OR:
      switch (instr.addressing) {
        case CROWCPU_ADDRESSING_ARITHMETIC_IMMEDIATE_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic_constant_reg.dest] =
            instr.payload.arithmetic_constant_reg.lhs |
            mobo->cpu.registers[instr.payload.arithmetic_constant_reg.rhs];
          break;

        case CROWCPU_ADDRESSING_ARITHMETIC_REGISTER_IMMEDIATE:
          mobo->cpu.registers[instr.payload.arithmetic_reg_constant.dest] =
            mobo->cpu.registers[instr.payload.arithmetic_reg_constant.lhs] |
            instr.payload.arithmetic_reg_constant.rhs;
          break;

        case CROWCPU_ADDRESSING_ARITHMETIC_REGISTER_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic.dest] =
            mobo->cpu.registers[instr.payload.arithmetic.lhs] |
            mobo->cpu.registers[instr.payload.arithmetic.rhs];
          break;

        default:
          break;
      }
      break;

    case CROWCPU_DECODED_XOR:
      switch (instr.addressing) {
        case CROWCPU_ADDRESSING_ARITHMETIC_IMMEDIATE_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic_constant_reg.dest] =
            instr.payload.arithmetic_constant_reg.lhs ^
            mobo->cpu.registers[instr.payload.arithmetic_constant_reg.rhs];
          break;

        case CROWCPU_ADDRESSING_ARITHMETIC_REGISTER_IMMEDIATE:
          mobo->cpu.registers[instr.payload.arithmetic_reg_constant.dest] =
            mobo->cpu.registers[instr.payload.arithmetic_reg_constant.lhs] ^
            instr.payload.arithmetic_reg_constant.rhs;
          break;

        case CROWCPU_ADDRESSING_ARITHMETIC_REGISTER_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic.dest] =
            mobo->cpu.registers[instr.payload.arithmetic.lhs] ^
            mobo->cpu.registers[instr.payload.arithmetic.rhs];
          break;

        default:
          break;
      }
      break;

    case CROWCPU_DECODED_NOT:
      mobo->cpu.registers[instr.payload.not.dest] =
        ~mobo->cpu.registers[instr.payload.not.src];
      break;

    case CROWCPU_DECODED_SHIFT_LEFT:
      switch (instr.addressing) {
        case CROWCPU_ADDRESSING_ARITHMETIC_IMMEDIATE_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic_constant_reg.dest] =
            instr.payload.arithmetic_constant_reg.lhs
            << mobo->cpu.registers[instr.payload.arithmetic_constant_reg.rhs];
          break;

        case CROWCPU_ADDRESSING_ARITHMETIC_REGISTER_IMMEDIATE:
          mobo->cpu.registers[instr.payload.arithmetic_reg_constant.dest] =
            mobo->cpu.registers[instr.payload.arithmetic_reg_constant.lhs]
            << instr.payload.arithmetic_reg_constant.rhs;
          break;

        case CROWCPU_ADDRESSING_ARITHMETIC_REGISTER_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic.dest] =
            mobo->cpu.registers[instr.payload.arithmetic.lhs]
            << mobo->cpu.registers[instr.payload.arithmetic.rhs];
          break;

        default:
          break;
      }
      break;

    case CROWCPU_DECODED_SHIFT_RIGHT:
      switch (instr.addressing) {
        case CROWCPU_ADDRESSING_ARITHMETIC_IMMEDIATE_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic_constant_reg.dest] =
            instr.payload.arithmetic_constant_reg.lhs >>
            mobo->cpu.registers[instr.payload.arithmetic_constant_reg.rhs];
          break;

        case CROWCPU_ADDRESSING_ARITHMETIC_REGISTER_IMMEDIATE:
          mobo->cpu.registers[instr.payload.arithmetic_reg_constant.dest] =
            mobo->cpu.registers[instr.payload.arithmetic_reg_constant.lhs] >>
            instr.payload.arithmetic_reg_constant.rhs;
          break;

        case CROWCPU_ADDRESSING_ARITHMETIC_REGISTER_REGISTER:
          mobo->cpu.registers[instr.payload.arithmetic.dest] =
            mobo->cpu.registers[instr.payload.arithmetic.lhs] >>
            mobo->cpu.registers[instr.payload.arithmetic.rhs];
          break;

        default:
          break;
      }
      break;

    case CROWCPU_DECODED_HALT:
      return CROWCPU_STEP_HALT;

    case CROWCPU_DECODED_CALL:
      mobo->cpu.registers[REGISTER_LEAF] = mobo->cpu.registers[REGISTER_PC];
      mobo->cpu.registers[REGISTER_PC] =
        mobo->cpu.registers[instr.payload.call_indirect.base];
      break;

    case CROWCPU_DECODED_SYSJUMP:
    default:
      crowcpu_motherboard_send_irq(mobo, CROWCPU_INTERRUPT_ILLEGAL_INSTRUCTION);
      longjmp(fault_jump, 0);
      break;
  }

  return CROWCPU_STEP_OK;
}

void
crowcpu_dump_registers(crowcpu_motherboard_t* mobo)
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
