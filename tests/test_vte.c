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
    // Use the enhanced print function but also update output buffer
    enhanced_print(panel, codepoint);
    
    // Also add to output buffer for text verification
    if (output_pos < sizeof(output_buffer) - 1) {
        output_buffer[output_pos++] = (char)codepoint;
    }
}

static void test_execute(terminal_panel_t *panel, uint8_t byte) {
    // Use the enhanced execute function but also update output buffer
    enhanced_execute(panel, byte);
    
    if (byte == '\n' && output_pos < sizeof(output_buffer) - 1) {
        output_buffer[output_pos++] = '\n';
    }
}

static void test_csi_dispatch(terminal_panel_t *panel, const vte_params_t *params,
                             const uint8_t *intermediates, size_t intermediate_len,
                             bool ignore, char action) {
    // Use the enhanced CSI dispatch from vte_parser.c
    enhanced_csi_dispatch(panel, params, intermediates, intermediate_len, ignore, action);
}

static void test_esc_dispatch(terminal_panel_t *panel, const uint8_t *intermediates,
                             size_t intermediate_len, bool ignore, uint8_t byte) {
    // Use the enhanced ESC dispatch from vte_parser.c
    enhanced_esc_dispatch(panel, intermediates, intermediate_len, ignore, byte);
}

