# Bare-Metal Custom JIT-VM

A minimalist, high-performance Just-In-Time (JIT) compiler and register-based Virtual Machine built entirely from scratch in pure C. This project compiles a custom intermediate bytecode into native x86-64 machine instructions at runtime, executing them directly on the physical host CPU.

The toolchain features a decoupled architecture consisting of an offline assembler (`asm`) and a dynamic runtime executor (`vm`).

---

## Architecture Overview

The execution pipeline follows a standard compiler toolchain flow:

[ text.txt Source ]
        │
        ▼ 
(Assembler / asm)
[ binary.cvm File ] ──► Read dynamically into RAM by the VM
        │
        ▼ 
(JIT Compiler / vm)
[ Native x86-64 Code ] ──► Memory marked executable via mprotect() ──► Executed on CPU


1. **The Assembler (`asm`):** Parses human-readable text instructions, tokenizes the arguments, and packetizes them into a custom 32-bit binary format (`.cvm`).
2. **The VM/JIT Engine (`vm`):** Dynamically parses the `.cvm` header, allocates execution memory using `mmap()`, translates the virtual bytecode into native x86-64 machine instructions, flips page permissions using `mprotect()`, and jumps the host CPU's instruction pointer directly into the buffer.

---

## Custom ISA & File Format Details

The VM implements a 64-bit virtual register architecture utilizing three virtual registers (`V0`, `V1`, `V2`). 

### The `.cvm` Binary Spec

Every compiled binary contains a strict header layout followed by a 4-byte instruction stream payload:
* **Bytes 0–3:** Magic Bytes (`CVM0`)
* **Bytes 4–11:** 64-bit little-endian integer defining the total `Instruction Count`
* **Bytes 12+:** The raw stream of 32-bit instructions packetized as:
  * `Byte 0:` Opcode (`0x01` = MOV_IMM, `0x02` = ADD, `0x03` = RETURN)
  * `Byte 1:` Destination Register ID
  * `Byte 2–3:` Source Register ID or 16-bit Immediate Value

---

## Getting Started

### Prerequisites

* A Linux environment (like Arch Linux) running an **x86-64** processor.
* `gcc` toolchain and GNU `make` (optional).

### 1. Compilation

Compile both the assembler and the VM runtime using `gcc`:

```bash
# Compile the assembler utility
gcc assembler.c -o asm

# Compile the JIT virtual machine engine
gcc vm.c -o vm

# Compute (10 + 32) and return the result
MOV V0, 10
MOV V1, 32
ADD V0, V1
RETURN V0

# 1. Assemble text into custom bytecode
./asm program.txt binary.cvm

# 2. Inspect the raw bytes (Optional)
hexdump binary.cvm

# 3. Feed the payload directly into the JIT engine
./vm binary.cvm
