/*** includes ***/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <termios.h>

#include <sys/ioctl.h>

/*** defines ***/

#define EDITOR_NAME "Kilo"
#define EDITOR_VERSION "0.0.1"

#define CTRL_KEY(key) ((key) & 0x1F)

#define VT100_CLEAR_SCREEN "\x1b[2J"
#define VT100_CLEAR_LINE "\x1b[K"
#define VT100_SET_CURSOR_POS "\x1b[%d;%dH"
#define VT100_GET_CURSOR_POS "\x1b[6n"
#define VT100_CURSOR_DOWN "\x1b[%dB"
#define VT100_CURSOR_RIGHT "\x1b[%dC"
#define VT100_CURSOR_HIDE "\x1b[?25l" // ?25 option (cursor visibility) is supported in later
#define VT100_CURSOR_SHOW "\x1b[?25h" // VT versions, so it will not appear in VT100 docs

/*** data ***/

struct EditorConfig {
	int x, y;
	int rows, cols;
	struct termios originalTerminal;
};
struct EditorConfig config = { 0 };

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
}

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

void Die(const char* name) {
	struct String str = STR_INIT;
	TerminalClear(&str);
	write(STDOUT_FILENO, str.buf, str.len);
	StringFree(&str);

	perror(name);
	exit(1);
}

void DisableRawMode() {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &config.originalTerminal) == -1) {
		Die("tcsetattr");
	}
}
void EnableRawMode() {
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

char EditorReadKey() {
	int bytesRead = 0;
	char c = '\0';
	while ((bytesRead = read(STDIN_FILENO, &c, 1)) != 1) {
		if (bytesRead == -1 && errno != EAGAIN) {
			Die("read");
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

  	return 0;
}

int GetTerminalSize(int* rows, int* cols) {
	struct winsize ws = { 0 };

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
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

/*** output ***/

void EditorDrawRows(struct String* term) {
	for (int y = 0; y < config.rows; ++y) {
		if (y == config.rows / 3) {
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

		TerminalClearLine(term);
		if (y < config.rows - 1) {
			StringAppend(term, "\r\n", 2);
		}
	}
}

void EditorRefreshScreen() {
	struct String term = STR_INIT;

	TerminalHideCursor(&term);
	EditorDrawRows(&term);

	TerminalSetCursor(&term, config.y + 1, config.x + 1);
	TerminalShowCursor(&term);

	write(STDOUT_FILENO, term.buf, term.len);
	StringFree(&term);
}

/*** input ***/

void EditorProcessKeypress() {
	char c = EditorReadKey();

	switch (c) {
		case CTRL_KEY('q'): {
				struct String str = STR_INIT;
				TerminalClear(&str);
				write(STDOUT_FILENO, str.buf, str.len);
				StringFree(&str);
				exit(0);
			}
			break;
		case 'h':
			--config.x;
			break;
		case 'j':
			++config.y;
			break;
		case 'k':
			--config.y;
			break;
		case 'l':
			++config.x;
			break;
	}

	if (config.x < 0) config.x = 0;
	if (config.y < 0) config.y = 0;
}

/*** init ***/

void InitEditor() {
	config.x = 0;
	config.y = 0;

	if (GetTerminalSize(&config.rows, &config.cols) == -1) {
		Die("GetTerminalSize");
	}
}

int main() { 
	EnableRawMode();
	InitEditor();

	while (1) {
		EditorRefreshScreen();
		EditorProcessKeypress();
	}

	return 0;
}
