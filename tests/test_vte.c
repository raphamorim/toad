#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include "vte/vte_parser.h"

// Test counters
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { \
        printf("Running test_%s... ", #name); \
        tests_run++; \
        if (test_##name()) { \
            printf("✅ PASS\n"); \
            tests_passed++; \
        } else { \
            printf("❌ FAIL\n"); \
        } \
    } while(0)

// Test terminal setup
static terminal_panel_t test_panel;
static terminal_cell_t test_screen[10][40];
static char output_buffer[1024];
static size_t output_pos = 0;

// Test perform implementation
static void test_print(terminal_panel_t *panel, uint32_t codepoint) {
    if (panel->cursor_y >= 0 && panel->cursor_y < 10 &&
        panel->cursor_x >= 0 && panel->cursor_x < 40) {
        
        terminal_cell_t *cell = &test_screen[panel->cursor_y][panel->cursor_x];
        cell->codepoint = codepoint;
        cell->fg_color = panel->fg_color;
        cell->bg_color = panel->bg_color;
        cell->attrs = panel->attrs;
        
        // Also add to output buffer for text verification
        if (output_pos < sizeof(output_buffer) - 1) {
            output_buffer[output_pos++] = (char)codepoint;
        }
        
        panel->cursor_x++;
        if (panel->cursor_x >= 40) {
            panel->cursor_x = 0;
            panel->cursor_y++;
        }
    }
}

static void test_execute(terminal_panel_t *panel, uint8_t byte) {
    if (byte == '\n' && output_pos < sizeof(output_buffer) - 1) {
        output_buffer[output_pos++] = '\n';
        panel->cursor_x = 0;
        panel->cursor_y++;
    } else if (byte == '\r') {
        panel->cursor_x = 0;
    }
}

static void test_csi_dispatch(terminal_panel_t *panel, const vte_params_t *params,
                             const uint8_t *intermediates, size_t intermediate_len,
                             bool ignore, char action) {
    (void)intermediates; (void)intermediate_len; (void)ignore;
    
    if (action == 'm') {
        if (vte_params_len(params) == 0) {
            panel->fg_color = -1;
            panel->bg_color = -1;
            panel->attrs = 0;
            return;
        }
        
        for (size_t i = 0; i < vte_params_len(params); i++) {
            uint16_t param = vte_params_get_single(params, i, 0);
            
            switch (param) {
                case 0:
                    panel->fg_color = -1;
                    panel->bg_color = -1;
                    panel->attrs = 0;
                    break;
                case 1:
                    panel->attrs |= 1; // Bold (simplified)
                    break;
                case 4:
                    panel->attrs |= 2; // Underline (simplified)
                    break;
                case 7:
                    panel->attrs |= 4; // Reverse (simplified)
                    break;
                case 22:
                    panel->attrs &= ~1; // No bold
                    break;
                case 24:
                    panel->attrs &= ~2; // No underline
                    break;
                case 27:
                    panel->attrs &= ~4; // No reverse
                    break;
                case 30: case 31: case 32: case 33:
                case 34: case 35: case 36: case 37:
                    panel->fg_color = param - 30;
                    break;
                case 39:
                    panel->fg_color = -1; // Default foreground
                    break;
                case 40: case 41: case 42: case 43:
                case 44: case 45: case 46: case 47:
                    panel->bg_color = param - 40;
                    break;
                case 49:
                    panel->bg_color = -1; // Default background
                    break;
                case 90: case 91: case 92: case 93:
                case 94: case 95: case 96: case 97:
                    panel->fg_color = param - 90;
                    panel->attrs |= 1; // Bright colors are bold
                    break;
                case 100: case 101: case 102: case 103:
                case 104: case 105: case 106: case 107:
                    panel->bg_color = param - 100;
                    break;
            }
        }
    }
}

static const vte_perform_t test_perform = {
    .print = test_print,
    .execute = test_execute,
    .csi_dispatch = test_csi_dispatch,
    .esc_dispatch = NULL,
    .osc_dispatch = NULL,
    .hook = NULL,
    .put = NULL,
    .unhook = NULL
};

void setup_test() {
    memset(&test_panel, 0, sizeof(test_panel));
    memset(test_screen, 0, sizeof(test_screen));
    memset(output_buffer, 0, sizeof(output_buffer));
    output_pos = 0;
    
    test_panel.screen = (terminal_cell_t**)malloc(10 * sizeof(terminal_cell_t*));
    for (int i = 0; i < 10; i++) {
        test_panel.screen[i] = test_screen[i];
    }
    test_panel.screen_width = 40;
    test_panel.screen_height = 10;
    test_panel.fg_color = -1;
    test_panel.bg_color = -1;
    test_panel.attrs = 0;
    
    vte_parser_init(&test_panel.parser);
    test_panel.perform = test_perform;
}

void cleanup_test() {
    if (test_panel.screen) {
        free(test_panel.screen);
        test_panel.screen = NULL;
    }
}

void parse_input(const char *input) {
    vte_parser_advance(&test_panel.parser, &test_panel, 
                      (const uint8_t *)input, strlen(input));
}

// Test functions
int test_basic_text() {
    setup_test();
    
    parse_input("Hello World");
    
    int result = (strcmp(output_buffer, "Hello World") == 0);
    cleanup_test();
    return result;
}

int test_control_characters() {
    setup_test();
    
    parse_input("Line1\nLine2");
    
    int result = (strcmp(output_buffer, "Line1\nLine2") == 0);
    cleanup_test();
    return result;
}

int test_color_sequences() {
    setup_test();
    
    // Test: Normal text, blue text, reset to default
    parse_input("Normal\033[34mBlue\033[39mDefault");
    
    // Check text content
    if (strcmp(output_buffer, "NormalBlueDefault") != 0) {
        cleanup_test();
        return 0;
    }
    
    // Check colors
    // "Normal" should be default (-1)
    if (test_screen[0][0].fg_color != -1 || test_screen[0][5].fg_color != -1) {
        cleanup_test();
        return 0;
    }
    
    // "Blue" should be blue (4)
    if (test_screen[0][6].fg_color != 4 || test_screen[0][9].fg_color != 4) {
        cleanup_test();
        return 0;
    }
    
    // "Default" should be default (-1) again
    if (test_screen[0][10].fg_color != -1 || test_screen[0][16].fg_color != -1) {
        cleanup_test();
        return 0;
    }
    
    cleanup_test();
    return 1;
}

int test_sgr_reset() {
    setup_test();
    
    // Set colors and attributes, then reset
    parse_input("\033[1;4;7;31;42mStyled\033[0mNormal");
    
    // Check that "Styled" has attributes and colors
    if (test_screen[0][0].attrs == 0 || test_screen[0][0].fg_color != 1 || test_screen[0][0].bg_color != 2) {
        cleanup_test();
        return 0;
    }
    
    // Check that "Normal" is reset
    if (test_screen[0][6].attrs != 0 || test_screen[0][6].fg_color != -1 || test_screen[0][6].bg_color != -1) {
        cleanup_test();
        return 0;
    }
    
    cleanup_test();
    return 1;
}

int test_bright_colors() {
    setup_test();
    
    parse_input("\033[91mBright Red\033[39m");
    
    // Bright red should set fg_color to 1 and bold attribute
    if (test_screen[0][0].fg_color != 1 || (test_screen[0][0].attrs & 1) == 0) {
        cleanup_test();
        return 0;
    }
    
    cleanup_test();
    return 1;
}

int test_background_colors() {
    setup_test();
    
    parse_input("\033[42mGreen BG\033[49mDefault BG");
    
    // "Green BG" should have green background
    if (test_screen[0][0].bg_color != 2) {
        cleanup_test();
        return 0;
    }
    
    // "Default BG" should have default background
    if (test_screen[0][8].bg_color != -1) {
        cleanup_test();
        return 0;
    }
    
    cleanup_test();
    return 1;
}

int test_parameter_parsing() {
    vte_params_t params;
    vte_params_init(&params);
    
    // Test basic parameter
    vte_params_push(&params, 42);
    if (vte_params_get_single(&params, 0, 0) != 42) {
        return 0;
    }
    
    // Test multiple parameters
    vte_params_push(&params, 123);
    if (vte_params_get_single(&params, 1, 0) != 123 || vte_params_len(&params) != 2) {
        return 0;
    }
    
    // Test subparameters
    vte_params_clear(&params);
    vte_params_push(&params, 38);
    vte_params_extend(&params, 2);
    vte_params_extend(&params, 255);
    vte_params_extend(&params, 0);
    vte_params_extend(&params, 255);
    
    if (vte_params_len(&params) != 1) {
        return 0;
    }
    
    size_t subparam_count;
    const uint16_t *subparams = vte_params_get(&params, 0, &subparam_count);
    if (!subparams || subparam_count != 5) {
        return 0;
    }
    
    if (subparams[0] != 38 || subparams[1] != 2 || subparams[2] != 255 || 
        subparams[3] != 0 || subparams[4] != 255) {
        return 0;
    }
    
    return 1;
}

int test_utf8_utilities() {
    // Test UTF-8 character length detection
    if (vte_utf8_char_len('A') != 1) return 0;
    if (vte_utf8_char_len(0xC3) != 2) return 0;
    if (vte_utf8_char_len(0xE2) != 3) return 0;
    if (vte_utf8_char_len(0xF0) != 4) return 0;
    
    // Test UTF-8 decoding
    uint8_t utf8_a[] = {'A'};
    if (vte_utf8_decode(utf8_a, 1) != 'A') return 0;
    
    uint8_t utf8_euro[] = {0xE2, 0x82, 0xAC}; // Euro symbol
    uint32_t euro = vte_utf8_decode(utf8_euro, 3);
    if (euro != 0x20AC) return 0;
    
    return 1;
}

int test_complex_sequences() {
    setup_test();
    
    // Test complex sequence with multiple attributes and colors
    parse_input("\033[1;4;31;42mComplex\033[22;24;39;49mPartial Reset\033[0mFull Reset");
    
    // Check "Complex" has all attributes
    terminal_cell_t *cell = &test_screen[0][0];
    if (cell->fg_color != 1 || cell->bg_color != 2 || (cell->attrs & 3) != 3) {
        cleanup_test();
        return 0;
    }
    
    // Check "Partial Reset" has some attributes removed
    cell = &test_screen[0][7];
    if (cell->fg_color != -1 || cell->bg_color != -1 || (cell->attrs & 3) != 0) {
        cleanup_test();
        return 0;
    }
    
    // Check "Full Reset" is completely reset
    cell = &test_screen[0][20];
    if (cell->fg_color != -1 || cell->bg_color != -1 || cell->attrs != 0) {
        cleanup_test();
        return 0;
    }
    
    cleanup_test();
    return 1;
}

int main() {
    printf("🧪 Running VTE Parser Test Suite\n");
    printf("================================\n\n");
    
    // Run all tests
    TEST(basic_text);
    TEST(control_characters);
    TEST(color_sequences);
    TEST(sgr_reset);
    TEST(bright_colors);
    TEST(background_colors);
    TEST(parameter_parsing);
    TEST(utf8_utilities);
    TEST(complex_sequences);
    
    // Print results
    printf("\n📊 Test Results\n");
    printf("===============\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_run - tests_passed);
    
    if (tests_passed == tests_run) {
        printf("\n🎉 All tests passed!\n");
        return 0;
    } else {
        printf("\n❌ Some tests failed!\n");
        return 1;
    }
}