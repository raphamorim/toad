#include "vte_parser.h"
#include <string.h>
#include <stdlib.h>
#include <ncurses.h>

// UTF-8 utilities
bool vte_is_utf8_continuation(uint8_t byte) {
    return (byte & 0xC0) == 0x80;
}

size_t vte_utf8_char_len(uint8_t first_byte) {
    if ((first_byte & 0x80) == 0) return 1;
    if ((first_byte & 0xE0) == 0xC0) return 2;
    if ((first_byte & 0xF0) == 0xE0) return 3;
    if ((first_byte & 0xF8) == 0xF0) return 4;
    return 0; // Invalid
}

uint32_t vte_utf8_decode(const uint8_t *bytes, size_t len) {
    if (len == 0) return 0;
    
    uint8_t first = bytes[0];
    if ((first & 0x80) == 0) {
        return first;
    }
    
    if ((first & 0xE0) == 0xC0) {
        if (len >= 2) {
            return ((first & 0x1F) << 6) | (bytes[1] & 0x3F);
        }
        return 0xFFFD; // Replacement character
    }
    
    if ((first & 0xF0) == 0xE0) {
        if (len >= 3) {
            return ((first & 0x0F) << 12) | ((bytes[1] & 0x3F) << 6) | (bytes[2] & 0x3F);
        }
        return 0xFFFD;
    }
    
    if ((first & 0xF8) == 0xF0) {
        if (len >= 4) {
            return ((first & 0x07) << 18) | ((bytes[1] & 0x3F) << 12) | 
                   ((bytes[2] & 0x3F) << 6) | (bytes[3] & 0x3F);
        }
        return 0xFFFD;
    }
    
    return 0xFFFD;
}

// Parameter utilities
void vte_params_init(vte_params_t *params) {
    memset(params, 0, sizeof(vte_params_t));
}

void vte_params_clear(vte_params_t *params) {
    params->len = 0;
    params->current_subparams = 0;
    memset(params->subparams, 0, sizeof(params->subparams));
}

void vte_params_push(vte_params_t *params, uint16_t value) {
    if (params->len >= VTE_MAX_PARAMS) return;
    
    // Finalize current parameter if we have subparams
    if (params->current_subparams > 0) {
        size_t param_start = params->len - params->current_subparams;
        params->subparams[param_start] = params->current_subparams;
    }
    
    // Add new parameter
    params->params[params->len] = value;
    params->subparams[params->len] = 1; // This parameter has 1 value
    params->current_subparams = 0;
    params->len++;
}

void vte_params_extend(vte_params_t *params, uint16_t value) {
    if (params->len >= VTE_MAX_PARAMS) return;
    
    // Add subparameter
    params->params[params->len] = value;
    params->current_subparams++;
    params->len++;
    
    // Update count for the current parameter
    if (params->len > params->current_subparams) {
        size_t param_start = params->len - params->current_subparams - 1;
        params->subparams[param_start] = params->current_subparams + 1;
    }
}

bool vte_params_is_full(const vte_params_t *params) {
    return params->len >= VTE_MAX_PARAMS;
}

size_t vte_params_len(const vte_params_t *params) {
    // Count actual parameters (not subparameters)
    size_t count = 0;
    size_t pos = 0;
    
    while (pos < params->len) {
        size_t subparams = params->subparams[pos];
        if (subparams == 0) subparams = 1;
        pos += subparams;
        count++;
    }
    
    return count;
}

const uint16_t *vte_params_get(const vte_params_t *params, size_t index, size_t *subparam_count) {
    size_t current_param = 0;
    size_t pos = 0;
    
    // Find the start of the requested parameter
    while (pos < params->len && current_param < index) {
        size_t subparams = params->subparams[pos];
        if (subparams == 0) subparams = 1;
        pos += subparams;
        current_param++;
    }
    
    if (pos >= params->len || current_param != index) {
        *subparam_count = 0;
        return NULL;
    }
    
    *subparam_count = params->subparams[pos];
    if (*subparam_count == 0) *subparam_count = 1;
    
    return &params->params[pos];
}

uint16_t vte_params_get_single(const vte_params_t *params, size_t index, uint16_t default_val) {
    size_t subparam_count;
    const uint16_t *param = vte_params_get(params, index, &subparam_count);
    return param ? param[0] : default_val;
}

// Character set mapping
uint32_t map_charset_char(charset_t charset, uint8_t ch) {
    if (charset == CHARSET_DEC_SPECIAL && ch >= 0x60 && ch <= 0x7E) {
        // DEC Special Character Set mapping
        static const uint32_t dec_special[] = {
            0x25C6, // ` -> diamond
            0x2592, // a -> checkerboard
            0x2409, // b -> HT symbol
            0x240C, // c -> FF symbol
            0x240D, // d -> CR symbol
            0x240A, // e -> LF symbol
            0x00B0, // f -> degree symbol
            0x00B1, // g -> plus/minus
            0x2424, // h -> NL symbol
            0x240B, // i -> VT symbol
            0x2518, // j -> lower right corner
            0x2510, // k -> upper right corner
            0x250C, // l -> upper left corner
            0x2514, // m -> lower left corner
            0x253C, // n -> crossing lines
            0x23BA, // o -> horizontal line - scan 1
            0x23BB, // p -> horizontal line - scan 3
            0x2500, // q -> horizontal line
            0x23BC, // r -> horizontal line - scan 7
            0x23BD, // s -> horizontal line - scan 9
            0x251C, // t -> left tee
            0x2524, // u -> right tee
            0x2534, // v -> bottom tee
            0x252C, // w -> top tee
            0x2502, // x -> vertical line
            0x2264, // y -> less than or equal
            0x2265, // z -> greater than or equal
            0x03C0, // { -> pi
            0x2260, // | -> not equal
            0x00A3, // } -> UK pound sign
            0x00B7  // ~ -> centered dot
        };
        return dec_special[ch - 0x60];
    }
    
    // For other character sets, return the character as-is for now
    return ch;
}

// Terminal initialization
void terminal_panel_init(terminal_panel_t *panel, int width, int height) {
    panel->screen_width = width;
    panel->screen_height = height;
    panel->cursor_x = 0;
    panel->cursor_y = 0;
    panel->saved_cursor_x = 0;
    panel->saved_cursor_y = 0;
    panel->scroll_top = 0;
    panel->scroll_bottom = height - 1;
    
    // Initialize character sets
    panel->g0_charset = CHARSET_ASCII;
    panel->g1_charset = CHARSET_DEC_SPECIAL;
    panel->using_g1 = false;
    
    // Initialize modes
    memset(&panel->modes, 0, sizeof(panel->modes));
    panel->modes.auto_wrap = true;
    panel->modes.cursor_visible = true;
    
    // Initialize tab stops (every 8 columns)
    memset(panel->tab_stops, 0, sizeof(panel->tab_stops));
    for (int i = 8; i < 256; i += 8) {
        panel->tab_stops[i] = true;
    }
    
    // Initialize colors and attributes
    panel->fg_color = -1;
    panel->bg_color = -1;
    panel->attrs = 0;
    panel->saved_fg_color = -1;
    panel->saved_bg_color = -1;
    panel->saved_attrs = 0;
}

void terminal_panel_reset(terminal_panel_t *panel) {
    terminal_panel_init(panel, panel->screen_width, panel->screen_height);
    terminal_clear_screen(panel, 2);  // Clear entire screen
}

