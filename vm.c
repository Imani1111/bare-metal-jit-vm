#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>

#define REX_W        0x48  // 64-bit operation prefix
// Base register encoding for RDI (+disp8 mode) --> because the VMState_t struct has 64bit array
//  which are the virtual registers.

// mods for modrm byte 
#define MEM_MOD_8BYTE_DISP 0x01
#define MOD_NO_DISPLACEMENT 0x00
#define MOD_REG_TO_REG 0x03
#define TRIGGER_SIB_BYTE 0b100 // I can't think of the hex rep of this rn so.... yeah binary

#define MOV_VAL_FROM_RM_TO_REG 0x8B  // mov dest, src
#define MOV_VAL_FROM_REG_TO_RM 0x89
#define MOV_ZERO_EXTEND 0xB6
#define MOV_CONSTANT_DIRECTLY_TO_MEM_ADDR 0xC7
#define MOV_64BIT_CONSTANT_FROM_RM_TO_REG 0xB8
#define ADD_REG_TO_RM 0x01
#define SUB_REG_FROM_RM 0x29
#define XOR_REG_TO_REG 0x31
#define UNCONDITIONAL_JMP 0xE9
#define PREFIX_FOR_EXTENDED_OPCODES 0x0F
#define CONDITIONAL_JNE_BYTE2 0x85
#define CONDITIONAL_JE_BYTE2 0x84
#define CMP_REG_TO_REG 0x3B
#define CMP_REG_TO_IMM 0x81
#define PUSH_RDI 0x57
#define POP_RDI 0x5F
#define PUSH_RBX 0X53
#define POP_RBX 0x5B
//#define SYSCALL 
#define PUSH_R14_BYTE1 0x41
#define PUSH_R14_BYTE2 0x56
#define LOAD_EFFECTIVE_ADDR 0x8D
#define OPCODE_RET  0xC3
// opcodes
#define OP_MOV        0x01
#define OP_ADD        0x02
#define OP_RETURN     0x03
#define OP_SUB        0x04
#define OP_XOR        0x05
#define OP_JMP        0x06
#define OP_JNE        0x07
#define OP_JE         0x08
#define OP_JZ         0x09
#define OP_JG         0xA
#define OP_JNLE       0xB
#define OP_JL         0xC
#define OP_NGE        0xD
#define OP_JGE        0xE
#define OP_JNL        0xF
#define OP_JLE        0x10
#define OP_JNG        0x11
#define OP_CMP        0x12
#define OP_INDEX_PTRB 0x13
#define OP_INDEX_PTRW 0x14
#define OP_INDEX_PTRD 0x15
#define OP_INDEX_PTRQ 0x16
#define OP_LOAD       0x17
#define OP_STORE      0x18
#define OP_LET        0x19
#define OP_CALL       0x1A
// virtual registers
#define V0  0 
#define V1  1
#define V2  2
#define V3  3
#define V4  4
#define V5  5
#define V6  6
#define V7  7
#define V8  8

#define MAX_INSTRUCTIONS 1024
#define MAX_PATCH_ADDRESSES 100
#define PAGE_SIZE 4096
#define REMOVE_BIT7 0b01111111 // Binary representation of 0x7F
#define BIT7_CHECKER 0b10000000 // Binary representation of 0x80
                                
#define MAX_EXTERNAL_ENTRIES 64

typedef enum {
    REG_RAX = 0, REG_RCX = 1, REG_RDX = 2, REG_RBX = 3,
    REG_RSP = 4, REG_RBP = 5, REG_RSI = 6, REG_RDI = 7,
    REG_R8  = 8, REG_R9  = 9, REG_R10=10, REG_R11=11,
    REG_R12=12, REG_R13=13, REG_R14=14, REG_R15=15
} X86Reg;

typedef struct {
    uint64_t registers[32];
}VMState_t;

typedef struct {
    uint8_t magic_signature[4];
    uint64_t instruction_count;
    uint8_t* string_pool_base;
}VMBinaryHeader_t;

typedef struct {
    uint8_t* address_storing_my_placeholder_instr;
    uint32_t target_instruction;
    uint8_t instruction_size;
}ForwardJumpPatch_t;

typedef struct {
    char function_name[64];
    void* function_address;
}GOT_Entry_t;

typedef struct {
    GOT_Entry_t function_entries[MAX_EXTERNAL_ENTRIES];
    size_t entry_count;
}GlobalOffsetTable_t;

typedef struct {
    char name[64];
    uint8_t* got_entry_pointer;
}ProcedureLinkageTable_t;

ForwardJumpPatch_t patch_list[MAX_PATCH_ADDRESSES];
size_t patch_count = 0;

GlobalOffsetTable_t global_offset_table;
ProcedureLinkageTable_t procedure_linkage_table[MAX_EXTERNAL_ENTRIES];

static inline void emit_u8(uint8_t** buffer, uint8_t byte)
{
    **buffer = byte;
    (*buffer)++;
}

