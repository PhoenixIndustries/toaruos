/* vim: tabstop=4 shiftwidth=4 noexpandtab
 * This file is part of ToaruOS and is released under the terms
 * of the NCSA / University of Illinois License - see LICENSE.md
 * Copyright (C) 2013-2018 K. Lange
 *
 * E-Shell
 *
 * This is the "experimental shell". It provides
 * a somewhat unix-like shell environment, but does
 * not include a parser any advanced functionality.
 * It simply cuts its input into arguments and executes
 * programs.
 */

#define _XOPEN_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <signal.h>
#include <getopt.h>
#include <termios.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>

#include <sys/time.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <sys/stat.h>

#include <_xlog.h>

#include <toaru/list.h>
#include <toaru/kbd.h>
#include <toaru/rline.h>

#define PIPE_TOKEN "\xFF\xFFPIPE\xFF\xFF"
#define STAR_TOKEN "\xFF\xFFSTAR\xFF\xFF"
#define WRITE_TOKEN "\xFF\xFFWRITE\xFF\xFF"
#define APPEND_TOKEN "\xFF\xFF""APPEND\xFF"

/* A shell command is like a C program */
typedef uint32_t(*shell_command_t) (int argc, char ** argv);

/* We have a static array that fits a certain number of them. */
#define SHELL_COMMANDS 512
char * shell_commands[SHELL_COMMANDS];          /* Command names */
shell_command_t shell_pointers[SHELL_COMMANDS]; /* Command functions */
char * shell_descript[SHELL_COMMANDS];          /* Command descriptions */

/* This is the number of actual commands installed */
int shell_commands_len = 0;

int shell_interactive = 1;
int last_ret = 0;
char ** shell_argv = NULL;
int shell_argc = 0;


int pid; /* Process ID of the shell */

void shell_install_command(char * name, shell_command_t func, char * desc) {
	if (shell_commands_len == SHELL_COMMANDS) {
		fprintf(stderr, "Ran out of space for static shell commands. The maximum number of commands is %d\n", SHELL_COMMANDS);
		return;
	}
	shell_commands[shell_commands_len] = name;
	shell_pointers[shell_commands_len] = func;
	shell_descript[shell_commands_len] = desc;
	shell_commands_len++;
}

shell_command_t shell_find(char * str) {
	for (int i = 0; i < shell_commands_len; ++i) {
		if (!strcmp(str, shell_commands[i])) {
			return shell_pointers[i];
		}
	}
	return NULL;
}

void install_commands();

/* Maximum command length */
#define LINE_LEN 4096

/* Current working directory */
char cwd[1024] = {'/',0};

/* Username */
char username[1024];

/* Hostname for prompt */
char _hostname[256];

/* function to update the cached username */
void getuser() {
	char * tmp = getenv("USER");
	if (tmp) {
		_XLOG("Got user:");
		_XLOG(tmp);
		strcpy(username, tmp);
	} else {
		sprintf(username, "%d", getuid());
	}
}

/* function to update the cached hostname */
void gethost() {
	struct utsname buf;

	uname(&buf);

	int len = strlen(buf.nodename);
	memcpy(_hostname, buf.nodename, len+1);
}

/* Draw the user prompt */
void draw_prompt(int ret) {
	/* Get the time */
	struct tm * timeinfo;
	struct timeval now;
	gettimeofday(&now, NULL); //time(NULL);
	timeinfo = localtime((time_t *)&now.tv_sec);

	/* Format the date and time for prompt display */
	char date_buffer[80];
	strftime(date_buffer, 80, "%m/%d", timeinfo);
	char time_buffer[80];
	strftime(time_buffer, 80, "%H:%M:%S", timeinfo);

	/* Print the working directory in there, too */
	getcwd(cwd, 512);
	char _cwd[512];
	strcpy(_cwd, cwd);

	char * home = getenv("HOME");
	if (home && strstr(cwd, home) == cwd) {
		char * c = cwd + strlen(home);
		if (*c == '/' || *c == 0) {
			sprintf(_cwd, "~%s", c);
		}
	}

	/* Print the prompt. */
	printf("\033]1;%s@%s:%s\007", username, _hostname, _cwd);
	printf("\033[1m\033[s\033[400C\033[16D\033[1m\033[38;5;59m[\033[38;5;173m%s \033[38;5;167m%s\033[38;5;59m]\033[u\033[38;5;221m%s\033[38;5;59m@\033[38;5;81m%s ",
			date_buffer, time_buffer,
			username, _hostname);
	if (ret != 0) {
		printf("\033[38;5;167m%d ", ret);
	}

	printf("\033[0m%s%s\033[0m ", _cwd, getuid() == 0 ? "\033[1;38;5;196m#" : "\033[1;38;5;47m$");
	fflush(stdout);
}