static const vte_perform_t test_perform = {
    .print = test_print,
    .execute = test_execute,
    .csi_dispatch = test_csi_dispatch,
    .esc_dispatch = test_esc_dispatch,
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
    
    // Initialize with enhanced terminal functions
    terminal_panel_init(&test_panel, 40, 10);
    
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

int test_cursor_positioning() {
    setup_test();
    
    // Test basic cursor advancement
    parse_input("Hello");
    if (test_panel.cursor_x != 5 || test_panel.cursor_y != 0) {
        cleanup_test();
        return 0;
    }
    
    // Test newline
    parse_input("\nWorld");
    if (test_panel.cursor_x != 5 || test_panel.cursor_y != 1) {
        cleanup_test();
        return 0;
    }
    
    // Test cursor positioning escape sequence
    parse_input("\033[3;10H*");  // Move to row 3, col 10, then print *
    if (test_panel.cursor_x != 10 || test_panel.cursor_y != 2) {
        cleanup_test();
        return 0;
    }
    
    // Test carriage return
    parse_input("\rStart");
    if (test_panel.cursor_x != 5 || test_panel.cursor_y != 2) {
        cleanup_test();
        return 0;
    }
    
    cleanup_test();
    return 1;
}

int test_cursor_movement() {
    setup_test();
    
    // Test cursor up/down/left/right
    parse_input("Hello\033[3D\033[2A*");  // Move left 3, up 2, print *
    
    // Should be at position (2, -2) but clamped to (2, 0)
    if (test_panel.cursor_x != 3 || test_panel.cursor_y != 0) {
        cleanup_test();
        return 0;
    }
    
    // Test cursor positioning
    parse_input("\033[5;10H+");  // Move to row 5, col 10, print +
    if (test_panel.cursor_x != 10 || test_panel.cursor_y != 4) {
        cleanup_test();
        return 0;
    }
    
    cleanup_test();
    return 1;
}

int test_screen_clearing() {
    setup_test();
    
    // Fill screen with text
    parse_input("Line1\nLine2\nLine3\n");
    
    // Clear from cursor to end of screen
    parse_input("\033[2;3H\033[0J");
    
    // Check that content before cursor is preserved
    if (test_screen[0][0].codepoint != 'L' || test_screen[1][0].codepoint != 'L') {
        cleanup_test();
        return 0;
    }
    
    // Check that content after cursor is cleared
    if (test_screen[1][3].codepoint != ' ' || test_screen[2][0].codepoint != ' ') {
        cleanup_test();
        return 0;
    }
    
    cleanup_test();
    return 1;
}

int test_line_operations() {
    setup_test();
    
    // Add some lines
    parse_input("Line1\nLine2\nLine3\n");
    
    // Go to line 2 and insert a line
    parse_input("\033[2H\033[1L");
    
    // Line 2 should now be empty, Line 3 should have "Line2"
    if (test_screen[1][0].codepoint != ' ' || test_screen[2][0].codepoint != 'L') {
        cleanup_test();
        return 0;
    }
    
    // Delete the inserted line
    parse_input("\033[1M");
    
    // Line 2 should now have "Line2" again
    if (test_screen[1][0].codepoint != 'L' || test_screen[1][4].codepoint != '2') {
        cleanup_test();
        return 0;
    }
    
    cleanup_test();
    return 1;
}

int test_character_operations() {
    setup_test();
    
    // Type some text
    parse_input("Hello World");
    
    // Go back and insert characters
    parse_input("\033[6G\033[3@");  // Go to column 6, insert 3 chars
    
    // Should have "Hello    World" with cursor at position 5
    if (test_screen[0][5].codepoint != ' ' || test_screen[0][9].codepoint != 'W') {
        cleanup_test();
        return 0;
    }
    
    // Delete some characters
    parse_input("\033[2P");  // Delete 2 characters
    
    // Should have "Hello  World" 
    if (test_screen[0][5].codepoint != ' ' || test_screen[0][7].codepoint != 'W') {
        cleanup_test();
        return 0;
    }
    
    cleanup_test();
    return 1;
}

int test_scrolling_regions() {
    setup_test();
    
    // Set scrolling region from line 2 to 4
    parse_input("\033[2;4r");
    
    if (test_panel.scroll_top != 1 || test_panel.scroll_bottom != 3) {
        cleanup_test();
        return 0;
    }
    
    // Cursor should be at home position
    if (test_panel.cursor_x != 0 || test_panel.cursor_y != 0) {
        cleanup_test();
        return 0;
    }
    
    cleanup_test();
    return 1;
}

int test_tab_operations() {
    setup_test();
    
    // Test basic tab
    parse_input("A\tB");
    if (test_panel.cursor_x != 9) {  // Should be at next tab stop (8) + 1 for 'B'
        cleanup_test();
        return 0;
    }
    
    // Set a custom tab stop
    parse_input("\033[15G\033H");  // Go to column 15, set tab
    
    // Test custom tab (don't clear all tabs first)
    parse_input("\033[1G\t");  // Go to start, tab to first tab stop (8)
    if (test_panel.cursor_x != 8) {  // Should be at first tab stop (8)
        cleanup_test();
        return 0;
    }
    
    // Tab again to custom tab stop
    parse_input("\t");  // Tab to custom tab stop (14)
    if (test_panel.cursor_x != 14) {  // Should be at custom tab (14)
        cleanup_test();
        return 0;
    }
    
    cleanup_test();
    return 1;
}

int test_character_sets() {
    setup_test();
    
    // Switch to DEC special character set and draw a line
    parse_input("\033(0qqq\033(B");  // G0 = DEC special, draw line, G0 = ASCII
    
    // Check that line drawing characters were mapped
    if (test_screen[0][0].codepoint != 0x2500) {  // Horizontal line
        cleanup_test();
        return 0;
    }
    
    cleanup_test();
    return 1;
}

int test_save_restore_cursor() {
    setup_test();
    
    // Move cursor and set attributes
    parse_input("\033[5;10H\033[31mRed");
    
    // Save cursor
    parse_input("\033[s");
    
    // Move elsewhere and change attributes
    parse_input("\033[1;1H\033[32mGreen");
    
    // Restore cursor
    parse_input("\033[u");
    
    // Should be back at (5,10) with red color after printing "Red"
    if (test_panel.cursor_x != 12 || test_panel.cursor_y != 4 || test_panel.fg_color != 1) {
        cleanup_test();
        return 0;
    }
    
    cleanup_test();
    return 1;
}

int test_terminal_modes() {
    setup_test();
    
    // Test application cursor keys mode
    parse_input("\033[?1h");  // Set application cursor keys
    if (!test_panel.modes.application_cursor_keys) {
        cleanup_test();
        return 0;
    }
    
    parse_input("\033[?1l");  // Reset application cursor keys
    if (test_panel.modes.application_cursor_keys) {
        cleanup_test();
        return 0;
    }
    
    // Test auto wrap mode
    parse_input("\033[?7l");  // Disable auto wrap
    if (test_panel.modes.auto_wrap) {
        cleanup_test();
        return 0;
    }
    
    cleanup_test();
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

int test_extended_colors() {
    setup_test();
    
    // Test 256-color mode
    parse_input("\033[38;5;196mBright Red");
    if (test_panel.fg_color != 196) {
        cleanup_test();
        return 0;
    }
    
    // Test RGB color mode (simplified)
    parse_input("\033[38;2;255;0;0mRGB Red");
    if (test_panel.fg_color != 1) {  // Should map to basic red
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
    TEST(cursor_positioning);
    TEST(complex_sequences);
    TEST(cursor_movement);
    TEST(screen_clearing);
    TEST(line_operations);
    TEST(character_operations);
    TEST(scrolling_regions);
    TEST(tab_operations);
    TEST(character_sets);
    TEST(save_restore_cursor);
    TEST(terminal_modes);
    TEST(extended_colors);
    
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