static inline void emit_i8(uint8_t** buffer, int8_t byte)
{
    **buffer = byte;
    (*buffer)++;
}

static inline void emit_dynamic_modrm(uint8_t** buffer, uint8_t mod, X86Reg source_reg, X86Reg destination_reg)
{   // its just a bit mask using 7, ive written the binary rep for quicker understanding during review
    uint8_t modrm = (mod << 6) | ((source_reg & 0b00000111) << 3) | (destination_reg & 0b00000111);
    emit_u8(buffer, modrm);
}
static inline void emit_dynamic_sib_byte(uint8_t** code_pointer, X86Reg index_reg, X86Reg base_reg, uint8_t scale)
{
    uint8_t byte_size;
    switch (scale){
        case 1: byte_size = 0b00; break;
        case 2: byte_size = 0b01; break;
        case 4: byte_size = 0b10; break;
        case 8: byte_size = 0b11; break;
        default:
                byte_size = 0b00;
                fprintf(stderr, "Invalid byte size for array, defaulted to a byte addressing\n");
                break;
    }
    uint8_t sib_byte = (base_reg & 0b111) | ((index_reg & 0b111) << 3) | ((byte_size & 0b11) << 6);
    emit_u8(code_pointer, sib_byte);
}
static inline void emit_u32(uint8_t** buffer, uint32_t value)
{
    emit_u8(buffer, (value & 0xFF));
    emit_u8(buffer, (value >> 8) & 0xFF);
    emit_u8(buffer, (value >> 16) & 0xFF);
    emit_u8(buffer, (value >> 24) & 0xFF);
}

static inline void emit_i32(uint8_t** buffer, int32_t value)
{
   emit_i8(buffer, (value & 0xFF));
   emit_i8(buffer, (value >> 8) & 0xFF);
   emit_i8(buffer, (value >> 16) & 0xFF);
   emit_i8(buffer, (value >> 24) & 0xFF);
}

