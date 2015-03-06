#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <errno.h>

/* FIXME: This is only for testing purposes. */
static const char initial_line[] = "Now is the time for all good men to come to the aid of the party. Lorem Ipsum is a stupid Microsoft thing.";


static void clean_abort(void);
static void update_status(void);

/* Text is stored line-by-line. Line lengths are stored with the text data
 * to avoid lots of strlen() calls for line wrapping, insertion, etc. */
struct line {
	struct line *prev;
	struct line *next;
	char *text;
	int len;
};
static struct line *line_head = NULL;

/* Terminal configuration data */
static struct termios term_orig, term_config;
static int termdesc = -1;
static int term_rows;
static int term_real_rows;
static int term_cols;


/* Current editing locations (screen and file) */
static char crsr_x, crsr_y, cur_line, line_shift;
static char crsr_set_string[12];
/* Track current line's struct pointer to avoid extra walks */
static struct line *cur_line_s = NULL;

/* Current mode: 0=command, 1=insert, 2=replace */
#define MODE_COMMAND 0
#define MODE_INSERT 1
#define MODE_REPLACE 2
static int vi_mode = 0;
static const char * const mode_string[] = {
	"", "--- INSERT ---", "--- REPLACE ---"
};
#define MAX_STATUS 64
static char custom_status[MAX_STATUS] = "";

/* Total number of lines allocated */
static int line_count = 0;

/* When a line is being edited, store the edits in a temporary buffer.
 * Backspace/delete operations will have a negative length. This buffer
 * is committed on cursor movement, leaving edit mode, or hitting the
 * end of the temporary buffer. */
#define TEMP_BUFSIZ 128
struct {
	int insert_pos;
	int len;
	char text[TEMP_BUFSIZ];
} temp_line;

/* Escape sequence function definitions */
#define CLEAR_SCREEN() write(STDOUT_FILENO, "\033[H\033[J", 6);
#define ERASE_LINE() write(STDOUT_FILENO, "\033[2K", 4);
#define CRSR_HOME() write(STDOUT_FILENO, "\033[H", 3);
#define CRSR_UP() write(STDOUT_FILENO, "\033[1A", 4);
#define CRSR_DOWN() write(STDOUT_FILENO, "\033[1B", 4);
#define CRSR_LEFT() write(STDOUT_FILENO, "\033[1D", 4);
#define CRSR_RIGHT() write(STDOUT_FILENO, "\033[1C", 4);
#define CRSR_YX(a,b) { snprintf(crsr_set_string, 12, "\033[%d;%df", a, b); \
	write(STDOUT_FILENO, crsr_set_string, strlen(crsr_set_string)); }
#define DISABLE_LINE_WRAP() write(STDOUT_FILENO, "\033[7l", 4);
#define ENABLE_LINE_WRAP() write(STDOUT_FILENO, "\033[7h", 4);

/***************************************************************************/

/* Read terminal dimensions */
static void read_term_dimensions(void)
{
	struct winsize w;
	ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
	term_real_rows = w.ws_row;
	term_rows = w.ws_row - 1;
	term_cols = w.ws_col;
	return;
}


/* Invalid command warning */
static void invalid_command(void)
{
	strcpy(custom_status, "Invalid command");
	update_status();
	return;
}


/* Walk the line list to the requested line */
static inline struct line *walk_to_line(int num)
{
	struct line *line = line_head;
	int i = 0;

	if (num == 0) return NULL;
	while (line != NULL) {
		i++;
		if (i == num) break;
		line = line->next;
	}
	return line;
}


/* Allocate a new line after the selected line */
static struct line *new_line(unsigned int start, const char * restrict text)
{
	struct line *cur_line, *new_line;

	/* Cannot open lines out of current range */
	if (start > line_count) return NULL;

	if (start > 0) cur_line = walk_to_line(start);
	else cur_line = line_head;

	/* Insert a new line */
	new_line = (struct line *)malloc(sizeof(struct line));
	if (!new_line) goto oom;
	if (cur_line == NULL) {
		/* If line_head is NULL, no lines exist yet */
		line_head = new_line;
		new_line->next = NULL;
		new_line->prev = NULL;
	} else {
		new_line->next = cur_line->next;
		new_line->prev = cur_line;
		cur_line->next = new_line;
	}
	if (new_line->next != NULL) new_line->next->prev = new_line;
	/* Allocate the text area (if applicable) */
	if (text == NULL) {
		new_line->text = NULL;
		new_line->len = 0;
	} else {
		new_line->len = strlen(text);
		new_line->text = (char *)malloc(new_line->len + 1);
		if (!new_line->text) goto oom;
		strncpy(new_line->text, text, new_line->len);
	}

	return new_line;
oom:
	fprintf(stderr, "out of memory\n");
	clean_abort();

	return NULL;
}

/* Destroy and free one line in the line list */
static int destroy_line(int num)
{
	struct line *line;

	line = walk_to_line(num);
	if (line == NULL) return -1;
	if (line->text != NULL) free(line->text);
	line->prev->next = line->next;
	free(line);

	return 0;
}