// Screen manipulation
void terminal_clear_screen(terminal_panel_t *panel, int mode) {
    int start_row = 0, end_row = panel->screen_height;
    int start_col = 0, end_col = panel->screen_width;
    
    switch (mode) {
        case 0: // Clear from cursor to end of screen
            start_row = panel->cursor_y;
            start_col = panel->cursor_x;
            break;
        case 1: // Clear from beginning of screen to cursor
            end_row = panel->cursor_y + 1;
            end_col = (panel->cursor_y == end_row - 1) ? panel->cursor_x + 1 : panel->screen_width;
            break;
        case 2: // Clear entire screen
            break;
        case 3: // Clear entire screen and scrollback (same as 2 for now)
            break;
    }
    
    for (int row = start_row; row < end_row && row < panel->screen_height; row++) {
        int col_start = (row == start_row) ? start_col : 0;
        int col_end = (row == end_row - 1) ? end_col : panel->screen_width;
        
        for (int col = col_start; col < col_end && col < panel->screen_width; col++) {
            if (panel->screen && panel->screen[row]) {
                panel->screen[row][col].codepoint = ' ';
                panel->screen[row][col].fg_color = panel->fg_color;
                panel->screen[row][col].bg_color = panel->bg_color;
                panel->screen[row][col].attrs = panel->attrs;
            }
        }
    }
}

void terminal_clear_line(terminal_panel_t *panel, int mode) {
    if (panel->cursor_y < 0 || panel->cursor_y >= panel->screen_height) return;
    if (!panel->screen || !panel->screen[panel->cursor_y]) return;
    
    int start_col = 0, end_col = panel->screen_width;
    
    switch (mode) {
        case 0: // Clear from cursor to end of line
            start_col = panel->cursor_x;
            break;
        case 1: // Clear from beginning of line to cursor
            end_col = panel->cursor_x + 1;
            break;
        case 2: // Clear entire line
            break;
    }
    
    for (int col = start_col; col < end_col && col < panel->screen_width; col++) {
        panel->screen[panel->cursor_y][col].codepoint = ' ';
        panel->screen[panel->cursor_y][col].fg_color = panel->fg_color;
        panel->screen[panel->cursor_y][col].bg_color = panel->bg_color;
        panel->screen[panel->cursor_y][col].attrs = panel->attrs;
    }
}

void terminal_scroll_up(terminal_panel_t *panel, int lines) {
    if (lines <= 0 || !panel->screen) return;
    
    int top = panel->scroll_top;
    int bottom = panel->scroll_bottom;
    
    if (top >= bottom || top < 0 || bottom >= panel->screen_height) return;
    
    // Move lines up (scroll content up)
    for (int i = 0; i < lines; i++) {
        // Move each line up by one
        for (int row = top; row < bottom; row++) {
            if (row + 1 <= bottom && panel->screen[row] && panel->screen[row + 1]) {
                memcpy(panel->screen[row], panel->screen[row + 1], 
                       panel->screen_width * sizeof(terminal_cell_t));
            }
        }
        
        // Clear the bottom line
        if (panel->screen[bottom]) {
            for (int col = 0; col < panel->screen_width; col++) {
                panel->screen[bottom][col].codepoint = ' ';
                panel->screen[bottom][col].fg_color = panel->fg_color;
                panel->screen[bottom][col].bg_color = panel->bg_color;
                panel->screen[bottom][col].attrs = panel->attrs;
            }
        }
    }
}

void terminal_scroll_down(terminal_panel_t *panel, int lines) {
    if (lines <= 0 || !panel->screen) return;
    
    int top = panel->scroll_top;
    int bottom = panel->scroll_bottom;
    
    if (top >= bottom || top < 0 || bottom >= panel->screen_height) return;
    
    // Move lines down (scroll content down)
    for (int i = 0; i < lines; i++) {
        // Move each line down by one (start from bottom)
        for (int row = bottom; row > top; row--) {
            if (row - 1 >= top && panel->screen[row] && panel->screen[row - 1]) {
                memcpy(panel->screen[row], panel->screen[row - 1], 
                       panel->screen_width * sizeof(terminal_cell_t));
            }
        }
        
        // Clear the top line
        if (panel->screen[top]) {
            for (int col = 0; col < panel->screen_width; col++) {
                panel->screen[top][col].codepoint = ' ';
                panel->screen[top][col].fg_color = panel->fg_color;
                panel->screen[top][col].bg_color = panel->bg_color;
                panel->screen[top][col].attrs = panel->attrs;
            }
        }
    }
}

void terminal_insert_lines(terminal_panel_t *panel, int count) {
    if (count <= 0 || !panel->screen) return;
    
    int current_row = panel->cursor_y;
    int top = panel->scroll_top;
    int bottom = panel->scroll_bottom;
    
    // Insert lines only works within the scrolling region
    if (current_row < top || current_row > bottom) return;
    
    // Move existing lines down
    for (int i = 0; i < count; i++) {
        // Move lines down from bottom to current position
        for (int row = bottom; row > current_row; row--) {
            if (panel->screen[row] && panel->screen[row - 1]) {
                memcpy(panel->screen[row], panel->screen[row - 1], 
                       panel->screen_width * sizeof(terminal_cell_t));
            }
        }
        
        // Clear the inserted line
        if (panel->screen[current_row]) {
            for (int col = 0; col < panel->screen_width; col++) {
                panel->screen[current_row][col].codepoint = ' ';
                panel->screen[current_row][col].fg_color = panel->fg_color;
                panel->screen[current_row][col].bg_color = panel->bg_color;
                panel->screen[current_row][col].attrs = panel->attrs;
            }
        }
    }
}

void terminal_delete_lines(terminal_panel_t *panel, int count) {
    if (count <= 0 || !panel->screen) return;
    
    int current_row = panel->cursor_y;
    int top = panel->scroll_top;
    int bottom = panel->scroll_bottom;
    
    // Delete lines only works within the scrolling region
    if (current_row < top || current_row > bottom) return;
    
    // Move lines up to delete current lines
    for (int i = 0; i < count; i++) {
        // Move lines up from current position to bottom
        for (int row = current_row; row < bottom; row++) {
            if (panel->screen[row] && panel->screen[row + 1]) {
                memcpy(panel->screen[row], panel->screen[row + 1], 
                       panel->screen_width * sizeof(terminal_cell_t));
            }
        }
        
        // Clear the bottom line
        if (panel->screen[bottom]) {
            for (int col = 0; col < panel->screen_width; col++) {
                panel->screen[bottom][col].codepoint = ' ';
                panel->screen[bottom][col].fg_color = panel->fg_color;
                panel->screen[bottom][col].bg_color = panel->bg_color;
                panel->screen[bottom][col].attrs = panel->attrs;
            }
        }
    }
}

void terminal_insert_chars(terminal_panel_t *panel, int count) {
    if (count <= 0 || panel->cursor_y < 0 || panel->cursor_y >= panel->screen_height) return;
    if (!panel->screen || !panel->screen[panel->cursor_y]) return;
    
    int row = panel->cursor_y;
    int start_col = panel->cursor_x;
    
    // Shift characters to the right
    for (int col = panel->screen_width - 1; col >= start_col + count; col--) {
        if (col - count >= start_col) {
            panel->screen[row][col] = panel->screen[row][col - count];
        }
    }
    
    // Clear the inserted positions
    for (int col = start_col; col < start_col + count && col < panel->screen_width; col++) {
        panel->screen[row][col].codepoint = ' ';
        panel->screen[row][col].fg_color = panel->fg_color;
        panel->screen[row][col].bg_color = panel->bg_color;
        panel->screen[row][col].attrs = panel->attrs;
    }
}