static inline void emit_u64(uint8_t** code_pointer, uint64_t immediate)
{
    emit_u32(code_pointer, (uint32_t)(immediate & 0xFFFFFFFF));
    emit_u32(code_pointer, (uint32_t)(immediate >> 32));
}
// mov [rbx + offset], imm
static inline void emit_x86_mov_imm(uint8_t** buffer, uint8_t destination_register, uint64_t immediate)
{
    uint8_t cleaned_dest = destination_register & REMOVE_BIT7;
    emit_u8(buffer, REX_W);
    emit_u8(buffer, MOV_64BIT_CONSTANT_FROM_RM_TO_REG);
    emit_u64(buffer, immediate);
    emit_u8(buffer, REX_W); emit_u8(buffer, MOV_VAL_FROM_REG_TO_RM);
    emit_dynamic_modrm(buffer, MEM_MOD_8BYTE_DISP, 0, REG_RBX); 
    emit_u8(buffer, cleaned_dest * 8);
}
// mov [rbx + offset], [rbx + offset]
static inline void emit_x86_mov_r64_r64(uint8_t** code_pointer, uint8_t destination_register, uint8_t source_register, 
                                        X86Reg scratch_register)
{   // 
    uint8_t cleaned_dest = destination_register & REMOVE_BIT7;
    uint8_t cleaned_src = source_register & REMOVE_BIT7;
    emit_u8(code_pointer, REX_W); emit_u8(code_pointer, MOV_VAL_FROM_RM_TO_REG); emit_dynamic_modrm(code_pointer, 
                                                                                    MEM_MOD_8BYTE_DISP, scratch_register, REG_RBX);
    emit_u8(code_pointer, cleaned_src * 8); 
    emit_u8(code_pointer, REX_W); emit_u8(code_pointer, MOV_VAL_FROM_REG_TO_RM); emit_dynamic_modrm(code_pointer, 
                                                                            MEM_MOD_8BYTE_DISP, scratch_register, REG_RBX);
    emit_u8(code_pointer, cleaned_dest * 8);
}
// add [rdi + offset], [rdi + offset] through rax
static inline void emit_x86_add(uint8_t** buffer, uint8_t destination_register, uint8_t source_register, X86Reg scratch_reg)
{
    emit_u8(buffer, REX_W); emit_u8(buffer, MOV_VAL_FROM_RM_TO_REG); emit_dynamic_modrm(buffer, MEM_MOD_8BYTE_DISP, scratch_reg, REG_RBX); 
    emit_u8(buffer, source_register * 8);
    emit_u8(buffer, REX_W); emit_u8(buffer, ADD_REG_TO_RM); emit_dynamic_modrm(buffer, MEM_MOD_8BYTE_DISP, scratch_reg, REG_RBX);
    emit_u8(buffer, destination_register * 8); 
}
// sub [rdi + offset], [rdi + offset] through rax
static inline void emit_x86_sub(uint8_t** buffer, uint8_t destination_register, uint8_t source_register, X86Reg scratch_reg)
{
    emit_u8(buffer, REX_W); emit_u8(buffer, MOV_VAL_FROM_RM_TO_REG); emit_dynamic_modrm(buffer, 
                                                                        MEM_MOD_8BYTE_DISP, scratch_reg, REG_RBX);
    emit_u8(buffer, source_register * 8);
    emit_u8(buffer, REX_W); emit_u8(buffer, SUB_REG_FROM_RM); emit_dynamic_modrm(buffer, MEM_MOD_8BYTE_DISP, scratch_reg, REG_RBX);
    emit_u8(buffer, destination_register * 8);
}
// xor [rdi + offset], [rdi + offset] through rax
static inline void emit_x86_xor(uint8_t** buffer, uint8_t destination_register, uint8_t source_register, X86Reg scratch_reg)
{
    emit_u8(buffer, REX_W); emit_u8(buffer, MOV_VAL_FROM_RM_TO_REG); emit_dynamic_modrm(buffer, MEM_MOD_8BYTE_DISP, scratch_reg, REG_RBX); 
    emit_u8(buffer, source_register * 8);
    emit_u8(buffer, REX_W); emit_u8(buffer, XOR_REG_TO_REG); emit_dynamic_modrm(buffer, MEM_MOD_8BYTE_DISP, scratch_reg, REG_RBX);
    emit_u8(buffer, destination_register * 8);
}
// mov rax, [rbx + offset](base memory address of the array) then mov rcx, [rbx + offset](this is the index number)
// then lea rax, [rax + rcx*scale] then mov [rbx + offset], rax 
static inline void emit_x86_array_indexing(uint8_t** code_pointer, X86Reg x86_base_register, X86Reg x86_index_register, 
                                           uint8_t v_base_reg, uint8_t v_index_reg, uint8_t scale)
{
    emit_u8(code_pointer, REX_W); emit_u8(code_pointer, MOV_VAL_FROM_RM_TO_REG); emit_dynamic_modrm(code_pointer,
                                                                                 MEM_MOD_8BYTE_DISP, REG_RAX, REG_RBX);
    emit_u8(code_pointer, v_base_reg * 8);
    emit_u8(code_pointer, REX_W); emit_u8(code_pointer, MOV_VAL_FROM_RM_TO_REG); emit_dynamic_modrm(code_pointer, 
                                                                                MEM_MOD_8BYTE_DISP, REG_RCX, REG_RBX);
    emit_u8(code_pointer, v_index_reg * 8);
    X86Reg trigger_sib_byte = REG_RSP; // Because REG_RSP is 4 or 0b100 which tells the modrm byte to look for the sib byte
    emit_u8(code_pointer, REX_W); emit_u8(code_pointer, LOAD_EFFECTIVE_ADDR);
    // only use MEM_MOD_8BYTE_DISP (0x01) when passing REG_RAX as base memory address otherwise use MOD_NO_DISPLACEMENT (0x00)
    // and remember that when you use 0x00 the cpu truncates rax and rcx to eax and ecx so this means it uses 32-bit addressing math
    emit_dynamic_modrm(code_pointer, MEM_MOD_8BYTE_DISP, REG_RAX, trigger_sib_byte);
    emit_dynamic_sib_byte(code_pointer, x86_index_register, x86_base_register, scale);
    emit_i8(code_pointer, 0x00); // 0x01 requires a displacement byte to be added so im hardcoding 0x00 for lea rax, [rax, rcx*scale] + 0
    emit_u8(code_pointer, REX_W); emit_u8(code_pointer, MOV_VAL_FROM_REG_TO_RM); emit_dynamic_modrm(code_pointer, MEM_MOD_8BYTE_DISP,
                                                                                 REG_RAX, REG_RBX);
    emit_u8(code_pointer, v_base_reg * 8);
}
// mov rax, [rdi + offset] then mov rax, [rax] then mov [rdi + offset], rax
static inline void emit_x86_load_mem_r64(uint8_t** code_pointer, uint8_t destination_register, uint8_t mem_address,
                                         X86Reg scratch_reg)
{
    emit_u8(code_pointer, REX_W); emit_u8(code_pointer, MOV_VAL_FROM_RM_TO_REG); emit_dynamic_modrm(code_pointer, 
                                                                                MEM_MOD_8BYTE_DISP, scratch_reg, REG_RBX);
    emit_u8(code_pointer, mem_address * 8);
    uint8_t dereference_rax_into_rax = 0x00;
    emit_u8(code_pointer, REX_W); emit_u8(code_pointer, PREFIX_FOR_EXTENDED_OPCODES); emit_u8(code_pointer, MOV_ZERO_EXTEND);
    emit_u8(code_pointer, dereference_rax_into_rax);
    emit_u8(code_pointer, REX_W); emit_u8(code_pointer, MOV_VAL_FROM_REG_TO_RM); emit_dynamic_modrm(code_pointer, 
                                                                                MEM_MOD_8BYTE_DISP, scratch_reg, REG_RBX);
    emit_u8(code_pointer, destination_register * 8);
}

