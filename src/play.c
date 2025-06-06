#include <ncurses.h>
#include <stdlib.h>
#include <unistd.h>

// Frog pixel art using quadblocks (each character represents 2x2 pixels)
// Colors: 0=black, 1=dark green, 2=green, 3=light green, 4=white, 5=red
const char frog_art[] = {
    "    0000000000    "
    "  001122221100  "
    "  0122333322110  "
    " 012233443332210 "
    " 012234443432210 "
    "0122344554432210"
    "0122345554532210"
    "0122345554532210"
    "0122344554432210"
    " 012234443432210 "
    " 012233443332210 "
    "  0122333322110  "
    "  001122221100  "
    "    0000000000    "
};

const int frog_width = 16;
const int frog_height = 14;

// Quadblock characters for different combinations
const char* quadblocks[] = {
    " ",  // 0000
    "▗",  // 0001
    "▖",  // 0010
    "▄",  // 0011
    "▝",  // 0100
    "▐",  // 0101
    "▞",  // 0110
    "▟",  // 0111
    "▘",  // 1000
    "▚",  // 1001
    "▌",  // 1010
    "▙",  // 1011
    "▀",  // 1100
    "▜",  // 1101
    "▛",  // 1110
    "█"   // 1111
};

void init_colors() {
    start_color();
    init_pair(1, COLOR_BLACK, COLOR_BLACK);     // black
    init_pair(2, COLOR_GREEN, COLOR_BLACK);     // dark green
    init_pair(3, COLOR_GREEN, COLOR_BLACK);     // green
    init_pair(4, COLOR_YELLOW, COLOR_BLACK);    // light green
    init_pair(5, COLOR_WHITE, COLOR_BLACK);     // white
    init_pair(6, COLOR_RED, COLOR_BLACK);       // red
}

int get_pixel(int x, int y) {
    if (x < 0 || x >= frog_width || y < 0 || y >= frog_height) {
        return 0;
    }
    return frog_art[y * frog_width + x] - '0';
}

void draw_frog(int start_x, int start_y) {
    for (int y = 0; y < frog_height; y += 2) {
        for (int x = 0; x < frog_width; x += 2) {
            // Get 2x2 pixel block
            int tl = get_pixel(x, y);       // top-left
            int tr = get_pixel(x+1, y);     // top-right
            int bl = get_pixel(x, y+1);     // bottom-left
            int br = get_pixel(x+1, y+1);   // bottom-right

            // Convert to quadblock index
            int block_index = (tl > 0 ? 8 : 0) +
                             (tr > 0 ? 4 : 0) +
                             (bl > 0 ? 2 : 0) +
                             (br > 0 ? 1 : 0);

            // Choose color based on dominant pixel
            int color = 1; // default black
            if (tl > 0 || tr > 0 || bl > 0 || br > 0) {
                int max_color = 0;
                if (tl > max_color) max_color = tl;
                if (tr > max_color) max_color = tr;
                if (bl > max_color) max_color = bl;
                if (br > max_color) max_color = br;
                color = max_color + 1;
            }

            // Draw the quadblock character
            attron(COLOR_PAIR(color));
            mvprintw(start_y + y/2, start_x + x/2, "%s", quadblocks[block_index]);
            attroff(COLOR_PAIR(color));
        }
    }
}

int main() {
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    timeout(100);

    if (has_colors()) {
        init_colors();
    }

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    // Center the frog
    int start_x = (max_x - frog_width/2) / 2;
    int start_y = (max_y - frog_height/2) / 2;

    while (1) {
        clear();

        // Draw title
        mvprintw(1, (max_x - 12) / 2, "TOAD 🐸");
        mvprintw(2, (max_x - 30) / 2, "Jump, says \"qwark-qwark-qwark\"");

        // Draw the frog
        draw_frog(start_x, start_y);

        // Instructions
        mvprintw(max_y - 2, (max_x - 20) / 2, "Press 'q' to quit");

        refresh();

        int ch = getch();
        if (ch == 'q' || ch == 'Q') {
            break;
        }
    }

    endwin();
    return 0;
}