uint32_t child = 0;

void sig_pass(int sig) {
	/* Interrupt handler */
	if (child) {
		kill(child, sig);
	}
}

void redraw_prompt_func(rline_context_t * context) {
	draw_prompt(last_ret);
}

void draw_prompt_c() {
	printf("> ");
	fflush(stdout);
}
void redraw_prompt_func_c(rline_context_t * context) {
	draw_prompt_c();
}

void tab_complete_func(rline_context_t * c) {
	char * dup = malloc(LINE_LEN);
	
	memcpy(dup, c->buffer, LINE_LEN);

	char *pch, *save;
	char *argv[1024];
	int argc = 0;
	int cursor = 0;

	pch = strtok_r(dup, " ", &save);

	if (!pch) {
		argv[0] = "";
		argc = 0;
	}

	while (pch != NULL) {
		if (pch - dup <= c->offset) cursor = argc;
		argv[argc] = pch;
		++argc;
		pch = strtok_r(NULL, " ", &save);
	}
	argv[argc] = NULL;

	if (c->offset && c->buffer[c->offset-1] == ' ' && argc) {
		cursor++;
	}

	char * word = argv[cursor];
	int word_offset = word ? (c->offset - (argv[cursor] - dup)) : 0;

	char * prefix = malloc(word_offset + 1);
	if (word) memcpy(prefix, word, word_offset);
	prefix[word_offset] = '\0';

	/* Complete file path */
	list_t * matches = list_create();
	char * match = NULL;
	int free_matches = 0;
	int no_space_if_only = 0;

	/* TODO custom auto-complete as a configuration file? */
#define COMPLETE_FILE    1
#define COMPLETE_COMMAND 2
#define COMPLETE_CUSTOM  3
	int complete_mode = COMPLETE_FILE;

	int command_adj = 0;
	int cursor_adj = cursor;

	/* sudo should shift commands */
	if (cursor_adj > command_adj && (!strcmp(argv[command_adj], "sudo") || !strcmp(argv[command_adj], "gsudo"))) {
		cursor_adj -= 1;
		command_adj += 1;
	}

	/* initial tab completion should be commands, unless typing a file path */
	if (cursor_adj == 0 && !strchr(prefix,'/')) {
		complete_mode = COMPLETE_COMMAND;
	}

	/* term-set has some commands to complete */
	if (cursor_adj >= 1 && !strcmp(argv[command_adj], "term-set")) {
		complete_mode = COMPLETE_CUSTOM;
	}

	if (complete_mode == COMPLETE_COMMAND) {
		/* Complete a command name */

		for (int i = 0; i < shell_commands_len; ++i) {
			if (strstr(shell_commands[i], prefix) == shell_commands[i]) {
				list_insert(matches, shell_commands[i]);
				match = shell_commands[i];
			}
		}
	} else if (complete_mode == COMPLETE_FILE) {
		/* Complete a file path */

		free_matches = 1;
		char * tmp = strdup(prefix);
		char * last_slash = strrchr(tmp, '/');
		DIR * dirp;
		char * compare = prefix;
		if (last_slash) {
			*last_slash = '\0';
			word = word + (last_slash - tmp) + 1;
			word_offset = word_offset - (last_slash - tmp + 1);
			compare = word;
			if (last_slash == tmp) {
				dirp = opendir("/");
			} else {
				dirp = opendir(tmp);
			}
		} else {
			dirp = opendir(".");
		}

		if (!dirp) {
			free(tmp);
			goto finish_tab;
		}

		struct dirent * ent = readdir(dirp);
		while (ent != NULL) {
			if (ent->d_name[0] != '.') {
				if (!word || strstr(ent->d_name, compare) == ent->d_name) {
					struct stat statbuf;
					/* stat it */
					if (last_slash) {
						char * x = malloc(strlen(tmp) + 1 + strlen(ent->d_name) + 1);
						sprintf(x,"%s/%s",tmp,ent->d_name);
						lstat(x, &statbuf);
					} else {
						lstat(ent->d_name, &statbuf);
					}
					char * s;
					if (S_ISDIR(statbuf.st_mode)) {
						s = malloc(strlen(ent->d_name) + 2);
						sprintf(s,"%s/", ent->d_name);
						no_space_if_only = 1;
					} else {
						s = strdup(ent->d_name);
					}
					list_insert(matches, s);
					match = s;
				}
			}
			ent = readdir(dirp);
		}
		closedir(dirp);

		free(tmp);
	} else if (complete_mode == COMPLETE_CUSTOM) {

		char ** completions = NULL;
		char * term_set_completions[] = {"scale","size","gamma","sdf","alpha",NULL};

		if (!strcmp(argv[command_adj],"term-set")) {
			completions = term_set_completions;
		}

		while (*completions) {
			if (strstr(*completions, prefix) == *completions) {
				list_insert(matches, *completions);
				match = *completions;
			}
			completions++;
		}

	}

	if (matches->length == 1) {
		/* Insert */
		rline_insert(c, &match[word_offset]);
		if (word && word_offset == (int)strlen(word) && !no_space_if_only) {
			rline_insert(c, " ");
		}
		rline_redraw(c);
	} else if (matches->length > 1) {
		if (!c->tabbed) {
			/* see if there is a minimum subset we can fill in */
			size_t j = word_offset;
			do {
				char d = match[j];
				int diff = 0;
				foreach(node, matches) {
					char * match = (char *)node->value;
					if (match[j] != d || match[j] == '\0') diff = 1;
				}
				if (diff) break;
				j++;
			} while (j < (size_t)c->requested);
			if (j > (size_t)word_offset) {
				char * tmp = strdup(match);
				tmp[j] = '\0';
				rline_insert(c, &tmp[word_offset]);
				rline_redraw(c);
				free(tmp);
			} else {
				c->tabbed = 1;
			}
		} else {
			/* Print matches */
			fprintf(stderr,"\n");
			size_t j = 0;
			foreach(node, matches) {
				char * match = (char *)node->value;
				fprintf(stderr, "%s", match);
				++j;
				if (j < matches->length) {
					fprintf(stderr, ", ");
				}
			}
			fprintf(stderr,"\n");
			c->callbacks->redraw_prompt(c);
			fprintf(stderr, "\033[s");
			rline_redraw(c);
		}
	}

finish_tab:
	if (free_matches) list_destroy(matches);
	list_free(matches);
	free(prefix);
	free(dup);

}

