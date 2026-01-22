#define _POSIX_C_SOURCE 200809L
/*** includes ***/

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <termios.h>

#include <sys/ioctl.h>

/*** defines ***/

#define EDITOR_NAME "Kilo"
#define EDITOR_VERSION "0.0.1"
#define EDITOR_TAB "    "
#define EDITOR_TAB_LEN (sizeof(EDITOR_TAB) - 1)
#define EDITOR_MSG_LEN 128
#define EDITOR_QUIT_CONFIRM 3

#define CTRL_KEY(key) ((key) & 0x1F)

#define VT100_CLEAR_SCREEN "\x1b[2J"
#define VT100_CLEAR_LINE "\x1b[K"
#define VT100_SET_CURSOR_POS "\x1b[%d;%dH"
#define VT100_GET_CURSOR_POS "\x1b[6n"
#define VT100_CURSOR_DOWN "\x1b[%dB"
#define VT100_CURSOR_RIGHT "\x1b[%dC"
#define VT100_CURSOR_HIDE "\x1b[?25l" // ?25 option (cursor visibility) is supported in later
#define VT100_CURSOR_SHOW "\x1b[?25h" // VT versions, so it will not appear in VT100 docs
#define VT100_INVERT_COLOR "\x1b[7m"
#define VT100_DEFAULT_COLOR "\x1b[m"

enum EditorKey {
    BACKSPACE = 127,
	ARROW_LEFT = 1000,
	ARROW_DOWN,
	ARROW_UP,
	ARROW_RIGHT,
    DELETE,
    HOME,
    END,
	PAGE_UP,
	PAGE_DOWN,
};

/*** append buffer ***/

struct String {
	char* buf;
	int len;
};

#define STR_INIT { NULL, 0 }

void StringAppend(struct String* str, const char* data, int len) {
	if (len <= 0) return;
	char* buf = realloc(str->buf, str->len + len);
	if (buf == NULL) return;
	
	memcpy(&buf[str->len], data, len);
	str->buf = buf;
	str->len += len;
}

void StringFree(struct String* str) {
	free(str->buf);
    str->len = 0;
}

void StringTruncate(struct String* str, int len) {
    str->len = len;
}

/*** data ***/

int startTime = 0;
#define GET_TIME (time(NULL) - startTime)

struct EditorLine {
    struct String str;
    struct String render;
};

struct EditorConfig {
	int x, y;
    int renderOffset;
	int rows, cols;
    int rowOffset, colOffset;
    int lines;
    struct EditorLine* line;
    int dirty;
    char* fileName;
    char msg[EDITOR_MSG_LEN];
    time_t msgTime;
	struct termios originalTerminal;
};
struct EditorConfig config = { 0 };

/*** prototypes ***/

void EditorSetMessage(const char* fmt, ...);
char* EditorPrompt(char* prompt, void (*callback)(char*, int));

/*** terminal ***/

void TerminalSetCursor(struct String* term, int row, int col) {
	char cmd[16] = { 0 };
	int length = snprintf(cmd, 16, VT100_SET_CURSOR_POS, row, col);
	StringAppend(term, cmd, length);
}

void TerminalMoveCursorDown(struct String* term, int rows) {
	char cmd[16] = { 0 };
	int length = snprintf(cmd, 16, VT100_CURSOR_DOWN, rows);
	StringAppend(term, cmd, length);
}

void TerminalMoveCursorRight(struct String* term, int cols) {
	char cmd[16] = { 0 };
	int length = snprintf(cmd, 16, VT100_CURSOR_RIGHT, cols);
	StringAppend(term, cmd, length);
}

void TerminalClear(struct String* term) {
	StringAppend(term, VT100_CLEAR_SCREEN, sizeof(VT100_CLEAR_SCREEN) - 1);
	TerminalSetCursor(term, 1,1);
}