void terminal_delete_chars(terminal_panel_t *panel, int count) {
    if (count <= 0 || panel->cursor_y < 0 || panel->cursor_y >= panel->screen_height) return;
    if (!panel->screen || !panel->screen[panel->cursor_y]) return;
    
    int row = panel->cursor_y;
    int start_col = panel->cursor_x;
    
    // Shift characters to the left
    for (int col = start_col; col < panel->screen_width - count; col++) {
        if (col + count < panel->screen_width) {
            panel->screen[row][col] = panel->screen[row][col + count];
        }
    }
    
    // Clear the end positions
    for (int col = panel->screen_width - count; col < panel->screen_width; col++) {
        if (col >= 0) {
            panel->screen[row][col].codepoint = ' ';
            panel->screen[row][col].fg_color = panel->fg_color;
            panel->screen[row][col].bg_color = panel->bg_color;
            panel->screen[row][col].attrs = panel->attrs;
        }
    }
}

// Cursor operations
void terminal_save_cursor(terminal_panel_t *panel) {
    panel->saved_cursor_x = panel->cursor_x;
    panel->saved_cursor_y = panel->cursor_y;
    panel->saved_fg_color = panel->fg_color;
    panel->saved_bg_color = panel->bg_color;
    panel->saved_attrs = panel->attrs;
}

void terminal_restore_cursor(terminal_panel_t *panel) {
    panel->cursor_x = panel->saved_cursor_x;
    panel->cursor_y = panel->saved_cursor_y;
    panel->fg_color = panel->saved_fg_color;
    panel->bg_color = panel->saved_bg_color;
    panel->attrs = panel->saved_attrs;
}

void terminal_set_cursor_visible(terminal_panel_t *panel, bool visible) {
    panel->modes.cursor_visible = visible;
}

// Tab operations
void terminal_set_tab_stop(terminal_panel_t *panel) {
    if (panel->cursor_x >= 0 && panel->cursor_x < 256) {
        panel->tab_stops[panel->cursor_x] = true;
    }
}

void terminal_clear_tab_stop(terminal_panel_t *panel, int mode) {
    switch (mode) {
        case 0: // Clear tab stop at current position
            if (panel->cursor_x >= 0 && panel->cursor_x < 256) {
                panel->tab_stops[panel->cursor_x] = false;
            }
            break;
        case 3: // Clear all tab stops
            memset(panel->tab_stops, 0, sizeof(panel->tab_stops));
            break;
    }
}

void terminal_tab_forward(terminal_panel_t *panel, int count) {
    for (int i = 0; i < count; i++) {
        int next_tab = panel->cursor_x + 1;
        
        // Find next tab stop
        while (next_tab < panel->screen_width && next_tab < 256 && !panel->tab_stops[next_tab]) {
            next_tab++;
        }
        
        if (next_tab < panel->screen_width) {
            panel->cursor_x = next_tab;
        } else {
            // If no tab stop found, go to end of line
            panel->cursor_x = panel->screen_width - 1;
            break;
        }
    }
}

void terminal_tab_backward(terminal_panel_t *panel, int count) {
    for (int i = 0; i < count; i++) {
        int prev_tab = panel->cursor_x - 1;
        while (prev_tab >= 0 && !panel->tab_stops[prev_tab]) {
            prev_tab--;
        }
        
        if (prev_tab >= 0) {
            panel->cursor_x = prev_tab;
        } else {
            panel->cursor_x = 0;
            break;
        }
    }
}

// Parser initialization
void vte_parser_init(vte_parser_t *parser) {
    memset(parser, 0, sizeof(vte_parser_t));
    parser->state = VTE_STATE_GROUND;
    vte_params_init(&parser->params);
}

// Internal helper functions
static void vte_reset_params(vte_parser_t *parser) {
    parser->intermediate_idx = 0;
    parser->ignoring = false;
    parser->current_param = 0;
    vte_params_clear(&parser->params);
}

static void vte_action_collect(vte_parser_t *parser, uint8_t byte) {
    if (parser->intermediate_idx >= VTE_MAX_INTERMEDIATES) {
        parser->ignoring = true;
    } else {
        parser->intermediates[parser->intermediate_idx++] = byte;
    }
}

static void vte_action_param(vte_parser_t *parser) {
    if (vte_params_is_full(&parser->params)) {
        parser->ignoring = true;
    } else {
        vte_params_push(&parser->params, parser->current_param);
        parser->current_param = 0;
    }
}

static void vte_action_subparam(vte_parser_t *parser) {
    if (vte_params_is_full(&parser->params)) {
        parser->ignoring = true;
    } else {
        vte_params_extend(&parser->params, parser->current_param);
        parser->current_param = 0;
    }
}

static void vte_action_paramnext(vte_parser_t *parser, uint8_t byte) {
    if (vte_params_is_full(&parser->params)) {
        parser->ignoring = true;
    } else {
        uint16_t digit = byte - '0';
        if (parser->current_param <= (UINT16_MAX - digit) / 10) {
            parser->current_param = parser->current_param * 10 + digit;
        } else {
            parser->current_param = UINT16_MAX;
        }
    }
}

static void vte_action_csi_dispatch(vte_parser_t *parser, terminal_panel_t *panel, uint8_t byte) {
    if (!vte_params_is_full(&parser->params)) {
        vte_params_push(&parser->params, parser->current_param);
    }
    
    if (panel->perform.csi_dispatch) {
        panel->perform.csi_dispatch(panel, &parser->params, parser->intermediates,
                                   parser->intermediate_idx, parser->ignoring, (char)byte);
    }
    
    parser->state = VTE_STATE_GROUND;
}

static void vte_action_esc_dispatch(vte_parser_t *parser, terminal_panel_t *panel, uint8_t byte) {
    if (panel->perform.esc_dispatch) {
        panel->perform.esc_dispatch(panel, parser->intermediates, parser->intermediate_idx,
                                   parser->ignoring, byte);
    }
    
    parser->state = VTE_STATE_GROUND;
}

static void vte_action_hook(vte_parser_t *parser, terminal_panel_t *panel, uint8_t byte) {
    if (!vte_params_is_full(&parser->params)) {
        vte_params_push(&parser->params, parser->current_param);
    }
    
    if (panel->perform.hook) {
        panel->perform.hook(panel, &parser->params, parser->intermediates,
                           parser->intermediate_idx, parser->ignoring, (char)byte);
    }
    
    parser->state = VTE_STATE_DCS_PASSTHROUGH;
}

static void vte_action_osc_put(vte_parser_t *parser, uint8_t byte) {
    if (parser->osc_raw_len < VTE_MAX_OSC_RAW) {
        parser->osc_raw[parser->osc_raw_len++] = byte;
    }
}

static void vte_action_osc_put_param(vte_parser_t *parser) {
    if (parser->osc_num_params >= VTE_MAX_OSC_PARAMS) return;
    
    size_t idx = parser->osc_raw_len;
    
    if (parser->osc_num_params == 0) {
        parser->osc_params[0].start = 0;
        parser->osc_params[0].end = idx;
    } else {
        size_t prev_end = parser->osc_params[parser->osc_num_params - 1].end;
        parser->osc_params[parser->osc_num_params].start = prev_end;
        parser->osc_params[parser->osc_num_params].end = idx;
    }
    
    parser->osc_num_params++;
}