void add_argument(list_t * argv, char * buf) {
	char * c = malloc(strlen(buf) + 1);
	memcpy(c, buf, strlen(buf) + 1);

	list_insert(argv, c);
}

int read_entry(char * buffer) {
	rline_callbacks_t callbacks = {
		tab_complete_func, redraw_prompt_func, NULL,
		NULL, NULL, NULL, NULL, NULL
	};
	int buffer_size = rline((char *)buffer, LINE_LEN, &callbacks);
	return buffer_size;
}

int read_entry_continued(char * buffer) {
	rline_callbacks_t callbacks = {
		tab_complete_func, redraw_prompt_func_c, NULL,
		NULL, NULL, NULL, NULL, NULL
	};
	int buffer_size = rline((char *)buffer, LINE_LEN, &callbacks);
	return buffer_size;
}

int variable_char(uint8_t c) {
	if (c >= 'A' && c <= 'Z')  return 1;
	if (c >= 'a' && c <= 'z') return 1;
	if (c >= '0' && c <= '9')  return 1;
	if (c == '_') return 1;
	if (c == '?') return 1;
	return 0;
}

void run_cmd(char ** args) {
	int i = execvp(*args, args);
	shell_command_t func = shell_find(*args);
	if (func) {
		int argc = 0;
		while (args[argc]) {
			argc++;
		}
		i = func(argc, args);
	} else {
		if (i != 0) {
			fprintf(stderr, "%s: Command not found\n", *args);
			i = 127;
		}
	}
	exit(i);
}