void TerminalClearLine(struct String* term) {
	StringAppend(term, VT100_CLEAR_LINE, sizeof(VT100_CLEAR_LINE) - 1);
}

void TerminalHideCursor(struct String* term) {
	StringAppend(term, VT100_CURSOR_HIDE, sizeof(VT100_CURSOR_HIDE) - 1);
}

void TerminalShowCursor(struct String* term) {
	StringAppend(term, VT100_CURSOR_SHOW, sizeof(VT100_CURSOR_SHOW) - 1);
}

void TerminalInvertColor(struct String* term) {
	StringAppend(term, VT100_INVERT_COLOR, sizeof(VT100_INVERT_COLOR) - 1);
}

void TerminalDefaultColor(struct String* term) {
	StringAppend(term, VT100_DEFAULT_COLOR, sizeof(VT100_DEFAULT_COLOR) - 1);
}

void Die(const char* name) {
	struct String str = STR_INIT;
	TerminalClear(&str);
	write(STDOUT_FILENO, str.buf, str.len);
	StringFree(&str);

	perror(name);
	exit(1);
}

void DisableRawMode(void) {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &config.originalTerminal) == -1) {
		Die("tcsetattr");
	}
}
void EnableRawMode(void) {
	if (tcgetattr(STDIN_FILENO, &config.originalTerminal) == -1) {
		Die("tcgetattr");
	}
	atexit(DisableRawMode);
	
	struct termios raw = config.originalTerminal;

	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1; // 100 miliseconds

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
		Die("tcsetattr");
	}
}

int EditorReadKey(void) {
	int bytesRead = 0;
	int c = 0;
	while ((bytesRead = read(STDIN_FILENO, &c, 1)) != 1) {
		if (bytesRead == -1 && errno != EAGAIN) {
			Die("read");
		}
	}

	if (c == '\x1b') {
		char seq[3];
		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
				if (seq[2] == '~') {
					switch (seq[1]) {
                        case '1': return HOME;
                        case '3': return DELETE;
                        case '4': return END;
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
                        case '7': return HOME;
                        case '8': return END;
					}
				}
			}
			else {
				switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME;
                    case 'F': return END;
				}
			}
		}
        else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME;
                case 'F': return END;
            }
        }
	}
	return c;
}

int GetCursorPosition(int* rows, int* cols) {
	if (write(STDOUT_FILENO, VT100_GET_CURSOR_POS, sizeof(VT100_GET_CURSOR_POS) - 1) != sizeof(VT100_GET_CURSOR_POS) - 1) {
		return -1;
	}

	char buf[16];
	int i = 0;
	while (i < (int)sizeof(buf) -1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
		if (buf[i] == 'R') break;
		++i;
  	}
	buf[i] = '\0';

	if (buf[0] != '\x1b' || buf[1] != '[') return -1;
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    if (*rows == 0 || *cols == 0) return -1;
  	return 0;
}

int GetTerminalSize(int* rows, int* cols) {
	struct winsize ws = { 0 };

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_row == 0 || ws.ws_col == 0) {
		struct String str = STR_INIT;
		TerminalMoveCursorDown(&str, 999);
		TerminalMoveCursorRight(&str, 999);
		if (write(STDOUT_FILENO, str.buf, str.len) != str.len) return -1;
		StringFree(&str);
		return GetCursorPosition(&config.rows, &config.cols);
	}
	else {
		*rows = ws.ws_row;
		*cols = ws.ws_col;
	}
	return 0;
}

/*** line operations ***/

int GetRenderOffset(struct EditorLine* line, int x) {
    int renderOffset = 0;
    for (int i = 0; i < x; ++i) {
        if (line->str.buf[i] == '\t') {
            renderOffset += EDITOR_TAB_LEN;
        }
        else {
            ++renderOffset;
        }
    }
    return renderOffset;
}

