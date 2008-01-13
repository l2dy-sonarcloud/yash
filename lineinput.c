/* Yash: yet another shell */
/* readline.c: interface to readline library */
/* © 2007-2008 magicant */

/* This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.  */


#define _GNU_SOURCE
#define _ATFILE_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <sys/select.h>
#include <sys/stat.h>
#include "yash.h"
#include "util.h"
#include "lineinput.h"
#include "exec.h"
#include "path.h"
#include "variable.h"
#include <assert.h>


static bool unset_nonblocking(int fd);
//static int char_is_quoted_p(char *text, int index);
//static char *quote_filename(char *text, int matchtype, char *quotepointer);
//static char *unquote_filename(char *text, int quotechar);
static int check_inhibit_expansion(char *str, int index);
static char **yash_completion(const char *text, int start, int end);
static int check_completion_type(int index);
static char *normal_file_completion_function(const char *text, int state);
static char *path_completion_function(const char *text, int state);
static char *env_completion_function(const char *text, int state);
static char *expand_prompt(const char *s)
	__attribute__((nonnull));

/* 履歴を保存するファイルのパス。NULL なら履歴は保存されない。 */
char *history_filename = NULL;
/* 履歴ファイルに入れる履歴の数 */
int history_filesize = 500;
/* シェル内で保存する履歴の数 */
int history_histsize = 500;
/* プロンプトとして表示する文字列。 */
char *readline_prompt1 = NULL;
char *readline_prompt2 = NULL;

/* ファイル名補完で、非実行可能ファイルを候補に入れるかどうか。 */
static bool executable_only;
/* ファイル名補完で、対象を検索するパス。path_completion_function が使う。 */
static const char *searchpath;
/* ファイル名補完で、大文字小文字の区別の有無 */
static bool completion_ignorecase;

/* readline を初期化する。
 * これは .yashrc の実行が終わった「後」に呼出す。 */
void initialize_readline(void)
{
	rl_readline_name = "yash";
	rl_outstream = stderr;
	rl_getc_function = yash_getc;
	rl_attempted_completion_function = yash_completion;
	rl_filename_quote_characters = " \t\n\\\"'<>|;&()#$`?*[!{";
	/* rl_char_is_quoted_p = XXX */
	/* rl_filename_quoting_function = XXX */
	/* rl_filename_dequoting_function = XXX */

	history_comment_char = '#';
	history_no_expand_chars = " \t\n\r={}|";
	history_quotes_inhibit_expansion = true;
	history_inhibit_expansion_function = check_inhibit_expansion;
	stifle_history(history_histsize);
	if (history_filename)
		read_history(history_filename);
}

/* readline を終了する */
void finalize_readline(void)
{
	if (history_filename) {
		stifle_history(history_filesize);
		write_history(history_filename);
	}
}

/* 指定したファイルディスクリプタを非ブロッキングモードにする。
 * 戻り値: 成功なら true、エラーなら errno を設定して false。 */
static bool unset_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL);
	if (flags >= 0) {
		if (flags & O_NONBLOCK) {
			return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != -1;
		} else {
			return true;
		}
	}
	return false;
}

/* fgetc のラッパー。シグナルを正しく処理する。rl_getc_function として使う。 */
int yash_getc(FILE *stream)
{
	sigset_t newset, oldset;
	fd_set fds;
	int fd = fileno(stream);
	int result;

	sigfillset(&newset);
	sigemptyset(&oldset);
	if (sigprocmask(SIG_BLOCK, &newset, &oldset) < 0) {
		xerror(0, errno, NULL);
		return EOF;
	}
	for (;;) {
		handle_signals();
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		if (pselect(fd + 1, &fds, NULL, NULL, NULL, &oldset) < 0) {
			switch (errno) {
				case EINTR:
					continue;
				default:
#if defined(RL_ISSTATE) && defined(RL_STATE_READCMD) && defined(READERR)
					result = RL_ISSTATE(RL_STATE_READCMD) ? READERR : EOF;
#else
					result = EOF;
#endif
					goto end;
			}
		}
		if (FD_ISSET(fd, &fds)) {
			unsigned char c;
			ssize_t r = read(fd, &c, sizeof c);
			if (r == sizeof c) {
				result = c;
				goto end;
			} else if (r == 0) {
				result = EOF;
				goto end;
			} else if (errno == EINTR) {
				continue;
			} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if (unset_nonblocking(fd)) {
					continue;
				} else {
					result = EOF;
					goto end;
				}
			} else {
				result = EOF;
				goto end;
			}
		}
	}