int is_number(const char * c) {
	while (*c) {
		if (!isdigit(*c)) return 0;
		c++;
	}
	return 1;
}

int shell_exec(char * buffer, size_t size, FILE * file) {

	/* Read previous history entries */
	if (buffer[0] == '!') {
		int x = atoi((char *)((uintptr_t)buffer + 1));
		if (x > 0 && x <= rline_history_count) {
			buffer = rline_history_get(x - 1);
		} else {
			fprintf(stderr, "esh: !%d: event not found\n", x);
			return 0;
		}
	}

	char * history = malloc(strlen(buffer) + 1);
	memcpy(history, buffer, strlen(buffer) + 1);

	if (buffer[0] != ' ' && buffer[0] != '\n') {
		rline_history_insert(history);
	} else {
		free(history);
	}

	char * argv[1024];
	int tokenid = 0;

	char quoted = 0;
	char backtick = 0;
	char buffer_[512] = {0};
	int collected = 0;

	list_t * args = list_create();
	int have_star = 0;

	while (1) {

		char * p = buffer;

		while (*p) {
			switch (*p) {
				case '$':
					if (quoted == '\'') {
						goto _just_add;
					} else {
						if (backtick) {
							goto _just_add;
						}
						p++;
						char var[100];
						int  coll = 0;
						if (*p == '{') {
							p++;
							while (*p != '}' && *p != '\0' && (coll < 100)) {
								var[coll] = *p;
								coll++;
								var[coll] = '\0';
								p++;
							}
							if (*p == '}') {
								p++;
							}
						} else {
							while (*p != '\0' && variable_char(*p) && (coll < 100)) {
								var[coll] = *p;
								coll++;
								var[coll] = '\0';
								if (coll == 0 && (isdigit(*p) || *p == '?')) {
									p++;
									break; /* Don't let these keep going */
								}
								p++;
							}
						}
						/* Special cases */
						char *c = NULL;
						char tmp[128];
						if (!strcmp(var, "?")) {
							sprintf(tmp,"%d",last_ret);
							c = tmp;
						} else if (is_number(var)) {
							int a = atoi(var);
							if (a >= 0 && a < shell_argc) {
								c = shell_argv[a];
							}
						} else {
							c = getenv(var);
						}

						if (c) {
							backtick = 0;
							for (int i = 0; i < (int)strlen(c); ++i) {
								buffer_[collected] = c[i];
								collected++;
							}
							buffer_[collected] = '\0';
						}
						continue;
					}
				case '\"':
					if (quoted == '\"') {
						if (backtick) {
							goto _just_add;
						}
						quoted = 0;
						goto _next;
					} else if (!quoted) {
						quoted = *p;
						goto _next;
					}
					goto _just_add;
				case '\'':
					if (quoted == '\'') {
						if (backtick) {
							goto _just_add;
						}
						quoted = 0;
						goto _next;
					} else if (!quoted) {
						quoted = *p;
						goto _next;
					}
					goto _just_add;
				case '*':
					if (quoted) {
						goto _just_add;
					}
					if (backtick) {
						goto _just_add;
					}
					if (have_star) {
						goto _just_add; /* TODO multiple globs */
					}
					have_star = 1;
					collected += sprintf(&buffer_[collected], STAR_TOKEN);
					goto _next;
				case '\\':
					if (quoted == '\'') {
						goto _just_add;
					}
					if (backtick) {
						goto _just_add;
					}
					backtick = 1;
					goto _next;
				case ' ':
					if (backtick) {
						goto _just_add;
					}
					if (!quoted) {
						goto _new_arg;
					}
					goto _just_add;
				case '\n':
					if (!quoted) {
						goto _done;
					}
					goto _just_add;
				case '|':
					if (!quoted && !backtick) {
						if (collected) {
							add_argument(args, buffer_);
						}
						collected = sprintf(buffer_, "%s", PIPE_TOKEN);
						goto _new_arg;
					}
					goto _just_add;
				case '>':
					if (!quoted && !backtick) {
						if (collected) {
							add_argument(args, buffer_);
						}
						collected = sprintf(buffer_, "%s", WRITE_TOKEN);
						goto _new_arg;
					}
					goto _just_add;
				case '#':
					if (!quoted && !backtick && !collected) {
						goto _done; /* Support comments; must not be part of an existing arg */
					}
					goto _just_add;
				default:
					if (backtick) {
						buffer_[collected] = '\\';
						collected++;
						buffer_[collected] = '\0';
					}
_just_add:
					backtick = 0;
					buffer_[collected] = *p;
					collected++;
					buffer_[collected] = '\0';
					goto _next;
			}

_new_arg:
			backtick = 0;
			if (collected) {
				add_argument(args, buffer_);
				buffer_[0] = '\0';
				have_star = 0;
				collected = 0;
			}

_next:
			p++;
		}

_done:

		if (quoted) {
			if (shell_interactive == 1) {
				draw_prompt_c();
				read_entry_continued(buffer);
				rline_history_append_line(buffer);
				continue;
			} else if (shell_interactive == 2) {
				fgets(buffer, size, file);
				continue;
			} else {
				fprintf(stderr, "Syntax error: Unterminated quoted string.\n");
				return 127;
			}
		}

		if (collected) {
			add_argument(args, buffer_);
			break;
		}

		break;
	}

	int cmdi = 0;
	char ** arg_starts[100] = { &argv[0], NULL };
	char * output_files[100] = { NULL };
	int file_args[100] = {0};
	int argcs[100] = {0};
	int next_is_file = 0;

	int i = 0;
	foreach(node, args) {
		char * c = node->value;

		if (next_is_file) {
			if (next_is_file == 1 && !strcmp(c, WRITE_TOKEN)) {
				next_is_file = 2;
				file_args[cmdi] = O_WRONLY | O_CREAT | O_APPEND;
				continue;
			}
			output_files[cmdi] = c;
			continue;
		}

		if (!strcmp(c, WRITE_TOKEN)) {
			next_is_file = 1;
			file_args[cmdi] = O_WRONLY | O_CREAT | O_TRUNC;
			continue;
		}

		if (!strcmp(c, PIPE_TOKEN)) {
			argv[i] = 0;
			i++;
			cmdi++;
			arg_starts[cmdi] = &argv[i];
			continue;
		}

		char * glob = strstr(c, STAR_TOKEN);
		if (glob) {
			/* Globbing */
			glob[0] = '\0';
			glob[1] = '\0';

			char * before = c;
			char * after = &glob[8];

			int has_before = !!strlen(before);
			int has_after = !!strlen(after);

			if (!has_before || !strchr(before,'/')) {
				/* read current directory, add all */
				DIR * dirp = opendir(".");

				int before_i = i;
				struct dirent * ent = readdir(dirp);
				while (ent != NULL) {
					if (ent->d_name[0] != '.') {
						char * s = malloc(sizeof(char) * (strlen(ent->d_name) + 1));
						memcpy(s, ent->d_name, strlen(ent->d_name) + 1);

						char * t = s;

						if (has_before) {
							if (strstr(s,before) != s) {
								goto _nope;
							}
							t = &s[strlen(before)];
						}
						if (has_after) {
							if (strlen(t) >= strlen(after)) {
								if (!strcmp(after,&t[strlen(t)-strlen(after)])) {
									argv[i] = s;
									i++;
									argcs[cmdi]++;
								}
							}
						} else {
							argv[i] = s;
							i++;
							argcs[cmdi]++;
						}
					}
_nope:
					ent = readdir(dirp);
				}
				closedir(dirp);

				if (before_i == i) {
					/* no matches */
					glob[0] = '*';
					memmove(&glob[1], after, strlen(after)+1);
					argv[i] = c;
					i++;
					argcs[cmdi]++;
				} else {
					free(c);
				}
			} else {
				/* directory globs not supported */
				glob[0] = '*';
				argv[i] = c;
				i++;
				argcs[cmdi]++;
			}
		} else {
			argv[i] = c;
			i++;
			argcs[cmdi]++;
		}
	}
	argv[i] = NULL;

	if (i == 0) {
		return -1;
	}

	list_free(args);

	char * cmd = *arg_starts[0];
	tokenid = i;

	unsigned int child_pid;

	int nowait = (!strcmp(argv[tokenid-1],"&"));
	if (nowait) {
		argv[tokenid-1] = NULL;
	}

	if (cmdi > 0) {
		int last_output[2];
		pipe(last_output);
		child_pid = fork();
		if (!child_pid) {
			dup2(last_output[1], STDOUT_FILENO);
			close(last_output[0]);
			run_cmd(arg_starts[0]);
		}

		for (int j = 1; j < cmdi; ++j) {
			int tmp_out[2];
			pipe(tmp_out);
			if (!fork()) {
				dup2(tmp_out[1], STDOUT_FILENO);
				dup2(last_output[0], STDIN_FILENO);
				close(tmp_out[0]);
				close(last_output[1]);
				run_cmd(arg_starts[j]);
			}
			close(last_output[0]);
			close(last_output[1]);
			last_output[0] = tmp_out[0];
			last_output[1] = tmp_out[1];
		}

		if (!fork()) {
			if (output_files[cmdi]) {
				dup2(open(output_files[cmdi], file_args[cmdi], 0666), STDOUT_FILENO);
			}
			dup2(last_output[0], STDIN_FILENO);
			close(last_output[1]);
			run_cmd(arg_starts[cmdi]);
		}
		close(last_output[0]);
		close(last_output[1]);

		/* Now execute the last piece and wait on all of them */
	} else {
		shell_command_t func = shell_find(*arg_starts[0]);
		if (func) {
			return func(argcs[0], arg_starts[0]);
		} else {
			child_pid = fork();
			if (!child_pid) {
				if (output_files[cmdi]) {
					dup2(open(output_files[cmdi], file_args[cmdi], 0666), STDOUT_FILENO);
				}
				run_cmd(arg_starts[0]);
			}
		}
	}

	tcsetpgrp(STDIN_FILENO, child_pid);
	int ret_code = 0;
	if (!nowait) {
		child = child_pid;
		int pid;
		do {
			pid = waitpid(-1, &ret_code, 0);
		} while (pid != -1 || (pid == -1 && errno != ECHILD));
		child = 0;
	}
	tcsetpgrp(STDIN_FILENO, getpid());
	free(cmd);
	return ret_code;
}

