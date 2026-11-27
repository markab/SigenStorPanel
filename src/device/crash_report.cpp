#include "crash_report.h"

#include <Arduino.h>
#include <esp_app_desc.h>
#include <esp_core_dump.h>
#include <esp_system.h>
#include <soc/soc.h>
#include <string.h>

#include <memory>
#include <new>

// The linker's bounds of the code, flash and IRAM, and the chip's ROM: a word on
// the stack inside any of them is likely a return address. ROM holds the heap's
// TLSF allocator and much of the C library, where asserts often fire.
extern "C" char _text_start[];
extern "C" char _text_end[];
extern "C" char _iram_text_start[];
extern "C" char _iram_text_end[];

namespace {

SheetRestart restart_reason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
      return SheetRestart::PowerOn;
    case ESP_RST_SW:
      return SheetRestart::Software;
    case ESP_RST_PANIC:
      return SheetRestart::Crash;
    case ESP_RST_TASK_WDT:
      return SheetRestart::TaskWatchdog;
    case ESP_RST_INT_WDT:
      return SheetRestart::IntWatchdog;
    case ESP_RST_WDT:
      return SheetRestart::Watchdog;
    case ESP_RST_BROWNOUT:
    case ESP_RST_PWR_GLITCH:
      return SheetRestart::PowerDip;
    case ESP_RST_EXT:
      return SheetRestart::ResetPin;
    case ESP_RST_UNKNOWN:
      return SheetRestart::Unknown;
    default:
      return SheetRestart::Other;
  }
}

bool is_code(uint32_t address) {
  // RISC-V instructions are at least 2-byte aligned.
  if ((address & 1u) != 0) {
    return false;
  }
  const uintptr_t a = address;
  return (a >= reinterpret_cast<uintptr_t>(_text_start) &&
          a < reinterpret_cast<uintptr_t>(_text_end)) ||
         (a >= reinterpret_cast<uintptr_t>(_iram_text_start) &&
          a < reinterpret_cast<uintptr_t>(_iram_text_end)) ||
         (a >= SOC_IROM_MASK_LOW && a < SOC_IROM_MASK_HIGH);
}

}  // namespace

void crash_report_read(SheetCrash* out) {
  *out = SheetCrash{};
  out->restart = restart_reason();
  Serial.printf("[crash] started after: reset reason %d\n", static_cast<int>(esp_reset_reason()));

  if (esp_core_dump_image_check() != ESP_OK) {
    Serial.println("[crash] no core dump saved");
    return;
  }
  // About 1.2 KB, mostly the stack dump: not for the stack.
  std::unique_ptr<esp_core_dump_summary_t> summary(new (std::nothrow) esp_core_dump_summary_t());
  if (!summary || esp_core_dump_get_summary(summary.get()) != ESP_OK) {
    Serial.println("[crash] a core dump is saved but its summary could not be read");
    return;
  }

  out->saved = true;
  snprintf(out->build, sizeof(out->build), "%.*s", static_cast<int>(sizeof(summary->app_elf_sha256)),
           reinterpret_cast<const char*>(summary->app_elf_sha256));
  char running[sizeof(summary->app_elf_sha256)] = {};
  esp_app_get_elf_sha256(running, sizeof(running));
  out->this_build = strncmp(running, out->build, sizeof(running) - 1) == 0;
  snprintf(out->task, sizeof(out->task), "%.*s", static_cast<int>(sizeof(summary->exc_task)),
           summary->exc_task);
  out->pc = summary->exc_pc;
  out->ra = summary->ex_info.ra;
  out->sp = summary->ex_info.sp;
  out->mcause = summary->ex_info.mcause;
  out->mtval = summary->ex_info.mtval;

  // The stack from the faulting frame up: its code addresses are the callers,
  // nearest first, with some noise a computer sorts out.
  const uint32_t size = summary->exc_bt_info.dump_size;
  const uint8_t* stack = summary->exc_bt_info.stackdump;
  for (uint32_t offset = 0; offset + 4 <= size && out->stack_count < SHEET_CRASH_STACK;
       offset += 4) {
    uint32_t word;
    memcpy(&word, stack + offset, sizeof(word));
    if (is_code(word) && word != out->ra &&
        (out->stack_count == 0 || out->stack[out->stack_count - 1] != word)) {
      out->stack[out->stack_count++] = word;
    }
  }

  Serial.printf("[crash] saved crash: build %s (%s), task %s, pc %08lx ra %08lx sp %08lx "
                "mcause %08lx mtval %08lx\n",
                out->build, out->this_build ? "this one" : "earlier", out->task,
                static_cast<unsigned long>(out->pc), static_cast<unsigned long>(out->ra),
                static_cast<unsigned long>(out->sp), static_cast<unsigned long>(out->mcause),
                static_cast<unsigned long>(out->mtval));
  Serial.print("[crash] stack:");
  for (uint8_t i = 0; i < out->stack_count; ++i) {
    Serial.printf(" %08lx", static_cast<unsigned long>(out->stack[i]));
  }
  Serial.println();
}