end:
	if (sigprocmask(SIG_SETMASK, &oldset, NULL) < 0)
		xerror(0, errno, NULL);
	return result;
}

/* プロンプトを出さずに yash_getline_input から行を読み取る。
 * info:   struct fgetline_info へのポインタ。
 * 戻り値: 読み取った行。(新しく malloc した文字列)
 *         EOF が入力されたときは NULL。 */
char *yash_fgetline(int ptype __attribute__((unused)), void *info)
{
	struct fgetline_info *finfo = info;
	char *bufstart = finfo->buffer + finfo->bufoff;
	sigset_t newset, oldset;
	fd_set fds;
	struct strbuf buf;
	char *result;

	/* まず info 内のバッファに既に一行文のデータがないか調べる */
	char *end = memchr(bufstart, '\n', finfo->buflen);
	if (end) {
		result = xstrndup(bufstart, end - bufstart);
		finfo->buflen -= end - bufstart + 1;
		finfo->bufoff += end - bufstart + 1;
		return result;
	}

	/* 駄目だったら実際にファイルから読み込む */
	sigfillset(&newset);
	sigemptyset(&oldset);
	if (sigprocmask(SIG_BLOCK, &newset, &oldset) < 0) {
		xerror(0, errno, NULL);
		return NULL;
	}
	sb_init(&buf);
	sb_nappend(&buf, bufstart, finfo->buflen);
	for (;;) {
		handle_signals();
		FD_ZERO(&fds);
		FD_SET(finfo->fd, &fds);
		if (pselect(finfo->fd + 1, &fds, NULL, NULL, NULL, &oldset) < 0){
			if (errno == EINTR) {
				continue;
			} else {
				result = NULL;
				goto end;
			}
		}
		if (FD_ISSET(finfo->fd, &fds)) {
			ssize_t r = read(finfo->fd, finfo->buffer, sizeof finfo->buffer);
			if (r < 0) {
				if (errno == EINTR) {
					continue;
				} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
					if (unset_nonblocking(finfo->fd)) {
						continue;
					} else {
						result = NULL;
						goto end;
					}
				} else {
					result = NULL;
					goto end;
				}
			} else if (r == 0) {
				result = NULL;
				goto end;
			} else {
				end = memchr(finfo->buffer, '\n', r);
				if (end) {
					sb_nappend(&buf, finfo->buffer, end - finfo->buffer);
					finfo->bufoff = end - finfo->buffer + 1;
					finfo->buflen = r - finfo->bufoff;
					result = sb_tostr(&buf);
					goto end;
				} else {
					sb_nappend(&buf, finfo->buffer, sizeof finfo->buffer);
				}
			}
		}
	}
end:
	if (sigprocmask(SIG_SETMASK, &oldset, NULL) < 0)
		xerror(0, errno, NULL);
	return result;
}

/* 文字列 yash_sgetline_src の yash_sgetline_offset 文字目から行を読み取る。
 * info:   struct sgetline_info へのポインタ。
 *         読み取った文字数だけ offset が増える。
 * 戻り値: 読み取った行。(新しく malloc した文字列)
 *         文字列の終端に達したときは NULL。 */
char *yash_sgetline(int ptype __attribute__((unused)), void *info)
{
	struct sgetline_info *i = info;
	const char *start = i->src + i->offset;
	const char *newline = start;
	if (!*start) return NULL;
	while (*newline && *newline != '\n') newline++;
	size_t len = newline - start;
	if (*newline == '\n') {
		char *result = xstrndup(start, len);
		i->offset += len + 1;
		return result;
	}
	i->offset += len;
	return xstrdup(start);
}