static void destroy_buffer(void)
{
	struct line *line = line_head;
	struct line *prev = NULL;

	line_head = NULL;

	/* Free lines in order until list is exhausted */
	while (line != NULL) {
		if (line->text != NULL) free(line->text);
		if (line->prev != NULL) free(line->prev);
		prev = line;
		line = line->next;
	}
	/* Free the final line, if applicable */
	if (prev != NULL) free(prev);
}

static void update_status(void)
{
	char num[4];
	int top_line;

	/* Move the cursor to the last line */
	CRSR_YX(term_real_rows, 0);
	ERASE_LINE();

	/* Print the current insert/replace mode or special status */
	if (*custom_status == '\0')
		strcpy(custom_status, mode_string[vi_mode]);
	write(STDOUT_FILENO, custom_status, strlen(custom_status));
	*custom_status = '\0';

	/* Print our location in the current line and file */
	CRSR_YX(term_real_rows, term_cols - 20);
	printf("%d,%d", cur_line, crsr_x + line_shift);
	CRSR_YX(term_real_rows, term_cols - 5);
	top_line = cur_line - crsr_y + 1;
	if (top_line < 1) goto error_top_line;
	if (top_line == 1) {
		write(STDOUT_FILENO, " Top", 4);
	} else if ((cur_line + term_rows) >= line_count) {
		write(STDOUT_FILENO, " Bot", 4);
	} else {
		snprintf(num, 4, "%d%%", (line_count * 100) / top_line);
		write(STDOUT_FILENO, num, strlen(num));
	}

	/* Put the cursor back where it was before we touched it */
	CRSR_YX(crsr_y, crsr_x);

	return;

error_top_line:
	fprintf(stderr, "error: top line is invalid (%d)\n", top_line);
	clean_abort();
}


/* Write a line to the screen with appropriate shift */
static void write_shifted_line(struct line *line)
{
	char *p = line->text + line_shift;
	int len = line->len - line_shift;

	CRSR_HOME();
	ERASE_LINE();
	if (len > term_cols) len = term_cols;
	write(STDOUT_FILENO, p, len);
	write(STDOUT_FILENO, "\n", 1);
	return;
}


/* Redraw the entire screen */
static void redraw_screen(void)
{
	struct line *line;
	int start_y, start_x;
	int remain_row;

	CLEAR_SCREEN();

	/* Get start line number and pointer */
	if (cur_line < crsr_y) goto error_line_cursor;
	start_y = cur_line - crsr_y + 1;

	/* Find the first line to write to the screen */
	line = walk_to_line(start_y);
	if (!line) goto error_line_walk;

	/* Draw lines until no more are left */
	remain_row = term_rows - 1;
	while (remain_row) {
		/* Write out this line data */
		write_shifted_line(line);
		line = line->next;
		remain_row--;
		if (line == NULL) break;
	}

	/* Fill the rest of the screen with tildes */
	while (remain_row) {
		write(STDOUT_FILENO, "~\n", 2);
		remain_row--;
	}

	update_status();
	CRSR_HOME();

	return;

error_line_cursor:
	fprintf(stderr, "error: cur_line < crsr_y\n");
	clean_abort();
error_line_walk:
	fprintf(stderr, "error: line walk invalid (%d) (%d - %d)\n",
			start_y, cur_line, crsr_y);
}


/* Delete char at cursor location */
static void do_del_crsr_char(void)
{
	char *p;

	if (cur_line_s->len == 0) return;
	p = cur_line_s->text + crsr_x + line_shift;

	/* Copy everything down one char */
	strncpy(p - 1, p, strlen(p));
	cur_line_s->len--;
	if (crsr_x > (cur_line_s->len - line_shift)) crsr_x--;
	if (crsr_x < 1) {
		if (line_shift > 0) line_shift--;
		crsr_x = 1;
	}
	write_shifted_line(cur_line_s);
	CRSR_YX(crsr_y, crsr_x);
	return;
}


/* Restore terminal to original configuration */
static void term_restore(void)
{
	if (termdesc != -1) tcsetattr(termdesc, TCSANOW, &term_orig);
	ENABLE_LINE_WRAP();
	return;
}