int GetLineIndex(struct EditorLine* line, int renderOffset) {
    int currOffset = 0;
    for (int i = 0; i < line->str.len; ++i) {
        if (line->str.buf[i] == '\t') {
            currOffset += EDITOR_TAB_LEN;
        }
        else {
            ++currOffset;
        }

        if (currOffset > renderOffset) return i;
    }
    return line->str.len - 1;
}

void EditorUpdateLine(struct EditorLine* line) {
    free(line->render.buf);
    line->render.buf = NULL;
    line->render.len = 0;
    
    for (int i = 0; i < line->str.len; ++i) {
        if (line->str.buf[i] == '\t') {
            StringAppend(&line->render, EDITOR_TAB, EDITOR_TAB_LEN);
        }
        else {
            StringAppend(&line->render, &line->str.buf[i], 1);
        }
    }
    // char status[64];
    // int len = snprintf(status, sizeof(status), "[len: %d | upt: %ld]", line->str.len, GET_TIME);
    // StringAppend(&line->render, status, len); 
}

void EditorInsertLine(char* buf, int len, int at) {
    if (at < 0 || at > config.lines) return;

    config.line = realloc(config.line, (config.lines + 1) * sizeof(struct EditorLine));
    if (at != config.lines) {
        memmove(&config.line[at + 1], &config.line[at], (config.lines - at) * sizeof(struct EditorLine)); 
    }

    struct EditorLine* line = &config.line[at];

    line->str.buf = (char*)malloc(len);
    memcpy(line->str.buf, buf, len);
    line->str.len = len;

    line->render.buf = NULL;
    EditorUpdateLine(line);

    ++config.lines;

    ++config.dirty;
}

void EditorFreeLine(struct EditorLine* line) {
    StringFree(&line->str);
    StringFree(&line->render);
}

void EditorDeleteLine(int at) {
    if (at < 0 || at >= config.lines) return;
    
    EditorFreeLine(&config.line[at]);
    memmove(&config.line[at], &config.line[at + 1], (config.lines - at - 1) * sizeof(struct EditorLine));
    --config.lines;
    
    ++config.dirty;
}

void EditorLineInsertChar(struct EditorLine* line, int at, char c) {
    struct String* str = &line->str;
    if (at < 0 || at > str->len) at = str->len;

    str->buf = realloc(str->buf, str->len + 1);
    memmove(&str->buf[at + 1], &str->buf[at], str->len - at);  
    ++str->len;

    str->buf[at] = c;

    EditorUpdateLine(line);
    ++config.dirty;
}

void EditorLineAppendString(struct EditorLine* line, struct String* str) {
    StringAppend(&line->str, str->buf, str->len);
    EditorUpdateLine(line);
}

void EditorLineDeleteChar(struct EditorLine* line, int at) {
    struct String* str = &line->str;
    if (at < 0 || at >= str->len) return;

    memmove(&str->buf[at], &str->buf[at + 1], str->len - at - 1);
    --str->len;

    EditorUpdateLine(line);
    ++config.dirty;
}

/*** editor operations ***/

void EditorInsertChar(int c) {
    if (config.y == config.lines) {
        EditorInsertLine(NULL, 0, config.lines);
    }
    EditorLineInsertChar(&config.line[config.y], config.x, (char)c);
    ++config.x;
}

void EditorInsertNewLine(void) {
    if (config.x == 0) {
        EditorInsertLine(NULL, 0, config.y);
    }
    else {
        struct EditorLine* line = &config.line[config.y];
        EditorInsertLine(&line->str.buf[config.x], line->str.len - config.x, config.y + 1);

        line = &config.line[config.y];
        StringTruncate(&line->str, config.x);

        EditorUpdateLine(line);
    }
    ++config.y;
    config.x = 0;
}