/* プロンプトを表示して行を読み取る。
 * ptype が 1 か 2 なら履歴展開を行い、履歴に追加する。
 * ptype:  プロンプトの種類。1~2。(PS1~PS2 に対応)
 * 戻り値: 読み取った行。(新しく malloc した文字列)
 *         EOF が入力されたときは NULL。 */
char *yash_readline(int ptype, void *info __attribute__((unused)))
{
	char *prompt, *actualprompt;
	char *line, *eline;
	bool terminal_info_valid = false;
	struct termios old_terminal_info, new_terminal_info;
	
yash_readline_start:
	switch (ptype) {
		case 1:
			wait_chld();
			print_all_job_status(true /* changed only */, false);
			prompt = readline_prompt1 ? readline_prompt1 : "\\s-\\v\\$ ";
			break;
		case 2:
			prompt = readline_prompt2 ? readline_prompt2 : "> ";
			break;
		default:
			xerror(2, 0, "internal error: yash_readline_start ptype=%d", ptype);
			assert(false);
	}

	if (tcsetpgrp(STDIN_FILENO, getpgrp()) < 0)
		xerror(0, errno, "tcsetpgrp before readline");

	if (tcgetattr(STDIN_FILENO, &old_terminal_info) == 0) {
		terminal_info_valid = true;
		new_terminal_info = old_terminal_info;
		new_terminal_info.c_iflag &= ~(INLCR | IGNCR);
		new_terminal_info.c_iflag |= ICRNL;
		new_terminal_info.c_lflag |= ICANON | ECHO | ECHOE | ECHOK;
		tcsetattr(STDIN_FILENO, TCSADRAIN, &new_terminal_info);
	}

	actualprompt = expand_prompt(prompt);
	line = readline(actualprompt);
	free(actualprompt);

	if (terminal_info_valid)
		tcsetattr(STDIN_FILENO, TCSADRAIN, &old_terminal_info);

	if (!line) {
		if (ptype == 1)      printf(is_loginshell ? "logout\n" : "exit\n");
		else if (ptype == 2) printf("\n");
		return NULL;
	}
	if (!*skipspaces(line)) {
		free(line);
		goto yash_readline_start;
	}

	if (ptype == 1 || ptype == 2) {
		switch (history_expand(line, &eline)) {
			case 1:  /* expansion successful */
				printf("%s\n", eline);
				/* falls thru! */
			case 0:  /* expansion successful without changes */
			default:
				free(line);
				add_history(eline);
				return eline;
			case -1:  /* Error */
				free(line);
				xerror(0, 0, "%s", eline);
				free(eline);
				goto yash_readline_start;
			case 2:   /* No execution */
				free(line);
				printf("%s\n", eline);
				add_history(eline);
				free(eline);
				goto yash_readline_start;
		}
	} else {
		return line;
	}
}

#if 0
/* text[index] がクォートされているかどうか判定する。 */
static int char_is_quoted_p(char *text, int index)
{
	return 0;  /*XXX*/
}

static char *quote_filename(char *text, int matchtype, char *quotepointer)
{
	return 0;  /*XXX*/
}

static char *unquote_filename(char *text, int quotechar)
{
	return 0; /*XXX*/
}
#endif

/* str の index 文字目で行う予定の履歴展開を抑止すべきかどうかを判定する。 */
static int check_inhibit_expansion(char *str, int index)
{
	if (index > 0 && str[index - 1] == '{')
		return true;
	return false;
}

/* コマンド補完を行う。ライブラリから呼び出される。 */
static char **yash_completion(
		const char *text, int start, int end __attribute__((unused)))
{
	char **matches = NULL;
	rl_compentry_func_t *completer;

	completion_ignorecase = strcmp(
			rl_variable_value("completion-ignore-case"), "on") == 0;
	rl_filename_quoting_desired = 1;

	switch (check_completion_type(start)) {
		case 0:  /* 通常の補完 */
			if (!executable_only)
				rl_filename_completion_desired = 1;

			if (text[0] == '~') {
				completer = !strchr(text, '/')
					? rl_username_completion_function
					: normal_file_completion_function;
				rl_filename_completion_desired = 1;
			} else if (!executable_only || hasprefix(text, "/")
					|| hasprefix(text, "./") || hasprefix(text, "../")) {
				completer = normal_file_completion_function;
				rl_filename_completion_desired = 1;
			} else {
				searchpath = getvar(VAR_PATH);
				completer = path_completion_function;
			}
			break;
		case 1:  /* 環境変数名の補完 */
			completer = env_completion_function;
			break;
		default:
			assert(false);
	}
	matches = rl_completion_matches(text, completer);
	rl_attempted_completion_over = 1;

	return matches;
}