static void vte_osc_end(vte_parser_t *parser, terminal_panel_t *panel, uint8_t byte) {
    vte_action_osc_put_param(parser);
    
    if (panel->perform.osc_dispatch && parser->osc_num_params > 0) {
        const uint8_t *params[VTE_MAX_OSC_PARAMS];
        size_t param_lens[VTE_MAX_OSC_PARAMS];
        
        for (size_t i = 0; i < parser->osc_num_params; i++) {
            params[i] = &parser->osc_raw[parser->osc_params[i].start];
            param_lens[i] = parser->osc_params[i].end - parser->osc_params[i].start;
        }
        
        panel->perform.osc_dispatch(panel, params, param_lens, parser->osc_num_params,
                                   byte == 0x07);
    }
    
    parser->osc_raw_len = 0;
    parser->osc_num_params = 0;
}

static void vte_anywhere(vte_parser_t *parser, terminal_panel_t *panel, uint8_t byte) {
    switch (byte) {
        case 0x18:
        case 0x1A:
            if (panel->perform.execute) {
                panel->perform.execute(panel, byte);
            }
            parser->state = VTE_STATE_GROUND;
            break;
        case 0x1B:
            vte_reset_params(parser);
            parser->state = VTE_STATE_ESCAPE;
            break;
    }
}

// State machine implementation
static void vte_advance_ground(vte_parser_t *parser, terminal_panel_t *panel, 
                              const uint8_t *bytes, size_t len, size_t *processed) {
    size_t i = 0;
    
    while (i < len) {
        uint8_t byte = bytes[i];
        
        if (byte == 0x1B) {
            vte_reset_params(parser);
            parser->state = VTE_STATE_ESCAPE;
            *processed = i + 1;
            return;
        }
        
        // Handle UTF-8 sequences
        if (byte >= 0x80) {
            size_t char_len = vte_utf8_char_len(byte);
            if (char_len == 0 || i + char_len > len) {
                // Invalid or incomplete UTF-8
                if (panel->perform.print) {
                    panel->perform.print(panel, 0xFFFD); // Replacement character
                }
                i++;
                continue;
            }
            
            uint32_t codepoint = vte_utf8_decode(&bytes[i], char_len);
            if (panel->perform.print) {
                panel->perform.print(panel, codepoint);
            }
            i += char_len;
        } else if (byte >= 0x20 && byte <= 0x7E) {
            // Printable ASCII
            if (panel->perform.print) {
                panel->perform.print(panel, byte);
            }
            i++;
        } else {
            // Control characters
            if (panel->perform.execute) {
                panel->perform.execute(panel, byte);
            }
            i++;
        }
    }
    
    *processed = len;
}

static void vte_advance_escape(vte_parser_t *parser, terminal_panel_t *panel, uint8_t byte) {
    switch (byte) {
        case 0x00 ... 0x17:
        case 0x19:
        case 0x1C ... 0x1F:
            if (panel->perform.execute) {
                panel->perform.execute(panel, byte);
            }
            break;
        case 0x20 ... 0x2F:
            vte_action_collect(parser, byte);
            parser->state = VTE_STATE_ESCAPE_INTERMEDIATE;
            break;
        case 0x30 ... 0x4F:
        case 0x51 ... 0x57:
        case 0x59 ... 0x5A:
        case 0x5C:
        case 0x60 ... 0x7E:
            vte_action_esc_dispatch(parser, panel, byte);
            break;
        case 0x50:
            vte_reset_params(parser);
            parser->state = VTE_STATE_DCS_ENTRY;
            break;
        case 0x58:
        case 0x5E:
        case 0x5F:
            parser->state = VTE_STATE_SOS_PM_APC_STRING;
            break;
        case 0x5B:
            vte_reset_params(parser);
            parser->state = VTE_STATE_CSI_ENTRY;
            break;
        case 0x5D:
            parser->osc_raw_len = 0;
            parser->osc_num_params = 0;
            parser->state = VTE_STATE_OSC_STRING;
            break;
        default:
            vte_anywhere(parser, panel, byte);
            break;
    }
}

static void vte_advance_csi_entry(vte_parser_t *parser, terminal_panel_t *panel, uint8_t byte) {
    switch (byte) {
        case 0x00 ... 0x17:
        case 0x19:
        case 0x1C ... 0x1F:
            if (panel->perform.execute) {
                panel->perform.execute(panel, byte);
            }
            break;
        case 0x20 ... 0x2F:
            vte_action_collect(parser, byte);
            parser->state = VTE_STATE_CSI_INTERMEDIATE;
            break;
        case 0x30 ... 0x39:
            vte_action_paramnext(parser, byte);
            parser->state = VTE_STATE_CSI_PARAM;
            break;
        case 0x3A:
            vte_action_subparam(parser);
            parser->state = VTE_STATE_CSI_PARAM;
            break;
        case 0x3B:
            vte_action_param(parser);
            parser->state = VTE_STATE_CSI_PARAM;
            break;
        case 0x3C ... 0x3F:
            vte_action_collect(parser, byte);
            parser->state = VTE_STATE_CSI_PARAM;
            break;
        case 0x40 ... 0x7E:
            vte_action_csi_dispatch(parser, panel, byte);
            break;
        default:
            vte_anywhere(parser, panel, byte);
            break;
    }
}

static void vte_advance_csi_param(vte_parser_t *parser, terminal_panel_t *panel, uint8_t byte) {
    switch (byte) {
        case 0x00 ... 0x17:
        case 0x19:
        case 0x1C ... 0x1F:
            if (panel->perform.execute) {
                panel->perform.execute(panel, byte);
            }
            break;
        case 0x20 ... 0x2F:
            vte_action_collect(parser, byte);
            parser->state = VTE_STATE_CSI_INTERMEDIATE;
            break;
        case 0x30 ... 0x39:
            vte_action_paramnext(parser, byte);
            break;
        case 0x3A:
            vte_action_subparam(parser);
            break;
        case 0x3B:
            vte_action_param(parser);
            break;
        case 0x3C ... 0x3F:
            parser->state = VTE_STATE_CSI_IGNORE;
            break;
        case 0x40 ... 0x7E:
            vte_action_csi_dispatch(parser, panel, byte);
            break;
        case 0x7F:
            break;
        default:
            vte_anywhere(parser, panel, byte);
            break;
    }
}

static void vte_advance_escape_intermediate(vte_parser_t *parser, terminal_panel_t *panel, uint8_t byte) {
    switch (byte) {
        case 0x00 ... 0x17:
        case 0x19:
        case 0x1C ... 0x1F:
            if (panel->perform.execute) {
                panel->perform.execute(panel, byte);
            }
            break;
        case 0x20 ... 0x2F:
            vte_action_collect(parser, byte);
            break;
        case 0x30 ... 0x7E:
            vte_action_esc_dispatch(parser, panel, byte);
            break;
        default:
            vte_anywhere(parser, panel, byte);
            break;
    }
}

static void vte_advance_csi_intermediate(vte_parser_t *parser, terminal_panel_t *panel, uint8_t byte) {
    switch (byte) {
        case 0x00 ... 0x17:
        case 0x19:
        case 0x1C ... 0x1F:
            if (panel->perform.execute) {
                panel->perform.execute(panel, byte);
            }
            break;
        case 0x20 ... 0x2F:
            vte_action_collect(parser, byte);
            break;
        case 0x40 ... 0x7E:
            vte_action_csi_dispatch(parser, panel, byte);
            break;
        case 0x7F:
            break;
        default:
            vte_anywhere(parser, panel, byte);
            break;
    }
}

