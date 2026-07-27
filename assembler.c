#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define OP_MOV     0x01
#define OP_ADD     0x02
#define OP_RETURN  0x03
#define OP_SUB     0x04
#define OP_XOR     0x05
#define OP_JMP     0x06
#define OP_JNE     0x07
#define OP_JE      0x08
#define OP_JZ      0x09
#define OP_JG      0xA
#define OP_JNLE    0xB
#define OP_JL      0xC
#define OP_NGE     0xD
#define OP_JGE     0xE
#define OP_JNL     0xF
#define OP_JLE     0x10
#define OP_JNG     0x11
#define OP_CMP     0x12
#define OP_INDEX_PTRB 0x13
#define OP_INDEX_PTRW 0x14
#define OP_INDEX_PTRD 0x15
#define OP_INDEX_PTRQ 0x16
#define OP_LOAD       0x17
#define OP_STORE      0x18
#define OP_LET        0x19
#define OP_CALL       0x1A

#define V0 0
#define V1 1
#define V2 2
#define V3 3
#define V4 4
#define V5 5
#define V6 6
#define V7 7
#define V8 8

#define SET_BIT7_TO_ZERO 0b01111111
#define SET_BIT7_TO_ONE  0b10000000
#define MAX_LINES 256
#define MAX_LABEL_LENGTH 1024
#define MAX_INSTRUCTIONS 1024
#define MAGIC_NUMBER "CVM0"
#define MAX_VARS 64

typedef struct {
    char* function_name;
    uint32_t instruction_index;
}Function_t;

typedef struct {
    char name[32];
    uint8_t virtualreg_index;
}Symbol_t;

typedef struct{
    Symbol_t vars[MAX_VARS];
    uint8_t count;
}SymbolTable_t;

SymbolTable_t s_table = { .count = 0};

Function_t functions[1024];
int function_count = 0;

int parse_register(const char* register_str)
{
    if (strcmp(register_str, "V0") == 0 || strcmp(register_str, "V0,") == 0) return V0;
    if (strcmp(register_str, "V1") == 0 || strcmp(register_str, "V1,") == 0) return V1;
    if (strcmp(register_str, "V2") == 0 || strcmp(register_str, "V2,") == 0) return V2;
    return -1;
}

uint8_t get_or_create_variable(SymbolTable_t* s_table, const char* name)
{
    for (uint8_t i = 0; i < s_table->count; i++){
        if (strcmp(s_table->vars[i].name, name) == 0){
            return s_table->vars[i].virtualreg_index;
        }
    }
    uint8_t new_reg = s_table->count;
    strcpy(s_table->vars[s_table->count].name, name);
    s_table->vars[s_table->count].virtualreg_index = new_reg;
    s_table->count++;
    return new_reg;
}

int is_label(const char* line)
{
    size_t len = strlen(line);
    if (len > 0 && line[len -1] == ':'){
        return 0;
    }
    else{
        return -1;
    }
}

void trim_line(char* str) {
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == '\n' || str[len - 1] == '\r' || str[len - 1] == ' ' || str[len - 1] == '\t')) {
        str[--len] = '\0';
    }
}

static inline uint32_t resolve_function_indices(const char* function_name)
{
    char clean_name[MAX_LABEL_LENGTH];
    strncpy(clean_name, function_name, sizeof(clean_name));
    clean_name[sizeof(clean_name) - 1] = '\0';
    trim_line(clean_name);
    for (int i = 0; i < function_count; i++){
        if (strcmp(functions[i].function_name, clean_name) == 0){
            return functions[i].instruction_index;
        }
    }
    fprintf(stderr, "Error: Unresolved label '%s'\n", function_name);
    return 0;
}

uint8_t parse_operand(char* operand, SymbolTable_t* s_table)
{
    if (operand[0] == 'V' || operand[0] == 'v'){
        uint8_t reg_index = atoi(operand + 1);
        return reg_index;
    }
    return get_or_create_variable(s_table, operand);
}