/* rl_line_buffer の index 直前の文字を見て、補完の種類を決める。
 * executable_only 変数を変更する。
 * 戻り値: 0: 通常の補完
 *         1: 環境変数名の補完 */
/* この関数は rl_line_buffer の index 以降の文字は見ない。それらの文字を見て
 * 補完の種類を決めるのは呼び出し元の責任である。 */
static int check_completion_type(int index)
{
	while (--index >= 0 && rl_line_buffer[index] == ' ');
	if (index < 0)
		goto command_completion;  /* Caution!  Dangerous goto */
	switch (rl_line_buffer[index]) {
		case '|':  case '&':  case ';':  case '(':
		command_completion:
			executable_only = true;
			return 0;
		case '$':
			executable_only = false;
			return 1;
		default:
			executable_only = false;
			return 0;
	}
}

/* 作業ディレクトリを相対パスの起点として、ファイル名に基づき補完候補を挙げる。
 * text が '/' で始まるなら絶対パスとみなす。 */
static char *normal_file_completion_function(const char *text, int state)
{
	/* getbasename: path の中の最後の '/' の直後の文字の位置を返す。
	 * '/' がなければ NULL を返す。 */
	char *getbasename(const char *path) {
		char *result = strrchr(path, '/');
		return result ? result + 1 : NULL;
	}
	static int (*comparer)(const char *a, const char *b, size_t n);
	static DIR *dir = NULL;
	static char *ldname = NULL, *bname = NULL;
	static size_t ldnamelen, bnamelen;
	static int dfd;
	struct dirent *dent;
	struct stat st;

	assert(text);
	if (!state) {  /* 初期化: 新たな補完を開始 */
		ldname = xstrdup(text);
		if (!*ldname) {
			dir = opendir(".");
		} else {
			char *dname = (text[0] == '~')
				? expand_tilde(ldname) : xstrdup(ldname);
			char *dnameend = getbasename(dname);

			if (dnameend) {
				*dnameend = '\0';
				dir = opendir(dname);
			} else {
				dir = opendir(".");
			}
			free(dname);
		}
		if (!dir)
			goto end;
		dfd = dirfd(dir);
		if (dfd < 0)
			goto end;
		bname = xstrdup(getbasename(text) ? : text);
		bnamelen = strlen(bname);
		*(getbasename(ldname) ? : ldname) = '\0';
		ldnamelen = strlen(ldname);
		comparer = completion_ignorecase ? strncasecmp : strncmp;
	}
	do {
		do {
			if (!(dent = readdir(dir)))
				goto end;
		} while (comparer(bname, dent->d_name, bnamelen) != 0
				|| strcmp(".", dent->d_name) == 0
				|| strcmp("..", dent->d_name) == 0);
		if (fstatat(dfd, dent->d_name, &st, 0) < 0)
			goto end;
	} while(executable_only && !(st.st_mode & S_IXUSR) && !S_ISDIR(st.st_mode));

	char *result = xmalloc(ldnamelen + strlen(dent->d_name) + 1);
	strcpy(result, ldname);
	strcpy(result + ldnamelen, dent->d_name);
	return result;

end:
	if (dir) {
		closedir(dir);
		dir = NULL;
	}
	free(ldname);
	ldname = NULL;
	free(bname);
	bname = NULL;
	return NULL;
}

/* コマンド名を生成する。コマンド補完でライブラリから呼び出される。 */
/* searchpath 内にあるファイルで該当するものを挙げる。
 * executable_only ならば組込みコマンド・エイリアスも候補に入れる。 */
