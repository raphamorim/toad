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
                case VTE_STATE_CSI_ENTRY:
                    vte_advance_csi_entry(parser, panel, byte);
                    break;
                case VTE_STATE_CSI_PARAM:
                    vte_advance_csi_param(parser, panel, byte);
                    break;
                case VTE_STATE_OSC_STRING:
                    vte_advance_osc_string(parser, panel, byte);
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

const vte_perform_t vte_default_perform = {
    .print = default_print,
    .execute = default_execute,
    .csi_dispatch = default_csi_dispatch,
    .esc_dispatch = default_esc_dispatch,
    .osc_dispatch = default_osc_dispatch,
    .hook = default_hook,
    .put = default_put,
    .unhook = default_unhook
};