static void vte_advance_csi_ignore(vte_parser_t *parser, terminal_panel_t *panel, uint8_t byte) {
    switch (byte) {
        case 0x00 ... 0x17:
        case 0x19:
        case 0x1C ... 0x1F:
            if (panel->perform.execute) {
                panel->perform.execute(panel, byte);
            }
            break;
        case 0x40 ... 0x7E:
            parser->state = VTE_STATE_GROUND;
            break;
        case 0x7F:
            break;
        default:
            vte_anywhere(parser, panel, byte);
            break;
    }
}

static void vte_advance_dcs_entry(vte_parser_t *parser, terminal_panel_t *panel, uint8_t byte) {
    switch (byte) {
        case 0x00 ... 0x17:
        case 0x19:
        case 0x1C ... 0x1F:
            break;
        case 0x20 ... 0x2F:
            vte_action_collect(parser, byte);
            parser->state = VTE_STATE_DCS_INTERMEDIATE;
            break;
        case 0x30 ... 0x39:
            vte_action_paramnext(parser, byte);
            parser->state = VTE_STATE_DCS_PARAM;
            break;
        case 0x3A:
            vte_action_subparam(parser);
            parser->state = VTE_STATE_DCS_PARAM;
            break;
        case 0x3B:
            vte_action_param(parser);
            parser->state = VTE_STATE_DCS_PARAM;
            break;
        case 0x3C ... 0x3F:
            vte_action_collect(parser, byte);
            parser->state = VTE_STATE_DCS_PARAM;
            break;
        case 0x40 ... 0x7E:
            vte_action_hook(parser, panel, byte);
            break;
        default:
            vte_anywhere(parser, panel, byte);
            break;
    }
}

static void vte_advance_dcs_param(vte_parser_t *parser, terminal_panel_t *panel, uint8_t byte) {
    switch (byte) {
        case 0x00 ... 0x17:
        case 0x19:
        case 0x1C ... 0x1F:
            break;
        case 0x20 ... 0x2F:
            vte_action_collect(parser, byte);
            parser->state = VTE_STATE_DCS_INTERMEDIATE;
            break;
        case 0x30 ... 0x39:
            vte_action_paramnext(parser, byte);
            break;
        case 0x3A:
            vte_action_subparam(parser);
            break;
        case 0x3B:
            vte_action_param(parser);
            break;
        case 0x3C ... 0x3F:
            parser->state = VTE_STATE_DCS_IGNORE;
            break;
        case 0x40 ... 0x7E:
            vte_action_hook(parser, panel, byte);
            break;
        case 0x7F:
            break;
        default:
            vte_anywhere(parser, panel, byte);
            break;
    }
}

static void vte_advance_dcs_intermediate(vte_parser_t *parser, terminal_panel_t *panel, uint8_t byte) {
    switch (byte) {
        case 0x00 ... 0x17:
        case 0x19:
        case 0x1C ... 0x1F:
            break;
        case 0x20 ... 0x2F:
            vte_action_collect(parser, byte);
            break;
        case 0x40 ... 0x7E:
            vte_action_hook(parser, panel, byte);
            break;
        case 0x7F:
            break;
        default:
            vte_anywhere(parser, panel, byte);
            break;
    }
}

static void vte_advance_dcs_passthrough(vte_parser_t *parser, terminal_panel_t *panel, uint8_t byte) {
    switch (byte) {
        case 0x00 ... 0x17:
        case 0x19:
        case 0x1C ... 0x1F:
            if (panel->perform.put) {
                panel->perform.put(panel, byte);
            }
            break;
        case 0x18:
        case 0x1A:
            if (panel->perform.unhook) {
                panel->perform.unhook(panel);
            }
            if (panel->perform.execute) {
                panel->perform.execute(panel, byte);
            }
            parser->state = VTE_STATE_GROUND;
            break;
        case 0x1B:
            if (panel->perform.unhook) {
                panel->perform.unhook(panel);
            }
            vte_reset_params(parser);
            parser->state = VTE_STATE_ESCAPE;
            break;
        case 0x20 ... 0x7E:
            if (panel->perform.put) {
                panel->perform.put(panel, byte);
            }
            break;
        case 0x7F:
            break;
        case 0x9C: // String Terminator
            if (panel->perform.unhook) {
                panel->perform.unhook(panel);
            }
            parser->state = VTE_STATE_GROUND;
            break;
        default:
            if (panel->perform.put) {
                panel->perform.put(panel, byte);
            }
            break;
    }
}

static void vte_advance_dcs_ignore(vte_parser_t *parser, terminal_panel_t *panel, uint8_t byte) {
    switch (byte) {
        case 0x00 ... 0x17:
        case 0x19:
        case 0x1C ... 0x1F:
            break;
        case 0x18:
        case 0x1A:
            if (panel->perform.execute) {
                panel->perform.execute(panel, byte);
            }
            parser->state = VTE_STATE_GROUND;
            break;
        case 0x1B:
            vte_reset_params(parser);
            parser->state = VTE_STATE_ESCAPE;
            break;
        case 0x20 ... 0x7F:
            break;
        case 0x9C: // String Terminator
            parser->state = VTE_STATE_GROUND;
            break;
        default:
            vte_anywhere(parser, panel, byte);
            break;
    }
}

static void vte_advance_sos_pm_apc_string(vte_parser_t *parser, terminal_panel_t *panel, uint8_t byte) {
    switch (byte) {
        case 0x00 ... 0x17:
        case 0x19:
        case 0x1C ... 0x1F:
            break;
        case 0x18:
        case 0x1A:
            if (panel->perform.execute) {
                panel->perform.execute(panel, byte);
            }
            parser->state = VTE_STATE_GROUND;
            break;
        case 0x1B:
            vte_reset_params(parser);
            parser->state = VTE_STATE_ESCAPE;
            break;
        case 0x20 ... 0x7F:
            break;
        case 0x9C: // String Terminator
            parser->state = VTE_STATE_GROUND;
            break;
        default:
            vte_anywhere(parser, panel, byte);
            break;
    }
}

static void vte_advance_osc_string(vte_parser_t *parser, terminal_panel_t *panel, uint8_t byte) {
    switch (byte) {
        case 0x00 ... 0x06:
        case 0x08 ... 0x17:
        case 0x19:
        case 0x1C ... 0x1F:
            break;
        case 0x07:
            vte_osc_end(parser, panel, byte);
            parser->state = VTE_STATE_GROUND;
            break;
        case 0x18:
        case 0x1A:
            vte_osc_end(parser, panel, byte);
            if (panel->perform.execute) {
                panel->perform.execute(panel, byte);
            }
            parser->state = VTE_STATE_GROUND;
            break;
        case 0x1B:
            vte_osc_end(parser, panel, byte);
            vte_reset_params(parser);
            parser->state = VTE_STATE_ESCAPE;
            break;
        case 0x3B:
            vte_action_osc_put_param(parser);
            break;
        default:
            vte_action_osc_put(parser, byte);
            break;
    }
}