static inline void emit_x86_store(uint8_t** code_pointer, uint8_t mem_ptr, uint8_t src_reg, X86Reg scratch_reg)
{
    emit_u8(code_pointer, REX_W); emit_u8(code_pointer, MOV_VAL_FROM_RM_TO_REG); emit_dynamic_modrm(code_pointer, MEM_MOD_8BYTE_DISP, scratch_reg, REG_RBX);    
    emit_u8(code_pointer, mem_ptr * 8);   
    emit_u8(code_pointer, REX_W); emit_u8(code_pointer, MOV_VAL_FROM_RM_TO_REG); emit_dynamic_modrm(code_pointer, MEM_MOD_8BYTE_DISP, REG_RCX, REG_RBX);
    emit_u8(code_pointer, src_reg * 8);
    emit_u8(code_pointer, REX_W); emit_u8(code_pointer, MOV_VAL_FROM_REG_TO_RM); emit_dynamic_modrm(code_pointer, MOD_NO_DISPLACEMENT, REG_RCX, REG_RAX);
}
// jmp target_instruction
static inline void emit_x86_jmp(uint8_t** buffer, uint32_t target_instruction, uint32_t current_instruction, 
                                const uint32_t* jit_label_map, uint8_t* buffer_base)
{
   if (target_instruction < current_instruction){
        uint32_t target_x86_offset = jit_label_map[target_instruction];
        uint32_t current_x86_offset = (uint32_t)(*buffer - buffer_base);

        int32_t relative_offset = (int32_t)target_x86_offset - (int32_t)(current_x86_offset + 5);
        printf("JMP Debug: Target Index = %u, Target x86 Offset = %u, Current x86 Offset = %u, Rel32 = %d\n",
       target_instruction, target_x86_offset, current_x86_offset, relative_offset);
        emit_u8(buffer, UNCONDITIONAL_JMP);
        emit_i32(buffer, relative_offset); 
   }else{
        emit_u8(buffer, UNCONDITIONAL_JMP);
        patch_list[patch_count].address_storing_my_placeholder_instr = *buffer;
        patch_list[patch_count].target_instruction = target_instruction;
        patch_list[patch_count].instruction_size = 4;
        patch_count++; 
        emit_i32(buffer, 0x00000000);
   }
}
// jne target_instruction
static inline void emit_x86_jne(uint8_t** code_pointer, uint32_t target_instruction, uint32_t current_instruction, 
                                const uint32_t* jit_label_map, uint8_t* buffer_base)
{
    if (target_instruction < current_instruction){
        uint32_t target_x86_offset = jit_label_map[target_instruction];
        uint32_t current_x86_offset = (uint32_t)(*code_pointer - buffer_base);
        int32_t relative_offset = (int32_t)target_x86_offset - (int32_t)(current_x86_offset + 6);
        emit_u8(code_pointer, PREFIX_FOR_EXTENDED_OPCODES);
        emit_u8(code_pointer, CONDITIONAL_JNE_BYTE2);
        emit_i32(code_pointer, relative_offset);
    }else{
        emit_u8(code_pointer, PREFIX_FOR_EXTENDED_OPCODES);
        emit_u8(code_pointer, CONDITIONAL_JNE_BYTE2);
        patch_list[patch_count].address_storing_my_placeholder_instr = *code_pointer;
        patch_list[patch_count].target_instruction = target_instruction;
        patch_list[patch_count].instruction_size = 4;
        patch_count++;
        emit_i32(code_pointer, 0x00000000);
    }
}
// je target_instruction
static inline void emit_x86_je_or_jz(uint8_t** code_pointer, uint32_t target_instruction, uint32_t current_instruction, const
                         uint32_t* jit_label_map, uint8_t* buffer_base)
{
    if (target_instruction < current_instruction){
        uint32_t target_x86_offset = jit_label_map[target_instruction];
        uint32_t current_x86_offset = (uint32_t)(*code_pointer - buffer_base);
        int32_t relative_offset = (int32_t)target_x86_offset - (int32_t)current_x86_offset;
        emit_u8(code_pointer, PREFIX_FOR_EXTENDED_OPCODES);
        emit_u8(code_pointer, CONDITIONAL_JE_BYTE2);
        emit_i32(code_pointer, relative_offset);
    }else{
        emit_u8(code_pointer, PREFIX_FOR_EXTENDED_OPCODES);
        emit_u8(code_pointer, CONDITIONAL_JE_BYTE2);
        patch_list[patch_count].address_storing_my_placeholder_instr = *code_pointer;
        patch_list[patch_count].target_instruction = target_instruction;
        patch_list[patch_count].instruction_size = 4;
        patch_count++;
        emit_i32(code_pointer, 0x00000000);
    }
}
// TODO: FIRST THING I WAKE UP, IMPLEMENT CMP INSTRUCTIONS
static inline void emit_x86_cmp_r64_r64_or_r64_imm(uint8_t** code_pointer, uint8_t destination_register, uint8_t source_register, 
                                        X86Reg scratch_reg, uint32_t immediate, uint8_t mode)
{
    uint8_t cleaned_dest = destination_register & REMOVE_BIT7;
    if (!mode){
        emit_u8(code_pointer, REX_W); emit_u8(code_pointer, MOV_VAL_FROM_RM_TO_REG); emit_dynamic_modrm(code_pointer, 
                                                                                    MEM_MOD_8BYTE_DISP, scratch_reg, REG_RBX);
        emit_u8(code_pointer, (source_register * 8));
        emit_u8(code_pointer, REX_W); emit_u8(code_pointer, CMP_REG_TO_REG); emit_dynamic_modrm(code_pointer, 
                                                                             MEM_MOD_8BYTE_DISP, scratch_reg, REG_RBX);
        emit_u8(code_pointer, (cleaned_dest * 8));
    }else{
        emit_u8(code_pointer, REX_W); emit_u8(code_pointer, CMP_REG_TO_IMM); emit_dynamic_modrm(code_pointer, MEM_MOD_8BYTE_DISP,
                                                                                                scratch_reg, REG_RBX);
        emit_u8(code_pointer, cleaned_dest * 8);
        emit_u32(code_pointer, immediate);
    }
}

