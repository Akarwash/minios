#include "isr.h"
#include "idt.h"
#include "../drivers/screen.h"
#include "../drivers/ports.h"
#include "../include/vectors.h"
#include "signal.h"

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);  extern void irq3(void);
extern void irq4(void);  extern void irq5(void);  extern void irq6(void);  extern void irq7(void);
extern void irq8(void);  extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);

// The single syscall entry point (kernel/isr_stubs.asm), named by the lone DPL 3
// gate installed below at SYSCALL_VECTOR.
extern void syscall_stub(void);

static isr_handler_t interrupt_handlers[256];

#define KCODE     0x08  // kernel code segment selector
#define GATE      0x8E  // present, ring 0, 64-bit interrupt gate
#define GATE_USER 0xEE  // present, ring 3, 64-bit interrupt gate (0x8E | DPL 3)

void isr_install(void) {
    // Zero the IDT, remap the PIC, and load the IDT register first.
    idt_init();

    idt_set_entry(0,  (uint64_t)isr0,  KCODE, GATE);
    idt_set_entry(1,  (uint64_t)isr1,  KCODE, GATE);
    idt_set_entry(2,  (uint64_t)isr2,  KCODE, GATE);
    idt_set_entry(3,  (uint64_t)isr3,  KCODE, GATE);
    idt_set_entry(4,  (uint64_t)isr4,  KCODE, GATE);
    idt_set_entry(5,  (uint64_t)isr5,  KCODE, GATE);
    idt_set_entry(6,  (uint64_t)isr6,  KCODE, GATE);
    idt_set_entry(7,  (uint64_t)isr7,  KCODE, GATE);
    idt_set_entry(8,  (uint64_t)isr8,  KCODE, GATE);
    idt_set_entry(9,  (uint64_t)isr9,  KCODE, GATE);
    idt_set_entry(10, (uint64_t)isr10, KCODE, GATE);
    idt_set_entry(11, (uint64_t)isr11, KCODE, GATE);
    idt_set_entry(12, (uint64_t)isr12, KCODE, GATE);
    idt_set_entry(13, (uint64_t)isr13, KCODE, GATE);
    idt_set_entry(14, (uint64_t)isr14, KCODE, GATE);
    idt_set_entry(15, (uint64_t)isr15, KCODE, GATE);
    idt_set_entry(16, (uint64_t)isr16, KCODE, GATE);
    idt_set_entry(17, (uint64_t)isr17, KCODE, GATE);
    idt_set_entry(18, (uint64_t)isr18, KCODE, GATE);
    idt_set_entry(19, (uint64_t)isr19, KCODE, GATE);
    idt_set_entry(20, (uint64_t)isr20, KCODE, GATE);
    idt_set_entry(21, (uint64_t)isr21, KCODE, GATE);
    idt_set_entry(22, (uint64_t)isr22, KCODE, GATE);
    idt_set_entry(23, (uint64_t)isr23, KCODE, GATE);
    idt_set_entry(24, (uint64_t)isr24, KCODE, GATE);
    idt_set_entry(25, (uint64_t)isr25, KCODE, GATE);
    idt_set_entry(26, (uint64_t)isr26, KCODE, GATE);
    idt_set_entry(27, (uint64_t)isr27, KCODE, GATE);
    idt_set_entry(28, (uint64_t)isr28, KCODE, GATE);
    idt_set_entry(29, (uint64_t)isr29, KCODE, GATE);
    idt_set_entry(30, (uint64_t)isr30, KCODE, GATE);
    idt_set_entry(31, (uint64_t)isr31, KCODE, GATE);

    // Hardware IRQ gates at the self-describing base (vectors.h): IRQ line N is
    // delivered at PIC_MASTER_VECTOR_BASE + N, so IRQ0 (timer) lands at 0x40 and
    // IRQ15 at 0x4F. The offsets 0..15 are IRQ line indices, not raw vectors.
    idt_set_entry(PIC_MASTER_VECTOR_BASE + 0,  (uint64_t)irq0,  KCODE, GATE);
    idt_set_entry(PIC_MASTER_VECTOR_BASE + 1,  (uint64_t)irq1,  KCODE, GATE);
    idt_set_entry(PIC_MASTER_VECTOR_BASE + 2,  (uint64_t)irq2,  KCODE, GATE);
    idt_set_entry(PIC_MASTER_VECTOR_BASE + 3,  (uint64_t)irq3,  KCODE, GATE);
    idt_set_entry(PIC_MASTER_VECTOR_BASE + 4,  (uint64_t)irq4,  KCODE, GATE);
    idt_set_entry(PIC_MASTER_VECTOR_BASE + 5,  (uint64_t)irq5,  KCODE, GATE);
    idt_set_entry(PIC_MASTER_VECTOR_BASE + 6,  (uint64_t)irq6,  KCODE, GATE);
    idt_set_entry(PIC_MASTER_VECTOR_BASE + 7,  (uint64_t)irq7,  KCODE, GATE);
    idt_set_entry(PIC_MASTER_VECTOR_BASE + 8,  (uint64_t)irq8,  KCODE, GATE);
    idt_set_entry(PIC_MASTER_VECTOR_BASE + 9,  (uint64_t)irq9,  KCODE, GATE);
    idt_set_entry(PIC_MASTER_VECTOR_BASE + 10, (uint64_t)irq10, KCODE, GATE);
    idt_set_entry(PIC_MASTER_VECTOR_BASE + 11, (uint64_t)irq11, KCODE, GATE);
    idt_set_entry(PIC_MASTER_VECTOR_BASE + 12, (uint64_t)irq12, KCODE, GATE);
    idt_set_entry(PIC_MASTER_VECTOR_BASE + 13, (uint64_t)irq13, KCODE, GATE);
    idt_set_entry(PIC_MASTER_VECTOR_BASE + 14, (uint64_t)irq14, KCODE, GATE);
    idt_set_entry(PIC_MASTER_VECTOR_BASE + 15, (uint64_t)irq15, KCODE, GATE);

    // The one and only DPL 3 gate. DPL on a gate is the lowest privilege level
    // allowed to reach it through `int N`; every gate above is DPL 0, so ring 3
    // cannot forge a fault or an IRQ. Only SYSCALL_VECTOR (0x50) is opened to
    // ring 3, and deliberately: a DPL 3 gate on vector 0x0E would let a user
    // program raise `int $0x0E` and fake a page fault, and one on 0x40 would let
    // it fake a timer tick, both feeding the kernel lies about hardware state.
    // The syscall gate stays an interrupt gate (GATE_USER carries gate type 0x0E,
    // not the trap type 0x0F), so the CPU clears IF on entry and a syscall cannot
    // be interrupted, including by another syscall; they do not nest.
    idt_set_entry(SYSCALL_VECTOR, (uint64_t)syscall_stub, KCODE, GATE_USER);

    // Handlers are in place; now enable hardware interrupts.
    __asm__ __volatile__("sti");
}