// Main parser advance function
void vte_parser_advance(vte_parser_t *parser, terminal_panel_t *panel, 
                       const uint8_t *data, size_t len) {
    size_t i = 0;
    
    while (i < len) {
        if (parser->state == VTE_STATE_GROUND) {
            size_t processed = 0;
            vte_advance_ground(parser, panel, &data[i], len - i, &processed);
            i += processed;
        } else {
            uint8_t byte = data[i];
            
            switch (parser->state) {
                case VTE_STATE_ESCAPE:
                    vte_advance_escape(parser, panel, byte);
                    break;
                case VTE_STATE_ESCAPE_INTERMEDIATE:
                    vte_advance_escape_intermediate(parser, panel, byte);
                    break;
                case VTE_STATE_CSI_ENTRY:
                    vte_advance_csi_entry(parser, panel, byte);
                    break;
                case VTE_STATE_CSI_PARAM:
                    vte_advance_csi_param(parser, panel, byte);
                    break;
                case VTE_STATE_CSI_INTERMEDIATE:
                    vte_advance_csi_intermediate(parser, panel, byte);
                    break;
                case VTE_STATE_CSI_IGNORE:
                    vte_advance_csi_ignore(parser, panel, byte);
                    break;
                case VTE_STATE_DCS_ENTRY:
                    vte_advance_dcs_entry(parser, panel, byte);
                    break;
                case VTE_STATE_DCS_PARAM:
                    vte_advance_dcs_param(parser, panel, byte);
                    break;
                case VTE_STATE_DCS_INTERMEDIATE:
                    vte_advance_dcs_intermediate(parser, panel, byte);
                    break;
                case VTE_STATE_DCS_PASSTHROUGH:
                    vte_advance_dcs_passthrough(parser, panel, byte);
                    break;
                case VTE_STATE_DCS_IGNORE:
                    vte_advance_dcs_ignore(parser, panel, byte);
                    break;
                case VTE_STATE_OSC_STRING:
                    vte_advance_osc_string(parser, panel, byte);
                    break;
                case VTE_STATE_SOS_PM_APC_STRING:
                    vte_advance_sos_pm_apc_string(parser, panel, byte);
                    break;
                default:
                    vte_anywhere(parser, panel, byte);
                    break;
            }
            i++;
        }
    }
}

// Default perform implementation stubs
static void default_print(terminal_panel_t *panel, uint32_t codepoint) {
    // Default implementation - could be overridden
}

static void default_execute(terminal_panel_t *panel, uint8_t byte) {
    // Default implementation - could be overridden
}

static void default_csi_dispatch(terminal_panel_t *panel, const vte_params_t *params,
                                const uint8_t *intermediates, size_t intermediate_len,
                                bool ignore, char action) {
    // Default implementation - could be overridden
}

static void default_esc_dispatch(terminal_panel_t *panel, const uint8_t *intermediates,
                                size_t intermediate_len, bool ignore, uint8_t byte) {
    // Default implementation - could be overridden
}

static void default_osc_dispatch(terminal_panel_t *panel, const uint8_t *const *params,
                                const size_t *param_lens, size_t num_params, bool bell_terminated) {
    // Default implementation - could be overridden
}

static void default_hook(terminal_panel_t *panel, const vte_params_t *params,
                        const uint8_t *intermediates, size_t intermediate_len,
                        bool ignore, char action) {
    // Default implementation - could be overridden
}

static void default_put(terminal_panel_t *panel, uint8_t byte) {
    // Default implementation - could be overridden
}

static void default_unhook(terminal_panel_t *panel) {
    // Default implementation - could be overridden
}

