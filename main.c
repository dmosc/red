/*** includes ***/
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** definitions ***/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE
#define TAB_STOP 4
#define CTRL_KEY(k) ((k) & 0x1f)
#define BUFFER_INIT {NULL, 0}
enum {
    UP = 1000,
    DOWN,
    RIGHT,
    LEFT,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/*** structs ***/
typedef struct document_row {
    char *content, *render_content;
    int size, render_size;
} document_row;

struct editor_config {
    int cursor_x, cursor_y;
    int rows, cols;
    int row_offset, col_offset;
    int document_rows;
    document_row *row;
    char *file_name;
    char status[80];
    time_t status_time;
    struct termios initial_state;
};

struct editor_config EC;

/*** utils ***/
void clear_and_reposition_cursor() {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
}

/*** Buffer printer ***/
struct buffer {
    char *content;
    int size;
};

void buffer_append(struct buffer *buff, const char *s, int size) {
    char *new_content = realloc(buff->content, buff->size + size);

    if (!new_content) return;
    memcpy(&new_content[buff->size], s, size);
    buff->content = new_content;
    buff->size += size;
}

void buffer_drop(struct buffer *buff) {
    free(buff->content);
}

/*** functions ***/
void editor_exit(const char *s) {
    clear_and_reposition_cursor();

    perror(s);
    exit(1);
}

void editor_disable() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &EC.initial_state) == -1) editor_exit("tcsetattr");
}

void editor_enable() {
    if (tcgetattr(STDIN_FILENO, &EC.initial_state) == -1) editor_exit("tcgetattr");
    atexit(editor_disable);

    struct termios raw = EC.initial_state;

    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) editor_exit("tcsetattr");
}

int read_key() {
    int read_no;
    char c;

    while ((read_no = read(STDIN_FILENO, &c, 1)) != 1) {
        if (read_no == -1 && errno != EAGAIN) editor_exit("read");
    }

    // Process arrow key sequence to determine cursor direction.
    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1':
                            return HOME_KEY;
                        case '3':
                            return DEL_KEY;
                        case '4':
                            return END_KEY;
                        case '5':
                            return PAGE_UP;
                        case '6':
                            return PAGE_DOWN;
                        case '7':
                            return HOME_KEY;
                        case '8':
                            return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A':
                        return UP;
                    case 'B':
                        return DOWN;
                    case 'C':
                        return RIGHT;
                    case 'D':
                        return LEFT;
                    case 'H':
                        return HOME_KEY;
                    case 'F':
                        return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
            }
        }

        return '\x1b';
    } else {
        return c;
    }
}

int cursor_position(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }

    buf[i] = '\0';
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int window_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        return cursor_position(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

void editor_init() {
    EC.cursor_x = 0;
    EC.cursor_y = 0;
    EC.row_offset = 0;
    EC.col_offset = 0;
    EC.document_rows = 0;
    EC.row = NULL;
    EC.file_name = NULL;
    EC.status[0] = '\0';
    EC.status_time = 0;

    if (window_size(&EC.rows, &EC.cols) == -1) editor_exit("window_size");
    EC.rows -= 2; // Leave space for status bar and status messages.
}

/*** input functions ***/
void move_cursor(int key) {
    document_row *row = EC.cursor_y >= EC.rows ? NULL : &EC.row[EC.cursor_y];
    switch (key) {
        case UP:
            if (EC.cursor_y != 0) --EC.cursor_y;
            break;
        case DOWN:
            if (EC.cursor_y < EC.document_rows) ++EC.cursor_y;
            break;
        case RIGHT:
            if (row && EC.cursor_x < row->size) {
                ++EC.cursor_x;
            } else if (row && EC.cursor_x == row->size) {
                ++EC.cursor_y;
                EC.cursor_x = 0;
            }
            break;
        case LEFT:
            if (EC.cursor_x != 0) {
                --EC.cursor_x;
            } else if (EC.cursor_y > 0) {
                --EC.cursor_y;
                EC.cursor_x = EC.row[EC.cursor_y].size;
            }
            break;
    }

    row = (EC.cursor_y >= EC.document_rows) ? NULL : &EC.row[EC.cursor_y];
    int size = row ? row->size : 0;
    if (EC.cursor_x > size) {
        EC.cursor_x = size;
    }
}

void process_key() {
    int c = read_key();
    switch (c) {
        case CTRL_KEY('q'):
            clear_and_reposition_cursor();
            exit(0);
        case HOME_KEY:
            EC.cursor_x = 0;
            break;
        case END_KEY:
            if (EC.cursor_y < EC.document_rows) EC.cursor_x = EC.row[EC.cursor_y].size;
            break;
        case PAGE_UP:
        case PAGE_DOWN: {
            if (c == PAGE_UP) {
                EC.cursor_y = EC.row_offset;
            } else {
                EC.cursor_y = EC.row_offset + EC.document_rows - 1;
                if (EC.cursor_y > EC.document_rows) EC.cursor_y = EC.document_rows;
            }

            int times = EC.rows;
            while (times--)
                move_cursor(c == PAGE_UP ? UP : DOWN);
            break;
        }
        case UP:
        case DOWN:
        case RIGHT:
        case LEFT:
            move_cursor(c);
            break;
    }
}

/*** output functions ***/
void scroll_window() {
    if (EC.cursor_y < EC.row_offset) // Check if cursor above visible window and scroll up
        EC.row_offset = EC.cursor_y;
    if (EC.cursor_y >= EC.row_offset + EC.rows) // Check if cursor below visible window and scroll down
        EC.row_offset = EC.cursor_y - EC.rows + 1;
    if (EC.cursor_x < EC.col_offset)
        EC.col_offset = EC.cursor_x;
    if (EC.cursor_x >= EC.col_offset + EC.cols)
        EC.col_offset = EC.cursor_x - EC.cols + 1;
}

void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(EC.status, sizeof(EC.status), fmt, ap);
    va_end(ap);
    EC.status_time = time(NULL);
}