void register_interrupt_handler(uint8_t n, isr_handler_t handler) {
    interrupt_handlers[n] = handler;
}

// One line per exception in this kernel's own teaching vocabulary: the finger
// is the instruction pointer, boxes are memory addresses, fake vs real is
// virtual vs physical. Indexed by vector; every slot 0..31 is filled so a stray
// exception still resolves to words, not a bare number.
typedef struct {
    char *name;
    char *what;   // plain English, what actually happened, not Intel's mnemonic
} exception_info_t;

static const exception_info_t exceptions[EXC_COUNT] = {
    { "divide by zero",       "the finger divided a box by nothing" },              // 0
    { "debug",                "a debug trap fired, a single step or a hardware breakpoint" }, // 1
    { "non-maskable",         "an unmaskable hardware line went off, something urgent outside the CPU" }, // 2
    { "breakpoint",           "the finger hit a planted int3 breakpoint" },         // 3
    { "overflow",             "an into instruction ran with the overflow flag set" }, // 4
    { "bound range",          "an index fell outside its bound check" },            // 5
    { "invalid opcode",       "the finger read a box that isn't a real instruction" }, // 6
    { "device not available", "the FPU or SSE was used while it was switched off" }, // 7
    { "double fault",         "a fault happened while handling a fault" },          // 8
    { "coprocessor overrun",  "a legacy coprocessor segment overrun, unused on this CPU" }, // 9
    { "invalid TSS",          "the CPU followed a task selector into a broken TSS" }, // 10
    { "segment not present",  "a segment was used whose present bit is clear" },     // 11
    { "stack-segment fault",  "a stack access broke its segment limit or hit a not-present stack" }, // 12
    { "general protection",   "the finger tried an operation the rules forbid" },    // 13
    { "page fault",           "the finger reached for a box that isn't mapped" },    // 14
    { "reserved",             "a vector Intel reserves, this should not fire" },     // 15
    { "x87 floating point",   "the x87 FPU reported a pending arithmetic error" },   // 16
    { "alignment check",      "a misaligned access happened while alignment checks are on" }, // 17
    { "machine check",        "the hardware reported an internal fault of its own" }, // 18
    { "SIMD floating point",  "an SSE or AVX floating-point operation faulted" },     // 19
    { "virtualization",       "a virtualization (EPT) violation was delivered to the guest" }, // 20
    { "control protection",   "a shadow-stack or CET control-flow violation" },       // 21
    { "reserved",             "a vector Intel reserves, this should not fire" },      // 22
    { "reserved",             "a vector Intel reserves, this should not fire" },      // 23
    { "reserved",             "a vector Intel reserves, this should not fire" },      // 24
    { "reserved",             "a vector Intel reserves, this should not fire" },      // 25
    { "reserved",             "a vector Intel reserves, this should not fire" },      // 26
    { "reserved",             "a vector Intel reserves, this should not fire" },      // 27
    { "hypervisor injection", "a hypervisor injection exception" },                   // 28
    { "VMM communication",    "an SEV-ES VMM communication exception" },              // 29
    { "security",             "a security-sensitive event was raised" },              // 30
    { "reserved",             "a vector Intel reserves, this should not fire" },      // 31
};