static inline void emit_x86_call(uint8_t** code_pointer, uint8_t arg_register, void* plt_stub_address, X86Reg scratch_reg)
{
    // 1. Load the argument from VM register [rbx + arg_register * 8] into RDI (1st argument in System V ABI)
    emit_u8(code_pointer, REX_W); 
    emit_u8(code_pointer, MOV_VAL_FROM_RM_TO_REG); 
    emit_dynamic_modrm(code_pointer, MEM_MOD_8BYTE_DISP, scratch_reg, REG_RBX);
    emit_u8(code_pointer, arg_register * 8);

    // Move scratch_reg (e.g., RAX) into RDI
    emit_u8(code_pointer, REX_W); 
    emit_u8(code_pointer, MOV_VAL_FROM_REG_TO_RM); 
    emit_dynamic_modrm(code_pointer, MOD_REG_TO_REG, scratch_reg, REG_RDI);

    // 2. Load the absolute address of the PLT stub into RAX
    emit_u8(code_pointer, REX_W); 
    emit_u8(code_pointer, MOV_64BIT_CONSTANT_FROM_RM_TO_REG);
    emit_u64(code_pointer, (uint64_t)plt_stub_address);

    // 3. Call the PLT stub using 'call rax' (Opcodes: FF D0)
    emit_u8(code_pointer, 0xFF);
    emit_u8(code_pointer, 0xD0);
}

// ret [rbx + offset] places the value of [rbx + offset] in rax and hands over execution
static inline void emit_x86_mov_vreg_to_rax(uint8_t** code_pointer, uint8_t destination_register)
{ 
    emit_u8(code_pointer, REX_W); emit_u8(code_pointer, MOV_VAL_FROM_RM_TO_REG); emit_dynamic_modrm(code_pointer, MEM_MOD_8BYTE_DISP, 
                                                                                        REG_RAX, REG_RBX);
    emit_u8(code_pointer, destination_register * 8);
}

static inline void resolve_patch_addresses(uint32_t* tracker, uint8_t* executable_code_buffer)
{
    for (size_t p = 0; p < patch_count; p++){
        uint8_t* placeholder_address = patch_list[p].address_storing_my_placeholder_instr;
        uint32_t target_instruction = patch_list[p].target_instruction;
        uint8_t instruction_size = patch_list[p].instruction_size;

        uint32_t target_x86_offset = tracker[target_instruction];
        uint8_t* instruction_end_ptr = placeholder_address + instruction_size;
        uint32_t placeholder_x86_offset = (uint32_t)(instruction_end_ptr - executable_code_buffer);
        int32_t relative_address = (int32_t)target_x86_offset - (int32_t)placeholder_x86_offset;

        *(int32_t*)placeholder_address = relative_address;
    }
    patch_count = 0;
}
// btw this was only for debugging individual instructions opcodes if you wonder why its never used!
static inline void print_instruction_disassembly(const uint32_t* instruction_map, uint8_t* code_base, size_t instruction_index,
                                                 uint32_t total_code_bytes, size_t total_instructions)
{
    uint32_t start_offset = instruction_map[instruction_index];
    uint32_t end_offset = (instruction_index == total_instructions - 1) ? total_code_bytes : instruction_map[instruction_index + 1];
    uint8_t* ptr = code_base + start_offset;
    uint8_t* end = code_base + end_offset;
    printf("Instruction %zu Opcodes (Size: %td bytes): ", instruction_index, end - ptr);
    while (ptr < end){
        printf("%02X ", *ptr);
        ptr++;
    }
    printf("\n");
}

