#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>

#define OP_MOV_IMM 0x01
#define OP_ADD 0x02
#define OP_RETURN 0x03
#define V0 0 
#define V1 1
#define V2 2

typedef struct {
    uint64_t registers[3];
}VMState_t;

void compile_and_run(uint32_t* code_to_be_executed, size_t instruction_count, VMState_t* vm_state)
{
    size_t page_size = 4096;
    uint8_t* executable_code_buffer = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS |
                                           MAP_PRIVATE, -1, 0);
    uint8_t* emit_code = executable_code_buffer;

    for (size_t i = 0; i < instruction_count; i++){
        uint32_t instruction = code_to_be_executed[i];
        uint8_t opcode = instruction & 0xFF;
        uint8_t destination_register = (instruction >> 8) & 0xFF;
        uint8_t source_register = (instruction >> 16) & 0xFF;
        uint16_t immediate = (instruction >> 16) & 0xFFFF;

        switch (opcode){
            // x86-64: mov qword ptr [rdi + offset], imm32
            // Opcodes: 0x48 0xC7 0x47 [offset] [4-byte imm]
            case OP_MOV_IMM: {
                                *emit_code++ = 0x48;
                                *emit_code++ = 0xC7;
                                *emit_code++ = 0x47;
                                *emit_code++ = destination_register * 8;
                                // Craft the immediate in little endian
                                *emit_code++ = immediate & 0xFF;
                                *emit_code++ = (immediate >> 8) & 0xFF;
                                *emit_code++ = 0x00;
                                *emit_code++ = 0x00;
                                break;
                             }
            case OP_ADD: {
                             // To add memory to memory in x86 load it into a temp register
                             // Step A: mov rax, [rdi + src_offset] -> 0x48 0x8B 0x47 [offset]
                             *emit_code++ = 0x48; *emit_code++ = 0x8B; *emit_code++ = 0x47;
                             *emit_code++ = source_register * 8;
                             // Step B: mov [rdi + destination_offset], rax 
                             *emit_code++ = 0x48; *emit_code++ = 0x01; *emit_code++ = 0x47;
                             *emit_code++ = destination_register * 8;
                             break;
                         }
            case OP_RETURN: {
                                // As obvious if you dont know let me tell you ignorant programmer, 
                                // In the C Standard calling convention, return values must be in the rax
                                *emit_code++ = 0x48; *emit_code++ = 0x8B; *emit_code++ = 0x47;
                                *emit_code++ = destination_register * 8;

                                *emit_code++ = 0xC3;
                                break;
                            }
        }
    }
    mprotect(executable_code_buffer, page_size, PROT_READ | PROT_EXEC);
    uint64_t (*jit_func)(VMState_t*) = (uint64_t(*)(VMState_t*))executable_code_buffer;
    uint64_t result = jit_func(vm_state);

    printf("JIT Execution Finished.\n");
    printf("Returned Value: %llu\n", result);
    printf("VM Register State: V0=%llu, V1=%llu, V2=%llu\n",
                vm_state->registers[V0], vm_state->registers[V1], vm_state->registers[V2]);

    munmap(executable_code_buffer, page_size);
}

void load_and_run_file(const char* cvm_file, VMState_t* vm_state)
{
    FILE* infile = fopen(cvm_file, "rb");
    if (!infile){
        perror("Error: Failed to open .cvm file\n");
        return;
    }
    char magic[4];
    if (fread(magic, 1, 4, infile) != 4 || magic[0] != 'C' || 
            magic[1] != 'V' || magic[2] != 'M' || magic[3] != '0'){
        fprintf(stderr, "Invalid File binary format (Magic Number Mismatch)\n");
        printf("HINT: Try assembling txt file with my assembler\n");
        fclose(infile);
        return;
    }

    uint64_t instruction_count = 0;
    if (fread(&instruction_count, sizeof(uint64_t), 1, infile) != 1){
        fprintf(stderr, "Error: Failed to read instruction count from header.\n");
        fclose(infile);
        return;
    }

    uint32_t* dynamic_program = malloc(instruction_count * sizeof(uint32_t));
    if (!dynamic_program){
        fprintf(stderr, "Out of memory allocating instruction buffer\n");
        fclose(infile);
        return;
    }

    size_t read_count = fread(dynamic_program, sizeof(uint32_t), instruction_count, infile);
    if (read_count != instruction_count){
        fprintf(stderr, "Error: File ended early, expected %llu instructions, got %zu.\n", instruction_count,
                    read_count);
        free(dynamic_program);
        fclose(infile);
        return;
    }

    fclose(infile);
    
    compile_and_run(dynamic_program, instruction_count, vm_state);
    free(dynamic_program);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <program.cvm>\n", argv[0]);
        return 1;
    }

    VMState_t vm = {.registers = {0, 0, 0}};
    load_and_run_file(argv[1], &vm);

    return 0;
}