static char *path_completion_function(const char *text, int state)
{
	/* builtin: 次に調べる組込みコマンドのインデックス。
	 *          全部調べ終わったら -1。 */
	static int (*comparer)(const char *a, const char *b, size_t n);
	static ssize_t builtin;
	static size_t textlen;
	//static const ALIAS *alias;
	static DIR *dir;
	static int dfd;
	static char *savepath, *path;
	struct dirent *dent;
	struct stat st;

	if (!state) {
		builtin = 0;
		//alias = get_all_aliases();
		dir = NULL;
		savepath = path = xstrdup(searchpath ? : "");
		textlen = strlen(text);
		comparer = completion_ignorecase ? strncasecmp : strncmp;
	}
	if (builtin >= 0) {
//		const char *bname;

//		while ((bname = builtins[builtin++].b_name)) {
//			if (comparer(bname, text, textlen) == 0)
//				return xstrdup(bname);
//		}
		builtin = -1;
	}
//	while (alias) {
//		const char *aliasname = alias->name;
//		alias = alias->next;
//		if (comparer(aliasname, text, textlen) == 0)
//			return xstrdup(aliasname);
//	}
	do {
next:
		while (!dir || !(dent = readdir(dir))) {
			/* 次のディレクトリに進む */
			size_t len;
			char oldend;

			if (!path)
				goto end;
			if (dir)
				closedir(dir);
			len = strcspn(path, ":");
			if (len) {
				oldend = path[len];
				path[len] = '\0';
				dir = opendir(path);
				path[len] = oldend;
				path += len;
			} else {
				dir = opendir(".");
			}
			if (dir)
				dfd = dirfd(dir);
			if (*path == '\0')
				path = NULL;
			else if (*path == ':')
				path++;
		}
		if (comparer(dent->d_name, text, textlen) != 0)
			goto next;
		if (dfd < 0 || fstatat(dfd, dent->d_name, &st, 0) < 0) {
			goto end;
		}
	} while(executable_only && !(st.st_mode & S_IXUSR) && !S_ISDIR(st.st_mode));

	return xstrdup(dent->d_name);

end:
	if (dir) {
		closedir(dir);
		dir = NULL;
	}
	free(savepath);
	return NULL;
}

/* 環境変数名の補完候補を挙げる。補完でライブラリから呼び出される。 */
static char *env_completion_function(const char *text, int state)
{
	static int (*comparer)(const char *a, const char *b, size_t n);
	static char **env;
	static size_t textlen;
	const char *envent;

	if (!state) {
		env = environ;
		if (!env)
			return NULL;
		textlen = strlen(text);
		comparer = completion_ignorecase ? strncasecmp : strncmp;
	}
	while ((envent = *env++)) {
		if (comparer(envent, text, textlen) == 0) {
			return xstrndup(envent, strcspn(envent, "="));
		}
	}
	env = NULL;
	return NULL;
}


static struct tm *get_time(void)
{
	time_t timeval = time(NULL);
	if (timeval != (time_t) -1) {
		return localtime(&timeval);
	}
	return NULL;
}

/* 文字列に含まれる '\a' や '\n' をプロンプト用に展開する。
 * 戻り値: s を展開した結果 (新しく malloc された文字列)。失敗したら NULL。 */
/* \a    ベル文字 '\07'
 * \d    日付
 * \e    エスケープ '\033'
 * \h    ホスト名の最初の '.' より前の部分
 * \H    ホスト名全体
 * \j    ジョブ数
 * \l    端末のベース名 (< ttyname(3), basename(3))
 * \L    端末名 (< ttyname(3))
 * \n    改行
 * \r    復帰
 * \s    シェルのベース名 (< 起動時の argv[0])
 * \S    シェルの名前     (< 起動時の argv[0])
 * \t    時刻 (標準形式)
 * \T    HH:MM:SS 形式の時刻 (12 時間制)
 * \@    HH:MM am/pm 形式の時刻
 * \u    ユーザ名
 * \v    yash のバージョン
 * \w    PWD
 * \W    PWD のベース名
 * \!    履歴番号
 * \$    実効 UID が 0 なら '#' さもなくば '$'
 * \nnn  八進数 nnn の文字
 * \\    バックスラッシュ '\\'
 * \[    RL_PROMPT_START_IGNORE
 * \]    RL_PROMPT_END_IGNORE */