void EditorDeleteChar(void) {
    if (config.y == config.lines) return;
    if (config.x == 0 && config.y == 0) return;

    struct EditorLine* line = &config.line[config.y];
    if (config.x > 0) {
        EditorLineDeleteChar(line, config.x - 1);
        --config.x;
    }
    else {
        struct EditorLine* prev = &config.line[config.y - 1];
        config.x = prev->str.len;
        EditorLineAppendString(prev, &line->str);
        EditorDeleteLine(config.y);
        --config.y;
    }
}

/*** file i/o ***/

void EditorLinesToString(struct String* str) {
    StringFree(str);
    for (int i = 0; i < config.lines; ++i) {
        str->len += config.line[i].str.len + 1;
    }

    str->buf = (char*)malloc(str->len);
    char* ptr = str->buf;
    for (int i = 0; i < config.lines; ++i) {
        int len = config.line[i].str.len;
        memcpy(ptr, config.line[i].str.buf, len);
        ptr += len;
        *ptr = '\n';
        ++ptr;
    }
}

void EditorOpen(const char* fileName) {
    free(config.fileName);
    config.fileName = strdup(fileName);

    FILE* file = fopen(fileName, "r");
    if (file == NULL) Die("fopen");

    char* line = NULL;
    int cap = 0;
    int len;
    while ((len = (int)getline(&line, (size_t*)&cap, file)) != -1) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) --len;

        EditorInsertLine(line, len, config.lines);
    }
    free(line);
    fclose(file);

    config.dirty = 0;
}

void EditorSave(void) {
    if (config.fileName == NULL) {
        config.fileName = EditorPrompt("Save as: %s", NULL);
        if (config.fileName == NULL) {
            EditorSetMessage("Save aborted");
        }
    }

    int fd = open(config.fileName, O_RDWR | O_CREAT, 0644);
    if (fd == -1) return;

    struct String str = STR_INIT;
    EditorLinesToString(&str);

    if (ftruncate(fd, str.len) != -1 && write(fd, str.buf, str.len) == str.len) {
        config.dirty = 0;
        EditorSetMessage("%d bytes written to disk", str.len);
    }
    else {
        EditorSetMessage("Can't save! I/O error: %s", strerror(errno));
    }
        
    close(fd);
    StringFree(&str);
}

/*** find ***/

void EditorFindCallback(char* query, int key) {
    static int lastMatch = -1;
    static int direction = 1;

    switch (key) {
        case '\r':
        case '\x1b':
            lastMatch = -1;
            direction = 1;
            return;
        case ARROW_RIGHT:
        case ARROW_UP:
            direction = 1;
            break;
        case ARROW_LEFT:
        case ARROW_DOWN:
            direction = -1;
            break;
        default:
            lastMatch = -1;
            direction = 1;
            break;
    }

    if (lastMatch == -1) direction = 1;
    int current = lastMatch;

    for (int i = 0; i < config.lines; ++i) {
        current += direction;
        if (current < 0) {
            current = config.lines - 1;
        }
        else if (current >= config.lines) {
            current = 0;
        }

        struct EditorLine* line = &config.line[current];
        int len = line->render.len;
        char* str = malloc(len);
        memcpy(str, line->render.buf, len);
        str[len] = '\0';
        char* match = strstr(str, query);
        if (match != NULL) {
            lastMatch = current;
            config.y = current;
            config.x = GetLineIndex(line, match - str);
            config.rowOffset = config.lines;
            free(str);
            break;
        }
        free(str);
    }
}

void EditorFind() {
    int x = config.x;
    int y = config.y;
    int rowOffset = config.rowOffset;
    int colOffset = config.colOffset;

    char* query = EditorPrompt("Search: %s (Use ESC/Arrows/Enter)", EditorFindCallback);
    if (query != NULL) {
        free(query);
    }
    else {
        config.x = x;
        config.y = y;
        config.rowOffset = rowOffset;
        config.colOffset = colOffset;
    }
}

/*** output ***/