void add_path_contents(char * path) {
	DIR * dirp = opendir(path);

	if (!dirp) return; /* Failed to load directly */

	struct dirent * ent = readdir(dirp);
	while (ent != NULL) {
		if (ent->d_name[0] != '.') {
			char * s = malloc(sizeof(char) * (strlen(ent->d_name) + 1));
			memcpy(s, ent->d_name, strlen(ent->d_name) + 1);
			shell_install_command(s, NULL, NULL);
		}

		ent = readdir(dirp);
	}
	closedir(dirp);

}

struct command {
	char * string;
	void * func;
	char * desc;
};

static int comp_shell_commands(const void *p1, const void *p2) {
	return strcmp(((struct command *)p1)->string, ((struct command *)p2)->string);
}

void sort_commands() {
	struct command commands[SHELL_COMMANDS];
	for (int i = 0; i < shell_commands_len; ++i) {
		commands[i].string = shell_commands[i];
		commands[i].func   = shell_pointers[i];
		commands[i].desc   = shell_descript[i];
	}
	qsort(&commands, shell_commands_len, sizeof(struct command), comp_shell_commands);
	for (int i = 0; i < shell_commands_len; ++i) {
		shell_commands[i] = commands[i].string;
		shell_pointers[i] = commands[i].func;
		shell_descript[i] = commands[i].desc;
	}
}