static char *expand_prompt(const char *s)
{
	struct strbuf result;
	struct tm *tm = NULL;

	sb_init(&result);
	while (*s) {
		if (*s != '\\') {
			sb_cappend(&result, *s);
		} else switch (*++s) {
			case 'a':
				sb_cappend(&result, '\a');
				break;
			case 'd':
				if (!tm)
					tm = get_time();
				if (tm)
					sb_strftime(&result, "%x", tm);
				break;
			case 'e':
				sb_cappend(&result, '\033');
				break;
			case 'h':
				{
					const char *host = getvar(VAR_HOSTNAME);
					if (host)
						sb_nappend(&result, host, strcspn(host, "."));
				}
				break;
			case 'H':
				{
					const char *host = getvar(VAR_HOSTNAME);
					if (host)
						sb_append(&result, host);
				}
				break;
			case 'j':
				sb_printf(&result, "%u", job_count());
				break;
			case 'l':
				{
					char *name = ttyname(STDIN_FILENO);
					if (name) {
						char bn[strlen(name) + 1];
						strcpy(bn, name);
						name = basename(bn);
						if (name)
							sb_append(&result, name);
					}
				}
				break;
			case 'L':
				{
					char *name = ttyname(STDIN_FILENO);
					if (name)
						sb_append(&result, name);
				}
				break;
			case 'n':
				sb_cappend(&result, '\n');
				break;
			case 'r':
				sb_cappend(&result, '\r');
				break;
			case 's':
				sb_append(&result, yash_program_invocation_short_name);
				break;
			case 'S':
				sb_append(&result, yash_program_invocation_name);
				break;
			case 't':
				if (!tm)
					tm = get_time();
				if (tm)
					sb_strftime(&result, "%X", tm);
				break;
			case 'T':
				if (!tm)
					tm = get_time();
				if (tm)
					sb_strftime(&result, "%I:%M:%S", tm);
				break;
			case '@':
				if (!tm)
					tm = get_time();
				if (tm)
					sb_strftime(&result, "%I:%M %p", tm);
				break;
			case 'u':
				{
					const char *user = getvar(VAR_USER);
					if (!user)
						user = getvar(VAR_LOGNAME);
					if (user)
						sb_append(&result, user);
				}
				break;
			case 'v':
				sb_append(&result, YASH_VERSION);
				break;
			case 'w':
				{
					char *dir = collapse_homedir(getvar(VAR_PWD));
					if (dir) {
						sb_append(&result, dir);
						free(dir);
					}
				}
				break;
			case 'W':
				{
					char *dir = collapse_homedir(getvar(VAR_PWD));
					if (dir) {
						const char *dir2 = basename(dir);
						if (dir2)
							sb_append(&result, dir2);
						free(dir);
					}
				}
				break;
			case '!':
				using_history();
				sb_printf(&result, "%d", history_base + where_history());
				break;
			case '$':
				sb_cappend(&result, geteuid() ? '$' : '#');
				break;
			case '0':  case '1':  case '2':  case '3':
			case '4':  case '5':  case '6':  case '7':
				if ('0' <= s[1] && s[1] <= '7' && '0' <= s[2] && s[2] <= '7') {
					sb_cappend(&result,
							((((s[0] - '0') << 3)
							  + s[1] - '0') << 3)
							  + s[2] - '0');
					s += 2;
				} else {
					goto default_case;
				}
				break;
			case '[':
				sb_cappend(&result, RL_PROMPT_START_IGNORE);
				break;
			case ']':
				sb_cappend(&result, RL_PROMPT_END_IGNORE);
				break;
			case '\0':
				sb_cappend(&result, '\\');
				continue;
			default:  default_case:
				sb_cappend(&result, '\\');
				sb_cappend(&result, *s);
				break;
		}
		s++;
	}
	return sb_tostr(&result);
}