void EditorScroll(void) {
    if (config.y < config.lines) {
        config.renderOffset = GetRenderOffset(&config.line[config.y], config.x);
    }
    else {
        config.renderOffset = 0; 
    }

    if (config.y < config.rowOffset) {
        config.rowOffset = config.y;
    }
    if (config.y >= config.rowOffset + config.rows) {
        config.rowOffset = config.y - config.rows + 1;
    }
    if (config.renderOffset < config.colOffset) {
        config.colOffset = config.renderOffset;
    }
    if (config.renderOffset >= config.colOffset + config.cols) {
        config.colOffset = config.renderOffset - config.cols + 1;
    }
}

void EditorDrawRows(struct String* term) {
	for (int y = 0; y < config.rows; ++y) {
        int row = config.rowOffset + y;
        if (row >= config.lines) {
            if (config.lines == 0 && y == config.rows / 3) {
                char welcome[64] = { 0 };
                int len = snprintf(welcome, 64, "%s editor - version %s", EDITOR_NAME, EDITOR_VERSION);
                if (len > config.cols) len = config.cols;

                int padding = (config.cols - len) / 2;
                if (padding) {
                    StringAppend(term, "~", 1);
                    --padding;
                }
                while (padding--) StringAppend(term, " ", 1);

                StringAppend(term, welcome, len);
            }
            else {
                StringAppend(term, "~", 1);
            }
        }
        else {
            int len = config.line[row].render.len - config.colOffset;
            if (len < 0) len = 0;
            if (len > config.cols) len = config.cols;
            StringAppend(term, &config.line[row].render.buf[config.colOffset], len);
        }

		TerminalClearLine(term);
        StringAppend(term, "\r\n", 2);
	}
}

void EditorDrawStatusBar(struct String* term) {
    TerminalInvertColor(term);

    char status[64];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", (config.fileName == NULL) ? "[No name]" : config.fileName,
            config.lines, (config.dirty == 0) ? "" : "(modified)");
    char rStatus[64];
    int rLen = snprintf(rStatus, sizeof(rStatus), "%d/%d", config.y, config.lines);
    
    if (len > config.cols) len = config.cols;
    StringAppend(term, status, len);

    for (int i = len; i < config.cols - rLen; ++i) {
        StringAppend(term, " ", 1);
    }

    StringAppend(term, rStatus, rLen);

    TerminalDefaultColor(term);
    StringAppend(term, "\r\n", 2);
}

void EditorDrawMessage(struct String* term) {
    TerminalClearLine(term);

    if (time(NULL) - config.msgTime > 5) return;

    int len = strlen(config.msg);
    if (len > config.cols) len = config.cols;
    StringAppend(term, config.msg, len);
}

void EditorRefreshScreen(void) {
    EditorScroll();

	struct String term = STR_INIT;

	TerminalHideCursor(&term);
    TerminalSetCursor(&term, 1, 1);

	EditorDrawRows(&term);
    EditorDrawStatusBar(&term);
    EditorDrawMessage(&term);

	TerminalSetCursor(&term, (config.y - config.rowOffset) + 1, (config.renderOffset - config.colOffset) + 1);
	TerminalShowCursor(&term);

	write(STDOUT_FILENO, term.buf, term.len);
	StringFree(&term);
}

void EditorSetMessage(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(config.msg, EDITOR_MSG_LEN, fmt, ap);
    va_end(ap);

    config.msgTime = time(NULL);
}

/*** input ***/

char* EditorPrompt(char* prompt, void (*callback)(char*, int)) {
    int cap = 128;
    char* buf = (char*)malloc(cap);
    buf[0] = '\0';
    int len = 0;

    while(1) {
        EditorSetMessage(prompt, buf);
        EditorRefreshScreen();

        int c = EditorReadKey();
        switch (c) {
            case BACKSPACE:
            case CTRL_KEY('h'):
            case DELETE:
                if (len > 0) {
                    --len;
                    buf[len] = '\0';
                }
                break;
            case '\r':
                EditorSetMessage("");
                if (callback) callback(buf, c);
                return buf; 
            case '\x1b':
                EditorSetMessage("");
                if (callback) callback(buf, c);
                free(buf);
                return NULL;
            default:
                if (iscntrl(c) == 0 && c < 128) {
                    if (len == cap - 1) {
                        cap *= 2;
                        buf = realloc(buf, cap);
                    }
                    buf[len] = c;
                    ++len;
                    buf[len] = '\0';
                }
                break;
        }
        if (callback) callback(buf, c);
    }
}