void show_version(void) {
	printf("esh 1.3.0\n");
}

void show_usage(int argc, char * argv[]) {
	printf(
			"Esh: The Experimental Shell\n"
			"\n"
			"usage: %s [-lha] [path]\n"
			"\n"
			" -c \033[4mcmd\033[0m \033[3mparse and execute cmd\033[0m\n"
			//-c cmd \033[...
			" -v     \033[3mshow version information\033[0m\n"
			" -?     \033[3mshow this help text\033[0m\n"
			"\n", argv[0]);
}

void add_path(void) {

	char * envvar = getenv("PATH");

	if (!envvar) {
		add_path_contents("/bin");
		return;
	}

	char * tmp = strdup(envvar);

	do {
		char * end = strstr(tmp,":");
		if (end) {
			*end = '\0';
			end++;
		}
		add_path_contents(tmp);
		tmp = end;
	} while (tmp);

	free(tmp);
}

int main(int argc, char ** argv) {

	pid = getpid();

	signal(SIGINT, sig_pass);
	signal(SIGWINCH, sig_pass);

	getuser();
	gethost();

	install_commands();
	/* Parse $PATH to add contents */
	add_path();
	sort_commands();

	if (argc > 1) {
		int c;
		while ((c = getopt(argc, argv, "c:v?")) != -1) {
			switch (c) {
				case 'c':
					shell_interactive = 0;
					last_ret = shell_exec(optarg, strlen(optarg), NULL);
					return (last_ret == -1) ? 0 : last_ret;
				case 'v':
					show_version();
					return 0;
				case '?':
					show_usage(argc, argv);
					return 0;
			}
		}
	}

	if (optind < argc) {
		shell_interactive = 2;
		FILE * f = fopen(argv[optind],"r");

		if (!f) {
			fprintf(stderr, "%s: %s: %s\n", argv[0], argv[optind], strerror(errno));
			return 1;
		}

		shell_argc = argc - 1;
		shell_argv = &argv[1];

		while (!feof(f)) {
			char buf[LINE_LEN] = {0};
			fgets(buf, LINE_LEN, f);
			int ret = shell_exec(buf, LINE_LEN, f);
			if (ret >= 0) last_ret = ret;
		}
		return 0;
	}

	shell_interactive = 1;

	while (1) {
		draw_prompt(last_ret);
		char buffer[LINE_LEN] = {0};

		read_entry(buffer);
		int ret = shell_exec(buffer, LINE_LEN, stdin);
		if (ret >= 0) last_ret = ret;
		rline_scroll = 0;

	}

	return 0;
}

