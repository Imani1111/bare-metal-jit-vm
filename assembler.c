#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define OP_MOV_IMM 0x01
#define OP_ADD     0x02
#define OP_RETURN  0x03

#define V0 0
#define V1 1
#define V2 2
#define MAX_LINES 256
#define MAX_INSTRUCTIONS 1024
#define MAGIC_NUMBER "CVM0"

int parse_register(const char* register_str)
{
    if (strcmp(register_str, "V0") == 0 || strcmp(register_str, "V0,") == 0) return V0;
    if (strcmp(register_str, "V1") == 0 || strcmp(register_str, "V1,") == 0) return V1;
    if (strcmp(register_str, "V2") == 0 || strcmp(register_str, "V2,") == 0) return V2;
    return -1;
}

int main(int argc, char** argv)
{
    if (argc < 3){
        fprintf(stderr, "Usage: %s <input.txt> <output.cvm>\n", argv[0]);
        return 1;
    }
    FILE* infile = fopen(argv[1], "r");
    if(!infile){
        fprintf(stderr, "Failed to open file %s\n", argv[1]);
        return 1;
    }
    uint32_t* instruction_buffer = malloc(MAX_INSTRUCTIONS * sizeof(uint32_t));
    size_t instruction_count = 0;

    char line[MAX_LINES];
    int line_number = 0;
    while (fgets(line, sizeof(line), infile)){
        line_number++;
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '#') continue;

        char op[32] = {0};
        char arg1[32] = {0};
        char arg2[32] = {0};
        
        int tokens = sscanf(line, "%31s %31s %31s", op, arg1, arg2);
        if (tokens <= 0)continue;

        uint32_t compiled_instruction = 0;
        if (strcmp(op, "MOV") == 0){
            int dest = parse_register(arg1);
            int imm = atoi(arg2);
            if (dest == -1){
                fprintf(stderr, "Line %d: Invalid destination register %s\n", line_number, arg1);
                continue;
            }
            compiled_instruction = OP_MOV_IMM | (dest << 8) | ((imm & 0xFFFF) << 16);
        }
        else if (strcmp(op, "ADD") == 0){
            int dest = parse_register(arg1);
            int src = parse_register(arg2);
            if(dest == -1 || src == -1){
                fprintf(stderr, "Line %d: Invalid registers for op ADD\n", line_number);
                continue;
            }
            compiled_instruction = OP_ADD | (dest << 8) | ((src & 0xFFFF) << 16);
        }
        else if (strcmp(op, "RETURN") == 0){
            int dest = parse_register(arg1);
            if (dest == -1){
                fprintf(stderr, "Line %d: Invalid register for op RETURN\n", line_number);
                continue;
            }
            compiled_instruction = OP_RETURN | (dest << 8);
        }
        else {
            fprintf(stderr, "Line %d: Unknown Instruction %s\n", line_number, op);
            continue;
        }
        instruction_buffer[instruction_count++] = compiled_instruction;
    }
    fclose(infile);

    FILE* outfile = fopen(argv[2], "wb");
    if (!outfile){
        perror("Failed to open binary output file\n");
        free(instruction_buffer);
        return 1;
    }

    fwrite(MAGIC_NUMBER, 1, 4, outfile);
    uint64_t count_64 = instruction_count;
    fwrite(&count_64, sizeof(uint64_t), 1, outfile);
    fwrite(instruction_buffer, sizeof(uint64_t), instruction_count, outfile);

    fclose(outfile);
    free(instruction_buffer);

    printf("Succesfully assembled %zu instructions into %s\n", instruction_count, argv[2]);
    return 0;
}