void EditorKeyActions(int key) {
    struct EditorLine* line = (config.y >= config.lines) ? NULL : &config.line[config.y];
    int len = (line == NULL) ? 0 : line->str.len;

    static int quitCount = EDITOR_QUIT_CONFIRM;

	switch (key) {
        case '\r':
            EditorInsertNewLine();
            break;
		case CTRL_KEY('q'): {
                if (config.dirty != 0 && quitCount > 0) {
                    EditorSetMessage("WARNING!!! File has unsaved changes. "
                            "Press Ctrl-Q %d more %s to quit.", quitCount, (quitCount == 1) ? "time" : "times");
                    --quitCount;
                    return;
                }
				struct String str = STR_INIT;
				TerminalClear(&str);
				write(STDOUT_FILENO, str.buf, str.len);
				StringFree(&str);
				exit(0);
			}
			break;
        case CTRL_KEY('s'):
            EditorSave();
            break;
        case HOME:
            config.x = 0;
            break;
        case END:
            config.x = len;
            break;
        case CTRL_KEY('f'):
            EditorFind();
            break;
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DELETE:
            if (key == DELETE) EditorKeyActions(ARROW_RIGHT);
            EditorDeleteChar();
            break;
        case PAGE_UP:
            config.y = config.rowOffset;
            break;
        case PAGE_DOWN:
            config.y = config.rowOffset + config.rows - 1;
            if (config.y > config.lines) config.y = config.lines;
            break;
		case ARROW_LEFT:
			if (config.x > 0) {
				--config.x;
			}
            else if (config.y > 0) {
                --config.y;
                config.x = config.line[config.y].str.len;
            }
			break;
		case ARROW_DOWN:
			if (config.y < config.lines) { 
				++config.y;
			}
			break;
		case ARROW_UP:
			if (config.y > 0) {
				--config.y;
			}
			break;
		case ARROW_RIGHT:
            if (line && config.x < line->str.len) {
                ++config.x;
            }
            else if (line && config.x == line->str.len) {
                ++config.y;
                config.x = 0;
            }
			break;
        case CTRL_KEY('l'):
        case '\x1b':
            break;
        default:
            EditorInsertChar(key);
            break;
	}

    line = (config.y >= config.lines) ? NULL : &config.line[config.y];
    len = (line == NULL) ? 0 : line->str.len;
    if (config.x > len) {
        config.x = len;
    }

    quitCount = EDITOR_QUIT_CONFIRM;

}

void EditorProcessKeypress(void) {
	int c = EditorReadKey();
    EditorKeyActions(c);
}

/*** init ***/

void InitEditor(void) {
	config.x = 0;
	config.y = 0;
    config.renderOffset = 0;
    config.rowOffset = 0;
    config.colOffset = 0;
    config.lines = 0;
    config.line = NULL;
    config.dirty = 0;
    config.fileName = NULL;
    config.msg[0] = '\0';
    config.msgTime = 0;

	if (GetTerminalSize(&config.rows, &config.cols) == -1) {
		Die("GetTerminalSize");
	}
    config.rows -= 2;
}

int main(int argc, char* argv[]) { 
    startTime = time(NULL);

	EnableRawMode();
	InitEditor();
    if (argc > 1) {
        EditorOpen(argv[1]);
    }

    EditorSetMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

	while (1) {
		EditorRefreshScreen();
		EditorProcessKeypress();
	}

	return 0;
}