// Print an unsigned 64-bit value as 0x + 16 uppercase hex digits. There is no
// hex path in the screen driver (only decimal print_int), and a fault dump needs
// addresses, so the formatter lives here where it is used.
static void print_hex64(uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buf[2 + i] = digits[(value >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\0';
    print_string(buf);
}

// Print a byte as 0x + 2 hex digits, for the compact vector number in the header.
static void print_hex_byte(uint8_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char buf[5];
    buf[0] = '0';
    buf[1] = 'x';
    buf[2] = digits[(value >> 4) & 0xF];
    buf[3] = digits[value & 0xF];
    buf[4] = '\0';
    print_string(buf);
}

// The desk (registers) every exception should show: where the finger was, the
// code segment it ran in, and the flags at the moment of the fault.
static void print_fault_frame(registers_t *regs) {
    print_string("  the finger (RIP) : "); print_hex64(regs->rip);    print_char('\n');
    print_string("  code seg   (CS)  : "); print_hex64(regs->cs);     print_char('\n');
    print_string("  flags    (RFLAGS): "); print_hex64(regs->rflags); print_char('\n');
}

// Page fault (vector 14). The CPU writes the faulting virtual address into CR2;
// the error code's low bits describe the access that failed.
static void report_page_fault(registers_t *regs) {
    uint64_t cr2;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
    uint64_t err = regs->err_code;

    print_string("THE FINGER REACHED FOR A BOX THAT ISN'T THERE\n");
    print_string("  fake address : "); print_hex64(cr2); print_char('\n');
    print_string("  it was       : ");
    print_string((err & 0x2) ? "a write\n" : "a read\n");
    print_string("  the page     : ");
    print_string((err & 0x1) ? "present but the access was forbidden\n" : "not present\n");
    print_string("  came from    : ");
    print_string((err & 0x4) ? "user\n" : "kernel\n");
    if (err & 0x8) {
        print_string("  also         : a reserved bit was set in a page table entry\n");
    }
    if (err & 0x10) {
        print_string("  also         : the access was an instruction fetch\n");
    }
}

// General protection fault (vector 13). A non-zero error code is a selector: bit
// 0 external, bits 1..2 the table it lives in, bits 3..15 the index. A zero
// error code means the fault is not tied to a specific selector.
static void report_general_protection(registers_t *regs) {
    uint64_t err = regs->err_code;

    print_string("SOMEONE TRIED SOMETHING THEY'RE NOT ALLOWED TO\n");
    print_string("  CPL was : "); print_int((uint32_t)(regs->cs & 0x3)); print_char('\n');
    if (err == 0) {
        print_string("  selector: none, the error code is zero (not tied to a segment)\n");
    } else {
        uint32_t index = (uint32_t)((err >> 3) & 0x1FFF);
        uint64_t table = (err >> 1) & 0x3;
        print_string("  selector: index "); print_int(index); print_string(" in the ");
        if (table == 0) {
            print_string("GDT");
        } else if (table == 2) {
            print_string("LDT");
        } else {
            print_string("IDT");
        }
        if (err & 0x1) {
            print_string(", flagged external");
        }
        print_char('\n');
    }
}

// Double fault (vector 8). Its error code is architecturally always zero.
static void report_double_fault(void) {
    print_string("A FAULT HAPPENED WHILE HANDLING A FAULT\n");
    print_string("  the error code is always zero here and carries no information.\n");
    print_string("  this usually means a broken IDT, a broken GDT, or a stack that\n");
    print_string("  could not be pushed to.\n");
}

// isr_handler is reached only for vectors 0..31 (the isr0..isr31 stubs); IRQs go
// to irq_handler. There is no scheduler and nothing to kill, so every exception
// is terminal: describe it, dump the desk, then halt.
void isr_handler(registers_t *regs) {
    uint8_t vec = (uint8_t)regs->int_no;
    char *name = (vec < EXC_COUNT) ? exceptions[vec].name : "unknown";
    char *what = (vec < EXC_COUNT) ? exceptions[vec].what : "not a CPU exception";

    print_char('\n');
    print_string("=== CPU EXCEPTION "); print_hex_byte(vec);
    print_string(" ("); print_string(name); print_string(") ===\n");
    print_string("  what happened: "); print_string(what); print_char('\n');

    switch (vec) {
        case EXC_PAGE_FAULT:         report_page_fault(regs);         break;
        case EXC_GENERAL_PROTECTION: report_general_protection(regs); break;
        case EXC_DOUBLE_FAULT:       report_double_fault();           break;
        default:                                                      break;
    }

    print_fault_frame(regs);
    print_string("halting. nothing to schedule, nothing to kill.\n");

    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

void irq_handler(registers_t *regs) {
    // Acknowledge the PIC(s). The slave handles IRQ8..15, which arrive at the
    // slave base (0x48) and above, so ack it first when the vector is at or
    // above PIC_SLAVE_VECTOR_BASE. This comparison MUST track the base in
    // vectors.h: hardcoding the old 40 (== 32 + 8) would silently stop acking
    // the slave now that the base is 0x40 and IRQ8 lives at 0x48.
    // The 0x20 written below is the PIC End-Of-Interrupt command byte and the
    // 0x20/0xA0 are the PIC command ports; none of these are vector numbers.
    if (regs->int_no >= PIC_SLAVE_VECTOR_BASE) {
        port_byte_out(0xA0, 0x20);
    }
    port_byte_out(0x20, 0x20);

    if (interrupt_handlers[regs->int_no]) {
        interrupt_handlers[regs->int_no](regs);
    }

    // AFTER the per-vector handler, not before, and this is the whole reason signals
    // work during a compute loop. For the timer that handler is schedule(), which
    // rewrites `regs` in place with the incoming task's context; running this first
    // would examine the outgoing task against the incoming task's frame. Afterwards,
    // `regs` and the current task agree, and this is the last thing that touches the
    // frame before the stub pops it and returns to ring 3.
    //
    // check_signals itself decides whether this frame is going to ring 3 at all —
    // nested interrupts return into kernel code, and delivering there corrupts it
    // (S1). It is not this function's job to know.
    check_signals(regs);
}