// Enhanced CSI dispatch for terminal operations
void enhanced_csi_dispatch(terminal_panel_t *panel, const vte_params_t *params,
                           const uint8_t *intermediates, size_t intermediate_len,
                           bool ignore, char action) {
    if (ignore) return;
    
    switch (action) {
        case 'A': { // Cursor Up
            int count = vte_params_get_single(params, 0, 1);
            panel->cursor_y = (panel->cursor_y - count < 0) ? 0 : panel->cursor_y - count;
            break;
        }
        case 'B': { // Cursor Down
            int count = vte_params_get_single(params, 0, 1);
            panel->cursor_y = (panel->cursor_y + count >= panel->screen_height) ? 
                             panel->screen_height - 1 : panel->cursor_y + count;
            break;
        }
        case 'C': { // Cursor Forward
            int count = vte_params_get_single(params, 0, 1);
            panel->cursor_x = (panel->cursor_x + count >= panel->screen_width) ? 
                             panel->screen_width - 1 : panel->cursor_x + count;
            break;
        }
        case 'D': { // Cursor Backward
            int count = vte_params_get_single(params, 0, 1);
            panel->cursor_x = (panel->cursor_x - count < 0) ? 0 : panel->cursor_x - count;
            break;
        }
        case 'E': { // Cursor Next Line
            int count = vte_params_get_single(params, 0, 1);
            panel->cursor_y = (panel->cursor_y + count >= panel->screen_height) ? 
                             panel->screen_height - 1 : panel->cursor_y + count;
            panel->cursor_x = 0;
            break;
        }
        case 'F': { // Cursor Previous Line
            int count = vte_params_get_single(params, 0, 1);
            panel->cursor_y = (panel->cursor_y - count < 0) ? 0 : panel->cursor_y - count;
            panel->cursor_x = 0;
            break;
        }
        case 'G': { // Cursor Horizontal Absolute
            int col = vte_params_get_single(params, 0, 1) - 1;
            panel->cursor_x = (col < 0) ? 0 : (col >= panel->screen_width) ? 
                             panel->screen_width - 1 : col;
            break;
        }
        case 'H': case 'f': { // Cursor Position
            int row = vte_params_get_single(params, 0, 1) - 1;
            int col = vte_params_get_single(params, 1, 1) - 1;
            
            if (panel->modes.origin_mode) {
                row += panel->scroll_top;
                if (row > panel->scroll_bottom) row = panel->scroll_bottom;
            }
            
            panel->cursor_y = (row < 0) ? 0 : (row >= panel->screen_height) ? 
                             panel->screen_height - 1 : row;
            panel->cursor_x = (col < 0) ? 0 : (col >= panel->screen_width) ? 
                             panel->screen_width - 1 : col;
            break;
        }
        case 'I': { // Cursor Horizontal Tab
            int count = vte_params_get_single(params, 0, 1);
            terminal_tab_forward(panel, count);
            break;
        }
        case 'J': { // Erase in Display
            int mode = vte_params_get_single(params, 0, 0);
            terminal_clear_screen(panel, mode);
            break;
        }
        case 'K': { // Erase in Line
            int mode = vte_params_get_single(params, 0, 0);
            terminal_clear_line(panel, mode);
            break;
        }
        case 'L': { // Insert Lines
            int count = vte_params_get_single(params, 0, 1);
            terminal_insert_lines(panel, count);
            break;
        }
        case 'M': { // Delete Lines
            int count = vte_params_get_single(params, 0, 1);
            terminal_delete_lines(panel, count);
            break;
        }
        case 'P': { // Delete Characters
            int count = vte_params_get_single(params, 0, 1);
            terminal_delete_chars(panel, count);
            break;
        }
        case 'S': { // Scroll Up
            int count = vte_params_get_single(params, 0, 1);
            terminal_scroll_up(panel, count);
            break;
        }
        case 'T': { // Scroll Down
            int count = vte_params_get_single(params, 0, 1);
            terminal_scroll_down(panel, count);
            break;
        }
        case 'X': { // Erase Characters
            int count = vte_params_get_single(params, 0, 1);
            if (panel->cursor_y >= 0 && panel->cursor_y < panel->screen_height &&
                panel->screen && panel->screen[panel->cursor_y]) {
                for (int i = 0; i < count && panel->cursor_x + i < panel->screen_width; i++) {
                    panel->screen[panel->cursor_y][panel->cursor_x + i].codepoint = ' ';
                    panel->screen[panel->cursor_y][panel->cursor_x + i].fg_color = panel->fg_color;
                    panel->screen[panel->cursor_y][panel->cursor_x + i].bg_color = panel->bg_color;
                    panel->screen[panel->cursor_y][panel->cursor_x + i].attrs = panel->attrs;
                }
            }
            break;
        }
        case 'Z': { // Cursor Backward Tab
            int count = vte_params_get_single(params, 0, 1);
            terminal_tab_backward(panel, count);
            break;
        }
        case '@': { // Insert Characters
            int count = vte_params_get_single(params, 0, 1);
            terminal_insert_chars(panel, count);
            break;
        }
        case 'd': { // Line Position Absolute
            int row = vte_params_get_single(params, 0, 1) - 1;
            if (panel->modes.origin_mode) {
                row += panel->scroll_top;
                if (row > panel->scroll_bottom) row = panel->scroll_bottom;
            }
            panel->cursor_y = (row < 0) ? 0 : (row >= panel->screen_height) ? 
                             panel->screen_height - 1 : row;
            break;
        }
        case 'g': { // Tab Clear
            int mode = vte_params_get_single(params, 0, 0);
            terminal_clear_tab_stop(panel, mode);
            break;
        }
        case 'h': { // Set Mode
            for (size_t i = 0; i < vte_params_len(params); i++) {
                uint16_t mode = vte_params_get_single(params, i, 0);
                
                if (intermediate_len > 0 && intermediates[0] == '?') {
                    // DEC Private Mode Set
                    switch (mode) {
                        case 1: panel->modes.application_cursor_keys = true; break;
                        case 6: panel->modes.origin_mode = true; break;
                        case 7: panel->modes.auto_wrap = true; break;
                        case 25: panel->modes.cursor_visible = true; break;
                        case 2004: panel->modes.bracketed_paste = true; break;
                    }
                } else {
                    // ANSI Mode Set
                    switch (mode) {
                        case 4: panel->modes.insert_mode = true; break;
                        case 12: panel->modes.local_echo = false; break;
                        case 20: panel->modes.auto_wrap = true; break;
                    }
                }
            }
            break;
        }
        case 'l': { // Reset Mode
            for (size_t i = 0; i < vte_params_len(params); i++) {
                uint16_t mode = vte_params_get_single(params, i, 0);
                
                if (intermediate_len > 0 && intermediates[0] == '?') {
                    // DEC Private Mode Reset
                    switch (mode) {
                        case 1: panel->modes.application_cursor_keys = false; break;
                        case 6: panel->modes.origin_mode = false; break;
                        case 7: panel->modes.auto_wrap = false; break;
                        case 25: panel->modes.cursor_visible = false; break;
                        case 2004: panel->modes.bracketed_paste = false; break;
                    }
                } else {
                    // ANSI Mode Reset
                    switch (mode) {
                        case 4: panel->modes.insert_mode = false; break;
                        case 12: panel->modes.local_echo = true; break;
                        case 20: panel->modes.auto_wrap = false; break;
                    }
                }
            }
            break;
        }
        case 'm': { // Select Graphic Rendition (SGR)
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
                    case 1: panel->attrs |= 1; break;  // Bold
                    case 2: panel->attrs |= 8; break;  // Dim
                    case 3: panel->attrs |= 16; break; // Italic
                    case 4: panel->attrs |= 2; break;  // Underline
                    case 5: panel->attrs |= 32; break; // Blink
                    case 7: panel->attrs |= 4; break;  // Reverse
                    case 8: panel->attrs |= 64; break; // Hidden
                    case 9: panel->attrs |= 128; break; // Strikethrough
                    case 22: panel->attrs &= ~(1|8); break; // No bold/dim
                    case 23: panel->attrs &= ~16; break; // No italic
                    case 24: panel->attrs &= ~2; break;  // No underline
                    case 25: panel->attrs &= ~32; break; // No blink
                    case 27: panel->attrs &= ~4; break;  // No reverse
                    case 28: panel->attrs &= ~64; break; // No hidden
                    case 29: panel->attrs &= ~128; break; // No strikethrough
                    case 30: case 31: case 32: case 33:
                    case 34: case 35: case 36: case 37:
                        panel->fg_color = param - 30;
                        break;
                    case 38: { // Extended foreground color
                        if (i + 1 < vte_params_len(params)) {
                            uint16_t color_mode = vte_params_get_single(params, i + 1, 0);
                            if (color_mode == 5 && i + 2 < vte_params_len(params)) {
                                // 256-color mode
                                panel->fg_color = vte_params_get_single(params, i + 2, 0);
                                i += 2;
                            } else if (color_mode == 2 && i + 4 < vte_params_len(params)) {
                                // RGB mode (simplified to closest 8-bit color)
                                uint16_t r = vte_params_get_single(params, i + 2, 0);
                                uint16_t g = vte_params_get_single(params, i + 3, 0);
                                uint16_t b = vte_params_get_single(params, i + 4, 0);
                                // Simple RGB to 8-color mapping
                                panel->fg_color = ((r > 127) ? 1 : 0) | 
                                                 ((g > 127) ? 2 : 0) | 
                                                 ((b > 127) ? 4 : 0);
                                i += 4;
                            }
                        }
                        break;
                    }
                    case 39: panel->fg_color = -1; break; // Default foreground
                    case 40: case 41: case 42: case 43:
                    case 44: case 45: case 46: case 47:
                        panel->bg_color = param - 40;
                        break;
                    case 48: { // Extended background color
                        if (i + 1 < vte_params_len(params)) {
                            uint16_t color_mode = vte_params_get_single(params, i + 1, 0);
                            if (color_mode == 5 && i + 2 < vte_params_len(params)) {
                                // 256-color mode
                                panel->bg_color = vte_params_get_single(params, i + 2, 0);
                                i += 2;
                            } else if (color_mode == 2 && i + 4 < vte_params_len(params)) {
                                // RGB mode (simplified to closest 8-bit color)
                                uint16_t r = vte_params_get_single(params, i + 2, 0);
                                uint16_t g = vte_params_get_single(params, i + 3, 0);
                                uint16_t b = vte_params_get_single(params, i + 4, 0);
                                panel->bg_color = ((r > 127) ? 1 : 0) | 
                                                 ((g > 127) ? 2 : 0) | 
                                                 ((b > 127) ? 4 : 0);
                                i += 4;
                            }
                        }
                        break;
                    }
                    case 49: panel->bg_color = -1; break; // Default background
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
            break;
        }
        case 'r': { // Set Scrolling Region
            int top = vte_params_get_single(params, 0, 1) - 1;
            int bottom = vte_params_get_single(params, 1, panel->screen_height) - 1;
            
            if (top >= 0 && bottom < panel->screen_height && top < bottom) {
                panel->scroll_top = top;
                panel->scroll_bottom = bottom;
                // Move cursor to home position in scrolling region
                panel->cursor_x = 0;
                panel->cursor_y = panel->modes.origin_mode ? top : 0;
            }
            break;
        }
        case 's': { // Save Cursor Position
            terminal_save_cursor(panel);
            break;
        }
        case 'u': { // Restore Cursor Position
            terminal_restore_cursor(panel);
            break;
        }
    }
}