int parse_function(const char* function_name)
{
    if (strcmp(function_name, "puts") == 0) return 0;
    fprintf(stderr, "Error: Unknown function '%s'\n", function_name);
    exit(1);
}

char string_pool[4096];
uint32_t string_pool_offset = 0;
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
    char line[MAX_LINES]; 
    size_t pass1_instruction_count = 0;
    while(fgets(line, sizeof(line), infile)){
        trim_line(line);
        if (line[0] == '#' || line[0] == '\0') continue;

        char first_word[128];
        if (sscanf(line, "%127s", first_word) <=0) continue;
        if (first_word[0] == '#' || first_word[0] == '\0') continue;

        if (is_label(first_word) == 0){
            trim_line(first_word);
            first_word[strlen(first_word) - 1] = '\0';
            functions[function_count].function_name = strdup(first_word);
            functions[function_count].instruction_index = pass1_instruction_count;
            function_count++;
        }
        else{
            pass1_instruction_count++;
        }
    }
    for (int i = 0; i < function_count; i++){
        printf("Registered Label: %s -> Index %u\n", 
            functions[i].function_name, 
            functions[i].instruction_index);
    }
    fseek(infile, 0, SEEK_SET);
    
    uint32_t* instruction_buffer = malloc(MAX_INSTRUCTIONS * sizeof(uint32_t));
    size_t instruction_count = 0;
    int line_number = 0;
    while (fgets(line, sizeof(line), infile)){
        line_number++;
        trim_line(line);
        if (line[0] == '\n' || line[0] == '\0' || line[0] == '\r' || line[0] == '#') continue;
        char op[32] = {0};
        char arg1[32] = {0};
        char arg2[128] = {0};
        
        char* line_ptr = line;
        while (*line_ptr == ' ' || *line_ptr == '\t') line_ptr++;
        int i = 0;
        while (*line_ptr && *line_ptr != ' ' && *line_ptr != '\t' && i < 31){
            op[i++] = *line_ptr++;
        }
        op[i] = '\0';
        while (*line_ptr == ' ' || *line_ptr == '\t') line_ptr++;

        if (*line_ptr){
            int i = 0;
            while (*line_ptr && *line_ptr != ',' && *line_ptr != ' ' && *line_ptr != '\t' && i < 31){
                arg1[i++] = *line_ptr++;
            }
            arg1[i] = '\0';
            if (*line_ptr == ',') line_ptr++;
            
            // FIX: Skip whitespace before capturing arg2 so arg2[0] becomes '"' instead of ' '
            while (*line_ptr == ' ' || *line_ptr == '\t') line_ptr++;

            if (*line_ptr){
                strncpy(arg2, line_ptr, sizeof(arg2) - 1);
                arg2[sizeof(arg2) - 1] = '\0';
                trim_line(arg2);
            }
        }

        if (is_label(op) == 0) continue;

        size_t len1 = strlen(arg1);
        if (len1 > 0 && arg1[len1 - 1] == ',') arg1[len1 - 1] = '\0';
        size_t len2 = strlen(arg2);
        if (len2 > 0 && arg2[len2 - 1] == ',') arg2[len2 - 1] = '\0';
        
        printf("DEBUG: op=[%s] arg1=[%s] arg2=[%s]\n", op, arg1, arg2);
        uint32_t compiled_instruction = 0;
        if (strcmp(op, "MOV") == 0 || strcmp(op, "mov") == 0){
            uint8_t dest = parse_operand(arg1, &s_table);
            int src = parse_register(arg2);
                if (src != -1){
                    uint8_t reg_mode_dest = (dest & SET_BIT7_TO_ZERO);
                    compiled_instruction = OP_MOV | ((reg_mode_dest & 0xFF) << 8) | ((src & 0xFFFF) << 16);
                }else{
                    uint8_t imm_mode_dest = (dest | SET_BIT7_TO_ONE);
                    int imm = atoi(arg2);
                    compiled_instruction = OP_MOV | (imm_mode_dest << 8) | ((imm & 0xFFFF) << 16);
                }
        }
        else if (strcmp(op, "LET") == 0 || strcmp(op, "let") == 0){
            uint8_t dest = parse_operand(arg1, &s_table);
           
            if (arg2[0] == '"'){ 
                uint8_t imm_mode_dest = dest | SET_BIT7_TO_ONE;
                char clean_str[128];
                sscanf(arg2, "\"%[^\"]\"", clean_str); // Extracts text inside the quotes
                uint64_t current_str_address = (uint64_t)(uintptr_t)&string_pool[string_pool_offset];
                compiled_instruction = OP_LET | (imm_mode_dest << 8) | (string_pool_offset << 16);
                strcpy(&string_pool[string_pool_offset], clean_str);
                string_pool_offset += strlen(clean_str) + 1;
            }
            else{
                int src = parse_register(arg2);
                if (src != -1){
                    uint8_t reg_mode_dest = (dest & SET_BIT7_TO_ZERO);
                    compiled_instruction = OP_MOV | ((reg_mode_dest & 0xFF) << 8) | ((src & 0xFFFF) << 16);
                }else{
                    uint8_t imm_mode_dest = (dest | SET_BIT7_TO_ONE);
                    int imm = atoi(arg2);
                    compiled_instruction = OP_MOV | (imm_mode_dest << 8) | ((imm & 0xFFFF) << 16);
                }
            }
        }
        else if (strcmp(op, "ADD") == 0 || strcmp(op, "add") == 0){
            int dest = parse_register(arg1);
            int src = parse_register(arg2);
            if(dest == -1 || src == -1){
                fprintf(stderr, "Line %d: Invalid registers for op ADD\n", line_number);
                continue;
            }
            compiled_instruction = OP_ADD | (dest << 8) | ((src & 0xFFFF) << 16);
        }
        else if(strcmp(op, "SUB") == 0 || strcmp(op, "sub") == 0){
            int dest = parse_register(arg1);
            int src = parse_register(arg2);
            if (dest == -1 || src == -1){
                fprintf(stderr, "Line %d: Invalid registers for op SUB\n", line_number);
                continue;
            }
            compiled_instruction = OP_SUB | (dest << 8) | ((src & 0xFFFF) << 16);
        }
        else if (strcmp(op, "XOR") == 0 || strcmp(op, "xor") == 0){
            int dest = parse_register(arg1);
            int src = parse_register(arg2);
            if (dest == -1 || src == -1){
                fprintf(stderr, "Line %d: Invalid registers for op XOR\n", line_number);
                continue;
            }
            compiled_instruction = OP_XOR | (dest << 8) | ((src & 0xFFFF) << 16);
        }
        else if (strcmp(op, "JMP") == 0 || strcmp(op, "jmp") == 0){
                if (arg1[strlen(arg1) - 1] == ',') arg1[strlen(arg1) - 1] = '\0';
                uint32_t target_instruction = resolve_function_indices(arg1);
                compiled_instruction = OP_JMP | ((target_instruction << 8) & 0xFFFFFF00);
        }
        // TODO: IMPLEMENT JUMP TO LABEL ADDRESSES FOR THE REST
        // IVE GONE TO SLEEP IM TIRED ASF!
        else if(strcmp(op, "JNE") == 0 || strcmp(op, "jne") == 0){
                uint32_t target_instruction = atoi(arg1);
                compiled_instruction = OP_JNE | (target_instruction << 8) & 0xFFFFFF00;
        }
        else if (strcmp(op, "JE") == 0 || strcmp(op, "je") == 0){
            if (arg1[strlen(arg1) -1] == ',') arg1[strlen(arg1) -1] = '\0';
            uint32_t target_instruction = resolve_function_indices(arg1);
            compiled_instruction = OP_JE | (target_instruction << 8) & 0xFFFFFF00;
        }
        else if (strcmp(op, "JZ") == 0 || strcmp(op, "jz") == 0){
            uint32_t target_instruction = atoi(arg1);
            compiled_instruction = OP_JZ | (target_instruction << 8) & 0xFFFFFF00;
        }
        else if (strcmp(op, "CMP") == 0 || strcmp(op, "cmp") == 0){
            uint8_t dest = parse_register(arg1);
            uint8_t src = parse_register(arg2);
            if (src != -1){ 
                uint8_t reg_mode_dest = (dest & SET_BIT7_TO_ZERO);
                compiled_instruction = OP_CMP | ((reg_mode_dest & 0xFF) << 8) | ((src & 0xFFFF) << 16);
            }else{
                uint16_t immediate = atoi(arg2);
                uint8_t imm_mode_dest = (dest | SET_BIT7_TO_ONE);
                compiled_instruction = OP_CMP | ((imm_mode_dest & 0xFF) << 8) | ((immediate & 0xFFFF) << 16);
            }
        }
        // IM TIREED IMPLEMENTING VARIABLE'S MEMORY ALLOCATION AND RETRIEVAL IS NOT A JOKE!
        else if (strcmp(op, "INDEXPTR_B") == 0 || strcmp(op, "indexptr_b") == 0){
            uint8_t base = parse_operand(arg1, &s_table);
            uint8_t index = parse_operand(arg2, &s_table);
            compiled_instruction = OP_INDEX_PTRB | ((base & 0xFF) << 8) | ((index & 0xFFFF) << 16);
        }
        else if (strcmp(op, "INDEX_PTR_Q") == 0 || strcmp(op, "indexptr_q") == 0) { // 8 bytes
            uint8_t base = parse_operand(arg1, &s_table);
            uint8_t index  = parse_operand(arg2, &s_table);
            compiled_instruction = OP_INDEX_PTRQ | ((base & 0xFF) << 8) | ((index & 0xFFFF) << 16);
        }
        else if (strcmp(op, "LOAD") == 0 || strcmp(op, "load") == 0) { // LOAD dst, ptr
            uint8_t dst = parse_operand(arg1, &s_table);
            uint8_t ptr = parse_operand(arg2, &s_table);
            compiled_instruction = OP_LOAD | ((dst & 0xFF) << 8) | ((ptr & 0xFFFF) << 16);
        }
        else if (strcmp(op, "STORE") == 0 || strcmp(op, "store") == 0) { // STORE ptr, val
            uint8_t ptr = parse_operand(arg1, &s_table);
            uint8_t val = parse_operand(arg2, &s_table);
            compiled_instruction = OP_STORE | ((ptr & 0xFF) << 8) | ((val & 0xFFFF) << 16);
        }
        else if (strcmp(op, "CALL") == 0 || strcmp(op, "call") == 0){
            int dest = parse_function(arg1);
            compiled_instruction = OP_CALL | ((dest & 0xFF) << 8);
        }
        else if (strcmp(op, "RETURN") == 0 || strcmp(op, "return") == 0){
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

    uint32_t pool_size = string_pool_offset;
    printf("DEBUG: Writing pool_size = %u\n", pool_size);
    fwrite(&pool_size, sizeof(uint32_t), 1, outfile);
    if (pool_size > 0) {
        fwrite(string_pool, 1, pool_size, outfile);
    }

    fwrite(instruction_buffer, sizeof(uint32_t), instruction_count, outfile);

    fclose(outfile);
    free(instruction_buffer);
    
    for(int i = 0; i < function_count; i++) {
        free(functions[i].function_name);
    }
    printf("Succesfully assembled %zu instructions into %s\n", instruction_count, argv[2]);
    return 0;
}