void initialize_got()
{
    global_offset_table.entry_count = 0;
    // 1. --> puts()
    strcpy(global_offset_table.function_entries[global_offset_table.entry_count].function_name, "puts");
    global_offset_table.function_entries[global_offset_table.entry_count].function_address = dlsym(RTLD_DEFAULT, "puts");
    global_offset_table.entry_count++;
    // 2. --> printf()
    strcpy(global_offset_table.function_entries[global_offset_table.entry_count].function_name, "printf");
    global_offset_table.function_entries[global_offset_table.entry_count].function_address = dlsym(RTLD_DEFAULT, "printf");
    global_offset_table.entry_count++;
}

int get_got_index(const char* name)
{
    for (int i = 0; i < global_offset_table.entry_count; i++){
        if (strcmp(global_offset_table.function_entries[i].function_name, name) == 0){
            return i;
        }
    }
    void* resolved = dlsym(RTLD_DEFAULT, name);
    if (resolved){
        int index = global_offset_table.entry_count++;
        strcpy(global_offset_table.function_entries[index].function_name, name);
        global_offset_table.function_entries[index].function_address = resolved;
        return index;
    }
    return -1;
}

void init_plt_from_got(ProcedureLinkageTable_t* plt)
{
    for (size_t i = 0; i < global_offset_table.entry_count; i++){
        uint8_t* plt_stub = mmap(NULL , 16, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        uint8_t* plt_pointer = plt_stub;
 
        emit_u8(&plt_pointer,  REX_W); emit_u8(&plt_pointer, MOV_64BIT_CONSTANT_FROM_RM_TO_REG);

        strncpy(plt[i].name, global_offset_table.function_entries[i].function_name, sizeof(plt[i].name) - 1);

        uint64_t function_address = (uint64_t)global_offset_table.function_entries[i].function_address;   
        emit_u64(&plt_pointer, function_address);
        
        uint8_t jmp = 0xFF;
        uint8_t rax = 0xE0;
        emit_u8(&plt_pointer, jmp);
        emit_u8(&plt_pointer, rax);
        plt[i].got_entry_pointer = plt_stub;
    }
}

static inline uint8_t* jit_create(size_t required_size)
{
    size_t alloc_size = ((required_size + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
    uint8_t* executable_code_buffer = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | 
                                           MAP_PRIVATE, -1, 0);
    if (executable_code_buffer == MAP_FAILED){
        perror("mmap failed");
        exit(1);
    }
    return executable_code_buffer;
}

void run_jit_compiler(uint32_t* code_to_be_executed, size_t instruction_count, VMState_t* vm_state, uint8_t* string_pool_base)
{
    uint32_t* jit_custom_label_tracker = malloc(instruction_count * sizeof(uint32_t));

    size_t estimated_size = instruction_count * 64;
    if (estimated_size < PAGE_SIZE) estimated_size = PAGE_SIZE;
    uint8_t* executable_code_buffer = jit_create(estimated_size);
    uint8_t* code_pointer = executable_code_buffer;
    // [PROLOGUE] Save C's RBX, then copy vm_state (RDI) into RBX 
    emit_u8(&code_pointer, PUSH_RBX);
    emit_u8(&code_pointer, REX_W); emit_u8(&code_pointer, MOV_VAL_FROM_REG_TO_RM); emit_dynamic_modrm(&code_pointer, MOD_REG_TO_REG, REG_RDI, REG_RBX);

    for (size_t i = 0; i < instruction_count; i++){
        jit_custom_label_tracker[i] = (uint32_t)(code_pointer - executable_code_buffer);

        uint32_t instruction = code_to_be_executed[i];
        uint8_t opcode = instruction & 0xFF;
        uint8_t destination_register = (instruction >> 8) & 0xFF;
        uint8_t source_register = (instruction >> 16) & 0xFF;
        uint16_t immediate = (instruction >> 16) & 0xFFFF;

        switch (opcode){ 
            case OP_MOV: {
                             printf("DEBUG MOV: dest_raw=0x%02X, has_bit7=%d\n", 
                                    destination_register, (destination_register & BIT7_CHECKER) != 0);
                             uint8_t mode = destination_register & BIT7_CHECKER;
                             if (mode){
                                emit_x86_mov_imm(&code_pointer, destination_register, immediate);
                             }else{
                                 uint8_t cleaned_src = source_register & BIT7_CHECKER;
                                 emit_x86_mov_r64_r64(&code_pointer, destination_register, cleaned_src, REG_RAX);
                             }
                             break;
                         }
            case OP_ADD: {
                            emit_x86_add(&code_pointer, destination_register, source_register, REG_RAX);
                            break;
                         }
            case OP_SUB: {
                             emit_x86_sub(&code_pointer, destination_register, source_register, REG_RAX);
                            break;
                         }
            case OP_XOR: {
                             emit_x86_xor(&code_pointer, destination_register, source_register, REG_RAX);
                             break;
                         }
            case OP_JMP: {
                             uint32_t target_instruction = (instruction >> 8) & 0xFFFFFF;
                             emit_x86_jmp(&code_pointer, target_instruction, i, jit_custom_label_tracker, executable_code_buffer);
                             break;
                         }
            case OP_JNE: {
                             uint32_t target_instruction = ((instruction >> 8) & 0xFFFFFF);
                             emit_x86_jne(&code_pointer, target_instruction, i, jit_custom_label_tracker, executable_code_buffer);
                             break;   
                         }
            case OP_JE: {
                            uint32_t target_instruction = ((instruction >> 8) & 0xFFFFFF);
                            emit_x86_je_or_jz(&code_pointer, target_instruction, i, jit_custom_label_tracker, executable_code_buffer);
                            break;
                        }
            case OP_JZ: {
                            uint32_t target_instruction = ((instruction >> 8) & 0xFFFFFF);
                            emit_x86_je_or_jz(&code_pointer, target_instruction, i, jit_custom_label_tracker, executable_code_buffer);
                            break;
                        }
            case OP_CMP: {
                             uint8_t mode = destination_register & BIT7_CHECKER;
                             emit_x86_cmp_r64_r64_or_r64_imm(&code_pointer, destination_register, source_register, REG_RAX, 
                                                             immediate, mode);
                             break;
                         }
            case OP_INDEX_PTRB: {
                                    uint8_t scale = 1;
                                    emit_x86_array_indexing(&code_pointer, REG_RAX, REG_RCX, destination_register,
                                        source_register, scale);
                                    break;
                              }
            case OP_INDEX_PTRW: {
                                    uint8_t scale = 2;
                                    emit_x86_array_indexing(&code_pointer, REG_RAX, REG_RCX, destination_register, 
                                        source_register, scale);
                                    break;
                                }
            case OP_INDEX_PTRD: {
                                    uint8_t scale = 4;
                                    emit_x86_array_indexing(&code_pointer, REG_RAX, REG_RCX, destination_register, 
                                        source_register, scale);
                                    break;
                                }
            case OP_INDEX_PTRQ: {
                                    uint8_t scale = 8;
                                    emit_x86_array_indexing(&code_pointer, REG_RAX, REG_RCX, destination_register, 
                                        source_register, scale);
                                    break;
                                }
            case OP_LOAD: {
                              emit_x86_load_mem_r64(&code_pointer, destination_register, source_register, REG_RAX);
                              break;
                          }
            case OP_STORE: {
                               emit_x86_store(&code_pointer, destination_register, source_register, REG_RAX);
                               break;
                           }
            case OP_LET: {
                             uint16_t string_index = immediate;
                            uint64_t string_address = (uint64_t)(uintptr_t)string_pool_base + string_index;
                            emit_x86_mov_imm(&code_pointer, destination_register, string_address);
                            break;
                         }
            case OP_CALL: {
                              int function_index = destination_register; 
                              void* target_plt_stub = procedure_linkage_table[function_index].got_entry_pointer;
                              // 2. Implicitly load v0 -> RDI
                             emit_u8(&code_pointer, REX_W); 
                             emit_u8(&code_pointer, MOV_VAL_FROM_RM_TO_REG); 
                             emit_dynamic_modrm(&code_pointer, MEM_MOD_8BYTE_DISP, REG_RAX, REG_RBX);
                             emit_u8(&code_pointer, V0 * 8); // Offset for v0

                             emit_u8(&code_pointer, REX_W); 
                             emit_u8(&code_pointer, MOV_VAL_FROM_REG_TO_RM); 
                             emit_dynamic_modrm(&code_pointer, MOD_REG_TO_REG, REG_RAX, REG_RDI);

                             // 3. Implicitly load v1 -> RSI
                             emit_u8(&code_pointer, REX_W); 
                             emit_u8(&code_pointer, MOV_VAL_FROM_RM_TO_REG); 
                             emit_dynamic_modrm(&code_pointer, MEM_MOD_8BYTE_DISP, REG_RAX, REG_RBX);
                             emit_u8(&code_pointer, V1 * 8); // Offset for v1

                             emit_u8(&code_pointer, REX_W); 
                             emit_u8(&code_pointer, MOV_VAL_FROM_REG_TO_RM); 
                             emit_dynamic_modrm(&code_pointer, MOD_REG_TO_REG, REG_RAX, REG_RSI);

                             // 4. Implicitly load v2 -> RDX
                             emit_u8(&code_pointer, REX_W); 
                             emit_u8(&code_pointer, MOV_VAL_FROM_RM_TO_REG); 
                             emit_dynamic_modrm(&code_pointer, MEM_MOD_8BYTE_DISP, REG_RAX, REG_RBX);
                             emit_u8(&code_pointer, V2 * 8); // Offset for v2

                             emit_u8(&code_pointer, REX_W); 
                             emit_u8(&code_pointer, MOV_VAL_FROM_REG_TO_RM); 
                             emit_dynamic_modrm(&code_pointer, MOD_REG_TO_REG, REG_RAX, REG_RDX);

                             // 5. Load the PLT stub address into RAX and perform the call
                             emit_u8(&code_pointer, REX_W); 
                             emit_u8(&code_pointer, MOV_64BIT_CONSTANT_FROM_RM_TO_REG);
                             emit_u64(&code_pointer, (uint64_t)target_plt_stub);

                             emit_u8(&code_pointer, 0xFF);
                             emit_u8(&code_pointer, 0xD0); // call rax
                             break;
                          }
            case OP_RETURN: {
                                emit_x86_mov_vreg_to_rax(&code_pointer, destination_register);
                                emit_u8(&code_pointer, POP_RBX); // pop rbx
                                emit_u8(&code_pointer, OPCODE_RET);
                                break;
                            }
        }
    }
    size_t alloc_size = estimated_size;

    resolve_patch_addresses(jit_custom_label_tracker, executable_code_buffer);
    uint32_t total_bytes = (uint32_t)(code_pointer - executable_code_buffer);
    for (size_t i = 0; i < instruction_count; i++){
        print_instruction_disassembly(jit_custom_label_tracker, executable_code_buffer, i, total_bytes, instruction_count);
    }
    mprotect(executable_code_buffer, alloc_size, PROT_READ | PROT_EXEC);

    typedef uint64_t(*JITFunc)(VMState_t*);
    JITFunc compiled_code = (JITFunc)executable_code_buffer;
    uint64_t result = compiled_code(vm_state);

    printf("JIT Execution Finished.\n");
    printf("Returned Value: %llu\n", result);
    printf("VM Register State: V0=%llu, V1=%llu, V2=%llu\n",
                vm_state->registers[V0], vm_state->registers[V1], vm_state->registers[V2]);

    munmap(executable_code_buffer, alloc_size);
}

void load_and_run_file(const char* cvm_file, VMState_t* vm_state)
{
    FILE* infile = fopen(cvm_file, "rb");
    if (!infile){
        perror("Error: Failed to open .cvm file\n");
        return;
    }
    VMBinaryHeader_t binary_header;
    if (fread(binary_header.magic_signature, 1, 4, infile) != 4 || binary_header.magic_signature[0] != 'C' || 
            binary_header.magic_signature[1] != 'V' || binary_header.magic_signature[2] != 'M' || binary_header.magic_signature[3] != '0'){
        fprintf(stderr, "Invalid File binary format (Magic Number Mismatch)\n");
        printf("HINT: Try assembling txt file with my assembler\n");
        fclose(infile);
        return;
    }

    if (fread(&binary_header.instruction_count, sizeof(uint64_t), 1, infile) != 1){
        fprintf(stderr, "Error: Failed to read instruction count from header.\n");
        fclose(infile);
        return;
    }
    
    uint32_t pool_size = 0;
    if (fread(&pool_size, sizeof(uint32_t), 1, infile) != 1){
        fprintf(stderr, "Error: Failed to read string pool size.\n");
        fclose(infile);
        return;
    }

    uint8_t* string_pool_base = NULL;
    if (pool_size > 0){
        string_pool_base = malloc(pool_size);
        if (!string_pool_base){
            fprintf(stderr, "Error: Out of memory allocating string pool\n");
            fclose(infile);
            return;
        }
        if (fread(string_pool_base, 1, pool_size, infile) != pool_size){
            fprintf(stderr, "Failed to read string pool data\n");
            free(string_pool_base);
            fclose(infile);
            return;
        }
    }

    uint32_t* dynamic_program = malloc(binary_header.instruction_count * sizeof(uint32_t));
    if (!dynamic_program){
        fprintf(stderr, "Out of memory allocating instruction buffer\n");
        fclose(infile);
        return;
    }

    size_t read_count = fread(dynamic_program, sizeof(uint32_t), binary_header.instruction_count, infile);
    if (read_count != binary_header.instruction_count){
        fprintf(stderr, "Error: File ended early, expected %llu instructions, got %zu.\n", binary_header.instruction_count,
                    read_count);
        free(dynamic_program);
        fclose(infile);
        return;
    }

    fclose(infile);
    
    run_jit_compiler(dynamic_program, binary_header.instruction_count, vm_state, string_pool_base);
    free(dynamic_program);
    free(string_pool_base);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <program.cvm>\n", argv[0]);
        return 1;
    }

    initialize_got();
    init_plt_from_got(procedure_linkage_table);

    VMState_t* vm = calloc(1, sizeof(VMState_t)); 
    load_and_run_file(argv[1], vm);
    
    free(vm);
    return 0;
}