// Enhanced ESC dispatch for character sets and other sequences
void enhanced_esc_dispatch(terminal_panel_t *panel, const uint8_t *intermediates,
                          size_t intermediate_len, bool ignore, uint8_t byte) {
    if (ignore) return;
    
    switch (byte) {
        case '7': // Save Cursor (DECSC)
            terminal_save_cursor(panel);
            break;
        case '8': // Restore Cursor (DECRC)
            terminal_restore_cursor(panel);
            break;
        case 'c': // Reset to Initial State (RIS)
            terminal_panel_reset(panel);
            break;
        case 'D': // Index (IND) - move cursor down, scroll if needed
            if (panel->cursor_y >= panel->scroll_bottom) {
                terminal_scroll_up(panel, 1);
            } else {
                panel->cursor_y++;
            }
            break;
        case 'E': // Next Line (NEL)
            panel->cursor_x = 0;
            if (panel->cursor_y >= panel->scroll_bottom) {
                terminal_scroll_up(panel, 1);
            } else {
                panel->cursor_y++;
            }
            break;
        case 'H': // Tab Set (HTS)
            terminal_set_tab_stop(panel);
            break;
        case 'M': // Reverse Index (RI) - move cursor up, scroll if needed
            if (panel->cursor_y <= panel->scroll_top) {
                terminal_scroll_down(panel, 1);
            } else {
                panel->cursor_y--;
            }
            break;
        case 'Z': // Identify Terminal (DECID)
            // Could send back terminal identification string
            break;
        case '=': // Application Keypad (DECKPAM)
            panel->modes.application_keypad = true;
            break;
        case '>': // Normal Keypad (DECKPNM)
            panel->modes.application_keypad = false;
            break;
        case 'N': // Single Shift 2 (SS2) - use G2 for next character
        case 'O': // Single Shift 3 (SS3) - use G3 for next character
            // Not fully implemented yet
            break;
        default:
            // Character set designation
            if (intermediate_len == 1) {
                charset_t charset = CHARSET_ASCII;
                
                switch (byte) {
                    case '0': charset = CHARSET_DEC_SPECIAL; break;
                    case 'A': charset = CHARSET_UK; break;
                    case 'B': charset = CHARSET_ASCII; break;
                    case '4': charset = CHARSET_DUTCH; break;
                    case '5': charset = CHARSET_FINNISH; break;
                    case 'C': case 'R': charset = CHARSET_FINNISH; break;
                    case 'Q': charset = CHARSET_FRENCH_CANADIAN; break;
                    case 'K': charset = CHARSET_GERMAN; break;
                    case 'Y': charset = CHARSET_ITALIAN; break;
                    case 'E': case '6': charset = CHARSET_NORWEGIAN_DANISH; break;
                    case 'Z': charset = CHARSET_SPANISH; break;
                    case '7': case 'H': charset = CHARSET_SWEDISH; break;
                    case '=': charset = CHARSET_SWISS; break;
                }
                
                switch (intermediates[0]) {
                    case '(': panel->g0_charset = charset; break;
                    case ')': panel->g1_charset = charset; break;
                    case '*': /* G2 charset */ break;
                    case '+': /* G3 charset */ break;
                }
            }
            break;
    }
}

// Enhanced print function with character set mapping
void enhanced_print(terminal_panel_t *panel, uint32_t codepoint) {
    // Apply character set mapping for ASCII range
    if (codepoint >= 0x20 && codepoint <= 0x7E) {
        charset_t active_charset = panel->using_g1 ? panel->g1_charset : panel->g0_charset;
        codepoint = map_charset_char(active_charset, (uint8_t)codepoint);
    }
    
    if (panel->cursor_y >= 0 && panel->cursor_y < panel->screen_height &&
        panel->cursor_x >= 0 && panel->cursor_x < panel->screen_width &&
        panel->screen && panel->screen[panel->cursor_y]) {
        
        // Handle insert mode
        if (panel->modes.insert_mode) {
            terminal_insert_chars(panel, 1);
        }
        
        terminal_cell_t *cell = &panel->screen[panel->cursor_y][panel->cursor_x];
        cell->codepoint = codepoint;
        cell->fg_color = panel->fg_color;
        cell->bg_color = panel->bg_color;
        cell->attrs = panel->attrs;
        
        panel->cursor_x++;
        
        // Handle auto-wrap
        if (panel->cursor_x >= panel->screen_width) {
            if (panel->modes.auto_wrap) {
                panel->cursor_x = 0;
                panel->cursor_y++;
                
                // Scroll if we've gone past the bottom
                if (panel->cursor_y > panel->scroll_bottom) {
                    panel->cursor_y = panel->scroll_bottom;
                    terminal_scroll_up(panel, 1);
                }
            } else {
                panel->cursor_x = panel->screen_width - 1;
            }
        }
    }
}

// Enhanced execute function for control characters
void enhanced_execute(terminal_panel_t *panel, uint8_t byte) {
    switch (byte) {
        case 0x07: // Bell (BEL)
            // Could trigger visual or audio bell
            break;
        case 0x08: // Backspace (BS)
            if (panel->cursor_x > 0) {
                panel->cursor_x--;
            }
            break;
        case 0x09: // Horizontal Tab (HT)
            terminal_tab_forward(panel, 1);
            break;
        case 0x0A: // Line Feed (LF)
            panel->cursor_x = 0;  // Move to beginning of line
            if (panel->cursor_y >= panel->scroll_bottom) {
                terminal_scroll_up(panel, 1);
            } else {
                panel->cursor_y++;
            }
            break;
        case 0x0B: // Vertical Tab (VT)
        case 0x0C: // Form Feed (FF)
            // Treat as line feed
            panel->cursor_x = 0;  // Move to beginning of line
            if (panel->cursor_y >= panel->scroll_bottom) {
                terminal_scroll_up(panel, 1);
            } else {
                panel->cursor_y++;
            }
            break;
        case 0x0D: // Carriage Return (CR)
            panel->cursor_x = 0;
            break;
        case 0x0E: // Shift Out (SO) - use G1 character set
            panel->using_g1 = true;
            break;
        case 0x0F: // Shift In (SI) - use G0 character set
            panel->using_g1 = false;
            break;
        case 0x84: // Index (IND)
            if (panel->cursor_y >= panel->scroll_bottom) {
                terminal_scroll_up(panel, 1);
            } else {
                panel->cursor_y++;
            }
            break;
        case 0x85: // Next Line (NEL)
            panel->cursor_x = 0;
            if (panel->cursor_y >= panel->scroll_bottom) {
                terminal_scroll_up(panel, 1);
            } else {
                panel->cursor_y++;
            }
            break;
        case 0x88: // Tab Set (HTS)
            terminal_set_tab_stop(panel);
            break;
        case 0x8D: // Reverse Index (RI)
            if (panel->cursor_y <= panel->scroll_top) {
                terminal_scroll_down(panel, 1);
            } else {
                panel->cursor_y--;
            }
            break;
    }
}

// Color mapping
int ansi_to_ncurses_color(int ansi_color) {
    switch (ansi_color) {
        case 0: return COLOR_BLACK;
        case 1: return COLOR_RED;
        case 2: return COLOR_GREEN;
        case 3: return COLOR_YELLOW;
        case 4: return COLOR_BLUE;
        case 5: return COLOR_MAGENTA;
        case 6: return COLOR_CYAN;
        case 7: return COLOR_WHITE;
        default: return -1;
    }
}

// Enhanced perform implementation
const vte_perform_t enhanced_perform = {
    .print = enhanced_print,
    .execute = enhanced_execute,
    .csi_dispatch = enhanced_csi_dispatch,
    .esc_dispatch = enhanced_esc_dispatch,
    .osc_dispatch = NULL,  // Could be implemented for window title, etc.
    .hook = NULL,
    .put = NULL,
    .unhook = NULL
};