void draw_rows(struct buffer *buff) {
    for (int r = 0; r < EC.rows; ++r) {
        int i = r + EC.row_offset;
        if (i >= EC.document_rows) {
            buffer_append(buff, "~", 1);
        } else {
            int size = EC.row[i].render_size - EC.col_offset;
            if (size < 0) size = 0;
            if (size > EC.cols) size = EC.cols;
            buffer_append(buff, &EC.row[i].render_content[EC.col_offset], size);
        }

        buffer_append(buff, "\x1b[K", 3);
        buffer_append(buff, "\r\n", 2);
    }
}

void draw_message_bar(struct buffer *buff) {
    buffer_append(buff, "\x1b[K", 3);
    int size = strlen(EC.status);
    if (size > EC.cols) size = EC.cols;
    if (size && time(NULL) - EC.status_time < 5)
        buffer_append(buff, EC.status, size);
}

void draw_status_bar(struct buffer *buff) {
    buffer_append(buff, "\x1b[7m", 4); // Invert colors

    char status[80];
    int cols = snprintf(
            status,
            sizeof(status),
            " %.20s - %d lines",
            EC.file_name ? EC.file_name : "[New document]",
            EC.rows
    );

    if (cols > EC.cols) cols = EC.cols;
    buffer_append(buff, status, cols);
    while (++cols < EC.cols)
        buffer_append(buff, " ", 1);
    buffer_append(buff, "\x1b[m", 3);
    buffer_append(buff, "\r\n", 2);
}

void refresh_screen() {
    scroll_window();
    struct buffer buff = BUFFER_INIT;

    buffer_append(&buff, "\x1b[?25l", 6);
    buffer_append(&buff, "\x1b[H", 3);

    draw_rows(&buff);
    draw_status_bar(&buff);
    draw_message_bar(&buff);

    char cursor_buff[32];
    snprintf(cursor_buff, sizeof(cursor_buff), "\x1b[%d;%dH", (EC.cursor_y - EC.row_offset) + 1,
             (EC.cursor_x - EC.col_offset) + 1);
    buffer_append(&buff, cursor_buff, strlen(cursor_buff));
    buffer_append(&buff, "\x1b[?25h", 6);

    write(STDOUT_FILENO, buff.content, buff.size);
    buffer_drop(&buff);
}

/*** file functions ***/
void row_append_render(document_row *row) {
    int i, tabs = 0;

    for (i = 0; i < row->size; ++i)
        tabs += row->content[i] == '\t' ? 1 : 0;

    free(row->render_content);
    row->render_content = malloc(row->size + tabs * (TAB_STOP - 1) + 1);

    int render_size = 0;
    for (i = 0; i < row->size; ++i) {
        if (row->content[i] == '\t') {
            row->render_content[render_size++] = ' ';
            while (render_size % TAB_STOP != 0) row->render_content[render_size++] = ' ';
        } else {
            row->render_content[render_size++] = row->content[i];
        }
    }

    row->render_content[render_size] = '\0';
    row->render_size = render_size;
}

void row_append(char *line, size_t size) {
    EC.row = realloc(EC.row, sizeof(document_row) * (EC.document_rows + 1));

    int i = EC.document_rows;
    EC.row[i].size = size;
    EC.row[i].content = malloc(size + 1);
    memcpy(EC.row[i].content, line, size);
    EC.row[i].content[size] = '\0';
    EC.row[i].render_size = 0;
    EC.row[i].render_content = NULL;

    row_append_render(&EC.row[i]);
    ++EC.document_rows;
}

void open_file(char *file_name) {
    free(EC.file_name);
    EC.file_name = strdup(file_name);

    FILE *file = fopen(file_name, "r");
    if (!file) editor_exit("fopen");

    char *line = NULL;
    size_t capacity = 0;
    ssize_t size;
    while ((size = getline(&line, &capacity, file)) != -1) {
        while (size > 0 && (line[size - 1] == '\n' || line[size - 1] == '\r'))
            size--;
        row_append(line, size);
    }

    free(line);
    fclose(file);
}

/*** init ***/
int main(int argc, char *argv[]) {
    editor_enable();
    editor_init();
    if (argc >= 2) {
        open_file(argv[1]);
    }

    set_status("Ctrl-Q = Quit");

    while (1) {
        refresh_screen();
        process_key();
    }

    return 0;
}