/*
 * cd [path]
 */
uint32_t shell_cmd_cd(int argc, char * argv[]) {
	if (argc > 1) {
		if (chdir(argv[1])) {
			goto cd_error;
		} /* else success */
	} else /* argc < 2 */ {
		char * home = getenv("HOME");
		if (home) {
			if (chdir(home)) {
				goto cd_error;
			}
		} else {
			char home_path[512];
			sprintf(home_path, "/home/%s", username);
			if (chdir(home_path)) {
				goto cd_error;
			}
		}
	}
	return 0;
cd_error:
	fprintf(stderr, "%s: could not cd '%s': no such file or directory\n", argv[0], argv[1]);
	return 1;
}

/*
 * history
 */
uint32_t shell_cmd_history(int argc, char * argv[]) {
	for (int i = 0; i < rline_history_count; ++i) {
		printf("%d\t%s\n", i + 1, rline_history_get(i));
	}
	return 0;
}

uint32_t shell_cmd_export(int argc, char * argv[]) {
	if (argc > 1) {
		putenv(argv[1]);
	}
	return 0;
}

uint32_t shell_cmd_exit(int argc, char * argv[]) {
	if (argc > 1) {
		exit(atoi(argv[1]));
	} else {
		exit(0);
	}
	return -1;
}

uint32_t shell_cmd_help(int argc, char * argv[]) {
	show_version();

	printf("\nThis shell is not POSIX-compliant, please be careful.\n\n");

	printf("Built-in commands:\n");
	for (int i = 0; i < shell_commands_len; ++i) {
		if (!shell_descript[i]) continue;
		printf(" %-20s - %s\n", shell_commands[i], shell_descript[i]);
	}

	return 0;
}

void install_commands() {
	shell_install_command("cd",      shell_cmd_cd, "change directory");
	shell_install_command("exit",    shell_cmd_exit, "exit the shell");
	shell_install_command("export",  shell_cmd_export, "set environment variables");
	shell_install_command("help",    shell_cmd_help, "display this help text");
	shell_install_command("history", shell_cmd_history, "list command history");
}