/* Initialize terminal settings */
static int term_init(void)
{
	/* Only init terminal once */
	if (termdesc != -1) return 0;

	/* Find a std* stream with a TTY */
	if (isatty(STDIN_FILENO)) termdesc = STDIN_FILENO;
	else if (isatty(STDOUT_FILENO)) termdesc = STDOUT_FILENO;
	else if (isatty(STDERR_FILENO)) termdesc = STDERR_FILENO;
	else return -ENOTTY;

	/* Get current terminal configuration and save it*/
	if (tcgetattr(termdesc, &term_orig)) return -EBADF;
	memcpy(&term_config, &term_orig, sizeof(struct termios));

	/* Disable buffering */
	if (isatty(STDIN_FILENO)) setvbuf(stdin, NULL, _IONBF, 0);
	if (isatty(STDOUT_FILENO)) setvbuf(stdout, NULL, _IONBF, 0);
	if (isatty(STDERR_FILENO)) setvbuf(stderr, NULL, _IONBF, 0);

	/* Configure terminal settings */

	/* c_cc */
	term_config.c_cc[VTIME] = 0;
	term_config.c_cc[VMIN] = 1;	/* read() one char at a time*/

	/* iflag */
#ifdef IUCLC
	term_config.c_iflag &= ~IUCLC;
#endif
	term_config.c_iflag &= ~PARMRK;
	term_config.c_iflag |= IGNPAR;
	term_config.c_iflag &= ~IGNBRK;
	term_config.c_iflag |= BRKINT;
	term_config.c_iflag &= ~ISTRIP;
	term_config.c_iflag &= ~(INLCR | IGNCR | ICRNL);

	/* cflag */
	term_config.c_cflag &= ~CSIZE;
	term_config.c_cflag |= CS8;
	term_config.c_cflag |= CREAD;

	/* lflag */
	term_config.c_lflag |= ISIG;
	term_config.c_lflag &= ~ICANON;	/* disable line buffering */
	term_config.c_lflag &= ~IEXTEN;

	/* disable local echo */
	term_config.c_lflag &= ~(ECHO | ECHONL | ECHOE | ECHOK);

	/* Finalize settings */
	tcsetattr(termdesc, TCSANOW, &term_config);

	/* Disable automatic line wrapping */
	DISABLE_LINE_WRAP();

	return 0;
}

/* Clean abort */
static void clean_abort(void)
{
	term_restore();
	destroy_buffer();
	exit(EXIT_FAILURE);
}


/* Oh dear God, NO! */
static void oh_dear_god_no(char *string)
{
	strcpy(custom_status, "THIS SHOULDN'T HAPPEN: ");
	strcat(custom_status, string);
	update_status();
	return;
}


/* Editing mode. Doesn't return until ESC pressed. */
void edit_mode(void)
{
	char c;
	while (read(STDIN_FILENO, &c, 1)) {
		if (c == '\033') {
			/* FIXME: poll for ESC sequences */
			vi_mode = MODE_COMMAND;
			update_status();
			return;
		}

		/* Catch any invalid characters */
		if (c < 32 || c > 127) {
			strcpy(custom_status, "Invalid char entered\n");
			update_status();
			continue;
		}
		/* Insert character at cursor position */
		/* FIXME */
	}
	return;
}


/* Handle an incoming command */
int do_cmd(char c)
{
	switch (c) {
	case 'q':
		goto end_vi;
		break;
	case 'i':
		vi_mode = MODE_INSERT;
		update_status();
		edit_mode();
		break;
	case ':':
		c = getc(stdin);
		switch (c) {
		default:
			invalid_command();
			break;
		}
		break;
	case 'h':	/* left */
		if (crsr_x == 1) {
			if (line_shift > 0) {
				line_shift--;
				write_shifted_line(cur_line_s);
				break;
			} else break;
		}
		crsr_x--; CRSR_LEFT();
		break;
	case 'j':	/* down */
		//do_cursor_down();
		break;
	case 'k':	/* up */
		//do_cursor_up();
		break;
	case 'l':	/* right */
		if (crsr_x == cur_line_s->len) break;
		if (crsr_x == term_cols) {
			if ((cur_line_s->len - line_shift) > term_cols) {
				line_shift++;
				write_shifted_line(cur_line_s);
			} else {
				oh_dear_god_no("cmd: l: term_cols check");
				break;
			}
		} else {
			if (crsr_x < (cur_line_s->len - line_shift)) {
				crsr_x++;
				CRSR_RIGHT();
			}
		}
		break;
	case 'x':	/* Delete char at cursor */
		do_del_crsr_char();
		break;
	case '!':	/* NON-STANDARD cursor pos dump */
		sprintf(custom_status, "term %dx%d, c_x %d, c_y %d",
				term_cols, term_real_rows, crsr_x, crsr_y);
		break;
	default:
		snprintf(custom_status, MAX_STATUS,
				"Unknown key %u", c);
		break;
	}
	update_status();
	return 0;

end_vi:
	CLEAR_SCREEN();
	term_restore();
	destroy_buffer();
	exit(EXIT_SUCCESS);
}


int main(int argc, char **argv)
{
	int i;
	char c;

	line_shift = 0;
	if ((i = term_init()) != 0) {
		if (i == -ENOTTY) fprintf(stderr, "tty is required\n");
		else fprintf(stderr, "cannot init terminal: %s\n", strerror(-i));
		clean_abort();
	}

	read_term_dimensions();
	CLEAR_SCREEN();
	/* Set up the first line */
	cur_line_s = new_line(0, initial_line);
	crsr_x = 1; crsr_y = 1;
	cur_line = 1; /* crsr_x doubles as current position */
	redraw_screen();
	/* Read commands forever */
	while (read(STDIN_FILENO, &c, 1)) do_cmd(c);
	clean_abort();
}
